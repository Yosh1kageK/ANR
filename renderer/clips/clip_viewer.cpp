#include "renderer/clips/clip_viewer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>

#include <imgui.h>
#include "renderer/clips/exporter.h"
#include "renderer/clips/ffmpeg_source.h"
#include "renderer/dock_layout.h"
#include "renderer/display.h"
#include "renderer/paths.h"
#include "device/log.h"
#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace fs = std::filesystem;

ClipViewer::ClipViewer() {
    nr::InitLogging(/*to_stderr=*/false);
    m_Resizable = true;
    m_Vsync = true;
    m_AlwaysActive = true;
}

ClipViewer::~ClipViewer() {
    SaveIfDue(true);
    if (gfx_.NRI.HasCore()) {
        if (captures_)
            captures_->Shutdown();
        captures_.reset();
        sync_.Cancel();
        exporter_.reset();
        gfx_.NRI.DeviceWaitIdle(gfx_.device);
        for (QueuedFrame& q : queued_frames_)
            q.pair.reset();
        tick_ = Player::Tick();
        player_.reset();
        source_[0].reset();
        source_[1].reset();
        pass_.Destroy();
        swap_.Destroy();
        for (QueuedFrame& q : queued_frames_) {
            gfx_.NRI.DestroyCommandBuffer(q.command_buffer);
            gfx_.NRI.DestroyCommandAllocator(q.allocator);
        }
        if (frame_fence_)
            gfx_.NRI.DestroyFence(frame_fence_);
        if (streamer_)
            gfx_.NRI.DestroyStreamer(streamer_);
    }
    DestroyImgui();
    gfx_.Destroy();
}

void ClipViewer::InitCmdLine(cmdline::parser& cmd) {
    cmd.add("compare", 0, "compare mode: two recordings in lockstep; choose them in the window (no video paths here)");
    cmd.add<std::string>("project", 0, "open this project file (.compare.json), which holds the paths of its two recordings", false, "");
    cmd.add<int>("compare-check", 0, "play N pairs, print one JSON line and exit", false, 0);
    cmd.add("compare-window", 0, "with --compare-check: in the window (the GPU path), not headless");
    cmd.add<int>("cache-mib", 0, "decoded frame cache per recording in MiB (ffmpeg decoder, video memory)", false, 1536);
    cmd.add<std::string>("decoder", 0, "auto (GPU decode where available) or ffmpeg", false, "auto");
    cmd.add("no-vsync", 0, "present without vsync");
    cmd.add("play", 0, "start playing");
    cmd.add<int>("start-frame", 0, "start at this frame of A (default: the overlap's start)", false, -1);
    cmd.add<std::string>("view", 0, "slider, side-by-side, a or b", false, "");
    cmd.add("hide-ui", 0, "start with the UI hidden (F1 shows it)");
    cmd.add("captures", 0, "start on the Captures tab: browse the game captures and recreate them with DLSS-G and ANR FG");
    cmd.add<std::string>("capture", 0, "write the window's pixels of one frame to FILE.bin (+ FILE.json), then quit", false, "");
    cmd.add<int>("capture-frame", 0, "the frame --capture takes", false, 40);
    cmd.add<std::string>("tab", 0, "the panel tab in front at start: Clips, Areas, Export or Display", false, "");
    cmd.add<std::string>("script", 0, "actions at frames for checks: FRAME:ACTION, comma-separated (pause, play, seek=N, view=NAME, "
                         "zoom=Z, offset=K, mark, preview=0|1, quit)", false, "");
    cmd.add<std::string>("swapchain", 0, "the swap chain: sdr, hdr10 (PQ) or scrgb (FP16), where the surface offers it",
                         false, "sdr");
    cmd.add<float>("paper-white", 0, "HDR paper white in nits (0: the display's, else 203)", false, 0.0f);
    cmd.add<float>("peak", 0, "HDR peak in nits (0: the display's, else 1000)", false, 0.0f);
    cmd.add("no-hdr-metadata", 0, "HDR swap chains without HDR metadata (VK_EXT_hdr_metadata)");
    cmd.add<std::string>("log-level", 0, "trace, debug, info, warn or error", false, "info");
}

void ClipViewer::ReadCmdLine(cmdline::parser& cmd) {
    m_AlwaysActive = true;  // keep playing behind other windows
    m_Vsync = !cmd.exist("no-vsync");
    spdlog::set_level(spdlog::level::from_str(cmd.get<std::string>("log-level")));
    if (!cmd.rest().empty())
        startup_error_ = "--compare takes no video paths: choose the recordings in the window (Choose video..., or drop the files), "
                         "or open a project with --project FILE";
    project_path_ = UserPath(cmd.get<std::string>("project"));
    check_pairs_ = cmd.get<int>("compare-check");
    if (check_pairs_ > 0 && project_path_.empty())
        startup_error_ = "--compare-check needs --project FILE";
    cache_mib_ = (uint64_t)std::max(cmd.get<int>("cache-mib"), 64);
    force_ffmpeg_ = cmd.get<std::string>("decoder") == "ffmpeg";
    autoplay_ = cmd.exist("play") || check_pairs_ > 0;
    start_frame_ = cmd.get<int>("start-frame");
    layout_option_ = cmd.get<std::string>("view");
    hide_ui_ = cmd.exist("hide-ui");
    start_captures_ = cmd.exist("captures") || cmd.get<std::string>("mode") == "captures";
    capture_path_ = UserPath(cmd.get<std::string>("capture"));
    capture_frame_ = cmd.get<int>("capture-frame");
    capture_quits_ = !capture_path_.empty();
    script_ = cmd.get<std::string>("script");
    focus_tab_ = cmd.get<std::string>("tab");
    {
        std::stringstream items(script_);
        for (std::string item; std::getline(items, item, ',');) {
            const size_t colon = item.find(':');
            if (colon == std::string::npos) {
                spdlog::warn("--script: {} is not FRAME:ACTION", item);
                continue;
            }
            script_actions_.push_back({(uint32_t)std::stoul(item.substr(0, colon)), item.substr(colon + 1)});
        }
    }
    const int swap = EncodingByName(cmd.get<std::string>("swapchain"), SWAP_NAMES);
    swap_options_.vsync = m_Vsync;
    swap_options_.wanted = swap < 0 ? (uint32_t)SRGB8 : (uint32_t)swap;
    swap_options_.paper_white_set = cmd.get<float>("paper-white") > 0.0f;
    swap_options_.peak_set = cmd.get<float>("peak") > 0.0f;
    swap_options_.paper_white = cmd.get<float>("paper-white");
    swap_options_.peak = cmd.get<float>("peak");
    swap_options_.hdr_metadata = !cmd.exist("no-hdr-metadata");
}

// --- Setup ------------------------------------------------------------------------------------------------------------

bool ClipViewer::Initialize(nri::GraphicsAPI, bool) {
    std::string error;
    if (!startup_error_.empty()) {
        std::fprintf(stderr, "%s\n", startup_error_.c_str());
        spdlog::critical("{}", startup_error_);
        return false;
    }
    {
        // The renderer's state folder: the recent pairs, and the projects of pairs chosen in the window, in compare/.
        const std::string cache = paths::State();
        registry_path_ = cache + "/compare-recent.json";
        compare_dir_ = cache + "/compare";
        std::string note;
        registry_ = PairRegistry::Load(registry_path_, &note);
        if (!note.empty())
            spdlog::warn("{}", note);
    }
    if (!gfx_.Create(/*window=*/true, error, /*video_decode=*/!force_ffmpeg_)) {
        spdlog::critical("{}", error);
        return false;
    }
    auto& NRI = gfx_.NRI;
    nri::StreamerDesc streamer = {};
    streamer.dynamicBufferMemoryLocation = nri::MemoryLocation::HOST_UPLOAD;
    streamer.dynamicBufferDesc = {0, 0, nri::BufferUsageBits::VERTEX | nri::BufferUsageBits::INDEX};
    streamer.constantBufferMemoryLocation = nri::MemoryLocation::HOST_UPLOAD;
    streamer.queuedFrameNum = GetQueuedFrameNum();
    streamer.hostDataCapacity = IMGUI_HOST_DATA_CAPACITY;
    if (NRI.CreateStreamer(*gfx_.device, streamer, streamer_) != nri::Result::SUCCESS ||
        NRI.CreateFence(*gfx_.device, nri::FenceDesc{}, frame_fence_) != nri::Result::SUCCESS) {
        spdlog::critical("cannot create the streamer or the frame fence");
        return false;
    }
    queued_frames_.resize(GetQueuedFrameNum());
    for (QueuedFrame& q : queued_frames_)
        if (NRI.CreateCommandAllocator(*gfx_.queue, q.allocator) != nri::Result::SUCCESS ||
            NRI.CreateCommandBuffer(*q.allocator, q.command_buffer) != nri::Result::SUCCESS) {
            spdlog::critical("cannot create a command buffer");
            return false;
        }
    if (!gfx_.CreateDescriptorPool(32, 64)) {
        spdlog::critical("cannot create the descriptor pool");
        return false;
    }
    swap_.QueryDisplay(gfx_, *this, m_Window, swap_options_);
    if (!swap_.Create(gfx_, *this, GetQueuedFrameNum(), error) || !pass_.Create(gfx_, GetQueuedFrameNum(), error)) {
        spdlog::critical("{}", error);
        return false;
    }

    FfmpegOptions ffmpeg;
    ffmpeg.memory_bytes = cache_mib_ << 20;
    SetVideoOptions(ffmpeg, force_ffmpeg_);

    if (!InitImgui(*gfx_.device))
        return false;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    imgui_ini_ = paths::State() + "/imgui-layout-clips.ini";
    std::error_code ec;
    fs::create_directories(fs::path(imgui_ini_).parent_path(), ec);
    io.IniFilename = imgui_ini_.c_str();
    renderer::SetupUiStyle(base_style_, mono_);
    ApplyUiScale();
    show_ui_ = !hide_ui_;
    refresh_hz_ = RefreshRate();
    spdlog::info("window system: {}, {:.0f} Hz", VideoDriver(), refresh_hz_);
    check_started_ = Seconds();
    // --project opens a project at start; without it the window starts empty (no decoder, no ffmpeg) and shows the cards
    if (!project_path_.empty()) {
        if (!StartupOpen(project_path_, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            spdlog::critical("{}", error);
            return false;
        }
        check_started_ = Seconds();
    }
    UpdateWindowTitle();
    spdlog::info("mode: {}", start_captures_ ? "captures" : "compare");
    return true;
}

// The Captures tab's studio: the device, the swap chain and the few things of the window it needs.
bool ClipViewer::EnsureCaptures() {
    if (captures_)
        return true;
    captures::StudioHost host;
    host.gfx = &gfx_;
    host.swap = &swap_;
    host.window = m_Window;
    host.queued_frames = GetQueuedFrameNum();
    const char* workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY");
    host.workspace = workspace ? workspace : fs::current_path().string();
    while (host.workspace.size() > 1 && (host.workspace.back() == '/' || host.workspace.back() == '\\'))
        host.workspace.pop_back();
    host.seconds = [] { return Seconds(); };
    host.scroll = [this](float& x, float& y, float& pinch) {
        x = GetScroll().x;
        y = GetScroll().y;
        pinch = GetPinch();
    };
    host.toggle_fullscreen = [this] {
        fullscreen_ = !fullscreen_;
        SetFullscreen(fullscreen_);
    };
    host.quit = [this] { close_ = true; };
    host.switch_mode = [this](renderer::Mode mode) { SwitchMode(mode); };
    host.notify = [this](const std::string& text, bool bad) { Notify(text, bad); };
    host.capture = [this](const std::string& path) {
        capture_path_ = path;
        capture_frame_ = 0;
        capture_quits_ = false;
    };
    auto studio = std::make_unique<captures::CaptureStudio>();
    std::string error;
    if (!studio->Create(host, error)) {
        spdlog::error("the Captures tab: {}", error);
        Notify("The Captures tab cannot start: " + error, true);
        return false;
    }
    captures_ = std::move(studio);
    return true;
}

void ClipViewer::StartSync(bool local) {
    SyncJob::Recording a = {a_path_, probe_[0].duration}, b = {b_path_, probe_[1].duration};
    local_window_.reset();
    local_note_.clear();
    if (local)
        sync_.StartLocal(a, b, fps_, TimeOf(player_->clock().position()), (double)OffsetHere() / fps_);
    else
        sync_.StartFull(a, b, fps_);
}

int64_t ClipViewer::OffsetHere() const {
    return project_.Offsets().at(player_->clock().position());
}

// The offset as a constant: the project's offset_frames, and no anchors in use.
void ClipViewer::ApplyOffset(int64_t k, bool from_detection) {
    if (k == project_.offset_frames && !project_.follow_drift && !from_detection)
        return;
    project_.follow_drift = false;
    project_.offset_frames = k;
    player_->SetOffsets(project_.Offsets());
    MarkDirty();
}

// A whole map (a detection, an anchor edit): its control points are the project's, and offset_frames is its first K.
void ClipViewer::ApplyOffsets(const OffsetMap& offsets, bool) {
    project_.SetOffsets(offsets);
    player_->SetOffsets(project_.Offsets());
    MarkDirty();
}

// A nudge acts where the playhead is when the drift is followed: an anchor within a second of it moves, else the nudge makes
// an anchor at the playhead; without the drift it is the constant offset.
void ClipViewer::NudgeOffset(int64_t delta) {
    if (!project_.follow_drift) {
        ApplyOffset(project_.offset_frames + delta, false);
        return;
    }
    OffsetMap map = project_.Offsets();
    map.Nudge(player_->clock().position(), delta, (int64_t)std::llround(fps_));
    ApplyOffsets(map, false);
}

// A detection around the playhead: with the drift followed, an anchor at the window's middle (the anchors within its 10 s go,
// the others stay); else the constant offset.
void ClipViewer::ApplyLocalWindow(const SyncWindow& w) {
    const int64_t k = (int64_t)std::llround(w.offset * fps_);
    if (!project_.follow_drift) {
        ApplyOffset(k, false);
        return;
    }
    const int64_t middle = (int64_t)std::llround(w.a_time * fps_), half = (int64_t)std::llround(5.0 * fps_);
    std::vector<OffsetPoint> points;
    for (const OffsetPoint& p : project_.offset_map)
        if (std::llabs(p.a - middle) > half)
            points.push_back(p);
    points.push_back({middle, w.offset * fps_});
    ApplyOffsets(OffsetMap(points, k), false);
}

// The number field and the scripts: K at the playhead should be k.
void ClipViewer::SetOffsetHere(int64_t k) {
    if (!project_.follow_drift)
        ApplyOffset(k, false);
    else if (k != OffsetHere())
        NudgeOffset(k - OffsetHere());
}

// Following: the anchors are K(n) again. Not following: the constant is K where the playhead is now.
void ClipViewer::SetFollowDrift(bool follow) {
    if (follow == project_.follow_drift)
        return;
    if (follow) {
        project_.follow_drift = true;
        project_.offset_frames = OffsetMap(project_.offset_map, project_.offset_frames).first_k();
    } else {
        const int64_t here = OffsetHere();
        project_.follow_drift = false;
        project_.offset_frames = here;
    }
    player_->SetOffsets(project_.Offsets());
    MarkDirty();
}

// An anchor at the playhead with the K shown there (after a nudge, say): it keeps that K while its neighbours move.
void ClipViewer::AddAnchorHere() {
    OffsetMap map = project_.Offsets();  // the followed map, or the constant when the drift is not followed
    const int64_t n = player_->clock().position();
    map.Add(n, (double)map.at(n));
    project_.follow_drift = true;
    ApplyOffsets(map, false);
}

void ClipViewer::RemoveAnchorHere() {
    OffsetMap map = project_.Offsets();
    const int64_t here = OffsetHere();
    if (!map.RemoveNearest(player_->clock().position()))
        return;
    if (map.is_constant())  // the last anchor: K stays what it is here
        map = OffsetMap(here);
    ApplyOffsets(map, false);
}

void ClipViewer::ClearAnchors() {
    if (project_.offset_map.empty())
        return;
    const int64_t here = OffsetHere();
    project_.follow_drift = false;
    ApplyOffsets(OffsetMap(here), false);
}

void ClipViewer::PollJobs() {
    if (!sync_.running()) {
        if (auto result = sync_.TakeResult()) {
            project_.sync = result;
            // The detection replaces the offset: its map (followed when the drift or a jump is real) or its constant.
            if (result->tracking.significant && !result->tracking.map.empty()) {
                project_.follow_drift = true;
                ApplyOffsets(OffsetMap(result->tracking.map, result->offset_frames), true);
            } else {
                project_.offset_map.clear();
                ApplyOffset(result->offset_frames, true);
            }
            sync_line_ = "detected " + std::to_string(result->offset_frames) + " frames (" + result->quality + ")";
            MarkDirty();
            // a new pair started at frame 0, where only A has frames before the offset: paused there, it goes to the overlap
            const Timeline& t = player_->clock().timeline();
            if (seek_to_overlap_ && !player_->clock().playing() && t.has_overlap() && player_->clock().position() < t.overlap_first())
                player_->clock().Seek(t.overlap_first());
        }
        seek_to_overlap_ = false;
        if (auto window = sync_.TakeWindow()) {
            local_window_ = window;
            local_note_.clear();
        }
        const SyncJob::Status status = sync_.Poll();
        if (!status.error.empty() && status.error != "cancelled")
            sync_line_ = status.error;
    }
}

void ClipViewer::MarkDirty() {
    if (!dirty_)
        dirty_since_ = Seconds();
    dirty_ = true;
}

void ClipViewer::SaveIfDue(bool force) {
    if (!loaded_ || !dirty_ || project_path_.empty())
        return;
    if (!force && Seconds() - dirty_since_ < 0.4)
        return;
    project_.layout = base_layout_ == Layout::side_by_side ? "side-by-side" : "slider";
    project_.divider = divider_;
    std::string error;
    if (SaveProject(project_, project_path_, error)) {
        MarkSaved(project_);
        save_note_ = "saved " + project_path_;
    } else {
        save_note_ = "not saved: " + error;
        spdlog::error("{}", save_note_);
    }
    save_note_time_ = Seconds();
    dirty_ = false;
}

// --- Frame loop -------------------------------------------------------------------------------------------------------

void ClipViewer::ApplyUiScale() {
    if (ui_scale_auto_)
        ui_scale_ = renderer::AutoUiScale(*this);
    ImGuiStyle& style = ImGui::GetStyle();
    style = base_style_;
    style.ScaleAllSizes(ui_scale_);
    style.FontScaleMain = ui_scale_;
}

void ClipViewer::SignalFrame(uint32_t frame_index) {
    nri::FenceSubmitDesc done = {frame_fence_, 1 + (uint64_t)frame_index};
    nri::QueueSubmitDesc submit = {};
    submit.signalFences = &done;
    submit.signalFenceNum = 1;
    gfx_.NRI.QueueSubmit(*gfx_.queue, submit);
}

void ClipViewer::LatencySleep(uint32_t frame_index) {
    auto& NRI = gfx_.NRI;
    const uint32_t queued_num = GetQueuedFrameNum();
    // Without vsync at most twice the monitor's refresh rate: presenting at 1000+ fps through XWayland can stall the X
    // server for seconds.
    if (!m_Vsync && refresh_hz_ > 0.0) {
        using Clock = std::chrono::steady_clock;
        const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(0.5 / refresh_hz_));
        Clock::time_point now = Clock::now();
        if (next_frame_ < now - 2 * interval || next_frame_ > now + 2 * interval)
            next_frame_ = now;
        while ((now = Clock::now()) < next_frame_)
            std::this_thread::sleep_for(std::min<Clock::duration>(next_frame_ - now, std::chrono::milliseconds(1)));
        next_frame_ += interval;
    }
    NRI.Wait(*frame_fence_, frame_index >= queued_num ? 1 + frame_index - queued_num : 0);
    QueuedFrame& q = queued_frames_[frame_index % queued_num];
    NRI.ResetCommandAllocator(*q.allocator);
    q.pair.reset();  // the GPU frame that used these frames has retired
}

void ClipViewer::HandleResize() {
    const int fw = (int)PixelSize().x, fh = (int)PixelSize().y;
    if (fw > 0 && fh > 0 && (m_OutputResolution.x != (uint32_t)fw || m_OutputResolution.y != (uint32_t)fh)) {
        spdlog::info("window {}x{}", fw, fh);
        m_OutputResolution = uint2((uint32_t)fw, (uint32_t)fh);
        swap_.dirty = true;
        ImGui::GetIO().DisplaySize = ImVec2((float)fw, (float)fh);  // the loop set the old size for this frame
        ApplyUiScale();
    }
    if (swap_.dirty && fw > 0 && fh > 0) {
        gfx_.NRI.DeviceWaitIdle(gfx_.device);
        if (swap_.wanted() != swap_.encoding())
            swap_.UpdateDefaults(m_Window);  // the new mode's, for its HDR metadata
        swap_.vsync = m_Vsync;
        swap_.Destroy();
        std::string error;
        if (!swap_.Create(gfx_, *this, GetQueuedFrameNum(), error)) {
            spdlog::critical("{}", error);
            close_ = true;
        }
    }
}

void ClipViewer::PushDrop(const std::vector<std::string>& files, double x, bool complete) {
    int w = 0, h = 0;
    SDL_GetWindowSize(m_Window, &w, &h);
    auto push = [&](SDL_EventType type, const char* data) {
        SDL_Event e;
        SDL_zero(e);
        e.type = type;
        e.drop.windowID = SDL_GetWindowID(m_Window);
        e.drop.x = (float)(x * w);
        e.drop.y = (float)h * 0.5f;
        e.drop.data = data;
        if (!SDL_PushEvent(&e))
            spdlog::warn("--script: cannot push a drop event: {}", SDL_GetError());
    };
    push(SDL_EVENT_DROP_BEGIN, nullptr);
    push(SDL_EVENT_DROP_POSITION, nullptr);
    for (const std::string& file : files) {
        pushed_drop_paths_.push_back(file);
        push(SDL_EVENT_DROP_FILE, pushed_drop_paths_.back().c_str());
    }
    if (complete)
        push(SDL_EVENT_DROP_COMPLETE, nullptr);
}

void ClipViewer::RunScript(uint32_t app_frame) {
    // wait=idle holds the script (not the viewer) until no detection runs and no open waits, wait=SECONDS for that long:
    // later actions keep their distance in frames from it
    if (script_wait_ || Seconds() < script_wait_until_) {
        if (Seconds() < script_wait_until_ || sync_.running() || open_request_ || dialogs_.busy()) {
            script_delay_++;
            return;
        }
        script_wait_ = false;
    }
    const uint32_t frame_index = app_frame - script_delay_;
    pushed_drop_paths_.clear();  // the events that pointed to them were taken at the start of this frame
    if (scripted_mouse_)  // the real pointer's position was queued by the loop: the scripted one goes after it
        ImGui::GetIO().AddMousePosEvent(scripted_pos_.x, scripted_pos_.y);
    for (auto it = script_releases_.begin(); it != script_releases_.end();)
        if (it->first == frame_index) {
            ImGui::GetIO().AddKeyEvent(it->second, false);
            it = script_releases_.erase(it);
        } else {
            ++it;
        }
    for (const auto& [frame, action] : script_actions_) {
        if (frame != frame_index)
            continue;
        const size_t eq = action.find('=');
        const std::string name = action.substr(0, eq), value = eq == std::string::npos ? "" : action.substr(eq + 1);
        // Choosing the recordings: the same functions a dialog's result, a drop or a click on Recent calls. An open runs
        // three frames after its action (one that says "opening" shows first): give the next action a few frames.
        if (name == "open-a" || name == "open-b") {
            HandleDialogResult({name == "open-a" ? FileDialog::Kind::VIDEO_A : FileDialog::Kind::VIDEO_B, false, "", {UserPath(value)}});
            continue;
        }
        if (name == "open-project" || name == "save-project") {
            HandleDialogResult({name == "open-project" ? FileDialog::Kind::PROJECT_OPEN : FileDialog::Kind::PROJECT_SAVE, false, "",
                                {UserPath(value)}});
            continue;
        }
        // The drop actions push SDL's own drop events (begin, position, one file event per file, complete), which the next
        // frame's event loop takes: App::PollEvents, OnEvent, the batching, the decision, as a system's drop goes. The
        // position is a fraction of the window's width.
        if (name == "drop-a" || name == "drop-b" || name == "drop-both") {  // drop-both=PATH|PATH: two files in one drop
            const size_t bar = value.find('|');
            std::vector<std::string> files = {UserPath(value.substr(0, bar))};
            if (name == "drop-both" && bar != std::string::npos)
                files.push_back(UserPath(value.substr(bar + 1)));
            PushDrop(files, name == "drop-b" ? 0.75 : 0.25, true);
            continue;
        }
        if (name == "drop-hover") {  // drop-hover=X (a drag over the window at that fraction of its width) or off
            if (value == "off")
                PushDrop({}, 0.0, true);  // the drag leaves: complete without files
            else
                PushDrop({}, std::stod(value), false);
            continue;
        }
        if (name == "swap") {
            SwapRecordings();
            continue;
        }
        if (name == "mode") {  // mode=scene|compare|captures: the Mode menu's entry (its confirmation is for a person)
            if (const auto mode = renderer::ParseMode(value))
                SwitchMode(*mode);
            else
                spdlog::warn("--script: mode={}: scene, compare or captures", value);
            continue;
        }
        if (name == "wait") {  // wait=idle (no detection, no open pending) or wait=SECONDS
            if (value == "idle")
                script_wait_ = true;
            else
                script_wait_until_ = Seconds() + std::atof(value.c_str());
            continue;
        }
        if (name.rfind("cap-", 0) == 0) {  // the Captures tab's actions: queued in the studio, run in order (captures/studio.h)
            if (EnsureCaptures())
                captures_->RunScript(name, value);
            continue;
        }
        if (name == "note") {  // note=TEXT: a line in the log that a check waits for to look at the process
            spdlog::info("note: {}", value);
            continue;
        }
        if (name == "capture") {  // capture=FILE: the window's pixels of the next frame (FILE.bin and FILE.json); the run goes on
            capture_path_ = UserPath(value);
            capture_frame_ = 0;
            capture_quits_ = false;
            continue;
        }
        if (name == "recent" || name == "remove-recent") {  // recent=N: the Nth entry of the list, as a click opens it
            const size_t index = (size_t)std::atoi(value.c_str());
            if (index >= registry_.entries().size()) {
                spdlog::warn("--script: no recent entry {}", index);
            } else if (name == "recent") {
                OpenRecent(registry_.entries()[index]);
            } else {
                const RecentPair entry = registry_.entries()[index];
                registry_.Remove(entry.a, entry.b);
                SaveRegistry();
            }
            continue;
        }
        if (name == "typing") {  // typing=0|1: the path fields (as when the system has no file dialog)
            typing_ = value != "0";
            continue;
        }
        if (name == "fail-dialog") {  // as the callback of a system without a file dialog
            HandleDialogResult({FileDialog::Kind::VIDEO_A, true, value.empty() ? "no file dialog backend" : value, {}});
            continue;
        }
        static const char* const needs_pair[] = {"pause", "play", "seek", "step", "offset", "view", "zoom", "camera", "divider",
                                                 "preview", "mark", "area", "detect", "select", "preview-mark", "logos", "export",
                                                 "mark-kind", "follow", "anchor"};
        bool wanted = false;
        for (const char* n : needs_pair)
            wanted = wanted || name == n;
        if (wanted && !loaded_) {
            spdlog::warn("--script: {} needs the two recordings", name);
            continue;
        }
        if (name == "pause")
            player_->clock().Pause();
        else if (name == "play")
            player_->clock().Play();
        else if (name == "seek")
            player_->clock().Seek(std::stoll(value));
        else if (name == "step")
            player_->clock().Step(std::stoll(value));
        else if (name == "offset")  // K at the playhead (the constant offset, or the local one when the drift is followed)
            SetOffsetHere(std::stoll(value));
        else if (name == "follow")  // follow=0|1: the drift
            SetFollowDrift(value != "0");
        else if (name == "anchor") {  // anchor=add|remove|clear
            if (value == "add")
                AddAnchorHere();
            else if (value == "remove")
                RemoveAnchorHere();
            else
                ClearAnchors();
        }
        else if (name == "view")
            SetLayout(value == "side-by-side" || value == "sbs" ? Layout::side_by_side
                      : value == "a"                            ? Layout::a_only
                      : value == "b"                            ? Layout::b_only
                                                                : Layout::slider);
        else if (name == "zoom")
            camera_.z = std::stod(value), preview_ = false;
        else if (name == "camera") {
            double cx = 0.5, cy = 0.5, z = 1.0;
            std::sscanf(value.c_str(), "%lf:%lf:%lf", &cx, &cy, &z);
            camera_ = Camera{cx, cy, z};
            preview_ = false;
        } else if (name == "divider")
            divider_ = std::stod(value);
        else if (name == "preview")
            preview_ = value != "0";
        else if (name == "mark")
            mark_mode_ = true;
        else if (name == "area") {  // x:y:w:h at the playhead
            double r[4] = {0, 0, 0, 0};
            std::sscanf(value.c_str(), "%lf:%lf:%lf:%lf", &r[0], &r[1], &r[2], &r[3]);
            AddArea(r);
        } else if (name == "hide-ui")
            show_ui_ = false;
        else if (name == "detect")  // detect=local (around the playhead) or full
            StartSync(value == "local");
        else if (name == "key") {  // key=NAME: a press, through ImGui's input queue like a real one (released next frame)
            static const std::pair<const char*, ImGuiKey> keys[] = {
                {"Space", ImGuiKey_Space}, {"Left", ImGuiKey_LeftArrow}, {"Right", ImGuiKey_RightArrow},
                {"Comma", ImGuiKey_Comma}, {"Period", ImGuiKey_Period}, {"1", ImGuiKey_1}, {"2", ImGuiKey_2},
                {"3", ImGuiKey_3}, {"4", ImGuiKey_4}, {"Tab", ImGuiKey_Tab}, {"LeftBracket", ImGuiKey_LeftBracket},
                {"RightBracket", ImGuiKey_RightBracket}, {"Equal", ImGuiKey_Equal}, {"Minus", ImGuiKey_Minus},
                {"0", ImGuiKey_0}, {"R", ImGuiKey_R}, {"Z", ImGuiKey_Z}, {"Delete", ImGuiKey_Delete},
                {"Home", ImGuiKey_Home}, {"End", ImGuiKey_End}, {"F1", ImGuiKey_F1}};
            for (const auto& [key_name, key] : keys)
                if (value == key_name) {
                    ImGui::GetIO().AddKeyEvent(key, true);
                    script_releases_.push_back({frame_index + 1, key});
                }
        } else if (name == "mouse") {  // mouse=X:Y: the pointer at window pixels (it stays there while scripted)
            float x = 0.0f, y = 0.0f;
            std::sscanf(value.c_str(), "%f:%f", &x, &y);
            scripted_mouse_ = true;
            scripted_pos_ = ImVec2(x, y);
        } else if (name == "mouse-down" || name == "mouse-up") {  // the button 0 left, 1 right, 2 middle
            ImGui::GetIO().AddMouseButtonEvent(std::atoi(value.c_str()), name == "mouse-down");
        } else if (name == "wheel") {
            ImGui::GetIO().AddMouseWheelEvent(0.0f, std::stof(value));
        } else if (name == "log") {  // the viewer's state, for checks
            spdlog::info("state: pair {} A {} B {} project {} sync {} request {}", loaded_ ? "open" : "none", a_path_, b_path_,
                         project_path_, sync_.running() ? "running" : "idle", open_request_ ? "pending" : "none");
            spdlog::info("state: recent {} entries, notice \"{}\"", registry_.entries().size(), notice_.text);
            if (!loaded_)
                continue;
            const Lockstep& c = player_->clock();
            spdlog::info("state: frame {} {} shown {} offset {} layout {} zoom {:.3f} camera {:.4f},{:.4f} divider {:.3f} areas {} rate {:.2f}",
                         (long long)c.position(), c.playing() ? "playing" : "paused", (long long)c.shown(),
                         (long long)project_.offset_frames, (int)layout_, mapping_[0].camera.z, mapping_[0].camera.cx,
                         mapping_[0].camera.cy, divider_, project_.areas.size(), c.rate());
            spdlog::info("state: pairs shown {} held {} stalls {} mismatched {} labels {} | {}",
                         (unsigned long long)c.stats().pairs_shown, (unsigned long long)c.stats().pairs_held,
                         (unsigned long long)c.stats().stalls, (unsigned long long)player_->mismatched(), project_.label_a,
                         project_.label_b);
            for (const Area& a : project_.areas)
                spdlog::info("state: area {} \"{}\" {} {:.2f}-{:.2f} rect {:.4f},{:.4f},{:.4f},{:.4f}", a.id, a.label,
                             a.kind == AreaKind::FREEZE ? "freeze" : "span", a.t0, a.t1, a.rect[0], a.rect[1], a.rect[2],
                             a.rect[3]);
            spdlog::info("state: marks {} {}", zoom_marks_ ? "on" : "off", MarkStatus().empty() ? "idle" : MarkStatus());
        }
        else if (name == "marks")  // marks=0|1: "Zoom marks while playing"
            zoom_marks_ = value != "0";
        else if (name == "select")  // select=ID: the selected mark
            selected_area_ = std::atoi(value.c_str());
        else if (name == "mark-kind")  // mark-kind=freeze|span: the kind of the next marks
            project_.new_mark_kind = value == "span" ? AreaKind::SPAN : AreaKind::FREEZE, MarkDirty();
        else if (name == "preview-mark") {  // preview-mark=ID: the sequence of a freeze mark, from its start
            for (const Area& a : project_.areas)
                if (a.id == std::atoi(value.c_str()))
                    PreviewMark(a);
        } else if (name == "logos")  // logos=0|1
            SetLogosOn(value != "0");
        else if (name == "export") {  // export=FILE[:HEIGHT[:START:END]]
            if (!export_settings_)
                export_settings_ = std::make_unique<ExportSettings>();
            double height = 0.0, start = -1.0, end = -1.0;
            const size_t colon = value.find(':');
            export_settings_->output = value.substr(0, colon);
            if (colon != std::string::npos)
                std::sscanf(value.c_str() + colon + 1, "%lf:%lf:%lf", &height, &start, &end);
            export_settings_->height = (int)height;
            export_settings_->start = start;
            export_settings_->end = end;
            StartExport();
        } else if (name == "quit") {
            if (value == "export")
                quit_after_export_ = true;  // when the export of this run ends
            else
                close_ = true;
        } else
            spdlog::warn("--script: unknown action {}", action);
    }
}

void ClipViewer::PrepareFrame(uint32_t frame_index) {
    RunScript(frame_index);
    HandleResize();
    HandleEvents();
    const double now = Seconds();
    if (start_captures_ && !loaded_) {  // --captures: the Captures tab, from the first frame
        start_captures_ = false;
        want_captures_tab_ = true;
        EnsureCaptures();
    }
    if (captures_)
        captures_->Update();
    if (loaded_) {
        // Freeze marks: the clock gets its stop before it moves, the runner follows what it showed.
        UpdateMarks();
        mark_run_.SetEnabled(zoom_marks_);
        mark_run_.BeforeTick(player_->clock(), *edit_);
        tick_ = player_->Update(now);
        PollJobs();
        if (preview_) {
            shot_ = shot_at(project_.areas, project_.ease, divider_, TimeOf(player_->clock().position()));
            preview_camera_ = shot_.camera;
        }
        mark_state_ = mark_run_.AfterTick(now, player_->clock(), *edit_, LiveShot());
    }
    DrawUi();
    SaveIfDue(false);
    // a script's export ends the run when it is done (quit=export)
    if (quit_after_export_ && exporter_ && !exporter_->running()) {
        spdlog::info("export ended: {}", exporter_->error().empty() ? "ok" : exporter_->error());
        close_ = true;
    }
    if (loaded_ && check_pairs_ > 0 && player_->clock().stats().pairs_shown >= (uint64_t)check_pairs_) {
        const LockstepStats& s = player_->clock().stats();
        std::printf("{\"pairs_shown\": %llu, \"pairs_held\": %llu, \"mismatched\": %llu, \"stalls\": %llu, \"rate_min\": %.3f, "
                    "\"decode_fps_a\": %.1f, \"decode_fps_b\": %.1f, \"seconds\": %.2f, \"decoder\": \"%s\", \"window\": true}\n",
                    (unsigned long long)s.pairs_shown, (unsigned long long)s.pairs_held,
                    (unsigned long long)player_->mismatched(), (unsigned long long)s.stalls, s.rate_min,
                    source_[0]->Stats().decode_fps, source_[1]->Stats().decode_fps, now - check_started_,
                    source_notes_[0].c_str());
        std::fflush(stdout);
        close_ = true;
    }
}

void ClipViewer::RecordVideo(nri::CommandBuffer& cb, uint32_t queued, nri::Format format) {
    if (captures_ && captures_->open()) {  // a capture's stills are on screen
        captures_->Record(cb, queued, format);
        return;
    }
    const std::shared_ptr<const Pair>& pair = queued_frames_[queued].pair;
    nri::Descriptor* luma[2] = {nullptr, nullptr};
    nri::Descriptor* chroma[2] = {nullptr, nullptr};
    VideoSourceParams params[2] = {params_[0], params_[1]};
    if (pair) {
        if (pair->has_a) {
            luma[0] = pair->a.luma;
            chroma[0] = pair->a.chroma;
        }
        if (pair->has_b) {
            luma[1] = pair->b.luma;
            chroma[1] = pair->b.chroma;
        }
    }
    params[0].exists = pair && pair->has_a;
    params[1].exists = pair && pair->has_b;
    const Regions regions = compare_regions(current_layout_, (int)params_[0].width, (int)params_[0].height, CurrentCamera());
    PassTarget target;
    target.encoding = swap_.encoding();
    target.code_encoding = SRGB8;
    target.paper_white = swap_.hdr.paper_white;
    target.hw_srgb = swap_.hw_srgb();
    target.gap_gray = pair ? 0.06f : 0.0f;
    pass_.RecordVideo(cb, queued, format, MakeVideoConstants(regions, params, Filter::AUTO, target), luma, chroma);
}

void ClipViewer::RenderFrame(uint32_t frame_index) {
    auto& NRI = gfx_.NRI;
    const uint32_t queued = frame_index % GetQueuedFrameNum();
    QueuedFrame& q = queued_frames_[queued];
    if (Minimized() || swap_.dirty) {
        SignalFrame(frame_index);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }
    SwapOutput::Frame frame;
    if (!swap_.Acquire(frame_index, frame)) {
        SignalFrame(frame_index);
        return;
    }
    q.pair = tick_.pair;  // pinned until this GPU frame retires (LatencySleep)
    nri::CommandBuffer& cb = *q.command_buffer;
    NRI.BeginCommandBuffer(cb, gfx_.descriptor_pool);
    // A capture: the swap chain's texture is copied to a buffer after the frame is drawn.
    nri::Buffer* capture = nullptr;
    nri::Memory* capture_memory = nullptr;
    uint32_t capture_pitch = 0;
    if (!capture_path_.empty() && (int)frame_index >= capture_frame_) {
        const uint32_t align = NRI.GetDeviceDesc(*gfx_.device).memoryAlignment.uploadBufferTextureRow;
        capture_pitch = (swap_.size().x * (swap_.encoding() == SCRGB16 ? 8 : 4) + align - 1) / align * align;
        nri::BufferDesc desc = {};
        desc.size = (uint64_t)capture_pitch * swap_.size().y;
        if (NRI.CreateBuffer(*gfx_.device, desc, capture) == nri::Result::SUCCESS) {
            nri::ResourceGroupDesc group = {};
            group.memoryLocation = nri::MemoryLocation::HOST_READBACK;
            group.buffers = &capture;
            group.bufferNum = 1;
            if (NRI.AllocateAndBindMemory(*gfx_.device, group, &capture_memory) != nri::Result::SUCCESS) {
                NRI.DestroyBuffer(capture);
                capture = nullptr;
            }
        }
    }
    swap_.Record(cb, *streamer_, frame, [&](nri::Format format) {
        if (loaded_ || (captures_ && captures_->open()))  // the empty start is the UI on black
            RecordVideo(cb, queued, format);
    }, true, capture, capture_pitch);
    NRI.EndCommandBuffer(cb);
    const bool presented = swap_.Present(cb, frame_index, frame);
    NRI.EndStreamerFrame(*streamer_);
    if (!presented)
        swap_.dirty = true;
    SignalFrame(frame_index);
    if (capture) {
        NRI.QueueWaitIdle(gfx_.queue);
        const char* bytes = (const char*)NRI.MapBuffer(*capture, 0, (uint64_t)capture_pitch * swap_.size().y);
        const uint32_t pixel = swap_.encoding() == SCRGB16 ? 8 : 4;
        FILE* file = std::fopen((capture_path_ + ".bin").c_str(), "wb");
        if (file) {
            for (uint32_t y = 0; y < swap_.size().y; y++)
                std::fwrite(bytes + (uint64_t)y * capture_pitch, 1, (size_t)swap_.size().x * pixel, file);
            std::fclose(file);
        }
        NRI.UnmapBuffer(*capture);
        NRI.DestroyBuffer(capture);
        NRI.FreeMemory(capture_memory);
        const nri::Format f = swap_.format();
        char note[256];
        std::snprintf(note, sizeof(note),
                      "{\"width\": %u, \"height\": %u, \"format\": %d, \"bgra\": %s, \"encoding\": %u, \"frame\": %u, "
                      "\"pair\": %lld}\n",
                      swap_.size().x, swap_.size().y, (int)f,
                      (f == nri::Format::BGRA8_UNORM || f == nri::Format::BGRA8_SRGB) ? "true" : "false", swap_.encoding(),
                      frame_index, tick_.pair ? (long long)tick_.pair->n : -1LL);
        if (FILE* json = std::fopen((capture_path_ + ".json").c_str(), "wb")) {
            std::fputs(note, json);
            std::fclose(json);
        }
        spdlog::info("capture {}: frame {}, {}x{}", capture_path_, frame_index, swap_.size().x, swap_.size().y);
        capture_path_.clear();
        if (capture_quits_)
            close_ = true;
    }
    const double now = Seconds();
    if (last_frame_time_ > 0.0) {
        const double ms = (now - last_frame_time_) * 1000.0;
        frame_ms_ = frame_ms_ == 0.0 ? ms : frame_ms_ + (1.0 - std::exp(-ms / 300.0)) * (ms - frame_ms_);
    }
    last_frame_time_ = now;
}

}  // namespace renderer::clips
