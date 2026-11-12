// GPU decode on NRI's video extension. The decode thread works sample by sample:
//
//   decode queue: [barriers, CmdDecodeVideo for each picture of the sample, copy of the displayed picture's two planes
//                  into a staging buffer]                                  signals decode_fence
//   graphics queue (after the CPU saw decode_fence):
//                 [copy of the staging buffer into the pool frame's two textures]
//                                  waits decode_fence (already passed), signals copy_fence
//
// The decoded picture is copied out on the decode queue, right after its decode, so the DPB picture never changes
// layout on another queue while the decode queue still uses it as a reference. The copy into the pool texture runs on
// the graphics queue, after anything the consumer has submitted against the old contents. Picture state tracking
// (the barriers between decode, reference and copy use) follows NRISamples' VideoEncodeDecode sample (MIT license,
// (c) NVIDIA Corporation): required states from GetVideoPictureState, the state kept per picture between uses.

#include "renderer/clips/video/nri_video_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <spdlog/spdlog.h>

#include "renderer/clips/video/codec_backend.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace renderer::clips {

using namespace video;

namespace {

uint64_t AlignUp(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }

double ThreadCpuSeconds() {
#ifdef _WIN32
    FILETIME create, exit_time, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &create, &exit_time, &kernel, &user))
        return 0;
    auto to_seconds = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return to_seconds(kernel) + to_seconds(user);
#else
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#endif
}

double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A DPB picture: a texture (or one layer of one) with its video picture and the state the next barrier starts from.
struct Picture {
    nri::Texture* texture = nullptr;
    uint32_t layer = 0;
    nri::VideoPicture* picture = nullptr;
    nri::AccessLayoutStage state = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL};
};

// One displayed frame of the output pool: luma and chroma textures with their views.
struct PoolFrame {
    nri::Texture* luma_texture = nullptr;
    nri::Texture* chroma_texture = nullptr;
    nri::Descriptor* luma = nullptr;
    nri::Descriptor* chroma = nullptr;
    nri::AccessLayoutStage state = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL};
    uint32_t index = 0;
    int pins = 0;
};

enum class EntryState { Decoding, Copying, Ready };

struct Entry {
    PoolFrame* frame = nullptr;
    EntryState state = EntryState::Decoding;
    uint64_t copy_value = 0;
};

nri::AccessLayoutStage CopySourceState() {
    return {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
}
nri::AccessLayoutStage CopyDestinationState() {
    return {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
}
nri::AccessLayoutStage ShaderResourceState() {
    return {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::ALL};
}

#define NRI_TRY(expr, what)                                                                  \
    do {                                                                                     \
        const nri::Result nri_result_ = (expr);                                              \
        if (nri_result_ != nri::Result::SUCCESS) {                                           \
            Fail(std::string(what) + " failed (nri::Result " + std::to_string(int(nri_result_)) + ")"); \
            return false;                                                                    \
        }                                                                                    \
    } while (0)

}  // namespace

struct NriVideoSource::Impl {
    // One sample in flight on the decode and graphics queues.
    struct Job {
        enum State { Free, Decoding, Copying } state = Free;
        nri::CommandAllocator* decode_allocator = nullptr;
        nri::CommandBuffer* decode_command = nullptr;
        nri::CommandAllocator* copy_allocator = nullptr;
        nri::CommandBuffer* copy_command = nullptr;
        nri::Buffer* bitstream = nullptr;
        uint64_t bitstream_size = 0;
        nri::Buffer* staging = nullptr;
        uint64_t decode_value = 0, copy_value = 0;
        PoolFrame* out = nullptr;  // the frame this sample fills, null if none
        int64_t frame = -1;
    };

    // ---- set once in Init
    VideoContext context;
    const nri::CoreInterface* core = nullptr;
    nri::VideoInterface video = {};
    NriVideoOptions options;
    nri::Device* device = nullptr;
    nri::Queue* graphics_queue = nullptr;
    nri::Queue* decode_queue = nullptr;
    std::unique_ptr<Mp4> mp4;
    std::unique_ptr<CodecBackend> backend;
    VideoInfo info;
    StreamFormat format;
    std::string description;
    nri::Format video_format = nri::Format::NV12_UNORM, luma_format = nri::Format::R8_UNORM,
                chroma_format = nri::Format::RG8_UNORM;
    uint32_t bytes_per_sample = 1;
    nri::VideoCapabilities caps = {};
    nri::VideoSession* session = nullptr;
    nri::VideoSessionParameters* parameters = nullptr;
    std::array<nri::AccessLayoutStage, size_t(nri::VideoPictureRole::MAX_NUM)> role_state = {};
    std::vector<nri::Texture*> dpb_textures;
    std::vector<Picture> dpb;
    uint32_t luma_width = 0, luma_height = 0, chroma_width = 0, chroma_height = 0;  // of a frame
    uint32_t pitch_luma = 0, pitch_chroma = 0;
    uint64_t chroma_offset = 0, staging_bytes = 0;
    uint64_t frame_bytes = 0, capacity = 0;
    nri::Fence* decode_fence = nullptr;
    nri::Fence* copy_fence = nullptr;
    uint64_t decode_counter = 0, copy_counter = 0;
    std::vector<Job> jobs;
    size_t job_head = 0, job_count = 0;
    std::vector<uint32_t> start_samples;
    uint64_t bitstream_offset_alignment = 1, bitstream_size_alignment = 1;

    // ---- shared with the render thread (mutex)
    mutable std::mutex mutex;
    std::condition_variable wake;
    int64_t first = 0, last = -1, playhead = 0;
    int direction = 1;
    std::map<int64_t, Entry> cache;
    std::vector<std::unique_ptr<PoolFrame>> pool;
    std::vector<PoolFrame*> free_frames;
    bool stop = false;
    std::string error;
    std::deque<double> completions;  // of decodes, for the rate
    std::atomic<int64_t> decoded_total{0};
    std::atomic<double> thread_cpu{0};

    // ---- the decode thread
    std::thread thread;
    bool cursor_valid = false;
    uint32_t cursor = 0, cursor_gop = 0;
    std::vector<uint8_t> sample_bytes;

    // test readback
    nri::Buffer* readback = nullptr;
    uint8_t* readback_mapped = nullptr;
    nri::CommandAllocator* readback_allocator = nullptr;
    nri::CommandBuffer* readback_command = nullptr;
    std::mutex readback_mutex;

    ~Impl() { Shutdown(); }

    void Fail(const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex);
        if (error.empty()) {
            error = why;
            spdlog::error("GPU video decode: {}", why);
        }
    }

    bool Init(const VideoContext& ctx, const std::string& path, const NriVideoOptions& opts, std::string* why);
    void Shutdown();

    // creation helpers
    bool CreateJobs(std::string* why);
    PoolFrame* CreatePoolFrame();

    // decode thread
    void Run();
    bool Step();
    bool DecodeSample(uint32_t sample);
    bool RecordPicture(nri::CommandBuffer& command, const PictureJob& pj, Job& job, uint64_t offset, uint64_t range);
    void RecordStageOne(nri::CommandBuffer& command, Job& job, int slot);
    bool SubmitCopy(Job& job);
    void Poll();
    void WaitOldest();
    uint32_t StartOf(uint32_t sample) const;

    // pool and planning (call with the mutex held)
    int64_t Cost(int64_t frame) const;
    int64_t PlanTarget();
    bool CanReserve(int64_t frame) const;
    PoolFrame* Reserve(int64_t frame);
    int64_t UsableCapacity() const;
    int64_t pinned_frames = 0;  // pool frames the consumer holds (mutex)
    static constexpr int64_t kOutside = INT64_MAX, kBeyond = INT64_MAX - 1;
};

// ---------------------------------------------------------------------------------------------------------------

bool NriVideoSource::Impl::Init(const VideoContext& ctx, const std::string& path, const NriVideoOptions& opts,
                                std::string* why) {
    context = ctx;
    options = opts;
    core = ctx.core;
    device = ctx.device;
    graphics_queue = ctx.graphics_queue;
    if (!device || !core || !graphics_queue) {
        *why = "no NRI device, core interface or graphics queue in the video context";
        return false;
    }
    const nri::DeviceDesc& device_desc = core->GetDeviceDesc(*device);
    if (device_desc.graphicsAPI == nri::GraphicsAPI::METAL) {
        *why = "Metal has no NRI video decode in this build";
        return false;
    }
    if (!device_desc.features.video) {
        *why = "the device has no video support (no video decode queue, or the driver lacks the video extensions)";
        return false;
    }
    mp4 = Mp4::Open(path, why);
    if (!mp4)
        return false;
    const Mp4Track& track = mp4->track();
    switch (track.codec) {
        case Codec::AV1:
            if (!device_desc.videoFeatures.decode.AV1) {
                *why = "the video decode queue does not decode AV1";
                return false;
            }
            backend = MakeAv1Backend();
            break;
        case Codec::H264:
            if (!device_desc.videoFeatures.decode.H264) {
                *why = "the video decode queue does not decode H.264";
                return false;
            }
            backend = MakeH264Backend();
            break;
        case Codec::H265:
            if (!device_desc.videoFeatures.decode.H265) {
                *why = "the video decode queue does not decode H.265";
                return false;
            }
            backend = MakeH265Backend();
            break;
    }
    if (!backend->Init(*mp4, why))
        return false;
    format = backend->format();
    start_samples = backend->start_samples();
    if (format.bit_depth != 8 && format.bit_depth != 10) {
        *why = "bit depth " + std::to_string(format.bit_depth) + " is not supported";
        return false;
    }
    video_format = format.bit_depth == 8 ? nri::Format::NV12_UNORM : nri::Format::P010_UNORM;
    luma_format = format.bit_depth == 8 ? nri::Format::R8_UNORM : nri::Format::R16_UNORM;
    chroma_format = format.bit_depth == 8 ? nri::Format::RG8_UNORM : nri::Format::RG16_UNORM;
    bytes_per_sample = format.bit_depth == 8 ? 1 : 2;

    if (nri::nriGetInterface(*device, NRI_INTERFACE(nri::VideoInterface), &video) != nri::Result::SUCCESS) {
        *why = "NRI has no video interface on this device";
        return false;
    }
    // The sources of a process take the decode queues in turn: with two hardware decoders (two queues), two videos
    // decode at the same time.
    static std::atomic<uint32_t> next_decode_queue{0};
    const uint32_t queue_index = next_decode_queue.fetch_add(1);
    if (core->GetQueue(*device, nri::QueueType::VIDEO_DECODE, queue_index % 2, decode_queue) != nri::Result::SUCCESS &&
        core->GetQueue(*device, nri::QueueType::VIDEO_DECODE, 0, decode_queue) != nri::Result::SUCCESS) {
        *why = "the device has no video decode queue (create it with a video decode queue family)";
        return false;
    }

    // Session
    nri::VideoSessionDesc session_desc = {};
    session_desc.type = nri::VideoSessionType::DECODE;
    session_desc.codec = backend->nri_codec();
    session_desc.format = video_format;
    session_desc.width = format.coded_width;
    session_desc.height = format.coded_height;
    session_desc.maxReferenceNum = format.dpb_slots - 1;
    nri::Result caps_result = video.GetVideoCapabilities(*device, session_desc, caps);
    if (caps_result != nri::Result::SUCCESS && caps.widthMax == 0) {
        *why = "the driver does not decode this codec at " + std::to_string(format.bit_depth) + " bits";
        return false;
    }
    const uint32_t granularity_w = std::max(caps.pictureAccessGranularityWidth, 1u);
    const uint32_t granularity_h = std::max(caps.pictureAccessGranularityHeight, 1u);
    const uint32_t aligned_w = uint32_t(AlignUp(format.coded_width, granularity_w));
    const uint32_t aligned_h = uint32_t(AlignUp(format.coded_height, granularity_h));
    session_desc.width = aligned_w;
    session_desc.height = aligned_h;
    caps_result = video.GetVideoCapabilities(*device, session_desc, caps);
    if (caps_result != nri::Result::SUCCESS) {
        *why = "the driver cannot decode " + std::to_string(aligned_w) + "x" + std::to_string(aligned_h) +
               " with these settings (supported sizes " + std::to_string(caps.widthMin) + "x" +
               std::to_string(caps.heightMin) + " to " + std::to_string(caps.widthMax) + "x" +
               std::to_string(caps.heightMax) + ")";
        return false;
    }
    if (caps.maxReferenceNum < format.dpb_slots - 1) {
        *why = "the driver supports " + std::to_string(caps.maxReferenceNum) + " reference pictures, the stream needs " +
               std::to_string(format.dpb_slots - 1);
        return false;
    }
    if (!(caps.decodeBitstreamSourceMask & nri::VideoDecodeBitstreamSourceBits::BUFFER)) {
        *why = "the driver does not decode from a bitstream buffer";
        return false;
    }
    if (!caps.decodeDpbAndOutputCoincide) {
        *why = "the driver needs separate decode output pictures (not implemented)";
        return false;
    }
    if (caps.decodeReferenceOnlyRequired) {
        *why = "the driver keeps DPB pictures out of reach of copies (not implemented)";
        return false;
    }
    bitstream_offset_alignment = std::max<uint64_t>(caps.bitstreamOffsetAlignment, 1);
    bitstream_size_alignment = std::max<uint64_t>(caps.bitstreamSizeAlignment, 1);
    if (std::getenv("NR_VIDEO_TRACE"))
        std::fprintf(stderr,
                     "video caps: extent %ux%u..%ux%u granularity %ux%u max refs %u bitstream offset/size alignment %llu/%llu "
                     "max size %llu coincide %d distinct %d reference-only %d array %d (min layers %u) dpb slots wanted %u\n",
                     caps.widthMin, caps.heightMin, caps.widthMax, caps.heightMax, caps.pictureAccessGranularityWidth,
                     caps.pictureAccessGranularityHeight, caps.maxReferenceNum,
                     (unsigned long long)caps.bitstreamOffsetAlignment, (unsigned long long)caps.bitstreamSizeAlignment,
                     (unsigned long long)caps.bitstreamSizeMax, caps.decodeDpbAndOutputCoincide, caps.decodeDpbAndOutputDistinct,
                     caps.decodeReferenceOnlyRequired, caps.dpbTextureArrayRequired, caps.dpbTextureArrayMinLayerNum,
                     format.dpb_slots);
    {
        nri::Result r = video.CreateVideoSession(*device, session_desc, session);
        if (r != nri::Result::SUCCESS) {
            *why = "cannot create the video session (nri::Result " + std::to_string(int(r)) + ")";
            return false;
        }
        nri::VideoSessionParametersDesc parameters_desc = {};
        backend->FillSessionParameters(&parameters_desc);
        parameters_desc.session = session;
        r = video.CreateVideoSessionParameters(*device, parameters_desc, parameters);
        if (r != nri::Result::SUCCESS) {
            *why = "cannot create the video session parameters (nri::Result " + std::to_string(int(r)) + ")";
            return false;
        }
    }
    // DPB pictures
    {
        nri::TextureDesc desc = {};
        desc.type = nri::TextureType::TEXTURE_2D;
        desc.usage = nri::TextureUsageBits::VIDEO_DECODE;
        desc.format = video_format;
        desc.width = nri::Dim_t(aligned_w);
        desc.height = nri::Dim_t(aligned_h);
        desc.mipNum = 1;
        desc.layerNum = 1;
        desc.videoCodec = backend->nri_codec();
        const bool use_array = caps.dpbTextureArrayRequired;
        const uint32_t slots = std::max(format.dpb_slots, use_array ? caps.dpbTextureArrayMinLayerNum : 0u);
        dpb.assign(slots, Picture{});
        nri::Texture* array_texture = nullptr;
        if (use_array) {
            desc.layerNum = nri::Dim_t(slots);
            if (core->CreateCommittedTexture(*device, nri::MemoryLocation::DEVICE, 0.0f, desc, array_texture) !=
                nri::Result::SUCCESS) {
                *why = "cannot create the DPB texture array";
                return false;
            }
            dpb_textures.push_back(array_texture);
        }
        for (uint32_t i = 0; i < slots; i++) {
            Picture& p = dpb[i];
            if (use_array) {
                p.texture = array_texture;
                p.layer = i;
            } else {
                desc.layerNum = 1;
                if (core->CreateCommittedTexture(*device, nri::MemoryLocation::DEVICE, 0.0f, desc, p.texture) !=
                    nri::Result::SUCCESS) {
                    *why = "cannot create a DPB texture";
                    return false;
                }
                dpb_textures.push_back(p.texture);
            }
            nri::VideoPictureDesc picture_desc = {};
            picture_desc.texture = p.texture;
            picture_desc.usage = nri::VideoPictureUsage::DECODE_OUTPUT;
            picture_desc.width = nri::Dim_t(format.coded_width);
            picture_desc.height = nri::Dim_t(format.coded_height);
            picture_desc.layer = nri::Dim_t(p.layer);
            if (video.CreateVideoPicture(*device, picture_desc, p.picture) != nri::Result::SUCCESS) {
                *why = "cannot create a video picture";
                return false;
            }
        }
        // The required states do not depend on the picture.
        for (nri::VideoPictureRole role : {nri::VideoPictureRole::DECODE_OUTPUT, nri::VideoPictureRole::DECODE_REFERENCE,
                                           nri::VideoPictureRole::DECODE_SETUP, nri::VideoPictureRole::DECODE_OUTPUT_AND_SETUP}) {
            nri::VideoPictureState state = {};
            if (video.GetVideoPictureState(*dpb[0].picture, role, state) == nri::Result::SUCCESS)
                role_state[size_t(role)] = state.required;
        }
    }

    // Frame geometry
    luma_width = format.width;
    luma_height = format.height;
    chroma_width = (format.width + 1) / 2;
    chroma_height = (format.height + 1) / 2;
    {
        const uint32_t row_alignment = std::max(device_desc.memoryAlignment.uploadBufferTextureRow, 4u);
        const uint32_t slice_alignment = std::max(device_desc.memoryAlignment.uploadBufferTextureSlice, 4u);
        pitch_luma = uint32_t(AlignUp(uint64_t(luma_width) * bytes_per_sample, row_alignment));
        pitch_chroma = uint32_t(AlignUp(uint64_t(chroma_width) * 2 * bytes_per_sample, row_alignment));
        chroma_offset = AlignUp(uint64_t(pitch_luma) * luma_height, slice_alignment);
        staging_bytes = AlignUp(chroma_offset + uint64_t(pitch_chroma) * chroma_height, slice_alignment);
        frame_bytes = (uint64_t(luma_width) * luma_height + uint64_t(chroma_width) * chroma_height * 2) * bytes_per_sample;
        const uint64_t memory_frames = std::max<uint64_t>(options.memory_bytes / frame_bytes, 4);
        capacity = std::min<uint64_t>(memory_frames, track.display_to_sample.size());
        capacity = std::max<uint64_t>(capacity, std::min<uint64_t>(4, track.display_to_sample.size()));
    }

    if (!CreateJobs(why))
        return false;
    // The whole pool now: resources are created and destroyed on the opening and closing threads only (NRI's Destroy
    // calls are not thread safe), and the memory the limit names is taken at once.
    for (uint64_t i = 0; i < capacity; i++) {
        PoolFrame* created = CreatePoolFrame();
        if (!created) {
            *why = "cannot allocate the output frames (" + std::to_string(i) + " of " + std::to_string(capacity) + " made; lower the memory limit)";
            return false;
        }
        free_frames.push_back(created);
    }
    if (core->CreateFence(*device, {}, decode_fence) != nri::Result::SUCCESS ||
        core->CreateFence(*device, {}, copy_fence) != nri::Result::SUCCESS) {
        *why = "cannot create fences";
        return false;
    }

    // The clip's description for the viewer.
    info.width = format.width;
    info.height = format.height;
    info.fps = track.fps;
    info.frames = int64_t(track.display_to_sample.size());
    info.codec = CodecName(track.codec);
    info.bit_depth = format.bit_depth;
    uint8_t matrix = 2;
    bool full = false;
    if (format.has_color) {
        matrix = format.matrix;
        full = format.full_range;
    } else if (track.has_colr) {
        matrix = track.matrix;
        full = track.full_range;
    }
    info.range = full ? YuvRange::FULL : YuvRange::LIMITED;
    if (matrix == 5 || matrix == 6)
        info.matrix = YuvMatrix::BT601;
    else if (matrix == 9 || matrix == 10)
        info.matrix = YuvMatrix::BT2020;
    else if (matrix == 1)
        info.matrix = YuvMatrix::BT709;
    else
        info.matrix = format.height >= 720 ? YuvMatrix::BT709 : YuvMatrix::BT601;
    if (start_samples.size() > 1) {
        std::vector<uint32_t> gaps;
        for (size_t i = 1; i < start_samples.size(); i++)
            gaps.push_back(start_samples[i] - start_samples[i - 1]);
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
        info.keyframe_gap = gaps[gaps.size() / 2];
    }
    description = std::string("NRI ") + (device_desc.graphicsAPI == nri::GraphicsAPI::VK ? "Vulkan" : "D3D12") +
                  " Video " + (track.codec == Codec::AV1 ? "AV1" : track.codec == Codec::H264 ? "H.264" : "H.265") +
                  ", " + device_desc.adapterDesc.name;

    last = info.frames - 1;
    thread = std::thread([this] { Run(); });
    return true;
}

bool NriVideoSource::Impl::CreateJobs(std::string* why) {
    const uint64_t max_sample = [&] {
        uint32_t m = 0;
        for (const Sample& s : mp4->track().samples)
            m = std::max(m, s.size);
        return uint64_t(m);
    }();
    jobs.assign(std::max(options.in_flight, 2u), Job{});
    for (Job& job : jobs) {
        if (core->CreateCommandAllocator(*decode_queue, job.decode_allocator) != nri::Result::SUCCESS ||
            core->CreateCommandBuffer(*job.decode_allocator, job.decode_command) != nri::Result::SUCCESS ||
            core->CreateCommandAllocator(*graphics_queue, job.copy_allocator) != nri::Result::SUCCESS ||
            core->CreateCommandBuffer(*job.copy_allocator, job.copy_command) != nri::Result::SUCCESS) {
            *why = "cannot create command buffers";
            return false;
        }
        // The bitstream of a sample, each picture at an aligned offset and padded to the size alignment.
        job.bitstream_size = AlignUp(max_sample + 16 * (bitstream_offset_alignment + bitstream_size_alignment) + 4096, 4096);
        nri::BufferDesc buffer_desc = {};
        buffer_desc.size = job.bitstream_size;
        buffer_desc.usage = nri::BufferUsageBits::VIDEO_DECODE;
        if (core->CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_UPLOAD, 0.0f, buffer_desc, job.bitstream) !=
            nri::Result::SUCCESS) {
            *why = "cannot create the bitstream buffer";
            return false;
        }
        nri::BufferDesc staging_desc = {};
        staging_desc.size = staging_bytes;
        staging_desc.usage = nri::BufferUsageBits::NONE;
        if (core->CreateCommittedBuffer(*device, nri::MemoryLocation::DEVICE, 0.0f, staging_desc, job.staging) !=
            nri::Result::SUCCESS) {
            *why = "cannot create the staging buffer";
            return false;
        }
    }
    return true;
}

PoolFrame* NriVideoSource::Impl::CreatePoolFrame() {
    auto frame = std::make_unique<PoolFrame>();
    nri::TextureDesc desc = {};
    desc.type = nri::TextureType::TEXTURE_2D;
    desc.usage = nri::TextureUsageBits::SHADER_RESOURCE;
    desc.mipNum = 1;
    desc.layerNum = 1;
    desc.format = luma_format;
    desc.width = nri::Dim_t(luma_width);
    desc.height = nri::Dim_t(luma_height);
    bool ok = core->CreateCommittedTexture(*device, nri::MemoryLocation::DEVICE, 0.0f, desc, frame->luma_texture) ==
              nri::Result::SUCCESS;
    desc.format = chroma_format;
    desc.width = nri::Dim_t(chroma_width);
    desc.height = nri::Dim_t(chroma_height);
    ok = ok && core->CreateCommittedTexture(*device, nri::MemoryLocation::DEVICE, 0.0f, desc, frame->chroma_texture) ==
                   nri::Result::SUCCESS;
    if (ok) {
        nri::TextureViewDesc view = {};
        view.type = nri::TextureView::TEXTURE;
        view.mipNum = 1;
        view.layerNum = 1;
        view.sliceNum = 1;
        view.planes = nri::PlaneBits::ALL;
        view.texture = frame->luma_texture;
        view.format = luma_format;
        ok = core->CreateTextureView(view, frame->luma) == nri::Result::SUCCESS;
        view.texture = frame->chroma_texture;
        view.format = chroma_format;
        ok = ok && core->CreateTextureView(view, frame->chroma) == nri::Result::SUCCESS;
    }
    if (!ok) {
        Fail("cannot create an output frame");
        if (frame->luma)
            core->DestroyDescriptor(frame->luma);
        if (frame->chroma)
            core->DestroyDescriptor(frame->chroma);
        if (frame->luma_texture)
            core->DestroyTexture(frame->luma_texture);
        if (frame->chroma_texture)
            core->DestroyTexture(frame->chroma_texture);
        return nullptr;
    }
    PoolFrame* raw = frame.get();
    std::lock_guard<std::mutex> lock(mutex);
    raw->index = uint32_t(pool.size());
    pool.push_back(std::move(frame));
    return raw;
}

void NriVideoSource::Impl::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stop && !thread.joinable())
            return;
        stop = true;
    }
    wake.notify_all();
    if (thread.joinable())
        thread.join();
    if (!core || !device)
        return;
    // Everything that may still read or write our resources: our queues' work, and the consumer's on the graphics queue.
    if (graphics_queue)
        core->QueueWaitIdle(graphics_queue);
    if (decode_queue)
        core->QueueWaitIdle(decode_queue);
    for (Job& job : jobs) {
        if (job.decode_command)
            core->DestroyCommandBuffer(job.decode_command);
        if (job.decode_allocator)
            core->DestroyCommandAllocator(job.decode_allocator);
        if (job.copy_command)
            core->DestroyCommandBuffer(job.copy_command);
        if (job.copy_allocator)
            core->DestroyCommandAllocator(job.copy_allocator);
        if (job.bitstream)
            core->DestroyBuffer(job.bitstream);
        if (job.staging)
            core->DestroyBuffer(job.staging);
    }
    jobs.clear();
    if (readback_command)
        core->DestroyCommandBuffer(readback_command);
    if (readback_allocator)
        core->DestroyCommandAllocator(readback_allocator);
    if (readback)
        core->DestroyBuffer(readback);
    for (auto& frame : pool) {
        if (frame->luma)
            core->DestroyDescriptor(frame->luma);
        if (frame->chroma)
            core->DestroyDescriptor(frame->chroma);
        if (frame->luma_texture)
            core->DestroyTexture(frame->luma_texture);
        if (frame->chroma_texture)
            core->DestroyTexture(frame->chroma_texture);
    }
    pool.clear();
    if (video.DestroyVideoPicture)
        for (Picture& p : dpb)
            if (p.picture)
                video.DestroyVideoPicture(p.picture);
    for (nri::Texture* t : dpb_textures)
        core->DestroyTexture(t);
    if (parameters && video.DestroyVideoSessionParameters)
        video.DestroyVideoSessionParameters(parameters);
    if (session && video.DestroyVideoSession)
        video.DestroyVideoSession(session);
    if (decode_fence)
        core->DestroyFence(decode_fence);
    if (copy_fence)
        core->DestroyFence(copy_fence);
}

uint32_t NriVideoSource::Impl::StartOf(uint32_t sample) const {
    auto it = std::upper_bound(start_samples.begin(), start_samples.end(), sample);
    return it == start_samples.begin() ? start_samples.front() : *(it - 1);
}

// ---- pool and planning ------------------------------------------------------------------------------------------

int64_t NriVideoSource::Impl::UsableCapacity() const {
    return std::max<int64_t>(int64_t(capacity) - pinned_frames, 2);
}

// How much a frame is worth keeping: 0 at the playhead, growing with the distance, in the play direction slowly and
// against it four times faster. kOutside: not wanted at all. kBeyond: wanted, but outside the window the memory limit
// allows; such a frame (decoded on the way to others) is kept only while the pool has room, and is the first to go.
int64_t NriVideoSource::Impl::Cost(int64_t frame) const {
    constexpr int64_t kInf = kBeyond;
    if (frame < first || frame > last)
        return kOutside;
    const int64_t n = UsableCapacity();
    int64_t ahead = n * 3 / 4, behind = n - ahead;
    int64_t sign = direction >= 0 ? 1 : -1;
    if (direction == 0) {
        ahead = n / 2;
        behind = n - ahead;
    }
    const int64_t rel = (frame - playhead) * sign;
    if (rel >= 0)
        return rel < ahead ? rel : kInf;
    return -rel <= behind ? -rel * 4 : kInf;
}

bool NriVideoSource::Impl::CanReserve(int64_t frame) const {
    const int64_t cost = Cost(frame);
    if (cost >= kBeyond)
        return false;
    if (!free_frames.empty())
        return true;
    for (const auto& [f, entry] : cache) {
        if (entry.frame->pins > 0 || entry.state != EntryState::Ready)
            continue;
        if (Cost(f) > cost)
            return true;
    }
    return false;
}

// The missing frame to decode next: the cheapest one in the window that fits in the pool.
int64_t NriVideoSource::Impl::PlanTarget() {
    const int64_t n = UsableCapacity();
    const int64_t reach = std::max<int64_t>(n, 4 * n);  // costs worth looking at
    for (int64_t c = 0; c <= reach; c++) {
        const int64_t sign = direction >= 0 ? 1 : -1;
        const int64_t ahead_frame = playhead + sign * c;
        if (ahead_frame >= first && ahead_frame <= last && cache.find(ahead_frame) == cache.end() && CanReserve(ahead_frame))
            return ahead_frame;
        if (c > 0 && c % 4 == 0) {
            const int64_t behind_frame = playhead - sign * (c / 4);
            if (behind_frame >= first && behind_frame <= last && cache.find(behind_frame) == cache.end() &&
                CanReserve(behind_frame))
                return behind_frame;
        }
    }
    return -1;
}

// A pool frame for the frame's pixels: a free one, or the farthest evictable one that is worth less.
PoolFrame* NriVideoSource::Impl::Reserve(int64_t frame) {
    if (cache.count(frame))
        return nullptr;
    const int64_t cost = Cost(frame);
    if (cost == kOutside)
        return nullptr;
    PoolFrame* chosen = nullptr;
    if (!free_frames.empty()) {
        chosen = free_frames.back();
        free_frames.pop_back();
    } else if (cost == kBeyond) {
        return nullptr;
    } else {
        int64_t worst = cost;
        std::map<int64_t, Entry>::iterator victim = cache.end();
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (it->second.frame->pins > 0 || it->second.state != EntryState::Ready)
                continue;
            const int64_t c = Cost(it->first);
            if (c > worst) {
                worst = c;
                victim = it;
            }
        }
        if (victim == cache.end())
            return nullptr;
        chosen = victim->second.frame;
        cache.erase(victim);
    }
    cache[frame] = Entry{chosen, EntryState::Decoding, 0};
    return chosen;
}

// ---- the decode thread ------------------------------------------------------------------------------------------

void NriVideoSource::Impl::Run() {
    nri::nriSetThreadName("video decode");
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stop)
                break;
        }
        Poll();
        thread_cpu.store(ThreadCpuSeconds());
        bool failed;
        {
            std::lock_guard<std::mutex> lock(mutex);
            failed = !error.empty();
        }
        if (failed) {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop; });
            continue;
        }
        bool did = false;
        if (job_count < jobs.size())
            did = Step();
        if (!did) {
            if (job_count > 0)
                WaitOldest();
            else {
                // Idle: SetInterest and Release notify; the timeout is only a backstop.
                std::unique_lock<std::mutex> lock(mutex);
                if (!stop)
                    wake.wait_for(lock, std::chrono::milliseconds(50));
            }
        }
    }
    // Let the GPU finish what was submitted.
    for (size_t i = 0; i < job_count; i++) {
        Job& job = jobs[(job_head + i) % jobs.size()];
        if (job.state == Job::Decoding)
            core->Wait(*decode_fence, job.decode_value);
        if (job.state == Job::Copying)
            core->Wait(*copy_fence, job.copy_value);
    }
}

// Advances the jobs the GPU has finished: copies for finished decodes, frames for finished copies.
void NriVideoSource::Impl::Poll() {
    const uint64_t decoded = job_count ? core->GetFenceValue(*decode_fence) : 0;
    const uint64_t copied = job_count ? core->GetFenceValue(*copy_fence) : 0;
    for (size_t i = 0; i < job_count; i++) {
        Job& job = jobs[(job_head + i) % jobs.size()];
        if (job.state == Job::Decoding && decoded >= job.decode_value) {
            const double now = NowSeconds();
            {
                std::lock_guard<std::mutex> lock(mutex);
                completions.push_back(now);
                while (completions.size() > 64 || (!completions.empty() && now - completions.front() > 2.0))
                    completions.pop_front();
            }
            decoded_total++;
            if (job.out) {
                if (!SubmitCopy(job))
                    return;
            } else {
                job.state = Job::Free;
            }
        }
    }
    while (job_count > 0) {
        Job& job = jobs[job_head];
        if (job.state == Job::Copying && copied >= job.copy_value) {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = cache.find(job.frame);
            if (it != cache.end() && it->second.frame == job.out)
                it->second.state = EntryState::Ready;
            job.state = Job::Free;
            job.out = nullptr;
        }
        if (job.state != Job::Free)
            break;
        job_head = (job_head + 1) % jobs.size();
        job_count--;
    }
}

void NriVideoSource::Impl::WaitOldest() {
    if (job_count == 0)
        return;
    Job& job = jobs[job_head];
    if (job.state == Job::Decoding)
        core->Wait(*decode_fence, job.decode_value);
    else if (job.state == Job::Copying)
        core->Wait(*copy_fence, job.copy_value);
}

// One step of decoding: the next sample on the way to the cheapest missing frame.
bool NriVideoSource::Impl::Step() {
    int64_t target;
    {
        std::lock_guard<std::mutex> lock(mutex);
        target = PlanTarget();
    }
    if (target < 0)
        return false;
    const Mp4Track& track = mp4->track();
    const uint32_t target_sample = track.display_to_sample[size_t(target)];
    const uint32_t start = StartOf(target_sample);
    if (!(cursor_valid && cursor_gop == start && cursor <= target_sample)) {
        backend->Reset();
        cursor = start;
        cursor_gop = start;
        cursor_valid = true;
    }
    const uint32_t sample = cursor++;
    return DecodeSample(sample);
}

bool NriVideoSource::Impl::DecodeSample(uint32_t sample) {
    const Mp4Track& track = mp4->track();
    if (!mp4->ReadSample(sample, &sample_bytes)) {
        Fail("cannot read sample " + std::to_string(sample));
        return false;
    }
    std::vector<PictureJob> picture_jobs;
    std::string why;
    if (!backend->ParseSample(sample, sample_bytes, &picture_jobs, &why)) {
        Fail("sample " + std::to_string(sample) + ": " + why);
        return false;
    }
    Job& job = jobs[(job_head + job_count) % jobs.size()];
    job.state = Job::Free;
    job.out = nullptr;
    job.frame = -1;
    static const bool trace = std::getenv("NR_VIDEO_TRACE") != nullptr;
    if (trace) {
        for (const PictureJob& pj : picture_jobs) {
            std::string refs;
            for (int slot : pj.bound_slots)
                refs += " " + std::to_string(slot);
            std::string extra;
            if (const H264Job* h = std::get_if<H264Job>(&pj.codec)) {
                extra = " h264 frame_num " + std::to_string(h->frame_num) + " poc " + std::to_string(h->poc[0]) + (h->idr ? " idr" : "") +
                        (h->reference ? " ref" : " nonref") + " refs:";
                for (const H264Ref& r : h->refs)
                    extra += " [slot " + std::to_string(r.slot) + " fn " + std::to_string(r.frame_num) + " poc " + std::to_string(r.poc[0]) + (r.long_term ? " lt" : "") + "]";
            } else if (const H265Job* h = std::get_if<H265Job>(&pj.codec)) {
                extra = " h265 poc " + std::to_string(h->poc) + (h->idr ? " idr" : "") + (h->irap ? " irap" : "") + (h->reference ? " ref" : " nonref") + " refs:";
                for (const H265Ref& r : h->refs)
                    extra += " [slot " + std::to_string(r.slot) + " poc " + std::to_string(r.poc) + (r.long_term ? " lt" : "") + "]";
            }
            std::fprintf(stderr, "sample %u: decode %d setup %d shown %d display %lld bound%s%s\n", sample, pj.decode, pj.setup_slot,
                         pj.shown, (long long)track.sample_to_display[sample], refs.c_str(), extra.c_str());
        }
    }

    // The frame this sample displays, and a pool frame for it if it is wanted.
    const int64_t display = track.sample_to_display[sample];
    PoolFrame* out = nullptr;
    int shown_job = -1;
    for (size_t i = 0; i < picture_jobs.size(); i++)
        if (picture_jobs[i].shown)
            shown_job = int(i);
    if (display >= 0 && shown_job >= 0) {
        std::lock_guard<std::mutex> lock(mutex);
        out = Reserve(display);
    }

    nri::CommandBuffer& command = *job.decode_command;
    core->ResetCommandAllocator(*job.decode_allocator);
    if (core->BeginCommandBuffer(command, nullptr) != nri::Result::SUCCESS) {
        Fail("BeginCommandBuffer failed");
        return false;
    }
    uint8_t* mapped = static_cast<uint8_t*>(core->MapBuffer(*job.bitstream, 0, job.bitstream_size));
    if (!mapped) {
        Fail("cannot map the bitstream buffer");
        return false;
    }
    uint64_t cursor_bytes = 0;
    bool first_decode = true;
    for (size_t i = 0; i < picture_jobs.size(); i++) {
        PictureJob& pj = picture_jobs[i];
        if (pj.decode) {
            const uint64_t offset = AlignUp(cursor_bytes, bitstream_offset_alignment);
            const uint64_t padded = AlignUp(std::max<uint64_t>(pj.bitstream.size(), 1), bitstream_size_alignment);
            if (offset + padded > job.bitstream_size) {
                Fail("the sample's bitstream does not fit the bitstream buffer");
                core->UnmapBuffer(*job.bitstream);
                return false;
            }
            std::memcpy(mapped + offset, pj.bitstream.data(), pj.bitstream.size());
            std::memset(mapped + offset + pj.bitstream.size(), 0, padded - pj.bitstream.size());
            cursor_bytes = offset + padded;
            if (first_decode) {
                nri::BufferBarrierDesc buffer_barrier = {};
                buffer_barrier.buffer = job.bitstream;
                buffer_barrier.before = {nri::AccessBits::NONE, nri::StageBits::NONE};
                buffer_barrier.after = {nri::AccessBits::VIDEO_DECODE_READ, nri::StageBits::VIDEO_DECODE};
                nri::BarrierDesc barrier = {};
                if (context.device && core->GetDeviceDesc(*device).graphicsAPI != nri::GraphicsAPI::D3D12) {
                    barrier.buffers = &buffer_barrier;
                    barrier.bufferNum = 1;
                    core->CmdBarrier(command, barrier);
                }
                first_decode = false;
            }
            if (!RecordPicture(command, pj, job, offset, padded)) {
                core->UnmapBuffer(*job.bitstream);
                return false;
            }
        }
        if (pj.shown && int(i) == shown_job && out)
            RecordStageOne(command, job, pj.show_slot);
    }
    core->UnmapBuffer(*job.bitstream);
    if (core->EndCommandBuffer(command) != nri::Result::SUCCESS) {
        Fail("EndCommandBuffer failed");
        return false;
    }
    job.decode_value = ++decode_counter;
    nri::FenceSubmitDesc signal = {decode_fence, job.decode_value, nri::StageBits::ALL};
    const nri::CommandBuffer* commands[] = {&command};
    nri::QueueSubmitDesc submit = {};
    submit.commandBuffers = commands;
    submit.commandBufferNum = 1;
    submit.signalFences = &signal;
    submit.signalFenceNum = 1;
    if (core->QueueSubmit(*decode_queue, submit) != nri::Result::SUCCESS) {
        Fail("decode queue submit failed");
        return false;
    }
    job.state = Job::Decoding;
    job.out = out;
    job.frame = out ? display : -1;
    job_count++;
    return true;
}

namespace {

nri::TextureBarrierDesc PictureBarrier(const Picture& picture, nri::AccessLayoutStage before, nri::AccessLayoutStage after) {
    nri::TextureBarrierDesc barrier = {};
    barrier.texture = picture.texture;
    barrier.before = before;
    barrier.after = after;
    barrier.mipOffset = 0;
    barrier.mipNum = 1;
    barrier.layerOffset = nri::Dim_t(picture.layer);
    barrier.layerNum = 1;
    barrier.planes = nri::PlaneBits::ALL;
    return barrier;
}

// The AV1 decode description (NRIVideo.h) of a parsed frame; the arrays live in this object.
struct Av1Desc {
    nri::VideoAV1DecodePictureDesc picture = {};
    nri::VideoAV1TileLayoutDesc tile_layout = {};
    nri::VideoAV1QuantizationDesc quantization = {};
    nri::VideoAV1LoopFilterDesc loop_filter = {};
    nri::VideoAV1CdefDesc cdef = {};
    nri::VideoAV1SegmentationDesc segmentation = {};
    nri::VideoAV1LoopRestorationDesc loop_restoration = {};
    nri::VideoAV1GlobalMotionDesc global_motion = {};
    nri::VideoAV1DecodeTileDesc tiles[64] = {};
    nri::VideoAV1ReferenceDesc references[15] = {};
    uint8_t order_hints[8] = {};
    uint16_t mi_column_starts[65] = {}, mi_row_starts[65] = {};
    uint16_t width_in_sbs[64] = {}, height_in_sbs[64] = {};

    void Fill(const Av1Job& job) {
        const av1::FrameHeader& h = job.pic.h;
        picture.frameType = h.frame_type == av1::kKeyFrame ? nri::VideoFrameType::IDR : nri::VideoFrameType::P;
        picture.orderHint = h.order_hint;
        picture.refreshFrameFlags = h.refresh_frame_flags;
        picture.primaryReferenceName =
            h.primary_ref_frame == av1::kPrimaryRefNone ? nri::VideoAV1ReferenceName::NONE
                                                        : nri::VideoAV1ReferenceName(1 + h.primary_ref_frame);
        picture.currentFrameId = h.current_frame_id;
        picture.frameHeaderOffset = 0;
        using B = nri::VideoAV1PictureBits;
        B flags = B::SHOWABLE_FRAME;  // a zero mask would mean the default flags
        auto set = [&](bool cond, B bit) {
            if (cond)
                flags = flags | bit;
        };
        set(h.error_resilient_mode, B::ERROR_RESILIENT_MODE);
        set(h.disable_cdf_update, B::DISABLE_CDF_UPDATE);
        set(h.use_superres, B::USE_SUPERRES);
        set(h.render_and_frame_size_different, B::RENDER_AND_FRAME_SIZE_DIFFERENT);
        set(h.allow_screen_content_tools, B::ALLOW_SCREEN_CONTENT_TOOLS);
        set(h.is_filter_switchable, B::IS_FILTER_SWITCHABLE);
        set(h.force_integer_mv, B::FORCE_INTEGER_MV);
        set(h.frame_size_override_flag, B::FRAME_SIZE_OVERRIDE);
        set(h.buffer_removal_time_present, B::BUFFER_REMOVAL_TIME_PRESENT);
        set(h.allow_intrabc, B::ALLOW_INTRABC);
        set(h.frame_refs_short_signaling, B::FRAME_REFS_SHORT_SIGNALING);
        set(h.allow_high_precision_mv, B::ALLOW_HIGH_PRECISION_MV);
        set(h.is_motion_mode_switchable, B::IS_MOTION_MODE_SWITCHABLE);
        set(h.use_ref_frame_mvs, B::USE_REF_FRAME_MVS);
        set(h.disable_frame_end_update_cdf, B::DISABLE_FRAME_END_UPDATE_CDF);
        set(h.allow_warped_motion, B::ALLOW_WARPED_MOTION);
        set(h.reduced_tx_set, B::REDUCED_TX_SET);
        set(h.reference_select, B::REFERENCE_SELECT);
        set(h.skip_mode_present, B::SKIP_MODE_PRESENT);
        set(h.delta_q_present, B::DELTA_Q_PRESENT);
        set(h.delta_lf_present, B::DELTA_LF_PRESENT);
        set(h.delta_lf_multi, B::DELTA_LF_MULTI);
        set(h.segmentation_enabled, B::SEGMENTATION_ENABLED);
        set(h.segmentation_update_map, B::SEGMENTATION_UPDATE_MAP);
        set(h.segmentation_temporal_update, B::SEGMENTATION_TEMPORAL_UPDATE);
        set(h.segmentation_update_data, B::SEGMENTATION_UPDATE_DATA);
        set(h.uses_lr, B::USES_LR);
        set(h.uses_chroma_lr, B::USES_CHROMA_LR);
        set(h.show_frame, B::SHOW_FRAME);
        picture.flags = flags;
        picture.renderWidthMinus1 = uint16_t(h.render_width - 1);
        picture.renderHeightMinus1 = uint16_t(h.render_height - 1);
        picture.baseQIndex = h.base_q_idx;
        picture.superresDenom = 8;
        picture.codedDenom = h.coded_denom;
        picture.interpolationFilter = h.interpolation_filter;
        picture.txMode = h.tx_mode;
        picture.cdefDampingMinus3 = h.cdef_damping_minus_3;
        picture.cdefBits = h.cdef_bits;
        picture.deltaQRes = h.delta_q_res;
        picture.deltaLfRes = h.delta_lf_res;

        tile_layout.columnNum = uint8_t(h.tile_cols);
        tile_layout.rowNum = uint8_t(h.tile_rows);
        tile_layout.tileSizeBytesMinus1 = uint8_t(h.tile_size_bytes_minus_1);
        tile_layout.uniformSpacing = h.uniform_tile_spacing;
        tile_layout.contextUpdateTileId = uint16_t(h.context_update_tile_id);
        std::memcpy(mi_column_starts, h.mi_col_starts, sizeof(mi_column_starts));
        std::memcpy(mi_row_starts, h.mi_row_starts, sizeof(mi_row_starts));
        std::memcpy(width_in_sbs, h.width_in_sbs_minus_1, sizeof(width_in_sbs));
        std::memcpy(height_in_sbs, h.height_in_sbs_minus_1, sizeof(height_in_sbs));
        tile_layout.miColumnStarts = mi_column_starts;
        tile_layout.miRowStarts = mi_row_starts;
        tile_layout.widthInSuperblocksMinus1 = width_in_sbs;
        tile_layout.heightInSuperblocksMinus1 = height_in_sbs;

        quantization.deltaQYDc = h.delta_q_y_dc;
        quantization.deltaQUDc = h.delta_q_u_dc;
        quantization.deltaQUAc = h.delta_q_u_ac;
        quantization.deltaQVDc = h.delta_q_v_dc;
        quantization.deltaQVAc = h.delta_q_v_ac;
        quantization.qmY = h.qm_y;
        quantization.qmU = h.qm_u;
        quantization.qmV = h.qm_v;
        quantization.usingQmatrix = h.using_qmatrix;
        quantization.diffUvDelta = h.diff_uv_delta;

        std::memcpy(loop_filter.level, h.loop_filter_level, 4);
        loop_filter.sharpness = h.loop_filter_sharpness;
        loop_filter.deltaEnabled = h.loop_filter_delta_enabled;
        loop_filter.deltaUpdate = h.loop_filter_delta_update;
        loop_filter.updateModeDelta = h.update_mode_delta;
        std::memcpy(loop_filter.refDeltas, h.loop_filter_ref_deltas, 8);
        std::memcpy(loop_filter.modeDeltas, h.loop_filter_mode_deltas, 2);

        std::memcpy(cdef.yPrimaryStrength, h.cdef_y_pri, 8);
        std::memcpy(cdef.ySecondaryStrength, h.cdef_y_sec, 8);
        std::memcpy(cdef.uvPrimaryStrength, h.cdef_uv_pri, 8);
        std::memcpy(cdef.uvSecondaryStrength, h.cdef_uv_sec, 8);

        // FeatureEnabled per segment is a bit mask over the features (SEG_LVL_ALT_Q and the others).
        for (int segment = 0; segment < 8; segment++) {
            uint8_t mask = 0;
            for (int feature = 0; feature < 8; feature++)
                if (h.feature_enabled[segment][feature])
                    mask |= uint8_t(1u << feature);
            segmentation.featureEnabled[segment] = mask;
        }
        std::memcpy(segmentation.featureData, h.feature_data, sizeof(segmentation.featureData));

        std::memcpy(loop_restoration.frameRestorationType, h.frame_restoration_type, 3);
        loop_restoration.lrUnitShift = h.lr_unit_shift;
        loop_restoration.lrUvShift = h.lr_uv_shift;

        std::memcpy(global_motion.type, h.gm_type, 8);
        std::memcpy(global_motion.params, h.gm_params, sizeof(global_motion.params));

        std::memcpy(order_hints, h.order_hints, 8);
        picture.orderHints = order_hints;
        picture.skipModeFrames[0] = nri::VideoAV1ReferenceName(h.skip_mode_frame[0]);
        picture.skipModeFrames[1] = nri::VideoAV1ReferenceName(h.skip_mode_frame[1]);

        picture.tileLayout = &tile_layout;
        picture.quantization = &quantization;
        picture.loopFilter = &loop_filter;
        picture.cdef = &cdef;
        picture.segmentation = &segmentation;
        picture.loopRestoration = &loop_restoration;
        picture.globalMotion = &global_motion;
        picture.filmGrain = nullptr;

        const size_t tile_count = std::min<size_t>(job.pic.tiles.size(), 64);
        for (size_t i = 0; i < tile_count; i++) {
            tiles[i].offset = job.tile_offsets[i];
            tiles[i].size = job.tile_sizes[i];
            tiles[i].row = job.pic.tiles[i].row;
            tiles[i].column = job.pic.tiles[i].column;
            tiles[i].anchorFrame = 0xff;
        }
        picture.tiles = tiles;
        picture.tileNum = uint32_t(tile_count);

        // The reference map: a named entry per reference name, then an unnamed one for every other valid map entry.
        uint32_t reference_num = 0;
        bool named[av1::kNumRefFrames] = {};
        for (int name = 0; name < av1::kRefsPerFrame; name++) {
            if (h.frame_type == av1::kKeyFrame || h.frame_type == av1::kIntraOnlyFrame)
                break;
            const int entry = h.ref_frame_idx[name];
            named[entry] = true;
            Describe(job, entry, nri::VideoAV1ReferenceName(1 + name), &references[reference_num++]);
        }
        for (int entry = 0; entry < av1::kNumRefFrames; entry++)
            if (!named[entry] && job.slot_of_ref[entry] >= 0 && job.pic.refs[entry].valid)
                Describe(job, entry, nri::VideoAV1ReferenceName::NONE, &references[reference_num++]);
        picture.references = references;
        picture.referenceNum = reference_num;
    }

    void Describe(const Av1Job& job, int entry, nri::VideoAV1ReferenceName name, nri::VideoAV1ReferenceDesc* out) {
        const av1::RefInfo& ref = job.pic.refs[entry];
        out->name = name;
        out->refFrameIndex = uint8_t(entry);
        out->frameType = ref.frame_type == av1::kKeyFrame ? nri::VideoFrameType::IDR : nri::VideoFrameType::P;
        out->orderHint = ref.order_hint;
        out->frameId = ref.frame_id;
        out->slot = uint32_t(job.slot_of_ref[entry]);
        out->savedOrderHints = ref.saved_order_hints;
    }
};

}  // namespace

bool NriVideoSource::Impl::RecordPicture(nri::CommandBuffer& command, const PictureJob& pj, Job& job, uint64_t offset,
                                         uint64_t range) {
    Picture& setup = dpb[size_t(pj.setup_slot)];
    nri::VideoDecodeDesc decode = {};
    decode.session = session;
    decode.parameters = parameters;
    decode.bitstream.buffer = job.bitstream;
    decode.bitstream.offset = offset;
    decode.bitstream.size = range;
    decode.dstPicture = setup.picture;
    decode.dstSlot = uint32_t(pj.setup_slot);

    std::vector<nri::VideoReference> references;
    for (int slot : pj.bound_slots)
        references.push_back({dpb[size_t(slot)].picture, uint32_t(slot)});
    decode.references = references.empty() ? nullptr : references.data();
    decode.referenceNum = uint32_t(references.size());

    Av1Desc av1_desc;
    nri::VideoH264DecodePictureDesc h264_desc = {};
    nri::VideoH265DecodePictureDesc h265_desc = {};
    std::vector<nri::VideoH264DecodeReferenceDesc> h264_refs;
    std::vector<nri::VideoH265ReferenceDesc> h265_refs;
    if (const Av1Job* a = std::get_if<Av1Job>(&pj.codec)) {
        av1_desc.Fill(*a);
        decode.av1PictureDesc = &av1_desc.picture;
    } else if (const H264Job* h = std::get_if<H264Job>(&pj.codec)) {
        h264_desc.flags = nri::VideoH264DecodePictureBits::NONE;
        if (h->intra)
            h264_desc.flags |= nri::VideoH264DecodePictureBits::INTRA;
        if (h->idr)
            h264_desc.flags |= nri::VideoH264DecodePictureBits::IDR;
        if (h->reference)
            h264_desc.flags |= nri::VideoH264DecodePictureBits::REFERENCE;
        h264_desc.sequenceParameterSetId = h->sps_id;
        h264_desc.pictureParameterSetId = h->pps_id;
        h264_desc.frameNum = h->frame_num;
        h264_desc.idrPictureId = h->idr_pic_id;
        h264_desc.topFieldOrderCount = h->poc[0];
        h264_desc.bottomFieldOrderCount = h->poc[1];
        h264_desc.sliceOffsets = h->slice_offsets.data();
        h264_desc.sliceOffsetNum = uint32_t(h->slice_offsets.size());
        for (const H264Ref& r : h->refs) {
            nri::VideoH264DecodeReferenceDesc d = {};
            d.flags = nri::VideoH264DecodeReferenceBits::TOP_FIELD | nri::VideoH264DecodeReferenceBits::BOTTOM_FIELD;
            if (r.long_term)
                d.flags |= nri::VideoH264DecodeReferenceBits::LONG_TERM;
            d.frameNum = r.frame_num;
            d.slot = uint32_t(r.slot);
            d.topFieldOrderCount = r.poc[0];
            d.bottomFieldOrderCount = r.poc[1];
            h264_refs.push_back(d);
        }
        h264_desc.references = h264_refs.empty() ? nullptr : h264_refs.data();
        h264_desc.referenceNum = uint32_t(h264_refs.size());
        decode.h264PictureDesc = &h264_desc;
    } else if (const H265Job* h = std::get_if<H265Job>(&pj.codec)) {
        h265_desc.flags = nri::VideoH265DecodePictureBits::NONE;
        if (h->irap)
            h265_desc.flags |= nri::VideoH265DecodePictureBits::IRAP;
        if (h->idr)
            h265_desc.flags |= nri::VideoH265DecodePictureBits::IDR;
        if (h->reference)
            h265_desc.flags |= nri::VideoH265DecodePictureBits::REFERENCE;
        if (h->short_term_ref_pic_set_sps)
            h265_desc.flags |= nri::VideoH265DecodePictureBits::SHORT_TERM_REF_PIC_SET_SPS;
        h265_desc.videoParameterSetId = h->vps_id;
        h265_desc.sequenceParameterSetId = h->sps_id;
        h265_desc.pictureParameterSetId = h->pps_id;
        h265_desc.pictureOrderCount = h->poc;
        h265_desc.numDeltaPocsOfRefRpsIdx = h->num_delta_pocs_of_ref_rps_idx;
        h265_desc.numBitsForShortTermRefPicSetInSlice = h->num_bits_for_st_ref_pic_set_in_slice;
        h265_desc.sliceSegmentOffsets = h->slice_offsets.data();
        h265_desc.sliceSegmentOffsetNum = uint32_t(h->slice_offsets.size());
        for (const H265Ref& r : h->refs) {
            nri::VideoH265ReferenceDesc d = {};
            d.slot = uint32_t(r.slot);
            d.pictureOrderCount = r.poc;
            d.frameType = nri::VideoFrameType::P;
            d.longTerm = r.long_term ? 1 : 0;
            h265_refs.push_back(d);
        }
        h265_desc.references = h265_refs.empty() ? nullptr : h265_refs.data();
        h265_desc.referenceNum = uint32_t(h265_refs.size());
        decode.h265PictureDesc = &h265_desc;
    }

    // Barriers: the new picture to its decode state, each bound reference to the reference state.
    nri::TextureBarrierDesc barriers[16];
    uint32_t barrier_num = 0;
    const nri::AccessLayoutStage setup_state = role_state[size_t(nri::VideoPictureRole::DECODE_OUTPUT_AND_SETUP)];
    const nri::AccessLayoutStage reference_state = role_state[size_t(nri::VideoPictureRole::DECODE_REFERENCE)];
    barriers[barrier_num++] = PictureBarrier(setup, setup.state, setup_state);
    for (int slot : pj.bound_slots) {
        Picture& ref = dpb[size_t(slot)];
        if (slot == pj.setup_slot)
            continue;
        const bool same = ref.state.layout == reference_state.layout && !(ref.state.access & nri::AccessBits::VIDEO_DECODE_WRITE);
        if (!same && barrier_num < 16)
            barriers[barrier_num++] = PictureBarrier(ref, ref.state, reference_state);
    }
    nri::BarrierDesc barrier = {};
    barrier.textures = barriers;
    barrier.textureNum = barrier_num;
    core->CmdBarrier(command, barrier);
    setup.state = setup_state;
    for (int slot : pj.bound_slots)
        if (slot != pj.setup_slot)
            dpb[size_t(slot)].state = reference_state;

    video.CmdDecodeVideo(command, decode);
    return true;
}

// On the decode queue, after the decode: both planes of the displayed picture into the staging buffer.
void NriVideoSource::Impl::RecordStageOne(nri::CommandBuffer& command, Job& job, int slot) {
    Picture& picture = dpb[size_t(slot)];
    nri::TextureBarrierDesc texture_barrier = PictureBarrier(picture, picture.state, CopySourceState());
    nri::BufferBarrierDesc buffer_barrier = {};
    buffer_barrier.buffer = job.staging;
    buffer_barrier.before = {nri::AccessBits::NONE, nri::StageBits::NONE};
    buffer_barrier.after = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
    nri::BarrierDesc barrier = {};
    barrier.textures = &texture_barrier;
    barrier.textureNum = 1;
    barrier.buffers = &buffer_barrier;
    barrier.bufferNum = 1;
    core->CmdBarrier(command, barrier);
    picture.state = CopySourceState();

    // The displayed rectangle; NRI halves a plane 1 region's numbers, so they are given in luma units, rounded up to even.
    nri::TextureRegionDesc luma = {};
    luma.x = nri::Dim_t(format.crop_x);
    luma.y = nri::Dim_t(format.crop_y);
    luma.width = nri::Dim_t(luma_width);
    luma.height = nri::Dim_t(luma_height);
    luma.depth = 1;
    luma.layerOffset = nri::Dim_t(picture.layer);
    luma.planes = nri::PlaneBits::PLANE_0;
    nri::TextureDataLayoutDesc luma_layout = {};
    luma_layout.offset = 0;
    luma_layout.rowPitch = pitch_luma;
    luma_layout.slicePitch = pitch_luma * luma_height;
    core->CmdReadbackTextureToBuffer(command, *job.staging, luma_layout, *picture.texture, luma);

    nri::TextureRegionDesc chroma = luma;
    chroma.width = nri::Dim_t((luma_width + 1) & ~1u);
    chroma.height = nri::Dim_t((luma_height + 1) & ~1u);
    chroma.planes = nri::PlaneBits::PLANE_1;
    nri::TextureDataLayoutDesc chroma_layout = {};
    chroma_layout.offset = chroma_offset;
    chroma_layout.rowPitch = pitch_chroma;
    chroma_layout.slicePitch = pitch_chroma * chroma_height;
    core->CmdReadbackTextureToBuffer(command, *job.staging, chroma_layout, *picture.texture, chroma);
}

// On the graphics queue, once the decode has finished: the staging buffer into the pool frame's textures.
bool NriVideoSource::Impl::SubmitCopy(Job& job) {
    PoolFrame& frame = *job.out;
    nri::CommandBuffer& command = *job.copy_command;
    core->ResetCommandAllocator(*job.copy_allocator);
    NRI_TRY(core->BeginCommandBuffer(command, nullptr), "BeginCommandBuffer");

    nri::TextureBarrierDesc to_copy[2] = {};
    for (int i = 0; i < 2; i++) {
        to_copy[i].texture = i == 0 ? frame.luma_texture : frame.chroma_texture;
        to_copy[i].before = frame.state;
        to_copy[i].after = CopyDestinationState();
        to_copy[i].mipNum = 1;
        to_copy[i].layerNum = 1;
        to_copy[i].planes = nri::PlaneBits::ALL;
    }
    nri::BufferBarrierDesc buffer_barrier = {};
    buffer_barrier.buffer = job.staging;
    buffer_barrier.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
    buffer_barrier.after = {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY};
    nri::BarrierDesc barrier = {};
    barrier.textures = to_copy;
    barrier.textureNum = 2;
    barrier.buffers = &buffer_barrier;
    barrier.bufferNum = 1;
    core->CmdBarrier(command, barrier);

    nri::TextureRegionDesc luma_region = {};
    luma_region.width = nri::Dim_t(luma_width);
    luma_region.height = nri::Dim_t(luma_height);
    luma_region.depth = 1;
    nri::TextureDataLayoutDesc luma_layout = {0, pitch_luma, pitch_luma * luma_height};
    core->CmdUploadBufferToTexture(command, *frame.luma_texture, luma_region, *job.staging, luma_layout);
    nri::TextureRegionDesc chroma_region = luma_region;
    chroma_region.width = nri::Dim_t(chroma_width);
    chroma_region.height = nri::Dim_t(chroma_height);
    nri::TextureDataLayoutDesc chroma_layout = {chroma_offset, pitch_chroma, pitch_chroma * chroma_height};
    core->CmdUploadBufferToTexture(command, *frame.chroma_texture, chroma_region, *job.staging, chroma_layout);

    nri::TextureBarrierDesc to_sample[2] = {};
    for (int i = 0; i < 2; i++) {
        to_sample[i] = to_copy[i];
        to_sample[i].before = CopyDestinationState();
        to_sample[i].after = ShaderResourceState();
    }
    barrier = {};
    barrier.textures = to_sample;
    barrier.textureNum = 2;
    core->CmdBarrier(command, barrier);
    frame.state = ShaderResourceState();
    NRI_TRY(core->EndCommandBuffer(command), "EndCommandBuffer");

    job.copy_value = ++copy_counter;
    nri::FenceSubmitDesc wait = {decode_fence, job.decode_value, nri::StageBits::ALL};
    nri::FenceSubmitDesc signal = {copy_fence, job.copy_value, nri::StageBits::ALL};
    const nri::CommandBuffer* commands[] = {&command};
    nri::QueueSubmitDesc submit = {};
    submit.waitFences = &wait;
    submit.waitFenceNum = 1;
    submit.commandBuffers = commands;
    submit.commandBufferNum = 1;
    submit.signalFences = &signal;
    submit.signalFenceNum = 1;
    NRI_TRY(core->QueueSubmit(*graphics_queue, submit), "graphics queue submit");
    job.state = Job::Copying;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(job.frame);
        if (it != cache.end() && it->second.frame == job.out) {
            it->second.state = EntryState::Copying;
            it->second.copy_value = job.copy_value;
        }
    }
    return true;
}

// ---- VideoSource --------------------------------------------------------------------------------------------------

NriVideoSource::NriVideoSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NriVideoSource::~NriVideoSource() = default;

std::unique_ptr<NriVideoSource> NriVideoSource::OpenWithOptions(const VideoContext& context, const std::string& path,
                                                                const NriVideoOptions& options, std::string* why_not) {
    std::string reason;
    if (!why_not)
        why_not = &reason;
    auto impl = std::make_unique<Impl>();
    if (!impl->Init(context, path, options, why_not)) {
        impl->Shutdown();
        return nullptr;
    }
    return std::unique_ptr<NriVideoSource>(new NriVideoSource(std::move(impl)));
}

std::unique_ptr<VideoSource> NriVideoSource::Open(const VideoContext& context, const std::string& path,
                                                  std::string* why_not) {
    return OpenWithOptions(context, path, NriVideoOptions{}, why_not);
}

const VideoInfo& NriVideoSource::Info() const { return impl_->info; }

void NriVideoSource::SetInterest(int64_t first, int64_t last, int64_t playhead, int direction) {
    Impl& s = *impl_;
    const int dir = direction > 0 ? 1 : direction < 0 ? -1 : 0;
    first = std::max<int64_t>(first, 0);
    last = std::min<int64_t>(last, s.info.frames - 1);
    playhead = std::clamp<int64_t>(playhead, 0, std::max<int64_t>(s.info.frames - 1, 0));
    bool changed;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        changed = s.first != first || s.last != last || s.playhead != playhead || s.direction != dir;
        s.first = first;
        s.last = last;
        s.playhead = playhead;
        s.direction = dir;
    }
    if (changed)
        s.wake.notify_one();
}

bool NriVideoSource::Ready(int64_t frame) const {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.cache.find(frame);
    if (it == s.cache.end())
        return false;
    const Entry& e = it->second;
    if (e.state == EntryState::Ready)
        return true;
    return e.state == EntryState::Copying && s.core->GetFenceValue(*s.copy_fence) >= e.copy_value;
}

bool NriVideoSource::Acquire(int64_t frame, FrameRef& out) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.cache.find(frame);
    if (it == s.cache.end())
        return false;
    Entry& e = it->second;
    if (e.state == EntryState::Copying && s.core->GetFenceValue(*s.copy_fence) >= e.copy_value)
        e.state = EntryState::Ready;
    if (e.state != EntryState::Ready)
        return false;
    if (e.frame->pins++ == 0)
        s.pinned_frames++;
    out.index = frame;
    out.luma = e.frame->luma;
    out.chroma = e.frame->chroma;
    out.slot = e.frame->index;
    return true;
}

void NriVideoSource::Release(const FrameRef& frame) {
    Impl& s = *impl_;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (frame.slot < s.pool.size() && s.pool[frame.slot]->pins > 0) {
            if (--s.pool[frame.slot]->pins == 0)
                s.pinned_frames--;
            wake = true;
        }
    }
    if (wake)
        s.wake.notify_one();
}

DecodeStats NriVideoSource::Stats() const {
    Impl& s = *impl_;
    DecodeStats stats;
    std::lock_guard<std::mutex> lock(s.mutex);
    stats.cached = int64_t(s.cache.size());
    stats.cached_bytes = uint64_t(s.cache.size()) * s.frame_bytes;
    stats.decoded_total = s.decoded_total.load();
    const double now = NowSeconds();
    if (s.completions.size() >= 2 && now - s.completions.back() < 1.0) {
        const double span = s.completions.back() - s.completions.front();
        if (span > 0)
            stats.decode_fps = double(s.completions.size() - 1) / span;
    }
    // Frames in flight, and wanted frames ahead of the playhead that are not cached.
    int64_t pending = int64_t(s.job_count);
    const int64_t reach = std::max<int64_t>(int64_t(s.capacity) * 3 / 4, 1);
    for (int64_t i = 0; i < reach; i++) {
        const int64_t frame = s.playhead + (s.direction >= 0 ? i : -i);
        if (frame >= s.first && frame <= s.last && !s.cache.count(frame))
            pending++;
    }
    stats.pending = pending;
    return stats;
}

std::string NriVideoSource::Describe() const {
    std::string text = impl_->description;
    const std::string error = Error();
    if (!error.empty())
        text += " (stopped: " + error + ")";
    return text;
}

std::string NriVideoSource::Error() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

uint64_t NriVideoSource::frame_bytes() const { return impl_->frame_bytes; }
uint64_t NriVideoSource::pool_capacity() const { return impl_->capacity; }
double NriVideoSource::DecodeThreadCpuSeconds() const { return impl_->thread_cpu.load(); }

bool NriVideoSource::ReadFrame(int64_t frame, std::vector<uint8_t>* luma, std::vector<uint8_t>* chroma) {
    Impl& s = *impl_;
    FrameRef ref;
    if (!Acquire(frame, ref))
        return false;
    std::lock_guard<std::mutex> readback_lock(s.readback_mutex);
    PoolFrame* pf;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        pf = s.pool[ref.slot].get();
    }
    bool ok = true;
    if (!s.readback) {
        nri::BufferDesc desc = {};
        desc.size = s.staging_bytes;
        ok = s.core->CreateCommittedBuffer(*s.device, nri::MemoryLocation::HOST_READBACK, 0.0f, desc, s.readback) ==
                 nri::Result::SUCCESS &&
             s.core->CreateCommandAllocator(*s.graphics_queue, s.readback_allocator) == nri::Result::SUCCESS &&
             s.core->CreateCommandBuffer(*s.readback_allocator, s.readback_command) == nri::Result::SUCCESS;
        if (ok)
            s.readback_mapped = static_cast<uint8_t*>(s.core->MapBuffer(*s.readback, 0, s.staging_bytes));
        ok = ok && s.readback_mapped;
    }
    if (ok) {
        nri::CommandBuffer& command = *s.readback_command;
        s.core->ResetCommandAllocator(*s.readback_allocator);
        ok = s.core->BeginCommandBuffer(command, nullptr) == nri::Result::SUCCESS;
        nri::TextureBarrierDesc to_copy[2] = {};
        for (int i = 0; i < 2; i++) {
            to_copy[i].texture = i == 0 ? pf->luma_texture : pf->chroma_texture;
            to_copy[i].before = ShaderResourceState();
            to_copy[i].after = CopySourceState();
            to_copy[i].mipNum = 1;
            to_copy[i].layerNum = 1;
            to_copy[i].planes = nri::PlaneBits::ALL;
        }
        nri::BufferBarrierDesc buffer_barrier = {};
        buffer_barrier.buffer = s.readback;
        buffer_barrier.before = {nri::AccessBits::NONE, nri::StageBits::NONE};
        buffer_barrier.after = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        nri::BarrierDesc barrier = {};
        barrier.textures = to_copy;
        barrier.textureNum = 2;
        barrier.buffers = &buffer_barrier;
        barrier.bufferNum = 1;
        s.core->CmdBarrier(command, barrier);
        nri::TextureRegionDesc region = {};
        region.width = nri::Dim_t(s.luma_width);
        region.height = nri::Dim_t(s.luma_height);
        region.depth = 1;
        nri::TextureDataLayoutDesc luma_layout = {0, s.pitch_luma, s.pitch_luma * s.luma_height};
        s.core->CmdReadbackTextureToBuffer(command, *s.readback, luma_layout, *pf->luma_texture, region);
        region.width = nri::Dim_t(s.chroma_width);
        region.height = nri::Dim_t(s.chroma_height);
        nri::TextureDataLayoutDesc chroma_layout = {s.chroma_offset, s.pitch_chroma, s.pitch_chroma * s.chroma_height};
        s.core->CmdReadbackTextureToBuffer(command, *s.readback, chroma_layout, *pf->chroma_texture, region);
        for (int i = 0; i < 2; i++) {
            std::swap(to_copy[i].before, to_copy[i].after);
        }
        barrier.buffers = nullptr;  // the host reads after the queue went idle; no barrier names host access to a buffer
        barrier.bufferNum = 0;
        s.core->CmdBarrier(command, barrier);
        ok = ok && s.core->EndCommandBuffer(command) == nri::Result::SUCCESS;
        if (ok) {
            const nri::CommandBuffer* commands[] = {&command};
            nri::QueueSubmitDesc submit = {};
            submit.commandBuffers = commands;
            submit.commandBufferNum = 1;
            ok = s.core->QueueSubmit(*s.graphics_queue, submit) == nri::Result::SUCCESS &&
                 s.core->QueueWaitIdle(s.graphics_queue) == nri::Result::SUCCESS;
        }
    }
    if (ok) {
        luma->resize(size_t(s.luma_width) * s.luma_height * s.bytes_per_sample);
        chroma->resize(size_t(s.chroma_width) * s.chroma_height * 2 * s.bytes_per_sample);
        for (uint32_t y = 0; y < s.luma_height; y++)
            std::memcpy(luma->data() + size_t(y) * s.luma_width * s.bytes_per_sample, s.readback_mapped + size_t(y) * s.pitch_luma,
                        size_t(s.luma_width) * s.bytes_per_sample);
        for (uint32_t y = 0; y < s.chroma_height; y++)
            std::memcpy(chroma->data() + size_t(y) * s.chroma_width * 2 * s.bytes_per_sample,
                        s.readback_mapped + s.chroma_offset + size_t(y) * s.pitch_chroma,
                        size_t(s.chroma_width) * 2 * s.bytes_per_sample);
    }
    Release(ref);
    return ok;
}

}  // namespace renderer::clips
