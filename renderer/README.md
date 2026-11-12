# Renderer: compare videos and browse captures

The renderer compares frame generation results. It is one binary with these modes:
- **Compare videos**: two screen recordings in lockstep, exact pixel zoom, marks, video and image exports
- **Browse captures**: nr-record recordings ([record/README.md](../record/README.md)); recreate their generated frames with NVIDIA's DLSS-G or a DLSS-G provider such as ANR FG through `nr-replay` ([record/replay/README.md](../record/replay/README.md)), and compare any two frames of a pair

```bash
bazel run //renderer                  # the launcher: a card per mode; click, Enter or 1, 2; Esc quits
bazel run //renderer -- --compare     # compare videos
bazel run //renderer -- --captures    # captures
bazel run //renderer -- --launcher    # the launcher, ignoring other arguments
```

It builds with Bazel on Linux and for Windows (Vulkan), and on macOS (Metal 4; not built in this tree yet). This build has no scene renderer and no study viewer: `--mode scene`, `--study` and other options of theirs say so.

## Modes

| Mode | Starts with |
| --- | --- |
| Compare videos ([Compare mode](#compare-mode)) | `--compare` or `--mode compare` |
| Browse captures ([Captures](#captures)) | `--captures` or `--mode captures` |

- **Launcher.** It loads no decoder and no capture. Each card shows whether its needs exist: amber when the mode runs with less (no ffmpeg and ffprobe; no recordings yet; no `nr-replay`).
- **Command line.** No arguments opens the launcher. `--mode`, `--compare`, `--captures`, `--compare-check` and `--compare-export` pick a mode. Contradicting mode flags are an error.
- **Mode menu** (first menu of every mode). A switch restarts the program as the chosen mode, with only its mode flag (`exec` on Linux and macOS, a new process on Windows). A confirmation appears only when a video export, a capture job or an image save would be lost.
- `--script` takes `FRAME:ACTION` pairs for checks. The launcher takes `select=compare|captures`, `focus=N`, `capture=FILE.png`, `log` and `quit`.

**Files.** The renderer keeps its state in `$XDG_CACHE_HOME/nr-record-viewer` (else `~/.cache/nr-record-viewer`) on Linux and macOS and in `%LOCALAPPDATA%\nr-record-viewer` on Windows: the last mode, the window layouts, the compare tool's recent pairs and projects (`compare/`), and the Captures tab's thumbnails, added folders and DLLs (`captures/state.json`), job frames (`captures/frames/`), marks (`captures/projects/`), job logs and saved images.

## Compare mode

`renderer --compare` plays two screen recordings of the same run in lockstep. It offers a divider, side by side, A only or B only, exact pixel zoom, marks that zoom to an area, and an export with optional logos. Its second tab is [Captures](#captures).

```bash
bazel run //renderer -- --compare                      # then choose A and B in the window
bazel run //renderer -- --compare --project cmp.compare.json
bazel run //renderer -- --compare --compare-check 600 --project cmp.compare.json
bazel run //renderer -- --compare --compare-export cmp.compare.json --out /tmp/cmp.mp4 --height 1080
```

`nr-video project` writes a project for two replays' videos ([record/replay/README.md](../record/replay/README.md)). No video path goes on a command line: choose the recordings in the window, or open a project with `--project FILE` (for scripts, tests and the headless modes). Relative paths resolve against where you ran the command.

FFmpeg and FFprobe are needed for sync detection, for the export, and for decoding where the GPU cannot. They are needed only after you choose a video. Put them on `PATH` or beside the executable, or name them with `NR_FFMPEG` and `NR_FFPROBE`.

**Choosing the recordings.** The start screen has two cards, **A** (left of the divider) and **B** (right). Each has **Choose video...** (SDL's file dialog, or a path field where the system has none) and a drop target. Below them are **Open project...** and **Recent**.
- One video dropped on the left half sets A, on the right half B. Two videos set A and B. A `.json` opens that project.
- The old pair stays, and a red banner names both files, when a file is missing, both are the same file, or the sizes or frame rates differ (to 0.001 fps).
- **Change...** replaces one side and looks up that pair's own project. **Swap A and B** swaps files, labels and decoders without decoding again; the offset changes sign, and marks and logos follow their moments.
- A pair's project comes from the recent-pairs registry (`compare-recent.json` in the state folder). Otherwise the renderer creates it in `compare/` there, with a start-sync detection in a worker thread. Edits autosave. **Save project as...** writes a copy with full paths.

**Lockstep.** Frame n of A shows the same moment as frame n - K(n) of B. K is one constant (`offset_frames`) or follows the project's offset map.
- One master clock moves the position. A pair goes on screen only when both frames are decoded and uploaded (`renderer/clips/lockstep.h`), so a mismatched pair never shows.
- A late frame holds the pair ("buffering"). If the decoders cannot keep real time, the rate drops in steps ("decode-limited 0.80x") and recovers.
- Where K(n) rises by one, B shows a frame twice. Where it falls, B skips one.
- Each recording caches decoded frames around the playhead in video memory (`--cache-mib`, 1536 per recording).
- `OpenVideo` (`renderer/clips/video_source.h`) takes the [GPU decoder](#gpu-video-decode) first and falls back to an FFmpeg pipe (`ffmpeg ... -f rawvideo -pix_fmt nv12 -`). `--decoder ffmpeg` skips the GPU one.
- From a zoom of 1, nearest source pixels snap to whole screen pixels. At an integer scale every source pixel is an exact block (`view_math.h`). No dithering or filtering touches the pixels.

| Key | Action |
| --- | --- |
| Space | play or pause |
| Left, Right | one frame back or forward (Shift: one second); pauses |
| `,` `.` | offset: B one frame earlier or later (Shift: 10); with the drift followed, at the playhead |
| 1, 2, 3, 4, Tab | divider, side by side, A only, B only; Tab switches A and B (single views) |
| `[` `]`, left drag | move the divider by 16 pixels; drag it (divider view) or pan (other views) |
| wheel, `+` `-`, pinch, 0 | zoom around the pointer (1x to 16x), back to the whole frame; right or middle drag pans |
| R | mark an area: drag a rectangle, made on release at the frame on screen (Esc cancels) |
| Z, Delete | preview the selected freeze mark (else toggle the span zoom preview); delete the selected area |
| Home, End, F1, F11, Esc | start and end of the overlap, hide the UI, fullscreen, cancel marking or quit |

The panels are **Clips** (files, labels, offset, **Follow drift** and anchors, the plot of K, sync detection, views), **Areas**, **Export** and **Display**. **Transport** runs along the bottom (play, step, speed 0.25x to 2x, a timeline with marks, status).

**Sync detection and drift.** Recordings started by hand are offset by K frames and drift. The tracker (`offset_estimate.h`) decodes each recording once to small grey features, masks cells that are static in either recording (letterbox, HUD, overlay text), tracks K in 10 s windows and fits a Theil-Sen slope, with jumps where residuals step by more than 1.5 frames. A drift over 1 frame across the overlap, or any jump, makes the map stored and followed; otherwise the project keeps one constant. Export audio follows A (or B from its first-pair frame).

**Areas.** A mark is a rectangle with a label, a divider that stays, sweeps or is fixed, and the camera that zooms to it. Press R and drag in any view to make one; the Areas panel edits it. Defaults: `freeze_hold` 0.5 s, `ease` 0.6 s, `hold` 3 s, and 2 swings of a sweeping divider over the mark (`sweeps`; 1 to 8).
- A **freeze** mark (the default) stops the video at one frame while the camera zooms to the rectangle, holds and zooms back, then the video plays on. The output is longer than the source range.
- A **span** mark zooms while the video plays: in over `ease` seconds before `t0`, hold until `t1`, out over `ease` seconds. Spans zoom only in the preview (Z), not in plain playing.
- **Zoom marks while playing** (on by default) runs freeze marks in the player. **Show marks** (near the playhead, always, never) is a window setting. The export draws no rectangles.

**Export.** Each output frame renders offscreen with the window's video pass and overlay code, and pipes to `ffmpeg`. The export decodes both recordings as playback does, on the GPU where it can; the log line `export: A decodes with` says which.
- Codecs: H.265 (default) or AV1, with a hardware encoder where one works (NVENC, VideoToolbox, QSV, VAAPI, AMF); the log line `export: encoder` says which. Otherwise libx265 or libsvtav1. `--software` forces CPU encoders; CRF 0 with libx265 is lossless.
- Rate (the Export panel's **rate**, or one of these flags):
  - constant quality, `--crf N` (default 14, libx265's scale; NVENC gets VBR with `-cq` at N + 11 for H.265 and N + 12 for AV1)
  - a target bitrate, `--bitrate MBPS`: variable bitrate that peaks at 1.5 times it, with a 2-second buffer
  - very high, `--very-high`: 0.40 bits per pixel and frame at 60 fps for H.265 and 0.30 for AV1, times the square root of fps / 60 (199 and 149 Mbit/s at 3840x2160 and 60 fps; 384 and 288 at 223 fps), with the encoder's slowest preset (NVENC p7 with a full-resolution second pass, 32 frames of lookahead, spatial and temporal adaptive quantization; H.265 in the high tier)
  - lossless, `--lossless` (H.265 only): NVENC's lossless tuning (p7), else libx265's lossless mode. It is lossless for the 4:2:0 frames every mode encodes.
- The headless JSON line and the log line `export: encoder` name the encoder and the rate.
- Measured (RTX 5090, a TLOU2 4X generated-only comparison at 3840x2160 and 223 fps, the first 10 s, 3,613 output frames): `--very-high` H.265 hevc_nvenc at 384 Mbit/s, 769 MB, 37.9 fps of export; AV1 av1_nvenc at 288 Mbit/s, 611 MB, 70.6 fps; `--lossless` hevc_nvenc, 1,702 Mbit/s (first 2 s: 1,829 frames, 1.74 GB, 90.8 fps).
- Scaling is `auto` (box below 1, nearest from a zoom of 2, else bicubic) or one named filter.
- Audio from A or B is cut where each sequence begins, and silence goes there.
- Headless, it prints one JSON line (frames, seconds, fps, bytes). It needs a GPU but no window.

```bash
renderer --compare --compare-export PROJECT --out FILE [--height N] [--start S --end E] [--codec h265|av1] [--software]
         [--crf N | --bitrate MBPS | --very-high | --lossless]
         [--audio none|a|b] [--scaling auto|nearest|bilinear|bicubic|box] [--layout slider|side-by-side] [--cache-mib N]
         [--freeze-hold SECONDS] [--ease SECONDS] [--logos | --no-logos]
renderer --compare --compare-export PROJECT --at SECONDS --out FILE.png [--layout slider|side-by-side] [--height N] ...
```

**Images.** **Export images of this frame** in the Export panel writes the pair at the playhead as two lossless 8-bit PNGs beside the video's file (`<video's name>_f<frame>_slider.png` and `_side-by-side.png`), at the export height with the labels and logos. `--at SECONDS` does the same from the command line, one layout per run.

**Logos.** The export can put the Apex logo bottom-left and NVIDIA's bottom-right (A, left of the divider, is the Apex side; a project whose A label names DLSS or NVIDIA and whose B label does not gets the corners swapped). The files are `apex.png` and `nvidia.png`, found in `NR_LOGO_DIR`, then the state folder's `logos/`, then the project's folder; they are not part of this tree. The **Logos** box in the Export panel is the master switch; `--logos` and `--no-logos` decide for one export; unset, logos are on when a logo file exists.

**Project file.** `.compare.json` holds `version`, `title`, `a.path`, `b.path`, `labels`, `layout`, `divider`, `ease`, `hold`, `areas`, `offset_frames`, `fps`, `size` and `sync`; optional fields are `offset_map`, `follow_drift`, `freeze_hold`, `new_mark_kind`, `logos` and an area's `sweeps`. Unknown fields survive a save.

**Check and options.** `--compare --compare-check N --project FILE` plays N pairs with the lockstep clock headless and prints one JSON line (`pairs_shown`, `pairs_held`, `mismatched`, `stalls`, `rate_min`, `decode_fps_a`, `decode_fps_b`); it exits 0 when no pair was mismatched. `--compare-window` runs it in the window.

| Option | Default | Meaning |
| --- | --- | --- |
| `--project FILE` | none (empty start) | open this project; headless modes need it |
| `--cache-mib N`, `--decoder auto\|ffmpeg` | 1536, auto | decoded frames kept per recording; GPU decode where it can, or FFmpeg |
| `--view V`, `--start-frame N`, `--play` | the project's, the overlap's start, paused | slider, side-by-side, a or b; where to start |
| `--no-vsync`, `--swapchain M`, `--paper-white N`, `--peak N`, `--no-hdr-metadata` | vsync on, sdr | swap chain (sdr, hdr10, scrgb) and HDR metadata |
| `--capture FILE [--capture-frame N]`, `--tab NAME` | | write one frame's window pixels (FILE.bin and FILE.json), then quit; the panel tab in front |
| `--script LIST` | | `FRAME:ACTION` pairs; the actions are in `renderer/clips/clip_viewer.cpp` |

```bash
bazel test //renderer/clips/...   # the pure parts: view math, edit list, mark runner, lockstep, offset map and estimate, project, registry, logos, captures
```

Limits: recordings need a constant frame rate and the same size and rate; audio is ignored in playback; a gradual change of rate reads to the tracker as a jump.

## Captures

The compare start screen's second tab, **Captures**, browses nr-record recordings of frame generation: the capture DLL's F6 recordings and its F9 8-frame captures ([record/README.md](../record/README.md)). You can:
- look at any two images of a pair as A and B with the video views
- recreate the pair's generated frame with NVIDIA's DLSS-G or a DLSS-G provider (ANR FG) through `nr-replay`
- save the composite as an image with labels and logos

```bash
bazel run //renderer -- --captures
```

No capture path goes on the command line. The browser lists the recordings folder (`$XDG_DATA_HOME/nr-record`, else `~/.local/share/nr-record`, on Linux and under Proton; `%LOCALAPPDATA%\nr-record` on Windows), the `OptiScaler/custom/<provider>/dlssg/captures` folders of each installed Steam game (in the game's folder or up to 4 levels below it), and folders you add with **Add a folder...**. A folder is a recording when it has `record.json`; the reader (`//record:reader`) decodes a frame's textures on the CPU, bit for bit, and opening a recording decodes only the frames in A and B.

- **Capture** picks the pair. A *real pair* has real frames k - 1 and k and one generated slot between them (t = index / multiplier). A *skip-one* generates real frame k from k - 1 and k + 1 at t = 1/2 and compares it with the recorded frame k, an approximate reference with unverified timing. No pair spans frames the recorder dropped.
- **Sources** lists the pair's images: real frames, "Rendered (approx.)", every generator's cached frame, and false colour of depth and motion. Any two can be A and B. A pair with finished jobs opens with the provider's frame in A and DLSS-G's in B.
- **The picture** has the video views and a fifth, **difference** (5): the largest channel's |A - B| in code units times 8, in the magma map. **Marks** (R, then drag) are kept per capture and pair.
- **Generators** lists DLLs for `nr-replay`: providers' `nvngx_dlssg.dll` under `OptiScaler/custom/<provider>/dlssg/` and games' own `nvngx_dlssg.dll` (NVIDIA's) in Steam games, and DLLs you add (**Add NVIDIA's DLSS-G DLL...**, **Add a provider DLL...**), one per kind and file version.
  - Each runs for the selected pair or the whole recording as a background job, one at a time (Jobs panel: log tail, progress, Cancel, Retry). Skip-ones are not replayed.
  - A job waits while a Steam game runs. Nothing is stopped.

| Generator | The job runs | Needs |
| --- | --- | --- |
| NVIDIA's DLSS-G | `nr-replay --record REC --generator nvidia --dll DLL --out DIR --raw [--frames A:B]` | an NVIDIA GPU and driver |
| A provider (ANR FG) | the same with `--generator provider` | the provider's folder (its DLL and files) |

On Windows the job runs `nr-replay.exe`; on Linux it runs `python3 -I nr_replay_wine.py` with the same options, which runs it under Wine ([record/replay/README.md](../record/replay/README.md) "Linux"). `NR_REPLAY` names either; else the renderer looks beside its executable, then in the workspace (`record/replay/nr_replay_wine.py`, `bazel-bin/record/replay/nr-replay.exe`). A pair's job replays real frames a to b (`--frames a:b+1`): the replay starts without history at a, and b's calls make the frames between them. The frames (`generated_f<a>-f<b>_t<t>.nrraw`, and `replay.json`) cache in `captures/frames/<capture>/<generator>-<version>-<tool revision>/` in the state folder; frames of an older tool or DLL show as stale.

**Colour and save.** An HDR10 swap chain shows PQ codes unchanged (sRGB sources at the paper white). An SDR swap chain, the thumbnails and the SDR image use the comparison curve (PQ to nits, BT.2020 to BT.709, 203 nits = 1.0, Reinhard, sRGB). The Save panel writes at 1x, 1080p, half size or a custom height:
- **Save image...** writes an 8-bit SDR PNG; for HDR sources also `<name>.hdr.png` (16-bit BT.2020 PQ) and `<name>.hdr.avif` (through ffmpeg).
- **Save marked crops...** writes the marked crops. **Save all...** writes the image, the crops and a `manifest.json`.

`--script` actions of the tab start with `cap-` (`cap-open=NAME`, `cap-pair=real:5:1|skip:5`, `cap-run=ID|apex|dlss-g[:all]`, `cap-wait=jobs`, `cap-view=...`, `cap-save=FILE`, `cap-log`, `cap-quit` and more; `renderer/clips/captures/studio.h`).

Limits: depth and motion show as false colour at their own grid; images of different sizes in A and B are sampled by ratio, not exactly.

## GPU video decode

`renderer/clips/video/` decodes long recordings on the GPU's video decode queue through NRI's video extension (`NriVideo.h`), with its own MP4 demuxer (`mp4.cpp`) and its own AV1, H.264 and H.265 parsers and reference management (`av1.cpp`, `backend_*.cpp`); it uses no decoding library. `NriVideoSource` implements `VideoSource` (`renderer/clips/video_source.h`). `Open` returns null with a reason when the file or GPU cannot be used, and the caller falls back to ffmpeg.

```cpp
renderer::clips::NriVideoOptions options;   // memory_bytes 1.5 GiB, in_flight 8
auto source = renderer::clips::NriVideoSource::OpenWithOptions(context, path, options, &why_not);   // or Open(context, path, &why_not)
source->SetInterest(first, last, playhead, direction);   // every frame, from the render thread
if (source->Acquire(frame, ref)) { /* sample ref.luma, ref.chroma */ source->Release(ref); }
```

The device needs a video decode queue family: `nr::VulkanDevice` (`device/vulkan.h`) creates up to two with `video_decode = true`, so two recordings decode at once on GPUs with two decoders. A frame is two textures: luma (`R8_UNORM`) and chroma (`RG8_UNORM`); at 10 bits `R16_UNORM` and `RG16_UNORM` hold the code shifted left by 6. A frame's pixels change only after `Release`. The pool keeps frames within `memory_bytes`, allocated once at `Open`, and decodes nearest the playhead first, then ahead.

| | Supported |
| --- | --- |
| Container | MP4 and MOV (`av01`, `avc1`, `avc3`, `hvc1`, `hev1`), constant frame rate |
| AV1 | Main, 4:2:0, 8 and 10 bit; hidden frames, `show_existing_frame`, intrabc, tiles, segmentation, global motion |
| H.264 | Baseline, Main, High, progressive, 4:2:0 8 bit; B-frames, weighted prediction, long-term pictures, several slices |
| H.265 | Main and Main 10, 4:2:0, one layer; B-frames, long-term pictures, tiles, slice segments; closed GOPs |
| API | Vulkan on Linux and Windows (Metal: no video decode in NRI here, `Open` says so) |
| Refused, with a reason | AV1 film grain, super-resolution, 4:4:4; H.264 interlaced, above 8 bit; H.265 4:4:4, open GOPs; fragmented MP4, variable frame rate, other containers; sizes below the driver's minimum |

```bash
bazel run //renderer/clips:decode_check -- FILE [N] [--seeks K] [--playback S] [--memory-mb M] [--throughput T] [--twin] [--stress S]
bazel test //renderer/clips/video:demux_test       # sample tables, AV1 frame headers against ffmpeg's trace, H.264 and H.265 parameter sets; no GPU
```

`decode_check` compares the first N frames (0: all) with `ffmpeg -pix_fmt nv12` byte for byte, then measures the decode rate, K random seeks (also compared), S seconds of playback both ways, `--twin` and `--stress S`. It prints one JSON line and exits 0 when exact, 1 on a difference or decode error, and 3 when the file cannot be decoded here. `NR_NRI_VALIDATION=1` turns on NRI's checks. `renderer/clips/testdata/generate.sh` makes the test clips.

## Build

The renderer builds with Bazel from pinned sources (`MODULE.bazel`): NRI, NRIFramework, SDL3 (with libdecor support patched in), ImGui, spdlog and the Vulkan headers and loader.

| | Linux | Windows | macOS |
| --- | --- | --- | --- |
| Graphics API | Vulkan | Vulkan | Metal 4 (NRI's Metal backend; macOS 26, Apple silicon) |
| Compiler | Clang from `toolchains_llvm` | clang-cl from an LLVM install (`BAZEL_LLVM`), or cross-compiled on Linux | Clang from `toolchains_llvm`, Xcode's SDK |
| Shaders | SPIR-V | SPIR-V | DXIL, converted to Metal by NRI with Apple's Metal Shader Converter |

```bash
bazel run //renderer -- --compare
bazel build --config=windows_cross //renderer   # a Windows build from Linux (tools/windows_cross/README.md)
```

- **Linux:** the binary needs only glibc. The C++ runtime is static, and the Vulkan loader runs from the runfiles. The GPU driver and the Wayland, xkbcommon, libdecor and X11 client libraries come from the system. The window is native Wayland in a Wayland session (`--x11` asks for X11).
- **Windows:** Visual Studio 2022 or its Build Tools (C++ workload), LLVM (`BAZEL_LLVM`) and a Vulkan driver. Bazel copies the runfiles (`--enable_runfiles`).
- **macOS:** macOS 26, Xcode 26 with its Metal toolchain, and Apple's Metal Shader Converter in `/usr/local`. DXC builds from its pinned source. Without the converter, the build succeeds but the renderer cannot load its shaders and says so.
