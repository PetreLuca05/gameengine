"""Vista locked frames (docs/STORY.md "Tests"): compares the vista_*.png captures of
tools/tests/vista_frames.cfg against the goldens in docs/story/ch01/frames.

Both are reduced to 320 x 180 (greyscale-free RGB, box filter) and the bottom 14 % (health bar,
hotbar) is masked. A frame passes when the mean absolute difference is under --mean (0..255) and
under 5 % of 16 x 16 px blocks differ by more than --block: lighting drift passes, a missing
landmark, a dark district or a moved skyline does not.

  python tools/tests/vista_check.py <dir with captures> [--update] [--mean 6] [--block 24]
"""
import argparse, glob, os, sys

from PIL import Image, ImageChops, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
GOLD = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "story", "ch01", "frames"))
SIZE = (320, 180)
MASK_Y = int(SIZE[1] * 0.86)


def reduce(path):
    return Image.open(path).convert("RGB").resize(SIZE, Image.BOX).crop((0, 0, SIZE[0], MASK_Y))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--mean", type=float, default=6.0)
    ap.add_argument("--block", type=float, default=24.0)
    a = ap.parse_args()
    caps = sorted(glob.glob(os.path.join(a.dir, "vista_*.png")))
    if not caps:
        print("vista_check: no vista_*.png in %s" % a.dir)
        return 1
    os.makedirs(GOLD, exist_ok=True)
    fails = 0
    for cap in caps:
        name = os.path.basename(cap)
        gold = os.path.join(GOLD, name)
        img = reduce(cap)
        if a.update or not os.path.exists(gold):
            Image.open(cap).convert("RGB").resize(SIZE, Image.BOX).save(gold, optimize=True)
            print("vista_check: %s golden %s" % (name, "updated" if a.update else "created"))
            continue
        ref = reduce(gold)
        diff = ImageChops.difference(img, ref)
        mean = sum(ImageStat.Stat(diff).mean) / 3.0
        bad = total = 0
        for by in range(0, MASK_Y - 15, 16):
            for bx in range(0, SIZE[0] - 15, 16):
                total += 1
                if sum(ImageStat.Stat(diff.crop((bx, by, bx + 16, by + 16))).mean) / 3.0 > a.block:
                    bad += 1
        ok = mean < a.mean and bad <= total * 0.05
        fails += not ok
        print("vista_check: %-26s mean %5.2f, %3d/%d blocks over %.0f  %s" % (name, mean, bad, total, a.block, "ok" if ok else "FAIL"))
    print("vista_check: %d frame(s), %d failure(s)" % (len(caps), fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
