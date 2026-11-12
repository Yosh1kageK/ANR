# The nr-record format

A recording is a folder with four files. It holds every input a game gave a rendering feature (frame generation, super resolution or ray reconstruction) for each recorded real frame, losslessly, and every parameter of every call, so that any implementation of the feature can be run offline on exactly the same inputs.

| File | What |
|---|---|
| `record.json` | What was recorded: the feature, its source, the textures and their coding, the parameter names, the status |
| `frames.idx` | 64 bytes per recorded real frame |
| `frames.bin` | Each recorded real frame's textures, coded losslessly |
| `params.bin` | Each call's parameters |

```bash
nr-record info REC                       # a summary as JSON
nr-record decode REC --frame 0 --out f0  # frame 0's textures as raw texels, one file per role
```

`reader.h` reads every version below; `format.h` has the binary records.

## Versions

| `format` | Changes |
|---|---|
| `nr-record-1` | Coding 1; `frames.bin` holds each frame's words as they are |
| `nr-record-2` | `frames.bin` holds each frame in blocks, each stored or zstd-compressed |
| `nr-record-3` | Coding 3 |
| `nr-record-4` | `record.json` names the feature, the source, the graphics API, the coding and each texture's role. The binary files are as in format 3 |

A recording without `feature` (formats 1 to 3) is frame generation recorded from NGX DLSS-G calls in a D3D12 game.

## record.json

| Field | What |
|---|---|
| `format` | `nr-record-4` |
| `coding` | The GPU coding of the textures: 3 ([Coding 3](#coding-3)); formats 1 to 3 imply 1, 1 and 3 |
| `feature` | `fg`, `sr` or `rr` ([Features](#features)) |
| `source` | The interface the inputs came through: `ngx-dlssg` (NVIDIA DLSS frame generation), `ngx-dlss` (DLSS super resolution), `ngx-dlssd` (DLSS ray reconstruction), `ffx-upscale`, `ffx-framegen` (AMD FSR API), `xess`, `xefg` (Intel XeSS) |
| `graphics_api` | `d3d12` or `vulkan` |
| `recorder` | The program that wrote it, with its version |
| `game`, `time` | The game's executable and the local start time |
| `width`, `height` | The feature's output size |
| `qpc_frequency` | Ticks per second of the timestamps in `frames.idx` and `params.bin` |
| `data_start`, `slot_bytes`, `ring` | The slot layout ([The slot](#the-slot)) and the recorder's ring |
| `textures` | One entry per recorded texture, in coding order (below) |
| `parameter_source`, `parameters`, `matrices` | Whose names the parameters are (`ngx`: the NGX parameter names of NVIDIA's SDK headers, such as `DLSSG.JitterOffsetX`) and the names in `params.bin` order |
| `call_bytes`, `index_bytes`, `block_bytes`, `zstd_level` | Record sizes and the blocks' compression |
| `status` | `recording` while it runs, then `complete` or `stopped: <reason>` |
| `frames`, `dropped`, `lost`, `bytes`, `raw_bytes`, `stored_frames` | Real frames written, dropped (the ring was full), lost (a pack that never ran), bytes in `frames.bin`, coded bytes before compression, frames stored uncompressed for speed |
| `threads`, `queue_mb`, `queue_peak_mb`, `pack_gpu_ms_median`, `copy_ms_median`, `write_seconds`, `*_cpu_seconds` | The recorder's settings and costs |

Each feature adds its own fields: FG `multiplier` (generated frames per real frame plus 1), `encoding` (the color encoding, such as `PQ10` or `sRGB8`), `hdr` and `mode` (`interleaved`: videos of real and generated frames; `generated`: generated frames only).

A texture entry:

| Field | What |
|---|---|
| `role` | What it is ([Features](#features)); `name` repeats it for older readers |
| `api_format` | Its format in the graphics API (`DXGI_FORMAT` in D3D12, `VkFormat` in Vulkan); `dxgi_format` repeats it in D3D12 |
| `vk_format` | The Vulkan format of its texels as coded (a D3D12 format's equivalent; a depth format's depth plane is D32) |
| `width`, `height`, `channels` | Its size and channel count (1 to 4) |
| `kinds` | Per channel 8 bits (red lowest): `bits \| type << 5`, type 0 UNORM codes of `bits` bits, 1 FP16, 2 FP32 |
| `depth`, `srgb` | The depth plane of a depth format; an sRGB format |
| `reference` | The role it is coded against ([Coding 3](#coding-3)), or null |
| `table` | The first word of its chunk table in the slot |

## Features

### FG: frame generation

Per real frame (each generated frame's two endpoints are real frames), one params.bin record per call: with DLSS-G a call per generated frame.

| Role | What |
|---|---|
| `backbuffer` | The frame as shown, with the UI. Required |
| `hudless` | The frame without the UI |
| `ui` | The UI layer (color and coverage), where the game gives it |
| `ui_alpha` | The UI's coverage alone |
| `depth` | Depth as rendered. Required |
| `motion` | Motion vectors. Required |

`reader.h` `Fg` gives a call's parameters in one convention; from `ngx-dlssg`:

| Field | NGX parameter | What |
|---|---|---|
| `index`, `count` | the call's header | Generated frame `index` (1 to `count`) of this real frame |
| `reset` | `DLSSG.Reset` | The history is invalid (a cut) |
| `jitter` | `DLSSG.JitterOffsetX`, `Y` | Render pixels |
| `motion_scale` | `DLSSG.MvecScaleX`, `Y` | The stored motion times this is in pixels of the motion texture |
| `motion_jittered` | `DLSSG.MvecJittered` | The motion includes the jitter |
| `depth_inverted` | `DLSSG.DepthInverted` | Near is 1 |
| `near`, `far`, `fov`, `aspect` | `DLSSG.CameraNear`, `CameraFar`, `CameraFOV`, `CameraAspectRatio` | The camera |
| `camera_pos`, `camera_up`, `camera_right`, `camera_fwd` | `DLSSG.CameraPosX` ... `CameraFwdZ` | The camera's position and basis in world space |
| `clip_to_prev_clip`, `prev_clip_to_clip`, `view_to_clip`, `clip_to_view` | `DLSSG.ClipToPrevClip`, `PrevClipToClip`, `CameraViewToClip`, `ClipToCameraView` | 4 x 4, as given |
| `hdr` | `DLSSG.ColorBuffersHDR` | HDR colors (with RGB10A2: PQ) |
| `*_rect` | `DLSSG.<Texture>SubrectBaseX` ... `Height` | The part of a texture in use (all 0: the whole texture) |

### SR: super resolution

Per real frame, one call: the upscaler's evaluation. From `ngx-dlss` (OptiScaler's DLSS interface, which also carries games' FSR and XeSS inputs after OptiScaler's translation):

| Role | NGX parameter | What |
|---|---|---|
| `color` | `Color` | The frame at render size, before upscaling. Required |
| `depth` | `Depth` | Depth as rendered. Required |
| `motion` | `MotionVectors` | Motion vectors. Required |
| `exposure` | `ExposureTexture` | 1 x 1 exposure, where given |
| `reactive` | `FSR.reactive` | FSR's reactive mask (OptiScaler's name; XeSS's responsive mask arrives here too) |
| `transparency` | `FSR.transparencyAndComposition` | FSR's transparency and composition mask |
| `transparency_mask` | `TransparencyMask` | DLSS's transparency mask |
| `bias` | `DLSS.Input.Bias.Current.Color.Mask` | DLSS's bias-current-color mask |
| `output` | `Output` | The source's own upscaled result for this call, recorded after it |

`record.json` adds `create`: the feature's creation parameters by name, each `{"u": ..., "f": ...}` as read (as in `params.bin`), and `forward`: what produced `output` (for example `nvidia`).

`reader.h` `Sr` gives a call's parameters in one convention:

| Field | NGX parameter | What |
|---|---|---|
| `reset` | `Reset` | The history is invalid |
| `jitter` | `Jitter.Offset.X`, `Y` | Render pixels |
| `motion_scale` | `MV.Scale.X`, `Y` | The stored motion times this is in render pixels |
| `render_size` | `DLSS.Render.Subrect.Dimensions.Width`, `Height`, else `Width`, `Height` | The part of the inputs in use |
| `output_size` | `OutWidth`, `OutHeight` | The output's size |
| `create_flags`, `hdr`, `motion_low_res`, `motion_jittered`, `depth_inverted`, `sharpening`, `auto_exposure` | `DLSS.Feature.Create.Flags` | NGX's DLSS feature flags (bits 0, 1, 2, 3, 5, 6) |
| `quality`, `preset` | `PerfQualityValue`, `DLSS.Hint.Render.Preset.<mode>` | The quality mode and the render preset hint for it |
| `pre_exposure`, `exposure_scale`, `sharpness` | `DLSS.Pre.Exposure`, `DLSS.Exposure.Scale`, `Sharpness` | |
| `near`, `far`, `fov` | `FSR.cameraNear`, `FSR.cameraFar`, `FSR.cameraFovAngleVertical` | Given by FSR games only (0: not given) |
| `frame_time_ms` | `FrameTimeDeltaInMsec`, else `FSR.frameTimeDelta` | |
| `*_base` | `DLSS.Input.Color.Subrect.Base.X` ... `DLSS.Output.Subrect.Base.Y` | The corner of the part in use |

A value the call does not carry comes from `create`.

### RR: ray reconstruction

As SR (`ngx-dlssd`), with the guide buffers of the denoiser:

| Role | NGX parameter |
|---|---|
| `diffuse_albedo`, `specular_albedo` | `DLSS.Input.DiffuseAlbedo`, `DLSS.Input.SpecularAlbedo` |
| `normals`, `roughness` | `GBuffer.Normals`, `GBuffer.Roughness` (with `DLSS.Roughness.Mode` 1 the roughness is in the normals' w) |
| `specular_hit_distance`, `specular_motion` | `DLSSD.SpecularHitDistance`, `GBuffer.SpecularMvec` |
| `transparency_layer` | `DLSS.TransparencyLayer` |

Other guide buffers keep their NGX name as their role (for example `DLSSD.DiffuseHitDistance`, `DLSS.TransparencyLayerOpacity`). `Sr` adds `denoise_mode`, `roughness_mode` (`DLSS.Denoise.Mode`, `DLSS.Roughness.Mode`), and the matrices `WorldToViewMatrix` and `ViewToClipMatrix` as given.

## frames.idx

64 bytes per recorded real frame, in recording order, little-endian:

| Offset | Type | What |
|---:|---|---|
| 0 | char[4] | `NRI1` |
| 4 | u32 | Recorded real frames before this one |
| 8 | u64 | The source's real frame number (its calls in `params.bin` carry it) |
| 16 | u64 | Offset of the frame in `frames.bin` |
| 24 | u64 | Its bytes there |
| 32 | i64 | Timestamp at its first call (`qpc_frequency`) |
| 40 | u32 | Real frames dropped right before it: replays reset here |
| 44 | f32 | GPU ms of its pack |
| 48 | f32 | ms of its copy to memory |
| 52 | u32 | From format 2: the slot's coded bytes before compression |
| 56 | f32 | From format 2: ms of its blocks' compression or copy, summed over threads |
| 60 | u32 | 0 |

## frames.bin

From format 2, per real frame: a 24-byte header (`NRZ1`, u32 block count, u64 coded bytes, u32 block bytes, i32 zstd level), then a u32 per block with its stored size (bit 31 set: stored as it is, else one zstd frame), then the blocks. The blocks in order are the frame's slot: its coded words. Format 1 stores the words alone.

## params.bin

Per call, in call order: a 32-byte header (`NRP1`, u32 index, u32 count, u32 flags, u64 real frame, i64 timestamp), then per name of `parameters` 12 bytes (u32 has: 1 an unsigned value, 2 a float value, 3 both; u32 the unsigned value; the float's bits), then per name of `matrices` 68 bytes (u32 has, 16 floats). NGX parameters have no declared type: each is read both ways, and `has` says which reads succeeded.

## The slot

A real frame's words (u32). Word 0 is the number of words used. Each texture's chunk table starts at its `table` word: 2 words (the chunk's first word and its word count) per chunk, where a chunk is up to 32 tiles of 8 x 8 texels along a tile row, tile rows from the top. Chunks start at `data_start` or later, in any order (the GPU allocates them as it goes).

**Values.** A channel's values are integers of its width: UNORM codes as they are; FP16 and FP32 bits mapped to an order-preserving integer (negative values: all bits flipped; others: the sign bit set). An sRGB texture's codes are the stored sRGB codes. A depth format's depth plane is FP32. Decoded texels (`reader.h` `Decode`) are the stored texels, channels in RGBA order: BGRA8 formats decode as RGBA8, and a depth-stencil format as its D32 depth plane. R11G11B10 floats (`vk_format` 122) are coded as 3 FP16 channels (each 11- or 10-bit float is exactly an FP16 value) and decode to their packed 32-bit words.

### Coding 3

Each value's residual is the value minus left + above - above-left, where values left of the texture and above its tile row are 0 (in a tile row's first row: minus left), or, for a texture with a `reference`, the value minus the reference texture's value at the same texel. The residual is taken modulo 2^width as the integer nearest 0 and zigzagged ((r << 1) ^ (r >> 31)), so it fits in width bits.

Per 8 x 8 tile and channel (an entry), with B the bit count of its largest residual, the 64 residuals take b = B bits each, or b < B bits and a flag bit each, with each flagged residual's B - b bits above b stored apart; the encoder takes the b that gives the fewest bits. A chunk's entries are tile-major (tile 0's channels, then tile 1's). A chunk's record:

1. one byte per entry: b, plus 0x80 if it has flags; zero-padded to whole words;
2. per entry, its 64 fields in row-major order, each b bits of the residual and above them the flag if the entry has flags, least significant bit first, in whole words (2 words per field bit);
3. per entry with flags, one byte, B - b; zero-padded to whole words;
4. per entry with flags, per flagged residual in order, its bits above b in B - b bits, least significant first, contiguous across the entries; zero-padded to a whole word.

A decoder reads the residuals, then sums them along each row and down each tile row's columns (or adds the reference's values), modulo 2^width.

### Coding 1

Each value's residual is the value minus its left neighbour (in column 0, minus the value above), zigzagged as above. A chunk's record: one byte per entry with its bit count b, zero-padded to whole words; then per entry its 64 residuals in b bits each, least significant bit first, in 2 b words.
