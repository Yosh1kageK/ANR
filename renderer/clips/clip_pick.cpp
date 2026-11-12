// The clip viewer's way to take two recordings: the empty start (two cards, the Recent list), the File menu and buttons,
// the feedback of a file dragged over the window, and the notice line. The decisions are clip_session.cpp's.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>
#include "renderer/clips/clip_viewer.h"
#include "renderer/clips/exporter.h"
#include "renderer/mode_switch.h"
#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace fs = std::filesystem;

namespace {

const ImVec4 WARNING(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 GOOD(0.55f, 0.9f, 0.6f, 1.0f);
const ImVec4 BAD(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 ACCENT(0.35f, 0.65f, 1.0f, 1.0f);

int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Name(const std::string& path) {
    return fs::path(path).filename().string();
}

std::string FormatLength(double seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%d:%04.1f", (int)(seconds / 60.0), seconds - 60.0 * (int)(seconds / 60.0));
    return text;
}

}  // namespace

// --- The path field (where the system has no file dialog) -----------------------------------------------------------------

bool ClipViewer::PathEntry(const char* id, std::string& text, const char* button) {
    ImGui::PushID(id);
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
    const float button_width = ImGui::CalcTextSize(button).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - button_width - ImGui::GetStyle().ItemSpacing.x, 80.0f));
    bool pressed = ImGui::InputTextWithHint("##path", "path of the file", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue);
    text = buffer;
    ImGui::SameLine();
    if (ImGui::Button(button))
        pressed = true;
    ImGui::PopID();
    return pressed && !text.empty();
}

// --- The cards of the empty start --------------------------------------------------------------------------------------

void ClipViewer::DrawCard(int side, float width, float height) {
    const bool over = drop_.hovering() && drop_.x() >= 0.0 && DropSide(drop_.x()) == side;
    const FileDialog::Kind kind = side == 0 ? FileDialog::Kind::VIDEO_A : FileDialog::Kind::VIDEO_B;
    const float line = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_Border, over ? ACCENT : ImGui::GetStyleColorVec4(ImGuiCol_Border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, over ? 3.0f : 1.0f);
    ImGui::BeginChild(side == 0 ? "card A" : "card B", ImVec2(width, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
    ImGui::TextColored(side == 0 ? ImVec4(0.95f, 0.7f, 0.3f, 1.0f) : ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "%s", side == 0 ? "A" : "B");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(side == 0 ? "left of the divider" : "right of the divider");
    const Pick& pick = pick_[side];
    const bool opening = open_request_.has_value();
    if (!pick.path.empty()) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
        ImGui::TextWrapped("%s", Name(pick.path).c_str());
        ImGui::PopFont();
        const VideoInfo& info = pick.probe.info;
        ImGui::TextDisabled("%ux%u, %.3f fps, %s, %s %u-bit", info.width, info.height, info.fps,
                            FormatLength(pick.probe.duration).c_str(), info.codec.c_str(), info.bit_depth);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", pick.path.c_str());
        ImGui::BeginDisabled(dialogs_.busy() || opening);
        if (ImGui::Button("Choose another..."))
            ShowDialog(kind);
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
            pick_[side] = Pick();
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("No video chosen");
        ImGui::BeginDisabled(dialogs_.busy() || opening);
        if (ImGui::Button("Choose video...", ImVec2(line * 11.0f, line * 1.9f)))
            ShowDialog(kind);
        ImGui::EndDisabled();
        if (dialogs_.busy())
            ImGui::TextColored(WARNING, "A file dialog is open.");
        else
            ImGui::TextDisabled("or drop a video here");
    }
    if (typing_) {
        if (dialogs_failed_)
            ImGui::TextColored(WARNING, "No file dialog here: type the path.");
        if (PathEntry(side == 0 ? "path A" : "path B", typed_[side], "Open"))
            ChooseVideo(side, UserPath(ExpandPath(typed_[side], "")));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void ClipViewer::DrawPicker() {
    const float line = ImGui::GetFontSize();
    const ImVec2 pos((float)view_[0], (float)view_[1]), size((float)view_[2], (float)view_[3]);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.075f, 0.09f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(line * 1.5f, line * 1.2f));
    ImGui::Begin("##picker", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNav);
    // Two tabs: the videos (below) and the game captures (renderer/clips/captures)
    const bool tabs = ImGui::BeginTabBar("##start");
    bool videos_tab = true;
    on_captures_tab_ = false;  // the Mode menu ticks the mode whose tab shows
    if (tabs) {
        const ImGuiTabItemFlags videos_flags = want_videos_tab_ ? ImGuiTabItemFlags_SetSelected : 0;
        want_videos_tab_ = false;
        videos_tab = ImGui::BeginTabItem("Videos", nullptr, videos_flags);
        if (videos_tab) {
            // fall through: the videos' cards and Recent list are drawn below, inside this tab
        }
    }
    if (tabs && !videos_tab) {
        const ImGuiTabItemFlags flags = want_captures_tab_ ? ImGuiTabItemFlags_SetSelected : 0;
        want_captures_tab_ = false;
        if (ImGui::BeginTabItem("Captures", nullptr, flags)) {
            on_captures_tab_ = true;
            if (EnsureCaptures())
                captures_->DrawBrowser();
            else
                ImGui::TextDisabled("The Captures tab cannot start (see the log).");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
    ImGui::TextUnformatted("Compare two videos");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, line * 0.2f));
    const float gap = ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const float card_width = (ImGui::GetContentRegionAvail().x - gap) / 2.0f;
    const float card_height = line * (typing_ ? 12.0f : 8.6f);
    DrawCard(0, card_width, card_height);
    ImGui::SameLine(0.0f, gap);
    DrawCard(1, card_width, card_height);
    ImGui::Dummy(ImVec2(0.0f, line * 0.3f));
    ImGui::BeginDisabled(dialogs_.busy() || open_request_.has_value());
    if (ImGui::Button("Open project..."))
        ShowDialog(FileDialog::Kind::PROJECT_OPEN);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(typing_ ? "Hide the path fields" : "Type a path..."))
        typing_ = !typing_;
    if (typing_) {
        ImGui::TextDisabled("Project file:");
        ImGui::SameLine();
        if (PathEntry("path project", typed_[2], "Open project"))
            OpenProjectFile(UserPath(ExpandPath(typed_[2], "")));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Drop a file on each half of the window (left A, right B), or both at once. A .compare.json opens a project.");
    ImGui::PopStyleColor();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGui::TextUnformatted("Recent");
    ImGui::PopFont();
    DrawRecent(false);
    if (tabs) {
        ImGui::EndTabItem();
        // the Captures tab, after the Videos tab's content
        const ImGuiTabItemFlags flags = want_captures_tab_ ? ImGuiTabItemFlags_SetSelected : 0;
        want_captures_tab_ = false;
        if (ImGui::BeginTabItem("Captures", nullptr, flags)) {
            on_captures_tab_ = true;
            if (EnsureCaptures())
                captures_->DrawBrowser();
            else
                ImGui::TextDisabled("The Captures tab cannot start (see the log).");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// --- Recent -----------------------------------------------------------------------------------------------------------

void ClipViewer::DrawRecent(bool compact) {
    const auto& entries = registry_.entries();
    if (entries.empty()) {
        ImGui::TextDisabled("Pairs you open show here.");
        return;
    }
    const int64_t now = UnixNow();
    int remove = -1;
    const bool blocked = open_request_.has_value() || (exporter_ && exporter_->running());
    const int columns = compact ? 4 : 5;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV;
    const float height = compact ? ImGui::GetFontSize() * 12.0f : std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFontSize() * 1.6f, ImGui::GetFontSize() * 5.5f);
    if (!ImGui::BeginTable(compact ? "recent-compact" : "recent", columns, flags, ImVec2(0.0f, height)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    if (!compact)
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Used", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * (compact ? 6.5f : 9.0f));
    ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.0f);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < entries.size(); i++) {
        const RecentPair& entry = entries[i];
        const PairStatus status = StatusOf(entry);
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!status.usable() || blocked);
        if (ImGui::Selectable(Name(entry.a).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            OpenRecent(entry);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("A  %s\nB  %s\nproject  %s%s", entry.a.c_str(), entry.b.c_str(), entry.project.c_str(),
                              status.project_exists ? "" : " (not there: a new one is made, and the offset detected again)");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(Name(entry.b).c_str());
        if (!compact) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", fs::path(entry.a).parent_path().string().c_str());
        }
        ImGui::TableNextColumn();
        if (!status.a_exists || !status.b_exists)
            ImGui::TextColored(BAD, "%s", !status.a_exists && !status.b_exists ? "A and B missing" : !status.a_exists ? "A missing" : "B missing");
        else
            ImGui::TextDisabled("%s", FormatAgo(now, entry.last_used).c_str());
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("x"))
            remove = (int)i;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove from the list (the files and the project stay)");
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove >= 0) {
        const RecentPair entry = entries[(size_t)remove];
        registry_.Remove(entry.a, entry.b);
        SaveRegistry();
    }
}

// --- The File menu and buttons ----------------------------------------------------------------------------------------

void ClipViewer::DrawFileMenu() {
    if (!ImGui::BeginMenu("File"))
        return;
    const bool exporting = exporter_ && exporter_->running();
    const bool busy = dialogs_.busy() || open_request_.has_value();
    ImGui::BeginDisabled(busy || exporting);
    if (ImGui::MenuItem(loaded_ ? "Change A..." : "Choose A..."))
        ShowDialog(FileDialog::Kind::VIDEO_A);
    if (ImGui::MenuItem(loaded_ ? "Change B..." : "Choose B..."))
        ShowDialog(FileDialog::Kind::VIDEO_B);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!loaded_ || exporting || open_request_.has_value());
    if (ImGui::MenuItem("Swap A and B"))
        SwapRecordings();
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::BeginDisabled(busy || exporting);
    if (ImGui::MenuItem("Open project..."))
        ShowDialog(FileDialog::Kind::PROJECT_OPEN);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(busy || !loaded_);
    if (ImGui::MenuItem("Save project as..."))
        ShowDialog(FileDialog::Kind::PROJECT_SAVE);
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::BeginMenu("Recent", !registry_.entries().empty() && !exporting && !open_request_)) {
        const auto entries = registry_.entries();  // a copy: opening one reorders the list
        for (size_t i = 0; i < entries.size(); i++) {
            const PairStatus status = StatusOf(entries[i]);
            const std::string label = Name(entries[i].a) + "  +  " + Name(entries[i].b) + "##recent" + std::to_string(i);
            if (ImGui::MenuItem(label.c_str(), nullptr, false, status.usable()))
                OpenRecent(entries[i]);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s\n%s%s", entries[i].a.c_str(), entries[i].b.c_str(), status.usable() ? "" : "\n(a file is missing)");
        }
        ImGui::EndMenu();
    }
    if (dialogs_.busy())
        ImGui::TextDisabled("A file dialog is open.");
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Esc"))
        close_ = true;
    ImGui::EndMenu();
}

// The file actions as buttons (the Clips panel's top).
void ClipViewer::DrawFileButtons() {
    const bool exporting = exporter_ && exporter_->running();
    const bool busy = dialogs_.busy() || open_request_.has_value();
    ImGui::BeginDisabled(!loaded_ || exporting || open_request_.has_value());
    if (ImGui::Button("Swap A and B"))
        SwapRecordings();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A and B change places, with their labels; the offset changes sign and the marks stay on the same moments.");
    // the next button goes beside the one before when it fits, else on the next line
    auto beside = [](const char* label) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
        if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - style.WindowPadding.x)
            ImGui::SameLine();
    };
    beside("Open project...");
    ImGui::BeginDisabled(busy || exporting);
    if (ImGui::Button("Open project..."))
        ShowDialog(FileDialog::Kind::PROJECT_OPEN);
    ImGui::EndDisabled();
    beside("Save project as...");
    ImGui::BeginDisabled(busy || !loaded_);
    if (ImGui::Button("Save project as..."))
        ShowDialog(FileDialog::Kind::PROJECT_SAVE);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && loaded_)
        ImGui::SetTooltip("A copy of the project, with the recordings' full paths. Edits go on saving to\n%s", project_path_.c_str());
    if (dialogs_.busy())
        ImGui::TextColored(WARNING, "A file dialog is open.");
}

// --- Drag feedback and the notice -------------------------------------------------------------------------------------

void ClipViewer::DrawDropOverlay() {
    if (!drop_.hovering())
        return;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const float scale = ImGui::GetFontSize();
    const double x = drop_.x();
    ImVec2 p0(0.0f, 0.0f), p1(io.DisplaySize.x, io.DisplaySize.y);
    std::string title = "Drop the videos here", detail = "one file on each half sets A or B; two files set A and B; a .compare.json opens a project";
    if (x >= 0.0) {
        const int side = DropSide(x);
        const char* name = side == 0 ? "A" : "B";
        if (side == 0)
            p1.x = io.DisplaySize.x * 0.5f;
        else
            p0.x = io.DisplaySize.x * 0.5f;
        title = std::string(loaded_ ? "Change " : "Set ") + name;
        detail = std::string("drop one video to use it as ") + name + "; two videos set A and B";
    }
    list->AddRectFilled(p0, p1, IM_COL32(50, 120, 230, 70));
    const float inset = scale * 0.6f;
    list->AddRect(ImVec2(p0.x + inset, p0.y + inset), ImVec2(p1.x - inset, p1.y - inset), IM_COL32(110, 175, 255, 240), scale * 0.8f,
                  0, std::max(3.0f, scale * 0.3f));
    ImFont* font = ImGui::GetFont();
    const float big = scale * 2.6f;
    const ImVec2 t = font->CalcTextSizeA(big, 1e9f, 0.0f, title.c_str());
    const ImVec2 d = font->CalcTextSizeA(scale * 1.1f, 1e9f, 0.0f, detail.c_str());
    const float cx = (p0.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
    list->AddRectFilled(ImVec2(cx - std::max(t.x, d.x) / 2.0f - scale, cy - t.y / 2.0f - scale),
                        ImVec2(cx + std::max(t.x, d.x) / 2.0f + scale, cy + t.y / 2.0f + d.y + scale * 1.2f), IM_COL32(10, 14, 24, 215),
                        scale * 0.6f);
    list->AddText(font, big, ImVec2(cx - t.x / 2.0f, cy - t.y / 2.0f), IM_COL32(255, 255, 255, 255), title.c_str());
    list->AddText(font, scale * 1.1f, ImVec2(cx - d.x / 2.0f, cy + t.y / 2.0f + scale * 0.4f), IM_COL32(190, 205, 230, 255), detail.c_str());
}

void ClipViewer::DrawNotice() {
    const float line = ImGui::GetFontSize();
    const float width = std::min((float)view_[2] * 0.8f, line * 60.0f);
    if (open_request_) {  // the open waits a few frames: this is what shows meanwhile
        const std::string text = "Opening " + Name(open_request_->a) + " and " + Name(open_request_->b) + " ...";
        ImGui::SetNextWindowPos(ImVec2((float)view_[0] + view_[2] * 0.5f, (float)view_[1] + view_[3] * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(line, line * 0.7f));
        ImGui::Begin("##opening", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar();
    }
    if (notice_.text.empty())
        return;
    if (Seconds() - notice_.time > (notice_.bad ? 60.0 : 8.0)) {
        notice_.text.clear();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2((float)view_[0] + view_[2] * 0.5f, (float)view_[1] + line * 0.8f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(width + line * 4.0f, 1e9f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, notice_.bad ? ImVec4(0.30f, 0.09f, 0.08f, 0.96f) : ImVec4(0.08f, 0.2f, 0.12f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, notice_.bad ? BAD : GOOD);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(line * 0.8f, line * 0.6f));
    ImGui::Begin("##notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextColored(notice_.bad ? ImVec4(1.0f, 0.85f, 0.82f, 1.0f) : ImVec4(0.85f, 1.0f, 0.88f, 1.0f), "%s", notice_.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss"))
        notice_.text.clear();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// --- Mode -------------------------------------------------------------------------------------------------------------

// The mode of this window: the compare mode, or the captures mode on the start screen's Captures tab and in an open capture.
renderer::Mode ClipViewer::CurrentMode() const {
    if (captures_ && captures_->open())
        return renderer::Mode::CAPTURES;
    if (loaded_)
        return renderer::Mode::COMPARE;
    return on_captures_tab_ ? renderer::Mode::CAPTURES : renderer::Mode::COMPARE;
}

// The start screen's Videos and Captures tabs are the compare and captures modes: no pair and no capture is open, so the
// switch is a tab, and nothing is lost.
bool ClipViewer::SwitchesInPlace(renderer::Mode target) const {
    return target != renderer::Mode::SCENE && !loaded_ && !(captures_ && captures_->open());
}

// What a switch ends that is not saved. The compare modes autosave (the project is written first); a running export and a
// capture job or save are asked about.
std::string ClipViewer::LosesOnSwitch(renderer::Mode target) const {
    if (SwitchesInPlace(target))
        return {};
    if (exporter_ && exporter_->running())
        return "A video export is running.";
    return captures_ ? captures_->BusyReason() : std::string();
}

void ClipViewer::SwitchMode(renderer::Mode target) {
    if (target == CurrentMode())
        return;
    if (SwitchesInPlace(target)) {
        (target == renderer::Mode::CAPTURES ? want_captures_tab_ : want_videos_tab_) = true;
        renderer::RememberMode(target);
        spdlog::info("mode: {} (the start screen's tab)", renderer::Info(target).name);
        return;
    }
    SaveIfDue(true);
    std::string error;
    if (!renderer::RequestMode(target, error)) {
        Notify(error, true);
        return;
    }
    close_ = true;  // like a quit: main starts the mode once the window and the device are gone
}

}  // namespace renderer::clips
