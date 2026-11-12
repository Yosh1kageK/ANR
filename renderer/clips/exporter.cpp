#include "renderer/clips/exporter.h"

#include "renderer/clips/encoders.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <imgui.h>
#include "renderer/clips/camera.h"
#include "renderer/clips/composer.h"
#include "renderer/clips/edit_list.h"
#include "renderer/clips/ffmpeg_source.h"
#include "renderer/clips/logo_layer.h"
#include "renderer/clips/overlay.h"
#include "renderer/clips/player.h"
#include "renderer/clips/probe.h"
#include "renderer/clips/process.h"
#include "renderer/clips/video_pass.h"
#include "renderer/display.h"
#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace fs = std::filesystem;

namespace {

constexpr int SLOTS = 3;  // frames in flight between the GPU and the encoder

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Seconds6(double value) {
    char text[48];
    std::snprintf(text, sizeof(text), "%.6f", value);
    return text;
}

}  // namespace

// The audio of an export with freeze marks: the source audio cut where each freeze begins (the end of its frame's
// interval), silence of the sequence's length inserted there, and the whole padded to the video's length, so the audio
// is exactly as long as the video and each sound stays with its frame. `source_seconds` is the length of the audio
// input (the exported range), `total_seconds` the video's. The graph's output is labelled [aout]; the input is ffmpeg's
// second one.
static std::string AudioWithSilenceGraph(const EditList& edit, int64_t first, double fps, double source_seconds, double total_seconds) {
    struct Cut {
        double at, silence;
    };
    std::vector<Cut> cuts;
    for (const FreezeSequence& f : edit.freezes()) {
        const double at = (double)(f.frame - first + 1) / fps, silence = (double)f.length() / fps;
        if (!cuts.empty() && std::fabs(cuts.back().at - at) < 1e-9)
            cuts.back().silence += silence;  // marks at one frame: one stretch of silence
        else
            cuts.push_back({at, silence});
    }
    // Audio pieces [0, c0), [c0, c1), ... [c_last, end); the last is left out when it is empty (a mark on the last frame).
    const size_t pieces = cuts.size() + 1;
    const bool last_empty = cuts.back().at >= source_seconds - 1e-6;
    std::string graph = "[1:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,asplit=" + std::to_string(pieces);
    for (size_t i = 0; i < pieces; i++)
        graph += "[x" + std::to_string(i) + "]";
    graph += ";";
    std::string order;
    size_t used = 0;
    for (size_t i = 0; i < pieces; i++) {
        if (i + 1 == pieces && last_empty)
            break;
        const std::string start = Seconds6(i == 0 ? 0.0 : cuts[i - 1].at);
        graph += "[x" + std::to_string(i) + "]atrim=start=" + start;
        if (i < cuts.size())
            graph += ":end=" + Seconds6(cuts[i].at);
        graph += ",asetpts=PTS-STARTPTS[p" + std::to_string(i) + "];";
        order += "[p" + std::to_string(i) + "]";
        used++;
        if (i < cuts.size()) {
            graph += "anullsrc=r=48000:cl=stereo,atrim=end=" + Seconds6(cuts[i].silence) + ",asetpts=PTS-STARTPTS[s" +
                     std::to_string(i) + "];";
            order += "[s" + std::to_string(i) + "]";
            used++;
        }
    }
    if (last_empty) {  // the unused split output must still end somewhere
        graph += "[x" + std::to_string(pieces - 1) + "]anullsink;";
    }
    graph += order + "concat=n=" + std::to_string(used) + ":v=0:a=1,apad=whole_dur=" + Seconds6(total_seconds) + "[aout]";
    return graph;
}

struct Exporter::Impl {
    struct Slot {  // the composer's slot of the same number holds the pixels
        std::shared_ptr<Pair> pair;
        bool busy = false;
    };

    explicit Impl(Gfx& g) : gfx(g) {}
    ~Impl();

    bool Open(const Project& project, const ExportSettings& settings, int64_t& total, std::string& error);
    bool Start(std::string& error);
    void Finish();
    void WriterLoop();
    void SetError(const std::string& message);

    Gfx& gfx;
    Project project;
    ExportSettings settings;
    std::string encoder_name, rate_text;  // what runs, for the log and the headless JSON line
    std::unique_ptr<VideoSource> source[2];
    ProbeResult probe[2];
    VideoSourceParams params[2];
    Timeline timeline;
    double fps = 60.0;
    int64_t first = 0, end = 0;  // the pairs exported: A frames [first, end)
    int64_t next = 0;            // the next output frame
    int64_t outputs = 0;         // output frames: the pairs plus what the freeze marks insert
    uint32_t ow = 0, oh = 0;
    Layout layout = Layout::slider;
    std::unique_ptr<EditList> edit;
    LogoLayer logo_layer;
    std::vector<IntRect> pills;

    OffscreenComposer composer;  // the target, the overlay's draw path and the readback (composer.h, shared with the stills)
    std::vector<Slot> slots;
    VideoPass pass;
    int next_slot = 0;
    // A prepared frame
    bool prepared = false;
    int prepared_slot = -1;
    Shot prepared_shot;
    Camera prepared_camera;
    std::shared_ptr<Pair> prepared_pair;

    Process encoder;
    std::string log_path;
    std::thread writer;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<int> queue;
    bool stop = false;
    bool writer_started = false;
    std::string error_text, status_text;
    std::atomic<int64_t>* done = nullptr;
    std::atomic<bool>* cancelled = nullptr;
    double started_at = 0.0;
    bool encoder_closed = false;
    bool finished = false;
};

Exporter::Impl::~Impl() {
    {
        std::lock_guard<std::mutex> lock(mu);
        stop = true;
    }
    cv.notify_all();
    if (writer.joinable())
        writer.join();
    encoder.Kill();
    if (gfx.NRI.HasCore() && gfx.device) {
        gfx.NRI.DeviceWaitIdle(gfx.device);
        for (Slot& s : slots)
            s.pair.reset();
        prepared_pair.reset();
        composer.Destroy();
        pass.Destroy();
    }
    source[0].reset();
    source[1].reset();
}

void Exporter::Impl::SetError(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!error_text.empty())
            return;
        error_text = message;
    }
    spdlog::error("export: {}", message);
}

Exporter::Exporter() = default;

Exporter::~Exporter() {
    if (impl_ && impl_->writer_started && !impl_->finished) {
        cancelled_ = true;
        impl_->encoder.Terminate();
        impl_->Finish();
    }
}

std::string Exporter::status() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->status_text;
}

std::string Exporter::encoder() const {
    return impl_ ? impl_->encoder_name : std::string();
}

std::string Exporter::rate() const {
    return impl_ ? impl_->rate_text : std::string();
}

std::string Exporter::error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error_text;
}

void Exporter::Cancel() {
    if (!running_ || finished_)
        return;
    cancelled_ = true;
    running_ = false;
    impl_->encoder.Terminate();
    impl_->Finish();  // waits for the frames in flight, removes the partial file
    finished_ = true;
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->status_text = "cancelled";
}

std::unique_ptr<Exporter> Exporter::Start(Gfx& gfx, const Project& project, const ExportSettings& settings,
                                          std::string& error) {
    std::unique_ptr<Exporter> exporter(new Exporter());
    exporter->impl_ = std::make_unique<Impl>(gfx);
    Impl& m = *exporter->impl_;
    m.done = &exporter->done_;
    m.cancelled = &exporter->cancelled_;
    if (!m.Open(project, settings, exporter->total_, error))
        return nullptr;
    if (!m.Start(error))
        return nullptr;
    exporter->running_ = true;
    return exporter;
}

bool Exporter::Impl::Open(const Project& p, const ExportSettings& s, int64_t& total, std::string& error) {
    project = p;
    settings = s;
    if (settings.output.empty()) {
        error = "no output file";
        return false;
    }
    const VideoContext context = {gfx.device, &gfx.NRI, &gfx.NRI, gfx.queue};
    const uint64_t memory = (uint64_t)(settings.cache_mib ? settings.cache_mib : 768) << 20;
    const std::string paths[2] = {project.ResolvedA(), project.ResolvedB()};
    for (int v = 0; v < 2; v++) {
        // The GPU decoder first, as playback's (ffmpeg's software decode of two 4K streams took over 20 cores and
        // slowed the window's frames, which drive its export); else ffmpeg.
        std::string reason;
        source[v] = OpenVideo(context, paths[v], &reason, memory);
        if (!source[v]) {
            error = "cannot open " + paths[v] + ": " + reason;
            return false;
        }
        spdlog::info("export: {} decodes with {}{}{}", v == 0 ? "A" : "B", source[v]->Describe(),
                     reason.empty() ? "" : " (no GPU decode: ", reason.empty() ? "" : reason + ")");
        std::string probe_error;
        if (!ProbeVideo(paths[v], probe[v], probe_error)) {
            error = probe_error;
            return false;
        }
        params[v].width = source[v]->Info().width;
        params[v].height = source[v]->Info().height;
        params[v].yuv = MakeYuvParams(source[v]->Info());
    }
    const VideoInfo& a = source[0]->Info();
    const VideoInfo& b = source[1]->Info();
    if (a.width != b.width || a.height != b.height || std::fabs(a.fps - b.fps) > 1e-3) {
        error = "the recordings differ in size or frame rate; export needs them equal";
        return false;
    }
    fps = a.fps;
    timeline = Timeline(a.frames, b.frames, project.Offsets(), fps);  // K(n) as the playback has it
    if (!timeline.has_overlap()) {
        error = "the recordings do not overlap at this offset";
        return false;
    }
    first = timeline.overlap_first();
    end = timeline.overlap_end();
    if (settings.image_at >= 0.0) {  // a still image: the one pair at that time
        first = std::clamp((int64_t)std::llround(settings.image_at * fps), timeline.overlap_first(),
                           timeline.overlap_end() - 1);
        end = first + 1;
    } else if (settings.start >= 0.0)
        first = std::max(first, (int64_t)std::ceil(settings.start * fps - 1e-6));
    if (settings.end >= 0.0 && settings.image_at < 0.0)
        end = std::min(end, (int64_t)std::floor(settings.end * fps + 1e-6));
    if (end <= first) {
        error = "the recordings do not overlap in that range";
        return false;
    }
    next = 0;
    // Size as video_compare.py does: the height asked, the width from the aspect ratio, both even.
    oh = settings.height > 0 ? (uint32_t)settings.height : a.height;
    ow = (uint32_t)(std::lround((double)oh * a.width / a.height / 2.0) * 2);
    oh -= oh % 2;
    const std::string wanted = settings.layout.empty() ? project.layout : settings.layout;
    layout = wanted == "side-by-side" ? Layout::side_by_side : Layout::slider;
    if (settings.image_at >= 0.0 && layout == Layout::side_by_side)
        ow *= 2;  // each recording whole in its half
    // What each output frame shows: the pairs of the range with the freeze marks' sequences inserted.
    EditOptions edit_options;
    edit_options.fps = fps;
    edit_options.first = first;
    edit_options.end = end;
    edit_options.ease = settings.ease >= 0.0 ? settings.ease : project.ease;
    edit_options.freeze_hold = settings.freeze_hold >= 0.0 ? settings.freeze_hold : project.freeze_hold;
    edit_options.divider = project.divider;
    edit = std::make_unique<EditList>(settings.image_at >= 0.0 ? std::vector<Area>() : project.areas, edit_options);
    for (const EditList::Skipped& skipped : edit->skipped())
        spdlog::info("export: mark {} left out: {}", skipped.area_id, skipped.why);
    total = outputs = edit->length();
    return true;
}

bool Exporter::Impl::Start(std::string& error) {
    // The target, the overlay's draw path and the readback slots (the composer), and the video pass
    if (!composer.Create(gfx, ow, oh, nri::Format::RGBA8_UNORM, SLOTS, error))
        return false;
    slots.resize(SLOTS);
    if (!pass.Create(gfx, SLOTS, error))
        return false;

    // The logos: as the project says; unset, they are on when their files exist. A missing file is logged once and left out.
    {
        const std::vector<LogoItem> items = project.logos.effective();
        const std::vector<std::string> folders = logo_folders(fs::path(project.file).parent_path().string());
        bool on = false;
        if (settings.logos >= 0) {
            on = settings.logos == 1;
        } else if (project.logos.enabled) {
            on = *project.logos.enabled;
        } else {
            for (const LogoItem& item : items)
                on = on || !find_logo_file(item.path, folders).empty();
            if (!on) {
                std::string where;
                for (const std::string& f : folders)
                    where += (where.empty() ? "" : ", ") + f;
                spdlog::info("export: logos off: no logo file found (looked in {})", where);
            }
        }
        if (on) {
            logo_layer.Set(items, folders, (int)ow, (int)oh);
            spdlog::info("export: {} of {} logos on", logo_layer.count(), items.size());
        }
    }

    // The encoder: a still image's PNG, else the video's
    char size[32], rate[48];
    std::snprintf(size, sizeof(size), "%ux%u", ow, oh);
    std::snprintf(rate, sizeof(rate), "%.6f", fps);
    std::vector<std::string> args;
    if (settings.image_at >= 0.0) {
        args = {FindTool("ffmpeg", "NR_FFMPEG"), "-nostdin", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba",
                "-s", size, "-i", "-", "-frames:v", "1", "-update", "1", "-pix_fmt", "rgb24", settings.output};
    } else {
    bool audio = settings.audio != 0;
    const bool silence = !edit->freezes().empty();  // the marks' sequences need silence in the audio
    if (audio && silence && !probe[settings.audio == 1 ? 0 : 1].has_audio) {
        spdlog::info("export: {} has no audio: the video has none", settings.audio == 1 ? "A" : "B");
        audio = false;
    }
    args = {FindTool("ffmpeg", "NR_FFMPEG"), "-nostdin", "-v", "error", "-y", "-f", "rawvideo",
            "-pix_fmt", "rgba", "-s", size, "-framerate", rate, "-i", "-"};
    if (audio) {
        // B's sound starts at B's frame of the first pair and then runs on B's own clock (it does not follow the drift)
        const int64_t audio_first = settings.audio == 1 ? first : timeline.b_frame(first);
        char start[48], length[48];
        std::snprintf(start, sizeof(start), "%.4f", (double)audio_first / fps);
        std::snprintf(length, sizeof(length), "%.4f", (double)(end - first) / fps);
        args.insert(args.end(), {"-ss", start, "-t", length, "-i", settings.audio == 1 ? project.ResolvedA() : project.ResolvedB()});
    }
    args.insert(args.end(), {"-map", "0:v"});
    if (audio && silence) {
        // The sound stays with its frame: cut at each freeze, silence for its sequence, padded to the video's length.
        args.insert(args.end(), {"-filter_complex",
                                 AudioWithSilenceGraph(*edit, first, fps, (double)(end - first) / fps, (double)outputs / fps),
                                 "-map", "[aout]", "-c:a", "aac", "-b:a", "192k"});
    } else if (audio) {
        args.insert(args.end(), {"-map", "1:a?", "-c:a", "aac", "-b:a", "192k"});
    }
    RateControl rate_control;
    rate_control.mode = (RateControl::Mode)std::clamp(settings.rate, 0, 3);
    rate_control.crf = settings.crf;
    rate_control.mbps = settings.mbps;
    if (rate_control.mode == RateControl::VERY_HIGH)
        rate_control.mbps = VeryHighMbps(settings.codec, ow, oh, fps);
    if (rate_control.mode == RateControl::BITRATE && rate_control.mbps <= 0.0) {
        error = "the bitrate must be above 0 Mbit/s";
        return false;
    }
    if (rate_control.mode == RateControl::LOSSLESS && settings.codec != 0) {
        error = "lossless is offered for H.265 only";
        return false;
    }
    const VideoEncoder encoder_choice = ChooseEncoder(settings.codec, rate_control, settings.hardware != 0, args[0]);
    encoder_name = encoder_choice.name + (encoder_choice.hardware ? " (hardware)" : " (software)");
    rate_text = Describe(rate_control);
    spdlog::info("export: encoder {}, {}{}", encoder_name, rate_text, encoder_choice.note.empty() ? "" : ": " + encoder_choice.note);
    args.insert(args.begin() + 5, encoder_choice.global_args.begin(), encoder_choice.global_args.end());  // before the input
    args.insert(args.end(), {"-vf", "scale=out_color_matrix=bt709:out_range=tv" +
                                        (encoder_choice.filter.empty() ? std::string() : "," + encoder_choice.filter)});
    args.insert(args.end(), encoder_choice.codec_args.begin(), encoder_choice.codec_args.end());
    if (encoder_choice.set_pixel_format)
        args.insert(args.end(), {"-pix_fmt", "yuv420p"});
    args.insert(args.end(), {"-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-movflags", "+faststart",
                             settings.output});
    }
    std::error_code ec;
    if (!fs::path(settings.output).parent_path().empty())
        fs::create_directories(fs::path(settings.output).parent_path(), ec);
    log_path = settings.output + ".ffmpeg.log";
    ProcessOptions options;
    options.pipe_stdin = true;
    options.stderr_path = log_path;
    std::string spawn_error;
    if (!encoder.Start(args, options, &spawn_error))
        return error = spawn_error + " (is ffmpeg installed? set NR_FFMPEG or put it on PATH)", false;
    started_at = Now();
    writer_started = true;
    writer = std::thread([this] { WriterLoop(); });
    if (settings.image_at >= 0.0)
        spdlog::info("export: image of A frame {} (B frame {}) at {}x{}, {} -> {}", first, timeline.b_frame(first), ow,
                     oh, layout == Layout::slider ? "divider" : "side by side", settings.output);
    else
        spdlog::info("export: {} frames ({:.1f} s; {} of them from {} freeze marks) from A frame {} (B frame {}) at {}x{}, {} -> {}",
                     outputs, (double)outputs / fps, outputs - (end - first), edit->freezes().size(), first,
                     timeline.b_frame(first), ow, oh, layout == Layout::slider ? "divider" : "side by side", settings.output);
    {
        std::lock_guard<std::mutex> lock(mu);
        status_text = "starting";
    }
    return true;
}

// Pipes the frames to the encoder in order as the GPU finishes them.
void Exporter::Impl::WriterLoop() {
    std::vector<uint8_t> packed;
    for (;;) {
        int index = -1;
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [&] { return stop || !queue.empty(); });
            if (queue.empty())
                return;  // stop with nothing left
            index = queue.front();
            queue.pop_front();
        }
        Slot& slot = slots[(size_t)index];
        composer.Wait((uint32_t)index);
        const uint8_t* pixels = composer.Pixels((uint32_t)index);
        const uint32_t row_pitch = composer.row_pitch();
        bool ok = true;
        if (!cancelled->load()) {
            if (row_pitch == ow * 4u) {
                ok = encoder.Write(pixels, (size_t)row_pitch * oh);
            } else {
                packed.resize((size_t)ow * 4 * oh);
                for (uint32_t y = 0; y < oh; y++)
                    std::memcpy(packed.data() + (size_t)y * ow * 4, pixels + (size_t)y * row_pitch, (size_t)ow * 4);
                ok = encoder.Write(packed.data(), packed.size());
            }
        }
        slot.pair.reset();  // the frames are free again
        if (!ok && !cancelled->load())
            SetError("the encoder stopped (see " + log_path + ")");
        {
            std::lock_guard<std::mutex> lock(mu);
            slot.busy = false;
        }
        const int64_t n = ++*done;
        if (n % 30 == 0 || n == outputs) {
            const double elapsed = Now() - started_at;
            char text[160];
            std::snprintf(text, sizeof(text), "frame %lld of %lld (%.1f fps, %.0f s left)", (long long)n, (long long)outputs,
                          (double)n / std::max(elapsed, 1e-6),
                          (double)(outputs - n) / std::max((double)n / std::max(elapsed, 1e-6), 1e-6));
            std::lock_guard<std::mutex> lock(mu);
            status_text = text;
        }
        cv.notify_all();
    }
}

void Exporter::Impl::Finish() {
    if (finished)
        return;
    finished = true;
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return queue.empty() && std::none_of(slots.begin(), slots.end(), [](const Slot& s) { return s.busy; }); });
    }
    encoder.CloseStdin();
    const int code = encoder.Wait();
    if (code != 0 && !cancelled->load()) {
        std::ifstream log(log_path);
        std::string line, last;
        while (std::getline(log, line))
            if (!line.empty())
                last = line;
        SetError("ffmpeg's encoder failed (exit " + std::to_string(code) + "): " + last);
    } else if (!cancelled->load()) {
        std::error_code ec;
        fs::remove(log_path, ec);
    }
    if (cancelled->load() || !error_text.empty()) {
        std::error_code ec;
        fs::remove(settings.output, ec);
    }
}

void Exporter::Prepare() {
    Impl& m = *impl_;
    if (!running_ || m.prepared)
        return;
    if (!m.error_text.empty()) {
        running_ = false;
        m.Finish();
        finished_ = true;
        return;
    }
    if (m.next >= m.outputs) {
        // everything is rendered: wait for the writer, then close the encoder
        if (done_ >= total_) {
            m.Finish();
            running_ = false;
            finished_ = true;
            std::lock_guard<std::mutex> lock(m.mu);
            m.status_text = m.error_text.empty() ? "finished" : m.status_text;
        }
        return;
    }
    // What this output frame shows: a pair of the range (the same one again for the frames a freeze mark inserts).
    const EditEntry entry = m.edit->at(m.next);
    const int64_t a_frame = entry.pair;
    // Decode interest: ahead of the pair to render.
    const int64_t b = m.timeline.b_frame(a_frame);  // B's frame of the pair: the playback's own pairing (Timeline::b_frame)
    for (int v = 0; v < 2; v++) {
        int64_t head_frame = v == 0 ? a_frame : b;
        int64_t first = head_frame - 4, last = head_frame + 40;
        if (v == 1 && !m.timeline.offsets().is_constant()) {  // B's frames of the next pairs: they follow K (jumps too)
            const Timeline::FrameRange r = m.timeline.b_range(a_frame - 4, a_frame, a_frame + 40, 120, -1);
            if (r.last >= r.first) {
                first = r.first;
                last = r.last;
                head_frame = std::clamp(r.head, first, last);
            }
        }
        m.source[v]->SetInterest(std::max<int64_t>(first, 0), std::min<int64_t>(last, m.source[v]->Info().frames - 1), head_frame, 1);
    }
    if (!m.source[0]->Ready(a_frame) || !m.source[1]->Ready(b))
        return;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(m.mu);
        if (!m.slots[(size_t)m.next_slot].busy)
            slot = m.next_slot;
    }
    if (slot < 0)
        return;
    auto pair = std::make_shared<Pair>();
    pair->n = a_frame;
    pair->source[0] = m.source[0].get();
    pair->source[1] = m.source[1].get();
    pair->has_a = m.source[0]->Acquire(a_frame, pair->a);
    pair->has_b = pair->has_a && m.source[1]->Acquire(b, pair->b);
    if (!pair->has_a || !pair->has_b)
        return;  // released by Pair's destructor; try again next frame
    m.prepared_pair = pair;
    m.prepared_slot = slot;
    m.prepared_shot = entry.shot;

    // The overlay: labels, divider line, caption and zoom, with the same code as the window.
    const Rect view = {0.0, 0.0, (double)m.ow, (double)m.oh};
    const CompareLayout areas = compare_layout(m.layout, view, m.prepared_shot.divider);
    OverlayInput in;
    in.layout = m.layout;
    in.view = view;
    in.areas = areas;
    in.video_w = (int)m.params[0].width;
    in.video_h = (int)m.params[0].height;
    for (int i = 0; i < 2; i++)
        in.mapping[i] = map_camera(areas.area[i], in.video_w, in.video_h, m.prepared_shot.camera,
                                   fit_scale(areas.area[i], in.video_w, in.video_h));
    m.prepared_camera = in.mapping[0].camera;
    in.label_a = m.project.label_a;
    in.label_b = m.project.label_b;
    in.divider_x = areas.split;
    in.caption = m.prepared_shot.caption;
    in.zoom = in.mapping[0].camera.z;
    in.size = (float)std::max(14.0, std::floor((double)m.oh / 42.0));
    in.line = (float)std::max(2.0, std::floor((double)m.oh / 720.0));
    ImDrawList* list = m.composer.BeginOverlay();
    m.pills.clear();
    DrawVideoOverlay(list, ImGui::GetFont(), in, &m.pills);
    m.logo_layer.Draw(list, ImVec2(0.0f, 0.0f), m.pills);  // after the overlay: the logos keep clear of its pills
    m.prepared = true;
}

void Exporter::Record() {
    Impl& m = *impl_;
    if (!m.prepared)
        return;
    m.prepared = false;
    const int index = m.prepared_slot;
    Impl::Slot& slot = m.slots[(size_t)index];
    // The pictures: the pair through the video pass, inside the composer's rendering
    m.composer.Record((uint32_t)index, [&](nri::CommandBuffer& cb) {
        const Rect view = {0.0, 0.0, (double)m.ow, (double)m.oh};
        const CompareLayout areas = compare_layout(m.layout, view, m.prepared_shot.divider);
        const Regions regions = compare_regions(areas, (int)m.params[0].width, (int)m.params[0].height, m.prepared_shot.camera);
        // The scaling: auto is PIL's choice in video_compare.py (box below a scale of 1, nearest from a zoom of 2, else bicubic)
        Filter filter = Filter::AUTO;
        switch (m.settings.resample) {
            case 1: filter = Filter::NEAREST; break;
            case 2: filter = Filter::BILINEAR; break;
            case 3: filter = Filter::BICUBIC; break;
            case 4: filter = Filter::BOX; break;
            default:
                filter = regions.region[0].scale < 1.0 - 1e-9 ? Filter::BOX
                                                             : (m.prepared_camera.z >= 2.0 - 1e-9 ? Filter::NEAREST : Filter::BICUBIC);
        }
        VideoSourceParams params[2] = {m.params[0], m.params[1]};
        params[0].exists = params[1].exists = true;
        PassTarget target;
        target.encoding = SRGB8;
        target.code_encoding = SRGB8;
        target.paper_white = 203.0f;
        target.hw_srgb = false;
        nri::Descriptor* luma[2] = {m.prepared_pair->a.luma, m.prepared_pair->b.luma};
        nri::Descriptor* chroma[2] = {m.prepared_pair->a.chroma, m.prepared_pair->b.chroma};
        m.pass.RecordVideo(cb, (uint32_t)index, nri::Format::RGBA8_UNORM, MakeVideoConstants(regions, params, filter, target), luma, chroma);
    });
    slot.pair = m.prepared_pair;
    m.prepared_pair.reset();
    {
        std::lock_guard<std::mutex> lock(m.mu);
        slot.busy = true;
        m.queue.push_back(index);
    }
    m.cv.notify_all();
    m.next++;
    m.next_slot = (m.next_slot + 1) % SLOTS;
}

bool Exporter::Run() {
    Impl& m = *impl_;
    const double start = Now();
    ImGuiIO& io = ImGui::GetIO();
    while (running_) {
        io.DisplaySize = ImVec2((float)m.ow, (float)m.oh);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        Prepare();
        ImGui::EndFrame();
        ImGui::Render();
        const bool had_frame = m.prepared;
        Record();
        if (!had_frame)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!m.error_text.empty() || cancelled_)
            break;
    }
    if (!m.finished)
        m.Finish();
    finished_ = true;
    running_ = false;
    const std::string failure = error();
    if (!failure.empty()) {
        spdlog::error("export failed: {}", failure);
        return false;
    }
    std::error_code ec;
    const uintmax_t bytes = fs::file_size(m.settings.output, ec);
    spdlog::info("export: wrote {} ({:.1f} MB) in {:.1f} s", m.settings.output, (double)bytes / 1e6, Now() - start);
    return true;
}

}  // namespace renderer::clips
