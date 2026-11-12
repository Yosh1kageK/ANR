// The project file: the schema of the compare tool's earlier Python version (a real one is embedded), unknown fields and the areas
// array kept on save, edits written, atomic save and load, the default path.
#include "renderer/clips/project.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "renderer/clips/check.h"

using namespace renderer::clips;
using json = nlohmann::ordered_json;

namespace {

// A project of the Cyberpunk 2077 benchmark pair (trimmed of two sync windows), plus fields the code does not know: a
// top-level one, one inside an area and one inside sync.
const char* SAMPLE = R"({
 "version": 1,
 "title": "Cyberpunk 2077 benchmark, generated frames only",
 "a": {"path": "/videos/cyberpunk-apex-gen-only.mp4"},
 "b": {"path": "/videos/cyberpunk-dlssg-gen-only.mp4", "note": "kept"},
 "labels": {"a": "ANR FG", "b": "DLSS-G"},
 "layout": "side-by-side",
 "divider": 0.3258064516129032,
 "ease": 0.6,
 "hold": 3,
 "areas": [
  {"id": 1, "label": "bar counter", "t0": 30, "t1": 33,
   "rect": [0.406767955801105, 0.31767955801104975, 0.1864640883977901, 0.13812154696132595],
   "divider": null, "colour": "orange"}
 ],
 "offset_frames": 413,
 "fps": 60.0,
 "size": [3840, 2160],
 "sync": {
  "offset": 6.876958695087944, "offset_frames": 413, "quality": "fine", "spread_seconds": 0.03228125669562942,
  "coarse": {"offset": 6.9, "similarity": 0.9060356020927429, "next_best": 0.492329478263855,
             "next_best_offset": 5.8, "z": 9.762804027575875},
  "windows": [{"a_time": 12.470833333333331, "offset": 6.827145381752214, "peak": 0.9310877323150635,
               "contrast": 0.07547664642333984, "width_frames": 25}],
  "detected_at": "2026-11-08T22:00:00+00:00", "method": "downscaled frame matching", "extra": 7
 },
 "from_the_future": {"x": [1, 2, 3]}
})";

void parse_the_sample() {
    Project p;
    std::string error;
    CHECK(ParseProject(json::parse(SAMPLE), p, error));
    CHECK(error.empty());
    CHECK_EQ(p.version, 1);
    CHECK(p.title == "Cyberpunk 2077 benchmark, generated frames only");
    CHECK(p.path_a == "/videos/cyberpunk-apex-gen-only.mp4");
    CHECK(p.label_a == "ANR FG" && p.label_b == "DLSS-G");
    CHECK(p.layout == "side-by-side");
    CHECK_NEAR(p.divider, 0.3258064516129032, 1e-15);
    CHECK_NEAR(p.ease, 0.6, 1e-12);
    CHECK_NEAR(p.hold, 3.0, 1e-12);
    CHECK_EQ(p.offset_frames, 413);
    CHECK_NEAR(p.fps, 60.0, 1e-12);
    CHECK_EQ(p.width, 3840);
    CHECK_EQ(p.height, 2160);
    CHECK_EQ((int)p.areas.size(), 1);
    CHECK_EQ(p.areas[0].id, 1);
    CHECK(p.areas[0].label == "bar counter");
    CHECK_NEAR(p.areas[0].t0, 30.0, 1e-12);
    CHECK_NEAR(p.areas[0].rect[2], 0.1864640883977901, 1e-15);
    CHECK(p.areas[0].divider.kind == AreaDivider::KEEP);
    CHECK(p.sync.has_value());
    CHECK(p.sync->quality == "fine");
    CHECK_EQ(p.sync->offset_frames, 413);
    CHECK(p.sync->spread_seconds.has_value());
    CHECK_NEAR(*p.sync->spread_seconds, 0.03228125669562942, 1e-15);
    CHECK_NEAR(p.sync->coarse.similarity, 0.9060356020927429, 1e-15);
    CHECK_NEAR(p.sync->coarse.next_best, 0.492329478263855, 1e-15);
    CHECK_EQ((int)p.sync->windows.size(), 1);
    CHECK_EQ(p.sync->windows[0].width_frames, 25);
}

void save_keeps_everything() {
    Project p;
    std::string error;
    CHECK(ParseProject(json::parse(SAMPLE), p, error));
    // nothing edited: the document comes back as it was, areas array included
    json out = ProjectToJson(p);
    const json in = json::parse(SAMPLE);
    CHECK(out["areas"] == in["areas"]);
    CHECK(out["from_the_future"] == in["from_the_future"]);
    CHECK(out["b"]["note"] == "kept");
    CHECK(out["sync"]["extra"] == 7);
    CHECK(out == in);
    CHECK(out.dump(1) == in.dump(1));  // and in the order they were written (the Python wrote version first)
    CHECK(out.items().begin().key() == "version");
    // known fields change, unknown ones stay
    p.offset_frames = 414;
    p.label_b = "DLSS-G 4";
    p.layout = "slider";
    p.divider = 0.5;
    out = ProjectToJson(p);
    CHECK(out["offset_frames"] == 414);
    CHECK(out["labels"]["b"] == "DLSS-G 4");
    CHECK(out["layout"] == "slider");
    CHECK(out["areas"] == in["areas"]);  // untouched while not edited
    CHECK(out["from_the_future"] == in["from_the_future"]);
    // an edited area keeps its unknown field, a new one is added, a deleted one goes
    p.areas[0].label = "counter";
    p.areas[0].divider.kind = AreaDivider::SWEEPING;
    Area extra;
    extra.id = 2;
    extra.label = "new";
    extra.t0 = 5.0;
    extra.t1 = 6.0;
    extra.rect[0] = 0.1;
    extra.rect[1] = 0.2;
    extra.rect[2] = 0.3;
    extra.rect[3] = 0.4;
    p.areas.push_back(extra);
    p.areas_dirty = true;
    out = ProjectToJson(p);
    CHECK_EQ((int)out["areas"].size(), 2);
    CHECK(out["areas"][0]["label"] == "counter");
    CHECK(out["areas"][0]["divider"] == "sweep");
    CHECK(out["areas"][0]["sweeps"] == DEFAULT_SWEEPS);
    CHECK(out["areas"][0]["colour"] == "orange");
    CHECK(out["areas"][1]["id"] == 2);
    CHECK(out["areas"][1]["divider"].is_null());
    CHECK(out["areas"][1]["rect"][3] == 0.4);
    p.areas.erase(p.areas.begin());
    out = ProjectToJson(p);
    CHECK_EQ((int)out["areas"].size(), 1);
    CHECK(out["areas"][0]["label"] == "new");
    // and it parses back to the same areas
    Project q;
    CHECK(ParseProject(out, q, error));
    CHECK_EQ((int)q.areas.size(), 1);
    CHECK_EQ(q.areas[0].id, 2);
    CHECK_NEAR(q.areas[0].rect[1], 0.2, 1e-15);
}

// Freeze marks: the kind, the per-area hold and ease, t1 = t0, legacy areas without a kind left as they were, and the
// newer project fields written only where they differ from their defaults or were there.
void freeze_marks() {
    Project p;
    std::string error;
    CHECK(ParseProject(json::parse(SAMPLE), p, error));
    CHECK(p.areas[0].kind == AreaKind::SPAN && !p.areas[0].kind_stored);  // no kind: a span
    CHECK(!p.areas[0].hold && !p.areas[0].ease);
    CHECK_NEAR(p.freeze_hold, 0.5, 1e-12);
    CHECK(p.new_mark_kind == AreaKind::FREEZE);
    CHECK(!p.logos.stored && !p.logos.enabled);
    // a legacy area edited: no kind appears, nothing else changes but the edit
    p.areas[0].label = "counter";
    p.areas_dirty = true;
    json out = ProjectToJson(p);
    CHECK(!out["areas"][0].contains("kind") && !out["areas"][0].contains("hold") && !out["areas"][0].contains("ease"));
    CHECK(out["areas"][0]["colour"] == "orange");
    CHECK(!out.contains("freeze_hold") && !out.contains("new_mark_kind") && !out.contains("logos"));
    const std::vector<std::string> keys_before = {"id", "label", "t0", "t1", "rect", "divider", "colour"};
    std::vector<std::string> keys;
    for (auto it = out["areas"][0].begin(); it != out["areas"][0].end(); ++it)
        keys.push_back(it.key());
    CHECK(keys == keys_before);  // the area's key order is the file's
    // a new freeze mark
    Area f;
    f.id = 2;
    f.label = "detail";
    f.kind = AreaKind::FREEZE;
    f.kind_stored = true;
    f.t0 = f.t1 = 1233.0 / 60.0;
    f.rect[0] = 0.1;
    f.rect[1] = 0.2;
    f.rect[2] = 0.3;
    f.rect[3] = 0.4;
    f.hold = 1.25;
    f.ease = 0.3;
    p.areas.push_back(f);
    p.freeze_hold = 0.75;
    p.new_mark_kind = AreaKind::SPAN;
    out = ProjectToJson(p);
    CHECK(out["areas"][1]["kind"] == "freeze");
    CHECK(out["areas"][1]["hold"] == 1.25 && out["areas"][1]["ease"] == 0.3);
    CHECK(out["areas"][1]["t0"] == out["areas"][1]["t1"]);
    CHECK(out["freeze_hold"] == 0.75 && out["new_mark_kind"] == "span");
    Project q;
    CHECK(ParseProject(out, q, error));
    CHECK_EQ((int)q.areas.size(), 2);
    CHECK(q.areas[1].kind == AreaKind::FREEZE && q.areas[1].kind_stored);
    CHECK(q.areas[1].hold && *q.areas[1].hold == 1.25 && q.areas[1].ease && *q.areas[1].ease == 0.3);
    CHECK(q.areas[0].kind == AreaKind::SPAN && !q.areas[0].kind_stored);
    CHECK(q.areas[1].t0 == 1233.0 / 60.0);  // the frame's time, exactly
    CHECK_NEAR(q.freeze_hold, 0.75, 1e-12);
    CHECK(q.new_mark_kind == AreaKind::SPAN);
    // an override removed leaves no key; an explicit span keeps its kind; a freeze turned span writes its t1
    q.areas[1].hold.reset();
    q.areas[1].kind = AreaKind::SPAN;
    q.areas[1].t1 = 5.0;
    q.areas_dirty = true;
    out = ProjectToJson(q);
    CHECK(!out["areas"][1].contains("hold") && out["areas"][1]["ease"] == 0.3);
    CHECK(out["areas"][1]["kind"] == "span" && out["areas"][1]["t1"] == 5.0);
    // unknown fields of a freeze area stay
    json doc = json::parse(SAMPLE);
    doc["areas"].push_back({{"id", 7}, {"label", "x"}, {"kind", "freeze"}, {"t0", 2.0}, {"t1", 2.0},
                            {"rect", {0.1, 0.1, 0.2, 0.2}}, {"divider", nullptr}, {"mine", true}});
    Project r;
    CHECK(ParseProject(doc, r, error));
    CHECK(r.areas[1].kind == AreaKind::FREEZE);
    CHECK(ProjectToJson(r) == doc);
    r.areas[1].label = "y";
    r.areas_dirty = true;
    CHECK(ProjectToJson(r)["areas"][1]["mine"] == true);
    // an unknown kind is a span without a kind
    doc["areas"][1]["kind"] = "bounce";
    CHECK(ParseProject(doc, r, error));
    CHECK(r.areas[1].kind == AreaKind::SPAN && !r.areas[1].kind_stored);
}

// A save makes the document what was written: a later edit of something else must not bring back the areas it held
// before (a project that was read from a file has its areas array in the document).
void edits_after_a_save_keep_earlier_edits() {
    Project p;
    std::string error;
    CHECK(ParseProject(json::parse(SAMPLE), p, error));
    p.areas[0].label = "edited";
    p.areas_dirty = true;
    CHECK(ProjectToJson(p)["areas"][0]["label"] == "edited");
    MarkSaved(p);
    CHECK(!p.areas_dirty && !p.logos.items_dirty);
    p.offset_frames = 400;  // another edit, another save: the label stays
    const json out = ProjectToJson(p);
    CHECK(out["areas"][0]["label"] == "edited");
    CHECK(out["offset_frames"] == 400);
    CHECK(out["areas"][0]["colour"] == "orange" && out["from_the_future"].contains("x"));  // unknown fields stay
}

// Swap A and B: the files, labels and the offset's sign, the marks and the detection on the same moments, the logos
// on the other sides, a default title in the new order, the unknown fields of a side with it; twice is the identity.
void swap_sides() {
    Project p;
    std::string error;
    json doc = json::parse(SAMPLE);
    doc["areas"] = json::parse(R"([
        {"id": 1, "label": "span", "t0": 30, "t1": 33, "rect": [0.4, 0.3, 0.2, 0.1], "divider": null},
        {"id": 2, "label": "freeze", "kind": "freeze", "t0": 20.0, "t1": 20.0, "rect": [0.1, 0.1, 0.2, 0.2], "divider": null}])");
    CHECK(ParseProject(doc, p, error));
    const Project before = p;
    p.title = DefaultTitle(p.path_a, p.path_b);
    p.logos.enabled = true;
    const int64_t k = p.offset_frames;  // 413
    SwapSides(p);
    CHECK(p.path_a == "/videos/cyberpunk-dlssg-gen-only.mp4" && p.path_b == before.path_a);
    CHECK(p.label_a == "DLSS-G" && p.label_b == "ANR FG");
    CHECK_EQ(p.offset_frames, -k);
    CHECK(p.title == "cyberpunk-dlssg-gen-only vs cyberpunk-apex-gen-only");  // the default title follows the order
    // the marks: A's frame n of the old timeline is frame n - K of the new one
    CHECK_NEAR(p.areas[0].t0, 30.0 - 413.0 / 60.0, 1e-9);
    CHECK_NEAR(p.areas[0].t1, 33.0 - 413.0 / 60.0, 1e-9);
    CHECK_EQ((int)std::llround(p.areas[1].t0 * p.fps), 20 * 60 - 413);  // a frozen frame stays a whole frame
    CHECK(p.areas[1].t0 == p.areas[1].t1);
    CHECK(p.areas[1].kind == AreaKind::FREEZE && p.areas[0].kind == AreaKind::SPAN);
    CHECK(p.areas[0].rect[0] == 0.4 && p.areas[0].label == "span");  // the rectangles are the same pixels
    CHECK(p.areas_dirty);
    // the pair a mark shows is the same two frames: old (A n, B n - K) is new (A' m, B' m - K') with A' = B, B' = A
    const int64_t n = 20 * 60, m = (int64_t)std::llround(p.areas[1].t0 * p.fps);
    CHECK_EQ(m - p.offset_frames, n);   // B' = old A
    CHECK_EQ(m, n - k);                 // A' = old B
    // the detection is the other way round
    CHECK(p.sync.has_value());
    CHECK_EQ(p.sync->offset_frames, -413);
    CHECK_NEAR(p.sync->offset, -6.876958695087944, 1e-12);
    CHECK_NEAR(p.sync->coarse.offset, -6.9, 1e-12);
    CHECK_NEAR(p.sync->coarse.next_best_offset, -5.8, 1e-12);
    CHECK_NEAR(p.sync->windows[0].offset, -6.827145381752214, 1e-12);
    CHECK_NEAR(p.sync->windows[0].a_time, 12.470833333333331 - 6.827145381752214, 1e-9);  // B's time of that moment
    // logos stay with their video: the sides change (default: Apex left, NVIDIA right)
    const std::vector<LogoItem> logos = p.logos.effective();
    CHECK(logos[0].id == "apex" && logos[0].corner == Corner::BOTTOM_RIGHT);
    CHECK(logos[1].id == "nvidia" && logos[1].corner == Corner::BOTTOM_LEFT);
    // what is written: the files and the labels, the "b" object's unknown field with B's file (now A)
    json out = ProjectToJson(p);
    CHECK(out["a"]["path"] == p.path_a && out["a"]["note"] == "kept" && !out["b"].contains("note"));
    CHECK(out["labels"]["a"] == "DLSS-G" && out["offset_frames"] == -413);
    CHECK(out["areas"][0]["divider"].is_null() && out["areas"][1]["kind"] == "freeze");
    CHECK(out["logos"]["items"][0]["corner"] == "bottom-right");
    // twice is the identity
    SwapSides(p);
    CHECK(p.path_a == before.path_a && p.path_b == before.path_b);
    CHECK(p.label_a == before.label_a && p.label_b == before.label_b);
    CHECK_EQ(p.offset_frames, k);
    CHECK_NEAR(p.areas[0].t0, 30.0, 1e-9);
    CHECK_NEAR(p.areas[0].t1, 33.0, 1e-9);
    CHECK_NEAR(p.areas[1].t0, 20.0, 1e-12);
    CHECK_EQ(p.sync->offset_frames, 413);
    CHECK_NEAR(p.sync->windows[0].a_time, 12.470833333333331, 1e-9);
    CHECK_NEAR(p.sync->windows[0].offset, 6.827145381752214, 1e-12);
    CHECK(p.logos.effective()[0].corner == Corner::BOTTOM_LEFT);
    CHECK(p.title == "cyberpunk-apex-gen-only vs cyberpunk-dlssg-gen-only");
    CHECK(ProjectToJson(p)["b"]["note"] == "kept");
    // a title the user wrote stays; a zero offset moves no mark; a project without a detection or marks works
    Project q = before;
    q.title = "My comparison";
    q.offset_frames = 0;
    q.sync.reset();
    SwapSides(q);
    CHECK(q.title == "My comparison" && q.offset_frames == 0 && !q.sync);
    CHECK_NEAR(q.areas[0].t0, 30.0, 1e-12);
    Project empty = NewProject("/v/a.mp4", "/v/b.mp4", 1920, 1080, 60.0);
    empty.offset_frames = -90;
    SwapSides(empty);
    CHECK_EQ(empty.offset_frames, 90);
    CHECK(empty.areas.empty() && empty.title == "b vs a" && empty.label_a == "b");
}

void logos_settings() {
    Project p;
    std::string error;
    json doc = json::parse(SAMPLE);
    doc["logos"] = {{"enabled", false},
                    {"note", "kept"},
                    {"items", json::array({{{"id", "apex"}, {"path", "a.png"}, {"corner", "top-left"}, {"height", 0.08},
                                            {"margin", 0.03}, {"opacity", 0.5}, {"backdrop", true}, {"extra", 1}},
                                           {{"id", "nvidia"}, {"path", "n.png"}}})}};
    CHECK(ParseProject(doc, p, error));
    CHECK(p.logos.stored && p.logos.enabled && !*p.logos.enabled && p.logos.has_items);
    CHECK_EQ((int)p.logos.items.size(), 2);
    const LogoItem& a = p.logos.items[0];
    CHECK(a.id == "apex" && a.path == "a.png" && a.corner == Corner::TOP_LEFT && a.backdrop);
    CHECK_NEAR(a.height, 0.08, 1e-12);
    CHECK_NEAR(a.margin, 0.03, 1e-12);
    CHECK_NEAR(a.opacity, 0.5, 1e-12);
    const LogoItem& n = p.logos.items[1];  // the rest from the defaults
    CHECK(n.corner == Corner::BOTTOM_LEFT && !n.backdrop);
    CHECK_NEAR(n.height, 0.05, 1e-12);
    CHECK_NEAR(n.opacity, 0.9, 1e-12);
    CHECK(ProjectToJson(p) == doc);  // untouched: as it was
    // edited: unknown keys stay, the item's own fields are written
    p.logos.enabled = true;
    p.logos.edit()[0].opacity = 0.7;
    p.logos.edit()[1].corner = Corner::TOP_RIGHT;
    json out = ProjectToJson(p);
    CHECK(out["logos"]["enabled"] == true && out["logos"]["note"] == "kept");
    CHECK(out["logos"]["items"][0]["extra"] == 1 && out["logos"]["items"][0]["opacity"] == 0.7);
    CHECK(out["logos"]["items"][1]["corner"] == "top-right" && out["logos"]["items"][1]["backdrop"] == false);
    // a project without logos: the defaults; touching the box stores {"enabled"} only
    Project fresh;
    CHECK(ParseProject(json::parse(SAMPLE), fresh, error));
    const std::vector<LogoItem> items = fresh.logos.effective();
    CHECK_EQ((int)items.size(), 2);
    CHECK(items[0].id == "apex" && items[1].id == "nvidia");
    fresh.logos.enabled = false;
    fresh.logos.stored = true;
    out = ProjectToJson(fresh);
    CHECK(out["logos"]["enabled"] == false && !out["logos"].contains("items"));
    Project again;
    CHECK(ParseProject(out, again, error));
    CHECK(!again.logos.has_items && again.logos.effective().size() == 2);
    // edit() starts from the defaults and writes every item
    fresh.logos.edit()[1].backdrop = true;
    out = ProjectToJson(fresh);
    CHECK_EQ((int)out["logos"]["items"].size(), 2);
    CHECK(out["logos"]["items"][0]["id"] == "apex" && out["logos"]["items"][0]["corner"] == "bottom-left");
    CHECK(out["logos"]["items"][1]["backdrop"] == true);
    // bad values are clamped, an unknown corner is the default
    doc["logos"] = {{"items", json::array({{{"id", "x"}, {"path", "x"}, {"corner", "middle"}, {"height", 9}, {"opacity", -1}}})}};
    CHECK(ParseProject(doc, p, error));
    CHECK(p.logos.items[0].corner == Corner::BOTTOM_LEFT);
    CHECK_NEAR(p.logos.items[0].height, 0.5, 1e-12);
    CHECK_NEAR(p.logos.items[0].opacity, 0.0, 1e-12);
}

void file_round_trip() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "nr-clips-project-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "x.compare.json").string();
    Project p;
    std::string error;
    CHECK(ParseProject(json::parse(SAMPLE), p, error));
    p.file = path;
    CHECK(SaveProject(p, path, error));
    CHECK(!fs::exists(path + ".tmp"));
    Project q;
    CHECK(LoadProject(path, q, error));
    CHECK(ProjectToJson(q) == json::parse(SAMPLE));
    // a second save replaces the first
    q.offset_frames = 400;
    CHECK(SaveProject(q, path, error));
    Project r;
    CHECK(LoadProject(path, r, error));
    CHECK_EQ(r.offset_frames, 400);
    // errors say what is wrong
    CHECK(!LoadProject((dir / "missing.json").string(), r, error));
    CHECK(!error.empty());
    std::ofstream((dir / "bad.json").string()) << "{not json";
    CHECK(!LoadProject((dir / "bad.json").string(), r, error));
    std::ofstream((dir / "nopath.json").string()) << R"({"version":1,"a":{"path":"x"}})";
    CHECK(!LoadProject((dir / "nopath.json").string(), r, error));
    CHECK(error.find("b.path") != std::string::npos);
    fs::remove_all(dir);
}

void new_projects_and_paths() {
    CHECK(DefaultProjectPath("/v/apex.mp4", "/w/dlssg.mp4") == "/v/apex-vs-dlssg.compare.json");
    const Project p = NewProject("/v/apex.mp4", "/w/dlssg.mp4", 1920, 1080, 60.0);
    CHECK(p.path_a == "/v/apex.mp4" && p.path_b == "/w/dlssg.mp4");
    CHECK(p.label_a == "apex" && p.label_b == "dlssg");
    CHECK_EQ(p.width, 1920);
    CHECK(p.layout == "slider");
    CHECK_NEAR(p.divider, 0.5, 1e-12);
    CHECK_NEAR(p.ease, 0.6, 1e-12);
    CHECK_NEAR(p.hold, 3.0, 1e-12);
    CHECK_NEAR(p.freeze_hold, 0.5, 1e-12);
    CHECK(p.new_mark_kind == AreaKind::FREEZE);
    const json doc = ProjectToJson(p);
    CHECK(doc["version"] == 1 && doc["areas"].empty() && doc["offset_frames"] == 0);
    CHECK(!doc.contains("sync"));
    // relative paths resolve against the project's folder
    Project rel = p;
    rel.path_a = "clips/a.mp4";
    rel.file = "/proj/x.compare.json";
    CHECK(rel.ResolvedA() == "/proj/clips/a.mp4");
    rel.path_b = "/abs/b.mp4";
    CHECK(rel.ResolvedB() == "/abs/b.mp4");
}

void sync_json() {
    SyncResult s;
    s.offset = 6.8;
    s.offset_frames = 408;
    s.quality = "coarse";
    s.coarse.offset = 6.8;
    s.windows.push_back({1.0, 2.0, 0.9, 0.2, 5});
    json j = SyncToJson(s);
    CHECK(j["spread_seconds"].is_null());
    auto back = SyncFromJson(j);
    CHECK(back.has_value());
    CHECK(!back->spread_seconds.has_value());
    CHECK_EQ(back->offset_frames, 408);
    CHECK_EQ((int)back->windows.size(), 1);
    CHECK(!SyncFromJson(json::object()).has_value());
}


std::vector<std::string> keys_of(const json& j) {
    std::vector<std::string> keys;
    for (auto it = j.begin(); it != j.end(); ++it)
        keys.push_back(it.key());
    return keys;
}

// The offset map in the project: a project without one is as before (nothing written, one constant); a map is read sorted,
// followed unless the file says not, written in place with the unknown fields and the key order kept; clearing it takes the
// keys out and leaves the constant.
void offset_maps_in_projects() {
    Project p;
    std::string error;
    CHECK(ParseProject(json::parse(SAMPLE), p, error));
    CHECK(p.offset_map.empty() && !p.follow_drift);
    CHECK(p.Offsets().is_constant());
    CHECK_EQ(p.Offsets().at(5000), 413);
    json plain = ProjectToJson(p);
    CHECK(!plain.contains("offset_map") && !plain.contains("follow_drift"));
    CHECK(keys_of(plain) == keys_of(json::parse(SAMPLE)));

    json doc = json::parse(SAMPLE);
    doc["offset_map"] = json::parse(R"([{"a": 4000, "k": 415.5}, {"a": 500, "k": 413}, {"a": 501, "k": 420.0}])");
    CHECK(ParseProject(doc, p, error));
    CHECK_EQ((int)p.offset_map.size(), 3);
    CHECK_EQ(p.offset_map[0].a, 500);  // sorted
    CHECK(p.follow_drift);  // a map without the field is followed
    const OffsetMap k = p.Offsets();
    CHECK_EQ(k.at(100), 413);
    CHECK_EQ(k.at(500), 413);
    CHECK_EQ(k.at(501), 420);
    CHECK_EQ(k.at(4000), 416);  // 415.5 rounds up
    CHECK_EQ(p.offset_frames, 413);  // the K at the first point, for readers that know no maps
    json written = ProjectToJson(p);
    CHECK(keys_of(written) == keys_of(doc) || (keys_of(written).size() == keys_of(doc).size() + 1));
    CHECK(written["from_the_future"]["x"][1] == 2 && written["offset_map"][1]["k"] == 420.0 && written["follow_drift"] == true);
    {
        // the key order of the document stays: offset_map where the document has it, after the keys it had
        const std::vector<std::string> before = keys_of(doc), after = keys_of(written);
        for (size_t i = 0; i < before.size(); i++)
            CHECK(after[i] == before[i]);
    }
    doc["follow_drift"] = false;
    CHECK(ParseProject(doc, p, error));
    CHECK(!p.follow_drift);
    CHECK(p.Offsets().is_constant());
    CHECK_EQ(p.Offsets().at(4000), 413);  // the constant: offset_frames
    // an edit of the map is written (the follow flag with it); clearing takes both out and keeps the constant
    p.follow_drift = true;
    p.SetOffsets(OffsetMap(std::vector<OffsetPoint>{{10, 400.0}, {20, 401.0}}));
    CHECK_EQ(p.offset_frames, 400);
    json edited = ProjectToJson(p);
    CHECK(edited["offset_map"].size() == 2 && edited["offset_map"][0]["a"] == 10 && edited["follow_drift"] == true);
    CHECK(edited["offset_frames"] == 400);
    p.SetOffsets(OffsetMap(399));
    json cleared = ProjectToJson(p);
    CHECK(!cleared.contains("offset_map") && !cleared.contains("follow_drift") && cleared["offset_frames"] == 399);
    // a new project with a map writes it at the end
    Project fresh = NewProject("/v/a.mp4", "/v/b.mp4", 1920, 1080, 60.0);
    CHECK(!ProjectToJson(fresh).contains("offset_map"));
    fresh.follow_drift = true;
    fresh.SetOffsets(OffsetMap(std::vector<OffsetPoint>{{0, 5.0}, {600, 6.0}}));
    CHECK(ProjectToJson(fresh)["offset_map"].size() == 2);
}

// Swapping with a map: the points are re-indexed, each mark moves by K at its own frame, the tracking summary is mirrored.
void swap_with_offset_map() {
    Project p;
    std::string error;
    json doc = json::parse(SAMPLE);
    doc["areas"] = json::parse(R"([
        {"id": 1, "label": "span", "t0": 2.0, "t1": 3.0, "rect": [0.4, 0.3, 0.2, 0.1], "divider": null},
        {"id": 2, "label": "freeze", "kind": "freeze", "t0": 10.0, "t1": 10.0, "rect": [0.1, 0.1, 0.2, 0.2], "divider": null}])");
    doc["offset_frames"] = 400;
    doc["offset_map"] = json::parse(R"([{"a": 0, "k": 400.0}, {"a": 1000, "k": 410.0}])");  // K(n) = 400 + n / 100
    CHECK(ParseProject(doc, p, error));
    p.sync->tracking.present = true;
    p.sync->tracking.slope_ppm = 10000.0;
    p.sync->tracking.drift_frames = 10.0;
    p.sync->tracking.jumps.push_back({600.0 / 60.0, -20.0});
    p.sync->tracking.map = {{0, 400.0}, {1000, 410.0}};
    const Project before = p;
    SwapSides(p);
    CHECK_EQ((int)p.offset_map.size(), 2);
    CHECK_EQ(p.offset_map[0].a, -400);
    CHECK_NEAR(p.offset_map[0].k, -400.0, 1e-12);
    CHECK_EQ(p.offset_map[1].a, 590);
    CHECK_EQ(p.offset_frames, -400);  // the K at the first point
    CHECK(p.follow_drift);
    // the freeze at A frame 600 (K = 406) is at frame 194 now, with K' = -406 there: the same pair
    const int64_t m = (int64_t)std::llround(p.areas[1].t0 * p.fps);
    CHECK_EQ(m, 600 - 406);
    CHECK_EQ(p.Offsets().at(m), -406);
    // the span: each end moves by K at its own frame (A frame 120 has K = 401, 180 has 402)
    CHECK_NEAR(p.areas[0].t0, 2.0 - 401.0 / 60.0, 1e-9);
    CHECK_NEAR(p.areas[0].t1, 3.0 - 402.0 / 60.0, 1e-9);
    // the tracking: the jump on B's clock, its sign and the slope the other way round
    CHECK_NEAR(p.sync->tracking.jumps[0].frames, 20.0, 1e-12);
    CHECK_NEAR(p.sync->tracking.jumps[0].a_time, 10.0 - 406.0 / 60.0, 1e-9);
    CHECK_NEAR(p.sync->tracking.drift_frames, -10.0, 1e-12);
    CHECK(p.sync->tracking.slope_ppm < 0.0 && std::fabs(p.sync->tracking.slope_ppm + 10101.0) < 2.0);
    // twice is the identity
    SwapSides(p);
    CHECK_EQ((int)p.offset_map.size(), 2);
    CHECK(OffsetMap(p.offset_map) == OffsetMap(before.offset_map));
    CHECK_EQ(p.offset_frames, 400);
    CHECK_NEAR(p.areas[0].t0, 2.0, 1e-9);
    CHECK_NEAR(p.areas[0].t1, 3.0, 1e-9);
    CHECK_NEAR(p.areas[1].t0, 10.0, 1e-9);
    CHECK_NEAR(p.sync->tracking.jumps[0].a_time, 10.0, 1e-9);
    // a map that is not followed stays as it was, and the constant is swapped
    Project q = before;
    q.follow_drift = false;
    SwapSides(q);
    CHECK_EQ(q.offset_frames, -400);
    CHECK_EQ((int)q.offset_map.size(), 2);
    CHECK(!q.follow_drift);
    CHECK_NEAR(q.areas[0].t0, 2.0 - 400.0 / 60.0, 1e-9);  // the constant moves the marks
}

// The tracker's summary survives the file; a window of an older file counts when its peak is sharp.
void tracking_json() {
    SyncResult s;
    s.offset = 6.8;
    s.offset_frames = 408;
    s.quality = "jumps";
    s.windows.push_back({1.0, 2.0, 0.9, 0.2, 5, 0.8, true, 0.1});
    s.windows.push_back({5.0, 2.1, 0.5, 0.05, 30, 0.0, false, -3.0});
    s.tracking.present = true;
    s.tracking.slope_ppm = -123.5;
    s.tracking.drift_frames = -2.5;
    s.tracking.jumps.push_back({50.0, 30.0});
    s.tracking.windows_used = 1;
    s.tracking.windows_rejected = 1;
    s.tracking.confidence = 0.7;
    s.tracking.mask_fraction = 0.06;
    s.tracking.significant = true;
    s.tracking.map = {{100, 408.0}, {2999, 405.0}, {3000, 435.0}};
    const auto back = SyncFromJson(SyncToJson(s));
    CHECK(back.has_value() && back->tracking.present);
    CHECK_NEAR(back->tracking.slope_ppm, -123.5, 1e-12);
    CHECK_EQ((int)back->tracking.jumps.size(), 1);
    CHECK_NEAR(back->tracking.jumps[0].frames, 30.0, 1e-12);
    CHECK(back->tracking.significant && back->tracking.windows_used == 1 && back->tracking.windows_rejected == 1);
    CHECK_EQ((int)back->tracking.map.size(), 3);
    CHECK_EQ(back->tracking.map[2].a, 3000);
    CHECK(back->windows[0].used && !back->windows[1].used);
    CHECK_NEAR(back->windows[1].residual_frames, -3.0, 1e-12);
    // not written for a result that has no tracking, and an older file's windows: sharp or not
    SyncResult old;
    old.offset_frames = 5;
    old.windows.push_back({1.0, 2.0, 0.9, 0.2, 5});
    old.windows.push_back({2.0, 2.0, 0.9, 0.07, 25});
    const json j = SyncToJson(old);
    CHECK(!j.contains("tracking") && !j["windows"][0].contains("used"));
    const auto old_back = SyncFromJson(j);
    CHECK(old_back->windows[0].used && !old_back->windows[1].used && !old_back->tracking.present);
}

}  // namespace

// A project whose A is NVIDIA's DLSS-G (as scripts make them) without stored logos: each logo on its product's
// side; once swapped, both move with their videos and stay right.
void logos_follow_labels() {
    Project p;
    std::string error;
    const json doc = json::parse(R"j({"version": 1, "a": {"path": "a.mp4"}, "b": {"path": "b.mp4"},
        "labels": {"a": "DLSS-G", "b": "ANR FG (b)"}, "layout": "slider", "fps": 60, "size": [64, 32]})j");
    CHECK(ParseProject(doc, p, error));
    std::vector<LogoItem> logos = p.logos.effective();
    CHECK(logos[0].id == "apex" && logos[0].corner == Corner::BOTTOM_RIGHT);
    CHECK(logos[1].id == "nvidia" && logos[1].corner == Corner::BOTTOM_LEFT);
    CHECK(!ProjectToJson(p).contains("logos"));  // not written until edited
    SwapSides(p);
    logos = p.logos.effective();
    CHECK(p.label_a == "ANR FG (b)" && logos[0].corner == Corner::BOTTOM_LEFT && logos[1].corner == Corner::BOTTOM_RIGHT);
}

int main() {
    parse_the_sample();
    logos_follow_labels();
    save_keeps_everything();
    freeze_marks();
    edits_after_a_save_keep_earlier_edits();
    swap_sides();
    logos_settings();
    file_round_trip();
    new_projects_and_paths();
    sync_json();
    offset_maps_in_projects();
    swap_with_offset_map();
    tracking_json();
    return check_result("project");
}
