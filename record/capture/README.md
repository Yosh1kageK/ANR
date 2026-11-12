# The capture DLL

`nvngx_dlssg.dll` records a game's frame generation inputs into nr-record folders ([FORMAT.md](../FORMAT.md)). It is an NGX frame generation feature that [OptiScaler](https://github.com/optiscaler/OptiScaler) loads as a custom DLSS-G provider, so it works in D3D12 games that offer DLSS frame generation, natively on Windows and under Proton. It contains no frame generation of its own: when it does not record, it passes every call to another frame generation DLL that you name (`forward`), or shows the real frames only.

```bash
bazel build --config=windows_cross //record/capture:package
# into the game: OptiScaler (dxgi.dll and OptiScaler.ini) beside the game's .exe, and this folder:
#   <game>/OptiScaler/custom/nr-record/dlssg/nvngx_dlssg.dll
#   <game>/OptiScaler/custom/nr-record/dlssg/nr_record.ini
#   <game>/OptiScaler/custom/nr-record/dlssg/kernels/record_pack.slang.record_{init,pack,done}.dxil
```

In `OptiScaler.ini` set `FGInput=nvngxfg` and `FGNvngxReplacement=nr-record` (the folder's name). Under Proton set the launch option `WINEDLLOVERRIDES="dxgi=n,b" %command%`. In the game, turn on DLSS frame generation.

| Key | Action |
|---|---|
| F6 | Start or stop a recording (`record_key`) |
| F9 | Record the next 8 real frames (`capture_key`, `capture_frames`): a capture for still comparisons |

A beep marks the start and the stop. `nr_record.log` beside the DLL has each start and stop with the folder, the textures, the frames written, the frames dropped and why.

- **Where.** `~/.local/share/nr-record/<date>-<time>-<game>/` under Proton (`$XDG_DATA_HOME/nr-record` if set), `%LOCALAPPDATA%\nr-record\...` on Windows, or `record_dir`. Captures end in `-capture`. At 3840 x 2160 with PQ10 color, HUD-less color, depth and motion a real frame takes about 26 MB.
- **What is shown while recording.** With `record_present = bypass` (the default) each generated slot is dropped (`OutputDisableInterpolation`) and the backbuffer goes to `OutputReal`, so the game shows its real frames only. With `keep` and a `forward` DLL, that DLL generates frames as without recording.
- **Stops.** The key, `record_max_seconds` (300), less free space than `record_free_gb` (50), a change of an input's size or format, or the game releasing frame generation (exit, resolution change). Real frames are dropped while the GPU's copies or the disk fall behind; the next recorded frame marks the gap.
- **Cost.** Per real frame, one compute pass on the game's command list codes the textures into GPU memory; a copy queue and threads below normal priority do the rest, and nothing waits on the game's thread.
- **forward.** What generates the frames while the DLL does not record (and, with `keep`, while it does): a DLL with DLSS-G's D3D12 NGX exports, for example a frame generation provider's `nvngx_dlssg.dll` in its own folder, or `nvidia`: NVIDIA's DLSS-G through NVIDIA's NGX core (`_nvngx.dll`, the driver's), as the game would use it. The core is the one the game or OptiScaler started; if none did, the DLL starts it with the game's folder and `nvidia_dlssg`'s in its search path for `nvngx_dlssg.dll`.

Not yet: Vulkan games.

| File | What |
|---|---|
| `capture.cpp` | The NGX exports, keys, recording start and stop, the bypass and the forward DLL |
| `nr_record.ini` | The settings with their defaults |
| `../d3d12_packer.*`, `../record_pack.slang` | The GPU coding on the game's command list |
