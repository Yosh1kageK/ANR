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

## ANR FG Beta

1. Download a package from [Releases](../../releases):
   - `anr-fg-full`: for games without OptiScaler.
   - `anr-fg-addon`: for games that already use [OptiScaler](https://github.com/optiscaler/OptiScaler).
2. Close the game, extract the package and run the installer with the folder of the game's `.exe`:
   - Windows: `install.cmd`
   - Linux and Steam Deck (Proton): `sh install.sh "<folder>"`, then set the game's launch options to
     `WINEDLLOVERRIDES="dxgi=n,b" %command%`
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
  from under the game's `OptiScaler/custom/nrfg` folder.

OptiScaler is GPL-3.0; its source for each release is attached to the release.
