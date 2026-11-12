// The Captures tab's pure parts: the pairs of a capture at 2X, 3X and 4X and across dropped frames, the sources of a pair
// (texture files and a recording's textures), nr-replay's command line and its frames in the cache, the texture file
// reader on a small synthetic capture, a raw frame, the SDR curve against values of the comparison tools' Python version,
// a 16-bit PNG with its cICP chunk, and what Steam games hold.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>

#include <Detex/stb_image.h>
#include "renderer/clips/captures/capture_info.h"
#include "renderer/clips/captures/capture_reader.h"
#include "renderer/clips/captures/curves.h"
#include "renderer/clips/captures/png_write.h"
#include "renderer/clips/captures/recording.h"
#include "renderer/clips/captures/sources.h"
#include "renderer/clips/captures/store.h"
#include "renderer/clips/check.h"

using namespace renderer::clips::captures;
namespace fs = std::filesystem;

namespace {

TextureInfo Texture(uint32_t w, uint32_t h, int dxgi, const char* texel) {
    TextureInfo t;
    t.width = w;
    t.height = h;
    t.dxgi = dxgi;
    t.texel = texel;
    return t;
}

// A capture of `frames` real frames at multiplier m: backbuffer and depth and motion each, and m - 1 calls per frame
// with the game's own generated frame.
CaptureInfo MakeCapture(int frames, int m) {
    CaptureInfo c;
    c.folder = "/captures/x";
    c.name = "x";
    c.format = "test";
    c.width = 4;
    c.height = 2;
    c.multiplier = m;
    c.encoding = "PQ10";
    c.coding = Coding::PQ10;
    for (int k = 0; k < frames; k++) {
        FrameInfo f;
        f.real_frame = 100 + k;
        f.files = {{"backbuffer", Texture(4, 2, 24, "float16x4")}, {"depth", Texture(4, 2, 40, "float32")},
                   {"motion", Texture(4, 2, 34, "float16x2")}};
        for (int i = 1; i < m; i++) {
            CallInfo call;
            call.index = i;
            call.t = (double)i / m;
            call.has_generated = true;
            call.generated = Texture(4, 2, 24, "float16x4");
            f.calls.push_back(call);
        }
        c.frames.push_back(f);
    }
    return c;
}

void WriteFile(const std::string& path, const void* data, size_t bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)data, (std::streamsize)bytes);
}

void TestPairs() {
    for (int m : {2, 3, 4}) {
        const CaptureInfo c = MakeCapture(4, m);
        const std::vector<PairSpec> pairs = EnumeratePairs(c);
        // frames 1..3 each with m - 1 slots, then the skip-ones of frames 1 and 2
        CHECK_EQ(pairs.size(), (size_t)(3 * (m - 1) + 2));
        CHECK(pairs[0].kind == PairKind::REAL && pairs[0].a == 0 && pairs[0].b == 1 && pairs[0].index == 1);
        CHECK_NEAR(pairs[0].t, 1.0 / m, 1e-9);
        const PairSpec& skip = pairs.back();
        CHECK(skip.kind == PairKind::SKIP_ONE && skip.a == 1 && skip.b == 3 && skip.rendered == 2);
        CHECK_NEAR(skip.t, 0.5, 1e-9);
        CHECK_EQ(SlotTimes(m).size(), (size_t)(m - 1));
    }
    const CaptureInfo c = MakeCapture(4, 3);
    const std::vector<PairSpec> pairs = EnumeratePairs(c);
    CHECK(pairs[1].Stem() == "real-pair_f0-f1_t0.667");
    CHECK(pairs.back().Stem() == "skip-one_f1-f3_t0.500");
    CHECK(pairs[1].Only() == "real:1:2" && pairs.back().Only() == "skip:2");
    // Frames dropped before frame 2: no pair spans the gap
    CaptureInfo gap = MakeCapture(4, 2);
    gap.frames[2].after_gap = true;
    for (const PairSpec& p : EnumeratePairs(gap))
        CHECK(!(p.a < 2 && p.b >= 2));
    CHECK_EQ(EnumeratePairs(gap).size(), (size_t)2);  // (0, 1) and (2, 3)
    CHECK(FractionLabel(0.333333, 3) == "1/3" && FractionLabel(0.5, 4) == "1/2" && FractionLabel(0.4, 3) == "0.400");
    CHECK(GameName("S:/steamapps/common/The Last of Us Part II/tlou-ii.exe") == "The Last of Us Part II");
    CHECK(GameName("C:\\Games\\x\\game.exe") == "game");
    CHECK_EQ(ParseTime("2026-11-07T21:33:39") - ParseTime("2026-11-07T21:33:09"), 30);
    CHECK_EQ(ParseTime("1970-01-02T00:00:00"), 86400);
    CHECK(DisplayTime("2026-11-07T21:33:39") == "2026-11-07 21:33");
    CHECK(CodingOf("PQ10") == Coding::PQ10 && CodingOf("sRGB8 (10-bit reads)") == Coding::SRGB8 && CodingOf("scRGB16") == Coding::SCRGB16);
}

void TestSources() {
    const CaptureInfo c = MakeCapture(4, 3);
    const std::vector<PairSpec> pairs = EnumeratePairs(c);
    std::vector<Source> real = BuildSources(c, pairs[2], {});  // frame 2, slot 1
    CHECK(FindSource(real, "real:1") && FindSource(real, "real:2") && FindSource(real, "ingame"));
    CHECK(FindSource(real, "real:1")->label == "Real frame 1");
    CHECK(FindSource(real, "real:1")->path == "/captures/x/f1_backbuffer.bin" && !FindSource(real, "real:1")->recorded);
    CHECK(FindSource(real, "ingame")->label == "In game (as captured)");
    CHECK(FindSource(real, "ingame")->path == "/captures/x/f2_generated_1.bin");
    CHECK(!FindSource(real, "rendered"));
    Generator apex, dlss;
    apex.kind = Generator::APEX;
    apex.id = "apex:m";
    apex.label = "ANR FG (m)";
    dlss.kind = Generator::DLSSG;
    dlss.id = "dlss-g";
    dlss.label = "DLSS-G";
    real = BuildSources(c, pairs[2], {{apex, "/r/a.nrraw", false}, {dlss, "/r/d.nrraw", false}});
    // A is the apex frame and B DLSS-G's once both exist; before, the game's frame against the later real frame
    CHECK(DefaultPick(real) == std::make_pair(std::string("gen:apex:m"), std::string("gen:dlss-g")));
    real = BuildSources(c, pairs[2], {});
    CHECK(DefaultPick(real) == std::make_pair(std::string("ingame"), std::string("real:2")));
    const std::vector<Source> skip = BuildSources(c, pairs.back(), {});  // frame 2 from 1 and 3
    CHECK(FindSource(skip, "rendered") && FindSource(skip, "rendered")->label == "Rendered (approx.)");
    CHECK(!FindSource(skip, "ingame") && FindSource(skip, "real:1") && FindSource(skip, "real:3"));
    CHECK(DefaultPick(skip).second == "rendered");
    // A recording's textures: the recording's folder, frame and role; distinct image keys per frame, role and kind
    CaptureInfo rec = MakeCapture(4, 3);
    rec.format = RECORDING_FORMAT;
    rec.folder = "/rec";
    const std::vector<Source> recorded = BuildSources(rec, EnumeratePairs(rec)[2], {});
    const Source* b = FindSource(recorded, "real:2");
    CHECK(b && b->recorded && b->path == "/rec" && b->frame == 2 && b->role == "backbuffer");
    const Source* depth = FindSource(recorded, "depth:2");
    CHECK(depth && depth->recorded && depth->role == "depth" && ImageKey(*depth) != ImageKey(*b));
    CHECK(ImageKey(*b) != ImageKey(*FindSource(recorded, "real:1")));
}

// nr-replay: its command line, its generated frames' names as pair stems, and that it makes recorded pairs only.
void TestReplay() {
    JobSpec spec;
    spec.capture_folder = "/rec";
    spec.generator.replay = true;
    spec.generator.kind = Generator::DLSSG;
    spec.generator.dll = "/g/nvngx_dlssg.dll";
    const CaptureInfo c = MakeCapture(4, 3);
    const std::vector<PairSpec> pairs = EnumeratePairs(c);
    spec.pairs = {pairs[2]};
    const std::vector<std::string> pair_command = ReplayCommand("nr-replay", spec, "/out");
    CHECK((pair_command == std::vector<std::string>{"nr-replay", "--record", "/rec", "--generator", "nvidia", "--dll",
                                                     "/g/nvngx_dlssg.dll", "--out", "/out", "--raw", "--frames", "1:3"}));
    spec.generator.kind = Generator::APEX;
    spec.pairs.clear();
    spec.all = true;
    const std::vector<std::string> all = ReplayCommand("/r/nr_replay_wine.py", spec, "/out");
    CHECK(all.size() == 12 && all[0] == "python3" && all[2] == "/r/nr_replay_wine.py" && all[6] == "provider");
    CHECK(CanMake(spec.generator, pairs[0]) && !CanMake(spec.generator, pairs.back()));
    CHECK_EQ(JobFrameCount(spec, c), 6);  // the real pairs of frames 1 to 3, two slots each
    // The cache: nr-replay's generated frames by pair stem, its real frames left out
    const fs::path base = fs::temp_directory_path() / ("captures_test_cache_" + std::to_string(::getpid()));
    fs::remove_all(base);
    Locations where;
    where.frames = base.string();
    Generator g;
    g.replay = true;
    g.id = "provider:abc";
    g.version = "abc";
    const fs::path folder = base / "cap" / (g.Dir() + "-older");
    fs::create_directories(folder);
    for (const char* name : {"generated_f0-f1_t0.333.nrraw", "real_f1-f1_t0.000.nrraw", "replay.json"})
        WriteFile((folder / name).string(), "x", 1);
    const std::vector<Cached> cached = CachedFrames(where, "cap", g);
    CHECK_EQ(cached.size(), (size_t)1);
    CHECK(!cached.empty() && cached[0].stem == pairs[0].Stem() && cached[0].stale);
    fs::remove_all(base);
}

void TestReader() {
    const fs::path dir = fs::temp_directory_path() / "nr-captures-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    TextureInfo info;
    info.width = 4;
    info.height = 2;
    info.dxgi = 24;
    info.texel = "float16x4";
    // Codes 0..1023 of a 10-bit capture, stored as code / 1023 in float16
    std::vector<uint16_t> colour(4 * 2 * 4);
    const int codes[8] = {0, 1, 100, 511, 512, 777, 1022, 1023};
    for (int i = 0; i < 8; i++) {
        colour[(size_t)i * 4 + 0] = FloatToHalf((float)codes[i] / 1023.0f);
        colour[(size_t)i * 4 + 1] = FloatToHalf((float)codes[(i + 1) % 8] / 1023.0f);
        colour[(size_t)i * 4 + 2] = FloatToHalf((float)codes[(i + 2) % 8] / 1023.0f);
        colour[(size_t)i * 4 + 3] = FloatToHalf(1.0f);
    }
    WriteFile((dir / "f0_backbuffer.bin").string(), colour.data(), colour.size() * 2);
    Image image;
    std::string error;
    CHECK(ReadColor((dir / "f0_backbuffer.bin").string(), info, image, error));
    CHECK_EQ(image.width, 4u);
    CHECK_EQ(CodeSteps(image.dxgi), 1023);
    for (int i = 0; i < 8; i++)  // every 10-bit code comes back exactly
        for (int c = 0; c < 3; c++)
            CHECK_EQ((int)std::lround(HalfToFloat(image.rgba[(size_t)i * 4 + (size_t)c]) * 1023.0f), codes[(i + c) % 8]);
    TextureInfo wrong = info;
    wrong.height = 3;
    CHECK(!ReadColor((dir / "f0_backbuffer.bin").string(), wrong, image, error));  // a size that does not match the file
    // Depth and motion
    const float depth[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    WriteFile((dir / "f0_depth.bin").string(), depth, sizeof(depth));
    std::vector<float> d;
    TextureInfo depth_info = info;
    depth_info.dxgi = 40;
    depth_info.texel = "float32";
    CHECK(ReadDepth((dir / "f0_depth.bin").string(), depth_info, d, error));
    CHECK_NEAR(d[7], 0.8, 1e-6);
    std::vector<uint16_t> motion(16);
    for (size_t i = 0; i < 16; i++)
        motion[i] = FloatToHalf((float)i - 8.0f);
    WriteFile((dir / "f0_motion.bin").string(), motion.data(), motion.size() * 2);
    TextureInfo motion_info = info;
    motion_info.dxgi = 34;
    motion_info.texel = "float16x2";
    std::vector<float> m;
    CHECK(ReadMotion((dir / "f0_motion.bin").string(), motion_info, m, error));
    CHECK_NEAR(m[0], -8.0, 1e-6);
    CHECK_NEAR(m[15], 7.0, 1e-6);
    const Image viz = DepthImage(d, 4, 2, 8, 4);
    CHECK_EQ(viz.rgba.size(), (size_t)(8 * 4 * 4));
    // A raw frame: the 32-byte header (magic, version 1, width, height, DXGI format, 4 channels, 0), then float16x4
    CHECK(WriteRawFrame((dir / "g.nrraw").string(), image, error));
    std::ifstream in(dir / "g.nrraw", std::ios::binary);
    unsigned char header[32];
    in.read((char*)header, 32);
    const unsigned char expected[32] = {'N', 'R', 'R', 'A', 'W', '1', 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 2, 0, 0, 0, 24, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(header, expected, 32) == 0);
    Image raw;
    CHECK(ReadRawFrame((dir / "g.nrraw").string(), raw, error));
    CHECK(raw.rgba == image.rgba && raw.dxgi == 24 && raw.width == 4 && raw.height == 2);
    uint32_t w = 0, h = 0;
    int dxgi = 0;
    CHECK(ReadRawHeader((dir / "g.nrraw").string(), w, h, dxgi) && w == 4 && h == 2 && dxgi == 24);
    WriteFile((dir / "bad.nrraw").string(), "not a frame at all, not a frame at all", 38);
    CHECK(!ReadRawFrame((dir / "bad.nrraw").string(), raw, error));
    for (uint32_t bits = 0; bits < 65536; bits++) {  // half <-> float round trip of every finite value
        if ((bits & 0x7c00u) == 0x7c00u)  // infinity and NaN (std::isfinite is not reliable under fast math)
            continue;
        CHECK_EQ((int)FloatToHalf(HalfToFloat((uint16_t)bits)), (int)bits);
    }
    fs::remove_all(dir);
}

void TestCurve() {
    // Computed by the comparison tools' Python version of the curve, per channel triple (and the heat map)
    struct Case {
        float in[3], out[3];
    } cases[] = {{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
                 {{0.5f, 0.5f, 0.5f}, {0.5947504f, 0.5947317f, 0.5947131f}},
                 {{0.58f, 0.58f, 0.58f}, {0.7342872f, 0.7342706f, 0.7342542f}},
                 {{0.75f, 0.4f, 0.2f}, {0.9491766f, 0.0f, 0.0f}},
                 {{1.0f, 1.0f, 1.0f}, {0.9912035f, 0.9912027f, 0.9912018f}},
                 {{0.2f, 0.6f, 0.9f}, {0.0f, 0.7646040f, 0.9801160f}},
                 {{100.0f / 1023.0f, 300.0f / 1023.0f, 700.0f / 1023.0f}, {0.0f, 0.1843359f, 0.8793080f}}};
    for (const Case& c : cases) {
        const Rgb out = SdrFromPq(c.in[0], c.in[1], c.in[2]);
        CHECK_NEAR(out.r, c.out[0], 3e-4);
        CHECK_NEAR(out.g, c.out[1], 3e-4);
        CHECK_NEAR(out.b, c.out[2], 3e-4);
    }
    CHECK_NEAR(PqToNits(0.5f), 92.24368, 0.01);
    const Rgb white = SdrFromScrgb(1.0f, 1.0f, 1.0f);
    CHECK_NEAR(white.r, 0.5682021, 2e-4);
    const Rgb s2 = SdrFromScrgb(0.5f, 2.0f, 0.1f);
    CHECK_NEAR(s2.r, 0.4424791, 2e-4);
    CHECK_NEAR(s2.g, 0.6949081, 2e-4);
    CHECK_NEAR(s2.b, 0.2148292, 2e-4);
    const float errors[] = {0.0f, 10.0f, 64.0f, 128.0f, 300.0f};
    const float heat[][3] = {{0.0f, 0.0f, 0.0156863f}, {0.0686945f, 0.0392540f, 0.1727024f}, {0.7105400f, 0.2121634f, 0.4780940f},
                             {0.9882353f, 0.9921569f, 0.7490196f}, {0.9882353f, 0.9921569f, 0.7490196f}};
    for (int i = 0; i < 5; i++) {
        const Rgb m = Magma(errors[i]);
        CHECK_NEAR(m.r, heat[i][0], 2e-5);
        CHECK_NEAR(m.g, heat[i][1], 2e-5);
        CHECK_NEAR(m.b, heat[i][2], 2e-5);
    }
}

void TestPng16() {
    // 3 x 2 pixels of 16-bit values (little-endian in memory), with the cICP chunk of BT.2020 PQ full range
    const uint16_t rgb[3 * 2 * 3] = {0, 65535, 32768, 1, 2, 3, 4096, 8192, 12288, 65535, 0, 65535, 40000, 50000, 60000, 7, 8, 9};
    const std::vector<uint8_t> bytes = EncodePng(3, 2, 3, 16, (const uint8_t*)rgb, 3 * 6, true);
    CHECK(bytes.size() > 40 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(std::memcmp(bytes.data() + 12, "IHDR", 4) == 0);
    CHECK_EQ((int)bytes[24], 16);  // bit depth
    CHECK_EQ((int)bytes[25], 2);   // colour type: RGB
    CHECK(std::memcmp(bytes.data() + 33 + 4, "cICP", 4) == 0);  // right after IHDR (8 + 25 bytes)
    CHECK(bytes[41] == 9 && bytes[42] == 16 && bytes[43] == 0 && bytes[44] == 1);
    int w = 0, h = 0, channels = 0;
    stbi_us* pixels = stbi_load_16_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &channels, 3);
    CHECK(pixels != nullptr && w == 3 && h == 2);
    if (pixels) {
        for (int i = 0; i < 18; i++)
            CHECK_EQ((int)pixels[i], (int)rgb[i]);  // lossless: every code comes back
        stbi_image_free(pixels);
    }
    const std::vector<uint8_t> plain = EncodePng(3, 2, 3, 16, (const uint8_t*)rgb, 3 * 6, false);
    CHECK(plain.size() < bytes.size() && std::memcmp(plain.data() + 33 + 4, "cICP", 4) != 0);
}

// What Steam games hold: capture folders and provider DLLs under OptiScaler in the game's folder (any provider name) and
// beside an Unreal Engine game's executable (<game>/<project>/Binaries/Win64), the games' own nvngx_dlssg.dll, not
// deeper than 4 levels.
void TestSteamFolders() {
    namespace fs = std::filesystem;
    const fs::path home = fs::temp_directory_path() / ("captures_test_home_" + std::to_string(::getpid()));
    fs::remove_all(home);
    const fs::path common = home / ".local" / "share" / "Steam" / "steamapps" / "common";
    const fs::path root = common / "Root Game/OptiScaler/custom/Apex Neural Rendering FG/dlssg";
    const fs::path unreal = common / "Hogwarts Legacy/Phoenix/Binaries/Win64/OptiScaler/custom/ANRFG/dlssg";
    const fs::path deep = common / "Deep Game/a/b/c/d/OptiScaler/custom/x/dlssg";
    for (const fs::path& p : {root, unreal, deep})
        fs::create_directories(p / "captures");
    WriteFile((root / "nvngx_dlssg.dll").string(), "MZ", 2);
    WriteFile((common / "Root Game" / "nvngx_dlssg.dll").string(), "MZ", 2);
    const char* old = std::getenv("HOME");
    const std::string saved = old ? old : "";
    setenv("HOME", home.c_str(), 1);
    const SteamFiles steam = ScanSteam();
    if (old)
        setenv("HOME", saved.c_str(), 1);
    auto found = [&](const fs::path& p) {
        return std::find(steam.capture_folders.begin(), steam.capture_folders.end(), p.string()) != steam.capture_folders.end();
    };
    CHECK(found(root / "captures"));
    CHECK(found(unreal / "captures"));
    CHECK(!found(deep / "captures"));
    CHECK_EQ((int)steam.capture_folders.size(), 2);
    CHECK_EQ((int)steam.dlls.size(), 2);
    int providers = 0, nvidia = 0;
    for (size_t i = 0; i < steam.dlls.size(); i++) {
        providers += steam.dlls[i].kind == "provider" && steam.dlls[i].path == (root / "nvngx_dlssg.dll").string();
        nvidia += steam.dlls[i].kind == "nvidia" && steam.dlls[i].path == (common / "Root Game" / "nvngx_dlssg.dll").string();
        CHECK(steam.dll_games[i] == "Root Game");
    }
    CHECK(providers == 1 && nvidia == 1);
    Locations where;
    State state;
    state.dlls.push_back({"provider", (root / "nvngx_dlssg.dll").string()});  // added by hand too: listed once
    setenv("NR_REPLAY", "", 1);
    const std::vector<Generator> generators = ReplayGenerators(where, state, steam, "NVIDIA GeForce", true);
    CHECK_EQ((int)generators.size(), 2);  // one per kind and version
    CHECK(generators.size() == 2 && generators[0].kind == Generator::APEX && generators[0].label == "Apex Neural Rendering FG (added)");
    CHECK(generators.size() == 2 && generators[1].kind == Generator::DLSSG && generators[1].label == "DLSS-G (Root Game)");
    CHECK(generators.size() == 2 && generators[0].replay && !generators[0].available);  // no nr-replay here
    fs::remove_all(home);
}

}  // namespace

int main() {
    TestSteamFolders();
    TestPairs();
    TestSources();
    TestReplay();
    TestReader();
    TestCurve();
    TestPng16();
    return check_result("captures_test");
}
