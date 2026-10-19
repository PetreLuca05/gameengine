"""Inventory character preview checks (docs/CHARACTERS.md "Character preview") on the captures and the log
of tools/tests/char_preview.cfg:

  * the first-person view does not change while the preview renders: the mean absolute difference of
    char_preview_fp_off vs _rendered stays within the frame-to-frame noise (off vs off2) + --fp;
  * the preview looks the same at noon, at midnight and in a storm: the mean colour of the character's
    face and chest (the preview is bottom-aligned and centred, its framing fixed) differs by less than
    --rig (0..255) between the three;
  * every char_preview_check line of the log says PASS (the live player untouched by the preview).

  python tools/tests/char_preview_check.py <dir with captures> <log> [--fp 0.5] [--rig 4]
"""
import argparse, os, re, sys

from PIL import Image, ImageChops, ImageStat


def mean_diff(a, b):
    d = ImageChops.difference(a, b)
    return sum(ImageStat.Stat(d).mean) / 3.0


def box_mean(img, box):
    return ImageStat.Stat(img.crop(box)).mean


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("log")
    ap.add_argument("--fp", type=float, default=0.5)
    ap.add_argument("--rig", type=float, default=4.0)
    a = ap.parse_args()
    ok = True

    def load(name):
        return Image.open(os.path.join(a.dir, name + ".png")).convert("RGB")

    off, ren, off2 = load("char_preview_fp_off"), load("char_preview_fp_rendered"), load("char_preview_fp_off2")
    noise, d = mean_diff(off, off2), mean_diff(off, ren)
    good = d <= noise + a.fp
    ok &= good
    print("fp view: off vs rendered %.3f, off vs off2 (noise) %.3f -> %s" % (d, noise, "PASS" if good else "FAIL"))

    imgs = {k: load("char_preview_" + k) for k in ("noon", "midnight", "rain")}
    W, H = imgs["noon"].size
    boxes = {"face": (int(W / 2 - 0.03 * H), int(0.26 * H), int(W / 2 + 0.03 * H), int(0.31 * H)),
             "chest": (int(W / 2 - 0.06 * H), int(0.40 * H), int(W / 2 + 0.06 * H), int(0.55 * H))}
    for name, box in boxes.items():
        means = {k: box_mean(im, box) for k, im in imgs.items()}
        worst = max(abs(means[k][c] - means["noon"][c]) for k in means for c in range(3))
        good = worst < a.rig
        ok &= good
        print("preview %s: %s, worst channel difference %.2f -> %s" % (
            name, ", ".join("%s (%.0f %.0f %.0f)" % (k, *v) for k, v in means.items()), worst, "PASS" if good else "FAIL"))

    text = open(a.log, encoding="utf-8", errors="replace").read()
    checks = re.findall(r"char_preview_check: (PASS|FAIL)", text)
    good = len(checks) >= 3 and all(c == "PASS" for c in checks)
    ok &= good
    print("live player untouched: %s -> %s" % (", ".join(checks) or "no char_preview_check lines", "PASS" if good else "FAIL"))
    print("char_preview: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
