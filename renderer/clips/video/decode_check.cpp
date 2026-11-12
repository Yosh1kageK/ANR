// Checks the GPU video decoder on a file, headless (no window): decodes on the GPU's video decode queue through
// NriVideoSource, reads the planes back and compares them byte for byte with ffmpeg's decode of the same frames
// (-pix_fmt nv12, or p010le at 10 bits), measures the decode rate and CPU use, and times random seeks (decode from
// the previous keyframe), each also compared with ffmpeg. Prints one JSON line.
//
//   bazel run //renderer/clips:decode_check -- FILE [N] [--seeks K] [--playback S] [--memory-mb M] [--throughput T]
//                                               [--twin] [--stress S] [--ffmpeg PATH]
//
// N: frames compared from the start (default 120; 0: all). --twin opens the file a second time and keeps that source
// decoding at another playhead during the comparison, as the clip comparison's two recordings do, then closes it.
// --stress S moves the playhead at random for S seconds (seeks, direction changes, steps) while holding up to three
// frames, and checks that a held frame's pixels never change.
// --playback S plays S seconds forward and S seconds in
// reverse at the file's frame rate through the interest interface, and counts the frames that were not ready when due
// (JSON "playback"). Exit code: 0 all frames equal, 1 a difference or a decode
// error, 2 usage, 3 the file cannot be decoded on this GPU (the JSON has the reason).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include <NRI.h>
#include <Extensions/NRIHelper.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>
#include "renderer/clips/video/mp4.h"
#include "renderer/clips/video/nri_video_source.h"
#include "device/vulkan.h"
#include "device/log.h"

using renderer::clips::FrameRef;
using renderer::clips::NriVideoOptions;
using renderer::clips::NriVideoSource;
using nlohmann::json;

namespace {

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double ProcessCpuSeconds() {
#ifdef _WIN32
    FILETIME create, exit_time, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &create, &exit_time, &kernel, &user);
    auto seconds = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return seconds(kernel) + seconds(user);
#else
    rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return double(usage.ru_utime.tv_sec) + double(usage.ru_stime.tv_sec) + 1e-6 * double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
#endif
}

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

std::string Quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
#endif
}

// ffmpeg's decode of a file as raw NV12 (P010 at 10 bits) frames in display order.
class Reference {
public:
    Reference(const std::string& ffmpeg, const std::string& file, uint64_t frame_bytes, bool ten_bit, int64_t frames,
              double seek_seconds = -1) {
        std::string command = Quote(ffmpeg) + " -v error -nostdin ";
        if (seek_seconds >= 0) {
            char text[64];
            std::snprintf(text, sizeof(text), "-ss %.6f ", seek_seconds);
            command += text;
        }
        command += "-i " + Quote(file) + " -an -sn -fps_mode passthrough ";
        if (frames > 0)
            command += "-frames:v " + std::to_string(frames) + " ";
        command += std::string("-pix_fmt ") + (ten_bit ? "p010le" : "nv12") + " -f rawvideo -";
        pipe_ = POPEN(command.c_str(), "r");
        frame_bytes_ = frame_bytes;
    }
    ~Reference() {
        if (pipe_)
            PCLOSE(pipe_);
    }
    bool Next(std::vector<uint8_t>* frame) {
        frame->resize(frame_bytes_);
        return pipe_ && std::fread(frame->data(), 1, frame_bytes_, pipe_) == frame_bytes_;
    }

private:
    FILE* pipe_ = nullptr;
    uint64_t frame_bytes_ = 0;
};

struct Diff {
    bool equal = true;
    const char* plane = "";
    uint64_t first_offset = 0;
    uint64_t differing = 0;
    int max_abs = 0;
};

Diff Compare(const std::vector<uint8_t>& luma, const std::vector<uint8_t>& chroma, const std::vector<uint8_t>& reference,
             bool ten_bit) {
    Diff d;
    auto compare_plane = [&](const char* name, const uint8_t* a, const uint8_t* b, size_t n) {
        if (std::memcmp(a, b, n) == 0)
            return;
        for (size_t i = 0; i < n; i++)
            if (a[i] != b[i]) {
                if (d.equal) {
                    d.equal = false;
                    d.plane = name;
                    d.first_offset = i;
                }
                d.differing++;
                d.max_abs = std::max(d.max_abs, std::abs(int(a[i]) - int(b[i])));
            }
    };
    (void)ten_bit;
    compare_plane("luma", luma.data(), reference.data(), luma.size());
    compare_plane("chroma", chroma.data(), reference.data() + luma.size(), chroma.size());
    return d;
}

bool WaitReady(NriVideoSource& source, int64_t frame, double timeout) {
    const double start = Now();
    while (!source.Ready(frame)) {
        if (!source.Error().empty() || Now() - start > timeout)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string file, ffmpeg = "ffmpeg";
    int64_t compare_frames = 120, seeks = 0, throughput_frames = 300;
    double playback_seconds = 0;
    bool twin_wanted = false;
    double stress_seconds = 0;
    uint64_t memory_mb = 1536;
    bool have_n = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--seeks" && i + 1 < argc)
            seeks = std::atoll(argv[++i]);
        else if (a == "--twin")
            twin_wanted = true;
        else if (a == "--stress" && i + 1 < argc)
            stress_seconds = std::atof(argv[++i]);
        else if (a == "--playback" && i + 1 < argc)
            playback_seconds = std::atof(argv[++i]);
        else if (a == "--memory-mb" && i + 1 < argc)
            memory_mb = uint64_t(std::atoll(argv[++i]));
        else if (a == "--throughput" && i + 1 < argc)
            throughput_frames = std::atoll(argv[++i]);
        else if (a == "--ffmpeg" && i + 1 < argc)
            ffmpeg = argv[++i];
        else if (file.empty())
            file = a;
        else if (!have_n) {
            compare_frames = std::atoll(argv[i]);
            have_n = true;
        } else {
            std::fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (file.empty()) {
        std::fprintf(stderr,
                     "usage: decode_check FILE [N] [--seeks K] [--playback S] [--memory-mb M] [--throughput T] [--twin] [--stress S] [--ffmpeg PATH]\n");
        return 2;
    }

    // stdout carries the JSON line only: the NRI and decoder messages go to stderr.
    spdlog::set_default_logger(spdlog::stderr_logger_mt("decode_check"));

    json out;
    out["file"] = file;

    // The renderer's device creation (device/vulkan.h), without a window, with a video decode queue.
    nr::VulkanDevice vk;
    vk.video_decode = true;
    std::string error;
    nri::VKBindingOffsets offsets = {};
    if (!vk.Create(false, {}, {}, offsets, error)) {
        out["supported"] = false;
        out["why"] = "no Vulkan device: " + error;
        std::printf("%s\n", out.dump().c_str());
        return 3;
    }
    nri::CoreInterface core = {};
    nri::HelperInterface helper = {};
    nri::nriGetInterface(*vk.nri, NRI_INTERFACE(nri::CoreInterface), &core);
    nri::nriGetInterface(*vk.nri, NRI_INTERFACE(nri::HelperInterface), &helper);
    nri::Queue* graphics_queue = nullptr;
    core.GetQueue(*vk.nri, nri::QueueType::GRAPHICS, 0, graphics_queue);
    out["device"] = vk.caps.device_name;
    out["driver"] = vk.caps.DriverVersion();
    out["video_decode_queue_family"] = vk.video_decode_queue_family == UINT32_MAX ? json(nullptr) : json(vk.video_decode_queue_family);

    auto memory_usage = [&] {
        nri::VideoMemoryInfo info = {};
        if (helper.QueryVideoMemoryInfo && helper.QueryVideoMemoryInfo(*vk.nri, nri::MemoryLocation::DEVICE, info) == nri::Result::SUCCESS)
            return info.usageSize;
        return uint64_t(0);
    };
    const uint64_t memory_before = memory_usage();

    renderer::clips::VideoContext context;
    context.device = vk.nri;
    context.core = &core;
    context.helper = &helper;
    context.graphics_queue = graphics_queue;
    NriVideoOptions options;
    options.memory_bytes = memory_mb << 20;
    std::string why;
    const double open_start = Now();
    std::unique_ptr<NriVideoSource> source = NriVideoSource::OpenWithOptions(context, file, options, &why);
    out["open_ms"] = (Now() - open_start) * 1000.0;
    if (!source) {
        out["supported"] = false;
        out["why"] = why;
        std::printf("%s\n", out.dump().c_str());
        vk.Destroy();
        return 3;
    }
    const renderer::clips::VideoInfo info = source->Info();
    out["supported"] = true;
    out["decoder"] = source->Describe();
    out["codec"] = info.codec;
    out["width"] = info.width;
    out["height"] = info.height;
    out["bit_depth"] = info.bit_depth;
    out["fps"] = info.fps;
    out["frames"] = info.frames;
    out["keyframe_gap"] = info.keyframe_gap;
    out["pool_frames"] = source->pool_capacity();
    out["frame_mib"] = double(source->frame_bytes()) / (1 << 20);
    std::unique_ptr<NriVideoSource> twin;
    if (twin_wanted) {
        std::string twin_why;
        twin = NriVideoSource::OpenWithOptions(context, file, options, &twin_why);
        out["twin"] = twin != nullptr;
        if (!twin)
            out["twin_why"] = twin_why;
    }
    int64_t twin_frames = 0;
    const bool ten_bit = info.bit_depth == 10;
    const uint64_t cw = (info.width + 1) / 2, ch = (info.height + 1) / 2;
    const uint64_t frame_bytes = (uint64_t(info.width) * info.height + cw * ch * 2) * (ten_bit ? 2 : 1);

    int exit_code = 0;

    // 1. Frames from the start against ffmpeg.
    const int64_t n = compare_frames <= 0 ? info.frames : std::min<int64_t>(compare_frames, info.frames);
    {
        Reference reference(ffmpeg, file, frame_bytes, ten_bit, n);
        int64_t mismatched = 0, compared = 0;
        json first_mismatch = nullptr;
        std::vector<uint8_t> luma, chroma, expected;
        for (int64_t f = 0; f < n; f++) {
            source->SetInterest(0, info.frames - 1, f, 1);
            if (twin) {  // the second recording, at its own playhead
                const int64_t g = (f + info.frames / 2) % info.frames;
                twin->SetInterest(0, info.frames - 1, g, 1);
                FrameRef twin_ref;
                if (twin->Acquire(g, twin_ref)) {
                    twin->Release(twin_ref);
                    twin_frames++;
                }
            }
            if (!WaitReady(*source, f, 60.0)) {
                out["error"] = "frame " + std::to_string(f) + " did not decode: " + (source->Error().empty() ? "timeout" : source->Error());
                exit_code = 1;
                break;
            }
            if (!source->ReadFrame(f, &luma, &chroma)) {
                out["error"] = "cannot read back frame " + std::to_string(f);
                exit_code = 1;
                break;
            }
            if (!reference.Next(&expected)) {
                out["error"] = "ffmpeg produced fewer than " + std::to_string(n) + " frames";
                exit_code = 1;
                break;
            }
            const Diff d = Compare(luma, chroma, expected, ten_bit);
            compared++;
            if (std::getenv("DECODE_CHECK_VERBOSE"))
                std::fprintf(stderr, "frame %lld: %s, %llu bytes differ, max %d\n", (long long)f, d.equal ? "equal" : d.plane,
                             (unsigned long long)d.differing, d.max_abs);
            if (!d.equal) {
                mismatched++;
                if (first_mismatch.is_null())
                    first_mismatch = {{"frame", f}, {"plane", d.plane}, {"first_byte", d.first_offset},
                                      {"differing_bytes", d.differing}, {"max_abs_diff", d.max_abs}};
            }
        }
        out["compared_frames"] = compared;
        out["mismatched_frames"] = mismatched;
        out["first_mismatch"] = first_mismatch;
        if (mismatched)
            exit_code = 1;
    }

    if (twin) {
        out["twin_frames_seen"] = twin_frames;
        // Seeks to the same distance after a keyframe: one source alone, then both sources at once. With a decode
        // queue (and hardware decoder) each, two seeks take about as long as one; on a shared one, twice as long.
        const int64_t gop = std::max<int64_t>(info.keyframe_gap, 30);
        const int64_t gops = std::max<int64_t>(info.frames / gop - 1, 1);
        std::vector<double> alone, together;
        std::mt19937_64 twin_rng(7);
        auto target = [&] { return std::min<int64_t>(int64_t(twin_rng() % uint64_t(gops)) * gop + gop * 4 / 5, info.frames - 1); };
        for (int k = 0; k < 4; k++) {
            int64_t a = target();
            double t0 = Now();
            source->SetInterest(a, std::min<int64_t>(a + 20, info.frames - 1), a, 1);
            WaitReady(*source, a, 60.0);
            alone.push_back((Now() - t0) * 1000.0);
            a = target();
            const int64_t b = target();
            t0 = Now();
            source->SetInterest(a, std::min<int64_t>(a + 20, info.frames - 1), a, 1);
            twin->SetInterest(b, std::min<int64_t>(b + 20, info.frames - 1), b, 1);
            WaitReady(*source, a, 60.0);
            WaitReady(*twin, b, 60.0);
            together.push_back((Now() - t0) * 1000.0);
        }
        std::sort(alone.begin(), alone.end());
        std::sort(together.begin(), together.end());
        out["twin_seek_ms_one_source_median"] = alone[alone.size() / 2];
        out["twin_seek_ms_both_sources_median"] = together[together.size() / 2];
        out["twin_error"] = twin->Error();
        twin.reset();  // closed while the first source goes on
    }

    // 2. Decode rate: frames from the middle of the file, consumed as soon as they are ready, with no readback.
    if (exit_code == 0 && throughput_frames > 0) {
        const int64_t span = std::min<int64_t>(throughput_frames, info.frames);
        // Start at a frame the cache does not hold: after the first compared window, a keyframe-aligned position.
        int64_t begin = info.frames > span + 2 * int64_t(source->pool_capacity()) ? info.frames / 2 : 0;
        begin = std::min<int64_t>(begin, info.frames - span);
        const double cpu0 = ProcessCpuSeconds();
        const double thread0 = source->DecodeThreadCpuSeconds();
        const double t0 = Now();
        int64_t done = 0;
        for (int64_t f = begin; f < begin + span; f++) {
            source->SetInterest(0, info.frames - 1, f, 1);
            if (!WaitReady(*source, f, 60.0)) {
                out["error"] = "throughput run: frame " + std::to_string(f) + " did not decode";
                exit_code = 1;
                break;
            }
            FrameRef ref;
            if (source->Acquire(f, ref))
                source->Release(ref);
            done++;
        }
        const double elapsed = Now() - t0;
        out["throughput_frames"] = done;
        out["decode_fps"] = elapsed > 0 ? double(done) / elapsed : 0.0;
        out["decode_ms_per_frame"] = done ? elapsed * 1000.0 / double(done) : 0.0;
        out["process_cpu_percent_of_one_core"] = elapsed > 0 ? 100.0 * (ProcessCpuSeconds() - cpu0) / elapsed : 0.0;
        out["decode_thread_cpu_percent_of_one_core"] = elapsed > 0 ? 100.0 * (source->DecodeThreadCpuSeconds() - thread0) / elapsed : 0.0;
        out["realtime_multiple"] = elapsed > 0 ? double(done) / elapsed / info.fps : 0.0;
    }

    // 3. Random seeks.
    if (exit_code == 0 && seeks > 0) {
        auto mp4 = renderer::clips::video::Mp4::Open(file, &why);
        std::mt19937_64 rng(20261109);
        json seek_list = json::array();
        std::vector<double> latencies;
        int64_t seek_mismatches = 0;
        for (int64_t k = 0; k < seeks; k++) {
            int64_t target = 0;
            for (int tries = 0; tries < 50; tries++) {
                target = int64_t(rng() % uint64_t(info.frames));
                if (!source->Ready(target))
                    break;
            }
            // Distance from the previous sync sample, in samples (equal to frames in decode order of a GOP).
            int64_t distance = -1;
            if (mp4) {
                const auto& track = mp4->track();
                int64_t sample = int64_t(track.display_to_sample[size_t(target)]);
                int64_t s = sample;
                while (s > 0 && !track.samples[size_t(s)].sync)
                    s--;
                distance = sample - s;
            }
            const double t0 = Now();
            source->SetInterest(target, std::min<int64_t>(target + 30, info.frames - 1), target, 1);
            const bool ready = WaitReady(*source, target, 60.0);
            const double ms = (Now() - t0) * 1000.0;
            json entry = {{"frame", target}, {"frames_from_keyframe", distance}, {"ms", ms}};
            if (!ready) {
                entry["error"] = source->Error().empty() ? "timeout" : source->Error();
                exit_code = 1;
            } else {
                std::vector<uint8_t> luma, chroma, expected;
                const double seek_seconds = std::max(0.0, (double(target) - 0.25) / info.fps);
                Reference reference(ffmpeg, file, frame_bytes, ten_bit, 1, seek_seconds);
                const bool read = source->ReadFrame(target, &luma, &chroma);
                const bool have = reference.Next(&expected);
                entry["exact"] = read && have && Compare(luma, chroma, expected, ten_bit).equal;
                if (!entry["exact"].get<bool>()) {
                    seek_mismatches++;
                    exit_code = 1;
                }
                latencies.push_back(ms);
            }
            seek_list.push_back(entry);
        }
        out["seeks"] = seek_list;
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            out["seek_ms_median"] = latencies[latencies.size() / 2];
            out["seek_ms_max"] = latencies.back();
        }
        out["seek_mismatches"] = seek_mismatches;
    }

    // 4. Stress: random seeks, direction changes and steps with frames held, as a viewer that pins what it shows.
    if (exit_code == 0 && stress_seconds > 0) {
        struct Held {
            int64_t frame;
            FrameRef ref;
            uint64_t hash;
            int age;
        };
        auto hash_frame = [&](int64_t frame, uint64_t* h) {
            std::vector<uint8_t> luma, chroma;
            if (!source->ReadFrame(frame, &luma, &chroma))
                return false;
            uint64_t x = 1469598103934665603ull;  // FNV-1a over a sample of the bytes
            for (size_t i = 0; i < luma.size(); i += 97)
                x = (x ^ luma[i]) * 1099511628211ull;
            for (size_t i = 0; i < chroma.size(); i += 89)
                x = (x ^ chroma[i]) * 1099511628211ull;
            *h = x;
            return true;
        };
        std::mt19937_64 stress_rng(99);
        std::vector<Held> held;
        int64_t moves = 0, checks = 0, violations = 0, acquired = 0, late = 0;
        int64_t playhead = info.frames / 2;
        int direction = 1;
        const double stress_end = Now() + stress_seconds;
        while (Now() < stress_end && violations == 0) {
            const uint64_t r = stress_rng() % 100;
            if (r < 8) {  // a seek
                playhead = int64_t(stress_rng() % uint64_t(info.frames));
            } else if (r < 20) {
                direction = int(stress_rng() % 3) - 1;
            } else {
                playhead = std::clamp<int64_t>(playhead + direction * int64_t(1 + stress_rng() % 3), 0, info.frames - 1);
            }
            moves++;
            const int64_t first = std::max<int64_t>(0, playhead - int64_t(stress_rng() % 200));
            const int64_t last = std::min<int64_t>(info.frames - 1, playhead + int64_t(stress_rng() % 400));
            source->SetInterest(first, std::max(first, last), playhead, direction);
            std::this_thread::sleep_for(std::chrono::microseconds(500 + stress_rng() % 8000));
            // Hold the frame shown, up to three.
            if (source->Ready(playhead) && held.size() < 3) {
                Held h;
                h.frame = playhead;
                h.age = 0;
                if (source->Acquire(playhead, h.ref) && hash_frame(playhead, &h.hash)) {
                    held.push_back(h);
                    acquired++;
                }
            } else if (!source->Ready(playhead)) {
                late++;
            }
            for (size_t i = 0; i < held.size();) {
                uint64_t now_hash = 0;
                checks++;
                if (!hash_frame(held[i].frame, &now_hash) || now_hash != held[i].hash) {
                    violations++;
                    break;
                }
                if (++held[i].age > 6) {
                    source->Release(held[i].ref);
                    held.erase(held.begin() + long(i));
                } else {
                    i++;
                }
            }
        }
        for (Held& h : held)
            source->Release(h.ref);
        out["stress"] = {{"moves", moves}, {"held_frames", acquired}, {"hold_checks", checks}, {"violations", violations},
                         {"not_ready_when_shown", late}, {"error", source->Error()}};
        if (violations || !source->Error().empty())
            exit_code = 1;
    }

    // 5. Playback at the frame rate: forward from a point inside a GOP, then in reverse, counting frames that were
    // not ready when due. A late frame stalls playback until it is ready, as the viewer's would.
    if (exit_code == 0 && playback_seconds > 0) {
        json playback = json::array();
        const int64_t span = int64_t(playback_seconds * info.fps);
        for (int direction : {1, -1}) {
            // Half a GOP after a keyframe, so the run crosses a keyframe (forward: the next one; in reverse: the one
            // below) with time for decode-ahead: reverse play from just after a keyframe has to wait for the whole GOP
            // before it.
            const int64_t gop = std::max<int64_t>(info.keyframe_gap, 30);
            const int64_t base = (info.frames * (direction > 0 ? 1 : 2) / 3 / gop) * gop + gop / 2;
            const int64_t start = std::clamp<int64_t>(direction > 0 ? base : base + gop, 0, info.frames - 1);
            source->SetInterest(0, info.frames - 1, start, direction);
            const double seek0 = Now();
            WaitReady(*source, start, 60.0);
            const double start_ms = (Now() - seek0) * 1000.0;
            int64_t played = 0, late = 0;
            double max_stall = 0, total_stall = 0;
            const double frame_time = 1.0 / info.fps;
            double schedule = Now();
            for (int64_t k = 0; k < span; k++) {
                const int64_t f = start + direction * k;
                if (f < 0 || f >= info.frames)
                    break;
                while (Now() < schedule + double(k) * frame_time)
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                source->SetInterest(0, info.frames - 1, f, direction);
                if (!source->Ready(f)) {
                    late++;
                    const double w0 = Now();
                    WaitReady(*source, f, 10.0);
                    const double stall = Now() - w0;
                    max_stall = std::max(max_stall, stall);
                    total_stall += stall;
                    schedule += stall;  // playback resumes from here
                }
                FrameRef ref;
                if (source->Acquire(f, ref))
                    source->Release(ref);
                played++;
            }
            playback.push_back({{"direction", direction}, {"start_frame", start}, {"start_ms", start_ms}, {"frames", played},
                                {"late_frames", late}, {"max_stall_ms", max_stall * 1000.0}, {"total_stall_ms", total_stall * 1000.0}});
            if (late)
                exit_code = std::max(exit_code, 0);
        }
        out["playback"] = playback;
    }

    const uint64_t memory_after = memory_usage();
    out["gpu_memory_mib"] = double(memory_after > memory_before ? memory_after - memory_before : 0) / (1 << 20);
    out["decoded_total"] = source->Stats().decoded_total;
    out["exact"] = exit_code == 0 && out.value("mismatched_frames", int64_t(0)) == 0;
    std::printf("%s\n", out.dump().c_str());
    source.reset();
    vk.Destroy();
    return exit_code;
}
