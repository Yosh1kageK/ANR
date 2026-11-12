#include "renderer/clips/project.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace renderer::clips {
namespace {

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

double Number(const json& j, const char* key, double fallback) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

std::string Text(const json& j, const char* key, const std::string& fallback) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

bool ParseArea(const json& j, Area& area, std::string& error) {
    if (!j.is_object()) {
        error = "an area is not an object";
        return false;
    }
    area.id = j.contains("id") && j["id"].is_number() ? j["id"].get<int>() : 0;
    area.label = Text(j, "label", "");
    area.t0 = Number(j, "t0", 0.0);
    area.t1 = Number(j, "t1", 0.0);
    auto rect = j.find("rect");
    if (rect == j.end() || !rect->is_array() || rect->size() != 4) {
        error = "an area's rect needs x, y, w, h";
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!(*rect)[i].is_number()) {
            error = "an area's rect holds a non-number";
            return false;
        }
        area.rect[i] = (*rect)[i].get<double>();
    }
    area.divider = AreaDivider();
    auto divider = j.find("divider");
    if (divider != j.end()) {
        if (divider->is_string() && divider->get<std::string>() == "sweep") {
            area.divider.kind = AreaDivider::SWEEPING;
            auto sweeps = j.find("sweeps");
            if (sweeps != j.end() && sweeps->is_number_integer())
                area.divider.sweeps = std::clamp(sweeps->get<int>(), 1, 16);
        }
        else if (divider->is_number()) {
            area.divider.kind = AreaDivider::FIXED;
            area.divider.fraction = std::clamp(divider->get<double>(), 0.0, 1.0);
        }
    }
    // Areas of older files have no kind: spans, and they stay without one.
    area.kind = AreaKind::SPAN;
    area.kind_stored = false;
    auto kind = j.find("kind");
    if (kind != j.end() && kind->is_string()) {
        const std::string name = kind->get<std::string>();
        if (name == "freeze" || name == "span") {
            area.kind = name == "freeze" ? AreaKind::FREEZE : AreaKind::SPAN;
            area.kind_stored = true;
        }
    }
    area.hold.reset();
    area.ease.reset();
    auto hold = j.find("hold");
    if (hold != j.end() && hold->is_number())
        area.hold = hold->get<double>();
    auto ease = j.find("ease");
    if (ease != j.end() && ease->is_number())
        area.ease = ease->get<double>();
    return true;
}

// The known fields of an area written over its stored object (unknown fields stay).
json AreaToJson(const Area& area, json stored) {
    if (!stored.is_object())
        stored = json::object();
    stored["id"] = area.id;
    stored["label"] = area.label;
    if (area.kind_stored)
        stored["kind"] = area.kind == AreaKind::FREEZE ? "freeze" : "span";
    stored["t0"] = area.t0;
    stored["t1"] = area.kind == AreaKind::FREEZE ? area.t0 : area.t1;  // a freeze has no duration: older readers skip it
    if (area.hold)
        stored["hold"] = *area.hold;
    else
        stored.erase("hold");
    if (area.ease)
        stored["ease"] = *area.ease;
    else
        stored.erase("ease");
    stored["rect"] = {area.rect[0], area.rect[1], area.rect[2], area.rect[3]};
    switch (area.divider.kind) {
        case AreaDivider::KEEP: stored["divider"] = nullptr; break;
        case AreaDivider::SWEEPING: stored["divider"] = "sweep"; break;
        case AreaDivider::FIXED: stored["divider"] = area.divider.fraction; break;
    }
    if (area.divider.kind == AreaDivider::SWEEPING)
        stored["sweeps"] = area.divider.sweeps;
    else
        stored.erase("sweeps");
    return stored;
}

// The default logos assume A is ours (Apex left, NVIDIA right). Where A's label names NVIDIA's DLSS and B's does not,
// as in projects made by scripts that put DLSS-G in A, each logo goes to its product's side instead; stored items win.
bool NamesNvidia(const std::string& label) {
    std::string l = label;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return l.find("dlss") != std::string::npos || l.find("nvidia") != std::string::npos;
}

void LogosByLabels(Project& p) {
    if (p.logos.has_items || !NamesNvidia(p.label_a) || NamesNvidia(p.label_b))
        return;
    p.logos.items = default_logos();
    for (LogoItem& item : p.logos.items)
        item.corner = item.corner == Corner::BOTTOM_LEFT ? Corner::BOTTOM_RIGHT : Corner::BOTTOM_LEFT;
    p.logos.has_items = true;  // not stored: the file changes only when the logos are edited
}

LogoItem ParseLogo(const json& j) {
    LogoItem item;
    item.id = Text(j, "id", "");
    item.path = Text(j, "path", "");
    Corner corner = Corner::BOTTOM_LEFT;
    if (parse_corner(Text(j, "corner", ""), corner))
        item.corner = corner;
    item.height = std::clamp(Number(j, "height", LOGO_HEIGHT), 0.005, 0.5);
    item.margin = std::clamp(Number(j, "margin", LOGO_MARGIN), 0.0, 0.5);
    item.opacity = std::clamp(Number(j, "opacity", LOGO_OPACITY), 0.0, 1.0);
    auto backdrop = j.find("backdrop");
    item.backdrop = backdrop != j.end() && backdrop->is_boolean() && backdrop->get<bool>();
    return item;
}

void ParseLogos(const json& j, LogoSettings& logos) {
    logos = LogoSettings();
    if (!j.is_object())
        return;
    logos.stored = true;
    auto enabled = j.find("enabled");
    if (enabled != j.end() && enabled->is_boolean())
        logos.enabled = enabled->get<bool>();
    auto items = j.find("items");
    if (items != j.end() && items->is_array()) {
        logos.has_items = true;
        for (const json& item : *items)
            if (item.is_object())
                logos.items.push_back(ParseLogo(item));
    }
}

json LogoToJson(const LogoItem& item, json stored) {
    if (!stored.is_object())
        stored = json::object();
    stored["id"] = item.id;
    stored["path"] = item.path;
    stored["corner"] = corner_name(item.corner);
    stored["height"] = item.height;
    stored["margin"] = item.margin;
    stored["opacity"] = item.opacity;
    stored["backdrop"] = item.backdrop;
    return stored;
}

}  // namespace

std::string ExpandPath(const std::string& path, const std::string& base_directory) {
    std::string p = path;
    if (!p.empty() && p[0] == '~' && (p.size() == 1 || p[1] == '/' || p[1] == '\\')) {
        const char* home = std::getenv("HOME");
#ifdef _WIN32
        if (!home)
            home = std::getenv("USERPROFILE");
#endif
        if (home)
            p = std::string(home) + p.substr(1);
    }
    fs::path result(p);
    if (result.is_relative() && !base_directory.empty())
        result = fs::path(base_directory) / result;
    return result.string();
}

json SyncToJson(const SyncResult& sync) {
    json windows = json::array();
    for (const SyncWindow& w : sync.windows) {
        json window = {{"a_time", w.a_time}, {"offset", w.offset}, {"peak", w.peak}, {"contrast", w.contrast},
                       {"width_frames", w.width_frames}};
        if (sync.tracking.present) {  // the tracker's fields, written only for its results
            window["confidence"] = w.confidence;
            window["used"] = w.used;
            window["residual_frames"] = w.residual_frames;
        }
        windows.push_back(window);
    }
    json j = {{"offset", sync.offset},
              {"offset_frames", sync.offset_frames},
              {"quality", sync.quality},
              {"spread_seconds", sync.spread_seconds ? json(*sync.spread_seconds) : json(nullptr)},
              {"coarse", {{"offset", sync.coarse.offset}, {"similarity", sync.coarse.similarity},
                          {"next_best", sync.coarse.next_best}, {"next_best_offset", sync.coarse.next_best_offset},
                          {"z", sync.coarse.z}}},
              {"windows", windows},
              {"detected_at", sync.detected_at},
              {"method", sync.method}};
    if (sync.tracking.present) {
        const SyncTracking& t = sync.tracking;
        json jumps = json::array();
        for (const SyncJump& jump : t.jumps)
            jumps.push_back({{"a_time", jump.a_time}, {"frames", jump.frames}});
        json map = json::array();
        for (const OffsetPoint& point : t.map)
            map.push_back({{"a", point.a}, {"k", point.k}});
        j["tracking"] = {{"slope_ppm", t.slope_ppm},
                         {"drift_frames", t.drift_frames},
                         {"jumps", jumps},
                         {"windows_used", t.windows_used},
                         {"windows_rejected", t.windows_rejected},
                         {"confidence", t.confidence},
                         {"mask_fraction", t.mask_fraction},
                         {"significant", t.significant},
                         {"map", map}};
    }
    return j;
}

std::optional<SyncResult> SyncFromJson(const json& j) {
    if (!j.is_object() || !j.contains("offset_frames"))
        return std::nullopt;
    SyncResult s;
    s.offset = Number(j, "offset", 0.0);
    s.offset_frames = (int64_t)std::llround(Number(j, "offset_frames", 0.0));
    s.quality = Text(j, "quality", "");
    auto spread = j.find("spread_seconds");
    if (spread != j.end() && spread->is_number())
        s.spread_seconds = spread->get<double>();
    auto coarse = j.find("coarse");
    if (coarse != j.end() && coarse->is_object()) {
        s.coarse.offset = Number(*coarse, "offset", 0.0);
        s.coarse.similarity = Number(*coarse, "similarity", 0.0);
        s.coarse.next_best = Number(*coarse, "next_best", 0.0);
        s.coarse.next_best_offset = Number(*coarse, "next_best_offset", 0.0);
        s.coarse.z = Number(*coarse, "z", 0.0);
    }
    auto windows = j.find("windows");
    if (windows != j.end() && windows->is_array())
        for (const json& w : *windows)
            if (w.is_object()) {
                SyncWindow window = {Number(w, "a_time", 0.0), Number(w, "offset", 0.0), Number(w, "peak", 0.0),
                                     Number(w, "contrast", 0.0), (int)Number(w, "width_frames", 0.0)};
                auto used = w.find("used");
                window.used = used != w.end() && used->is_boolean() ? used->get<bool>()
                                                                     : window.contrast >= 0.10 && window.width_frames <= 15;
                window.confidence = Number(w, "confidence", 0.0);
                window.residual_frames = Number(w, "residual_frames", 0.0);
                s.windows.push_back(window);
            }
    auto tracking = j.find("tracking");
    if (tracking != j.end() && tracking->is_object()) {
        SyncTracking& t = s.tracking;
        t.present = true;
        t.slope_ppm = Number(*tracking, "slope_ppm", 0.0);
        t.drift_frames = Number(*tracking, "drift_frames", 0.0);
        t.windows_used = (int)Number(*tracking, "windows_used", 0.0);
        t.windows_rejected = (int)Number(*tracking, "windows_rejected", 0.0);
        t.confidence = Number(*tracking, "confidence", 0.0);
        t.mask_fraction = Number(*tracking, "mask_fraction", 0.0);
        auto significant = tracking->find("significant");
        t.significant = significant != tracking->end() && significant->is_boolean() && significant->get<bool>();
        auto jumps = tracking->find("jumps");
        if (jumps != tracking->end() && jumps->is_array())
            for (const json& jump : *jumps)
                if (jump.is_object())
                    t.jumps.push_back({Number(jump, "a_time", 0.0), Number(jump, "frames", 0.0)});
        auto map = tracking->find("map");
        if (map != tracking->end() && map->is_array())
            for (const json& point : *map)
                if (point.is_object() && point.contains("a") && point.contains("k"))
                    t.map.push_back({(int64_t)std::llround(Number(point, "a", 0.0)), Number(point, "k", 0.0)});
    }
    s.detected_at = Text(j, "detected_at", "");
    s.method = Text(j, "method", "");
    return s;
}

std::string Project::ResolvedA() const {
    return ExpandPath(path_a, file.empty() ? "" : fs::path(file).parent_path().string());
}
std::string Project::ResolvedB() const {
    return ExpandPath(path_b, file.empty() ? "" : fs::path(file).parent_path().string());
}

bool ParseProject(const json& d, Project& p, std::string& error) {
    if (!d.is_object()) {
        error = "the project is not a JSON object";
        return false;
    }
    p = Project();
    p.raw = d;
    p.version = (int)Number(d, "version", 1);
    p.title = Text(d, "title", "");
    for (const char* side : {"a", "b"}) {
        auto it = d.find(side);
        if (it == d.end() || !it->is_object() || !it->contains("path") || !(*it)["path"].is_string()) {
            error = std::string("the project has no ") + side + ".path";
            return false;
        }
        (side[0] == 'a' ? p.path_a : p.path_b) = (*it)["path"].get<std::string>();
    }
    auto labels = d.find("labels");
    if (labels != d.end() && labels->is_object()) {
        p.label_a = Text(*labels, "a", "A");
        p.label_b = Text(*labels, "b", "B");
    }
    const std::string layout = Text(d, "layout", "slider");
    p.layout = layout == "side-by-side" ? layout : "slider";
    p.divider = std::clamp(Number(d, "divider", 0.5), 0.0, 1.0);
    p.ease = std::clamp(Number(d, "ease", DEFAULT_EASE), 0.0, 5.0);
    p.hold = std::clamp(Number(d, "hold", DEFAULT_HOLD), 0.2, 60.0);
    p.freeze_hold = std::clamp(Number(d, "freeze_hold", DEFAULT_FREEZE_HOLD), 0.0, 60.0);
    p.new_mark_kind = Text(d, "new_mark_kind", "freeze") == "span" ? AreaKind::SPAN : AreaKind::FREEZE;
    auto logos = d.find("logos");
    if (logos != d.end())
        ParseLogos(*logos, p.logos);
    LogosByLabels(p);
    p.offset_frames = (int64_t)std::llround(Number(d, "offset_frames", 0.0));
    auto offset_map = d.find("offset_map");
    if (offset_map != d.end() && offset_map->is_array()) {
        for (const json& point : *offset_map)
            if (point.is_object() && point.contains("a") && point["a"].is_number() && point.contains("k") && point["k"].is_number())
                p.offset_map.push_back({(int64_t)std::llround(point["a"].get<double>()), point["k"].get<double>()});
        std::stable_sort(p.offset_map.begin(), p.offset_map.end(),
                         [](const OffsetPoint& x, const OffsetPoint& y) { return x.a < y.a; });
    }
    auto follow = d.find("follow_drift");
    p.follow_drift = follow != d.end() && follow->is_boolean() ? follow->get<bool>() : !p.offset_map.empty();
    p.fps = Number(d, "fps", 0.0);
    auto size = d.find("size");
    if (size != d.end() && size->is_array() && size->size() == 2 && (*size)[0].is_number() && (*size)[1].is_number()) {
        p.width = (*size)[0].get<int>();
        p.height = (*size)[1].get<int>();
    }
    auto areas = d.find("areas");
    if (areas != d.end() && areas->is_array())
        for (const json& a : *areas) {
            Area area;
            if (!ParseArea(a, area, error))
                return false;
            p.areas.push_back(area);
        }
    auto sync = d.find("sync");
    if (sync != d.end())
        p.sync = SyncFromJson(*sync);
    return true;
}

bool LoadProject(const std::string& path, Project& project, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    json document;
    try {
        document = json::parse(in);
    } catch (const std::exception& e) {
        error = path + ": " + e.what();
        return false;
    }
    if (!ParseProject(document, project, error)) {
        error = path + ": " + error;
        return false;
    }
    project.file = path;
    return true;
}

json ProjectToJson(const Project& p) {
    json d = p.raw.is_object() ? p.raw : json::object();
    // A known field is written only where it differs, so a document keeps its own number formats.
    auto set = [](json& object, const char* key, const json& value) {
        if (!object.contains(key) || !(object[key] == value))
            object[key] = value;
    };
    auto sub = [&](const char* key) -> json& {
        if (!d.contains(key) || !d[key].is_object())
            d[key] = json::object();
        return d[key];
    };
    set(d, "version", p.version);
    set(d, "title", p.title);
    set(sub("a"), "path", p.path_a);
    set(sub("b"), "path", p.path_b);
    set(sub("labels"), "a", p.label_a);
    set(sub("labels"), "b", p.label_b);
    set(d, "layout", p.layout);
    set(d, "divider", p.divider);
    set(d, "ease", p.ease);
    set(d, "hold", p.hold);
    // The newer fields are written where they differ from their defaults or the document already has them, so a file
    // of an older version comes back as it was.
    if (d.contains("freeze_hold") || p.freeze_hold != DEFAULT_FREEZE_HOLD)
        set(d, "freeze_hold", p.freeze_hold);
    if (d.contains("new_mark_kind") || p.new_mark_kind != AreaKind::FREEZE)
        set(d, "new_mark_kind", p.new_mark_kind == AreaKind::FREEZE ? "freeze" : "span");
    if (p.logos.stored || d.contains("logos")) {
        json& logos = sub("logos");
        if (p.logos.enabled)
            set(logos, "enabled", *p.logos.enabled);
        if (p.logos.items_dirty) {
            std::map<std::string, json> stored;  // by id, so the unknown fields of an item stay
            if (logos.contains("items") && logos["items"].is_array())
                for (const json& item : logos["items"])
                    if (item.is_object() && item.contains("id") && item["id"].is_string())
                        stored[item["id"].get<std::string>()] = item;
            json array = json::array();
            for (const LogoItem& item : p.logos.effective()) {
                auto it = stored.find(item.id);
                array.push_back(LogoToJson(item, it == stored.end() ? json::object() : it->second));
            }
            logos["items"] = array;
        }
    }
    if (p.areas_dirty || !d.contains("areas")) {
        // The stored area objects by id, so their unknown fields stay.
        std::map<int, json> stored;
        if (d.contains("areas") && d["areas"].is_array())
            for (const json& a : d["areas"])
                if (a.is_object() && a.contains("id") && a["id"].is_number())
                    stored[a["id"].get<int>()] = a;
        json areas = json::array();
        for (const Area& a : p.areas) {
            auto it = stored.find(a.id);
            areas.push_back(AreaToJson(a, it == stored.end() ? json::object() : it->second));
        }
        d["areas"] = areas;
    }
    set(d, "offset_frames", p.offset_frames);
    if (!p.offset_map.empty()) {
        // The control points, in order; a document keeps its own array where the values are the same.
        json points = json::array();
        for (const OffsetPoint& point : p.offset_map)
            points.push_back({{"a", point.a}, {"k", point.k}});
        set(d, "offset_map", points);
        set(d, "follow_drift", p.follow_drift);
    } else {
        d.erase("offset_map");
        d.erase("follow_drift");
    }
    set(d, "fps", p.fps);
    set(d, "size", json::array({p.width, p.height}));
    if (p.sync) {
        json& sync = sub("sync");
        const json written = SyncToJson(*p.sync);  // not a temporary: items() keeps a pointer to it
        for (auto& [key, value] : written.items())
            set(sync, key.c_str(), value);
    }
    return d;
}

bool SaveProject(const Project& project, const std::string& path, std::string& error) {
    const std::string text = ProjectToJson(project).dump(1) + "\n";
    std::error_code ec;
    const fs::path target(path);
    if (!target.parent_path().empty())
        fs::create_directories(target.parent_path(), ec);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            error = "cannot write " + temporary;
            return false;
        }
    }
    fs::rename(temporary, target, ec);
    if (ec) {
        fs::remove(target, ec);  // Windows does not replace on rename
        ec.clear();
        fs::rename(temporary, target, ec);
        if (ec) {
            error = "cannot replace " + path + ": " + ec.message();
            return false;
        }
    }
    return true;
}

void MarkSaved(Project& project) {
    project.raw = ProjectToJson(project);
    project.areas_dirty = false;
    project.logos.items_dirty = false;
}

std::string DefaultProjectPath(const std::string& path_a, const std::string& path_b) {
    const fs::path a(path_a), b(path_b);
    return (a.parent_path() / (a.stem().string() + "-vs-" + b.stem().string() + ".compare.json")).string();
}

std::string DefaultTitle(const std::string& path_a, const std::string& path_b) {
    return fs::path(path_a).stem().string() + " vs " + fs::path(path_b).stem().string();
}

Project NewProject(const std::string& path_a, const std::string& path_b, int width, int height, double fps) {
    Project p;
    std::error_code ec;
    p.path_a = fs::absolute(path_a, ec).string();
    p.path_b = fs::absolute(path_b, ec).string();
    p.title = DefaultTitle(path_a, path_b);
    p.label_a = fs::path(path_a).stem().string();
    p.label_b = fs::path(path_b).stem().string();
    LogosByLabels(p);
    p.fps = fps;
    p.width = width;
    p.height = height;
    return p;
}

void SwapSides(Project& p) {
    const int64_t k = p.offset_frames;
    const OffsetMap before = p.Offsets();  // K(n) as it was: where each mark's moment moves to
    const bool default_title = p.title == DefaultTitle(p.path_a, p.path_b);
    std::swap(p.path_a, p.path_b);
    std::swap(p.label_a, p.label_b);
    if (p.raw.is_object() && p.raw.contains("a") && p.raw.contains("b") && p.raw["a"].is_object() && p.raw["b"].is_object())
        std::swap(p.raw["a"], p.raw["b"]);
    if (default_title)
        p.title = DefaultTitle(p.path_a, p.path_b);
    // The offset map's points are re-indexed ({a, k} becomes {a - k, -k}); the constant changes sign, and while the map is
    // followed offset_frames is the K at its first point.
    if (!p.offset_map.empty()) {
        const OffsetMap swapped = OffsetMap(p.offset_map).Swapped();
        p.offset_map = swapped.points();
        p.offset_frames = p.follow_drift ? swapped.first_k() : -k;
    } else {
        p.offset_frames = -k;
    }
    // The marks keep their moments: the pair that was at A's frame n is at frame n - K(n) now.
    if (!p.areas.empty() && p.fps > 0.0 && (k != 0 || !before.is_constant())) {
        for (Area& a : p.areas) {
            if (a.kind == AreaKind::FREEZE) {
                const int64_t frame = (int64_t)std::llround(a.t0 * p.fps);
                const int64_t moved = frame - before.at(frame);  // a frozen frame stays a whole frame
                a.t0 = a.t1 = (double)moved / p.fps;
            } else {
                a.t0 -= (double)before.at((int64_t)std::llround(a.t0 * p.fps)) / p.fps;
                a.t1 -= (double)before.at((int64_t)std::llround(a.t1 * p.fps)) / p.fps;
            }
        }
        p.areas_dirty = true;
    }
    if (p.sync) {
        SyncResult& s = *p.sync;
        s.offset = -s.offset;
        s.offset_frames = -s.offset_frames;
        s.coarse.offset = -s.coarse.offset;
        s.coarse.next_best_offset = -s.coarse.next_best_offset;
        for (SyncWindow& w : s.windows) {
            w.a_time -= w.offset;  // the window's moment on B's clock, which is A's now
            w.offset = -w.offset;
            w.residual_frames = -w.residual_frames;
        }
        SyncTracking& t = s.tracking;
        if (t.present) {
            const OffsetMap fitted(t.map);
            for (SyncJump& jump : t.jumps) {
                jump.a_time -= fitted.exact(jump.a_time * p.fps) / p.fps;  // the jump's moment on B's clock
                jump.frames = -jump.frames;
            }
            const double slope = t.slope_ppm * 1e-6;  // dK'/dm = -s / (1 - s)
            t.slope_ppm = -slope / (1.0 - slope) * 1e6;
            t.drift_frames = -t.drift_frames;
            t.map = fitted.Swapped().points();
        }
    }
    // A logo stays with its video: the left corners are the right ones now.
    std::vector<LogoItem> items = p.logos.effective();
    for (LogoItem& item : items) {
        switch (item.corner) {
            case Corner::BOTTOM_LEFT: item.corner = Corner::BOTTOM_RIGHT; break;
            case Corner::BOTTOM_RIGHT: item.corner = Corner::BOTTOM_LEFT; break;
            case Corner::TOP_LEFT: item.corner = Corner::TOP_RIGHT; break;
            case Corner::TOP_RIGHT: item.corner = Corner::TOP_LEFT; break;
        }
    }
    p.logos.edit() = items;
}

}  // namespace renderer::clips
