<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/apex-logo-white.svg">
    <img alt="Apex Compute" src="assets/apex-logo-black.svg" width="220">
  </picture>
</p>

# Apex Neural Rendering (ANR)

[Apex Neural Rendering](https://apexcompute.com/neural-rendering) by [Apex Compute](https://apexcompute.com).

- **ANR FG**: frame generation. In beta.
- **ANR SR**: super resolution (upscaling). Beta soon.
- **ANR RR**: ray reconstruction (denoising for ray tracing). Beta soon.
- **Source code**: coming soon.
- **nr-record**: records a game's frame generation inputs and compares ANR FG, NVIDIA DLSS-G and others on the same frames ([below](#record-and-compare-frame-generation)). Its source is in this repository.

## ANR FG Beta

1. Download a package from [Releases](../../releases):
   - `anr-fg-full`: for games without OptiScaler.
   - `anr-fg-addon`: for games that already use [OptiScaler](https://github.com/optiscaler/OptiScaler).
2. Close the game, extract the package and run the installer with the folder of the game's `.exe`:
   - Windows: `install.cmd`
   - Linux and Steam Deck (Proton): `sh install.sh "<folder>"`, then set the game's launch options to the line it
     prints: `WINEDLLOVERRIDES="dxgi=n,b" %command%`, or `winmm=n,b` in Vulkan games
3. In the game, turn on frame generation (the NVIDIA option where the game offers several).

To uninstall, run `uninstall.cmd` or `sh uninstall.sh` with the same folder.

## Supported GPUs

- NVIDIA GeForce RTX 20, 30, 40 and 50 series, and GTX 16 series
- AMD Radeon RX 5000, 6000, 7000 and 9000 series, and Steam Deck
- Intel Arc A and B series

On Windows 10 and 11, and on Linux and SteamOS with Proton.

## Results and problems

Please post them as [issues](../../issues/new/choose):
- **Performance results**: run the test kit from the release and attach its result file.
- **Problems**: the game, GPU and driver, operating system, settings and what you saw, with the newest `nr_fg-*.log`
  from under the game's `OptiScaler/custom/Apex Neural Rendering FG` folder.

OptiScaler is GPL-3.0; its source for each release is attached to the release.

## Record and compare frame generation

**nr-record** records what a game gives its frame generation while you play, losslessly. Then it runs other frame generation on exactly those frames, offline, so that you can compare ANR FG, NVIDIA DLSS frame generation and others frame by frame: as videos, side by side, or as still images. It ships no frame generation: you point it at the DLLs you have (an ANR FG release, the `nvngx_dlssg.dll` of a game).

You need a D3D12 game with DLSS frame generation and the ANR FG package (`anr-fg-full`, which brings OptiScaler) installed in it, Windows 10 or 11 or Linux with Proton, and [ffmpeg](https://ffmpeg.org/download.html) with ffprobe on the `PATH` for videos and images. Replaying NVIDIA's DLSS-G needs an NVIDIA RTX 40 or 50 series GPU.

1. **Install.** Download `nr-record-1.0-windows.zip` (or `-linux.tar.gz`) from [Releases](../../releases) and extract it. Close the game and run the installer with the folder of the game's `.exe`:
   - Windows: `install.cmd "<folder>"`. Windows needs no launch option.
   - Linux and Steam Deck (Proton): `sh install.sh "<folder>"`. Under Proton only, the game's Steam launch options need `WINEDLLOVERRIDES="dxgi=n,b" %command%` (the ANR FG installer prints the same line); do not set it on Windows (a game with it set there may not start).

   It copies `nr-record` into the game's `OptiScaler/custom/` folder, sets `FGNvngxReplacement=nr-record` in `OptiScaler.ini` (keeping the original), and lets ANR FG generate the frames when you are not recording (NVIDIA's DLSS-G where ANR FG is not installed: `forward` in `nr-record/dlssg/nr_record.ini`). `uninstall.cmd` or `sh uninstall.sh` with the same folder restores `OptiScaler.ini` and removes it. By hand: copy `capture/OptiScaler/custom/nr-record` into `OptiScaler/custom/`, set `FGNvngxReplacement=nr-record`, and set `forward` to `..\..\Apex Neural Rendering FG\dlssg\nvngx_dlssg.dll` or `nvidia`.
2. **Record.** In the game, with DLSS frame generation on: **F6** starts and stops a recording, **F9** records the next 8 frames. A beep marks each start and stop; the game shows its real frames only while it records. Recordings go to `%LOCALAPPDATA%\nr-record\` on Windows and `~/.local/share/nr-record/` under Proton; a frame takes about 26 MB at 3840x2160 and about 6 MB at 1806x972. `nr_record.log` in the `nr-record/dlssg` folder says what was recorded.
3. **Replay and make videos.** On Windows, in the package folder (`REC`: a recording's folder):

   ```bat
   tools\nr-record.exe info REC
   tools\nr-replay.exe --record REC --out out\anr --generator provider --dll "<ANR FG folder>\nvngx_dlssg.dll" --label "ANR FG" --stream - | tools\nr-video.exe video - --out out\anr.mp4
   tools\nr-replay.exe --record REC --out out\dlssg --generator nvidia --dll "<game>\nvngx_dlssg.dll" --label DLSS-G --stream - | tools\nr-video.exe video - --out out\dlssg.mp4
   tools\nr-video.exe project out\anr.mp4 out\dlssg.mp4 --out out\cmp.compare.json --labels "ANR FG" DLSS-G
   ```

   On Linux, `tools/nr_replay_wine.py` runs both replays under Proton's Wine and writes both videos, a side-by-side video and the compare project:

   ```bash
   python3 tools/nr_replay_wine.py --record REC --out cmp --video \
       --generator provider --dll "<ANR FG folder>/nvngx_dlssg.dll" --label "ANR FG" \
       --b-generator nvidia --b-dll "<game>/nvngx_dlssg.dll" --b-label DLSS-G
   ```

   Replays with `--raw` instead of `--stream` keep each frame, and `nr-video png` writes chosen frames as PNG images.
4. **Compare.** `viewer/renderer --compare --project cmp/compare.json` (on Windows `viewer\renderer.exe --compare --project out\cmp.compare.json`) plays both videos in lockstep with a divider, side by side, pixel zoom and marks, and exports videos and images (`--compare-export PROJECT --out FILE --very-high` from the command line). `viewer/renderer --captures` browses your recordings and recreates their frames with each generator.

Not yet: Vulkan games, super resolution and ray reconstruction, AMD FSR and Intel XeSS frame generation as replay generators. [record/README.md](record/README.md) has how recording works, [record/FORMAT.md](record/FORMAT.md) the recording format, [record/replay/README.md](record/replay/README.md) the replay and video tools, and [renderer/README.md](renderer/README.md) the viewer. To build from source: `bazel build //record:nr-record //record/replay:nr-video //renderer`, and `bazel build --config=windows_cross //record/capture:package //record/replay:nr-replay` for the Windows parts.

## Citation

```bibtex
@software{apex_neural_rendering,
  author = {{Apex Compute}},
  title  = {Apex Neural Rendering (ANR)},
  year   = {2026},
  url    = {https://github.com/apex-compute/ANR},
  note   = {https://apexcompute.com/neural-rendering}
}
```
