// The sources of one capture pair: the images that can be A and B in the viewer. Real frames, the game's own generated frame
// ("In game (as captured)"), the rendered middle frame of a skip-one ("Rendered (approx.)"), every generator's frame that
// exists in the cache, and false colour of depth and motion. Pure (no GPU), tested by captures_test.cpp.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "renderer/clips/captures/capture_info.h"
#include "renderer/clips/captures/store.h"

namespace renderer::clips::captures {

enum class SourceKind { REAL_FRAME, IN_GAME, RENDERED, GENERATED, DEPTH, MOTION };

struct Source {
    std::string id;       // "real:4", "ingame", "rendered", "gen:dlss-g", "depth:5", "motion:5"
    std::string label;    // what the viewer calls it by default
    SourceKind kind = SourceKind::REAL_FRAME;
    std::string path;     // the file; a recorded texture: the recording's folder
    TextureInfo info;     // capture textures: the size and format the capture gives
    bool raw = false;     // a generator's raw frame (NRRAW1), else a capture texture
    bool recorded = false;  // a texture of a recording (recording.h): frame `frame`'s `role`
    std::string role;
    Coding coding = Coding::SRGB8;
    int frame = -1;       // the real frame
    std::string generator_id;
    bool stale = false;   // a generator's frame from an older tool revision or generator version
    bool apex = false, dlss = false;
};

// What a generator's frames in the cache offer for a pair: found by the pair's stem.
struct GeneratedFrame {
    Generator generator;
    std::string file;
    bool stale = false;
};

// The sources of a pair. `capture_dir`: where the textures are. Real frames and the game's frame exist only where the capture
// has the files; depth and motion are listed for the frames of the pair.
std::vector<Source> BuildSources(const CaptureInfo& capture, const PairSpec& pair, const std::vector<GeneratedFrame>& generated);

// The A and B a pair opens with: A the first ANR FG frame, else the game's own frame, else the earlier real frame; B the
// DLSS-G frame, else the rendered frame, else the later real frame (so a finished pair of jobs shows apex left of DLSS-G).
std::pair<std::string, std::string> DefaultPick(const std::vector<Source>& sources);

const Source* FindSource(const std::vector<Source>& sources, const std::string& id);

// The store's key of a source: its file, or its recording's frame and role (false colour: with its kind), so the same frame
// is one image in every pair.
inline std::string ImageKey(const Source& s) {
    const std::string where = s.recorded ? s.path + "#f" + std::to_string(s.frame) + ":" + s.role : s.path;
    return s.coding == Coding::VIZ ? where + "#" + std::to_string((int)s.kind) : where;
}


}  // namespace renderer::clips::captures
