// The clip viewer's UI: the docked panels (Clips, Areas, Export, Display, Transport), the menu bar and status, the keys and
// the mouse on the picture. Written in the renderer's docked ImGui style.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>
#include "renderer/clips/clip_viewer.h"
#include "renderer/clips/encoders.h"
#include "renderer/clips/exporter.h"
#include "renderer/dock_layout.h"
#include "renderer/display.h"
#include "renderer/mode_switch.h"
#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace fs = std::filesystem;

namespace {

std::string FormatTime(double seconds) {
    if (seconds < 0.0)
        seconds = 0.0;
    const int minutes = (int)(seconds / 60.0);
    char text[48];
    std::snprintf(text, sizeof(text), "%d:%05.2f", minutes, seconds - 60.0 * minutes);
    return text;
}

bool TextInput(const char* label, std::string& value, float width = 0.0f) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
    if (width > 0.0f)
        ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(label, buffer, sizeof(buffer))) {
        value = buffer;
        return true;
    }
    return false;
}

const ImVec4 WARNING(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 GOOD(0.55f, 0.9f, 0.6f, 1.0f);
const ImVec4 BAD(1.0f, 0.45f, 0.4f, 1.0f);

}  // namespace

// --- Geometry ---------------------------------------------------------------------------------------------------------

Rect ClipViewer::ViewRect() const {
    return {(double)view_[0], (double)view_[1], (double)view_[2], (double)view_[3]};
}

CompareLayout ClipViewer::CurrentLayout() const {
    return compare_layout(layout_, ViewRect(), CurrentDivider());
}

int ClipViewer::PanelAt(double x, double y) const {
    const Rect v = ViewRect();
    if (x < v.x || y < v.y || x >= v.x + v.w || y >= v.y + v.h)
        return -1;
    if (layout_ == Layout::side_by_side) {
        for (int i = 1; i >= 0; i--) {
            const Rect& a = current_layout_.area[i];
            if (x >= a.x && x < a.x + a.w)
                return i;
        }
        return x < v.x + v.w / 2.0 ? 0 : 1;
    }
    return layout_ == Layout::b_only ? 1 : 0;
}

void ClipViewer::Seek(int64_t frame) {
    player_->clock().Seek(frame);
}

void ClipViewer::SetLayout(Layout layout) {
    layout_ = layout;
    if (layout == Layout::slider || layout == Layout::side_by_side) {
        base_layout_ = layout;
        MarkDirty();
    }
}

Area* ClipViewer::SelectedArea() {
    for (Area& a : project_.areas)
        if (a.id == selected_area_)
            return &a;
    return nullptr;
}

void ClipViewer::AddArea(const double rect[4]) {
    Area a;
    int id = 0;
    for (const Area& other : project_.areas)
        id = std::max(id, other.id);
    a.id = id + 1;
    a.label = "Area " + std::to_string(a.id);
    // A mark is made at once, at the current frame: a freeze (the video stops there and zooms) by default, or a span that
    // zooms while the video plays for the project's hold.
    a.kind = project_.new_mark_kind;
    a.kind_stored = true;
    a.t0 = TimeOf(player_->clock().position());
    a.t1 = a.kind == AreaKind::FREEZE ? a.t0 : std::min(a.t0 + project_.hold, (double)source_[0]->Info().frames / fps_);
    for (int i = 0; i < 4; i++)
        a.rect[i] = rect[i];
    project_.areas.push_back(a);
    project_.areas_dirty = true;
    selected_area_ = a.id;
    MarkDirty();
}

void ClipViewer::DeleteArea(int id) {
    project_.areas.erase(std::remove_if(project_.areas.begin(), project_.areas.end(), [&](const Area& a) { return a.id == id; }),
                         project_.areas.end());
    if (selected_area_ == id)
        selected_area_ = -1;
    project_.areas_dirty = true;
    MarkDirty();
}

// --- Marks ------------------------------------------------------------------------------------------------------------

EditOptions ClipViewer::MarkOptions() const {
    EditOptions o;
    o.fps = fps_;
    const Timeline& t = player_->clock().timeline();
    if (t.has_overlap()) {  // a freeze needs the pair of both recordings
        o.first = t.overlap_first();
        o.end = t.overlap_end();
    }
    o.ease = project_.ease;
    o.freeze_hold = project_.freeze_hold;
    o.divider = divider_;
    return o;
}

void ClipViewer::UpdateMarks() {
    edit_ = std::make_unique<EditList>(project_.areas, MarkOptions());
}

bool ClipViewer::InOverlap(const Area& area) const {
    const Timeline& t = player_->clock().timeline();
    const int64_t frame = MarkFrame(area);
    return t.has_overlap() && frame >= t.overlap_first() && frame < t.overlap_end();
}

// The rectangle of a mark shows near the playhead (mark_visibility.h), whole during its own freeze sequence and while it
// is being redrawn; "Never" shows none, "Always" all. The timeline and the Areas list are not affected.
double ClipViewer::MarkAlpha(const Area& area) const {
    if (marks_shown_.show == MarkShow::NEVER)
        return 0.0;
    if (marks_shown_.show == MarkShow::NEAR_PLAYHEAD) {
        if (mark_state_.active && mark_state_.area_id == area.id)
            return 1.0;
        if (mark_mode_ && redraw_area_ == area.id)
            return 1.0;
    }
    const double sequence = area.kind == AreaKind::FREEZE ? (double)make_freeze(area, MarkOptions()).length() / fps_ : 0.0;
    return mark_alpha(marks_shown_, TimeOf(player_->clock().position()), area.kind, area.t0, area.t1, sequence);
}

void ClipViewer::PreviewMark(const Area& area) {
    if (area.kind != AreaKind::FREEZE)
        return;
    mark_run_.StartPreview(make_freeze(area, MarkOptions()), player_->clock());
    selected_area_ = area.id;
}

// Z: with a freeze mark selected, its sequence from its start; else the zoom preview of the span marks.
void ClipViewer::TogglePreview() {
    if (preview_) {  // the zoom preview of the spans is on: Z turns it off
        preview_ = false;
        camera_ = Camera();
        return;
    }
    const Area* area = SelectedArea();
    if (area && area->kind == AreaKind::FREEZE)
        PreviewMark(*area);
    else
        preview_ = true;
}

void ClipViewer::SetMarkKind(Area& area, AreaKind kind) {
    if (area.kind == kind && area.kind_stored)
        return;
    const double duration = (double)source_[0]->Info().frames / fps_;
    area.kind = kind;
    area.kind_stored = true;
    if (kind == AreaKind::FREEZE) {
        area.t0 = TimeOf(MarkFrame(area));  // the frame's own time
        area.t1 = area.t0;
    } else {
        area.t1 = std::min(area.t0 + project_.hold, duration);
    }
    project_.areas_dirty = true;
    MarkDirty();
}

void ClipViewer::SetMarkFrame(Area& area, int64_t frame) {
    frame = std::clamp<int64_t>(frame, 0, std::max<int64_t>(source_[0]->Info().frames - 1, 0));
    area.t0 = area.t1 = TimeOf(frame);
    project_.areas_dirty = true;
    MarkDirty();
}

std::string ClipViewer::MarkStatus() const {
    if (!mark_state_.active)
        return {};
    const char* phase = mark_state_.phase == FreezePhase::ZOOMING_IN ? "zooming"
                        : mark_state_.phase == FreezePhase::HOLDING  ? "holding"
                                                                     : "returning";
    return "mark " + std::to_string(mark_state_.area_id) + ": " + phase + (mark_state_.preview ? " (preview)" : "");
}

// --- Logos ------------------------------------------------------------------------------------------------------------

std::vector<std::string> ClipViewer::LogoFolders() const {
    return logo_folders(fs::path(project_path_).parent_path().string());
}

// The project's choice; unset, on when a logo file exists.
bool ClipViewer::LogosOn() {
    if (project_.logos.enabled)
        return *project_.logos.enabled;
    if (Seconds() - logos_checked_ > 2.0) {
        logos_checked_ = Seconds();
        logos_auto_ = false;
        const std::vector<std::string> folders = LogoFolders();
        for (const LogoItem& item : project_.logos.effective())
            logos_auto_ = logos_auto_ || !find_logo_file(item.path, folders).empty();
        static bool said = false;
        if (!logos_auto_ && !said) {
            said = true;
            std::string where;
            for (const std::string& f : folders)
                where += (where.empty() ? "" : ", ") + f;
            spdlog::info("logos off: no logo file found (looked in {})", where);
        }
    }
    return logos_auto_;
}

void ClipViewer::SetLogosOn(bool on) {
    project_.logos.enabled = on;
    project_.logos.stored = true;
    MarkDirty();
}

// The Export panel's logos: the master box and each logo's file, corner, height, margin, opacity and backdrop.
void ClipViewer::DrawLogoSettings() {
    bool on = LogosOn();
    if (ImGui::Checkbox("Logos", &on))
        SetLogosOn(on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Logos at the output's corners, in the export and (to judge them) on the view.\n"
                          "Apex bottom-left (A, left of the divider), NVIDIA bottom-right (B, right of it).\n"
                          "Files (apex.png, nvidia.png): %s", LogoFolderText().c_str());
    ImGui::SameLine();
    if (ImGui::TreeNode("##logo-settings", "settings")) {
        const std::vector<LogoItem> items = project_.logos.effective();
        const char* corners[] = {"bottom-left", "bottom-right", "top-left", "top-right"};
        for (size_t i = 0; i < items.size(); i++) {
            LogoItem item = items[i];
            bool changed = false;
            ImGui::PushID((int)i);
            ImGui::Text("%s", item.id.empty() ? "logo" : item.id.c_str());
            std::string path = item.path;
            ImGui::SameLine();
            if (TextInput("file", path, ImGui::GetFontSize() * 14.0f)) {
                item.path = path;
                changed = true;
            }
            int corner = (int)item.corner;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
            if (ImGui::Combo("corner", &corner, corners, 4)) {
                item.corner = (Corner)corner;
                changed = true;
            }
            float height = (float)item.height, margin = (float)item.margin, opacity = (float)item.opacity;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            if (ImGui::SliderFloat("height", &height, 0.01f, 0.2f, "%.3f of the frame")) {
                item.height = height;
                changed = true;
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            if (ImGui::SliderFloat("margin", &margin, 0.0f, 0.1f, "%.3f of the frame")) {
                item.margin = margin;
                changed = true;
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            if (ImGui::SliderFloat("opacity", &opacity, 0.0f, 1.0f, "%.2f")) {
                item.opacity = opacity;
                changed = true;
            }
            if (ImGui::Checkbox("dark backdrop", &item.backdrop))
                changed = true;
            if (changed) {
                project_.logos.edit()[i] = item;
                MarkDirty();
            }
            ImGui::PopID();
            ImGui::Separator();
        }
        for (const std::string& problem : logo_layer_.problems())
            ImGui::TextColored(WARNING, "%s", problem.c_str());
        ImGui::TreePop();
    }
}

// --- Frame ------------------------------------------------------------------------------------------------------------

void ClipViewer::BuildDockLayout(ImGuiID dock) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float panel = std::clamp(380.0f * ui_scale_ / std::max(vp->WorkSize.x, 1.0f), 0.18f, 0.35f);
    const float bottom = std::clamp(150.0f * ui_scale_ / std::max(vp->WorkSize.y, 1.0f), 0.12f, 0.3f);
    const char* right[] = {"Clips", "Areas", "Export", "Display"};
    const char* below[] = {"Transport"};
    renderer::BuildDockLayout(dock, panel, right, 4, below, 1, bottom);
    layout_focus_ = 3;
    spdlog::info("UI: the default dock layout (saved layouts are in {})", imgui_ini_);
}

void ClipViewer::DrawUi() {
    LogoLayer::CollectRetired();
    ImGui::NewFrame();
    const uint2 window = GetOutputResolution();
    if (captures_ && captures_->open()) {  // a capture is open: the Captures tab's own screen (captures/studio.h)
        captures_->DrawWorkspace(view_, window.x, window.y);
        DrawNotice();
        if (const auto mode = renderer::DrawModeConfirm())
            SwitchMode(*mode);
        captures_->ExportPrepare();
        ImGui::EndFrame();
        ImGui::Render();
        captures_->ExportRecord();
        return;
    }
    if (show_ui_) {
        DrawMenuBar();
        const ImGuiID dock = ImGui::GetID("NRClipsDock");
        if (reset_layout_ || !renderer::DockLayoutExists(dock))
            BuildDockLayout(dock);
        reset_layout_ = false;
        ImGui::DockSpaceOverViewport(dock, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
        if (layout_focus_ == 1)
            ImGui::SetNextWindowFocus();  // Clips, the first window
        DrawClipsPanel();
        DrawAreasPanel();
        DrawExportPanel();
        DrawDisplayPanel();
        if (layout_focus_ == 2)
            ImGui::SetNextWindowFocus();
        DrawTransport();
        // Windows take the focus as they appear, and a focused docked window's tab goes in front: without this the last
        // one drawn would cover the saved layout's tab
        if (!ui_shown_)
            ImGui::SetWindowFocus(nullptr);
        if (!focus_tab_.empty() && layout_focus_ == 0 && ui_shown_) {
            ImGui::SetWindowFocus(focus_tab_.c_str());
            focus_tab_.clear();
        }
        ui_shown_ = true;
        if (layout_focus_ > 0)
            layout_focus_--;
        ImVec2 pos, size;
        if (renderer::DockCentralNode(dock, pos, size) && size.x >= 64.0f && size.y >= 64.0f)
            view_ = {(int)pos.x, (int)pos.y, (int)size.x, (int)size.y};
        else
            view_ = {0, 0, (int)window.x, (int)window.y};
    } else {
        view_ = {0, 0, (int)window.x, (int)window.y};
        ui_shown_ = false;
    }
    if (loaded_) {
        // The geometry of this frame: the layout in the view and the camera's mapping into each area.
        current_layout_ = CurrentLayout();
        for (int i = 0; i < 2; i++) {
            const Rect& area = current_layout_.area[i];
            mapping_[i] = map_camera(area, (int)params_[0].width, (int)params_[0].height, CurrentCamera(),
                                     fit_scale(area, (int)params_[0].width, (int)params_[0].height));
        }
        HandleKeys();
        HandleVideoInput();
        DrawVideoOverlay();
    } else {
        HandleKeys();
        DrawPicker();  // the empty start: the two cards
    }
    DrawNotice();
    if (const auto mode = renderer::DrawModeConfirm())
        SwitchMode(*mode);
    DrawDropOverlay();
    PumpImages();
    if (exporter_ && exporter_->running())
        exporter_->Prepare();  // the export's overlay text, while the font atlas is current
    ImGui::EndFrame();
    ImGui::Render();
    if (exporter_ && exporter_->running())
        exporter_->Record();
}

void ClipViewer::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar())
        return;
    if (const auto mode = renderer::DrawModeMenu(CurrentMode(), [this](renderer::Mode target) { return LosesOnSwitch(target); }))
        SwitchMode(*mode);
    DrawFileMenu();
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Divider", "1", layout_ == Layout::slider))
            SetLayout(Layout::slider);
        if (ImGui::MenuItem("Side by side", "2", layout_ == Layout::side_by_side))
            SetLayout(Layout::side_by_side);
        if (ImGui::MenuItem("A only", "3", layout_ == Layout::a_only))
            SetLayout(Layout::a_only);
        if (ImGui::MenuItem("B only", "4", layout_ == Layout::b_only))
            SetLayout(Layout::b_only);
        ImGui::Separator();
        if (ImGui::MenuItem("Preview (selected freeze mark, else the zoom of the spans)", "Z", preview_))
            TogglePreview();
        ImGui::MenuItem("Zoom marks while playing", nullptr, &zoom_marks_);
        if (ImGui::MenuItem("Reset zoom", "0"))
            camera_ = Camera(), preview_ = false;
        ImGui::Separator();
        if (ImGui::MenuItem("Fullscreen", "F11", fullscreen_)) {
            fullscreen_ = !fullscreen_;
            SetFullscreen(fullscreen_);
        }
        if (ImGui::MenuItem("Hide the UI", "F1"))
            show_ui_ = false;
        if (ImGui::MenuItem("Reset layout"))
            reset_layout_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Keys")) {
        DrawKeys();
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (!loaded_) {
        ImGui::TextDisabled("no recordings");
        ImGui::EndMainMenuBar();
        return;
    }
    const Lockstep& clock = player_->clock();
    const double time = TimeOf(clock.position());
    ImGui::Text("%s", FormatTime(time).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("frame %lld", (long long)clock.position());
    ImGui::SameLine();
    if (tick_.buffering)
        ImGui::TextColored(WARNING, "buffering");
    else
        ImGui::TextDisabled("%s", clock.playing() ? "playing" : "paused");
    if (mark_state_.active) {
        ImGui::SameLine();
        ImGui::TextColored(GOOD, "%s", MarkStatus().c_str());
    }
    if (clock.decode_limited()) {
        ImGui::SameLine();
        ImGui::TextColored(WARNING, "decode-limited %.2fx", clock.rate());
    } else if (clock.playing() && std::fabs(clock.speed() - 1.0) > 1e-9) {
        ImGui::SameLine();
        ImGui::TextDisabled("%.2fx", clock.speed());
    }
    if (swap_.encoding() != SRGB8) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s, paper white %.0f, peak %.0f nits", swap_.encoding() == PQ10 ? "HDR10" : "scRGB",
                            swap_.hdr.paper_white, swap_.hdr.peak);
    }
    if (Seconds() - save_note_time_ < 3.0 && !save_note_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(save_note_.rfind("not", 0) == 0 ? BAD : GOOD, "%s", save_note_.c_str());
    }
    ImGui::EndMainMenuBar();
}

void ClipViewer::DrawKeys() {
    const char* keys[][2] = {{"Space", "play or pause"},
                             {"Left, Right", "one frame back or forward (Shift: one second); pauses"},
                             {", .", "offset: B one frame earlier or later (Shift: 10); with the drift followed, at the playhead"},
                             {"1 2 3 4", "divider, side by side, A only, B only"},
                             {"Tab", "switch between A and B (single views)"},
                             {"[ ]", "move the divider by 16 pixels"},
                             {"wheel, + -", "zoom around the pointer (1x to 16x)"},
                             {"0", "back to the whole frame"},
                             {"left drag", "move the divider (divider view), pan (other views)"},
                             {"right or middle drag", "pan in every view"},
                             {"R", "mark an area: drag a rectangle on the video (a freeze mark at the current frame)"},
                             {"Z", "preview: the selected freeze mark's sequence, else the zoom of the span marks"},
                             {"Delete", "delete the selected area"},
                             {"Home, End", "start, end of the overlap"},
                             {"drop files", "one video: the left half of the window sets A, the right half B; two: A and B; a .compare.json opens a project"},
                             {"F1", "hide or show the UI"},
                             {"F11", "fullscreen"},
                             {"Esc", "cancel marking, else quit"}};
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (auto& k : keys) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k[0]);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k[1]);
        }
        ImGui::EndTable();
    }
}

// --- Panels -----------------------------------------------------------------------------------------------------------

void ClipViewer::DrawClipsPanel() {
    if (!ImGui::Begin("Clips")) {
        ImGui::End();
        return;
    }
    DrawFileButtons();
    ImGui::Separator();
    if (!loaded_) {
        ImGui::TextWrapped("No recordings yet. Choose A and B in the middle of the window, or drop the files on it.");
        ImGui::TextUnformatted("Recent");
        DrawRecent(true);
        ImGui::End();
        return;
    }
    if (!project_.title.empty())
        ImGui::TextWrapped("%s", project_.title.c_str());
    ImGui::TextDisabled("project: %s", fs::path(project_path_).filename().string().c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nEvery edit saves it after 0.4 s.", project_path_.c_str());
    const char* names[2] = {"A", "B"};
    std::string* labels[2] = {&project_.label_a, &project_.label_b};
    for (int v = 0; v < 2; v++) {
        ImGui::PushID(v);
        const VideoInfo& info = source_[v]->Info();
        const std::string& path = v == 0 ? a_path_ : b_path_;
        ImGui::Text("%s  %s", names[v], fs::path(path).filename().string().c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", path.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(dialogs_.busy() || open_request_.has_value() || (exporter_ && exporter_->running()));
        if (ImGui::SmallButton("Change..."))
            ShowDialog(v == 0 ? FileDialog::Kind::VIDEO_A : FileDialog::Kind::VIDEO_B);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Another recording for %s (or drop a file on the %s half of the window). It needs the size and frame "
                              "rate of %s; the pair then has its own project.",
                              names[v], v == 0 ? "left" : "right", v == 0 ? "B" : "A");
        if (typing_ && PathEntry(v == 0 ? "path A" : "path B", typed_[v], "Change"))
            ChooseVideo(v, UserPath(ExpandPath(typed_[v], "")));
        if (TextInput("label", *labels[v], ImGui::GetContentRegionAvail().x * 0.6f)) {
            MarkDirty();
        }
        ImGui::TextDisabled("%ux%u, %.3f fps, %.2f s, %lld frames", info.width, info.height, info.fps,
                            probe_[v].duration, (long long)info.frames);
        ImGui::TextDisabled("%s %u-bit %s %s", info.codec.c_str(), info.bit_depth,
                            info.matrix == YuvMatrix::BT709 ? "BT.709" : info.matrix == YuvMatrix::BT601 ? "BT.601" : "BT.2020",
                            info.range == YuvRange::FULL ? "full range" : "limited range");
        const DecodeStats stats = source_[v]->Stats();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("decoder: %s", source_notes_[v].c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !gpu_decode_note_[v].empty())
            ImGui::SetTooltip("GPU decode did not run: %s", gpu_decode_note_[v].c_str());
        if (stats.decode_fps > 0.0)
            ImGui::TextDisabled("%.0f fps decode, %lld frames cached (%.0f MiB)", stats.decode_fps, (long long)stats.cached,
                                (double)stats.cached_bytes / (1024.0 * 1024.0));
        else
            ImGui::TextDisabled("decoding idle, %lld frames cached (%.0f MiB)", (long long)stats.cached,
                                (double)stats.cached_bytes / (1024.0 * 1024.0));
        ImGui::PopID();
        ImGui::Separator();
    }

    // Offset
    {
        ImGui::PushTextWrapPos(0.0f);  // the summary lines wrap at the panel's edge
        const Timeline& t = player_->clock().timeline();
        const int64_t here = t.offset(player_->clock().position());
        const bool following = project_.follow_drift && !project_.offset_map.empty();
        ImGui::Text("Offset");
        ImGui::TextDisabled("Frame n of A shows the moment of frame n %s %lld of B%s", here >= 0 ? "-" : "+", (long long)std::llabs(here),
                            following ? " (K here)" : "");
        const int deltas[] = {-10, -1, 1, 10};
        for (int i = 0; i < 4; i++) {
            if (i > 0)
                ImGui::SameLine();
            char text[16];
            std::snprintf(text, sizeof(text), "%+d", deltas[i]);
            if (ImGui::Button(text))
                NudgeOffset(deltas[i]);
        }
        ImGui::SameLine();
        int value = (int)here;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::InputInt("##offset", &value, 0, 0))
            SetOffsetHere(value);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetTooltip("K at the playhead. With the drift followed, a change moves the nearest anchor (within a second),\nelse adds one at the playhead; otherwise it is the constant offset.");
        bool follow = project_.follow_drift;
        if (ImGui::Checkbox("Follow drift", &follow))
            SetFollowDrift(follow);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: K changes with A's frame, by the anchors below (from the detection, or added by hand): B repeats or skips a\n"
                              "frame where K changes by one, and jumps at a pause. Off: one constant K (the offset above) all along.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Add anchor here"))
            AddAnchorHere();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("An anchor at the playhead with the K shown here: K follows the line between anchors.\nTwo anchors one frame apart make a jump.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!following);
        if (ImGui::SmallButton("Remove anchor"))
            RemoveAnchorHere();
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear anchors"))
            ClearAnchors();
        ImGui::EndDisabled();
        DrawOffsetPlot(ImGui::GetFontSize() * 6.0f);
        if (project_.sync) {
            const SyncResult& s = *project_.sync;
            ImGui::TextDisabled("Detected %lld frames (%s%s)%s", (long long)s.offset_frames, s.quality.c_str(),
                                s.spread_seconds ? (", spread " + std::to_string((int)std::lround(*s.spread_seconds * 1000.0)) + " ms").c_str() : "",
                                s.offset_frames == project_.offset_frames ? "" : (", now " + std::to_string(project_.offset_frames - s.offset_frames)).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("coarse: A = B %+.1f s, similarity %.3f (next best %.3f, z %.1f)\n%zu windows",
                                  s.coarse.offset, s.coarse.similarity, s.coarse.next_best, s.coarse.z, s.windows.size());
            if (s.tracking.present) {
                const SyncTracking& tr = s.tracking;
                ImGui::TextDisabled("Slope %+.0f ppm (%+.2f frames over the overlap, %+.2f per minute), %zu jump%s", tr.slope_ppm,
                                    tr.drift_frames, tr.slope_ppm * 1e-6 * fps_ * 60.0, tr.jumps.size(), tr.jumps.size() == 1 ? "" : "s");
                for (size_t i = 0; i < tr.jumps.size() && i < 4; i++)
                    ImGui::TextDisabled("  jump at %s: %+.1f frames", FormatTime(tr.jumps[i].a_time).c_str(), tr.jumps[i].frames);
                ImGui::TextDisabled("Windows %d used, %d rejected; confidence %.2f; %.0f%% of the picture masked", tr.windows_used,
                                    tr.windows_rejected, tr.confidence, tr.mask_fraction * 100.0);
                if (tr.significant && !following)
                    ImGui::TextColored(WARNING, "The drift is real (%+.1f frames) and not followed.", tr.drift_frames);
            }
        } else {
            ImGui::TextDisabled("Not detected.");
        }
        ImGui::PopTextWrapPos();
    }
    const SyncJob::Status job = sync_.Poll();
    if (job.running) {
        ImGui::ProgressBar((float)job.progress, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("%s", job.line.c_str());
        if (ImGui::Button("Cancel"))
            sync_.Cancel();
    } else {
        if (ImGui::Button("Detect again"))
            StartSync(false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Decodes both recordings once and tracks the offset over them (10 s windows every 4 s). It replaces the\n"
                              "anchors. About 10 s per minute of 4K video.");
        ImGui::SameLine();
        if (ImGui::Button("Detect around the playhead"))
            StartSync(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Matches 10 s around the playhead, near K here: shows whether it drifts. The other anchors stay.");
        if (!sync_line_.empty())
            ImGui::TextDisabled("%s", sync_line_.c_str());
        if (local_window_) {
            const SyncWindow& w = *local_window_;
            const int64_t local = (int64_t)std::llround(w.offset * fps_);
            ImGui::Text("Here: %lld frames", (long long)local);
            ImGui::SameLine();
            ImGui::TextColored(WindowSharp(w) ? GOOD : WARNING, "(confidence %.2f, contrast %.2f, %d frames wide%s)", w.confidence,
                               w.contrast, w.width_frames, WindowSharp(w) ? "" : ", not sharp");
            if (local != OffsetHere() && ImGui::Button("Apply"))
                ApplyLocalWindow(w), local_window_.reset();
        } else if (!job.error.empty() && job.local) {
            ImGui::TextColored(WARNING, "%s", job.error.c_str());
        }
    }
    ImGui::Separator();

    // View
    ImGui::Text("View");
    const char* view_names[] = {"Divider", "Side by side", "A", "B"};
    const Layout view_layouts[] = {Layout::slider, Layout::side_by_side, Layout::a_only, Layout::b_only};
    for (int i = 0; i < 4; i++) {
        if (i > 0)
            ImGui::SameLine();
        const bool on = layout_ == view_layouts[i];
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
        if (ImGui::Button(view_names[i]))
            SetLayout(view_layouts[i]);
        if (on)
            ImGui::PopStyleColor();
    }
    float divider = (float)divider_;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    if (ImGui::SliderFloat("divider", &divider, 0.0f, 1.0f, "%.3f")) {
        divider_ = divider;
        MarkDirty();
    }
    ImGui::TextDisabled("zoom %.2fx%s", mapping_[0].camera.z, preview_ ? " (preview)" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton("1x"))
        camera_ = Camera(), preview_ = false;
    const Mapping& m = mapping_[0];
    ImGui::TextDisabled("%.2f screen pixels per source pixel (%s)", m.scale, m.nearest() ? "exact pixels" : "averaged");
    ImGui::Separator();
    const LockstepStats& s = player_->clock().stats();
    ImGui::TextDisabled("pairs shown %llu, held %llu, stalls %llu, mismatched %llu", (unsigned long long)s.pairs_shown,
                        (unsigned long long)s.pairs_held, (unsigned long long)s.stalls,
                        (unsigned long long)player_->mismatched());
    ImGui::End();
}

void ClipViewer::DrawAreasPanel() {
    if (!ImGui::Begin("Areas") || !loaded_) {
        if (!loaded_)
            ImGui::TextDisabled("Marks need two recordings.");
        ImGui::End();
        return;
    }
    // Defaults: the kind of a new mark, the freeze's wait, the zoom time, the span's length.
    const char* kind_names[] = {"freeze", "span"};
    int new_kind = project_.new_mark_kind == AreaKind::FREEZE ? 0 : 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    if (ImGui::Combo("new marks", &new_kind, kind_names, 2)) {
        project_.new_mark_kind = new_kind == 0 ? AreaKind::FREEZE : AreaKind::SPAN;
        MarkDirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("freeze: the video stops at the frame, zooms to the rectangle, waits, zooms back and plays on.\n"
                          "span: the camera zooms to the rectangle while the video plays, from t0 to t1.");
    double freeze_hold = project_.freeze_hold, hold = project_.hold, ease = project_.ease;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::InputDouble("freeze hold (s)", &freeze_hold, 0.1, 0.5, "%.2f")) {
        project_.freeze_hold = std::clamp(freeze_hold, 0.0, 60.0);
        MarkDirty();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::InputDouble("zoom in/out (s)", &ease, 0.1, 0.5, "%.2f")) {
        project_.ease = std::clamp(ease, 0.0, 5.0);
        MarkDirty();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::InputDouble("span hold (s)", &hold, 0.5, 1.0, "%.2f")) {
        project_.hold = std::clamp(hold, 0.2, 60.0);
        MarkDirty();
    }
    ImGui::Checkbox("Zoom marks while playing", &zoom_marks_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While playing, the clock stops at a freeze mark, the sequence runs and playing goes on by itself.\n"
                          "Scrubbing and paused viewing do not run marks.");
    {
        // Which rectangles show on the video: near the playhead (a span from `fade in` seconds before t0 to `fade out` after
        // t1; a freeze from before its frame to after the end of its sequence), always, or never. The timeline and the list
        // below always show every mark.
        const char* show_names[] = {"near the playhead", "always", "never"};
        int show = (int)marks_shown_.show;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
        if (ImGui::Combo("show marks", &show, show_names, 3))
            marks_shown_.show = (MarkShow)show;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Which rectangles are drawn on the video. Near the playhead they fade in and out; the timeline and\n"
                              "this list always show every mark. Not stored in the project.");
        ImGui::BeginDisabled(marks_shown_.show != MarkShow::NEAR_PLAYHEAD);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::InputDouble("fade in (s)", &marks_shown_.lead, 0.25, 1.0, "%.2f"))
            marks_shown_.lead = std::clamp(marks_shown_.lead, 0.0, 30.0);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Seconds before a mark's time (a span's start, a freeze's frame) over which its rectangle fades in.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::InputDouble("fade out (s)", &marks_shown_.trail, 0.25, 1.0, "%.2f"))
            marks_shown_.trail = std::clamp(marks_shown_.trail, 0.0, 30.0);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Seconds after the end of a mark's effect (a span's end, the end of a freeze's sequence) over which\n"
                              "its rectangle fades out.");
        ImGui::EndDisabled();
    }
    if (ImGui::Button(mark_mode_ ? "Marking: drag on the video (R)" : "Mark area (R)")) {
        mark_mode_ = !mark_mode_;
        redraw_area_ = -1;
    }
    ImGui::SameLine();
    const Area* selected_mark = SelectedArea();
    const bool previews_mark = selected_mark && selected_mark->kind == AreaKind::FREEZE;
    if (ImGui::Button(preview_ ? "Stop preview (Z)" : previews_mark ? "Preview mark (Z)" : "Preview zoom (Z)"))
        TogglePreview();
    if (project_.areas.empty())
        ImGui::TextDisabled("Press R, then drag a rectangle on the video: a mark at the frame on screen.");
    const double duration = (double)source_[0]->Info().frames / fps_;
    const std::string label_for_skipped = "outside the overlap of the recordings: left out of the playback and the export";
    int to_delete = -1;
    for (size_t i = 0; i < project_.areas.size(); i++) {
        Area& a = project_.areas[i];
        ImGui::PushID(a.id);
        const ImU32 color = AreaColor(i);
        const bool selected = a.id == selected_area_;
        const bool freeze = a.kind == AreaKind::FREEZE;
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float box = ImGui::GetFontSize();
        if (freeze) {  // a diamond for a freeze, a bar for a span, as on the timeline
            const ImVec2 c(p.x + box / 2.0f, p.y + box / 2.0f);
            const float r = box * 0.5f;
            ImGui::GetWindowDrawList()->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r),
                                                      ImVec2(c.x - r, c.y), color);
        } else {
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + box, p.y + box), color, 3.0f);
        }
        ImGui::Dummy(ImVec2(box, box));
        ImGui::SameLine();
        std::string label = a.label;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (TextInput("##label", label)) {
            a.label = label;
            project_.areas_dirty = true;
            MarkDirty();
        }
        if (ImGui::IsItemActive() || ImGui::IsItemClicked())
            selected_area_ = a.id;
        int kind = freeze ? 0 : 1;
        const char* kinds_short[] = {"freeze", "span"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::Combo("kind", &kind, kinds_short, 2)) {
            SetMarkKind(a, kind == 0 ? AreaKind::FREEZE : AreaKind::SPAN);
            selected_area_ = a.id;
        }
        const double now = TimeOf(player_->clock().position());
        if (freeze) {
            // The frame: its number and time, frame step buttons, and "= now".
            const int64_t frame = MarkFrame(a);
            ImGui::Text("frame %lld  (%s)", (long long)frame, FormatTime(TimeOf(frame)).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("-1")) {
                SetMarkFrame(a, frame - 1);
                selected_area_ = a.id;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+1")) {
                SetMarkFrame(a, frame + 1);
                selected_area_ = a.id;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("= now"))
                SetMarkFrame(a, player_->clock().position());
            if (!InOverlap(a))
                ImGui::TextColored(WARNING, "%s", label_for_skipped.c_str());
            // Own hold and zoom time, else the project's.
            double mark_hold = a.hold.value_or(project_.freeze_hold), mark_ease = a.ease.value_or(project_.ease);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            if (ImGui::InputDouble("hold (s)", &mark_hold, 0.1, 0.5, "%.2f")) {
                a.hold = std::clamp(mark_hold, 0.0, 60.0);
                project_.areas_dirty = true;
                MarkDirty();
                selected_area_ = a.id;
            }
            ImGui::SameLine();
            if (a.hold) {
                if (ImGui::SmallButton("reset##hold")) {
                    a.hold.reset();
                    project_.areas_dirty = true;
                    MarkDirty();
                }
            } else {
                ImGui::TextDisabled("(project's)");
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            if (ImGui::InputDouble("zoom in/out (s)", &mark_ease, 0.1, 0.5, "%.2f")) {
                a.ease = std::clamp(mark_ease, 0.0, 5.0);
                project_.areas_dirty = true;
                MarkDirty();
                selected_area_ = a.id;
            }
            ImGui::SameLine();
            if (a.ease) {
                if (ImGui::SmallButton("reset##ease")) {
                    a.ease.reset();
                    project_.areas_dirty = true;
                    MarkDirty();
                }
            } else {
                ImGui::TextDisabled("(project's)");
            }
        } else {
            double t0 = a.t0, t1 = a.t1;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            if (ImGui::InputDouble("from (s)", &t0, 0.05, 1.0, "%.2f")) {
                a.t0 = std::clamp(t0, 0.0, duration);
                project_.areas_dirty = true;
                MarkDirty();
                selected_area_ = a.id;
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            if (ImGui::InputDouble("to (s)", &t1, 0.05, 1.0, "%.2f")) {
                a.t1 = std::clamp(t1, 0.0, duration);
                project_.areas_dirty = true;
                MarkDirty();
                selected_area_ = a.id;
            }
            if (ImGui::SmallButton("from = now")) {
                a.t0 = now;
                project_.areas_dirty = true;
                MarkDirty();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("to = now")) {
                a.t1 = now;
                project_.areas_dirty = true;
                MarkDirty();
            }
        }
        int divider_kind = a.divider.kind;
        const char* freeze_dividers[] = {"divider stays", "divider sweeps during the hold", "divider fixed during the hold"};
        const char* span_dividers[] = {"divider stays", "divider sweeps", "divider fixed"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::Combo("##divider", &divider_kind, freeze ? freeze_dividers : span_dividers, 3)) {
            a.divider.kind = (AreaDivider::Kind)divider_kind;
            if (divider_kind == AreaDivider::FIXED)
                a.divider.fraction = divider_;
            project_.areas_dirty = true;
            MarkDirty();
        }
        if (a.divider.kind == AreaDivider::SWEEPING) {
            ImGui::SameLine();
            int sweeps = a.divider.sweeps;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (ImGui::SliderInt("##sweeps", &sweeps, 1, 8, "%d swings")) {
                a.divider.sweeps = sweeps;
                project_.areas_dirty = true;
                MarkDirty();
            }
        }
        if (a.divider.kind == AreaDivider::FIXED) {
            ImGui::SameLine();
            float f = (float)a.divider.fraction;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (ImGui::SliderFloat("##fraction", &f, 0.0f, 1.0f, "%.2f")) {
                a.divider.fraction = f;
                project_.areas_dirty = true;
                MarkDirty();
            }
        }
        if (ImGui::SmallButton("go to")) {
            player_->clock().Pause();
            Seek(freeze ? MarkFrame(a) : (int64_t)std::ceil(a.t0 * fps_) + 1);
            selected_area_ = a.id;
        }
        if (freeze) {
            ImGui::SameLine();
            if (ImGui::SmallButton("preview"))
                PreviewMark(a);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("redraw")) {
            mark_mode_ = true;
            redraw_area_ = a.id;
            selected_area_ = a.id;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("delete"))
            to_delete = a.id;
        ImGui::Separator();
        ImGui::PopID();
    }
    if (to_delete >= 0)
        DeleteArea(to_delete);
    ImGui::End();
}

void ClipViewer::DrawDisplayPanel() {
    if (!ImGui::Begin("Display")) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginCombo("swap chain", SWAP_LABELS[swap_.encoding()])) {
        for (uint32_t e = 0; e < ENCODINGS; e++) {
            ImGui::BeginDisabled(!swap_.supported(e));
            if (ImGui::Selectable(SWAP_LABELS[e], swap_.encoding() == e) && e != swap_.wanted()) {
                swap_.Want(e);
                swap_.dirty = true;
            }
            ImGui::EndDisabled();
            if (!swap_.supported(e) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The window's surface does not offer it.");
        }
        ImGui::EndCombo();
    }
    if (swap_.encoding() != SRGB8) {
        HdrState& h = swap_.hdr;
        bool metadata = false;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::SliderFloat("paper white (nits)", &h.paper_white, 40.0f, 600.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) {
            h.paper_white_set = true;
            h.peak = std::max(h.peak, h.paper_white);
        }
        metadata = ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Where SDR white goes (and the UI). Display: %s.",
                              h.reported_white > 0.0f ? (std::to_string((int)h.reported_white) + " nits SDR white").c_str()
                                                      : "no SDR white reported");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::SliderFloat("peak (nits)", &h.peak, h.paper_white, 10000.0f, "%.0f", ImGuiSliderFlags_Logarithmic))
            h.peak_set = true;
        metadata = metadata || ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::Checkbox("HDR metadata", &swap_.hdr_metadata))
            metadata = true;
        if (metadata)
            swap_.dirty = true;
    }
    if (ImGui::Checkbox("vsync", &m_Vsync))
        swap_.dirty = true;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::SliderFloat("UI scale", &ui_scale_, 0.6f, 2.5f, "%.2f")) {
        ui_scale_auto_ = false;
        ApplyUiScale();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("auto", &ui_scale_auto_))
        ApplyUiScale();
    ImGui::TextDisabled("window %ux%u, view %dx%d; %.1f fps", GetOutputResolution().x, GetOutputResolution().y, view_[2],
                        view_[3], frame_ms_ > 0.0 ? 1000.0 / frame_ms_ : 0.0);
    ImGui::TextDisabled("The video's codes go to the display as they are: sRGB codes on an SDR swap chain, the same codes "
                        "at the paper white on an HDR one. No dithering, no filtering from a scale of 1.");
    ImGui::End();
}

void ClipViewer::DrawExportPanel() {
    if (!ImGui::Begin("Export") || !loaded_) {
        if (!loaded_)
            ImGui::TextDisabled("The export needs two recordings.");
        ImGui::End();
        return;
    }
    if (!export_settings_) {
        export_settings_ = std::make_unique<ExportSettings>();
        // beside A (the project itself may be in the renderer's cache folder)
        export_settings_->output = (fs::path(a_path_).parent_path() / (fs::path(a_path_).stem().string() + "-vs-" +
                                                                        fs::path(b_path_).stem().string() + "-comparison.mp4"))
                                       .string();
    }
    ExportSettings& s = *export_settings_;
    const bool running = exporter_ && exporter_->running();
    ImGui::BeginDisabled(running);
    const char* heights[] = {"source", "2160", "1440", "1080", "720"};
    const int height_values[] = {0, 2160, 1440, 1080, 720};
    int height_index = 0;
    for (int i = 0; i < 5; i++)
        if (s.height == height_values[i])
            height_index = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    if (ImGui::Combo("height", &height_index, heights, 5))
        s.height = height_values[height_index];
    const char* resamples[] = {"auto", "nearest", "bilinear", "bicubic", "box"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    ImGui::Combo("scaling", &s.resample, resamples, 5);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("auto: box below a scale of 1, nearest pixels from a zoom of 2, else bicubic");
    const char* codecs[] = {"H.265", "AV1"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    ImGui::Combo("codec", &s.codec, codecs, 2);
    const char* encoders[] = {"software", "hardware"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    ImGui::Combo("encoder", &s.hardware, encoders, 2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("hardware: the first of this platform's ffmpeg hardware encoders that works (NVENC, VideoToolbox,\n"
                          "QSV, VAAPI, AMF), else software. The log line 'export: encoder' says which ran.");
    const char* rates[] = {"quality (crf)", "bitrate", "very high", "lossless (H.265)"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    ImGui::Combo("rate", &s.rate, rates, 4);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("quality: constant quality, crf in libx265's scale (lower is better)\n"
                          "bitrate: a target in Mbit/s, peaks to 1.5 times it\n"
                          "very high: a target from the size and frame rate (H.265 0.40, AV1 0.30 bits per pixel and frame\n"
                          "    at 60 fps, times the square root of fps / 60)\n"
                          "    with the encoder's slowest, best preset\n"
                          "lossless: NVENC's lossless H.265 (else libx265's) of the 4:2:0 frames");
    if (s.rate == 0) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
        ImGui::InputInt("crf", &s.crf, 1, 4);
        s.crf = std::clamp(s.crf, 0, 51);
    } else if (s.rate == 1) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::InputDouble("Mbit/s", &s.mbps, 5.0, 25.0, "%.0f");
        s.mbps = std::clamp(s.mbps, 1.0, 2000.0);
    } else if (s.rate == 2 && player_) {
        const uint32_t h = s.height > 0 ? (uint32_t)s.height : (uint32_t)project_.height;
        uint32_t w = project_.height > 0 ? (uint32_t)std::lround((double)h * project_.width / project_.height / 2.0) * 2 : 0;
        if ((s.layout.empty() ? project_.layout : s.layout) == "side-by-side")
            w *= 2;
        ImGui::TextDisabled("about %.0f Mbit/s at %ux%u, %.0f fps", VeryHighMbps(s.codec, w, h, project_.fps), w, h, project_.fps);
    } else if (s.rate == 3 && s.codec != 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "lossless is H.265 only");
    }
    const char* audios[] = {"none", "A's", "B's"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    ImGui::Combo("audio", &s.audio, audios, 3);
    const SyncJob::Status unused = sync_.Poll();
    (void)unused;
    const Timeline& t = player_->clock().timeline();
    const double lo = TimeOf(t.overlap_first()), hi = TimeOf(t.overlap_end());
    double start = s.start < 0.0 ? lo : s.start, end = s.end < 0.0 ? hi : s.end;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::InputDouble("from (s)", &start, 1.0, 10.0, "%.2f"))
        s.start = std::clamp(start, lo, hi);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::InputDouble("to (s)", &end, 1.0, 10.0, "%.2f"))
        s.end = std::clamp(end, lo, hi);
    if (ImGui::SmallButton("whole overlap"))
        s.start = s.end = -1.0;
    {
        // The output's length: the range's frames plus what the freeze marks insert (and which marks are left out).
        EditOptions o = MarkOptions();
        o.first = std::max(o.first, (int64_t)std::ceil(start * fps_ - 1e-6));
        o.end = std::min(o.end, (int64_t)std::floor(end * fps_ + 1e-6));
        const EditList list(project_.areas, o);
        const int64_t inserted = list.length() - list.source_frames();
        ImGui::TextDisabled("%lld frames (%.1f s)%s", (long long)list.length(), (double)list.length() / fps_,
                            inserted > 0 ? (", " + std::to_string((long long)inserted) + " of them from " +
                                            std::to_string(list.freezes().size()) + " freeze marks")
                                               .c_str()
                                         : "");
        if (!list.skipped().empty())
            ImGui::TextColored(WARNING, "%zu freeze marks are outside the range and left out", list.skipped().size());
    }
    DrawLogoSettings();
    TextInput("file", s.output);
    ImGui::EndDisabled();
    if (running) {
        ImGui::ProgressBar((float)exporter_->progress(), ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("%s", exporter_->status().c_str());
        if (ImGui::Button("Cancel export"))
            exporter_->Cancel();
    } else {
        if (ImGui::Button("Export comparison video"))
            StartExport();
        ImGui::SameLine();
        if (ImGui::Button("Export images of this frame"))
            StartImages();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("PNG images of the pair at the playhead: with the divider, and side by side (twice as wide,\n"
                              "each recording whole). At the export height, with the labels and logos, beside the video's file.");
        for (const std::string& image : images_written_)
            ImGui::TextColored(GOOD, "wrote %s", image.c_str());
        if (!export_error_.empty())
            ImGui::TextColored(BAD, "%s", export_error_.c_str());
        else if (exporter_ && !exporter_->error().empty())
            ImGui::TextColored(BAD, "%s", exporter_->error().c_str());
        else if (exporter_ && exporter_->finished_ok())
            ImGui::TextColored(GOOD, "wrote %s", s.output.c_str());
        else if (exporter_ && !exporter_->status().empty())
            ImGui::TextDisabled("%s", exporter_->status().c_str());
    }
    ImGui::TextDisabled("Encodes with the ffmpeg executable; the same pairing as playback (A frame n with B frame n - K).");
    ImGui::End();
}

void ClipViewer::StartImages() {
    if (!export_settings_ || (exporter_ && exporter_->running()))
        return;
    SaveIfDue(true);
    const int64_t frame = player_->clock().position();
    const fs::path video(export_settings_->output);
    const std::string stem = (video.parent_path() / video.stem()).string() + "_f" + std::to_string((long long)frame);
    image_queue_.clear();
    images_written_.clear();
    for (const char* layout : {"slider", "side-by-side"}) {
        ExportSettings image = *export_settings_;
        image.image_at = TimeOf(frame);
        image.layout = layout;
        image.output = stem + "_" + layout + ".png";
        image_queue_.push_back(image);
    }
    PumpImages();
}

void ClipViewer::PumpImages() {
    if (exporter_ && exporter_->running())
        return;
    if (!image_running_.empty()) {  // the image that just ended
        if (exporter_ && exporter_->finished_ok())
            images_written_.push_back(image_running_);
        image_running_.clear();
    }
    if (image_queue_.empty())
        return;
    const ExportSettings next = image_queue_.front();
    image_queue_.erase(image_queue_.begin());
    std::string error;
    exporter_ = Exporter::Start(gfx_, project_, next, error);
    if (!exporter_) {
        spdlog::error("export: {}", error);
        export_error_ = error;
        image_queue_.clear();
        return;
    }
    export_error_.clear();
    image_running_ = next.output;
}

void ClipViewer::StartExport() {
    SaveIfDue(true);
    images_written_.clear();
    std::string error;
    exporter_ = Exporter::Start(gfx_, project_, *export_settings_, error);
    if (!exporter_) {
        spdlog::error("export: {}", error);
        export_error_ = error;
    } else {
        export_error_.clear();
    }
}

// --- Transport and timeline -------------------------------------------------------------------------------------------

void ClipViewer::DrawTransport() {
    if (!ImGui::Begin("Transport") || !loaded_) {
        if (!loaded_)
            ImGui::TextDisabled("Play, step and the timeline show here once A and B are chosen.");
        ImGui::End();
        return;
    }
    Lockstep& clock = player_->clock();
    if (ImGui::Button(clock.playing() ? "Pause" : "Play", ImVec2(ImGui::GetFontSize() * 4.0f, 0.0f)))
        clock.playing() ? clock.Pause() : clock.Play();
    ImGui::SameLine();
    if (ImGui::Button("<|"))
        clock.Step(-1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("one frame back (Left)");
    ImGui::SameLine();
    if (ImGui::Button("|>"))
        clock.Step(1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("one frame forward (Right)");
    ImGui::SameLine();
    const double speeds[] = {0.25, 0.5, 1.0, 2.0};
    const char* speed_names[] = {"0.25x", "0.5x", "1x", "2x"};
    int speed = 2;
    for (int i = 0; i < 4; i++)
        if (std::fabs(clock.speed() - speeds[i]) < 1e-9)
            speed = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    if (ImGui::Combo("##speed", &speed, speed_names, 4))
        clock.SetSpeed(speeds[speed]);
    ImGui::SameLine();
    const int64_t n = clock.position();
    const Timeline& t = clock.timeline();
    const double duration = (double)t.end() / fps_;
    ImGui::Text("%s / %s", FormatTime(TimeOf(n)).c_str(), FormatTime(duration).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("A %s | B %s", t.has_a(n) ? std::to_string((long long)n).c_str() : "-",
                        t.has_b(n) ? std::to_string((long long)t.b_frame(n)).c_str() : "-");
    DrawTimeline(std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing(), ImGui::GetFontSize() * 1.5f));
    // status line
    if (mark_state_.active)
        ImGui::TextColored(GOOD, "%s (frame %lld held)", MarkStatus().c_str(), (long long)clock.position());
    else if (tick_.buffering)
        ImGui::TextColored(WARNING, "buffering: waiting for both recordings' frames");
    else if (clock.decode_limited())
        ImGui::TextColored(WARNING, "decode-limited %.2fx (the decoders sustain %.0f pairs per second)", clock.rate(),
                           player_->capability());
    else if (tick_.pair && !tick_.pair->has_b)
        ImGui::TextColored(WARNING, "only %s has frames here", project_.label_a.c_str());
    else if (tick_.pair && !tick_.pair->has_a)
        ImGui::TextColored(WARNING, "only %s has frames here", project_.label_b.c_str());
    else
    {
        const int64_t k = t.offset(n);
        if (project_.follow_drift && !t.offsets().is_constant()) {
            // K here, and the drift: the segment's slope, else the tracker's
            double slope = t.offsets().slope_at(n);
            if (slope == 0.0 && project_.sync && project_.sync->tracking.present)
                slope = project_.sync->tracking.slope_ppm * 1e-6;
            ImGui::TextDisabled("locked pairs: A frame n with B frame n %s %lld, drift %+.2f frames/min", k >= 0 ? "-" : "+",
                                (long long)std::llabs(k), slope * fps_ * 60.0);
        } else {
            ImGui::TextDisabled("locked pairs: A frame n with B frame n %s %lld", k >= 0 ? "-" : "+", (long long)std::llabs(k));
        }
    }
    ImGui::SameLine();
    const LockstepStats& s = clock.stats();
    ImGui::TextDisabled("  shown %llu, held %llu", (unsigned long long)s.pairs_shown, (unsigned long long)s.pairs_held);
    ImGui::End();
}

// K over A's time: the windows of the tracker (the ones the fit used filled, the rejected greyed), the line K(n) the playback
// uses (or the constant), the tracker's own fit when it is not the one in use, the anchors, and the playhead. A click or a drag
// moves the playhead.
void ClipViewer::DrawOffsetPlot(float height) {
    Lockstep& clock = player_->clock();
    const Timeline& t = clock.timeline();
    const OffsetMap offsets = project_.Offsets();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(height, 40.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##offset_plot", size);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const int64_t first = t.has_overlap() ? t.overlap_first() : t.first();
    const int64_t end = t.has_overlap() ? t.overlap_end() : std::max(t.end(), t.first() + 1);
    const double span = std::max((double)(end - first), 1.0);
    // the K range: every window, the followed line and the tracker's fit, with at least 4 frames of height
    double lo = 1e300, hi = -1e300;
    auto include = [&](double k) {
        lo = std::min(lo, k);
        hi = std::max(hi, k);
    };
    include(offsets.exact((double)first));
    include(offsets.exact((double)end));
    for (const OffsetPoint& p : offsets.points())
        include(p.k);
    const SyncResult* sync = project_.sync ? &*project_.sync : nullptr;
    if (sync) {
        for (const SyncWindow& w : sync->windows)
            if (sync->tracking.present)
                include(w.offset * fps_);
        for (const OffsetPoint& p : sync->tracking.map)
            include(p.k);
    }
    if (hi - lo < 4.0) {
        const double mid = 0.5 * (hi + lo);
        lo = mid - 2.0;
        hi = mid + 2.0;
    }
    const double pad = 0.12 * (hi - lo);
    lo -= pad;
    hi += pad;
    auto x_of = [&](double frame) { return p0.x + (float)((frame - (double)first) / span) * size.x; };
    auto y_of = [&](double k) { return p1.y - (float)((k - lo) / (hi - lo)) * size.y; };
    list->AddRectFilled(p0, p1, IM_COL32(10, 11, 14, 255), 3.0f);
    {
        // the span of A's time the plot covers
        const std::string from = FormatTime(TimeOf(first)), to = FormatTime(TimeOf(end));
        const float line = ImGui::GetFontSize();
        list->AddText(ImVec2(p0.x + 3.0f, p1.y - line - 1.0f), IM_COL32(120, 126, 140, 255), from.c_str());
        list->AddText(ImVec2(p1.x - ImGui::CalcTextSize(to.c_str()).x - 3.0f, p1.y - line - 1.0f), IM_COL32(120, 126, 140, 255), to.c_str());
    }
    // grid: whole frames at a step that keeps about four lines
    double step = 1.0;
    while ((hi - lo) / step > 5.0)
        step *= (step < 2.0 ? 2.0 : step < 5.0 ? 2.5 : 2.0);
    for (double k = std::ceil(lo / step) * step; k <= hi; k += step) {
        const float y = y_of(k);
        list->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(40, 44, 54, 255));
        char label[24];
        std::snprintf(label, sizeof(label), "%.0f", k);
        list->AddText(ImVec2(p0.x + 3.0f, y - ImGui::GetFontSize() * 0.95f), IM_COL32(120, 126, 140, 255), label);
    }
    list->PushClipRect(p0, p1, true);
    // the tracker's fit, when it is not what plays
    if (sync && sync->tracking.present && sync->tracking.map.size() >= 2 && OffsetMap(sync->tracking.map) != offsets) {
        const OffsetMap fitted(sync->tracking.map);
        ImVec2 last(0, 0);
        for (int x = 0; x <= (int)size.x; x += 2) {
            const ImVec2 point(p0.x + (float)x, y_of(fitted.exact((double)first + span * x / std::max(size.x, 1.0f))));
            if (x > 0)
                list->AddLine(last, point, IM_COL32(120, 126, 140, 255), 1.0f);
            last = point;
        }
    }
    // the windows
    if (sync && sync->tracking.present)
        for (const SyncWindow& w : sync->windows) {
            const ImVec2 c(x_of(w.a_time * fps_), y_of(w.offset * fps_));
            if (w.used)
                list->AddCircleFilled(c, 3.0f, IM_COL32(90, 160, 255, 230));
            else
                list->AddCircle(c, 3.0f, IM_COL32(110, 114, 124, 200));
        }
    // the line that plays
    {
        const ImU32 color = project_.follow_drift && !offsets.is_constant() ? IM_COL32(120, 220, 140, 255) : IM_COL32(200, 200, 210, 255);
        ImVec2 last(0, 0);
        for (int x = 0; x <= (int)size.x; x += 2) {
            const ImVec2 point(p0.x + (float)x, y_of(offsets.exact((double)first + span * x / std::max(size.x, 1.0f))));
            if (x > 0)
                list->AddLine(last, point, color, 1.5f);
            last = point;
        }
        for (const OffsetPoint& p : offsets.points()) {
            const ImVec2 c(x_of((double)p.a), y_of(p.k));
            list->AddRectFilled(ImVec2(c.x - 3.0f, c.y - 3.0f), ImVec2(c.x + 3.0f, c.y + 3.0f), IM_COL32(255, 170, 60, 255));
        }
    }
    const float head = x_of((double)clock.position() + 0.5);
    list->AddLine(ImVec2(head, p0.y), ImVec2(head, p1.y), IM_COL32(255, 255, 255, 255));
    list->PopClipRect();
    if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const double u = std::clamp((mouse.x - p0.x) / std::max(size.x, 1.0f), 0.0f, 1.0f);
        const int64_t frame = (int64_t)std::floor((double)first + u * span);
        if (ImGui::IsItemActive())
            Seek(frame);
        if (ImGui::IsItemHovered()) {
            // the nearest window within a few pixels says what it measured
            const SyncWindow* near_window = nullptr;
            float best = 8.0f;
            if (sync && sync->tracking.present)
                for (const SyncWindow& w : sync->windows) {
                    const float d = std::fabs(x_of(w.a_time * fps_) - mouse.x);
                    if (d < best) {
                        best = d;
                        near_window = &w;
                    }
                }
            if (near_window)
                ImGui::SetTooltip("%s: K %.2f frames (%+.2f from the fit), contrast %.2f, %d wide, confidence %.2f, %s",
                                  FormatTime(near_window->a_time).c_str(), near_window->offset * fps_, near_window->residual_frames,
                                  near_window->contrast, near_window->width_frames, near_window->confidence,
                                  near_window->used ? "used" : "rejected");
            else
                ImGui::SetTooltip("%s  frame %lld  K %lld", FormatTime(TimeOf(frame)).c_str(), (long long)frame,
                                  (long long)offsets.at(frame));
        }
    }
}

void ClipViewer::DrawTimeline(float height) {
    Lockstep& clock = player_->clock();
    const Timeline& t = clock.timeline();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(height, 24.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline", size);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const double first = (double)t.first(), span = std::max((double)(t.end() - t.first()), 1.0);
    auto x_of = [&](double frame) { return p0.x + (float)((frame - first) / span) * size.x; };
    list->AddRectFilled(p0, p1, IM_COL32(10, 11, 14, 255), 3.0f);
    // where only one recording exists is dimmer; the overlap is plain
    list->AddRectFilled(ImVec2(x_of((double)t.first()), p0.y), ImVec2(x_of((double)t.end()), p1.y), IM_COL32(40, 44, 54, 255));
    if (t.has_overlap())
        list->AddRectFilled(ImVec2(x_of((double)t.overlap_first()), p0.y), ImVec2(x_of((double)t.overlap_end()), p1.y),
                            IM_COL32(52, 66, 90, 255));
    for (size_t i = 0; i < project_.areas.size(); i++) {
        const Area& a = project_.areas[i];
        ImU32 color = AreaColor(i);
        if (a.kind == AreaKind::FREEZE) {
            // A freeze mark is a tick with a diamond on it, in the mark's colour, at the middle of its frame.
            const float x = std::floor(x_of((double)MarkFrame(a) + 0.5));
            const float cy = p0.y + size.y * 0.7f, r = std::max(size.y * 0.22f, 4.0f);
            const bool selected_mark = a.id == selected_area_;
            const ImU32 solid = (color & 0x00FFFFFF) | (InOverlap(a) ? 0xFF000000 : 0x70000000);
            list->AddRectFilled(ImVec2(x - 1.0f, p0.y + size.y * 0.3f), ImVec2(x + 1.0f, p1.y - 2.0f), solid);
            const ImVec2 diamond[4] = {ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy)};
            list->AddConvexPolyFilled(diamond, 4, solid);
            if (selected_mark)
                list->AddPolyline(diamond, 4, IM_COL32(255, 255, 255, 255), ImDrawFlags_Closed, 1.5f);
            continue;
        }
        const float x0 = x_of(a.t0 * fps_), x1 = std::max(x_of(a.t1 * fps_), x_of(a.t0 * fps_) + 2.0f);
        const float top = p0.y + size.y * 0.55f;
        list->AddRectFilled(ImVec2(x0, top), ImVec2(x1, p1.y - 2.0f), (color & 0x00FFFFFF) | (a.id == selected_area_ ? 0xF0000000 : 0x90000000), 2.0f);
        if (a.id == selected_area_)
            list->AddRect(ImVec2(x0, top), ImVec2(x1, p1.y - 2.0f), IM_COL32(255, 255, 255, 255), 2.0f);
    }
    const float head = x_of((double)clock.position() + 0.5);
    list->AddRectFilled(ImVec2(head - 1.0f, p0.y - 2.0f), ImVec2(head + 1.0f, p1.y + 2.0f), IM_COL32(255, 255, 255, 255));
    if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const double u = std::clamp((mouse.x - p0.x) / std::max(size.x, 1.0f), 0.0f, 1.0f);
        const int64_t frame = (int64_t)std::floor(first + u * span);
        if (ImGui::IsItemActive())
            Seek(frame);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s  frame %lld", FormatTime(TimeOf(frame)).c_str(), (long long)frame);
    }
}

// --- Keys and mouse --------------------------------------------------------------------------------------------------

void ClipViewer::HandleKeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || renderer::ModeConfirmOpen())  // the confirmation of a mode switch answers Esc
        return;
    if (!loaded_) {  // the empty start has these
        if (ImGui::IsKeyPressed(ImGuiKey_F1))
            show_ui_ = !show_ui_;
        if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
            fullscreen_ = !fullscreen_;
            SetFullscreen(fullscreen_);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            close_ = true;
        return;
    }
    Lockstep& clock = player_->clock();
    auto pressed = [&](ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); };
    const bool shift = io.KeyShift;
    if (pressed(ImGuiKey_Space))
        clock.playing() ? clock.Pause() : clock.Play();
    if (pressed(ImGuiKey_LeftArrow, true) || pressed(ImGuiKey_RightArrow, true)) {
        const int64_t d = pressed(ImGuiKey_LeftArrow, true) ? -1 : 1;
        clock.Step(shift ? d * (int64_t)std::llround(fps_) : d);
    }
    if (pressed(ImGuiKey_Comma, true) || pressed(ImGuiKey_Period, true))
        NudgeOffset((pressed(ImGuiKey_Comma, true) ? -1 : 1) * (shift ? 10 : 1));
    if (pressed(ImGuiKey_1))
        SetLayout(Layout::slider);
    if (pressed(ImGuiKey_2))
        SetLayout(Layout::side_by_side);
    if (pressed(ImGuiKey_3))
        SetLayout(Layout::a_only);
    if (pressed(ImGuiKey_4))
        SetLayout(Layout::b_only);
    if (pressed(ImGuiKey_Tab))
        SetLayout(layout_ == Layout::a_only ? Layout::b_only : Layout::a_only);
    // While a freeze sequence runs the camera and the divider are the sequence's: they wait for its end.
    const bool locked = mark_state_.active;
    if ((pressed(ImGuiKey_LeftBracket, true) || pressed(ImGuiKey_RightBracket, true)) && !locked) {
        const double d = (pressed(ImGuiKey_LeftBracket, true) ? -16.0 : 16.0) / std::max(ViewRect().w, 1.0);
        divider_ = std::clamp(divider_ + d, 0.0, 1.0);
        MarkDirty();
    }
    const bool zoom_in = pressed(ImGuiKey_Equal, true) || pressed(ImGuiKey_KeypadAdd, true);
    const bool zoom_out = pressed(ImGuiKey_Minus, true) || pressed(ImGuiKey_KeypadSubtract, true);
    if ((zoom_in || zoom_out) && !locked) {
        preview_ = false;
        const Rect v = ViewRect();
        const int panel = std::max(PanelAt(io.MousePos.x, io.MousePos.y), 0);
        const Rect& area = current_layout_.area[panel];
        const double base = fit_scale(area, (int)params_[0].width, (int)params_[0].height);
        const double factor = zoom_in ? 1.25 : 1.0 / 1.25;
        camera_ = zoom_camera(area, (int)params_[0].width, (int)params_[0].height, camera_, base, v.x + v.w / 2.0,
                              v.y + v.h / 2.0, camera_.z * factor);
    }
    if ((pressed(ImGuiKey_0) || pressed(ImGuiKey_Keypad0)) && !locked) {
        camera_ = Camera();
        preview_ = false;
    }
    if (pressed(ImGuiKey_R)) {
        mark_mode_ = !mark_mode_;
        redraw_area_ = -1;
    }
    if (pressed(ImGuiKey_Z))
        TogglePreview();
    if (pressed(ImGuiKey_Delete) && selected_area_ >= 0)
        DeleteArea(selected_area_);
    if (pressed(ImGuiKey_Home))
        Seek(clock.timeline().has_overlap() ? clock.timeline().overlap_first() : clock.timeline().first());
    if (pressed(ImGuiKey_End))
        Seek(std::max(clock.timeline().end() - 1, clock.timeline().first()));
    if (pressed(ImGuiKey_F1))
        show_ui_ = !show_ui_;
    if (pressed(ImGuiKey_F11)) {
        fullscreen_ = !fullscreen_;
        SetFullscreen(fullscreen_);
    }
    if (pressed(ImGuiKey_Escape)) {
        if (mark_mode_ || drag_.kind == Drag::RECT) {
            mark_mode_ = false;
            redraw_area_ = -1;
            drag_.kind = Drag::NONE;
        } else {
            close_ = true;
        }
    }
}

void ClipViewer::HandleVideoInput() {
    ImGuiIO& io = ImGui::GetIO();
    const int w = (int)params_[0].width, h = (int)params_[0].height;
    const ImVec2 mouse = io.MousePos;
    const bool captured = io.WantCaptureMouse;
    const int panel = PanelAt(mouse.x, mouse.y);
    const bool any_down = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
    const bool locked = mark_state_.active;  // a freeze sequence runs: the camera is the sequence's
    // Release
    if (drag_.kind != Drag::NONE && !any_down) {
        if (drag_.kind == Drag::RECT) {
            if (drag_.rect[2] > 0.004 && drag_.rect[3] > 0.004) {
                if (redraw_area_ >= 0) {
                    for (Area& a : project_.areas)
                        if (a.id == redraw_area_)
                            for (int i = 0; i < 4; i++)
                                a.rect[i] = drag_.rect[i];
                    project_.areas_dirty = true;
                    MarkDirty();
                } else {
                    AddArea(drag_.rect);
                }
                mark_mode_ = false;
                redraw_area_ = -1;
            }
        } else if (drag_.kind == Drag::DIVIDER) {
            MarkDirty();
        }
        drag_.kind = Drag::NONE;
    }
    // Press
    if (drag_.kind == Drag::NONE && !captured && panel >= 0) {
        if (io.MouseClicked[0]) {
            if (mark_mode_) {
                player_->clock().Pause();
                drag_.kind = Drag::RECT;
                drag_.panel = panel;
                double u, v;
                to_normalized(mapping_[panel], w, h, mouse.x, mouse.y, u, v);
                drag_.u0 = std::clamp(u, 0.0, 1.0);
                drag_.v0 = std::clamp(v, 0.0, 1.0);
                for (int i = 0; i < 4; i++)
                    drag_.rect[i] = i < 2 ? (i == 0 ? drag_.u0 : drag_.v0) : 0.0;
            } else if (locked) {
                // a freeze sequence runs: the camera and the divider are the sequence's
            } else if (layout_ == Layout::slider) {
                drag_.kind = Drag::DIVIDER;
            } else {
                drag_.kind = Drag::PAN;
                drag_.panel = panel;
            }
        } else if ((io.MouseClicked[1] || io.MouseClicked[2]) && !locked) {
            drag_.kind = Drag::PAN;
            drag_.panel = panel;
        }
    }
    // Drag
    if (drag_.kind == Drag::DIVIDER) {
        const Rect v = ViewRect();
        divider_ = std::clamp((mouse.x - v.x) / std::max(v.w, 1.0), 0.0, 1.0);
        preview_ = false;
    } else if (drag_.kind == Drag::PAN) {
        preview_ = false;
        const Rect& area = current_layout_.area[drag_.panel];
        camera_ = pan_camera(area, w, h, mapping_[drag_.panel].camera, fit_scale(area, w, h), io.MouseDelta.x, io.MouseDelta.y);
    } else if (drag_.kind == Drag::RECT) {
        double u, v;
        to_normalized(mapping_[drag_.panel], w, h, mouse.x, mouse.y, u, v);
        u = std::clamp(u, 0.0, 1.0);
        v = std::clamp(v, 0.0, 1.0);
        drag_.rect[0] = std::min(u, drag_.u0);
        drag_.rect[1] = std::min(v, drag_.v0);
        drag_.rect[2] = std::fabs(u - drag_.u0);
        drag_.rect[3] = std::fabs(v - drag_.v0);
    }
    // Wheel and pinch zoom around the pointer; a trackpad's two-finger scroll pans
    if (!captured && panel >= 0 && drag_.kind == Drag::NONE && !locked) {
        const float2 scroll = GetScroll();
        double factor = std::pow(1.2, (double)io.MouseWheel) * (double)GetPinch();
        const bool trackpad = scroll.x != 0.0f || scroll.y != 0.0f;
        const Rect& area = current_layout_.area[panel];
        const double base = fit_scale(area, w, h);
        if (trackpad && std::fabs(GetPinch() - 1.0f) < 1e-6f) {
            preview_ = false;
            camera_ = pan_camera(area, w, h, mapping_[panel].camera, base, -scroll.x * 4.0, -scroll.y * 4.0);
        } else if (std::fabs(factor - 1.0) > 1e-9) {
            preview_ = false;
            camera_ = zoom_camera(area, w, h, mapping_[panel].camera, base, mouse.x, mouse.y, mapping_[panel].camera.z * factor);
        }
    }
}

void ClipViewer::DrawVideoOverlay() {
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const Rect v = ViewRect();
    list->PushClipRect(ImVec2((float)v.x, (float)v.y), ImVec2((float)(v.x + v.w), (float)(v.y + v.h)), true);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    OverlayInput in;
    in.layout = layout_;
    in.view = v;
    in.areas = current_layout_;
    in.mapping[0] = mapping_[0];
    in.mapping[1] = mapping_[1];
    in.video_w = (int)params_[0].width;
    in.video_h = (int)params_[0].height;
    in.label_a = project_.label_a;
    in.label_b = project_.label_b;
    in.divider_x = current_layout_.split;
    in.zoom = mapping_[0].camera.z;
    in.caption = CurrentCaption();
    in.size = size * 1.05f;
    in.line = std::max(1.0f, std::floor(size / 10.0f));
    if (tick_.pair) {
        in.no_a = !tick_.pair->has_a && layout_ != Layout::b_only;
        in.no_b = !tick_.pair->has_b && layout_ != Layout::a_only;
    }
    std::vector<IntRect> pills;
    renderer::clips::DrawVideoOverlay(list, font, in, &pills);
    {
        // The logos, on the corners of the frame the export would have: the largest of the video's shape in the view.
        // They are made at that size (a window being resized waits for the size to stay) and keep clear of the pills.
        const double aspect = (double)in.video_w / std::max(in.video_h, 1);
        const int out_w = std::max(1, (int)std::lround(std::min(v.w, v.h * aspect)));
        const int out_h = std::max(1, (int)std::lround((double)out_w / aspect));
        const float ox = std::floor((float)(v.x + (v.w - out_w) / 2.0)), oy = std::floor((float)(v.y + (v.h - out_h) / 2.0));
        for (IntRect& pill : pills) {
            pill.x -= (int)ox;
            pill.y -= (int)oy;
        }
        logo_layer_.Set(LogosOn() ? project_.logos.effective() : std::vector<LogoItem>(), LogoFolders(), out_w, out_h, 10);
        logo_layer_.Draw(list, ImVec2(ox, oy), pills);
    }
    // Areas: each with the alpha its distance from the playhead gives (MarkAlpha), the selected one the same way
    for (size_t i = 0; i < project_.areas.size(); i++) {
        const Area& a = project_.areas[i];
        const bool selected = a.id == selected_area_;
        const double alpha = MarkAlpha(a);
        if (alpha < 0.004)
            continue;
        const int panels = layout_ == Layout::side_by_side ? 2 : 1;
        for (int p = 0; p < panels; p++) {
            const int side = layout_ == Layout::side_by_side ? p : (layout_ == Layout::b_only ? 1 : 0);
            DrawAreaOutline(list, font, size, mapping_[side], in.video_w, in.video_h, a, AreaColor(i), selected, (float)alpha);
        }
    }
    if (drag_.kind == Drag::RECT) {
        const int panels = layout_ == Layout::side_by_side ? 2 : 1;
        for (int p = 0; p < panels; p++) {
            const int side = layout_ == Layout::side_by_side ? p : (layout_ == Layout::b_only ? 1 : 0);
            DrawNormalizedRect(list, mapping_[side], in.video_w, in.video_h, drag_.rect, IM_COL32(255, 255, 255, 255), true);
        }
    }
    if (mark_mode_)
        DrawPill(list, font, size, ImVec2((float)(v.x + v.w / 2.0) - size * 6.0f, (float)v.y + size), "drag a rectangle on the video (Esc cancels)", false);
    if (tick_.buffering && player_->clock().playing())
        DrawPill(list, font, size, ImVec2((float)(v.x + v.w / 2.0) - size * 2.0f, (float)(v.y + v.h / 2.0)), "buffering", false);
    list->PopClipRect();
}

}  // namespace renderer::clips
