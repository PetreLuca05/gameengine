"""First-person framing comparison sheets (docs/CAMERA_HOLDPOINT.md "Framing").

  py tools/fp_framing_compare.py <name> [<name> ...]

Reads docs/fp_framing/<name>.png + <name>.json (written in the game by `fp_framing_capture <name>`) and
the reference (the json's "image") and writes docs/fp_framing/<name>_compare.jpg (the captures themselves are
git-ignored): the reference with its
targets, our frame with the measured points, and the two blended 50 %.
"""
import json
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(ROOT, "docs", "fp_framing")
W, H = 960, 540
TARGET, OURS = (255, 220, 40), (60, 230, 255)


def cross(d, p, c, r=10):
    x, y = p[0] * W, p[1] * H
    d.line([(x - r, y), (x + r, y)], fill=c, width=3)
    d.line([(x, y - r), (x, y + r)], fill=c, width=3)


def box(d, p, c, r=6):
    x, y = p[0] * W, p[1] * H
    d.rectangle([x - r, y - r, x + r, y + r], outline=c, width=2)


def marks(d, j, which):
    t = j["targets"]
    if which in ("ref", "both"):
        for k in ("front", "rear", "left_hand"):
            cross(d, t[k], TARGET)
        d.line([(t["exit_x"] * W, H - 16), (t["exit_x"] * W, H)], fill=TARGET, width=4)
    if which in ("ours", "both"):
        for k in ("front", "rear", "left_hand"):
            box(d, j[k], OURS)
        d.line([(j["rear"][0] * W, j["rear"][1] * H), (j["front"][0] * W, j["front"][1] * H)], fill=OURS, width=2)
        if j["exit_x"] >= 0:
            d.line([(j["exit_x"] * W, H - 16), (j["exit_x"] * W, H)], fill=OURS, width=3)


def sheet(name):
    j = json.load(open(os.path.join(DIR, name + ".json"), encoding="utf-8"))
    ref = Image.open(os.path.join(ROOT, j["image"])).convert("RGB").resize((W, H))
    ours = Image.open(os.path.join(DIR, name + ".png")).convert("RGB").resize((W, H))
    blend = Image.blend(ref, ours, 0.5)
    out = Image.new("RGB", (W * 3, H + 40), (20, 20, 24))
    for i, (im, which, label) in enumerate(((ref, "ref", "reference (targets)"), (ours, "ours", "ours (measured)"), (blend, "both", "50 % blend"))):
        im = im.copy()
        marks(ImageDraw.Draw(im), j, which)
        out.paste(im, (i * W, 40))
        ImageDraw.Draw(out).text((i * W + 10, 12), label, fill=(230, 230, 230))
    info = "%s  arms FOV %.1f vertical  world FOV %.1f vertical  angle %.1f (target %.1f)" % (j["item"], j["arms_fov_v"], j["world_fov_v"], j["angle"], j["targets"]["angle"])
    ImageDraw.Draw(out).text((W * 3 - 620, 12), info, fill=(230, 230, 230))
    p = os.path.join(DIR, name + "_compare.jpg")
    out.save(p, quality=88)
    print("wrote", os.path.relpath(p, ROOT))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for n in sys.argv[1:]:
        sheet(n)
