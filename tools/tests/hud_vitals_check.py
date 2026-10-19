#!/usr/bin/env python3
"""Checks hud_vitals.cfg's screenshots (docs/INVENTORY.md "HUD"): in the bottom band of each, the armor
bar's four full plate segments (the theme's info colour, long horizontal runs: the number's glyphs are
short) span a bar centred on the screen (the block is label + bar + number, centred: the bar sits
(label - number) / 2 right of the centre), with four segments.
Run: python3 tools/tests/hud_vitals_check.py <dir with hud_vitals_*.png>"""
import json, os, sys
from PIL import Image

INFO = (110, 160, 226)


def near(p, c, tol=40):
    return all(abs(int(p[i]) - c[i]) <= tol for i in range(3))


def main(argv):
    d = argv[1] if len(argv) > 1 else "."
    here = os.path.dirname(os.path.abspath(__file__))
    lay = json.load(open(os.path.join(here, "..", "..", "assets", "ui", "theme.json"), encoding="utf-8"))["layout"]
    fails, n = [], 0
    for name in ("hud_vitals_16x9.png", "hud_vitals_21x9.png", "hud_vitals_4x3.png"):
        path = os.path.join(d, name)
        if not os.path.exists(path):
            fails.append(f"{name}: missing")
            continue
        im = Image.open(path).convert("RGB")
        W, H = im.size
        px = im.load()
        best = None
        for y in range(int(H * 0.85), H):
            runs, x = [], 0
            while x < W:
                if near(px[x, y], INFO):
                    x0 = x
                    while x < W and near(px[x, y], INFO):
                        x += 1
                    if x - x0 >= 12:
                        runs.append((x0, x))
                x += 1
            if runs and (best is None or len(runs) > len(best[1]) or (len(runs) == len(best[1]) and runs[-1][1] - runs[0][0] > best[1][-1][1] - best[1][0][0])):
                best = (y, runs)
        n += 1
        if not best:
            fails.append(f"{name}: no armor bar found")
            continue
        y, runs = best
        x0, x1 = runs[0][0], runs[-1][1]
        s = H / 1080.0   # the GUI scale (design px at 1080p)
        want = W / 2 + (lay["hud_vitals_label_w"] - lay["hud_vitals_num_w"]) / 2 * s
        centre = (x0 + x1) / 2
        print(f"hud_vitals_check: {name} {W}x{H}: {len(runs)} plate segments at y {y}, bar {x0}..{x1}, centre {centre:.0f} (want {want:.0f})")
        if len(runs) != 4:
            fails.append(f"{name}: {len(runs)} plate segments (want 4)")
        if abs(centre - want) > max(4.0, 0.006 * W):
            fails.append(f"{name}: the armor bar isn't centred ({centre:.0f} vs {want:.0f})")
    for f in fails:
        print("hud_vitals_check: FAIL", f)
    print(f"hud_vitals_check: {n} screenshots, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
