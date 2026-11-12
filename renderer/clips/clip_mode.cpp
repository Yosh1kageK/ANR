#include "renderer/clips/clip_mode.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include "renderer/clips/clip_viewer.h"
#include "renderer/clips/exporter.h"
#include "renderer/clips/ffmpeg_source.h"
#include "renderer/clips/gfx.h"
#include "renderer/clips/player.h"
#include "renderer/clips/probe.h"
#include "renderer/clips/project.h"
#include "renderer/clips/study_entry.h"
#include "renderer/display.h"
#include "device/log.h"
#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace fs = std::filesystem;
namespace {

bool Has(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], flag) == 0)
            return true;
    return false;
}

// The value after `flag` (empty when absent).
std::string Value(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    return {};
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The value after `flag` unless it is another option (so that `--compare-export --project FILE` works too).
std::string ValueUnlessOption(int argc, char** argv, const char* flag) {
    const std::string value = Value(argc, argv, flag);
    return value.rfind("--", 0) == 0 ? std::string() : value;
}

// A command line the compare modes refuse: a video path (the recordings are chosen in the window, or by a project).
bool HasVideoPaths(int argc, char** argv) {
    static const char* with_value[] = {"--project", "--compare-check", "--compare-export", "--out", "--cache-mib", "--decoder",
                                       "--start-frame", "--log-level", "--offset", "--rate", "--height", "--start", "--end",
                                       "--codec", "--crf", "--bitrate", "--at", "--audio", "--scaling", "--layout", "--freeze-hold", "--ease",
                                       "--view", "--tab", "--script", "--capture", "--capture-frame", "--swapchain",
                                       "--paper-white", "--peak", "--width", "--timeLimit", "--frameNum", "--scene", "--mode"};
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            for (const char* option : with_value)
                if (arg == option && i + 1 < argc)
                    i++;
            continue;
        }
        return true;
    }
    return false;
}

constexpr const char* NO_PATHS =
    "the compare modes take no video paths: choose the recordings in the window (bazel run //renderer -- --compare), or give a "
    "project that holds them with --project FILE";

// --compare --compare-check N --project FILE: plays N pairs headless (no window; a device for the frame cache only) at the
// recordings' own rate, with the lockstep clock, and prints one JSON line.
int RunCompareCheck(int argc, char** argv) {
    nr::InitLogging(/*to_stderr=*/true);
    const int pairs = std::atoi(Value(argc, argv, "--compare-check").c_str());
    std::string a_path, b_path, project_path = UserPath(Value(argc, argv, "--project"));
    if (HasVideoPaths(argc, argv)) {
        std::fprintf(stderr, "%s\n", NO_PATHS);
        return 1;
    }
    OffsetMap offset;  // K(n): the project's (its map when it follows one), or --offset's constant
    bool have_offset = false;
    Project project;
    std::string error;
    if (!project_path.empty() && fs::exists(project_path)) {
        if (!LoadProject(project_path, project, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        a_path = project.ResolvedA();
        b_path = project.ResolvedB();
        offset = project.Offsets();
        have_offset = true;
    }
    if (!Value(argc, argv, "--offset").empty()) {
        offset = OffsetMap((int64_t)std::atoll(Value(argc, argv, "--offset").c_str()));
        have_offset = true;
    }
    if (a_path.empty() || b_path.empty() || pairs <= 0) {
        std::fprintf(stderr, "usage: --compare --compare-check N --project FILE [--offset FRAMES] [--cache-mib N] "
                             "[--decoder ffmpeg] [--start-frame N] [--rate X]\n");
        return 1;
    }

    FfmpegOptions ffmpeg;
    if (!Value(argc, argv, "--cache-mib").empty())
        ffmpeg.memory_bytes = (uint64_t)std::max(std::atoi(Value(argc, argv, "--cache-mib").c_str()), 64) << 20;
    SetVideoOptions(ffmpeg, Value(argc, argv, "--decoder") == "ffmpeg");
    Gfx gfx;
    if (!gfx.Create(/*window=*/false, error, /*video_decode=*/Value(argc, argv, "--decoder") != "ffmpeg")) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const VideoContext context = {gfx.device, &gfx.NRI, &gfx.NRI, gfx.queue};
    std::unique_ptr<VideoSource> source[2];
    std::string reason[2];
    const std::string* paths[2] = {&a_path, &b_path};
    for (int v = 0; v < 2; v++) {
        source[v] = OpenVideo(context, *paths[v], &reason[v]);
        if (!source[v]) {
            std::fprintf(stderr, "cannot open %s: %s\n", paths[v]->c_str(), reason[v].c_str());
            return 1;
        }
    }
    int code = 1;
    {
        Player player(source[0].get(), source[1].get(), offset);
        if (!Value(argc, argv, "--start-frame").empty())
            player.clock().Seek(std::atoll(Value(argc, argv, "--start-frame").c_str()));
        if (!Value(argc, argv, "--rate").empty())
            player.clock().SetSpeed(std::atof(Value(argc, argv, "--rate").c_str()));
        player.clock().Play();
        const double start = Now();
        const double fps = source[0]->Info().fps;
        const double limit = (double)pairs / fps * 4.0 + 60.0;
        std::shared_ptr<const Pair> held;  // what the display would hold
        bool timeout = false;
        while (player.clock().stats().pairs_shown < (uint64_t)pairs) {
            const Player::Tick tick = player.Update(Now());
            held = tick.pair;
            if (!player.clock().playing())
                break;  // the end of the recordings
            if (Now() - start > limit) {
                timeout = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(2000));  // a 240 Hz display
        }
        const double seconds = Now() - start;
        const LockstepStats& s = player.clock().stats();
        const DecodeStats da = source[0]->Stats(), db = source[1]->Stats();
        std::printf("{\"pairs_shown\": %llu, \"pairs_held\": %llu, \"mismatched\": %llu, \"stalls\": %llu, \"rate_min\": %.3f, "
                    "\"decode_fps_a\": %.1f, \"decode_fps_b\": %.1f, \"seconds\": %.2f, \"pairs_per_second\": %.1f, "
                    "\"decoder_a\": \"%s\", \"decoder_b\": \"%s\", \"cached_a\": %lld, \"cached_b\": %lld, "
                    "\"cached_mib_a\": %.0f, \"cached_mib_b\": %.0f, \"offset_frames\": %lld, \"window\": false%s}\n",
                    (unsigned long long)s.pairs_shown, (unsigned long long)s.pairs_held,
                    (unsigned long long)player.mismatched(), (unsigned long long)s.stalls, s.rate_min, da.decode_fps,
                    db.decode_fps, seconds, seconds > 0.0 ? (double)s.pairs_shown / seconds : 0.0,
                    source[0]->Describe().c_str(), source[1]->Describe().c_str(), (long long)da.cached, (long long)db.cached,
                    (double)da.cached_bytes / 1048576.0, (double)db.cached_bytes / 1048576.0, (long long)offset.first_k(),
                    timeout ? ", \"timeout\": true" : "");
        std::fflush(stdout);
        code = (player.mismatched() == 0 && !timeout) ? 0 : 1;
        held.reset();
    }
    source[0].reset();
    source[1].reset();
    return code;
}

}  // namespace

int RunClipMode(int argc, char** argv) {
    if (Has(argc, argv, "--render-test"))
        return RunRenderTest(argc, argv);
    if (Has(argc, argv, "--study"))
        return RunStudy(argc, argv);
    if (Has(argc, argv, "--compare-export"))
        return RunExportCli(argc, argv);
    if (Has(argc, argv, "--compare-check") && !Has(argc, argv, "--compare-window"))
        return RunCompareCheck(argc, argv);
    if (HasVideoPaths(argc, argv)) {  // before any window: the recordings are chosen in it
        std::fprintf(stderr, "%s\n", NO_PATHS);
        return 1;
    }
    // The window: the clip viewer. (NRIFramework's SAMPLE_MAIN deletes the sample in an atexit handler, after the Vulkan
    // driver's exit handlers; the app is destroyed before main returns, as the renderer's is.)
    ClipViewer* app = new ClipViewer;
    const bool ok = app->Create(argc, argv, "NR clips");
    if (ok)
        app->RenderLoop();
    delete app;
    spdlog::shutdown();
    return ok ? 0 : 1;
}

// --compare --compare-export PROJECT --out FILE ... (the project: the value of --compare-export, or --project FILE)
int RunExportCli(int argc, char** argv) {
    nr::InitLogging(/*to_stderr=*/true);
    if (HasVideoPaths(argc, argv)) {
        std::fprintf(stderr, "%s\n", NO_PATHS);
        return 1;
    }
    std::string project_path = UserPath(ValueUnlessOption(argc, argv, "--compare-export"));
    if (project_path.empty())
        project_path = UserPath(Value(argc, argv, "--project"));
    ExportSettings settings;
    settings.output = UserPath(Value(argc, argv, "--out"));
    if (project_path.empty() || settings.output.empty()) {
        std::fprintf(stderr,
                     "usage: --compare --compare-export PROJECT --out FILE [--height N] [--start S] [--end E] [--codec h265|av1] [--software]\n"
                     "       [--crf N | --bitrate MBPS | --very-high | --lossless]\n"
                     "       [--audio none|a|b] [--scaling auto|nearest|bilinear|bicubic|box] [--layout slider|side-by-side]\n"
                     "       [--cache-mib N] [--freeze-hold SECONDS] [--ease SECONDS] [--logos | --no-logos]\n");
        return 1;
    }
    if (!Value(argc, argv, "--height").empty())
        settings.height = std::atoi(Value(argc, argv, "--height").c_str());
    if (!Value(argc, argv, "--start").empty())
        settings.start = std::atof(Value(argc, argv, "--start").c_str());
    if (!Value(argc, argv, "--end").empty())
        settings.end = std::atof(Value(argc, argv, "--end").c_str());
    if (!Value(argc, argv, "--at").empty())
        settings.image_at = std::max(std::atof(Value(argc, argv, "--at").c_str()), 0.0);
    // The rate: constant quality (--crf, the default), a target bitrate, the very high preset, or lossless (H.265)
    const int rates = (int)!Value(argc, argv, "--crf").empty() + (int)!Value(argc, argv, "--bitrate").empty() +
                      (int)Has(argc, argv, "--very-high") + (int)Has(argc, argv, "--lossless");
    if (rates > 1) {
        std::fprintf(stderr, "--crf, --bitrate, --very-high and --lossless exclude each other\n");
        return 1;
    }
    if (!Value(argc, argv, "--crf").empty())
        settings.crf = std::atoi(Value(argc, argv, "--crf").c_str());
    if (!Value(argc, argv, "--bitrate").empty()) {
        settings.rate = 1;
        settings.mbps = std::atof(Value(argc, argv, "--bitrate").c_str());
    }
    if (Has(argc, argv, "--very-high"))
        settings.rate = 2;
    if (Has(argc, argv, "--lossless"))
        settings.rate = 3;
    if (!Value(argc, argv, "--cache-mib").empty())
        settings.cache_mib = (uint64_t)std::max(std::atoi(Value(argc, argv, "--cache-mib").c_str()), 64);
    if (!Value(argc, argv, "--freeze-hold").empty())
        settings.freeze_hold = std::max(std::atof(Value(argc, argv, "--freeze-hold").c_str()), 0.0);
    if (!Value(argc, argv, "--ease").empty())
        settings.ease = std::max(std::atof(Value(argc, argv, "--ease").c_str()), 0.0);
    if (Has(argc, argv, "--logos") && Has(argc, argv, "--no-logos")) {
        std::fprintf(stderr, "--logos and --no-logos together\n");
        return 1;
    }
    settings.logos = Has(argc, argv, "--no-logos") ? 0 : Has(argc, argv, "--logos") ? 1 : -1;
    const std::string codec = Value(argc, argv, "--codec");
    if (!codec.empty() && codec != "h265" && codec != "hevc" && codec != "av1") {
        std::fprintf(stderr, "--codec %s: use h265 or av1 (H.264 is not offered)\n", codec.c_str());
        return 1;
    }
    settings.codec = codec == "av1" ? 1 : 0;
    settings.hardware = Has(argc, argv, "--software") ? 0 : 1;
    const std::string audio = Value(argc, argv, "--audio");
    settings.audio = audio == "a" ? 1 : audio == "b" ? 2 : 0;
    const std::string scaling = Value(argc, argv, "--scaling");
    settings.resample = scaling == "nearest" ? 1 : scaling == "bilinear" ? 2 : scaling == "bicubic" ? 3 : scaling == "box" ? 4 : 0;
    settings.layout = Value(argc, argv, "--layout");
    Project project;
    std::string error;
    if (!LoadProject(project_path, project, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    // ImGui for the overlay's text, without a window or a backend.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.IniFilename = nullptr;
    ImGuiStyle base;
    ImFont* mono = nullptr;
    renderer::SetupUiStyle(base, mono);
    if (io.Fonts->Fonts.Size == 0)
        io.Fonts->AddFontDefaultVector();
    int code = 1;
    {
        Gfx gfx;
        if (!gfx.Create(/*window=*/false, error, /*video_decode=*/true)) {  // the GPU decoder where it runs
            std::fprintf(stderr, "%s\n", error.c_str());
        } else {
            io.DisplaySize = ImVec2(1920.0f, 1080.0f);
            auto exporter = Exporter::Start(gfx, project, settings, error);
            if (!exporter) {
                std::fprintf(stderr, "export: %s\n", error.c_str());
            } else {
                const double start = Now();
                const bool ok = exporter->Run();
                const double seconds = Now() - start;
                std::error_code ec;
                const uintmax_t bytes = ok ? fs::file_size(settings.output, ec) : 0;
                std::printf("{\"ok\": %s, \"frames\": %lld, \"seconds\": %.2f, \"fps\": %.1f, \"output\": \"%s\", \"bytes\": %llu, "
                            "\"encoder\": \"%s\", \"rate\": \"%s\"%s}\n",
                            ok ? "true" : "false", (long long)exporter->frames_done(), seconds,
                            seconds > 0.0 ? (double)exporter->frames_done() / seconds : 0.0, settings.output.c_str(),
                            (unsigned long long)bytes, exporter->encoder().c_str(), exporter->rate().c_str(),
                            ok ? "" : (", \"error\": \"" + exporter->error() + "\"").c_str());
                std::fflush(stdout);
                code = ok ? 0 : 1;
            }
        }
    }
    ImGui::DestroyContext();
    return code;
}

}  // namespace renderer::clips
