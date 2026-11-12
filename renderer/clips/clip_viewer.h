// The clip viewer: two recordings of one run side by side in the renderer's window, with a divider, exact pixel zoom, areas
// to zoom to and the offline export (renderer/README.md "Compare mode"). A separate renderer::App from Renderer: it loads
// no scene, models or DLSS, and builds its window, device and swap chain from the same pieces (renderer/app.h,
// renderer/display.h, clips/gfx.h, clips/swap_output.h).
//
//   bazel run //renderer -- --compare [--project FILE]
//
// The recordings are chosen in the window (native file dialogs, drag and drop, the Recent list) and never on the command
// line; --project FILE opens a project, which holds its recordings' paths (for scripts and tests).

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "renderer/app.h"
#include "renderer/modes.h"
#include "renderer/clips/captures/studio.h"
#include "renderer/clips/camera.h"
#include "renderer/clips/edit_list.h"
#include "renderer/clips/file_dialog.h"
#include "renderer/clips/gfx.h"
#include "renderer/clips/logo_layer.h"
#include "renderer/clips/mark_runner.h"
#include "renderer/clips/mark_visibility.h"
#include "renderer/clips/overlay.h"
#include "renderer/clips/pair_pick.h"
#include "renderer/clips/pair_registry.h"
#include "renderer/clips/player.h"
#include "renderer/clips/probe.h"
#include "renderer/clips/project.h"
#include "renderer/clips/swap_output.h"
#include "renderer/clips/sync_job.h"
#include "renderer/clips/video_pass.h"
#include "renderer/clips/view_math.h"

namespace renderer::clips {

class Exporter;
struct ExportSettings;

class ClipViewer : public renderer::App {
public:
    ClipViewer();
    ~ClipViewer() override;

    void InitCmdLine(cmdline::parser& cmd) override;
    void ReadCmdLine(cmdline::parser& cmd) override;
    bool Initialize(nri::GraphicsAPI api, bool) override;
    void LatencySleep(uint32_t frame_index) override;
    void PrepareFrame(uint32_t frame_index) override;
    void RenderFrame(uint32_t frame_index) override;
    bool AppShouldClose() override { return close_; }
    void OnEvent(const SDL_Event& event) override;

private:
    struct QueuedFrame {
        nri::CommandAllocator* allocator = nullptr;
        nri::CommandBuffer* command_buffer = nullptr;
        std::shared_ptr<const Pair> pair;  // the frames this GPU frame reads, pinned until it retires
    };

    // Setup
    bool EnsureCaptures();             // the Captures tab's studio, made when it is first needed
    void StartSync(bool local);
    // The offset between the recordings (offset_map.h): K at the playhead, set as a constant (`ApplyOffset`), nudged where
    // the playhead is (`NudgeOffset`: the nearest anchor, or a new one, when the drift is followed, else the constant), or
    // the whole map set (`ApplyOffsets`). Each updates the project and the player.
    void ApplyOffset(int64_t k, bool from_detection);
    void ApplyOffsets(const OffsetMap& offsets, bool from_detection);
    void NudgeOffset(int64_t delta);
    void ApplyLocalWindow(const SyncWindow& window);  // a detection around the playhead
    void SetOffsetHere(int64_t k);
    void SetFollowDrift(bool follow);
    void AddAnchorHere();
    void RemoveAnchorHere();
    void ClearAnchors();
    int64_t OffsetHere() const;  // K at the playhead
    void DrawOffsetPlot(float height);

    // The recordings (clip_session.cpp): choosing, opening, changing, swapping, the recent pairs, dialogs and drops. One
    // request is open at a time; every way to give the viewer a recording ends in ChooseVideo, OpenProjectFile or
    // OpenRecent, whether it came from a dialog, a drop, a typed path or a script.
    struct OpenRequest {
        std::string a, b;                 // the recordings (paths)
        std::optional<Project> project;   // a project that was chosen (its own file and paths)
        ProbeResult probe[2];
        bool have_probe[2] = {false, false};
        std::string changed;              // "A" or "B": the recording just chosen (for the message of a refusal)
    };
    bool OpenPair(OpenRequest& request, std::string& error);   // the whole change, or nothing: the old pair stays on an error
    void RequestOpen(OpenRequest request);                     // done in a later frame, after one that says "opening"
    void ReleasePlayer();                                      // the player and what refers to its frames, after the GPU is idle
    void ResetPairState();
    void ChooseVideo(int side, const std::string& path);       // a recording for A or B: picks it, or changes the pair's
    void OpenProjectFile(const std::string& path);
    void OpenRecent(const RecentPair& entry);
    void SwapRecordings();                                     // Swap A and B (SwapSides on the project, the sources swap too)
    void SaveProjectCopy(const std::string& path);
    void ShowDialog(FileDialog::Kind kind);
    void HandleDialogResult(const FileDialog::Result& result);
    void HandleDrop(const DropDecision& decision);
    void HandleEvents();                                       // the queued dialog results and the open request
    bool ExportRunning(const char* what);                      // an export runs: say so and refuse `what`
    void ChoosePair(const std::string& a, const std::string& b);  // two recordings at once (a drop of two files)
    void Notify(const std::string& text, bool bad);
    void SaveRegistry();
    void RememberFolder(const std::string& file, bool project);
    std::string DialogFolder(bool project) const;
    std::string CompareDir() const;
    bool StartupOpen(const std::string& project_file, std::string& error);
    void UpdateWindowTitle();
    // Modes (clip_pick.cpp): this window is the compare mode, or the captures mode on the start screen's Captures tab and in an
    // open capture. The Mode menu switches between the start screen's two tabs in place; to any other mode, or from an open
    // pair or capture, the process starts again as that mode (renderer/mode_switch.h).
    renderer::Mode CurrentMode() const;
    bool SwitchesInPlace(renderer::Mode target) const;
    std::string LosesOnSwitch(renderer::Mode target) const;  // what a switch ends that is not saved, or empty
    void SwitchMode(renderer::Mode target);

    // The empty start and the file actions (clip_pick.cpp)
    void DrawPicker();
    void DrawRecent(bool compact);
    void DrawFileMenu();
    void DrawFileButtons();
    void DrawDropOverlay();
    void DrawNotice();
    void DrawCard(int side, float width, float height);
    bool PathEntry(const char* id, std::string& text, const char* button);

    // Per frame
    void HandleResize();
    void ApplyUiScale();
    void SignalFrame(uint32_t frame_index);
    void RecordVideo(nri::CommandBuffer& cb, uint32_t queued, nri::Format format);
    void PollJobs();
    void SaveIfDue(bool force);
    void MarkDirty();

    // UI (clip_ui.cpp)
    void DrawUi();
    void DrawMenuBar();
    void DrawClipsPanel();
    void DrawAreasPanel();
    void DrawExportPanel();
    void DrawDisplayPanel();
    void DrawTransport();
    void DrawKeys();
    void DrawTimeline(float height);
    void BuildDockLayout(ImGuiID dock);
    void HandleVideoInput();
    void HandleKeys();
    void DrawVideoOverlay();

    // The picture's geometry in the central view
    CompareLayout CurrentLayout() const;
    Rect ViewRect() const;
    int PanelAt(double x, double y) const;
    // What is shown: a running freeze sequence's shot, else the preview's, else the viewer's own camera and divider.
    Camera CurrentCamera() const { return mark_state_.active ? mark_state_.shot.camera : preview_ ? preview_camera_ : camera_; }
    double CurrentDivider() const { return mark_state_.active ? mark_state_.shot.divider : preview_ ? shot_.divider : divider_; }
    std::string CurrentCaption() const { return mark_state_.active ? mark_state_.shot.caption : preview_ ? shot_.caption : ""; }
    Shot LiveShot() const { return {preview_ ? preview_camera_ : camera_, preview_ ? shot_.divider : divider_, preview_ ? shot_.caption : ""}; }
    double Fps() const { return fps_; }
    double TimeOf(int64_t frame) const { return (double)frame / fps_; }
    void Seek(int64_t frame);
    void SetLayout(Layout layout);
    void AddArea(const double rect[4]);
    void DeleteArea(int id);
    Area* SelectedArea();
    void StartExport();
    // Still images of the pair at the playhead, the slider's and the side by side's (PNG beside the video's file): queued,
    // and each started when the export before it has ended (PumpImages, each frame).
    void StartImages();
    void PumpImages();

    // Marks: the edit list of the viewer's range (the overlap), the freeze runner and the previews
    EditOptions MarkOptions() const;
    void UpdateMarks();
    void TogglePreview();                 // Z: the selected freeze mark's sequence, else the zoom preview of the spans
    void PreviewMark(const Area& area);
    void SetMarkKind(Area& area, AreaKind kind);
    int64_t MarkFrame(const Area& area) const { return (int64_t)std::llround(area.t0 * fps_); }
    void SetMarkFrame(Area& area, int64_t frame);
    std::string MarkStatus() const;       // "mark 2: zooming", empty when none runs
    bool InOverlap(const Area& area) const;
    double MarkAlpha(const Area& area) const;   // how much of its rectangle shows now (mark_visibility.h)

    // Logos (the live view draws what the export draws)
    bool LogosOn();
    void SetLogosOn(bool on);
    std::vector<std::string> LogoFolders() const;
    void DrawLogoSettings();

    // Device
    Gfx gfx_;
    SwapOutput swap_;
    VideoPass pass_;
    nri::Streamer* streamer_ = nullptr;
    nri::Fence* frame_fence_ = nullptr;
    std::vector<QueuedFrame> queued_frames_;
    SwapOptions swap_options_;

    // Recordings
    bool loaded_ = false;   // a pair is open: the player, sources and project below are set
    std::string a_path_, b_path_, project_path_;
    std::unique_ptr<VideoSource> source_[2];
    ProbeResult probe_[2];
    std::string source_notes_[2];
    std::string gpu_decode_note_[2];
    std::unique_ptr<Player> player_;
    Project project_;
    bool project_created_ = false;
    double fps_ = 60.0;
    VideoSourceParams params_[2] = {{1920, 1080}, {1920, 1080}};

    // Options
    uint64_t cache_mib_ = 1536;
    bool force_ffmpeg_ = false;
    bool autoplay_ = false;
    bool hide_ui_ = false;
    int check_pairs_ = 0;
    double check_started_ = 0.0;
    int64_t start_frame_ = -1;
    std::string layout_option_;
    // --capture FILE [--capture-frame N]: the window's pixels of one frame as raw words and a JSON note, then quit
    std::string capture_path_;
    int capture_frame_ = 40;
    bool capture_quits_ = true;  // --capture ends the run after its frame; the script's capture=FILE does not
    uint32_t script_delay_ = 0;  // frames the script waited (wait=idle)
    bool script_wait_ = false;
    double script_wait_until_ = 0.0;
    std::string script_;
    bool quit_after_export_ = false;
    std::string focus_tab_;  // --tab: the panel tab to bring to the front at start
    bool want_captures_tab_ = false;                   // the start screen shows the Captures tab next frame
    bool want_videos_tab_ = false;                     // the start screen shows the Videos tab next frame
    bool on_captures_tab_ = false;                     // the start screen's Captures tab was the one showing
    bool start_captures_ = false;                      // --captures: the start screen opens on the Captures tab
    std::unique_ptr<captures::CaptureStudio> captures_;  // the Captures tab (renderer/clips/captures)
    std::vector<std::pair<uint32_t, std::string>> script_actions_;
    bool scripted_mouse_ = false;
    ImVec2 scripted_pos_ = ImVec2(0.0f, 0.0f);
    std::vector<std::pair<uint32_t, ImGuiKey>> script_releases_;  // scripted key presses to end
    void RunScript(uint32_t app_frame);
    void PushDrop(const std::vector<std::string>& files, double x, bool complete);
    std::deque<std::string> pushed_drop_paths_;  // what a scripted drop's events point to, until the next frame

    // View state
    Layout layout_ = Layout::slider;
    Layout base_layout_ = Layout::slider;  // slider or side by side: what the project keeps
    double divider_ = 0.5;
    Camera camera_;
    Camera preview_camera_;
    bool preview_ = false;
    bool mark_mode_ = false;
    int redraw_area_ = -1;
    int selected_area_ = -1;
    Shot shot_;  // the preview's shot at the playhead
    std::unique_ptr<EditList> edit_;        // the marks over the overlap: which freezes run, in which order
    MarkRunner mark_run_;
    MarkRunner::State mark_state_;          // a freeze sequence on screen
    bool zoom_marks_ = true;                // "Zoom marks while playing"
    MarkShowSettings marks_shown_;          // "Show marks": near the playhead (default), always or never; a window setting
    LogoLayer logo_layer_;
    double logos_checked_ = -1e9;           // when the auto state (do the files exist) was last looked up
    bool logos_auto_ = false;
    struct Drag {
        enum Kind { NONE, DIVIDER, PAN, RECT } kind = NONE;
        int panel = 0;
        double u0 = 0, v0 = 0;
        double rect[4] = {0, 0, 0, 0};
    } drag_;
    std::array<int, 4> view_ = {0, 0, 0, 0};  // the central node in window pixels
    Mapping mapping_[2];
    CompareLayout current_layout_;
    Player::Tick tick_;
    std::string area_label_buffer_;

    // Choosing the recordings (clip_session.cpp, clip_pick.cpp)
    PairRegistry registry_;
    std::string registry_path_, compare_dir_;
    FileDialog dialogs_;
    struct Pick {
        std::string path;
        ProbeResult probe;
    };
    Pick pick_[2];                      // the empty start's choices, until both are there
    std::optional<OpenRequest> open_request_;
    int open_request_age_ = 0;          // frames since it was made: it runs after one has shown "opening"
    struct Notice {
        std::string text;
        bool bad = false;
        double time = 0.0;
    } notice_;
    bool dialogs_failed_ = false;       // the system has no file dialog
    bool typing_ = false;               // path fields show: the dialogs failed, or the user asked
    std::string typed_[3];              // the path fields: A, B, project
    DropCollector drop_;
    std::vector<DropDecision> pending_drops_;  // decided in the event loop, handled in the frame
    std::string startup_error_;         // a command line the viewer refuses
    bool seek_to_overlap_ = false;      // a new pair: when its detection ends, a paused viewer goes to the overlap's start
    bool startup_open_ = false;         // the first open: the command line's view, start frame and play apply to it

    // Project, saving
    double dirty_since_ = 0.0;
    bool dirty_ = false;
    std::string save_note_;
    double save_note_time_ = 0.0;

    // Sync
    SyncJob sync_;
    std::string sync_line_;
    std::optional<SyncWindow> local_window_;
    std::string local_note_;

    // Export
    std::unique_ptr<Exporter> exporter_;
    std::unique_ptr<ExportSettings> export_settings_;
    std::string export_error_;
    std::vector<ExportSettings> image_queue_;
    std::string image_running_;                // the image the exporter is writing, "" when it writes none
    std::vector<std::string> images_written_;

    // UI
    bool show_ui_ = true, reset_layout_ = false, ui_shown_ = false;
    int layout_focus_ = 0;
    float ui_scale_ = 1.0f;
    bool ui_scale_auto_ = true;
    ImGuiStyle base_style_;
    ImFont* mono_ = nullptr;
    std::string imgui_ini_;
    bool fullscreen_ = false;
    bool close_ = false;
    double refresh_hz_ = 60.0;
    std::chrono::steady_clock::time_point next_frame_;
    double frame_ms_ = 0.0;
    double last_frame_time_ = 0.0;
};

}  // namespace renderer::clips
