#!/usr/bin/env python3
"""Terrain runtime tests (docs/TERRAIN_SHADING.md): run the game headless from build/bin.

  levels   tools/tests/terrain_tiers.cfg under --validation: no validation message, a screenshot per Engine
           Fidelity level, the levels differ (each level's techniques run)
  reload   the strip captured, assets/terrain/terrain.json edited while the game runs (macro texture
           strength 0), captured again: the reload is logged and applied before the next capture (the
           parameters reach the GPU in the frame the change is seen; the poll runs once a second)
  seam     the grass fade: no luminance step in the ground where the grass ends (terrain grass term)

    py tools/tests/run_terrain_tests.py [--bin build/bin] [--only levels|reload|seam]
Exit 1 on a failure. Needs a Vulkan device (lavapipe works: VK_ICD_FILENAMES=.../lvp_icd.json, under xvfb-run).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def run(bin_dir, cfg, workdir, log, validation=False, on_line=None):
    exe = os.path.join(bin_dir, "deadground.exe" if os.name == "nt" else "deadground")
    args = [exe, "--test", "--nosound", "--log", log, "--script", cfg]
    if validation:
        args.insert(3, "--validation")
    p = subprocess.Popen(args, cwd=workdir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    seen = 0
    while p.poll() is None:
        time.sleep(0.5)
        if on_line and os.path.exists(log):
            with open(log, encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
            for ln in lines[seen:]:
                on_line(ln)
            seen = len(lines)
    with open(log, encoding="utf-8", errors="replace") as f:
        return p.returncode, f.read()


def mean_rgb(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float32).reshape(-1, 3).mean(0)


def test_levels(bin_dir, work):
    fails = []
    code, log = run(bin_dir, os.path.join(ROOT, "tools", "tests", "terrain_tiers.cfg"), work, os.path.join(work, "levels.log"), validation=True)
    if "validation layer enabled" not in log:
        fails.append("validation layer not enabled (install / VK_ADD_LAYER_PATH)")
    bad = [ln for ln in log.splitlines() if "VUID" in ln or "Validation Error" in ln]
    if bad:
        fails.append(f"{len(bad)} validation messages, first: {bad[0][:200]}")
    shots = [os.path.join(work, f"terrain_level_{i}.png") for i in range(5)]
    missing = [s for s in shots if not os.path.exists(s)]
    if missing:
        fails.append("missing " + ", ".join(os.path.basename(m) for m in missing))
    else:
        ims = [np.asarray(Image.open(s).convert("RGB")).astype(np.float32) for s in shots]
        for i in range(4):
            d = float(np.abs(ims[i] - ims[i + 1]).mean())
            if d < 0.5:
                fails.append(f"levels {i} and {i + 1} render the same (mean difference {d:.2f})")
    print(f"levels: {'ok' if not fails else 'FAIL'}" + "".join("\n  " + f for f in fails))
    return fails


def test_reload(bin_dir, work):
    fails = []
    tj = os.path.join(bin_dir, "assets", "terrain", "terrain.json")
    backup = tj + ".bak"
    shutil.copyfile(tj, backup)
    cfg = os.path.join(work, "reload.cfg")
    with open(cfg, "w") as f:
        f.write("set r_maxfps 60\nmap terrain_yard\nwait 20\ntime_set 11:30\ntime_pause\nsetpos -2 12.5 -61 160 -30\nwait 1\nnoclip\nwait 10\n"
                "screenshot reload_a.png\nwait 2\nwait 12\nscreenshot reload_b.png\nwait 2\nquit\n")
    state = {"edited": None, "reloaded": None}

    def on_line(ln):
        if "reload_a.png" in ln and "screenshot saved" in ln and not state["edited"]:
            d = json.load(open(tj))
            d["macro"]["texture_strength"] = 0.0
            d["macro"]["strength"] = 0.0
            with open(tj, "w") as f:
                json.dump(d, f, indent=2)
            state["edited"] = time.time()
        if "terrain: reloaded" in ln:
            state["reloaded"] = ln.strip()

    try:
        run(bin_dir, cfg, work, os.path.join(work, "reload.log"), on_line=on_line)
    finally:
        shutil.move(backup, tj)
    log = open(os.path.join(work, "reload.log"), encoding="utf-8", errors="replace").read()
    if not state["edited"]:
        fails.append("never saw the first capture (no edit made)")
    elif "terrain: reloaded" not in log:
        fails.append("the edit was not reloaded")
    else:
        i_r, i_b = log.find("terrain: reloaded"), log.find("reload_b.png")
        if i_b >= 0 and i_r > i_b:
            fails.append("reloaded after the second capture")
        a, b = os.path.join(work, "reload_a.png"), os.path.join(work, "reload_b.png")
        if os.path.exists(a) and os.path.exists(b) and float(np.abs(mean_rgb(a) - mean_rgb(b)).max()) < 0.3:
            fails.append("the reloaded parameters did not change the frame")
    print(f"reload: {'ok' if not fails else 'FAIL'} ({state['reloaded'] or 'no reload line'})" + "".join("\n  " + f for f in fails))
    return fails


def test_seam(bin_dir, work):
    """Grass fade seam: across the terrain yard's flat grass base (x = 60, looking south from 15 m), the ground's
    row luminance must not step where the grass ends (Modern: 70 m): the largest row-to-row change within
    60..80 m stays under 3x the median change over 40..110 m."""
    fails = []
    cfg = os.path.join(work, "seam.cfg")
    h, vfov = 15.0, 75.0
    with open(cfg, "w") as f:
        f.write("set r_maxfps 60\nset cl_fov %g\nmap terrain_yard\nwait 20\ntime_set 11:30\ntime_pause\nweather_set overcast 0\nweather_lock 1\nset r_fidelity 4\n"
                "setpos 60 %g -85 180 0\nwait 1\nnoclip\nwait 40\nscreenshot seam.png\nwait 2\nquit\n" % (vfov, 10.0 + h))
    run(bin_dir, cfg, work, os.path.join(work, "seam.log"))
    p = os.path.join(work, "seam.png")
    if not os.path.exists(p):
        return ["seam: no capture"]
    im = np.asarray(Image.open(p).convert("L")).astype(np.float32)
    H, W = im.shape
    t = np.tan(np.radians(vfov) / 2)

    def row(d):   # the screen row of ground at horizontal distance d (pitch 0)
        return int(round((0.5 + 0.5 * (h / d) / t) * H))

    cols = slice(int(W * 0.4), int(W * 0.6))
    ds = np.arange(40.0, 110.0, 1.0)
    lum = np.array([im[row(d), cols].mean() for d in ds])
    lum = np.convolve(lum, np.ones(3) / 3, mode="same")
    step = np.abs(np.diff(lum))
    med = float(np.median(step[2:-2])) + 1e-3
    near = step[(ds[:-1] >= 60) & (ds[:-1] <= 80)]
    ratio = float(near.max()) / med
    if ratio > 3.0:
        fails.append(f"luminance step at the grass end {near.max():.2f} = {ratio:.1f}x the median row change")
    print(f"seam: {'ok' if not fails else 'FAIL'} (largest change near the fade {ratio:.1f}x the median)" + "".join("\n  " + f for f in fails))
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "bin"))
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="terrain_tests_")
    fails = []
    if a.only in ("", "levels"):
        fails += test_levels(a.bin, work)
    if a.only in ("", "reload"):
        fails += test_reload(a.bin, work)
    if a.only in ("", "seam"):
        fails += test_seam(a.bin, work)
    print(f"outputs in {work}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
