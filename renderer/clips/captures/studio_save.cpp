// Saving: the current composite, one crop per mark, or all of it with a manifest. The composite is drawn by the same offscreen
// composer, overlay (labels, divider, caption, zoom) and logo layer as the video export (renderer/clips/composer.h,
// exporter.cpp); only the picture pass differs (still_pass.h). An SDR PNG is always written; for HDR sources also a 16-bit
// PQ PNG with a cICP chunk and an AVIF through ffmpeg when asked.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <future>

#include <imgui.h>
#include <nlohmann/json.hpp>
#include "renderer/clips/camera.h"
#include "renderer/clips/captures/capture_reader.h"
#include "renderer/clips/captures/png_write.h"
#include "renderer/clips/captures/studio.h"
#include "renderer/clips/composer.h"
#include "renderer/clips/process.h"
#include "renderer/display.h"
#include <spdlog/spdlog.h>

namespace renderer::clips::captures {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// <base>.hdr.png and <base>.hdr.avif beside the SDR file (<base>.png, or <base>.sdr.png)
std::string Base(const std::string& sdr_path) {
    std::string base = sdr_path;
    if (base.size() > 4 && base.substr(base.size() - 4) == ".png")
        base.erase(base.size() - 4);
    if (base.size() > 4 && base.substr(base.size() - 4) == ".sdr")
        base.erase(base.size() - 4);
    return base;
}

// ffmpeg's AVIF of an HDR image: 10-bit 4:4:4 AV1, BT.2020 PQ, full range, libaom still picture
bool WriteAvif(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint16_t>& rgb16, std::string& error) {
    const uint32_t w2 = w + w % 2, h2 = h + h % 2;
    char size[32], crop[64];
    std::snprintf(size, sizeof(size), "%ux%u", w2, h2);
    std::snprintf(crop, sizeof(crop), "crop=%u:%u:0:0,", w, h);
    const std::string tags = "color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=pc";
    const std::vector<std::string> args = {FindTool("ffmpeg", "NR_FFMPEG"), "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
                                           "-pix_fmt", "rgb48le", "-s", size, "-i", "-", "-vf",
                                           std::string(crop) + "scale=out_color_matrix=bt2020:out_range=full,format=yuv444p10le,setparams=" + tags,
                                           "-c:v", "libaom-av1", "-still-picture", "1", "-crf", "4", "-cpu-used", "6", "-row-mt", "1", path};
    Process ffmpeg;
    ProcessOptions options;
    options.pipe_stdin = true;
    if (!ffmpeg.Start(args, options, &error)) {
        error = "ffmpeg is not available (" + error + ")";
        return false;
    }
    std::vector<uint16_t> row(w2 * 3, 0);
    for (uint32_t y = 0; y < h2; y++) {
        std::fill(row.begin(), row.end(), (uint16_t)0);
        if (y < h)
            std::copy(rgb16.begin() + (size_t)y * w * 3, rgb16.begin() + (size_t)(y + 1) * w * 3, row.begin());
        if (!ffmpeg.Write(row.data(), row.size() * 2)) {
            error = "ffmpeg stopped";
            return false;
        }
    }
    ffmpeg.CloseStdin();
    if (ffmpeg.Wait() != 0) {
        error = "ffmpeg's AVIF encoder failed";
        return false;
    }
    return true;
}

}  // namespace

// --- Panel ------------------------------------------------------------------------------------------------------------

void CaptureStudio::DrawSavePanel() {
    if (!ImGui::Begin("Save##cap") || !capture_) {
        ImGui::End();
        return;
    }
    const char* sizes[] = {"The capture's size (1x)", "1080p", "Half size", "Custom height"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::Combo("Size", &save_.size_mode, sizes, 4);
    if (save_.size_mode == 3) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::InputInt("height", &save_.custom_height, 0, 0);
    }
    ImGui::TextDisabled("%d x %d px", (int)std::lround(OutputHeight() * (double)ImageWidth() / std::max<uint32_t>(ImageHeight(), 1)), OutputHeight());
    ImGui::Checkbox("Outline the marks on the full image", &save_.outlines);
    ImGui::Checkbox("Logos", &save_.logos);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Follows the Logos box of the Display panel (the project's settings); untick to leave them off in the saved images.");
    const bool hdr_source = image_[0] && image_[1] && (image_[0]->coding == Coding::PQ10 || image_[0]->coding == Coding::SCRGB16 ||
                                                       image_[1]->coding == Coding::PQ10 || image_[1]->coding == Coding::SCRGB16);
    ImGui::BeginDisabled(!hdr_source);
    ImGui::Checkbox("HDR: 16-bit PQ PNG (.hdr.png, cICP)", &save_.hdr_png);
    ImGui::Checkbox("HDR: AVIF through ffmpeg (.hdr.avif)", &save_.hdr_avif);
    ImGui::EndDisabled();
    char caption[256];
    std::snprintf(caption, sizeof(caption), "%s", save_.caption.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##caption", "caption under the picture (optional)", caption, sizeof(caption)))
        save_.caption = caption;
    ImGui::Separator();
    const std::string suggested = where_.Saved() + "/" + capture_->name + "_" + StemOfPair() + ".png";
    ImGui::BeginDisabled(dialogs_.busy() || !tasks_.empty());
    if (ImGui::Button("Save image..."))
        ShowDialog(Dialogs::Kind::SAVE_IMAGE, suggested);
    ImGui::SameLine();
    ImGui::BeginDisabled(project_.areas.empty());
    if (ImGui::Button("Save marked crops...")) {
        folder_crops_only_ = true;
        ShowDialog(Dialogs::Kind::SAVE_FOLDER, where_.Saved());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Save all...")) {
        folder_crops_only_ = false;
        ShowDialog(Dialogs::Kind::SAVE_FOLDER, where_.Saved());
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("A crop is the mark's region at the same zoom as the video's freeze marks (padding 1.15, up to 16x, nearest-neighbour) "
                       "in the current view, with the mark's label and the zoom. Save all writes the full image and every crop into a "
                       "folder with a manifest.json (capture, pair, generators and their versions).");
    if (!tasks_.empty())
        ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.6f, 1.0f), "%s", save_status_.c_str());
    for (const std::string& line : save_log_)
        ImGui::TextDisabled("%s", line.c_str());
    ImGui::End();
}

// --- Queue ------------------------------------------------------------------------------------------------------------

int CaptureStudio::OutputHeight() const {
    const int h = (int)ImageHeight();
    switch (save_.size_mode) {
        case 0: return h;
        case 1: return 1080;
        case 2: return std::max(h / 2, 1);
        default: return std::max(save_.custom_height, 16);
    }
}

void CaptureStudio::QueueSave(const SaveTask& task) {
    if (tasks_.empty())
        save_started_ = Now();
    tasks_.push_back(task);
    save_status_ = "saving " + fs::path(task.path).filename().string();
}

void CaptureStudio::QueueCrops(const std::string& folder, int height) {
    for (const Area& a : project_.areas) {
        SaveTask t;
        t.kind = SaveTask::CROP;
        t.mark = a.id;
        t.height = height;
        t.path = folder + "/" + capture_->name + "_" + StemOfPair() + "_mark" + std::to_string(a.id) + ".png";
        QueueSave(t);
    }
}

void CaptureStudio::QueueSaveAll(const std::string& folder, int height) {
    batch_folder_ = folder;
    batch_files_.clear();
    SaveTask t;
    t.kind = SaveTask::FULL;
    t.height = height;
    t.path = folder + "/" + capture_->name + "_" + StemOfPair() + ".png";
    QueueSave(t);
    QueueCrops(folder, height);
}

bool CaptureStudio::ExportBusy() const {
    return !tasks_.empty() || !writes_.empty();
}

// --- Frame ------------------------------------------------------------------------------------------------------------

bool CaptureStudio::PrepareTask(const SaveTask& t, bool hdr) {
    const int src_w = (int)ImageWidth(), src_h = (int)ImageHeight();
    int oh = t.height > 0 ? t.height : OutputHeight();
    int ow = oh == src_h ? src_w : (int)std::lround((double)oh * src_w / src_h);
    ow = std::max(ow, 16);
    const nri::Format format = hdr ? nri::Format::RGBA16_UNORM : nri::Format::RGBA8_UNORM;
    const int slot = hdr ? 1 : 0;
    if (!composer_[slot] || composer_w_[slot] != (uint32_t)ow || composer_h_[slot] != (uint32_t)oh) {
        composer_[slot].reset();
        composer_[slot] = std::make_unique<OffscreenComposer>();
        std::string error;
        if (!composer_[slot]->Create(*host_.gfx, (uint32_t)ow, (uint32_t)oh, format, 1, error, 203.0f)) {
            Notify("cannot save: " + error, true);
            composer_[slot].reset();
            return false;
        }
        composer_w_[slot] = (uint32_t)ow;
        composer_h_[slot] = (uint32_t)oh;
    }
    OffscreenComposer* composer = composer_[slot].get();
    prepared_.w = (uint32_t)ow;
    prepared_.h = (uint32_t)oh;
    prepared_.hdr = hdr;
    prepared_.layout = ShownLayout();
    prepared_.divider = divider_;
    const Area* mark = nullptr;
    for (const Area& a : project_.areas)
        if (t.kind == SaveTask::CROP && a.id == t.mark)
            mark = &a;
    if (t.kind == SaveTask::CROP && !mark) {
        Notify("the mark is gone", true);
        return false;
    }
    prepared_.camera = mark ? view_for(mark->rect) : camera_;
    // The overlay: labels, divider line, caption and zoom, with the video export's code
    const Rect view = {0.0, 0.0, (double)ow, (double)oh};
    const CompareLayout areas = compare_layout(prepared_.layout, view, prepared_.divider);
    OverlayInput in;
    in.layout = prepared_.layout;
    in.view = view;
    in.areas = areas;
    in.video_w = src_w;
    in.video_h = src_h;
    for (int i = 0; i < 2; i++)
        in.mapping[i] = map_camera(areas.area[i], src_w, src_h, prepared_.camera, fit_scale(areas.area[i], src_w, src_h));
    in.label_a = diff_ ? "difference |A - B| x8 (codes)" : project_.label_a;
    in.label_b = project_.label_b;
    in.divider_x = areas.split;
    in.caption = mark ? mark->label : save_.caption;
    in.zoom = in.mapping[0].camera.z;
    in.size = (float)std::max(14.0, std::floor((double)oh / 42.0));
    in.line = (float)std::max(2.0, std::floor((double)oh / 720.0));
    ImDrawList* list = composer->BeginOverlay();
    pills_.clear();
    DrawVideoOverlay(list, ImGui::GetFont(), in, &pills_);
    if (t.kind == SaveTask::FULL && save_.outlines)
        for (size_t i = 0; i < project_.areas.size(); i++)
            for (int p = 0; p < (prepared_.layout == Layout::side_by_side ? 2 : 1); p++)
                DrawAreaOutline(list, ImGui::GetFont(), in.size, in.mapping[prepared_.layout == Layout::b_only ? 1 : p], src_w, src_h,
                                project_.areas[i], AreaColor(i), false);
    {  // the logos, after the overlay: they keep clear of its pills (logo_layer.h)
        const bool on = save_.logos && LogosOn();
        export_logos_.Set(on ? project_.logos.effective() : std::vector<LogoItem>(), LogoFolders(), ow, oh);
        export_logos_.Draw(list, ImVec2(0.0f, 0.0f), pills_);
    }
    task_prepared_ = true;
    return true;
}

void CaptureStudio::ExportPrepare() {
    if (!created_ || tasks_.empty() || task_prepared_ || !capture_)
        return;
    if (!image_[0] || !image_[1]) {
        if (image_state_[0] == ImageStore::State::FAILED || image_state_[1] == ImageStore::State::FAILED) {
            Notify("cannot save: a source did not load", true);
            tasks_.clear();
        }
        return;
    }
    const SaveTask& task = tasks_.front();
    const bool hdr_source = image_[0]->coding == Coding::PQ10 || image_[0]->coding == Coding::SCRGB16 ||
                            image_[1]->coding == Coding::PQ10 || image_[1]->coding == Coding::SCRGB16;
    if (current_pass_ == 1 && !(hdr_source && (save_.hdr_png || save_.hdr_avif)))
        current_pass_ = 0;
    if (!PrepareTask(task, current_pass_ == 1))
        tasks_.erase(tasks_.begin());
}

void CaptureStudio::ExportRecord() {
    if (!task_prepared_)
        return;
    task_prepared_ = false;
    const SaveTask task = tasks_.front();
    const bool hdr = prepared_.hdr;
    const Rect view = {0.0, 0.0, (double)prepared_.w, (double)prepared_.h};
    const CompareLayout areas = compare_layout(prepared_.layout, view, prepared_.divider);
    const int src_w = (int)ImageWidth(), src_h = (int)ImageHeight();
    StillParams p;
    p.regions = compare_regions(areas, src_w, src_h, prepared_.camera);
    p.view = diff_ ? StillView::DIFF : StillView::PICTURES;
    p.target_encoding = hdr ? (uint32_t)PQ10 : (uint32_t)SRGB8;
    p.paper_white = 203.0f;
    p.hw_srgb = false;
    for (int i = 0; i < 2; i++) {
        p.coding[i] = image_[i]->coding;
        p.size[i][0] = image_[i]->width;
        p.size[i][1] = image_[i]->height;
        p.steps[i] = (float)CodeSteps(image_[i]->dxgi);
        if (image_[i]->coding == Coding::PQ10 && p.steps[i] == 0.0f)
            p.steps[i] = 1023.0f;
    }
    const nri::Format format = hdr ? nri::Format::RGBA16_UNORM : nri::Format::RGBA8_UNORM;
    OffscreenComposer& composer = *composer_[hdr ? 1 : 0];
    composer.Record(0, [&](nri::CommandBuffer& cb) {
        export_pass_.Record(cb, 0, format, p, image_[0]->view, image_[1]->view);
    });
    composer.Wait(0);
    // The pixels go to a writer thread (the deflate of a 4K image takes a second or two)
    const uint32_t w = prepared_.w, h = prepared_.h, pitch = composer.row_pitch();
    const uint8_t* pixels = composer.Pixels(0);
    const std::string base = Base(task.path);
    const bool want_png16 = hdr && save_.hdr_png, want_avif = hdr && save_.hdr_avif;
    std::string sdr_path = task.path;
    std::vector<uint8_t> copy((size_t)pitch * h);
    std::memcpy(copy.data(), pixels, copy.size());
    const double started = Now();
    writes_.push_back(std::async(std::launch::async, [=, copy = std::move(copy)]() -> std::string {
        std::string error;
        if (!hdr) {
            std::vector<uint8_t> rgb((size_t)w * h * 3);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    for (int k = 0; k < 3; k++)
                        rgb[((size_t)y * w + x) * 3 + (size_t)k] = copy[(size_t)y * pitch + (size_t)x * 4 + (size_t)k];
            if (!WritePng8(sdr_path, w, h, 3, rgb.data(), (int)(w * 3), error))
                return error;
            char text[256];
            std::snprintf(text, sizeof(text), "%s (%ux%u, %.2f s)", sdr_path.c_str(), w, h, Now() - started);
            return std::string("ok ") + text;
        }
        std::vector<uint16_t> rgb((size_t)w * h * 3);
        for (uint32_t y = 0; y < h; y++) {
            const uint16_t* row = (const uint16_t*)(copy.data() + (size_t)y * pitch);
            for (uint32_t x = 0; x < w; x++)
                for (int k = 0; k < 3; k++)
                    rgb[((size_t)y * w + x) * 3 + (size_t)k] = row[(size_t)x * 4 + (size_t)k];
        }
        std::string done;
        if (want_png16) {
            if (!WritePng16(base + ".hdr.png", w, h, rgb.data(), true, error))
                return error;
            done += base + ".hdr.png ";
        }
        if (want_avif) {
            if (!WriteAvif(base + ".hdr.avif", w, h, rgb, error))
                return error;
            done += base + ".hdr.avif ";
        }
        char text[64];
        std::snprintf(text, sizeof(text), "(%.2f s)", Now() - started);
        return "ok " + done + text;
    }));
    batch_files_.push_back(hdr ? base : task.path);
    // The next pass of the task (the HDR image), else the next task
    const bool hdr_source = image_[0]->coding == Coding::PQ10 || image_[0]->coding == Coding::SCRGB16 ||
                            image_[1]->coding == Coding::PQ10 || image_[1]->coding == Coding::SCRGB16;
    if (current_pass_ == 0 && hdr_source && (save_.hdr_png || save_.hdr_avif)) {
        current_pass_ = 1;
    } else {
        current_pass_ = 0;
        tasks_.erase(tasks_.begin());
        if (tasks_.empty()) {
            char text[128];
            std::snprintf(text, sizeof(text), "saved in %.1f s", Now() - save_started_);
            save_status_ = text;
            if (!batch_folder_.empty()) {
                WriteManifest(batch_folder_, batch_files_);
                batch_folder_.clear();
            }
        }
    }
}

// Collects the writer threads' results (called from Update).
void CaptureStudio::PollWrites() {
    for (auto it = writes_.begin(); it != writes_.end();) {
        if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        const std::string result = it->get();
        if (result.rfind("ok ", 0) == 0) {
            save_log_.push_back("saved " + result.substr(3));
            spdlog::info("captures: saved {}", result.substr(3));
        } else {
            save_log_.push_back("not saved: " + result);
            Notify("not saved: " + result, true);
        }
        if (save_log_.size() > 8)
            save_log_.erase(save_log_.begin());
        it = writes_.erase(it);
    }
}

// --- Manifest ---------------------------------------------------------------------------------------------------------

void CaptureStudio::WriteManifest(const std::string& folder, const std::vector<std::string>& files) {
    const CaptureInfo& c = *capture_;
    const PairSpec& pair = pairs_[(size_t)pair_index_];
    auto describe = [&](const std::string& id) {
        json j = {{"id", id}};
        if (const Source* s = FindSource(sources_, id)) {
            j["label"] = s->label;
            j["file"] = s->path;
            j["coding"] = s->coding == Coding::PQ10 ? "pq10" : s->coding == Coding::SCRGB16 ? "scrgb16" : s->coding == Coding::VIZ ? "false-colour" : "srgb";
            if (s->kind == SourceKind::GENERATED)
                for (const Generator& g : generators_)
                    if (g.id == s->generator_id) {
                        j["generator"] = {{"id", g.id}, {"label", g.label}, {"kind", g.kind == Generator::DLSSG ? "dlss-g" : "apex-nr-fg"},
                                          {"version", g.version}, {"dll", g.dll}, {"export", g.export_dir},
                                          {"tool_revision", ToolRevision(where_, g)}, {"stale", s->stale},
                                          {"result_record", fs::path(s->path).parent_path().string() +
                                                                (g.replay ? "/replay.json" : "/manifest.json")}};
                    }
            if (s->kind == SourceKind::IN_GAME)
                j["in_game"] = c.in_game;
        }
        return j;
    };
    json marks = json::array();
    for (const Area& a : project_.areas)
        marks.push_back({{"id", a.id}, {"label", a.label}, {"rect", {a.rect[0], a.rect[1], a.rect[2], a.rect[3]}}});
    json list = json::array();
    for (const std::string& f : files) {
        const bool hdr = f.size() < 4 || f.substr(f.size() - 4) != ".png";
        if (hdr) {
            if (save_.hdr_png)
                list.push_back(f + ".hdr.png");
            if (save_.hdr_avif)
                list.push_back(f + ".hdr.avif");
        } else {
            list.push_back(f);
        }
    }
    json manifest = {{"tool", "renderer Captures tab"},
                     {"capture", {{"name", c.name}, {"folder", c.folder}, {"game", c.game}, {"time", c.time}, {"width", c.width},
                                  {"height", c.height}, {"multiplier", c.multiplier}, {"encoding", c.encoding},
                                  {"in_game", c.in_game}}},
                     {"pair", {{"kind", pair.kind == PairKind::REAL ? "real-pair" : "skip-one"}, {"stem", pair.Stem()}, {"a", pair.a},
                               {"b", pair.b}, {"index", pair.index}, {"t", pair.t}, {"rendered", pair.rendered},
                               {"note", pair.kind == PairKind::SKIP_ONE ? "the rendered middle frame is an approximate reference; its timing is unverified" : ""}}},
                     {"a", describe(a_id_)},
                     {"b", describe(b_id_)},
                     {"labels", {{"a", project_.label_a}, {"b", project_.label_b}}},
                     {"view", diff_ ? "difference" : prepared_.layout == Layout::side_by_side ? "side-by-side" : prepared_.layout == Layout::a_only ? "a" : prepared_.layout == Layout::b_only ? "b" : "slider"},
                     {"camera", {camera_.cx, camera_.cy, camera_.z}},
                     {"divider", divider_},
                     {"output_height", prepared_.h},
                     {"logos", save_.logos && LogosOn()},
                     {"marks", marks},
                     {"files", list}};
    std::error_code ec;
    fs::create_directories(folder, ec);
    std::ofstream out(folder + "/manifest.json");
    out << manifest.dump(1) << "\n";
    save_log_.push_back("wrote " + folder + "/manifest.json");
    spdlog::info("captures: wrote {}/manifest.json", folder);
}

}  // namespace renderer::clips::captures
