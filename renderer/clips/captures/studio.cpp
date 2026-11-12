// The Captures tab: the browser, opening a capture, its pairs and sources, the generator jobs, the still view (geometry, input,
// overlay, the picture pass) and the scripted actions for checks. The panels are studio_ui.cpp, saving studio_save.cpp.

#include "renderer/clips/captures/studio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "renderer/clips/camera.h"
#include "renderer/clips/composer.h"
#include "renderer/clips/captures/recording.h"
#include "renderer/clips/captures/user_texture.h"
#include "renderer/clips/process.h"
#include "renderer/dock_layout.h"
#include "renderer/display.h"
#include "renderer/mode_switch.h"
#include <spdlog/spdlog.h>

namespace renderer::clips::captures {
namespace fs = std::filesystem;

namespace {

const ImVec4 WARNING(1.0f, 0.8f, 0.3f, 1.0f);

std::string Gigabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB", (double)bytes / 1e9);
    return text;
}

}  // namespace

CaptureStudio::CaptureStudio() = default;

CaptureStudio::~CaptureStudio() {
    Shutdown();
}

bool CaptureStudio::Create(const StudioHost& host, std::string& error) {
    host_ = host;
    where_ = DefaultLocations(host.workspace);
    where_.executable = ExecutablePath();
    state_ = LoadState(where_);
    thumbs_ = std::make_unique<ThumbnailLoader>(where_);
    jobs_ = std::make_unique<JobQueue>(where_);
    images_.Init(*host.gfx);
    if (!pass_.Create(*host.gfx, host.queued_frames, error) || !export_pass_.Create(*host.gfx, 1, error))
        return false;
    created_ = true;
    return true;
}

void CaptureStudio::Shutdown() {
    if (!created_)
        return;
    created_ = false;
    SaveIfDue(true);
    jobs_.reset();
    thumbs_.reset();
    host_.gfx->NRI.DeviceWaitIdle(host_.gfx->device);
    writes_.clear();
    composer_[0].reset();
    composer_[1].reset();
    images_.Destroy();
    pass_.Destroy();
    export_pass_.Destroy();
}

void CaptureStudio::Notify(const std::string& text, bool bad) {
    if (bad)
        spdlog::warn("captures: {}", text);
    else
        spdlog::info("captures: {}", text);
    if (host_.notify)
        host_.notify(text, bad);
}

// --- Browser ---------------------------------------------------------------------------------------------------------

void CaptureStudio::Rescan() {
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::string> roots = DefaultCaptureFolders();
    for (const std::string& folder : state_.capture_folders)
        roots.push_back(folder);
    steam_ = ScanSteam();
    for (const std::string& folder : steam_.capture_folders)
        roots.push_back(folder);
    entries_ = ScanCaptures(roots);
    scanned_ = true;
    scanned_at_ = host_.seconds();
    uint64_t bytes = 0;
    for (const Entry& e : entries_)
        bytes += e.bytes;
    spdlog::info("captures: {} found ({}) in {:.0f} ms", entries_.size(), Gigabytes(bytes),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

ImTextureData* CaptureStudio::ThumbTexture(const Entry& e) {
    auto found = thumb_textures_.find(e.info.folder);
    if (found != thumb_textures_.end())
        return found->second;
    const Thumbnail* image = nullptr;
    if (thumbs_->Request(e.info, &image) != ThumbnailLoader::State::READY || !image)
        return nullptr;
    ImTextureData* texture = MakeUserTexture(image->width, image->height, image->rgba.data());
    thumb_textures_[e.info.folder] = texture;
    return texture;
}

void CaptureStudio::DrawCard(const Entry& e, float width) {
    const CaptureInfo& c = e.info;
    ImGui::PushID(c.folder.c_str());
    const float line = ImGui::GetTextLineHeight();
    const float image_height = std::floor(width * 9.0f / 16.0f);
    const float height = image_height + line * 5.6f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), hovered ? IM_COL32(48, 56, 72, 255) : IM_COL32(30, 34, 44, 255), 6.0f);
    list->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(70, 78, 96, 255), 6.0f);
    const ImVec2 image_min(p.x + 4.0f, p.y + 4.0f), image_max(p.x + width - 4.0f, p.y + 4.0f + image_height - 4.0f);
    ImTextureData* texture = ImGui::IsRectVisible(p, ImVec2(p.x + width, p.y + height)) ? ThumbTexture(e) : nullptr;
    if (texture)
        list->AddImage(texture->GetTexRef(), image_min, image_max);
    else
        list->AddRectFilled(image_min, image_max, IM_COL32(16, 18, 24, 255));
    const ImU32 white = IM_COL32(235, 238, 245, 255), grey = IM_COL32(150, 158, 175, 255);
    list->PushClipRect(p, ImVec2(p.x + width, p.y + height), true);
    float y = p.y + image_height + 2.0f;
    const float x = p.x + 8.0f;
    char text[256];
    list->AddText(ImVec2(x, y), white, c.game.c_str());
    y += line * 1.15f;
    std::snprintf(text, sizeof(text), "%s   %ux%u", DisplayTime(c.time).c_str(), c.width, c.height);
    list->AddText(ImVec2(x, y), grey, text);
    y += line * 1.1f;
    int generated = 0;
    if (c.frames.size() > 1)
        generated = (int)c.frames[1].calls.size();
    std::snprintf(text, sizeof(text), "%dX  %s  %d real frames, %d generated per frame", c.multiplier, c.encoding.c_str(),
                  (int)c.frames.size(), generated);
    list->AddText(ImVec2(x, y), grey, text);
    y += line * 1.1f;
    std::snprintf(text, sizeof(text), "in game: %s", c.in_game.empty() ? "unknown" : c.in_game.c_str());
    list->AddText(ImVec2(x, y), grey, text);
    y += line * 1.1f;
    std::snprintf(text, sizeof(text), "%s   %s", c.name.c_str(), Gigabytes(e.bytes).c_str());
    list->AddText(ImVec2(x, y), IM_COL32(110, 118, 135, 255), text);
    list->PopClipRect();
    if (hovered)
        ImGui::SetTooltip("%s\n%s", c.folder.c_str(), c.game_path.c_str());
    if (clicked)
        Open(c.folder);
    ImGui::PopID();
}

void CaptureStudio::DrawBrowser() {
    if (!created_)
        return;
    if (!scanned_)
        Rescan();
    const float line = ImGui::GetFontSize();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
    ImGui::TextUnformatted("Game captures");
    ImGui::PopFont();
    // Filters
    std::vector<std::string> games = {"All games"};
    for (const Entry& e : entries_)
        if (std::find(games.begin(), games.end(), e.info.game) == games.end())
            games.push_back(e.info.game);
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s", filter_text_.c_str());
    ImGui::SetNextItemWidth(line * 9.0f);
    if (ImGui::InputTextWithHint("##filter", "filter: game, time, name", buffer, sizeof(buffer)))
        filter_text_ = buffer;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(line * 9.0f);
    if (ImGui::BeginCombo("##game", filter_game_.c_str())) {
        for (const std::string& g : games)
            if (ImGui::Selectable(g.c_str(), g == filter_game_))
                filter_game_ = g;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(line * 7.0f);
    const char* sorts[] = {"Newest first", "Oldest first", "By game"};
    ImGui::Combo("##sort", &sort_, sorts, 3);
    ImGui::BeginDisabled(dialogs_.busy());
    if (ImGui::Button("Add a folder..."))
        ShowDialog(Dialogs::Kind::CAPTURE_FOLDER, RecordingsFolder());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Rescan"))
        Rescan();
    ImGui::SameLine();
    if (ImGui::Button(typing_ ? "Hide the path field" : "Type a path..."))
        typing_ = !typing_;
    if (typing_) {
        ImGui::SetNextItemWidth(line * 30.0f);
        ImGui::InputTextWithHint("##path", "folder with captures", path_buffer_, sizeof(path_buffer_));
        ImGui::SameLine();
        if (ImGui::Button("Add") && path_buffer_[0]) {
            state_.capture_folders.push_back(path_buffer_);
            SaveState(where_, state_);
            path_buffer_[0] = 0;
            Rescan();
        }
    }
    if (!browser_note_.empty())
        ImGui::TextColored(WARNING, "%s", browser_note_.c_str());
    uint64_t bytes = 0;
    for (const Entry& e : entries_)
        bytes += e.bytes;
    std::string defaults;
    for (const std::string& folder : DefaultCaptureFolders())
        defaults += (defaults.empty() ? "" : ", ") + folder;
    ImGui::TextDisabled("%d captures, %s. Folders: %s, %d added, %d in Steam games. %d thumbnails loading.",
                        (int)entries_.size(), Gigabytes(bytes).c_str(), defaults.c_str(), (int)state_.capture_folders.size(),
                        (int)steam_.capture_folders.size(), thumbs_->loading());
    std::vector<const Entry*> shown;
    for (const Entry& e : entries_) {
        if (filter_game_ != "All games" && e.info.game != filter_game_)
            continue;
        std::string hay = e.info.game + " " + e.info.time + " " + e.info.name + " " + e.info.in_game + " " + e.info.encoding;
        std::string needle = filter_text_;
        for (std::string* s : {&hay, &needle})
            for (char& ch : *s)
                ch = (char)std::tolower((unsigned char)ch);
        if (!needle.empty() && hay.find(needle) == std::string::npos)
            continue;
        shown.push_back(&e);
    }
    if (sort_ == 1)
        std::reverse(shown.begin(), shown.end());
    else if (sort_ == 2)
        std::stable_sort(shown.begin(), shown.end(), [](const Entry* a, const Entry* b) { return a->info.game < b->info.game; });
    ImGui::BeginChild("##grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float card = std::max(line * 11.0f, 200.0f);
    const int columns = std::max(1, (int)((ImGui::GetContentRegionAvail().x + spacing) / (card + spacing)));
    const float width = std::floor((ImGui::GetContentRegionAvail().x - spacing * (float)(columns - 1)) / (float)columns);
    int column = 0;
    for (const Entry* e : shown) {
        if (column > 0)
            ImGui::SameLine(0.0f, spacing);
        DrawCard(*e, width);
        column = (column + 1) % columns;
    }
    if (shown.empty())
        ImGui::TextDisabled("No captures here. nr-record's capture DLL writes them in a game (F6 records, F9 captures 8 frames); "
                            "Add a folder... names others.");
    ImGui::EndChild();
}

// --- Opening a capture ------------------------------------------------------------------------------------------------

void CaptureStudio::Open(const std::string& folder) {
    CaptureInfo info;
    std::string error;
    if (!LoadCapture(folder, info, error)) {
        browser_note_ = error;
        Notify(error, true);
        return;
    }
    browser_note_.clear();
    images_.Clear();
    SaveIfDue(true);
    capture_ = info;
    pairs_ = EnumeratePairs(info);
    UpdateGenerators();
    cached_.clear();
    results_revision_ = ~0ull;
    camera_ = Camera();
    diff_ = false;
    selected_mark_ = -1;
    mark_mode_ = false;
    int first = 0;
    for (size_t i = 0; i < pairs_.size(); i++)
        if (pairs_[i].kind == PairKind::REAL) {
            first = (int)i;
            break;
        }
    pair_index_ = -1;
    SelectPair(first);
    ui_shown_ = false;
    spdlog::info("captures: opened {} ({}x{}, {}X, {} frames, {} pairs)", info.name, info.width, info.height, info.multiplier,
                 info.frames.size(), pairs_.size());
}

void CaptureStudio::Close() {
    SaveIfDue(true);
    tasks_.clear();
    task_prepared_ = false;
    capture_.reset();
    images_.Clear();
    image_[0] = image_[1] = nullptr;
    ui_shown_ = false;
}

std::string CaptureStudio::StemOfPair() const {
    return pair_index_ >= 0 ? pairs_[(size_t)pair_index_].Stem() : "";
}

void CaptureStudio::SelectPair(int index) {
    if (index < 0 || index >= (int)pairs_.size() || index == pair_index_)
        return;
    SaveIfDue(true);
    pair_index_ = index;
    camera_ = Camera();
    selected_mark_ = -1;
    LoadPairProject();
    RefreshResults();
    RebuildSources();
}

void CaptureStudio::LoadPairProject() {
    const CaptureInfo& c = *capture_;
    project_path_ = where_.Project(c.name, StemOfPair());
    project_ = Project();
    std::string error;
    if (fs::exists(project_path_) && renderer::clips::LoadProject(project_path_, project_, error)) {
        a_id_ = project_.path_a;
        b_id_ = project_.path_b;
        picked_[0] = picked_[1] = !a_id_.empty() && !b_id_.empty();  // the pair's project has its own choice
    } else {
        picked_[0] = picked_[1] = false;
        project_ = NewProject("", "", (int)c.width, (int)c.height, 0.0);
        project_.title = c.name + " " + StemOfPair();
        project_.logos = LogoSettings();
        a_id_.clear();
        b_id_.clear();
        if (!error.empty())
            spdlog::warn("captures: {}: {}", project_path_, error);
    }
    project_.file = project_path_;
    layout_ = base_layout_ = project_.layout == "side-by-side" ? Layout::side_by_side : Layout::slider;
    divider_ = project_.divider;
    diff_ = false;
    dirty_ = false;
}

void CaptureStudio::MarkDirty() {
    if (!dirty_)
        dirty_since_ = host_.seconds();
    dirty_ = true;
}

void CaptureStudio::SaveIfDue(bool force) {
    if (!dirty_ || !capture_ || project_path_.empty())
        return;
    if (!force && host_.seconds() - dirty_since_ < 0.4)
        return;
    project_.path_a = a_id_;
    project_.path_b = b_id_;
    project_.layout = base_layout_ == Layout::side_by_side ? "side-by-side" : "slider";
    project_.divider = divider_;
    std::error_code ec;
    fs::create_directories(fs::path(project_path_).parent_path(), ec);
    std::string error;
    if (SaveProject(project_, project_path_, error)) {
        project_.areas_dirty = false;
        project_.logos.items_dirty = false;
    } else {
        Notify("not saved: " + error, true);
    }
    dirty_ = false;
}

void CaptureStudio::RefreshResults() {
    if (!capture_)
        return;
    for (const Generator& g : generators_)
        cached_[g.id] = CachedFrames(where_, capture_->name, g);
    results_revision_ = jobs_->revision();
}

std::vector<GeneratedFrame> CaptureStudio::ResultsOfPair() const {
    std::vector<GeneratedFrame> found;
    const std::string stem = StemOfPair();
    for (const Generator& g : generators_) {
        auto it = cached_.find(g.id);
        if (it == cached_.end())
            continue;
        for (const Cached& frame : it->second)
            if (frame.stem == stem) {
                found.push_back({g, frame.file, frame.stale});
                break;
            }
    }
    return found;
}

void CaptureStudio::RebuildSources() {
    if (!capture_ || pair_index_ < 0)
        return;
    sources_ = BuildSources(*capture_, pairs_[(size_t)pair_index_], ResultsOfPair());
    auto exists = [&](const std::string& id) { return !id.empty() && FindSource(sources_, id); };
    // A side the user has not chosen follows the defaults (a finished job puts its frame there)
    const auto defaults = DefaultPick(sources_);
    for (int side = 0; side < 2; side++) {
        std::string& id = side == 0 ? a_id_ : b_id_;
        const std::string& wanted = side == 0 ? defaults.first : defaults.second;
        if ((!picked_[side] || !exists(id)) && id != wanted) {
            SetSource(side, wanted);
            picked_[side] = false;  // a default, not a choice
        }
    }
}

// A or B: the source, and its label unless the project has one for this choice already
void CaptureStudio::SetSource(int side, const std::string& id) {
    const Source* s = FindSource(sources_, id);
    if (!s)
        return;
    picked_[side] = true;
    (side == 0 ? a_id_ : b_id_) = id;
    (side == 0 ? project_.label_a : project_.label_b) = s->label;
    MarkDirty();
}

void CaptureStudio::SwapSources() {
    picked_[0] = picked_[1] = true;
    std::swap(a_id_, b_id_);
    std::swap(project_.label_a, project_.label_b);
    MarkDirty();
}

uint32_t CaptureStudio::ImageWidth() const {
    return image_[0] ? image_[0]->width : (capture_ ? capture_->width : 1920);
}
uint32_t CaptureStudio::ImageHeight() const {
    return image_[0] ? image_[0]->height : (capture_ ? capture_->height : 1080);
}

// --- Jobs -------------------------------------------------------------------------------------------------------------

void CaptureStudio::RunGenerator(const Generator& g, bool all) {
    if (!capture_ || !g.available)
        return;
    JobSpec spec;
    spec.capture_folder = capture_->folder;
    spec.capture_name = capture_->name;
    spec.generator = g;
    spec.all = all;
    if (!all && pair_index_ >= 0)
        spec.pairs.push_back(pairs_[(size_t)pair_index_]);
    const int id = jobs_->Add(spec, *capture_);
    script_wait_job_ = id;
    Notify("queued: " + spec.Title(), false);
}

// --- Geometry, input, overlay and the picture ------------------------------------------------------------------------

int CaptureStudio::PanelAt(double x, double y) const {
    const Rect v = ViewRect();
    if (x < v.x || y < v.y || x >= v.x + v.w || y >= v.y + v.h)
        return -1;
    if (ShownLayout() == Layout::side_by_side) {
        for (int i = 1; i >= 0; i--) {
            const Rect& a = current_layout_.area[i];
            if (x >= a.x && x < a.x + a.w)
                return i;
        }
        return x < v.x + v.w / 2.0 ? 0 : 1;
    }
    return ShownLayout() == Layout::b_only ? 1 : 0;
}

void CaptureStudio::SetLayout(Layout layout) {
    diff_ = false;
    layout_ = layout;
    if (layout == Layout::slider || layout == Layout::side_by_side) {
        base_layout_ = layout;
        MarkDirty();
    }
}

Area* CaptureStudio::SelectedMark() {
    for (Area& a : project_.areas)
        if (a.id == selected_mark_)
            return &a;
    return nullptr;
}

void CaptureStudio::AddMark(const double rect[4]) {
    Area a;
    int id = 0;
    for (const Area& other : project_.areas)
        id = std::max(id, other.id);
    a.id = id + 1;
    a.label = "Mark " + std::to_string(a.id);
    for (int i = 0; i < 4; i++)
        a.rect[i] = rect[i];
    project_.areas.push_back(a);  // a span with no time: on a still every mark is always there
    project_.areas_dirty = true;
    selected_mark_ = a.id;
    MarkDirty();
}

void CaptureStudio::DeleteMark(int id) {
    project_.areas.erase(std::remove_if(project_.areas.begin(), project_.areas.end(), [&](const Area& a) { return a.id == id; }),
                         project_.areas.end());
    if (selected_mark_ == id)
        selected_mark_ = -1;
    project_.areas_dirty = true;
    MarkDirty();
}

void CaptureStudio::HandleInput() {
    ImGuiIO& io = ImGui::GetIO();
    const int w = (int)ImageWidth(), h = (int)ImageHeight();
    const ImVec2 mouse = io.MousePos;
    const bool captured = io.WantCaptureMouse;
    const int panel = PanelAt(mouse.x, mouse.y);
    const bool any_down = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
    const Layout shown = ShownLayout();
    if (drag_.kind != Drag::NONE && !any_down) {
        if (drag_.kind == Drag::RECT) {
            if (drag_.rect[2] > 0.004 && drag_.rect[3] > 0.004) {
                AddMark(drag_.rect);
                mark_mode_ = false;
            }
        } else if (drag_.kind == Drag::DIVIDER) {
            MarkDirty();
        }
        drag_.kind = Drag::NONE;
    }
    if (drag_.kind == Drag::NONE && !captured && panel >= 0) {
        if (io.MouseClicked[0]) {
            if (mark_mode_) {
                drag_.kind = Drag::RECT;
                drag_.panel = panel;
                double u, v;
                to_normalized(mapping_[panel], w, h, mouse.x, mouse.y, u, v);
                drag_.u0 = std::clamp(u, 0.0, 1.0);
                drag_.v0 = std::clamp(v, 0.0, 1.0);
                drag_.rect[0] = drag_.u0;
                drag_.rect[1] = drag_.v0;
                drag_.rect[2] = drag_.rect[3] = 0.0;
            } else if (shown == Layout::slider) {
                drag_.kind = Drag::DIVIDER;
            } else {
                drag_.kind = Drag::PAN;
                drag_.panel = panel;
            }
        } else if (io.MouseClicked[1] || io.MouseClicked[2]) {
            drag_.kind = Drag::PAN;
            drag_.panel = panel;
        }
    }
    if (drag_.kind == Drag::DIVIDER) {
        const Rect v = ViewRect();
        divider_ = std::clamp((mouse.x - v.x) / std::max(v.w, 1.0), 0.0, 1.0);
    } else if (drag_.kind == Drag::PAN) {
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
    if (!captured && panel >= 0 && drag_.kind == Drag::NONE) {
        float sx = 0.0f, sy = 0.0f, pinch = 1.0f;
        if (host_.scroll)
            host_.scroll(sx, sy, pinch);
        const double factor = std::pow(1.2, (double)io.MouseWheel) * (double)pinch;
        const Rect& area = current_layout_.area[panel];
        const double base = fit_scale(area, w, h);
        if ((sx != 0.0f || sy != 0.0f) && std::fabs(pinch - 1.0f) < 1e-6f)
            camera_ = pan_camera(area, w, h, mapping_[panel].camera, base, -sx * 4.0, -sy * 4.0);
        else if (std::fabs(factor - 1.0) > 1e-9)
            camera_ = zoom_camera(area, w, h, mapping_[panel].camera, base, mouse.x, mouse.y, mapping_[panel].camera.z * factor);
    }
}

void CaptureStudio::HandleKeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || renderer::ModeConfirmOpen())  // the confirmation of a mode switch answers Esc
        return;
    auto pressed = [&](ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); };
    if (pressed(ImGuiKey_1))
        SetLayout(Layout::slider);
    if (pressed(ImGuiKey_2))
        SetLayout(Layout::side_by_side);
    if (pressed(ImGuiKey_3))
        SetLayout(Layout::a_only);
    if (pressed(ImGuiKey_4))
        SetLayout(Layout::b_only);
    if (pressed(ImGuiKey_5))
        diff_ = true;
    if (pressed(ImGuiKey_Tab))
        SetLayout(layout_ == Layout::a_only ? Layout::b_only : Layout::a_only);
    if (pressed(ImGuiKey_LeftBracket, true) || pressed(ImGuiKey_RightBracket, true)) {
        divider_ = std::clamp(divider_ + (pressed(ImGuiKey_LeftBracket, true) ? -16.0 : 16.0) / std::max(ViewRect().w, 1.0), 0.0, 1.0);
        MarkDirty();
    }
    const bool zoom_in = pressed(ImGuiKey_Equal, true) || pressed(ImGuiKey_KeypadAdd, true);
    const bool zoom_out = pressed(ImGuiKey_Minus, true) || pressed(ImGuiKey_KeypadSubtract, true);
    if (zoom_in || zoom_out) {
        const Rect v = ViewRect();
        const int panel = std::max(PanelAt(io.MousePos.x, io.MousePos.y), 0);
        const Rect& area = current_layout_.area[panel];
        const int w = (int)ImageWidth(), h = (int)ImageHeight();
        camera_ = zoom_camera(area, w, h, camera_, fit_scale(area, w, h), v.x + v.w / 2.0, v.y + v.h / 2.0,
                              camera_.z * (zoom_in ? 1.25 : 1.0 / 1.25));
    }
    if (pressed(ImGuiKey_0) || pressed(ImGuiKey_Keypad0))
        camera_ = Camera();
    if (pressed(ImGuiKey_R))
        mark_mode_ = !mark_mode_;
    if (pressed(ImGuiKey_Delete) && selected_mark_ >= 0)
        DeleteMark(selected_mark_);
    if (pressed(ImGuiKey_LeftArrow, true))
        SelectPair(pair_index_ - 1);
    if (pressed(ImGuiKey_RightArrow, true))
        SelectPair(pair_index_ + 1);
    if (pressed(ImGuiKey_F1))
        show_ui_ = !show_ui_;
    if (pressed(ImGuiKey_F11) && host_.toggle_fullscreen)
        host_.toggle_fullscreen();
    if (pressed(ImGuiKey_Escape)) {
        if (mark_mode_ || drag_.kind == Drag::RECT) {
            mark_mode_ = false;
            drag_.kind = Drag::NONE;
        } else {
            Close();
        }
    }
}

std::vector<std::string> CaptureStudio::LogoFolders() const {
    return logo_folders(fs::path(project_path_).parent_path().string());
}

bool CaptureStudio::LogosOn() {
    if (project_.logos.enabled)
        return *project_.logos.enabled;
    if (host_.seconds() - logos_checked_ > 2.0) {
        logos_checked_ = host_.seconds();
        logos_auto_ = false;
        const std::vector<std::string> folders = LogoFolders();
        for (const LogoItem& item : project_.logos.effective())
            logos_auto_ = logos_auto_ || !find_logo_file(item.path, folders).empty();
    }
    return logos_auto_;
}

void CaptureStudio::DrawOverlay() {
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const Rect v = ViewRect();
    list->PushClipRect(ImVec2((float)v.x, (float)v.y), ImVec2((float)(v.x + v.w), (float)(v.y + v.h)), true);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const int w = (int)ImageWidth(), h = (int)ImageHeight();
    OverlayInput in;
    in.layout = ShownLayout();
    in.view = v;
    in.areas = current_layout_;
    in.mapping[0] = mapping_[0];
    in.mapping[1] = mapping_[1];
    in.video_w = w;
    in.video_h = h;
    in.label_a = diff_ ? "difference |A - B| x8 (codes)" : project_.label_a;
    in.label_b = project_.label_b;
    in.divider_x = current_layout_.split;
    in.zoom = mapping_[0].camera.z;
    in.size = size * 1.05f;
    in.line = std::max(1.0f, std::floor(size / 10.0f));
    std::vector<IntRect> pills;
    DrawVideoOverlay(list, font, in, &pills);
    {  // the logos on the corners of the frame the save would have: the largest of the image's shape in the view
        const double aspect = (double)w / std::max(h, 1);
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
    if (show_marks_) {
        const int panels = ShownLayout() == Layout::side_by_side ? 2 : 1;
        for (size_t i = 0; i < project_.areas.size(); i++)
            for (int p = 0; p < panels; p++) {
                const int side = ShownLayout() == Layout::side_by_side ? p : (ShownLayout() == Layout::b_only ? 1 : 0);
                DrawAreaOutline(list, font, size, mapping_[side], w, h, project_.areas[i], AreaColor(i), project_.areas[i].id == selected_mark_);
            }
    }
    if (drag_.kind == Drag::RECT) {
        const int panels = ShownLayout() == Layout::side_by_side ? 2 : 1;
        for (int p = 0; p < panels; p++) {
            const int side = ShownLayout() == Layout::side_by_side ? p : (ShownLayout() == Layout::b_only ? 1 : 0);
            DrawNormalizedRect(list, mapping_[side], w, h, drag_.rect, IM_COL32(255, 255, 255, 255), true);
        }
    }
    if (mark_mode_)
        DrawPill(list, font, size, ImVec2((float)(v.x + v.w / 2.0) - size * 6.0f, (float)v.y + size), "drag a rectangle on the picture (Esc cancels)", false);
    for (int i = 0; i < 2; i++)
        if (!image_[i]) {
            const std::string text = image_state_[i] == ImageStore::State::FAILED ? image_error_[i] : std::string(i ? "B" : "A") + ": loading...";
            DrawPill(list, font, size, ImVec2((float)(v.x + v.w / 2.0) - size * 5.0f, (float)(v.y + v.h / 2.0) + (float)i * size * 2.0f), text, false);
        }
    list->PopClipRect();
}

void CaptureStudio::Record(nri::CommandBuffer& cb, uint32_t queued, nri::Format format) {
    if (!capture_)
        return;
    const int w = (int)ImageWidth(), h = (int)ImageHeight();
    StillParams p;
    p.regions = compare_regions(current_layout_, w, h, camera_);
    p.view = diff_ ? StillView::DIFF : StillView::PICTURES;
    p.target_encoding = host_.swap->encoding();
    p.paper_white = host_.swap->hdr.paper_white;
    p.hw_srgb = host_.swap->hw_srgb();
    for (int i = 0; i < 2; i++) {
        p.exists[i] = image_[i] != nullptr;
        if (image_[i]) {
            p.coding[i] = image_[i]->coding;
            p.size[i][0] = image_[i]->width;
            p.size[i][1] = image_[i]->height;
            p.steps[i] = (float)CodeSteps(image_[i]->dxgi);
            if (image_[i]->coding == Coding::PQ10 && p.steps[i] == 0.0f)
                p.steps[i] = 1023.0f;
        } else {
            p.size[i][0] = (uint32_t)w;
            p.size[i][1] = (uint32_t)h;
        }
    }
    pass_.Record(cb, queued, format, p, image_[0] ? image_[0]->view : nullptr, image_[1] ? image_[1]->view : nullptr);
}

// --- Frame ------------------------------------------------------------------------------------------------------------

std::string CaptureStudio::BusyReason() const {
    if (!created_)
        return {};
    if (jobs_ && jobs_->Active())
        return "A capture job (DLSS-G or ANR FG frames) is queued or running.";
    if (ExportBusy())
        return "Images are being saved.";
    return {};
}

bool CaptureStudio::Idle() const {
    return !created_ || (tasks_.empty() && writes_.empty() && !jobs_->Active() && script_queue_.empty());
}

void CaptureStudio::Update() {
    if (!created_)
        return;
    TakeDialog();
    bool ready = true;
    if (capture_) {
        if (results_revision_ != jobs_->revision()) {
            RefreshResults();
            RebuildSources();
        }
        // The images of A and B, and the ones in use kept resident
        std::vector<std::string> keep;
        for (int i = 0; i < 2; i++) {
            const Source* s = FindSource(sources_, i == 0 ? a_id_ : b_id_);
            image_[i] = nullptr;
            if (!s) {
                image_state_[i] = ImageStore::State::FAILED;
                image_error_[i] = "no source";
                continue;
            }
            image_[i] = images_.Get(*s, capture_->width, capture_->height, image_state_[i], image_error_[i]);
            keep.push_back(ImageKey(*s));
        }
        images_.Pump(keep);
        SaveIfDue(false);
        PollWrites();
        ready = (image_[0] || image_state_[0] != ImageStore::State::LOADING) && (image_[1] || image_state_[1] != ImageStore::State::LOADING);
    }
    // Scripted actions, one per frame, each when what the last one started has settled
    if (script_wait_ && !jobs_->Active())
        script_wait_ = false;
    if (!script_queue_.empty() && !script_wait_ && tasks_.empty() && writes_.empty() && ready && !dialogs_.busy()) {
        const auto [name, value] = script_queue_.front();
        script_queue_.erase(script_queue_.begin());
        if (name == "cap-wait")
            script_wait_ = jobs_->Active();
        else
            RunScript("do:" + name, value);
    }
}

void CaptureStudio::DrawWorkspace(std::array<int, 4>& view, uint32_t window_w, uint32_t window_h) {
    if (!capture_)
        return;
    if (show_ui_) {
        DrawMenuBar();
        const ImGuiID dock = ImGui::GetID("NRCapturesDock");
        if (reset_layout_ || !renderer::DockLayoutExists(dock)) {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float panel = std::clamp(400.0f * (ImGui::GetFontSize() / 15.0f) / std::max(vp->WorkSize.x, 1.0f), 0.18f, 0.36f);
            const float bottom = std::clamp(190.0f * (ImGui::GetFontSize() / 15.0f) / std::max(vp->WorkSize.y, 1.0f), 0.12f, 0.35f);
            const char* right[] = {"Capture##cap", "Sources##cap", "Generators##cap", "Marks##cap", "Save##cap", "Display##cap"};
            const char* below[] = {"Jobs##cap"};
            renderer::BuildDockLayout(dock, panel, right, 6, below, 1, bottom);
            layout_focus_ = 3;
        }
        reset_layout_ = false;
        {  // the dock space in a host window of its own (DockSpaceOverViewport's is shared with the video panels' dock space)
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::SetNextWindowViewport(vp->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::Begin("##CapturesDockHost", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                             ImGuiWindowFlags_NoBackground);
            ImGui::PopStyleVar(3);
            ImGui::DockSpace(dock, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::End();
        }
        if (layout_focus_ == 1)
            ImGui::SetNextWindowFocus();
        DrawCapturePanel();
        DrawSourcesPanel();
        DrawGeneratorsPanel();
        DrawMarksPanel();
        DrawSavePanel();
        DrawDisplayPanel();
        DrawJobsPanel();
        if (!ui_shown_)
            ImGui::SetWindowFocus(nullptr);
        ui_shown_ = true;
        if (layout_focus_ > 0)
            layout_focus_--;
        ImVec2 pos, size;
        if (renderer::DockCentralNode(dock, pos, size) && size.x >= 64.0f && size.y >= 64.0f)
            view_ = {(int)pos.x, (int)pos.y, (int)size.x, (int)size.y};
        else
            view_ = {0, 0, (int)window_w, (int)window_h};
    } else {
        view_ = {0, 0, (int)window_w, (int)window_h};
        ui_shown_ = false;
    }
    view = view_;
    const int w = (int)ImageWidth(), h = (int)ImageHeight();
    current_layout_ = compare_layout(ShownLayout(), ViewRect(), divider_);
    for (int i = 0; i < 2; i++)
        mapping_[i] = map_camera(current_layout_.area[i], w, h, camera_, fit_scale(current_layout_.area[i], w, h));
    HandleKeys();
    if (!capture_)  // Esc went back to the browser
        return;
    HandleInput();
    DrawOverlay();
}

// --- Scripts ----------------------------------------------------------------------------------------------------------

bool CaptureStudio::RunScript(const std::string& raw_name, const std::string& value) {
    std::string name = raw_name;
    if (name.rfind("do:", 0) == 0) {
        name = name.substr(3);
    } else if (name.rfind("cap-", 0) == 0) {  // queued: run in order when what came before has settled
        script_queue_.push_back({name, value});
        return true;
    } else {
        return false;
    }
    auto args = [&](const char* format, double* out, int n) { return std::sscanf(value.c_str(), format, out, out + 1, out + 2, out + 3) >= n; };
    if (name == "cap-open") {
        if (!scanned_)
            Rescan();
        for (const Entry& e : entries_)
            if (e.info.name == value || e.info.folder == value)
                Open(e.info.folder);
        if (!capture_)
            Notify("cap-open: no capture " + value, true);
    } else if (name == "cap-pair") {
        for (size_t i = 0; i < pairs_.size(); i++)
            if (pairs_[i].Only() == value)
                SelectPair((int)i);
    } else if (name == "cap-a" || name == "cap-b") {
        SetSource(name == "cap-a" ? 0 : 1, value);
    } else if (name == "cap-run") {  // GENERATOR[:all]; "apex" is the default curated release
        const bool all = value.size() > 4 && value.compare(value.size() - 4, 4, ":all") == 0;
        const std::string id = all ? value.substr(0, value.size() - 4) : value;
        const Generator* pick = nullptr;
        for (const Generator& g : generators_)
            if (!pick && (g.id == id || (id == "apex" && g.kind == Generator::APEX && (g.is_default || g.replay)) ||
                          (id == "dlss-g" && g.kind == Generator::DLSSG && g.available)))
                pick = &g;
        if (!pick) {
            Notify("cap-run: no generator " + id, true);
        } else if (!pick->available) {
            Notify("cap-run: " + pick->label + " is unavailable: " + pick->unavailable, true);
        } else {
            RunGenerator(*pick, all);
        }
    } else if (name == "cap-view") {
        if (value == "diff")
            diff_ = true;
        else
            SetLayout(value == "sbs" || value == "side-by-side" ? Layout::side_by_side
                      : value == "a"                            ? Layout::a_only
                      : value == "b"                            ? Layout::b_only
                                                                : Layout::slider);
    } else if (name == "cap-zoom") {
        camera_.z = std::atof(value.c_str());
    } else if (name == "cap-camera") {
        double v[4] = {0.5, 0.5, 1.0, 0};
        if (args("%lf:%lf:%lf", v, 3))
            camera_ = Camera{v[0], v[1], v[2]};
    } else if (name == "cap-divider") {
        divider_ = std::atof(value.c_str());
    } else if (name == "cap-mark") {
        double v[4] = {0, 0, 0, 0};
        if (args("%lf:%lf:%lf:%lf", v, 4))
            AddMark(v);
    } else if (name == "cap-label") {  // a:TEXT or b:TEXT
        (value[0] == 'a' ? project_.label_a : project_.label_b) = value.substr(2);
        MarkDirty();
    } else if (name == "cap-logos") {
        project_.logos.enabled = value != "0";
        project_.logos.stored = true;
        MarkDirty();
    } else if (name == "cap-save" || name == "cap-crops" || name == "cap-all") {
        // FILE_OR_FOLDER[:HEIGHT]
        const size_t colon = value.rfind(':');
        int height = 0;
        std::string path = value;
        if (colon != std::string::npos && colon + 1 < value.size() && std::isdigit((unsigned char)value[colon + 1])) {
            height = std::atoi(value.c_str() + colon + 1);
            path = value.substr(0, colon);
        }
        if (name == "cap-save") {
            SaveTask t;
            t.kind = SaveTask::FULL;
            t.path = path;
            t.height = height;
            QueueSave(t);
        } else if (name == "cap-crops") {
            QueueCrops(path, height);
        } else {
            QueueSaveAll(path, height);
        }
    } else if (name == "cap-log") {
        spdlog::info("captures state: capture {} pair {} A {} B {} sources {} marks {} view {} zoom {:.2f} jobs {}",
                     capture_ ? capture_->name : "-", StemOfPair(), a_id_, b_id_, sources_.size(), project_.areas.size(),
                     diff_ ? "diff" : std::to_string((int)layout_), camera_.z, jobs_->Snapshot().size());
        for (const Job& j : jobs_->Snapshot())
            spdlog::info("captures job {}: {} {} {}/{} {:.1f}s", j.id, j.spec.Title(), JobStateName(j.state), j.done, j.total, j.seconds);
        for (const Source& s : sources_)
            spdlog::info("captures source {}: {} {}", s.id, s.label, s.stale ? "(stale)" : "");
    } else if (name == "cap-shot") {  // the window's pixels (FILE.bin and FILE.json), for looking at the screen
        if (host_.capture)
            host_.capture(value);
    } else if (name == "cap-quit") {
        if (host_.quit)
            host_.quit();
    } else {
        spdlog::warn("--script: unknown action {}", name);
    }
    return true;
}

void CaptureStudio::UpdateGenerators() {
    if (!capture_)
        return;
    const std::string device = host_.gfx->device_name;
    const bool nvidia = device.find("NVIDIA") != std::string::npos || device.find("GeForce") != std::string::npos;
    generators_ = ListGenerators(where_, state_, steam_, *capture_, device, nvidia);
}

void CaptureStudio::ShowDialog(Dialogs::Kind kind, const std::string& location) {
    if (!dialogs_.Show(kind, host_.window, location))
        Notify("a dialog is open already", true);
}

void CaptureStudio::TakeDialog() {
    auto result = dialogs_.Take();
    if (!result)
        return;
    if (result->error) {
        browser_note_ = "No file dialog here (" + result->message + "): type the path.";
        typing_ = true;
        return;
    }
    if (result->path.empty())
        return;
    switch (result->kind) {
        case Dialogs::Kind::CAPTURE_FOLDER:
            if (std::find(state_.capture_folders.begin(), state_.capture_folders.end(), result->path) == state_.capture_folders.end())
                state_.capture_folders.push_back(result->path);
            SaveState(where_, state_);
            Rescan();
            break;
        case Dialogs::Kind::EXPORT_FOLDER: {
            const FolderFormat* format = capture_ ? FormatOf(*capture_) : nullptr;
            const std::string needs = format ? format->add_needs : std::string();
            if (!needs.empty() && !fs::exists(result->path + "/" + needs)) {
                Notify(result->path + " has no " + needs, true);
                break;
            }
            if (std::find(state_.export_folders.begin(), state_.export_folders.end(), result->path) == state_.export_folders.end())
                state_.export_folders.push_back(result->path);
            SaveState(where_, state_);
            if (capture_) {
                UpdateGenerators();
                RefreshResults();
            }
            break;
        }
        case Dialogs::Kind::NVIDIA_DLL:
        case Dialogs::Kind::PROVIDER_DLL: {
            const std::string kind = result->kind == Dialogs::Kind::NVIDIA_DLL ? "nvidia" : "provider";
            bool known = false;
            for (const State::Dll& d : state_.dlls)
                known = known || (d.path == result->path && d.kind == kind);
            if (!known)
                state_.dlls.push_back({kind, result->path});
            SaveState(where_, state_);
            if (capture_) {
                UpdateGenerators();
                RefreshResults();
            }
            break;
        }
        case Dialogs::Kind::SAVE_IMAGE: {
            SaveTask t;
            t.kind = SaveTask::FULL;
            t.path = result->path;
            if (t.path.size() < 4 || t.path.substr(t.path.size() - 4) != ".png")
                t.path += ".png";
            QueueSave(t);
            break;
        }
        case Dialogs::Kind::SAVE_FOLDER:
            if (folder_crops_only_)
                QueueCrops(result->path, 0);
            else
                QueueSaveAll(result->path, 0);
            break;
    }
}

}  // namespace renderer::clips::captures
