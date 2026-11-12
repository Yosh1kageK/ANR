# The capture DLLs

`nvngx_dlssg.dll` records a game's frame generation inputs into nr-record folders ([FORMAT.md](../FORMAT.md)). It is an NGX frame generation feature that [OptiScaler](https://github.com/optiscaler/OptiScaler) loads as a custom DLSS-G provider, so it works in D3D12 and Vulkan games that offer DLSS frame generation, natively on Windows and under Proton. It contains no frame generation of its own: when it does not record, it passes every call to another frame generation DLL that you name (`forward`), or shows the real frames only. `nr_record_sr.dll` does the same for super resolution and ray reconstruction ([SR and RR](#sr-and-rr)).

```bash
bazel build --config=windows_cross //record/capture:package
```

The release packages carry this folder as `capture/` with installers. The game needs OptiScaler first (the ANR FG full package installs it), then:

```bash
install.cmd "<folder of the game's .exe>"          # Windows
sh install.sh "<folder of the game's .exe>"        # Linux, games under Proton
```

The installer copies `nr-record` into the game's `OptiScaler/custom/`, sets `FGNvngxReplacement=nr-record` in `OptiScaler.ini` (keeping the original in `OptiScaler/nr_record_backup`), and sets `forward` to ANR FG where it is installed, else to `nvidia`; a reinstall keeps your `nr_record.ini`. `uninstall.cmd` or `sh uninstall.sh` with the same folder restores `OptiScaler.ini` and removes `nr-record`. Under Proton the game's Steam launch options need `WINEDLLOVERRIDES="dxgi=n,b" %command%` (OptiScaler loads as `dxgi.dll`); on Windows set no launch option. In the game, turn on DLSS frame generation.

| Key | Action |
|---|---|
| F6 | Start or stop a recording (`record_key`) |
| F9 | Record the next 8 real frames (`capture_key`, `capture_frames`): a capture for still comparisons |

A beep marks the start and the stop. A log per game session beside the DLL, `nr_record-<date>-<time>.log` (the last 10 kept), has each start and stop with the folder, the textures, the frames written, the frames dropped and why.

- **Where.** `~/.local/share/nr-record/<date>-<time>-<game>/` under Proton (`$XDG_DATA_HOME/nr-record` if set), `%LOCALAPPDATA%\nr-record\...` on Windows, or `record_dir`. Captures end in `-capture`. At 3840 x 2160 with PQ10 color, HUD-less color, depth and motion a real frame takes about 26 MB.
- **What is shown while recording.** With `record_present = bypass` (the default) each generated slot is dropped (`OutputDisableInterpolation`) and the backbuffer goes to `OutputReal`, so the game shows its real frames only. With `keep` and a `forward` DLL, that DLL generates frames as without recording.
- **Stops.** The key, `record_max_seconds` (300), less free space than `record_free_gb` (50), a change of an input's size or format, or the game releasing frame generation (exit, resolution change). Real frames are dropped while the GPU's copies or the disk fall behind; the next recorded frame marks the gap.
- **Cost.** Per real frame, one compute pass on the game's command list codes the textures into GPU memory; a copy queue and threads below normal priority do the rest, and nothing waits on the game's thread. In Vulkan games the pass writes the coded words straight into host memory (Vulkan gives no queue of our own on the game's device), so there is no copy; the threads read the memory once an event on the game's command buffer is set.
- **forward.** What generates the frames while the DLL does not record (and, with `keep`, while it does): a DLL with DLSS-G's NGX exports (D3D12's for D3D12 games, Vulkan's for Vulkan games), for example a frame generation provider's `nvngx_dlssg.dll` in its own folder, or `nvidia`: NVIDIA's DLSS-G through NVIDIA's NGX core (`_nvngx.dll`, the driver's), as the game would use it. The core is the one the game or OptiScaler started; if none did, the DLL starts it with the game's folder and `nvidia_dlssg`'s in its search path for `nvngx_dlssg.dll`.


## SR and RR

`nr_record_sr.dll` records a game's upscaler inputs (FORMAT.md "SR", "RR"). It is an NGX runtime that OptiScaler loads as a custom DLSS upscaler from `OptiScaler/custom/nr-record/dlss/`. OptiScaler translates whatever upscaler the game calls, DLSS, FSR 2 to 3.1 or XeSS, into one NGX DLSS evaluation and hands it to this DLL, so the one DLL records every D3D12 game's SR inputs, with stock OptiScaler. It contains no upscaler of its own: every call goes to `forward`, and the DLL records the call's inputs and parameters, and the forward's output after the call (`output`), in the same pass.

To record SR, open OptiScaler's menu (Insert), pick `nr-record` in the upscaler list, and press F6 (F9 for a capture). OptiScaler switches to it live, and back when you pick your own upscaler again. The installers do not change the upscaler setting, so the DLL is in the upscale path only while `nr-record` is picked. If you save OptiScaler's settings while it is picked, OptiScaler starts with it next time.

| `forward` | Upscaler |
|---|---|
| `auto` (default) | The upscaler `OptiScaler.ini` names (`Dx12Upscaler` as OptiScaler last saved it: `dlss`, `xess`, or an FSR), so that picking `nr-record` keeps the picture as it was; else `nvidia` where NVIDIA's NGX core is installed, else `ffx`. RR: `nvidia` |
| `nvidia` | NVIDIA's DLSS (and DLSS-RR) through NVIDIA's NGX core, which finds `nvngx_dlss.dll` in OptiScaler's or the game's folder or `nvidia_dlss` |
| `ffx` | AMD's FSR 3.1 through the FidelityFX API, on any GPU: `ffx_dll`, else `amd/nr_record_ffx_dx12.dll` beside the DLL (the package's, [AMD's](#amd-fidelityfx)), else `amd_fidelityfx_dx12.dll` beside the DLL, in OptiScaler's folder or in the game's |
| `xess` | Intel's XeSS through `libxess.dll`, on any GPU: `xess_dll`, else `libxess.dll` beside the DLL, in OptiScaler's folder or in the game's (not OptiScaler itself under that name) |

OptiScaler takes over every DLL loaded under AMD's FidelityFX names or as `libxess.dll` (it hooks them as the game's upscaler inputs), so the DLL loads FSR and XeSS from copies under names of its own (`%TEMP%\nr-record\nr_record_<name>`). A FidelityFX loader (`amd_fidelityfx_loader_dx12.dll`) still loads AMD's providers under AMD's names and does not work here; `amd_fidelityfx_dx12.dll` (FSR 3.1, one DLL) does.
| a path | Another DLL with NGX's D3D12 exports |

With `auto`, an OptiScaler setting of `auto` (OptiScaler picks per game) gives `nvidia` or `ffx`, which may differ from what OptiScaler had picked. The keys, folders, stops and costs are the frame generation DLL's (above). Folders end in `-sr` or `-rr` (`-sr-capture` for F9). Its settings are `dlss/nr_record.ini` ([nr_record_sr.ini](nr_record_sr.ini)).

- **What OptiScaler hands over.** The game's inputs after OptiScaler's translation: for DLSS games NGX's parameters as the game set them; for FSR and XeSS games OptiScaler's NGX names, including `FSR.reactive`, `FSR.transparencyAndComposition` and FSR's camera (`FSR.cameraNear` ...). OptiScaler's own options that change the inputs (its output scaling, mask and exposure overrides) apply before the DLL sees them.
- **RR.** Stock OptiScaler sends DLSS-RR (`nvngx_dlssd`) calls to NVIDIA's DLSS-RR only and shows no upscaler list for them. An OptiScaler build with the change that sends them to a picked custom runtime too (not in stock OptiScaler yet) lists `DLSSD` and `nr-record` in the menu for RR games, and records them the same way. The DLL records RR features with their guide buffers (`ngx-dlssd`) and forwards them to NVIDIA's DLSS-RR.
- **Formats.** Colors RGBA16F, R11G11B10 (bit for bit), RGB10A2, RGBA8; depth D32, R32 and D32S8; motion RG16F and RG32F; masks R8. D24S8 depth is not coded yet: such a recording does not start.
- **Checking it.** `nr-sr-host` plays a game with a synthetic jittered scene: under Wine with vkd3d-proton, `nr-sr-host.exe --ngx custom\nr_record_sr.dll --frames 60 --record-frames 0,59` records 59 frames through the DLL directly, and with OptiScaler as `dxgi.dll` beside it (`--ngx dxgi.dll`, `Dx12Upscaler=nr-record` in `OptiScaler.ini`) through OptiScaler's custom upscaler slot.

### AMD FidelityFX

The package's `dlss/amd/nr_record_ffx_dx12.dll` is AMD's `amd_fidelityfx_dx12.dll` from FidelityFX SDK 1.1.4 (FSR 3.1.4), renamed and otherwise unchanged, under AMD's MIT license, which `AMD_FidelityFX_license.md` beside it reproduces. It sits in `amd/` because OptiScaler takes the first DLL in `dlss/` as the custom upscaler.

| File | What |
|---|---|
| `capture.cpp` | The NGX exports, keys, recording start and stop, the bypass and the forward DLL |
| `nr_record.ini` | The settings with their defaults |
| `sr_capture.cpp`, `nr_record_sr.ini` | The SR and RR capture DLL and its settings |
| `../xess_upscaler.*` | XeSS through libxess.dll (the `xess` forward) |
| `sr_host.cpp` | `nr-sr-host`, the synthetic game that checks it |
| `../ffx_upscaler.*`, `../sr_roles.h` | FSR through the FidelityFX API (the `ffx` forward); SR's and RR's roles and NGX names |
| `../d3d12_packer.*`, `../vulkan_ring.*`, `../record_pack.slang` | The GPU coding on the game's command list (D3D12, DXIL) or command buffer (Vulkan, SPIR-V) |
