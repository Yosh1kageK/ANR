# nr-record

nr-record records the inputs a game gives a rendering feature (frame generation, super resolution or ray reconstruction) while it plays, losslessly, with one GPU pass per real frame on the game's queue and no waits, so that other implementations of the feature can run offline on exactly the same frames and be compared: for example NVIDIA DLSS frame generation, AMD FSR, Intel XeSS and Apex Neural Rendering (ANR) on one recording.

```bash
bazel build //record:nr-record
REC=~/.local/share/nr-record/20261111-165107-game     # a recording folder
bazel-bin/record/nr-record info $REC             # feature, source, textures, frames, calls
bazel-bin/record/nr-record decode $REC --frame 0 --out /tmp/f0   # frame 0's textures as raw texels
bazel-bin/record/nr-record verify $REC           # decodes every frame
bazel-bin/record/nr-record params $REC --call 0  # a call's parameters by name, and for FG in one convention
```

Each command prints one JSON line. `decode` writes `f<N>_<role>.bin` per texture with its Vulkan format, size and bytes per texel in the JSON; the texels are the game's, bit for bit.

[FORMAT.md](FORMAT.md) specifies the recording folder: `record.json`, the frame index, the coded frames and the parameters, with the texture roles and parameters of each feature.

## How a recording is made

The recorders in games are DLLs that OptiScaler loads in its custom provider slots ([capture/README.md](capture/README.md)): `nvngx_dlssg.dll` for frame generation, and `nr_record_sr.dll` for SR and RR, which gets every upscaler call of a D3D12 game (DLSS, FSR or XeSS) as one NGX DLSS call. [replay/README.md](replay/README.md) runs DLSS-G, DLSS, DLSS-RR, FSR and XeSS on the recordings and makes the videos.

`recorder.h` is the recording core. A recorder in the game (a DLL that receives the feature's calls) gives it, per real frame, the frame's input textures and a command list, and per call the call's parameters:

- Each real frame's textures are coded losslessly on the GPU, on the game's command list, into a slot of a ring in GPU memory (`Packer`; the coding is [FORMAT.md](FORMAT.md#coding-3)'s). The game's queue gets this pass and nothing else, and never waits.
- A copy queue of the recorder copies each finished slot to memory (D3D12). In Vulkan games the pass writes the slot in host memory, as Vulkan gives no queue of our own on the game's device, and an event on the game's command buffer tells when it is complete. Worker threads below normal priority copy (or zstd-compress) it in 2 MiB blocks, which frees the slot, and a writer thread appends the frames and parameters to the folder in order.
- If the GPU's packs or copies, or the disk, fall behind, the ring fills and real frames are dropped; the next recorded frame marks the gap, and replays reset there.
- It stops at a time or frame limit, below a free-space limit, on a change of an input's size or format, or when the recorder stops it.

At 3840 x 2160 with PQ10 color, HUD-less color, D32S8 depth and RG16F motion, a real frame takes about 26 MB (1.6 GB/s at 60 real frames per second). On an RTX 5090 the pass takes 0.33 ms per real frame, and a frame that took 6.66 to 6.83 ms took 7.05 ms while recording, with no frame dropped in 600; the recorder's threads used 8.9 ms of CPU time per real frame.

Recordings go to `~/.local/share/nr-record/` under Proton (`$XDG_DATA_HOME/nr-record` if set) and to `%LOCALAPPDATA%\nr-record` on Windows, one folder per recording: `<date>-<time>-<game>`.

## Build

Bazel 9 ([Bazelisk](https://github.com/bazelbuild/bazelisk) picks the version in `.bazelversion`); Clang compiles everything.

```bash
bazel build //record:all                                  # Linux: the reader and nr-record
bazel build --config=windows_cross //record:recorder      # the recording core for Windows, on Linux
```

On Windows, `bazel build //record:all` needs an LLVM install (`BAZEL_LLVM`) and Visual Studio's C++ build tools. [tools/windows_cross/README.md](../tools/windows_cross/README.md) has the cross build and the license terms it accepts.

| File | What |
|---|---|
| `format.h` | The binary records and the coding's size bounds |
| `recorder.h`, `recorder.cpp` | The recording core (Windows): ring, copier, workers, writer; D3D12's ring |
| `d3d12_packer.*`, `vulkan_ring.*`, `record_pack.slang` | The GPU coding on a D3D12 game's command list (DXIL) and a Vulkan game's command buffer (SPIR-V) |
| `reader.h`, `reader.cpp`, `pool.h` | Reading recordings, formats 1 to 4, and decoding frames on the CPU |
| `nr_record.cpp` | The `nr-record` command |
