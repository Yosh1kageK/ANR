# nr-replay and nr-video

`nr-replay` runs a frame generation (FG), super resolution (SR) or ray reconstruction (RR) implementation on an nr-record recording ([FORMAT.md](../FORMAT.md)): it uploads each recorded real frame's textures, gives the implementation each call's recorded parameters, and writes the generated or upscaled frames. `nr-video` makes SDR videos, side-by-side videos, PNG images and the renderer's compare projects from them. Run two implementations on one recording to compare them on exactly the same frames.

On Linux, `nr_replay_wine.py` runs `nr-replay.exe` under Wine and streams its frames into `nr-video`:

```bash
REC=~/.local/share/nr-record/20261111-165107-game
python3 record/replay/nr_replay_wine.py --record $REC --out /tmp/cmp --video \
    --generator provider --dll ~/anr-fg/nvngx_dlssg.dll --label "ANR FG" \
    --b-generator nvidia --b-dll ~/game/nvngx_dlssg.dll --b-label DLSS-G
# /tmp/cmp/a/video.mp4, /tmp/cmp/b/video.mp4, /tmp/cmp/side-by-side.mp4, /tmp/cmp/compare.json
bazel run //renderer -- --compare --project /tmp/cmp/compare.json
```

On Windows (`cmd.exe`):

```bat
nr-replay.exe --record REC --out out\anr --generator provider --dll anr-fg\nvngx_dlssg.dll --stream - | nr-video.exe video - --out out\anr\video.mp4
nr-replay.exe --record REC --out out\anr --generator provider --dll anr-fg\nvngx_dlssg.dll --frames 100:104 --raw
nr-video.exe png out\anr --frames 1,2 --out out\anr\png
```

The tools ship no implementation. Each is a DLL the user gives, loaded at run time and called through its public interface.

## Generators

| `--generator` | `--dll` | How it runs |
|---|---|---|
| `provider` | A DLSS-G provider's `nvngx_dlssg.dll`, for example the ANR FG release's folder (the DLL with its files beside it) | Its own NGX D3D12 exports (`NVSDK_NGX_D3D12_Init_Ext`, `CreateFeature`, `EvaluateFeature`), with a parameter map of `nr-replay`'s |
| `nvidia` | NVIDIA's `nvngx_dlssg.dll`, for example from a game's folder | NVIDIA's NGX core from the driver (`_nvngx.dll`) with the DLL's folder as its search path, so that NGX loads DLSS-G from that DLL |

Per recorded real frame, `nr-replay` calls `EvaluateFeature` once per generated frame, as a game's DLSS-G host does:
- The textures are D3D12 textures of their recorded DXGI formats (a Vulkan recording's formats map to their D3D12 equivalents), in the states DLSS-G hosts give them: the backbuffer `NON_PIXEL_SHADER_RESOURCE`, the HUD-less color, UI, depth and motion `COPY_DEST`, the outputs `UNORDERED_ACCESS`.
- Each call gets its recorded NGX parameters. NGX parameters have no declared type, so the recording holds each value read both ways; the names NVIDIA's DLSS-G helper header sets as floats, and values whose two readings differ, go back as floats, the others as unsigned integers.
- `nr-replay` sets the size, backbuffer format, `MultiFrameCount`, `MultiFrameIndex` and `Reset` itself. `Reset` is 1 at the first replayed frame and after dropped frames (frames.idx `dropped`), and there the outputs are discarded: a run restarts with no generated frames before its first real frame.
- With `--multiplier` other than the recorded one, every generated frame gets the real frame's first call with its own count and index.
- `nvidia` only: where the recording has no camera matrices, field of view, aspect ratio or camera basis, DLSS-G gets a perspective projection from the recorded near and far planes (1 radian, the output's aspect where those are missing), identities for the other matrices, and a basis along the axes. A provider gets what the game gave.

`replay.json` names the DLL with its file version and SHA-256. With `nvidia`, it also names the NGX core and the `nvngx_dlssg.dll` NGX loaded, which can differ from `--dll` (NGX may prefer a newer copy it finds).

Other implementations fit behind `FgGenerator` ([generator.h](generator.h)): create on the recording's size and formats, evaluate one real frame's calls into the output textures, describe. AMD's FidelityFX frame generation DLLs and Intel's XeSS frame generation (`libxess_fg.dll`) would take the reader's typed parameters (`record::FgCall`) instead of NGX's names; XeSS frame generation presents through a swap chain, so its implementation would keep one of its own and copy the generated frames out. Neither is implemented yet.

## SR and RR

For an SR or RR recording (record.json `feature` `sr` or `rr`), `nr-replay` runs one upscaler per recorded frame and writes its output, one frame per recorded frame:

```bash
REC=~/.local/share/nr-record/20261112-100546-game-sr
python3 record/replay/nr_replay_wine.py --record $REC --out /tmp/sr --video \
    --generator dlss --dll ~/game/nvngx_dlss.dll \
    --b-generator fsr --b-dll ~/fsr/amd_fidelityfx_loader_dx12.dll
bazel run //renderer -- --compare --project /tmp/sr/compare.json
```

| `--generator` | `--dll` | How it runs |
|---|---|---|
| `dlss` | NVIDIA's `nvngx_dlss.dll`, for example from a game's folder | NVIDIA's NGX core from the driver with the DLL's folder as its search path; `--preset X` sets the render preset hint of every quality mode |
| `dlssd` | NVIDIA's `nvngx_dlssd.dll` | The same for DLSS ray reconstruction (RR recordings). Where the recording has no camera matrices, the identity and a perspective projection |
| `ngx` | A DLL with NGX's D3D12 exports and its own SR or RR feature | Its exports, with a parameter map of `nr-replay`'s |
| `fsr` | AMD's `amd_fidelityfx_loader_dx12.dll` (with `amd_fidelityfx_upscaler_dx12.dll` beside it) or FSR 3.1's `amd_fidelityfx_dx12.dll` | The FidelityFX API. The camera planes and field of view are the recording's (games that use FSR give them), else 0.1, 10000 and 60 degrees |
| `xess` | Intel's `libxess.dll` | XeSS's D3D12 API, at the quality setting whose input range holds the recording's render size |
| `recorded` | none | The recording's own `output` (what the game showed), converted to FP16 |

- The inputs are D3D12 textures of their recorded formats (typeless formats as their typed form), `NON_PIXEL_SHADER_RESOURCE` during the call; the output is `R16G16B16A16_FLOAT` at the recording's output size.
- NGX generators get every recorded parameter (the creation ones from record.json `create`) and set `Width`, `Height`, `OutWidth`, `OutHeight` and `Reset` themselves. `Reset` is 1 at the first replayed frame, after dropped frames, and where the game set it. FSR and XeSS get the reader's typed view (`record::Sr`): NGX's jitter and motion conventions are theirs too.
- Frames are `kind` `output` in `replay.json` (stream kind 2), `output_f<k>-f<k>_t0.000.nrraw` as raw frames. `hdr` is the recording's: HDR outputs are scene-linear and go through the FP16 HDR curve of `nr-video`.
- A recording that starts at the game's first frame replays through the DLL that recorded it bit for bit: DLSS 310.9.16 and FSR 3.1.6 reproduce the recorded `output` exactly on a synthetic recording.

## nr-replay

```text
nr-replay --record DIR --out DIR --generator provider|nvidia --dll PATH [--label TEXT] [--frames A:B]
          [--multiplier M] [--mode interleaved|generated] [--raw] [--stream PATH|-] [--ngx-log]
nr-replay --record SR_OR_RR_DIR --out DIR --generator dlss|dlssd|ngx|fsr|xess|recorded [--dll PATH] [--label TEXT]
          [--frames A:B] [--preset X] [--raw] [--stream PATH|-] [--ngx-log]
```

| Option | What |
|---|---|
| `--record` | The recording folder |
| `--out` | Where `replay.json` and the raw frames go |
| `--label` | The generator's name in outputs (default: from the DLL's version resource) |
| `--frames A:B` | Recorded real frames A to B - 1 (`A:` to the end) |
| `--multiplier M` | Generated frames per real frame plus 1 (default: the recording's) |
| `--mode` | `interleaved`: real and generated frames in display order; `generated`: generated frames only (default: the recording's) |
| `--raw` | One raw frame file per output frame in `--out` |
| `--stream PATH` | The frame stream to a file, a FIFO, or `-` for standard output |
| `--ngx-log` | NGX's log on standard error |

It needs `--raw`, `--stream` or both. It prints a summary line (on standard error when the stream uses standard output) and exits 1 on a failure, with `replay.json`'s `status` saying why.

Raw frames are for short runs: a 3840 x 2160 frame takes 66 MB. For videos of long recordings, stream the frames into `nr-video`: nothing goes to disk but the video.

## Output

### replay.json

| Field | What |
|---|---|
| `format` | `nr-replay-1` |
| `recording`, `recording_path` | The recording folder's name, and its path as given |
| `game`, `source` | From `record.json` |
| `generator` | `kind` (`provider` or `nvidia`), `label`, `dll` (absolute path), `file_version` (a.b.c.d, or null), `description` (the DLL's FileDescription, or null), `sha256` of the DLL, `multi_frame_count_max`. With `nvidia` also `ngx_core` and `ngx_core_version`, `loaded` (the `nvngx_dlssg.dll` NGX loaded), and `loaded_file_version` and `loaded_sha256` where it is another file |
| `gpu` | The adapter's name |
| `size` | [width, height] of the frames |
| `dxgi_format` | The frames' DXGI format: the recorded backbuffer's |
| `encoding`, `hdr` | From `record.json`: the color encoding (`PQ10`, `sRGB8`, ...) and whether the colors are HDR (10-bit: PQ) |
| `multiplier`, `recorded_multiplier`, `mode` | As replayed, and as recorded |
| `fps` | The frames' rate at the recorded pace: the median interval between consecutive recorded real frames with nothing dropped between them, times the frames per real frame (`multiplier` interleaved, `multiplier` - 1 generated); within 2% of a whole number, that number. 0 if the recording has no timestamps |
| `real_frames` | [A, B]: the recorded real frames replayed, A to B - 1 |
| `raw`, `stream` | Whether raw frames were written, and the stream's path or null |
| `frames` | Every output frame in display order (below) |
| `gaps` | `{frame, dropped}` per recorded frame after dropped frames: a run restarts there |
| `skipped` | `{frame, why}` per recorded frame without calls |
| `gpu_ms_median` | The median GPU time of one real frame's calls (D3D12 timestamps around them) |
| `seconds` | The replay's wall time |
| `ignored_textures` | Recorded roles that are not DLSS-G inputs, where any |
| `status` | `complete` or `failed: <why>` |

A frame:

| Field | What |
|---|---|
| `kind` | `real` or `generated`; `output` for SR and RR |
| `real` | The recorded real frame (index in `frames.idx`) whose calls made it: for a generated frame its later endpoint |
| `a`, `b` | The real frames it lies between; a real frame has a = b |
| `index` | The generated frame's index, 1 to `multiplier` - 1; 0 for a real frame |
| `t` | `index` / `multiplier`; 0 for a real frame |
| `file` | `<kind>_f<a>-f<b>_t<t>.nrraw` (t to 3 decimals), or null without `--raw` |

In `interleaved` mode, real frame k follows the generated frames between k - 1 and k. A real frame is the recorded backbuffer, as the game showed it.

### Raw frames

The renderer's raw format: a 32-byte header (`NRRAW1\0\0`, then u32 version 1, width, height, DXGI format, 4 channels, 0; little-endian), then float16 RGBA texels, rows from the top. UNORM formats store code / steps (1023 for 10-bit colors, 3 for their 2-bit alpha, 255 for 8 bits), so `round(value * steps)` gives each code back exactly; BGRA formats are in RGBA order. FP16 formats store their values as they are.

### The frame stream

`NRSTRM1\0`, u32 version 1, u32 header bytes, then the header: `replay.json` without `frames`, `gaps`, `skipped`, `gpu_ms_median`, `seconds` and `status`. Then per output frame a 48-byte record and its texels, and at the end a record with magic `NRSE`:

| Offset | Type | What |
|---:|---|---|
| 0 | char[4] | `NRSF` |
| 4 | u32 | kind: 0 real, 1 generated, 2 an SR or RR output |
| 8 | u32, u32 | a, b |
| 16 | u32 | index |
| 20 | f32 | t |
| 24 | u32, u32 | width, height |
| 32 | u32 | DXGI format |
| 36 | u32 | bytes per texel |
| 40 | u64 | bytes of texels that follow (width x height x bytes per texel) |

The texels are as stored, rows packed, in RGBA order: 10-bit colors as 32-bit words (red in the low bits), 8-bit colors as RGBA bytes, FP16 colors as 4 float16. A stream without the end record stopped early.

## nr-video

```text
nr-video video SOURCE --out FILE.mp4 [--fps F] [--codec auto|hevc_nvenc|libx265] [--qp N] [--ffmpeg PATH]
nr-video side-by-side SOURCE_A SOURCE_B --out FILE.mp4 [--video-a FILE] [--video-b FILE] [as video]
nr-video png SOURCE --frames N,N,... --out DIR [--ffmpeg PATH]
nr-video project VIDEO_A VIDEO_B --out FILE.compare.json [--labels A B] [--title TEXT]
```

A SOURCE is a replay folder with raw frames, or a frame stream (a FIFO, a file, or `-` for standard input).

- `video` writes 10-bit HEVC (4:2:0, BT.709 limited range, MP4) through ffmpeg: `hevc_nvenc` (constant QP) where ffmpeg has it and the GPU can encode with it, else `libx265` (CRF); `--qp` sets either (default 16). The rate is the source's `fps` (`--fps` overrides it; 60 where neither gives one).
- `side-by-side` puts two sources of one height side by side, A on the left, frame by frame, and with `--video-a` and `--video-b` writes each one's own video in the same pass. It reports where the two sources' frames differ in kind or index (`mismatch`).
- `png` writes the chosen output frames (numbered from 0 in display order, or by name: `generated_f4-f5_t0.333`) as 8-bit PNGs, `DIR/<name>.png`.
- `project` writes the renderer's compare project for two videos ([renderer/clips/project.h](../../renderer/clips/project.h); `bazel run //renderer -- --compare --project FILE`), with their labels, size and rate from the videos' sidecars.

Each video gets a sidecar `<video>.json`: frames, rate, codec, the curve, size, layout, labels and each source's recording and generator.

The SDR curve: 10-bit HDR colors are PQ codes to nits, BT.2020 to BT.709, 203 nits as 1.0, Reinhard x / (1 + x) per channel, then sRGB. FP16 HDR colors (scRGB) are 80 nits per 1.0 through the same curve. 8-bit colors and 10-bit SDR colors are sRGB codes, taken as they are. Odd sizes lose their last column or row.

## Linux

`nr_replay_wine.py` (Python 3, standard library) runs `nr-replay.exe` under Wine:
- Wine, vkd3d-proton, DXVK and dxvk-nvapi come from the newest Proton or GE-Proton under the Steam folders, or `--proton DIR` (a Proton build's `files/`), or `--wine PATH --vkd3d DIR --dxvk DIR [--nvapi DIR]`.
- The prefix is `$XDG_CACHE_HOME/nr-replay/wine-prefix` (`--prefix DIR`); the first run makes it. DXVK's `dxgi.dll` goes into its `system32`; each run gets a folder with `nr-replay.exe`, vkd3d-proton's `d3d12.dll` and `d3d12core.dll`, and DXVK's `dxgi.dll`.
- With `--generator nvidia`, `dlss` or `dlssd` it puts NVIDIA's NGX DLLs for Wine (`_nvngx.dll`, `nvngx.dll`, from `nvidia/wine` beside the driver's `libGLX_nvidia.so.0`, or `--nvidia-wine DIR`), dxvk-nvapi's `nvapi64.dll` and DXVK's `d3d11.dll` into `system32`, and sets `DXVK_ENABLE_NVAPI=1`, as Proton does for DLSS.
- `--video` sends each replay's stream through a pipe into `nr-video`. With `--b-generator` both replays run at once, and `nr-video` writes `a/video.mp4`, `b/video.mp4` and `side-by-side.mp4`, then `compare.json`.
- `--raw`, `--frames`, `--multiplier`, `--mode`, `--preset` and `--ngx-log` go to `nr-replay`; `--codec`, `--qp` and `--fps` to `nr-video`. The tools come from beside the script or the Bazel output folders (`--exe`, `--nr-video`).

## Build

```bash
bazel build //record/replay:nr-video                                 # Linux
bazel build --config=windows_cross //record/replay:nr-replay //record/replay:nr-video   # Windows x64, on Linux
```

`nr-replay` uses NVIDIA's NGX headers from the DLSS SDK, the FidelityFX API headers and XeSS's headers, and links none of their libraries. ffmpeg is the user's.

| File | What |
|---|---|
| `nr_replay.cpp` | `nr-replay`: decoding, upload, the calls, readback, outputs |
| `generator.h`, `ngx_generator.cpp` | The FG generator interface and its DLSS-G NGX implementation |
| `sr_generator.h`, `sr_generators.cpp`, `sr_replay.cpp` | SR and RR: the generator interface, its NGX, FSR and XeSS implementations, and the replay |
| `ngx_common.h`, `replay_io.h` | NGX's parameter map and core; the frame stream's writer and what both replays share |
| `frames.h`, `frames.cpp` | Raw frames, the frame stream, the SDR curve, P010 |
| `nr_video.cpp` | `nr-video` |
| `nr_replay_wine.py` | The Linux runner |
| `sha256.h` | SHA-256 of the DLLs |

## Limits

- D3D12 only; natively on Windows it is built but not yet run.
- SR and RR: subrects reach NGX generators as recorded; FSR gets none and XeSS the color, depth and motion corners. FSR and XeSS do no RR.
- The frames' formats: RGB10A2, RGBA8 and BGRA8 (and their sRGB and typeless forms), RGBA16F.
- Inputs whose decoded texels differ from their D3D12 layout stop the replay with a message (R11G11B10 decodes to its packed words, so it uploads as it is).
- One process runs one generator: two DLLs both named `nvngx_dlssg.dll` cannot share a process.
