#!/usr/bin/env python3
"""BCn bitstream cross-check (docs/TEXTURE_MIPS.md "Block compression", src/render/bc_codec.c).

Runs test_bc_codec --dds <dir> (every encoded test image as a .dds plus our own decode as
<name>_ref.png, and bc7_modes.dds: every BC7 mode / partition / rotation the encoder writes) and
decodes each .dds with Pillow's independent BCn decoders. BC7 must match bit for bit (the format
fixes its integer interpolation); BC1 / BC3 / BC4 / BC5 within 1 (decoders round their palettes
differently; GPUs decode them at a higher precision).

  python tools/tests/bc_crosscheck.py [--bin build/bin] [--keep <dir>]

Needs Pillow (with numpy for speed when present); skips (exit 0) when Pillow is missing.
"""
import argparse
import os
import subprocess
import sys
import tempfile

try:
    from PIL import Image
except ImportError:
    print("bc_crosscheck: Pillow not installed: skipped")
    sys.exit(0)


def load_rgba(path):
    im = Image.open(path)
    im.load()
    return im.convert("RGBA")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(os.path.dirname(__file__), "..", "..", "build", "bin"))
    ap.add_argument("--keep", default=None, help="write the .dds / .png files here (else a temp dir)")
    a = ap.parse_args()
    exe = os.path.join(a.bin, "test_bc_codec.exe" if os.name == "nt" else "test_bc_codec")
    if not os.path.exists(exe):
        print("bc_crosscheck: %s not built" % exe)
        return 1
    out = a.keep or tempfile.mkdtemp(prefix="bc_cross_")
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([os.path.abspath(exe), "--dds", os.path.abspath(out)], cwd=a.bin, capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "(no output)")
    names = sorted(f[:-4] for f in os.listdir(out) if f.endswith(".dds"))
    if not names:
        print("bc_crosscheck: no .dds written (assets missing? run from a build with the assets copied)")
        return 1
    bad = 0
    for n in names:
        ours = load_rgba(os.path.join(out, n + "_ref.png"))
        try:
            theirs = load_rgba(os.path.join(out, n + ".dds"))
        except Exception as e:  # an unsupported format in this Pillow
            print("  %-24s Pillow cannot decode (%s): skipped" % (n, e))
            continue
        if ours.size != theirs.size:
            print("  %-24s size %s vs %s" % (n, ours.size, theirs.size))
            bad += 1
            continue
        a_px, b_px = ours.tobytes(), theirs.tobytes()
        exact = "bc7" in n
        tol = 0 if exact else 1
        # BC4 / BC5 store fewer channels: compare only those (Pillow fills the rest its own way)
        chans = [0] if "bc4" in n else [0, 1] if "bc5" in n else [0, 1, 2] if "bc1" in n else [0, 1, 2, 3]
        worst, count, first = 0, 0, None
        for i in range(0, len(a_px), 4):
            for c in chans:
                d = abs(a_px[i + c] - b_px[i + c])
                if d > worst:
                    worst = d
                if d > tol:
                    count += 1
                    if first is None:
                        first = (i // 4 % ours.size[0], i // 4 // ours.size[0], c, a_px[i + c], b_px[i + c])
        ok = count == 0
        bad += 0 if ok else 1
        print("  %-24s %s  max diff %d%s" % (n, "ok  " if ok else "FAIL", worst, "" if ok else "  (%d channel values off; first at x %d y %d ch %d: ours %d, Pillow %d)" % ((count,) + first)))
    print("bc_crosscheck: %d / %d images agree with Pillow's decoders (%s)" % (len(names) - bad, len(names), out))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
