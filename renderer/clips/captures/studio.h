// The Captures tab of the compare tool (renderer/README.md "Captures"): a browser of the game recordings and captures, and
// for one capture a viewer of still images with the viewer's own views (divider, side by side, A only, B only,
// difference), exact pixel zoom, marks, and saving the composite as an image with the same overlay and logos as the video
// export. The generators (DLSS-G, ANR FG) that recreate a capture's frames run as jobs (jobs.h).
//
// The studio does not own the window: ClipViewer (or any host) gives it the device and the swap chain and calls it inside
// its frame loop.
//
//   host frame:  studio.Update();  ImGui::NewFrame();  studio.DrawBrowser() | studio.DrawWorkspace(...);  studio.ExportPrepare();
//                ImGui::Render();  studio.ExportRecord();  ... swap_.Record(... studio.Record(cb, queued, format) ...)

#pragma once

#include <array>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include "renderer/clips/captures/capture_info.h"
#include "renderer/clips/captures/dialogs.h"
#include "renderer/clips/captures/image_store.h"
#include "renderer/clips/captures/jobs.h"
#include "renderer/clips/captures/sources.h"
#include "renderer/clips/captures/still_pass.h"
#include "renderer/clips/captures/store.h"
#include "renderer/clips/captures/thumbnails.h"
#include "renderer/clips/gfx.h"
#include "renderer/clips/logo_layer.h"
#include "renderer/clips/overlay.h"
#include "renderer/clips/project.h"
#include "renderer/clips/swap_output.h"
#include "renderer/clips/view_math.h"
#include "renderer/modes.h"

struct SDL_Window;

namespace renderer::clips {
class OffscreenComposer;
}

namespace renderer::clips::captures {

struct StudioHost {
    Gfx* gfx = nullptr;
    SwapOutput* swap = nullptr;
    SDL_Window* window = nullptr;
    uint32_t queued_frames = 3;
    std::string workspace;
    std::function<double()> seconds;
    std::function<void(float& scroll_x, float& scroll_y, float& pinch)> scroll;  // a trackpad's scroll and pinch this frame
    std::function<void()> toggle_fullscreen;
    std::function<void()> quit;
    std::function<void(renderer::Mode)> switch_mode;       // the Mode menu's choice (the host asks about what would be lost)
    std::function<void(const std::string&, bool)> notify;  // a line for the host's notice line (bad: a failure)
    std::function<void(const std::string&)> capture;       // the window's pixels of the next frame to FILE.bin and FILE.json
};

// What a save writes and how.
struct SaveSettings {
    int size_mode = 1;            // 0 the capture's size (1x), 1 1080p, 2 half, 3 custom height
    int custom_height = 1440;
    bool outlines = false;        // the marks' rectangles on the full composite
    bool hdr_png = true;          // for HDR sources: 16-bit PQ PNG with a cICP chunk
    bool hdr_avif = false;        // for HDR sources: AVIF through ffmpeg (10-bit, BT.2020 PQ), when ffmpeg is there
    bool logos = true;
    std::string caption;          // under the picture (the crops use the mark's label)
};

class CaptureStudio {
public:
    CaptureStudio();
    ~CaptureStudio();

    bool Create(const StudioHost& host, std::string& error);
    // The GPU resources go before the device does.
    void Shutdown();

    // Once per frame before ImGui::NewFrame: jobs, thumbnails and images.
    void Update();
    // The Captures tab's contents (inside the start screen's window). Opens a capture when a card is chosen.
    void DrawBrowser();
    bool open() const { return capture_.has_value(); }
    void Open(const std::string& folder);
    void Close();
    // The studio's screen while a capture is open: menu bar, docked panels, the picture's overlay and input. Sets the central
    // view's rectangle (window pixels).
    void DrawWorkspace(std::array<int, 4>& view, uint32_t window_w, uint32_t window_h);
    // The picture inside the swap chain's rendering.
    void Record(nri::CommandBuffer& cb, uint32_t queued, nri::Format format);
    // A save in progress: Prepare inside the ImGui frame, Record after ImGui::Render (like exporter.h).
    void ExportPrepare();
    void ExportRecord();
    bool ExportBusy() const;
    // What leaving the Captures mode would end: a capture job (queued, waiting for a game or running) or a save of images.
    // Empty when nothing runs.
    std::string BusyReason() const;
    // Scripted actions for checks ("cap-open=NAME", "cap-pair=real:5:1", "cap-a=ID", "cap-b=ID", "cap-run=GENERATOR[:all]",
    // "cap-wait=jobs", "cap-view=slider|sbs|a|b|diff", "cap-zoom=Z", "cap-camera=CX:CY:Z", "cap-mark=X:Y:W:H",
    // "cap-save=FILE[:HEIGHT]", "cap-crops=FOLDER[:HEIGHT]", "cap-all=FOLDER[:HEIGHT]", "cap-shot=FILE", "cap-log", "cap-quit"). False: not one of ours.
    bool RunScript(const std::string& name, const std::string& value);
    // True while a scripted wait holds the script back (cap-wait=jobs).
    bool ScriptWaiting() const { return script_wait_; }
    // The scripted exports end: for `quit=export`-like waits.
    bool Idle() const;

private:
    struct SaveTask {
        enum Kind { FULL, CROP } kind = FULL;
        std::string path;         // the SDR PNG's file; the HDR files sit beside it (.hdr.png, .hdr.avif)
        int mark = -1;            // CROP: the mark's id
        int height = 0;
    };

    // Browser
    void Rescan();
    void DrawCard(const Entry& entry, float width);
    ImTextureData* ThumbTexture(const Entry& entry);
    // Opening
    void SelectPair(int index);
    void RebuildSources();
    void PickDefaults();
    void SetSource(int side, const std::string& id);
    void SwapSources();
    void LoadPairProject();
    void SaveIfDue(bool force);
    void MarkDirty();
    std::string StemOfPair() const;
    // Generators and jobs
    void RunGenerator(const Generator& generator, bool all);
    void RefreshResults();
    std::vector<GeneratedFrame> ResultsOfPair() const;
    // Geometry and input
    Rect ViewRect() const { return {(double)view_[0], (double)view_[1], (double)view_[2], (double)view_[3]}; }
    int PanelAt(double x, double y) const;
    Layout ShownLayout() const { return diff_ ? Layout::a_only : layout_; }
    void SetLayout(Layout layout);
    void HandleKeys();
    void HandleInput();
    void DrawOverlay();
    uint32_t ImageWidth() const;
    uint32_t ImageHeight() const;
    // UI panels
    void DrawMenuBar();
    void DrawCapturePanel();
    void DrawSourcesPanel();
    void DrawGeneratorsPanel();
    void DrawJobsPanel();
    void DrawMarksPanel();
    void DrawSavePanel();
    void DrawDisplayPanel();
    void DrawKeys();
    void DrawLogoSettings();
    bool LogosOn();
    std::vector<std::string> LogoFolders() const;
    void AddMark(const double rect[4]);
    void DeleteMark(int id);
    Area* SelectedMark();
    void ShowDialog(Dialogs::Kind kind, const std::string& location);
    void TakeDialog();
    void UpdateGenerators();
    // The buttons that add a generator (a DLL for recordings, the format's folder for others).
    void DrawAddGenerator();
    // Saving
    void QueueSave(const SaveTask& task);
    void QueueSaveAll(const std::string& folder, int height);
    void QueueCrops(const std::string& folder, int height);
    void PollWrites();
    int OutputHeight() const;
    bool PrepareTask(const SaveTask& task, bool hdr);
    void FinishTask();
    void WriteManifest(const std::string& folder, const std::vector<std::string>& files);
    void Notify(const std::string& text, bool bad);

    StudioHost host_;
    Locations where_;
    State state_;
    std::unique_ptr<ThumbnailLoader> thumbs_;
    std::unique_ptr<JobQueue> jobs_;
    Dialogs dialogs_;
    bool created_ = false;

    // Browser
    std::vector<Entry> entries_;
    bool scanned_ = false;
    double scanned_at_ = 0.0;
    std::string filter_text_, filter_game_ = "All games";
    int sort_ = 0;  // 0 newest first, 1 oldest first, 2 game
    std::map<std::string, ImTextureData*> thumb_textures_;
    std::string path_text_;       // a typed folder (where the system has no dialog)
    bool typing_ = false;
    std::string browser_note_;

    // The open capture
    std::optional<CaptureInfo> capture_;
    std::vector<PairSpec> pairs_;
    int pair_index_ = -1;
    std::vector<Generator> generators_;
    std::vector<Source> sources_;
    std::string a_id_, b_id_;
    bool picked_[2] = {false, false};  // the user chose this side (else it follows the defaults)
    SteamFiles steam_;
    std::map<std::string, std::vector<Cached>> cached_;  // generator id -> its frames in the cache
    uint64_t results_revision_ = ~0ull;
    bool diff_ = false;
    Layout layout_ = Layout::slider, base_layout_ = Layout::slider;
    double divider_ = 0.5;
    Camera camera_;
    bool show_marks_ = true, mark_mode_ = false;
    int selected_mark_ = -1;
    struct Drag {
        enum Kind { NONE, DIVIDER, PAN, RECT } kind = NONE;
        int panel = 0;
        double u0 = 0, v0 = 0;
        double rect[4] = {0, 0, 0, 0};
    } drag_;
    std::array<int, 4> view_ = {0, 0, 0, 0};
    CompareLayout current_layout_;
    Mapping mapping_[2];
    Project project_;
    std::string project_path_;
    bool dirty_ = false;
    double dirty_since_ = 0.0;
    LogoLayer logo_layer_;
    double logos_checked_ = -1e9;
    bool logos_auto_ = false;
    std::string source_note_[2];
    std::string last_error_;

    // Pictures
    ImageStore images_;
    StillPass pass_;
    StillPass export_pass_;
    std::string key_a_, key_b_;
    const GpuImage* image_[2] = {nullptr, nullptr};
    ImageStore::State image_state_[2] = {ImageStore::State::LOADING, ImageStore::State::LOADING};
    std::string image_error_[2];

    // Panels
    bool reset_layout_ = false, ui_shown_ = false;
    int layout_focus_ = 0;
    bool show_ui_ = true;
    int panel_tab_ = 0;
    char path_buffer_[1024] = {0};
    std::string generators_note_;

    // Saving
    SaveSettings save_;
    std::vector<SaveTask> tasks_;
    std::string batch_folder_;
    std::vector<std::string> batch_files_;
    bool task_prepared_ = false;
    SaveTask current_task_;
    int current_pass_ = 0;        // 0 the SDR image, 1 the HDR image
    std::unique_ptr<OffscreenComposer> composer_[2];  // SDR (RGBA8) and HDR (RGBA16 PQ), kept between the tasks
    uint32_t composer_w_[2] = {0, 0}, composer_h_[2] = {0, 0};
    LogoLayer export_logos_;
    std::vector<IntRect> pills_;
    std::string save_status_;
    std::vector<std::future<std::string>> writes_;  // the writer threads' results
    bool folder_crops_only_ = false;
    struct Prepared {
        Layout layout = Layout::slider;
        Camera camera;
        double divider = 0.5;
        uint32_t w = 0, h = 0;
        bool hdr = false;
    } prepared_;
    double save_started_ = 0.0;
    std::map<int, double> save_seconds_;
    std::vector<std::string> save_log_;
    // Scripts
    bool script_wait_ = false;
    int script_wait_job_ = 0;
    std::vector<std::pair<std::string, std::string>> script_queue_;
};

}  // namespace renderer::clips::captures
