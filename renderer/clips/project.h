// The compare project file (.compare.json), the schema of the compare tool's earlier Python version and web player: version,
// title, a.path, b.path, labels, layout, divider, ease, hold, areas, offset_frames, fps, size, sync; and the fields of
// the freeze marks and the logos: freeze_hold, new_mark_kind, an area's kind, hold and ease, and logos (written only
// where they differ from their defaults or were already in the file). Fields this code does not know stay as they were
// on save (the whole document is kept and only the known fields are written into it), and so do the unknown fields of
// each area and logo. Pure (no GPU, no OS apart from files), tested by project_test.cpp.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>  // ordered_json: the document keeps its keys in the order they were written
#include "renderer/clips/camera.h"
#include "renderer/clips/logos.h"
#include "renderer/clips/offset_map.h"

namespace renderer::clips {

// A start-sync detection (video_compare.py estimate_offset's result, the project's "sync"; the tracking summary is ours).
struct SyncCoarse {
    double offset = 0.0, similarity = 0.0, next_best = 0.0, next_best_offset = 0.0, z = 0.0;
};
struct SyncWindow {
    double a_time = 0.0, offset = 0.0, peak = 0.0, contrast = 0.0;
    int width_frames = 0;
    // The tracker's (windows of older files have no such fields: they count when their peak is sharp)
    double confidence = 0.0;       // 0 to 1, from the peak's contrast and width
    bool used = true;              // the fit used it; false: rejected (a weak peak, or an outlier of the fit)
    double residual_frames = 0.0;  // its offset minus the fit's, in frames
};
// A pause or a stutter: K changes by `frames` (B went ahead or fell back) at A time `a_time` (seconds).
struct SyncJump {
    double a_time = 0.0, frames = 0.0;
};
// What the dense tracker found: the drift as a slope and the jumps, and the offset map it fitted.
struct SyncTracking {
    bool present = false;
    double slope_ppm = 0.0;     // dK/dn in parts per million: frames of drift per million frames of A
    double drift_frames = 0.0;  // K at the end of the overlap minus K at its start, jumps left out
    std::vector<SyncJump> jumps;
    int windows_used = 0, windows_rejected = 0;
    double confidence = 0.0;     // 0 to 1: the share of the windows that agree with the fit, weighted by their own
    double mask_fraction = 0.0;  // the share of the picture left out of the match as static or overlay
    bool significant = false;    // more than a frame of drift over the overlap, or a jump
    std::vector<OffsetPoint> map;  // the fitted control points (A frame, K)
};
struct SyncResult {
    double offset = 0.0;  // seconds, A time = B time + offset
    int64_t offset_frames = 0;
    std::string quality;  // "fine", "drifting", "jumps", "weak" or "coarse"
    std::optional<double> spread_seconds;
    SyncCoarse coarse;
    std::vector<SyncWindow> windows;
    SyncTracking tracking;
    std::string detected_at, method;
};

nlohmann::ordered_json SyncToJson(const SyncResult& sync);
std::optional<SyncResult> SyncFromJson(const nlohmann::ordered_json& j);

struct Project {
    int version = 1;
    std::string title;
    std::string path_a, path_b;      // as stored
    std::string label_a = "A", label_b = "B";
    std::string layout = "slider";   // "slider" or "side-by-side"
    double divider = 0.5;
    double ease = DEFAULT_EASE;             // seconds of zoom in and out (spans and freezes)
    double hold = DEFAULT_HOLD;             // seconds a new span mark lasts
    double freeze_hold = DEFAULT_FREEZE_HOLD;  // seconds a freeze mark waits at its zoom
    AreaKind new_mark_kind = AreaKind::FREEZE;  // the kind a newly drawn mark gets
    LogoSettings logos;
    std::vector<Area> areas;
    // The offset between the recordings: K, a constant (`offset_frames`), or K(n) by A frame from control points
    // (`offset_map`, offset_map.h) when `follow_drift` is on. offset_frames stays the K at the first control point while
    // the map is followed (so a reader that knows no maps is right there), and the constant K otherwise.
    int64_t offset_frames = 0;
    std::vector<OffsetPoint> offset_map;
    bool follow_drift = false;  // the map is used (a map without the field is followed)
    double fps = 0.0;
    int width = 0, height = 0;
    std::optional<SyncResult> sync;

    // Where it came from (relative paths resolve against its directory), and the document it was read from.
    std::string file;
    nlohmann::ordered_json raw = nlohmann::ordered_json::object();
    bool areas_dirty = false;  // the areas were edited: written back; otherwise the document's array stays as it was

    std::string ResolvedA() const;
    std::string ResolvedB() const;

    // K(n) as playback and export use it: the map when it is followed, else the constant.
    OffsetMap Offsets() const {
        return follow_drift && !offset_map.empty() ? OffsetMap(offset_map, offset_frames) : OffsetMap(offset_frames);
    }
    // Makes `map` the project's offsets: the map's points are stored (none: the constant `map.first_k()` stays), and
    // offset_frames is its first K.
    void SetOffsets(const OffsetMap& map) {
        offset_map = map.points();
        offset_frames = map.first_k();
    }
};

// Parses a project document. The error says what is wrong.
bool ParseProject(const nlohmann::ordered_json& document, Project& project, std::string& error);
bool LoadProject(const std::string& path, Project& project, std::string& error);

// The document: the stored one with the known fields written into it.
nlohmann::ordered_json ProjectToJson(const Project& project);
// Writes it atomically (a temporary file, then a rename).
bool SaveProject(const Project& project, const std::string& path, std::string& error);
// After a successful save: the document is what was written, so that an edit of something else later does not write back
// the areas or logos the document held before.
void MarkSaved(Project& project);

// The default project file next to the first video: <a stem>-vs-<b stem>.compare.json.
std::string DefaultProjectPath(const std::string& path_a, const std::string& path_b);

// A new project's fields from the two recordings.
Project NewProject(const std::string& path_a, const std::string& path_b, int width, int height, double fps);
// "<a stem> vs <b stem>": the title NewProject gives.
std::string DefaultTitle(const std::string& path_a, const std::string& path_b);

// Swap A and B. The files and their labels change places and the offset changes sign (A = B + K becomes B = A - K), and
// what belonged to a moment of the recordings stays on that moment: A's timeline now starts at what was B's first
// frame, so a mark's times move by -K frames (K at the mark's own frame when the offset follows a map; every mark stays:
// one that falls outside the overlap is left out of the playback and the export as such marks are); the offset map's
// points are re-indexed the same way ({a, k} becomes {a - k, -k}); the sync detection's result is mirrored the same way; a logo stays with its
// video, so the corners swap sides (an Apex logo at the bottom left of A is at the bottom right when A is on the right);
// a title that was the default one follows the new order. Swapping twice gives the project back. Unknown fields of the
// "a" and "b" objects go with their side.
void SwapSides(Project& project);

std::string ExpandPath(const std::string& path, const std::string& base_directory);

}  // namespace renderer::clips
