#!/usr/bin/env python3
"""Checks hud_vitals.cfg's screenshots (docs/INVENTORY.md "HUD"): the player card sits bottom left. In
the bottom band of each, the armor bar (the theme's info colour) is one long run (no plate segments)
starting a portrait's width right of the safe area's left edge, and the portrait left of it isn't empty.
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
        for y in range(int(H * 0.8), H):
            runs, x = [], 0
            while x < W // 2:
                if near(px[x, y], INFO):
                    x0 = x
                    while x < W and near(px[x, y], INFO):
                        x += 1
                    if x - x0 >= 12:
                        runs.append((x0, x))
                x += 1
            if runs and (best is None or runs[0][1] - runs[0][0] > best[1][0][1] - best[1][0][0]):
                best = (y, runs)
        n += 1
        if not best:
            fails.append(f"{name}: no armor bar found")
            continue
        y, runs = best
        x0, x1 = runs[0]
        s = H / 1080.0   # the GUI scale (design px at 1080p)
        want = (lay["hud_card_bar_w"]) * s
        print(f"hud_vitals_check: {name} {W}x{H}: armor bar {x0}..{x1} at y {y} ({len(runs)} run(s), {x1 - x0} px, want {want:.0f})")
        if len(runs) != 1:
            fails.append(f"{name}: {len(runs)} armor runs (want one bar)")
        if abs((x1 - x0) - want) > max(4.0, 0.02 * want):
            fails.append(f"{name}: the armor bar is {x1 - x0} px (want {want:.0f}: full, one bar)")
        if x0 > W * 0.35:
            fails.append(f"{name}: the armor bar isn't bottom left (x {x0})")
        # The portrait: something drawn left of the bar (the character, not just the game behind).
        pw = int((lay["hud_card_name_h"] + 2 * lay["hud_card_row_h"]) * lay["hud_card_portrait_aspect"] * s)
        gap = int(lay["hud_card_gap"] * s)
        px0 = max(0, x0 - gap - pw)
        cols = set(px[xx, yy] for xx in range(px0, x0 - gap, 2) for yy in range(max(0, y - pw), min(H, y + pw // 3), 2))
        print(f"hud_vitals_check: {name}: portrait {px0}..{x0 - gap}, {len(cols)} colours")
        if len(cols) < 40:
            fails.append(f"{name}: the portrait looks empty ({len(cols)} colours)")
    for f in fails:
        print("hud_vitals_check: FAIL", f)
    print(f"hud_vitals_check: {n} screenshots, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
