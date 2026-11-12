#!/usr/bin/env python3
"""Runs nr-replay.exe on Linux under Wine with vkd3d-proton (README.md "Linux"), and with --video makes the videos
with nr-video as the frames come (no frames on disk).

    python3 nr_replay_wine.py --record REC --out OUT --generator provider --dll ANR/nvngx_dlssg.dll --label "ANR FG" \\
        [--b-generator nvidia --b-dll GAME/nvngx_dlssg.dll --b-label DLSS-G] [--video] [--frames A:B] [--raw] ...

One generator: OUT/replay.json, with --raw OUT's raw frames, with --video OUT/video.mp4. Two (--b-*): OUT/a and OUT/b,
both replays at once; with --video also OUT/side-by-side.mp4 and OUT/compare.json (the renderer's compare project).
Other options pass to nr-replay (--frames, --multiplier, --mode, --raw, --ngx-log) and nr-video (--codec, --qp, --fps).

Wine: Proton's (newest Proton or GE-Proton under the Steam folders, or --proton DIR), with its vkd3d-proton, DXVK and
dxvk-nvapi; or --wine PATH with --vkd3d DIR and --dxvk DIR (and --nvapi DIR for nvidia). The prefix is
$XDG_CACHE_HOME/nr-replay/wine-prefix (--prefix). --generator nvidia also needs the NVIDIA driver's NGX DLLs for Wine
(_nvngx.dll, nvngx.dll), found next to the driver's libGLX_nvidia.so.0 in nvidia/wine (--nvidia-wine DIR).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def wine_path(path):
    return "Z:" + str(Path(path).resolve()).replace("/", "\\")


def steam_roots():
    home = Path.home()
    return [home / ".local/share" / "Steam", home / ".steam" / "steam", home / ".steam" / "root",
            home / ".var/app/com.valvesoftware.Steam/data" / "Steam"]


def find_proton():
    """The newest Proton build (files/ with bin/wine and vkd3d-proton) under the Steam folders."""
    found = []
    for root in steam_roots():
        for parent in (root / "compatibilitytools.d", root / "steamapps" / "common"):
            if parent.is_dir():
                for d in parent.iterdir():
                    files = d / "files"
                    if (files / "bin/wine").exists() and wine_dlls(files):
                        found.append(files)
    found = sorted(set(found), key=lambda f: (f / "bin/wine").stat().st_mtime)
    return found[-1] if found else None


def wine_dlls(files):
    """Proton's DLL folders: vkd3d-proton, DXVK and dxvk-nvapi (x86_64-windows), or None."""
    for lib in ("lib/wine", "lib64/wine"):
        base = files / lib
        if (base / "vkd3d-proton/x86_64-windows/d3d12.dll").exists():
            return dict(vkd3d=base / "vkd3d-proton/x86_64-windows", dxvk=base / "dxvk/x86_64-windows",
                        nvapi=base / "nvapi/x86_64-windows")
    return None


def nvidia_wine_dir():
    """The NVIDIA driver's NGX DLLs for Wine: nvidia/wine next to libGLX_nvidia.so.0, as Proton finds them."""
    candidates = []
    ldconfig = shutil.which("ldconfig") or "/sbin/ldconfig"
    try:
        listing = subprocess.run([ldconfig, "-p"], capture_output=True, text=True, timeout=30).stdout
    except OSError:
        listing = ""
    for line in listing.splitlines():
        if "libGLX_nvidia.so.0" in line and "x86-64" in line and "=>" in line:
            candidates.append(Path(line.split("=>")[1].strip()).parent / "nvidia/wine")
    candidates += [Path("/usr/lib/x86_64-linux-gnu/nvidia/wine"), Path("/usr/lib64/nvidia/wine"),
                   Path("/usr/lib/nvidia/wine")]
    return next((c for c in candidates if (c / "_nvngx.dll").exists()), None)


def find_tool(given, names):
    """A tool: the given path, next to this script, in the Bazel output folders, or on PATH."""
    if given:
        return Path(given)
    repo = HERE.parents[1]
    for name in names:
        for place in (HERE / name, repo / "bazel-windows-bin/record/replay" / name,
                      repo / "bazel-bin/record/replay" / name):
            if place.exists():
                return place
        if shutil.which(name):
            return Path(shutil.which(name))
    raise SystemExit(f"{names[0]} not found: build it (README.md) or give its path")


def copy_if_changed(source, target):
    if not target.exists() or target.read_bytes() != source.read_bytes():
        shutil.copyfile(source, target)


class Wine:
    def __init__(self, args):
        if args.wine:
            self.wine = Path(args.wine)
            if not args.vkd3d or not args.dxvk:
                raise SystemExit("--wine needs --vkd3d DIR (d3d12.dll, d3d12core.dll) and --dxvk DIR (dxgi.dll, "
                                 "d3d11.dll)")
            self.dlls = dict(vkd3d=Path(args.vkd3d), dxvk=Path(args.dxvk), nvapi=Path(args.nvapi or args.dxvk))
        else:
            files = Path(args.proton) if args.proton else find_proton()
            if not files or not wine_dlls(files):
                raise SystemExit("no Proton with vkd3d-proton found: give --proton DIR (a Proton build's files/) or "
                                 "--wine PATH --vkd3d DIR --dxvk DIR")
            self.wine, self.dlls = files / "bin/wine", wine_dlls(files)
        self.prefix = Path(args.prefix or Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") /
                           "nr-replay/wine-prefix")
        self.env = dict(os.environ, WINEPREFIX=str(self.prefix), WINEDEBUG=os.environ.get("WINEDEBUG", "-all"),
                        WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n", DXVK_LOG_LEVEL="none",
                        VKD3D_DEBUG=os.environ.get("VKD3D_DEBUG", "none"))
        if not (self.prefix / "system.reg").exists():
            self.prefix.mkdir(parents=True, exist_ok=True)
            print(f"nr-replay-wine: making the Wine prefix {self.prefix}", file=sys.stderr)
            subprocess.run([str(self.wine), "wineboot", "-u"], env=self.env, check=True, timeout=600,
                           capture_output=True)
        self.system32 = self.prefix / "drive_c/windows/system32"
        copy_if_changed(self.dlls["dxvk"] / "dxgi.dll", self.system32 / "dxgi.dll")
        self.nvidia_ready = False

    def setup_nvidia(self, given_dir):
        """NVIDIA's NGX core and dxvk-nvapi in the prefix, as Proton sets them up for DLSS."""
        if self.nvidia_ready:
            return
        ngx = Path(given_dir) if given_dir else nvidia_wine_dir()
        if not ngx or not (ngx / "_nvngx.dll").exists():
            raise SystemExit("the NVIDIA driver's NGX DLLs for Wine (_nvngx.dll) were not found: give --nvidia-wine "
                             "DIR")
        for source in (ngx / "_nvngx.dll", ngx / "nvngx.dll", self.dlls["nvapi"] / "nvapi64.dll",
                       self.dlls["dxvk"] / "d3d11.dll"):  # nvapi64 imports d3d11
            copy_if_changed(source, self.system32 / source.name)
        self.env.update(WINEDLLOVERRIDES=self.env["WINEDLLOVERRIDES"] + ";d3d11=n;nvapi64=n;nvcuda=b",
                        DXVK_ENABLE_NVAPI="1", NVIDIA_WINE_DLL_DIR=str(ngx),
                        DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS="DLSSIndicator=0,DLSSGIndicator=0,")
        self.nvidia_ready = True

    def run_dir(self, exe, work):
        """A folder with nr-replay.exe and the D3D12 DLLs it runs on."""
        run = Path(tempfile.mkdtemp(prefix="run-", dir=work))
        shutil.copy(exe, run / "nr-replay.exe")
        for source in (self.dlls["vkd3d"] / "d3d12.dll", self.dlls["vkd3d"] / "d3d12core.dll",
                       self.dlls["dxvk"] / "dxgi.dll"):
            shutil.copy(source, run / source.name)
        return run


def replay_argv(args, generator, dll, label, out, stream):
    argv = ["--record", wine_path(args.record), "--out", wine_path(out), "--generator", generator, "--dll",
            wine_path(dll)]
    if label:
        argv += ["--label", label]
    for name in ("frames", "multiplier", "mode"):
        if getattr(args, name):
            argv += [f"--{name}", str(getattr(args, name))]
    argv += ["--raw"] if args.raw else []
    argv += ["--ngx-log"] if args.ngx_log else []
    argv += ["--stream", "-"] if stream else []
    return argv


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--record", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--generator", choices=("provider", "nvidia"), required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--label")
    parser.add_argument("--b-generator", choices=("provider", "nvidia"))
    parser.add_argument("--b-dll", type=Path)
    parser.add_argument("--b-label")
    parser.add_argument("--video", action="store_true", help="encode the videos from the frame stream")
    parser.add_argument("--frames")
    parser.add_argument("--multiplier", type=int)
    parser.add_argument("--mode", choices=("interleaved", "generated"))
    parser.add_argument("--raw", action="store_true")
    parser.add_argument("--ngx-log", action="store_true")
    parser.add_argument("--codec")
    parser.add_argument("--qp", type=int)
    parser.add_argument("--fps", type=float)
    parser.add_argument("--exe", help="nr-replay.exe (default: next to this script, or the Bazel output)")
    parser.add_argument("--nr-video", help="nr-video (default: next to this script, the Bazel output, or PATH)")
    parser.add_argument("--proton", help="a Proton build's files/ folder")
    parser.add_argument("--wine")
    parser.add_argument("--vkd3d")
    parser.add_argument("--dxvk")
    parser.add_argument("--nvapi")
    parser.add_argument("--prefix")
    parser.add_argument("--nvidia-wine", help="the NVIDIA driver's NGX DLLs for Wine")
    args = parser.parse_args(argv)
    if bool(args.b_generator) != bool(args.b_dll):
        parser.error("--b-generator and --b-dll go together")
    if not args.video and not args.raw:
        parser.error("nothing to write: give --video and/or --raw")

    exe = find_tool(args.exe, ["nr-replay.exe"])
    video_tool = find_tool(args.nr_video, ["nr-video"]) if args.video else None
    wine = Wine(args)
    sides = [("a" if args.b_generator else "", args.generator, args.dll, args.label)]
    if args.b_generator:
        sides.append(("b", args.b_generator, args.b_dll, args.b_label))
    for _, generator, _, _ in sides:
        if generator == "nvidia":
            wine.setup_nvidia(args.nvidia_wine)
    args.out.mkdir(parents=True, exist_ok=True)
    video_options = []
    for name in ("codec", "qp", "fps"):
        if getattr(args, name) is not None:
            video_options += [f"--{name}", str(getattr(args, name))]

    with tempfile.TemporaryDirectory(prefix="nr-replay-") as work:
        replays, pipes = [], []
        for side, generator, dll, label in sides:
            out = args.out / side if side else args.out
            out.mkdir(parents=True, exist_ok=True)
            run = wine.run_dir(exe, Path(work))
            read_end, write_end = os.pipe() if args.video else (None, None)
            command = [str(wine.wine), str(run / "nr-replay.exe")] + replay_argv(args, generator, dll, label, out,
                                                                                bool(args.video))
            replays.append(subprocess.Popen(command, cwd=run, env=wine.env, stdout=write_end if args.video else None))
            if args.video:
                os.close(write_end)
                pipes.append(read_end)
            elif len(sides) > 1:  # without streams, one replay after the other
                replays[-1].wait()
        video = None
        if args.video:
            if len(sides) == 1:
                command = [str(video_tool), "video", "-", "--out", str(args.out / "video.mp4")] + video_options
                video = subprocess.Popen(command, stdin=pipes[0])
            else:
                command = [str(video_tool), "side-by-side", f"/dev/fd/{pipes[0]}", f"/dev/fd/{pipes[1]}", "--out",
                           str(args.out / "side-by-side.mp4"), "--video-a", str(args.out / "a/video.mp4"),
                           "--video-b", str(args.out / "b/video.mp4")] + video_options
                video = subprocess.Popen(command, pass_fds=pipes)
            for fd in pipes:
                os.close(fd)
        codes = [p.wait() for p in replays]
        video_code = video.wait() if video else 0
    failed = any(codes) or video_code
    if args.video and len(sides) > 1 and not failed:
        subprocess.run([str(video_tool), "project", str(args.out / "a/video.mp4"), str(args.out / "b/video.mp4"),
                        "--out", str(args.out / "compare.json")], check=True)
    summary = dict(out=str(args.out), replays=codes, video=video_code if args.video else None)
    for side, _, _, _ in sides:
        report = (args.out / side / "replay.json") if side else args.out / "replay.json"
        if report.exists():
            data = json.loads(report.read_text())
            summary[side or "replay"] = dict(status=data.get("status"),
                                             generator=data.get("generator", {}).get("label"),
                                             frames=len(data.get("frames", [])),
                                             gpu_ms_median=data.get("gpu_ms_median"))
    print(json.dumps(summary))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
