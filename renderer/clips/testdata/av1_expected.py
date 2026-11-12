"""Prints, from ffmpeg's trace_headers filter, the AV1 frame headers of a file as the rows av1_expected in
renderer/clips/video/demux_test.cpp holds: one row per frame header in decode order (temporal units are separated by
a blank comment line).

    python3 av1_expected.py av1_svt_128.mp4

Each row: frame_type, show_frame, order_hint, refresh_frame_flags, primary_ref_frame, base_q_idx, ref_frame_idx[7];
a show_existing_frame row is {255, frame_to_show_map_idx, 0, 0, 0, 0, ...}. Values the syntax does not code are the
specification's: refresh_frame_flags 255 for a shown key frame, primary_ref_frame 7 when it is not coded.
"""

import re
import subprocess
import sys

FIELD = re.compile(r"\] +\d+ +([A-Za-z_0-9]+(?:\[\d+\])?) +[01]+ = (-?\d+)")


def trace(path):
    command = ["ffmpeg", "-hide_banner", "-loglevel", "trace", "-i", path, "-map", "0:v", "-c", "copy", "-bsf:v",
               "trace_headers", "-f", "null", "-"]
    text = subprocess.run(command, capture_output=True, text=True, check=True).stderr
    return [line for line in text.splitlines() if line.startswith("[trace_headers")]


def frames(lines):
    current = None
    out = []
    for line in lines:
        if "Packet:" in line:
            out.append("TU")
        m = FIELD.search(line)
        if not m:
            continue
        name, value = m.group(1), int(m.group(2))
        if name == "show_existing_frame":
            if current is not None:
                out.append(current)
            current = {"show_existing_frame": value}
        elif current is not None:
            current[name] = value
    if current is not None:
        out.append(current)
    return out


def main():
    rows = frames(trace(sys.argv[1]))
    for f in rows:
        if f == "TU":
            continue
        if f["show_existing_frame"]:
            print("    {255, %d, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0}}," % f["frame_to_show_map_idx"])
            continue
        frame_type = f["frame_type"]
        show = f["show_frame"]
        refresh = f.get("refresh_frame_flags", 255 if frame_type == 0 and show else 0)
        primary = f.get("primary_ref_frame", 7)
        refs = [f.get("ref_frame_idx[%d]" % i, 0) for i in range(7)]
        print("    {%d, %d, %d, %d, %d, %d, {%s}}," % (frame_type, show, f["order_hint"], refresh, primary, f["base_q_idx"],
                                                      ", ".join(map(str, refs))))


main()
