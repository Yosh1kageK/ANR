// A game capture as the Captures tab sees it: what it holds of each real frame, and the pairs of frames a generated frame
// can be made from. nr-record recordings (recording.h) and the folder formats a build adds (store.h FolderFormat) are read
// into it. Pure (no GPU, no OS), tested by captures_test.cpp.
//
// A real pair is real frames (k - 1, k) and one of the generated slots the game asked for between them (2X: t = 1/2;
// 3X: 1/3, 2/3; 4X: 1/4, 2/4, 3/4); a skip-one is real frame k generated from k - 1 and k + 1 at t = 1/2, with the real frame
// k as an approximate reference (its timing is unverified).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>


namespace renderer::clips::captures {

// How a source's colour values are coded: what the display path must do with them.
enum class Coding { SRGB8, PQ10, SCRGB16, VIZ };  // VIZ: display-referred false colour (depth, motion), as sRGB codes

struct TextureInfo {
    uint32_t width = 0, height = 0;
    int dxgi = 0;
    std::string texel;  // "float16x4", "float32" or "float16x2"
};

struct CallInfo {
    int index = 0;      // 1-based slot of the pair
    double t = 0.0;
    bool has_generated = false;  // the game's own generated frame (f<k>_generated_<index>.bin)
    TextureInfo generated;
};

struct FrameInfo {
    int64_t real_frame = 0;
    std::map<std::string, TextureInfo> files;  // backbuffer, hudless, ui, ui_alpha, depth, motion
    std::vector<CallInfo> calls;
    bool after_gap = false;  // real frames were dropped right before this one: no pair spans the gap
};

struct CaptureInfo {
    std::string folder, name;  // the folder, and its last component (the capture's identity: a time)
    std::string format;        // "nr-record", or the FolderFormat's name
    std::string game_path, game, time, encoding;
    std::string in_game;       // what made the game's own frames (a recorder, a provider's model), where the capture says
    int64_t epoch = 0;         // time as seconds since 1970 (read as UTC: only for ordering)
    uint32_t width = 0, height = 0;
    int multiplier = 2;
    Coding coding = Coding::SRGB8;
    std::vector<FrameInfo> frames;

    const FrameInfo* Frame(int k) const { return k >= 0 && k < (int)frames.size() ? &frames[(size_t)k] : nullptr; }
    bool Has(int k, const std::string& texture) const { return Frame(k) && Frame(k)->files.count(texture); }
};

// "The Last of Us Part II" from ".../steamapps/common/The Last of Us Part II/tlou-ii.exe" (else the executable's stem).
std::string GameName(const std::string& game_path);
// "2026-11-07T21:33:39" as seconds since 1970 (UTC); 0 when it does not parse. "2026-11-07 21:33" for display.
int64_t ParseTime(const std::string& time);
std::string DisplayTime(const std::string& time);
// The coding of a capture's colours from its "encoding" string: PQ10, scRGB16, else sRGB codes (also "sRGB8 (10-bit reads)").
Coding CodingOf(const std::string& encoding);

enum class PairKind { REAL, SKIP_ONE };

struct PairSpec {
    PairKind kind = PairKind::REAL;
    int a = 0, b = 1;       // the real frames the generated frame comes from
    int index = 0;          // REAL: the slot (1-based)
    double t = 0.5;
    int rendered = -1;      // SKIP_ONE: the real frame in the middle (approximate reference)

    // The name a generated frame's raw file has in the cache: "real-pair_f4-f5_t0.333", "skip-one_f4-f6_t0.500".
    std::string Stem() const;
    // "frames 4 and 5, slot 1 (t = 1/3)", "skip-one: frame 5 from 4 and 6 (t = 1/2)".
    std::string Label(int multiplier) const;
    // The pair as scripts and tools name it: "real:5:1" or "skip:5".
    std::string Only() const;
    bool operator==(const PairSpec& o) const { return kind == o.kind && a == o.a && b == o.b && index == o.index; }
};

// Every pair of a capture: the real pairs of each frame k >= 1 at each of its calls, then the skip-ones of the frames 1 to n - 2
// (none across dropped frames).
std::vector<PairSpec> EnumeratePairs(const CaptureInfo& capture);
// The slot times of a multiplier: 1/m, ..., (m - 1)/m.
std::vector<double> SlotTimes(int multiplier);
// "1/3" for t near a slot of the multiplier, else "0.333".
std::string FractionLabel(double t, int multiplier);

// A capture folder's file of a texture of a real frame ("f5_hudless.bin") and of a generated frame of the game
// ("f5_generated_2.bin"), for formats that keep each texture in a file.
std::string TextureFile(int frame, const std::string& name);
std::string GeneratedFile(int frame, int index);

}  // namespace renderer::clips::captures
