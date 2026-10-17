#!/usr/bin/env python3
"""Lens vs world: the scope picture-in-picture must show the world with the same brightness and
colour as the main view around it (docs/ATTACHMENTS.md "The PiP pass"). On a screenshot aimed at a
uniform fog wall (tools/tests/scope_world.cfg 4x_fog_match.png), compares the mean colour of a band
just above the horizontal reticle line inside the lens with the same screen rows left and right of
the scope body (the same elevation: the wall is uniform in yaw, so only the elevation matters).
Usage: python3 tools/tests/scope_match.py <png> [max_delta (0..255, default 4)]"""
import sys
from PIL import Image


def mean(img, x0, y0, x1, y1):
    px = img.crop((x0, y0, x1, y1)).getdata()
    n = len(px)
    return [sum(p[i] for p in px) / n for i in range(3)]


def main():
    img = Image.open(sys.argv[1]).convert("RGB")
    lim = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0
    w, h = img.size
    cx, cy = w // 2, h // 2
    sy = h / 900.0
    # 12-36 px above the reticle's horizontal line (screen centre), clear of the vertical post.
    y0, y1 = int(cy - 36 * sy), int(cy - 12 * sy)
    inside = [mean(img, int(cx - 160 * sy), y0, int(cx - 30 * sy), y1), mean(img, int(cx + 30 * sy), y0, int(cx + 160 * sy), y1)]
    outside = [mean(img, int(cx - 640 * sy), y0, int(cx - 380 * sy), y1), mean(img, int(cx + 380 * sy), y0, int(cx + 640 * sy), y1)]
    li = [(a + b) / 2 for a, b in zip(*inside)]
    lo = [(a + b) / 2 for a, b in zip(*outside)]
    d = max(abs(a - b) for a, b in zip(li, lo))
    print("scope_match: lens %.1f %.1f %.1f  world %.1f %.1f %.1f  max channel delta %.1f (limit %.1f) -> %s"
          % (li[0], li[1], li[2], lo[0], lo[1], lo[2], d, lim, "PASS" if d <= lim else "FAIL"))
    return 0 if d <= lim else 1


if __name__ == "__main__":
    sys.exit(main())
