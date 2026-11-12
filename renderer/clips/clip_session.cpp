// The clip viewer's recordings: how a pair gets chosen, opened, changed and swapped, the recent pairs and their projects,
// the file dialogs' results and the drops. The rules that can be tested without a window are pair_pick.h, pair_registry.h
// and SwapSides in project.h; this file runs them against the decoders and the player.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "renderer/clips/clip_viewer.h"
#include "renderer/clips/exporter.h"
#include "renderer/clips/process.h"
#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace fs = std::filesystem;

namespace {

int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Name(const std::string& path) {
    return fs::path(path).filename().string();
}

MediaFacts Facts(const std::string& path, const ProbeResult& probe) {
    return {path, probe.info.width, probe.info.height, probe.info.fps};
}

}  // namespace

// --- Small things -----------------------------------------------------------------------------------------------------

void ClipViewer::Notify(const std::string& text, bool bad) {
    notice_.text = text;
    notice_.bad = bad;
    notice_.time = Seconds();
    if (bad)
        spdlog::warn("{}", text);
    else
        spdlog::info("{}", text);
}

bool ClipViewer::ExportRunning(const char* what) {
    if (!exporter_ || !exporter_->running())
        return false;
    Notify(std::string("an export is running: cancel it or wait for it before you ") + what, true);
    return true;
}

void ClipViewer::SaveRegistry() {
    std::string error;
    if (!registry_path_.empty() && !registry_.Save(registry_path_, error))
        spdlog::warn("recent pairs not saved: {}", error);
}

void ClipViewer::RememberFolder(const std::string& file, bool project) {
    const std::string folder = fs::path(AbsolutePath(file)).parent_path().string();
    (project ? registry_.last_project_folder : registry_.last_video_folder) = folder;
    SaveRegistry();
}

// Where a dialog starts: the folder of the last file chosen of its kind, else the folder of what is open.
std::string ClipViewer::DialogFolder(bool project) const {
    std::error_code ec;
    const std::string& remembered = project ? registry_.last_project_folder : registry_.last_video_folder;
    if (!remembered.empty() && fs::is_directory(remembered, ec))
        return remembered;
    if (loaded_)
        return fs::path(project ? project_path_ : a_path_).parent_path().string();
    return {};
}

std::string ClipViewer::CompareDir() const {
    return compare_dir_;
}

void ClipViewer::UpdateWindowTitle() {
    if (!m_Window)
        return;
    const std::string title = loaded_ ? "NR clips: " + project_.label_a + " vs " + project_.label_b : "NR clips";
    SDL_SetWindowTitle(m_Window, title.c_str());
}

// --- Dialogs, drops, the queue ----------------------------------------------------------------------------------------

void ClipViewer::ShowDialog(FileDialog::Kind kind) {
    if (dialogs_.busy())
        return;
    if (kind != FileDialog::Kind::PROJECT_SAVE && ExportRunning("change the recordings"))
        return;
    const bool project = kind == FileDialog::Kind::PROJECT_OPEN || kind == FileDialog::Kind::PROJECT_SAVE;
    std::string name;
    if (kind == FileDialog::Kind::PROJECT_SAVE)
        name = fs::path(project_path_).filename().string();
    dialogs_.Show(kind, m_Window, DialogFolder(project), name);
}

void ClipViewer::HandleDialogResult(const FileDialog::Result& result) {
    if (result.error) {
        dialogs_failed_ = true;
        typing_ = true;
        Notify("this system has no file dialog (" + result.message + "): type the path instead", true);
        return;
    }
    if (result.files.empty())
        return;  // cancelled
    const std::string file = AbsolutePath(result.files[0]);
    switch (result.kind) {
        case FileDialog::Kind::VIDEO_A:
        case FileDialog::Kind::VIDEO_B:
            RememberFolder(file, false);
            ChooseVideo(result.kind == FileDialog::Kind::VIDEO_A ? 0 : 1, file);
            break;
        case FileDialog::Kind::PROJECT_OPEN:
            RememberFolder(file, true);
            OpenProjectFile(file);
            break;
        case FileDialog::Kind::PROJECT_SAVE:
            RememberFolder(file, true);
            SaveProjectCopy(file);
            break;
    }
}

void ClipViewer::OnEvent(const SDL_Event& e) {
    auto fraction = [&](float x) {
        int w = 0, h = 0;
        SDL_GetWindowSize(m_Window, &w, &h);
        return w > 0 ? (double)x / (double)w : -1.0;
    };
    switch (e.type) {
        case SDL_EVENT_DROP_BEGIN:
            drop_.Begin();
            break;
        case SDL_EVENT_DROP_POSITION:
            drop_.Position(fraction(e.drop.x));
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data)
                drop_.File(e.drop.data, fraction(e.drop.x));
            break;
        case SDL_EVENT_DROP_COMPLETE: {
            const DropDecision decision = drop_.Complete();
            if (decision.action != DropDecision::Action::NONE)
                pending_drops_.push_back(decision);
            break;
        }
        default:
            break;
    }
}

void ClipViewer::HandleEvents() {
    while (auto result = dialogs_.Take())
        HandleDialogResult(*result);
    std::vector<DropDecision> drops;
    drops.swap(pending_drops_);
    for (const DropDecision& decision : drops)
        HandleDrop(decision);
    // An open waits two frames, so that a frame that says "opening" is on screen before the decoders start.
    if (open_request_ && ++open_request_age_ >= 3) {
        OpenRequest request = std::move(*open_request_);
        open_request_.reset();
        std::string error;
        if (OpenPair(request, error)) {
            pick_[0] = pick_[1] = Pick();
        } else {
            Notify(error, true);
        }
    }
}

void ClipViewer::HandleDrop(const DropDecision& decision) {
    using Action = DropDecision::Action;
    switch (decision.action) {
        case Action::NONE:
            break;
        case Action::REJECT:
            Notify(decision.message, true);
            break;
        case Action::SET_A:
            ChooseVideo(0, decision.a);
            break;
        case Action::SET_B:
            ChooseVideo(1, decision.b);
            break;
        case Action::SET_BOTH:
            ChoosePair(decision.a, decision.b);
            break;
        case Action::OPEN_PROJECT:
            OpenProjectFile(decision.project);
            break;
    }
}

// --- Choosing ---------------------------------------------------------------------------------------------------------

void ClipViewer::RequestOpen(OpenRequest request) {
    spdlog::info("opening {} and {}", Name(request.a), Name(request.b));
    open_request_ = std::move(request);
    open_request_age_ = 0;
}

// A recording for A or B. On the empty start it is picked, and the pair opens when both are there; with a pair open it
// replaces that side (the pair's project is then the one of the new pair). The old pair stays whenever the new one is
// refused: a file that cannot be read, the same file twice, or another size or frame rate.
void ClipViewer::ChooseVideo(int side, const std::string& raw_path) {
    const std::string path = AbsolutePath(raw_path);
    const char* name = side == 0 ? "A" : "B";
    std::error_code ec;
    if (ExportRunning("change a video"))
        return;
    if (!fs::is_regular_file(path, ec)) {
        Notify("cannot use " + path + " as " + name + ": the file is not there", true);
        return;
    }
    if (loaded_ && SameFile(path, side == 0 ? a_path_ : b_path_)) {
        Notify(Name(path) + " is " + name + " already", false);
        return;
    }
    ProbeResult probe;
    std::string error;
    if (!ProbeVideo(path, probe, error)) {
        Notify("cannot use " + Name(path) + " as " + name + ": " + error, true);
        return;
    }
    const std::string& other_path = loaded_ ? (side == 0 ? b_path_ : a_path_) : pick_[1 - side].path;
    const ProbeResult& other_probe = loaded_ ? probe_[1 - side] : pick_[1 - side].probe;
    if (!other_path.empty()) {
        const char* other_name = side == 0 ? "B" : "A";
        if (SameFile(path, other_path)) {
            Notify("cannot use " + Name(path) + " as " + name + ": it is " + other_name +
                       " already; a video compared with itself shows no difference",
                   true);
            return;
        }
        const std::string problem = side == 0 ? CompatibilityProblem(Facts(path, probe), Facts(other_path, other_probe), "A")
                                              : CompatibilityProblem(Facts(other_path, other_probe), Facts(path, probe), "B");
        if (!problem.empty()) {
            const std::string& kept = side == 0 ? a_path_ : b_path_;
            Notify(problem + (loaded_ ? std::string(". ") + name + " stays " + Name(kept) : std::string()), true);
            return;
        }
    }
    if (!loaded_) {
        pick_[side] = {path, probe};
        notice_.text.clear();
        if (pick_[1 - side].path.empty())
            return;  // the other one is still to choose
    }
    OpenRequest request;
    request.a = loaded_ ? (side == 0 ? path : a_path_) : pick_[0].path;
    request.b = loaded_ ? (side == 0 ? b_path_ : path) : pick_[1].path;
    request.probe[side] = probe;
    request.have_probe[side] = true;
    if (!loaded_) {
        request.probe[1 - side] = pick_[1 - side].probe;
        request.have_probe[1 - side] = true;
    }
    request.changed = name;
    RequestOpen(std::move(request));
}

// Two recordings at once (two files dropped): a new pair, A and B in order.
void ClipViewer::ChoosePair(const std::string& raw_a, const std::string& raw_b) {
    OpenRequest request;
    request.a = AbsolutePath(raw_a);
    request.b = AbsolutePath(raw_b);
    std::error_code ec;
    if (ExportRunning("change the recordings"))
        return;
    for (int v = 0; v < 2; v++) {
        const std::string& path = v == 0 ? request.a : request.b;
        std::string error;
        if (!fs::is_regular_file(path, ec)) {
            Notify("cannot use " + path + " as " + (v == 0 ? "A" : "B") + ": the file is not there", true);
            return;
        }
        if (!ProbeVideo(path, request.probe[v], error)) {
            Notify("cannot use " + Name(path) + " as " + (v == 0 ? "A" : "B") + ": " + error, true);
            return;
        }
        request.have_probe[v] = true;
    }
    if (SameFile(request.a, request.b)) {
        Notify("both files are " + Name(request.a) + "; a video compared with itself shows no difference", true);
        return;
    }
    const std::string problem = CompatibilityProblem(Facts(request.a, request.probe[0]), Facts(request.b, request.probe[1]));
    if (!problem.empty()) {
        Notify(problem + (loaded_ ? ". The pair stays as it is" : std::string()), true);
        return;
    }
    RequestOpen(std::move(request));
}

void ClipViewer::OpenProjectFile(const std::string& raw_path) {
    if (ExportRunning("open a project"))
        return;
    const std::string path = AbsolutePath(raw_path);
    OpenRequest request;
    Project project;
    std::string error;
    if (!LoadProject(path, project, error)) {
        Notify("cannot open the project: " + error, true);
        return;
    }
    project.file = path;
    request.a = AbsolutePath(project.ResolvedA());
    request.b = AbsolutePath(project.ResolvedB());
    request.project = std::move(project);
    RequestOpen(std::move(request));
}

void ClipViewer::OpenRecent(const RecentPair& entry) {
    if (ExportRunning("open another pair"))
        return;
    OpenRequest request;
    request.a = entry.a;
    request.b = entry.b;
    RequestOpen(std::move(request));
}

// --- Opening ----------------------------------------------------------------------------------------------------------

// The pair on the screen goes: the player, and what holds its frames (the GPU is idle first).
void ClipViewer::ReleasePlayer() {
    sync_.Clear();
    if (gfx_.NRI.HasCore())
        gfx_.NRI.DeviceWaitIdle(gfx_.device);
    for (QueuedFrame& q : queued_frames_)
        q.pair.reset();
    tick_ = Player::Tick();
    mark_run_ = MarkRunner();
    mark_state_ = MarkRunner::State();
    edit_.reset();
    player_.reset();
}

// What belongs to the pair that was open and not to the viewer.
void ClipViewer::ResetPairState() {
    camera_ = Camera();
    preview_camera_ = Camera();
    preview_ = false;
    mark_mode_ = false;
    redraw_area_ = -1;
    selected_area_ = -1;
    drag_ = Drag();
    shot_ = Shot();
    sync_line_.clear();
    local_window_.reset();
    local_note_.clear();
    exporter_.reset();
    export_settings_.reset();
    export_error_.clear();
    logos_checked_ = -1e9;
    area_label_buffer_.clear();
}

// Opens the pair of `request`: every check first, then the new decoders, and only then does the old pair go, so that a
// refusal or a failure leaves the viewer as it was. A recording that is open on the same side already stays open. The
// project is the request's, else the registry's for these two files, else a new one in the cache folder (its start
// offset is then detected in the background).
bool ClipViewer::OpenPair(OpenRequest& r, std::string& error) {
    if (exporter_ && exporter_->running()) {
        error = "an export is running: cancel it or wait for it first";
        return false;
    }
    const std::string path[2] = {AbsolutePath(r.a), AbsolutePath(r.b)};
    std::error_code ec;
    for (int v = 0; v < 2; v++)
        if (!fs::is_regular_file(path[v], ec)) {
            error = std::string("video ") + (v == 0 ? "A" : "B") + " is not there: " + path[v];
            return false;
        }
    if (SameFile(path[0], path[1])) {
        error = "A and B are the same file (" + Name(path[0]) + ")";
        return false;
    }
    const bool keep[2] = {loaded_ && SameFile(path[0], a_path_), loaded_ && SameFile(path[1], b_path_)};
    ProbeResult probe[2];
    for (int v = 0; v < 2; v++) {
        if (keep[v])
            probe[v] = probe_[v];
        else if (r.have_probe[v])
            probe[v] = r.probe[v];
        else if (!ProbeVideo(path[v], probe[v], error))
            return false;
    }
    const std::string problem = CompatibilityProblem(Facts(path[0], probe[0]), Facts(path[1], probe[1]), r.changed);
    if (!problem.empty()) {
        error = problem;
        return false;
    }

    const VideoContext context = {gfx_.device, &gfx_.NRI, &gfx_.NRI, gfx_.queue};
    std::unique_ptr<VideoSource> source[2];
    std::string reason[2], note[2];
    for (int v = 0; v < 2; v++) {
        if (keep[v])
            continue;
        source[v] = OpenVideo(context, path[v], &reason[v]);
        if (!source[v]) {
            error = "cannot open " + path[v] + ": " + reason[v];
            return false;
        }
        note[v] = source[v]->Describe();
        spdlog::info("{}: {} ({})", path[v], note[v], reason[v]);
    }

    // The project: the chosen one, the registry's, or a new one.
    Project project;
    std::string project_file;
    bool created = false;
    if (r.project) {
        project = std::move(*r.project);
        project_file = project.file;
    } else {
        std::string candidate;
        if (const RecentPair* entry = registry_.Find(path[0], path[1]); entry && fs::is_regular_file(entry->project, ec))
            candidate = entry->project;
        if (candidate.empty()) {  // not in the list (or its file is gone): the file this pair's name gives, if it exists
            const std::string by_name = ProjectFileFor(CompareDir(), path[0], path[1]);
            if (fs::is_regular_file(by_name, ec))
                candidate = by_name;
        }
        if (!candidate.empty()) {
            std::string load_error;
            if (LoadProject(candidate, project, load_error) && SameFile(project.ResolvedA(), path[0]) &&
                SameFile(project.ResolvedB(), path[1])) {
                project_file = candidate;
                project.file = candidate;
            } else {
                spdlog::warn("project {} is not usable ({}): making a new one", candidate,
                             load_error.empty() ? "it is of other recordings" : load_error);
                project = Project();
            }
        }
        if (project_file.empty()) {
            project_file = ProjectFileFor(CompareDir(), path[0], path[1]);
            project = NewProject(path[0], path[1], (int)probe[0].info.width, (int)probe[0].info.height, probe[0].info.fps);
            project.file = project_file;
            created = true;
        }
    }

    // Everything holds: the old pair goes. Its project is saved first; a recording that stays moves to the new pair.
    SaveIfDue(true);
    ReleasePlayer();
    for (int v = 0; v < 2; v++) {
        if (keep[v])
            source[v] = std::move(source_[v]);
        else
            source_[v].reset();  // its decoder and frame cache close here
    }
    for (int v = 0; v < 2; v++) {
        source_[v] = std::move(source[v]);
        probe_[v] = probe[v];
        if (!keep[v]) {
            source_notes_[v] = note[v];
            gpu_decode_note_[v] = reason[v];
        }
    }
    a_path_ = path[0];
    b_path_ = path[1];
    project_ = std::move(project);
    project_path_ = project_file;
    project_created_ = created;
    ResetPairState();

    const VideoInfo& a = source_[0]->Info();
    fps_ = a.fps;
    project_.fps = a.fps;
    project_.width = (int)a.width;
    project_.height = (int)a.height;
    layout_ = base_layout_ = project_.layout == "side-by-side" ? Layout::side_by_side : Layout::slider;
    divider_ = project_.divider;
    if (startup_open_) {
        if (layout_option_ == "slider")
            layout_ = base_layout_ = Layout::slider;
        else if (layout_option_ == "side-by-side" || layout_option_ == "sbs")
            layout_ = base_layout_ = Layout::side_by_side;
        else if (layout_option_ == "a")
            layout_ = Layout::a_only;
        else if (layout_option_ == "b")
            layout_ = Layout::b_only;
    }
    for (int v = 0; v < 2; v++) {
        const VideoInfo& info = source_[v]->Info();
        params_[v].width = info.width;
        params_[v].height = info.height;
        params_[v].yuv = MakeYuvParams(info);
    }
    player_ = std::make_unique<Player>(source_[0].get(), source_[1].get(), project_.Offsets());
    if (startup_open_ && start_frame_ >= 0)
        player_->clock().Seek(start_frame_);
    if (startup_open_ && autoplay_)
        player_->clock().Play();
    loaded_ = true;
    dirty_ = false;

    registry_.Touch(path[0], path[1], project_file, UnixNow());
    SaveRegistry();
    UpdateWindowTitle();
    notice_.text.clear();
    if (created) {
        spdlog::info("new project {}: detecting the offset", project_path_);
        MarkDirty();
        StartSync(false);
        seek_to_overlap_ = !(startup_open_ && start_frame_ >= 0);
    } else {
        spdlog::info("project {} ({} marks, offset {}{})", project_path_, project_.areas.size(), project_.offset_frames,
                     project_.follow_drift && !project_.offset_map.empty()
                         ? ", following " + std::to_string(project_.offset_map.size()) + " anchors"
                         : "");
    }
    return true;
}

// The first open, from --project: the same path as "Open project..." without the frames of delay, and its failure ends
// the run (a script or a test that gives a project that cannot be opened wants the exit code).
bool ClipViewer::StartupOpen(const std::string& project_file, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(project_file, ec)) {
        error = "the project " + project_file + " is not there";
        return false;
    }
    OpenRequest request;
    Project project;
    if (!LoadProject(project_file, project, error))
        return false;
    project.file = AbsolutePath(project_file);
    request.a = AbsolutePath(project.ResolvedA());
    request.b = AbsolutePath(project.ResolvedB());
    request.project = std::move(project);
    startup_open_ = true;
    const bool ok = OpenPair(request, error);
    startup_open_ = false;
    return ok;
}

// --- Swap and save a copy -----------------------------------------------------------------------------------------------

// Swap A and B: the files, labels and the offset's sign change places, the marks and the detection stay on the same
// moments, the logos change sides (SwapSides). Nothing is decoded again: the sources change places too, and the player is
// made again over them, at the same moment of the recordings, playing or paused as it was.
void ClipViewer::SwapRecordings() {
    if (!loaded_ || ExportRunning("swap A and B"))
        return;
    const int64_t old_position = player_->clock().position();
    const int64_t old_offset = project_.Offsets().at(old_position);  // K at the playhead: the same pair is at n - K
    const bool playing = player_->clock().playing();
    const double speed = player_->clock().speed();
    const bool detecting = sync_.running() && !sync_.Poll().local;
    SaveIfDue(true);
    ReleasePlayer();
    const std::string old_a = a_path_, old_b = b_path_;
    std::swap(source_[0], source_[1]);
    std::swap(probe_[0], probe_[1]);
    std::swap(source_notes_[0], source_notes_[1]);
    std::swap(gpu_decode_note_[0], gpu_decode_note_[1]);
    std::swap(a_path_, b_path_);
    SwapSides(project_);
    project_.file = project_path_;
    for (int v = 0; v < 2; v++) {
        const VideoInfo& info = source_[v]->Info();
        params_[v].width = info.width;
        params_[v].height = info.height;
        params_[v].yuv = MakeYuvParams(info);
    }
    // The view stays (zoom, divider, layout, the marks and their selection); what described the old order goes.
    preview_ = false;
    sync_line_.clear();
    local_window_.reset();
    local_note_.clear();
    export_settings_.reset();
    export_error_.clear();
    player_ = std::make_unique<Player>(source_[0].get(), source_[1].get(), project_.Offsets());
    player_->clock().SetSpeed(speed);
    player_->clock().Seek(old_position - old_offset);  // the same pair of frames: the A numbering moved by K
    if (playing)
        player_->clock().Play();
    if (!registry_.Rekey(old_a, old_b, a_path_, b_path_, UnixNow()))
        registry_.Touch(a_path_, b_path_, project_path_, UnixNow());
    SaveRegistry();
    MarkDirty();
    SaveIfDue(true);
    UpdateWindowTitle();
    if (detecting)
        StartSync(false);  // the detection that ran was of the other order
    Notify("swapped: A is " + Name(a_path_) + ", B is " + Name(b_path_) + ", offset " +
               std::to_string((long long)project_.Offsets().at(player_->clock().position())) + " frames",
           false);
}

// "Save project as...": a copy of the project, with the recordings' absolute paths (so that it opens from anywhere). The
// pair keeps its own project in the cache folder.
void ClipViewer::SaveProjectCopy(const std::string& raw_path) {
    if (!loaded_)
        return;
    std::string target = AbsolutePath(raw_path);
    auto ends_with = [](const std::string& text, const std::string& tail) {
        return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
    };
    std::string lower = target;
    for (char& c : lower)
        c = (char)std::tolower((unsigned char)c);
    if (!ends_with(lower, ".compare.json")) {
        if (ends_with(lower, ".json"))
            target.resize(target.size() - 5);
        target += ".compare.json";
    }
    SaveIfDue(true);
    Project copy = project_;
    copy.layout = base_layout_ == Layout::side_by_side ? "side-by-side" : "slider";
    copy.divider = divider_;
    copy.path_a = a_path_;
    copy.path_b = b_path_;
    copy.file = target;
    copy.areas_dirty = true;
    copy.logos.items_dirty = copy.logos.has_items;
    std::string error;
    if (SaveProject(copy, target, error))
        Notify("saved a copy: " + target, false);
    else
        Notify("not saved: " + error, true);
}

}  // namespace renderer::clips
