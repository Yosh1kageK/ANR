// The Captures tab's docked panels: Capture (the pair), Sources (A and B and their labels), Generators (DLSS-G, ANR FG),
// Marks, Save and Display on the right, Jobs along the bottom, and the menu bar and the keys.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>
#include "renderer/clips/camera.h"
#include "renderer/clips/captures/recording.h"
#include "renderer/clips/captures/studio.h"
#include "renderer/dock_layout.h"
#include "renderer/display.h"
#include "renderer/mode_switch.h"
#include <spdlog/spdlog.h>

namespace renderer::clips::captures {
namespace fs = std::filesystem;
namespace {

const ImVec4 WARNING(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 GOOD(0.55f, 0.9f, 0.6f, 1.0f);
const ImVec4 BAD(1.0f, 0.45f, 0.4f, 1.0f);

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

const char* CodingName(Coding c) {
    switch (c) {
        case Coding::SRGB8: return "sRGB codes";
        case Coding::PQ10: return "BT.2020 PQ";
        case Coding::SCRGB16: return "scRGB";
        case Coding::VIZ: return "false colour";
    }
    return "";
}

}  // namespace

void CaptureStudio::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar())
        return;
    // The host asks about a running job or save and starts the other mode; a capture is open, so every other mode restarts
    if (const auto mode = renderer::DrawModeMenu(renderer::Mode::CAPTURES, [this](renderer::Mode) { return BusyReason(); }))
        if (host_.switch_mode)
            host_.switch_mode(*mode);
    if (ImGui::BeginMenu("Captures")) {
        if (ImGui::MenuItem("Back to the browser", "Esc"))
            Close();
        if (ImGui::MenuItem("Add a capture folder..."))
            ShowDialog(Dialogs::Kind::CAPTURE_FOLDER, RecordingsFolder());
        const FolderFormat* format = capture_ ? FormatOf(*capture_) : nullptr;
        if (format && !format->add_label.empty() && ImGui::MenuItem(format->add_label.c_str()))
            ShowDialog(Dialogs::Kind::EXPORT_FOLDER, where_.workspace + "/" + format->add_location);
        if (capture_ && capture_->format == RECORDING_FORMAT) {
            if (ImGui::MenuItem("Add NVIDIA's DLSS-G DLL..."))
                ShowDialog(Dialogs::Kind::NVIDIA_DLL, "");
            if (ImGui::MenuItem("Add a provider DLL..."))
                ShowDialog(Dialogs::Kind::PROVIDER_DLL, "");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Divider", "1", !diff_ && layout_ == Layout::slider))
            SetLayout(Layout::slider);
        if (ImGui::MenuItem("Side by side", "2", !diff_ && layout_ == Layout::side_by_side))
            SetLayout(Layout::side_by_side);
        if (ImGui::MenuItem("A only", "3", !diff_ && layout_ == Layout::a_only))
            SetLayout(Layout::a_only);
        if (ImGui::MenuItem("B only", "4", !diff_ && layout_ == Layout::b_only))
            SetLayout(Layout::b_only);
        if (ImGui::MenuItem("Difference", "5", diff_))
            diff_ = true;
        ImGui::Separator();
        ImGui::MenuItem("Show marks", nullptr, &show_marks_);
        if (ImGui::MenuItem("Reset zoom", "0"))
            camera_ = Camera();
        ImGui::Separator();
        if (ImGui::MenuItem("Fullscreen", "F11") && host_.toggle_fullscreen)
            host_.toggle_fullscreen();
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
    if (capture_)
        ImGui::TextDisabled("%s  %s", capture_->game.c_str(), capture_->name.c_str());
    if (pair_index_ >= 0) {
        ImGui::SameLine();
        ImGui::Text("%s", pairs_[(size_t)pair_index_].Label(capture_->multiplier).c_str());
    }
    if (host_.swap->encoding() != SRGB8) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s, paper white %.0f", host_.swap->encoding() == PQ10 ? "HDR10" : "scRGB", host_.swap->hdr.paper_white);
    }
    if (jobs_->Active()) {
        ImGui::SameLine();
        ImGui::TextColored(WARNING, "jobs running");
    }
    if (!tasks_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(GOOD, "%s", save_status_.c_str());
    }
    ImGui::EndMainMenuBar();
}

void CaptureStudio::DrawKeys() {
    const char* keys[][2] = {{"1 2 3 4 5", "divider, side by side, A only, B only, difference"},
                             {"Tab", "switch between A and B (single views)"},
                             {"Left, Right", "the previous or next pair"},
                             {"[ ]", "move the divider by 16 pixels"},
                             {"wheel, + -", "zoom around the pointer (1x to 16x), nearest-neighbour"},
                             {"0", "back to the whole image"},
                             {"left drag", "move the divider (divider view), pan (other views)"},
                             {"right or middle drag", "pan in every view"},
                             {"R", "mark a rectangle: drag on the picture"},
                             {"Delete", "delete the selected mark"},
                             {"F1", "hide or show the UI"},
                             {"F11", "fullscreen"},
                             {"Esc", "cancel marking, else back to the browser"}};
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

// --- Capture ----------------------------------------------------------------------------------------------------------

void CaptureStudio::DrawCapturePanel() {
    if (!ImGui::Begin("Capture##cap") || !capture_) {
        ImGui::End();
        return;
    }
    const CaptureInfo& c = *capture_;
    ImGui::Text("%s", c.game.c_str());
    ImGui::TextDisabled("%s  %s", DisplayTime(c.time).c_str(), c.name.c_str());
    ImGui::TextDisabled("%ux%u, %dX, %s, %d real frames", c.width, c.height, c.multiplier, c.encoding.c_str(), (int)c.frames.size());
    ImGui::TextDisabled("in game: %s", c.in_game.empty() ? "unknown" : c.in_game.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("What recorded the capture or made the game's own generated frames, as the capture says.");
    ImGui::Separator();
    ImGui::TextUnformatted("Pair");
    std::string current = pair_index_ >= 0 ? pairs_[(size_t)pair_index_].Label(c.multiplier) : "";
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##pair", current.c_str())) {
        for (size_t i = 0; i < pairs_.size(); i++) {
            if (i > 0 && pairs_[i].kind != pairs_[i - 1].kind)
                ImGui::Separator();
            if (ImGui::Selectable(pairs_[i].Label(c.multiplier).c_str(), (int)i == pair_index_))
                SelectPair((int)i);
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("< Previous"))
        SelectPair(pair_index_ - 1);
    ImGui::SameLine();
    if (ImGui::Button("Next >"))
        SelectPair(pair_index_ + 1);
    if (pair_index_ >= 0) {
        const PairSpec& p = pairs_[(size_t)pair_index_];
        if (p.kind == PairKind::REAL) {
            ImGui::TextWrapped("A real pair: the game's frames %d and %d, and the generated frame between them at the slot's time. "
                               "DLSS-G and ANR FG get the same pair and t.", p.a, p.b);
        } else {
            ImGui::TextColored(WARNING, "Skip-one (approximate): frame %d is generated from %d and %d at t = 1/2 and compared with the "
                                        "rendered frame %d. The rendered frame is a different render: an approximate reference, its timing "
                                        "is not verified.", p.rendered, p.a, p.b, p.rendered);
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("Captures are evaluation data: nothing here writes into a capture folder.");
    ImGui::End();
}

// --- Sources ----------------------------------------------------------------------------------------------------------

void CaptureStudio::DrawSourcesPanel() {
    if (!ImGui::Begin("Sources##cap") || !capture_) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("sources", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("A");
        ImGui::TableSetupColumn("B");
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const Source& s : sources_) {
            ImGui::TableNextRow();
            ImGui::PushID(s.id.c_str());
            ImGui::TableNextColumn();
            if (ImGui::RadioButton("##a", a_id_ == s.id))
                SetSource(0, s.id);
            ImGui::TableNextColumn();
            if (ImGui::RadioButton("##b", b_id_ == s.id))
                SetSource(1, s.id);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.label.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s", s.path.c_str(), CodingName(s.coding));
            ImGui::TableNextColumn();
            if (s.coding == Coding::VIZ)
                ImGui::TextDisabled("false colour");
            else if (s.raw)
                ImGui::TextDisabled("generated");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Labels (on the picture and the saved images)");
    if (TextInput("A##label", project_.label_a, ImGui::GetFontSize() * 15.0f))
        MarkDirty();
    if (TextInput("B##label", project_.label_b, ImGui::GetFontSize() * 15.0f))
        MarkDirty();
    if (ImGui::Button("Swap A and B"))
        SwapSources();
    ImGui::Separator();
    for (int i = 0; i < 2; i++) {
        const GpuImage* image = image_[i];
        ImGui::TextDisabled("%s: %s", i == 0 ? "A" : "B", image ? "" : image_state_[i] == ImageStore::State::FAILED ? image_error_[i].c_str() : "loading...");
        if (image) {
            ImGui::SameLine();
            ImGui::TextDisabled("%ux%u, %s, loaded in %.0f ms", image->width, image->height, CodingName(image->coding), image->load_seconds * 1000.0);
        }
    }
    ImGui::TextDisabled("Images in video memory: %.0f MiB", (double)images_.bytes() / 1048576.0);
    ImGui::End();
}

// --- Generators -------------------------------------------------------------------------------------------------------

void CaptureStudio::DrawGeneratorsPanel() {
    if (!ImGui::Begin("Generators##cap") || !capture_) {
        ImGui::End();
        return;
    }
    const FolderFormat* format = FormatOf(*capture_);
    if (format)
        ImGui::TextWrapped("Recreate the pair's generated frame with another generator. %s The frame is cached and becomes a "
                           "source.", format->about.c_str());
    else
        ImGui::TextWrapped("Recreate the pair's generated frame with another generator. A job runs nr-replay with the "
                           "generator's DLL on the recorded frames (under Wine on Linux) and waits while a Steam game runs. "
                           "The frame is cached and becomes a source.");
    const std::string stem = StemOfPair();
    const PairSpec* pair = pair_index_ >= 0 ? &pairs_[(size_t)pair_index_] : nullptr;
    if (generators_.empty())
        ImGui::TextColored(WARNING, "No generators: add a DLL below, or install a game with DLSS-G or a provider in Steam.");
    const std::vector<Job> jobs = jobs_->Snapshot();
    for (const Generator& g : generators_) {
        ImGui::PushID(g.id.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted(g.label.c_str());
        if (g.curated) {
            ImGui::SameLine();
            ImGui::TextDisabled(g.is_default ? "curated, default" : "curated");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nversion %s\n%s", g.detail.c_str(), g.version.c_str(), g.replay ? g.dll.c_str() : g.export_dir.c_str());
        if (!g.available) {
            ImGui::TextColored(WARNING, "Unavailable: %s", g.unavailable.c_str());
            ImGui::PopID();
            continue;
        }
        // The state of this pair for this generator
        std::string state = "not made";
        ImVec4 colour = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        auto cached = cached_.find(g.id);
        if (cached != cached_.end())
            for (const Cached& f : cached->second)
                if (f.stem == stem) {
                    state = f.stale ? "made by an older tool or version (stale)" : "done (cached)";
                    colour = f.stale ? WARNING : GOOD;
                    break;
                }
        const std::string failed = fs::exists(ResultDir(where_, capture_->name, g) + "/" + stem + ".failed") ? "the last run failed" : "";
        for (const Job& j : jobs)
            if (j.spec.generator.id == g.id && j.spec.capture_name == capture_->name &&
                (j.spec.all || (!j.spec.pairs.empty() && j.spec.pairs[0].Stem() == stem)) &&
                (j.state == JobState::QUEUED || j.state == JobState::WAITING_GAME || j.state == JobState::RUNNING)) {
                state = JobStateName(j.state);
                colour = WARNING;
            }
        if (state == "not made" && !failed.empty()) {
            state = failed;
            colour = BAD;
        }
        ImGui::TextColored(colour, "This pair: %s", state.c_str());
        bool running = false;
        for (const Job& j : jobs)
            running = running || (j.spec.generator.id == g.id && (j.state == JobState::QUEUED || j.state == JobState::RUNNING || j.state == JobState::WAITING_GAME));
        const bool can = pair && CanMake(g, *pair);
        ImGui::BeginDisabled(!can);
        if (ImGui::Button("Run for this pair"))
            RunGenerator(g, false);
        ImGui::EndDisabled();
        if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("nr-replay recreates the recorded pairs, not skip-ones.");
        ImGui::SameLine();
        if (ImGui::Button("Run for the whole capture"))
            RunGenerator(g, true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(g.replay ? "Every recorded pair of the capture." : "Every real pair and every skip-one of the capture.");
        ImGui::PopID();
    }
    ImGui::Separator();
    DrawAddGenerator();
    ImGui::End();
}

void CaptureStudio::DrawAddGenerator() {
    ImGui::BeginDisabled(dialogs_.busy());
    const FolderFormat* format = FormatOf(*capture_);
    if (format) {
        if (!format->add_label.empty()) {
            if (ImGui::Button(format->add_label.c_str()))
                ShowDialog(Dialogs::Kind::EXPORT_FOLDER, where_.workspace + "/" + format->add_location);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", format->add_hint.c_str());
        }
    } else {
        if (ImGui::Button("Add NVIDIA's DLSS-G DLL..."))
            ShowDialog(Dialogs::Kind::NVIDIA_DLL, "");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("An nvngx_dlssg.dll of NVIDIA's (from a game's folder or the DLSS SDK): nr-replay runs it through "
                              "NVIDIA's NGX (an NVIDIA GPU and driver).");
        ImGui::SameLine();
        if (ImGui::Button("Add a provider DLL..."))
            ShowDialog(Dialogs::Kind::PROVIDER_DLL, "");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("A DLSS-G provider's nvngx_dlssg.dll (ANR FG's, from its release): nr-replay calls it as a game "
                              "does.");
    }
    ImGui::EndDisabled();
}

void CaptureStudio::DrawJobsPanel() {
    if (!ImGui::Begin("Jobs##cap") || !capture_) {
        ImGui::End();
        return;
    }
    const std::vector<Job> jobs = jobs_->Snapshot();
    if (jobs.empty())
        ImGui::TextDisabled("No jobs yet. Generators > Run.");
    static int selected = 0;
    for (size_t i = jobs.size(); i-- > 0;) {
        const Job& j = jobs[i];
        ImGui::PushID(j.id);
        const bool active = j.state == JobState::QUEUED || j.state == JobState::WAITING_GAME || j.state == JobState::RUNNING;
        ImVec4 colour = j.state == JobState::DONE ? GOOD : j.state == JobState::FAILED ? BAD : active ? WARNING : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        if (ImGui::Selectable("##row", selected == j.id, 0, ImVec2(0.0f, ImGui::GetTextLineHeight())))
            selected = j.id;
        ImGui::SameLine();
        ImGui::Text("#%d %s: %s", j.id, j.spec.capture_name.c_str(), j.spec.Title().c_str());
        ImGui::SameLine();
        ImGui::TextColored(colour, "%s", j.status.c_str());
        if (j.total > 0) {
            ImGui::SameLine();
            ImGui::ProgressBar((float)j.done / (float)j.total, ImVec2(ImGui::GetFontSize() * 6.0f, ImGui::GetTextLineHeight()));
        }
        ImGui::SameLine();
        if (active && ImGui::SmallButton("Cancel"))
            jobs_->Cancel(j.id);
        if ((j.state == JobState::FAILED || j.state == JobState::CANCELLED) && ImGui::SmallButton("Retry"))
            jobs_->Retry(j.id, *capture_);
        ImGui::PopID();
    }
    for (const Job& j : jobs)
        if (j.id == selected && !j.log.empty()) {
            ImGui::Separator();
            ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            for (const std::string& line : j.log)
                ImGui::TextDisabled("%s", line.c_str());
            if (!j.log_path.empty())
                ImGui::TextDisabled("log: %s", j.log_path.c_str());
            ImGui::EndChild();
        }
    ImGui::End();
}

// --- Marks ------------------------------------------------------------------------------------------------------------

void CaptureStudio::DrawMarksPanel() {
    if (!ImGui::Begin("Marks##cap") || !capture_) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Rectangles on the picture, kept with this capture and pair. On a still they are always shown; Save marked crops "
                       "writes each one's zoomed view.");
    if (ImGui::Button(mark_mode_ ? "Drag a rectangle... (R)" : "Mark a rectangle (R)"))
        mark_mode_ = !mark_mode_;
    ImGui::SameLine();
    ImGui::Checkbox("Show marks", &show_marks_);
    ImGui::Separator();
    for (size_t i = 0; i < project_.areas.size(); i++) {
        Area& a = project_.areas[i];
        ImGui::PushID(a.id);
        const ImU32 colour = AreaColor(i);
        ImGui::ColorButton("##colour", ImGui::ColorConvertU32ToFloat4(colour), ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
        if (ImGui::Selectable(("Mark " + std::to_string(a.id)).c_str(), selected_mark_ == a.id, 0, ImVec2(ImGui::GetFontSize() * 5.0f, 0.0f)))
            selected_mark_ = a.id;
        ImGui::SameLine();
        std::string label = a.label;
        if (TextInput("##label", label, ImGui::GetFontSize() * 11.0f)) {
            a.label = label;
            project_.areas_dirty = true;
            MarkDirty();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("zoom to")) {
            selected_mark_ = a.id;
            camera_ = view_for(a.rect);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("redraw")) {
            DeleteMark(a.id);
            mark_mode_ = true;
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("delete")) {
            DeleteMark(a.id);
            ImGui::PopID();
            break;
        }
        ImGui::TextDisabled("    x %.4f  y %.4f  w %.4f  h %.4f  (%.0f x %.0f px)", a.rect[0], a.rect[1], a.rect[2], a.rect[3],
                            a.rect[2] * (double)ImageWidth(), a.rect[3] * (double)ImageHeight());
        ImGui::PopID();
    }
    if (project_.areas.empty())
        ImGui::TextDisabled("No marks.");
    ImGui::End();
}

// --- Display ----------------------------------------------------------------------------------------------------------

void CaptureStudio::DrawDisplayPanel() {
    if (!ImGui::Begin("Display##cap") || !capture_) {
        ImGui::End();
        return;
    }
    const uint32_t encoding = host_.swap->encoding();
    ImGui::TextWrapped("The swap chain is %s. %s", encoding == SRGB8 ? "SDR" : encoding == PQ10 ? "HDR10 (PQ)" : "scRGB",
                       encoding == PQ10 ? "PQ sources show their codes unchanged; sRGB sources are placed at the paper white."
                                        : "PQ sources go through the SDR curve: PQ to nits, BT.2020 to BT.709, "
                                          "203 nits = 1.0, Reinhard x / (1 + x) per channel, sRGB. sRGB sources show their codes.");
    ImGui::TextDisabled("Zoom %.2fx at %.4f, %.4f", mapping_[0].camera.z, mapping_[0].camera.cx, mapping_[0].camera.cy);
    if (ImGui::Button("Reset zoom"))
        camera_ = Camera();
    ImGui::TextWrapped("From a scale of 1 every screen pixel is one source pixel (nearest, no filtering). Below it the source pixels a "
                       "screen pixel covers are averaged after the display transform.");
    ImGui::Separator();
    DrawLogoSettings();
    ImGui::End();
}

// The logos: the master box and each logo's file, corner, height, margin, opacity and backdrop (the video export's settings).
void CaptureStudio::DrawLogoSettings() {
    bool on = LogosOn();
    if (ImGui::Checkbox("Logos", &on)) {
        project_.logos.enabled = on;
        project_.logos.stored = true;
        MarkDirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Logos at the image's corners, in the saved images and (to judge them) on the view.\n"
                          "Apex bottom-left (A, left of the divider), NVIDIA bottom-right (B, right of it): put the ANR FG frame "
                          "in A and DLSS-G in B, or change the corners here.\nFiles (apex.png, nvidia.png): %s", LogoFolderText().c_str());
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
            if (TextInput("file", path, ImGui::GetFontSize() * 12.0f)) {
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

}  // namespace renderer::clips::captures
