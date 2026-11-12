// The native export: the composite of the two recordings (divider or side by side, labels, divider line, the shown area's
// caption and zoom, the camera of the marked areas, the logos) rendered offscreen at the export size with the same video
// pass and overlay code as the window, read back and piped as raw frames to an ffmpeg encoder. Frame pairing is the
// playback's: A frame n with B frame n - K; the edit list (edit_list.h) says which pair and shot each output frame
// shows, with the freeze marks' sequences inserted. No Python. In the viewer (Prepare and Record once per UI frame) and from the command
// line (RunExportCli: --compare-export PROJECT --out FILE, headless).

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "renderer/clips/gfx.h"
#include "renderer/clips/project.h"

namespace renderer::clips {

struct ExportSettings {
    std::string output;
    int height = 0;            // 0: the recordings' height
    int resample = 0;          // 0 auto (box below a scale of 1, nearest from a zoom of 2, else bicubic), 1 nearest, 2 bilinear, 3 bicubic, 4 box
    int codec = 0;             // 0 H.265 (hevc), 1 AV1
    int hardware = 1;          // 1 a hardware encoder where one works (encoders.h), 0 software (libx265, libsvtav1)
    int rate = 0;              // RateControl::Mode: 0 quality (crf), 1 bitrate (mbps), 2 very high, 3 lossless (H.265)
    int crf = 14;
    double mbps = 80.0;        // the bitrate mode's target in Mbit/s
    int audio = 0;             // 0 none, 1 A's, 2 B's
    double start = -1.0, end = -1.0;  // seconds on A's timeline; negative: the overlap's start and end
    std::string layout;        // "" the project's, "slider" or "side-by-side"
    uint64_t cache_mib = 768;  // decoded frame cache per recording
    double freeze_hold = -1.0; // seconds a freeze mark waits at its zoom; negative: the project's
    double ease = -1.0;        // seconds of zoom in and out; negative: the project's
    int logos = -1;            // -1 the project's (on when the logo files exist), 0 off, 1 on
    // >= 0: a still image instead of a video, the pair at this time (seconds on A's timeline) as a lossless 8-bit PNG
    // in `output`: no freeze marks; side by side is twice as wide, so each recording shows whole at the export height
    double image_at = -1.0;
};

class Exporter {
public:
    ~Exporter();

    // Starts an export of `project`: opens the recordings (their own decoders), the offscreen target and the encoder.
    // Needs an ImGui context with a font (the overlay's text). False: why not.
    static std::unique_ptr<Exporter> Start(Gfx& gfx, const Project& project, const ExportSettings& settings,
                                           std::string& error);

    // One export frame, in two steps each UI frame: Prepare inside the ImGui frame (it builds the overlay with the frame's
    // text while the font atlas is current), Record after ImGui::Render (the GPU commands and the readback). Cheap when
    // the frames or a free slot are not there yet.
    void Prepare();
    void Record();

    bool running() const { return running_; }
    double progress() const { return total_ > 0 ? (double)done_ / (double)total_ : 0.0; }
    std::string status() const;
    std::string error() const;
    bool finished_ok() const { return finished_ && !cancelled_ && error().empty(); }
    void Cancel();
    // Headless: runs to the end (an ImGui frame per export frame). True when the file was written.
    bool Run();

    // Output frames: the source range's plus what the freeze marks insert.
    int64_t frames_done() const { return done_; }
    int64_t frames_total() const { return total_; }
    // The video encoder that runs ("hevc_nvenc (hardware)") and its rate ("very high (199 Mbit/s)"); empty for an image.
    std::string encoder() const;
    std::string rate() const;

private:
    Exporter();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    bool finished_ = false;
    std::atomic<bool> cancelled_{false};
    std::atomic<int64_t> done_{0};
    int64_t total_ = 0;
};

// `--compare-export PROJECT --out FILE [--height N] [--start S --end E] [--codec h265|av1] [--crf N | --bitrate MBPS |
// --very-high | --lossless]
// [--audio none|a|b] [--scaling auto|nearest|bilinear|bicubic|box] [--layout slider|side-by-side] [--cache-mib N]
// [--freeze-hold SECONDS] [--ease SECONDS] [--logos|--no-logos] [--at SECONDS]`: headless (no window). With --at, a
// still image of that time (--out FILE.png) instead of a video. argv as main's. Prints one JSON line and returns the
// exit code.
int RunExportCli(int argc, char** argv);

}  // namespace renderer::clips
