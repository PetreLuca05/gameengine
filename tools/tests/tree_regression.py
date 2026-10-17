#!/usr/bin/env python3
"""Tree regression (docs/FOLIAGE_GRASS.md: the ground-cover and terrain changes leave trees untouched): the
same canopy views rendered by two builds must match, to within the noise of one build rendered twice.

    py tools/tests/tree_regression.py <before build/bin> <after build/bin> [--heights h1,h2,h3]

The views look up into the vegetation map's pine, broadleaf and dead forests from below the canopy (sky
behind, no ground or grass in frame), wind and volumetrics off (both read time / shadow maps that grass
now writes into), TAA and eye adaptation off, HUD hidden, time and weather fixed. Two runs of one build
are still not bit-identical (frame pacing feeds streaming and the debug text), so the "before" build runs
twice: the before / after difference may not exceed that noise floor (x1.5, mean and pixels off by more
than 8 levels; the top and bottom bands, where the debug text and the held item sit, are left out).
Exit 1 when a view exceeds it.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
# forest centre (x, z), eye height above the ground there, yaw, pitch (up)
VIEWS = [("pine", -135, -125, 0, 65), ("broad", 135, -125, 90, 60), ("dead", -160, 95, 200, 60)]


def cfg_text(heights):
    lines = ["set r_maxfps 60", "set r_foliage_wind 0", "set r_volumetrics 0", "set r_aa 0", "set r_auto_exposure 0", "set r_upscaler 0", "set hud_opacity 0", "set cl_camera_socket 0", "set cl_fp_body 0", "map vegetation", "wait 20",
             "time_set 11:30", "time_pause", "weather_set clear 0", "weather_lock 1"]
    for (name, x, z, yaw, pitch), h in zip(VIEWS, heights):
        lines += [f"setpos {x} {h + 1.7:.2f} {z} {yaw} {pitch}", "wait 1", "noclip", "wait 40", f"screenshot tree_{name}.png", "wait 2"]
    lines.append("quit")
    return "\n".join(lines) + "\n"


def run(bin_dir, cfg, out):
    exe = os.path.join(bin_dir, "deadground.exe" if os.name == "nt" else "deadground")
    subprocess.run([exe, "--test", "--nosound", "--log", os.path.join(out, "log.txt"), "--script", cfg], cwd=out, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def diff(a, b):
    ia, ib = (np.asarray(Image.open(p).convert("RGB")).astype(np.int16) for p in (a, b))
    h = ia.shape[0]
    d = np.abs(ia - ib)[int(h * 0.08):int(h * 0.85)]
    return float(d.mean()), int((d.max(axis=2) > 8).sum()), d.max(axis=2)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    heights = [16.0, 16.0, 16.0]
    if "--heights" in sys.argv:
        heights = [float(v) for v in sys.argv[sys.argv.index("--heights") + 1].split(",")]
    work = tempfile.mkdtemp(prefix="tree_regression_")
    cfg = os.path.join(work, "trees.cfg")
    with open(cfg, "w") as f:
        f.write(cfg_text(heights))
    runs = {"before": sys.argv[1], "before2": sys.argv[1], "after": sys.argv[2]}
    for k, b in runs.items():
        os.makedirs(os.path.join(work, k))
        run(os.path.abspath(b), cfg, os.path.join(work, k))
    bad = 0
    for name, *_ in VIEWS:
        a, a2, b = (os.path.join(work, k, f"tree_{name}.png") for k in runs)
        if not all(os.path.exists(p) for p in (a, a2, b)):
            print(f"{name}: missing capture")
            bad += 1
            continue
        nm, nn, _ = diff(a, a2)
        m, n, d = diff(a, b)
        ok = m <= nm * 1.5 + 0.02 and n <= nn * 1.5 + 50
        print(f"{name}: before / after mean {m:.3f}, {n} px > 8 | noise floor mean {nm:.3f}, {nn} px > 8 | {'ok' if ok else 'DIFFERS'}")
        bad += not ok
        Image.fromarray(np.clip(d * 16, 0, 255).astype(np.uint8)).save(os.path.join(work, f"diff_{name}.png"))
    print(f"outputs in {work}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
