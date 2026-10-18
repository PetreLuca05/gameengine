#!/usr/bin/env python3
"""Texture load A / B (docs/TEXTURE_MIPS.md "Block compression"; PF-0035 BC7, PF-0024 decode on the jobs).

Runs tools/tests/tex_bc.cfg for side A and side B, alternating, `reps` times each (first one B run
that fills the texture cache, reported as the cold load: its encode time). By default A is
r_texture_compress 0 (RGBA8) and B is 1 (BC7); --a / --b give other command lines (PF-0024:
--a "+set perf_fix_disable PF-0024" --b ""). Reports the medians per side of: VRAM (vram_usage_mb /
vram_allocated_mb of the dev room and vegetation captures), GPU frame ms (frame cap off during the
captures), the startup (prof: startup) and the texture load lines (manifest, VFX atlas, trees, grass,
surfaces, terrain); and per view the image difference of A0 vs B0 (PSNR, mean / max abs difference,
pixels off by more than 8 levels) next to A0 vs A1 (what two identical runs differ by), with a diff
heat map and an A | B pair when Pillow is installed.

  python tools/tests/tex_bc_compare.py [--bin build/bin] [--out tex_bc_out] [--reps 2] [--a ARGS] [--b ARGS]
"""
import argparse
import json
import math
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = os.path.join(HERE, "tex_bc.cfg")
VIEWS = ["dev_hall", "dev_character", "veg_meadow", "veg_ground", "veg_pine", "veg_edge", "veg_wheat"]
LOADS = [
    ("startup", r"prof: startup ([\d.]+) ms"),
    ("manifest", r"assets: \d+ manifest textures in ([\d.]+) ms"),
    ("vfx_atlas", r"vfx: atlas .* in ([\d.]+) ms"),
    ("trees", r"vgeo: 9 tree species .* in ([\d.]+) ms"),
    ("grass", r"grass: .* in ([\d.]+) ms"),
    ("surfaces", r"surfaces: .* in ([\d.]+) ms"),
    ("terrain", r"terrain: \d+ textures .* in ([\d.]+) ms"),
]


def run(exe, out, tag, args_side):
    log = os.path.join(out, tag + ".log")
    shots = os.path.join(out, tag)
    os.makedirs(shots, exist_ok=True)
    args = [exe, "--test", "--nosound", "--log", log] + args_side.split() + ["--script", CFG]
    bindir = os.path.dirname(exe)   # the working directory the game expects (relative data paths)
    t0 = time.time()
    subprocess.run(args, cwd=bindir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    res = {"tag": tag, "wall_s": time.time() - t0}
    for f in os.listdir(bindir):   # the cfg writes tex_bc_* into the working directory
        if f.startswith("tex_bc_") and (f.endswith(".png") or f.endswith(".json")):
            os.replace(os.path.join(bindir, f), os.path.join(shots, f))
    text = open(log, errors="replace").read() if os.path.exists(log) else ""
    for name, rx in LOADS:
        m = re.findall(rx, text)
        if m:
            res[name] = float(m[-1])
    m = re.findall(r"texc: .*?(\d+) encoded \((\d+) ms\)", text)
    if m:
        res["encoded_sets"], res["encode_ms"] = int(m[-1][0]), int(m[-1][1])
    for scene in ("dev", "veg"):
        p = os.path.join(shots, "tex_bc_%s.json" % scene)
        if not os.path.exists(p):
            continue
        s = json.load(open(p))["series"]
        for k in ("vram_usage_mb", "vram_allocated_mb", "gpu_frame_ms"):
            if s.get(k):
                res["%s_%s" % (scene, k)] = statistics.median(s[k])
    return res


def compare_images(a_dir, b_dir, out):
    try:
        from PIL import Image, ImageChops
    except ImportError:
        print("  (Pillow not installed: no image comparison)")
        return {}
    os.makedirs(out, exist_ok=True)
    rows = {}
    for v in VIEWS:
        pa, pb = os.path.join(a_dir, "tex_bc_%s.png" % v), os.path.join(b_dir, "tex_bc_%s.png" % v)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            continue
        a, b = Image.open(pa).convert("RGB"), Image.open(pb).convert("RGB")
        d = ImageChops.difference(a, b)
        hist = d.histogram()   # 3 x 256
        n = a.size[0] * a.size[1] * 3
        se = sum(hist[c * 256 + i] * i * i for c in range(3) for i in range(256))
        sa = sum(hist[c * 256 + i] * i for c in range(3) for i in range(256))
        mx = max(i for c in range(3) for i in range(256) if hist[c * 256 + i])
        mse = se / n
        rows[v] = {"psnr": 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse), "mean_abs": sa / n, "max_abs": mx,
                   "over8_pct": 100.0 * sum(hist[c * 256 + i] for c in range(3) for i in range(9, 256)) / n}
        d.convert("L").point(lambda x: min(255, x * 8)).save(os.path.join(out, "diff_%s.png" % v))
        w, h = a.size
        pair = Image.new("RGB", (w * 2, h))
        pair.paste(a, (0, 0))
        pair.paste(b, (w, 0))
        pair.save(os.path.join(out, "pair_%s.jpg" % v), quality=90)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(HERE, "..", "..", "build", "bin"))
    ap.add_argument("--out", default="tex_bc_out")
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--a", default="+set r_texture_compress 0", help="side A command line (default RGBA8)")
    ap.add_argument("--b", default="+set r_texture_compress 1", help="side B command line (default BC7)")
    ap.add_argument("--no-warm", action="store_true", help="skip the cache-filling B run")
    a = ap.parse_args()
    exe = os.path.join(os.path.abspath(a.bin), "deadground.exe" if os.name == "nt" else "deadground")
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    runs = []
    if not a.no_warm:
        r = run(exe, out, "B_warm", a.b)
        print("warm-up B: %d sets encoded in %s ms (wall %.0f s)" % (r.get("encoded_sets", 0), r.get("encode_ms", "?"), r["wall_s"]))
    for rep in range(a.reps):
        for side, args in (("A", a.a), ("B", a.b)):
            r = run(exe, out, "%s%d" % (side, rep), args)
            r["side"] = side
            print("%s%d: %s" % (side, rep, ", ".join("%s %.1f" % (k, v) for k, v in r.items() if isinstance(v, float) and k != "wall_s")), flush=True)
            runs.append(r)
    keys = [k for k, _ in LOADS] + ["dev_vram_usage_mb", "dev_vram_allocated_mb", "dev_gpu_frame_ms", "veg_vram_usage_mb", "veg_vram_allocated_mb", "veg_gpu_frame_ms"]
    summary = {}
    print("\nA: %s\nB: %s\n\n| metric (median of %d) | A | B | B - A |\n|---|---|---|---|" % (a.a or "(defaults)", a.b or "(defaults)", a.reps))
    for k in keys:
        va = [r[k] for r in runs if r["side"] == "A" and k in r]
        vb = [r[k] for r in runs if r["side"] == "B" and k in r]
        if not va or not vb:
            continue
        ma, mb = statistics.median(va), statistics.median(vb)
        summary[k] = {"A": va, "B": vb}
        print("| %s | %.1f | %.1f | %+.1f (%+.0f %%) |" % (k, ma, mb, mb - ma, 100.0 * (mb - ma) / ma if ma else 0.0))
    img = compare_images(os.path.join(out, "A0"), os.path.join(out, "B0"), out)
    noise = compare_images(os.path.join(out, "A0"), os.path.join(out, "A1"), os.path.join(out, "noise")) if a.reps >= 2 else {}
    if img:
        print("\n| view | PSNR dB (A0 vs B0) | mean abs | max abs | > 8 levels | A0 vs A1 PSNR |\n|---|---|---|---|---|---|")
        for v, r in img.items():
            nz = "%.2f" % noise[v]["psnr"] if v in noise else "-"
            print("| %s | %.2f | %.3f | %d | %.2f %% | %s |" % (v, r["psnr"], r["mean_abs"], r["max_abs"], r["over8_pct"], nz))
    json.dump({"a": a.a, "b": a.b, "runs": runs, "summary": summary, "images": img, "noise": noise}, open(os.path.join(out, "tex_bc_compare.json"), "w"), indent=1)
    print("\n-> %s" % out)


if __name__ == "__main__":
    sys.exit(main())
