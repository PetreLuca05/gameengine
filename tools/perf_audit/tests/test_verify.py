"""The fidelity-neutral proof harness: image diff and state hash comparison on a known-neutral change and
a known-visible change, the A/B timing statistics, and a whole perf_verify directory end to end."""
import json
import os
import random
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import synth as S                       # noqa: E402
from pa import imgdiff, verify          # noqa: E402
from pa.profile import Profile          # noqa: E402

W, H = 96, 64


def scene_image(seed=1):
    """A small 'rendered' image: gradients, edges and texture."""
    rnd = random.Random(seed)
    px = bytearray(W * H * 3)
    for y in range(H):
        for x in range(W):
            o = (y * W + x) * 3
            v = (x * 255 // W) ^ (y * 4 & 0x3F)
            px[o] = v & 255
            px[o + 1] = (y * 255 // H)
            px[o + 2] = 128 + (40 if (x // 8 + y // 8) % 2 else -40) + rnd.randint(-3, 3)
    return imgdiff.Image(W, H, px)


def taa_noise(img, seed=7, amount=2, frac=0.0):
    """Known-neutral change: sub-tolerance noise (TAA jitter residue) on a few pixels."""
    rnd = random.Random(seed)
    px = bytearray(img.px)
    for i in range(len(px)):
        if rnd.random() < 0.05:
            px[i] = max(0, min(255, px[i] + rnd.randint(-amount, amount)))
    return imgdiff.Image(img.w, img.h, px)


def exposure(img, ev):
    """Known-visible change: +ev exposure in display space."""
    k = 2.0 ** (ev / 2.2)
    return imgdiff.Image(img.w, img.h, bytearray(min(255, int(round(v * k))) for v in img.px))


class ImageDiff(unittest.TestCase):
    def roundtrip(self, img):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "x.png")
            imgdiff.write_png(p, img)
            return imgdiff.read_png(p)

    def test_png_roundtrip(self):
        a = scene_image()
        b = self.roundtrip(a)
        self.assertEqual(a.px, b.px)

    def test_identical(self):
        a = scene_image()
        r = imgdiff.compare(a, a)
        self.assertTrue(r["pass"])
        self.assertEqual(r["max_diff"], 0)

    def test_known_neutral_change_passes(self):
        a = scene_image()
        r = imgdiff.compare(a, taa_noise(a))
        self.assertTrue(r["pass"], r)
        self.assertLessEqual(r["max_diff"], imgdiff.TOL)

    def test_known_visible_change_fails(self):
        a = scene_image()
        r = imgdiff.compare(a, exposure(a, 0.1))
        self.assertFalse(r["pass"], r)

    def test_small_local_change_fails(self):
        a = scene_image()
        b = imgdiff.Image(a.w, a.h, bytearray(a.px))
        for y in range(10, 18):            # an 8x8 patch that went darker (a missing shadow, a dropped decal)
            for x in range(20, 28):
                o = (y * W + x) * 3
                b.px[o:o + 3] = bytes(max(0, v - 60) for v in b.px[o:o + 3])
        self.assertFalse(imgdiff.compare(a, b)["pass"])

    def test_envelope(self):
        a = scene_image()
        aa = imgdiff.compare(a, taa_noise(a, seed=3, amount=8))      # the same build twice already differs this much
        ab = imgdiff.compare(a, taa_noise(a, seed=4, amount=8))
        self.assertTrue(imgdiff.within_envelope(ab, aa))
        self.assertFalse(imgdiff.within_envelope(imgdiff.compare(a, exposure(a, 0.3)), aa))


class StateAndTiming(unittest.TestCase):
    def prof(self, hashes, cpu=10.0, fixes=0):
        d = S.base()
        d["frames"]["cpu_ms"] = [cpu + 0.01 * (i % 5) for i in range(S.N)]
        d["state"] = {"kind": "replicated-entities-fnv32", "hashes": hashes}
        d["meta"]["fixes_disabled"] = fixes
        return Profile(d, "x.json")

    def test_state_identical(self):
        h = list(range(S.N))
        self.assertEqual(verify.state_verdict([self.prof(h), self.prof(h)], [self.prof(h)])["verdict"], "identical")

    def test_state_changed(self):
        h = list(range(S.N))
        g = h[:10] + [999] + h[11:]
        v = verify.state_verdict([self.prof(h), self.prof(h)], [self.prof(g)])
        self.assertEqual(v["verdict"], "behaviour changed")
        self.assertEqual(v["first_diff_tick"], 10)

    def test_state_unstable(self):
        h = list(range(S.N))
        v = verify.state_verdict([self.prof(h), self.prof([x + 1 for x in h])], [self.prof(h)])
        self.assertFalse(v["pass"])
        self.assertIn("inconclusive", v["verdict"])

    def test_state_per_scene(self):
        # Two scenes in one run: their hashes differ from each other, each matches its own A.
        h, g = list(range(S.N)), [x * 7 for x in range(S.N)]
        pa, pb, qa, qb = self.prof(h), self.prof(h), self.prof(g), self.prof(g)
        for p, sc in ((pa, "one"), (pb, "one"), (qa, "two"), (qb, "two")):
            p.key = sc + "/modern"
        v = verify.state_verdict([pa, qa], [pb, qb])
        self.assertTrue(v["pass"], v)
        qc = self.prof(g[:10] + [999] + g[11:])
        qc.key = "two/modern"
        v = verify.state_verdict([pa, qa], [pb, qc])
        self.assertFalse(v["pass"])
        self.assertIn("behaviour changed", v["verdict"])

    def test_mann_whitney(self):
        a = [10.0 + 0.1 * (i % 7) for i in range(60)]
        b = [9.5 + 0.1 * (i % 7) for i in range(60)]
        self.assertLess(verify.mann_whitney_less(b, a), 0.01)
        self.assertGreater(verify.mann_whitney_less(a, a), 0.3)

    def test_timing_verdict(self):
        A = [self.prof([0] * S.N, cpu=10.0)]
        B = [self.prof([0] * S.N, cpu=9.0)]
        self.assertTrue(verify.timing_verdict(A, B, "cpu_work")["improved"])
        self.assertFalse(verify.timing_verdict(B, A, "cpu_work")["improved"])


class EndToEnd(unittest.TestCase):
    def make_dir(self, t, b_image, b_hash_shift=0, b_cpu=9.0):
        man = {"kind": "verify", "fix": "PF-9999", "frames": S.N, "settle": 1, "shots": 1, "passes": []}
        base = scene_image()
        for rep in range(2):
            for pas in ("A", "B"):
                d = S.base(scene="unit")
                d["frames"]["cpu_ms"] = [(10.0 if pas == "A" else b_cpu) + 0.02 * ((i + rep) % 5) for i in range(S.N)]
                hashes = list(range(S.N))
                if pas == "B" and b_hash_shift:
                    hashes = [h + b_hash_shift for h in hashes]
                d["state"] = {"kind": "x", "hashes": hashes}
                prof = os.path.join(t, "unit_modern_%s%d.json" % (pas, rep))
                with open(prof, "w") as f:
                    json.dump(d, f)
                shot = os.path.join(t, "unit_modern_%s%d_shot0.png" % (pas, rep))
                img = taa_noise(base, seed=rep * 2 + (pas == "B")) if pas == "A" else b_image(base, rep)
                imgdiff.write_png(shot, img)
                man["passes"].append({"scene": "unit", "level": "modern", "pass": pas, "rep": rep, "profile": prof, "shots": [shot]})
        with open(os.path.join(t, "manifest.json"), "w") as f:
            json.dump(man, f)

    def test_neutral_fix_closes(self):
        with tempfile.TemporaryDirectory() as t:
            self.make_dir(t, lambda img, rep: taa_noise(img, seed=50 + rep))
            r = verify.verify_dir(t, {"PF-9999": {"metrics": ["cpu_work"]}})
            self.assertTrue(r["closed"], json.dumps(r)[:600])

    def test_visible_change_does_not_close(self):
        with tempfile.TemporaryDirectory() as t:
            self.make_dir(t, lambda img, rep: exposure(img, 0.1))
            r = verify.verify_dir(t, {"PF-9999": {"metrics": ["cpu_work"]}})
            self.assertFalse(r["images_pass"])
            self.assertFalse(r["closed"])

    def test_behaviour_change_does_not_close(self):
        with tempfile.TemporaryDirectory() as t:
            self.make_dir(t, lambda img, rep: taa_noise(img, seed=50 + rep), b_hash_shift=1)
            r = verify.verify_dir(t, {"PF-9999": {"metrics": ["cpu_work"]}})
            self.assertEqual(r["state"]["verdict"], "behaviour changed")
            self.assertFalse(r["closed"])

    def test_tradeoff_closes_on_timing_and_state(self):
        # a visual trade-off (fixes.json "tradeoff"): the images differ by design, timing + state decide
        with tempfile.TemporaryDirectory() as t:
            self.make_dir(t, lambda img, rep: exposure(img, 0.1))
            r = verify.verify_dir(t, {"PF-9999": {"metrics": ["cpu_work"], "tradeoff": "grass shadows end nearer"}})
            self.assertFalse(r["images_pass"])
            self.assertTrue(r["tradeoff"])
            self.assertTrue(r["closed"], json.dumps(r)[:600])

    def test_tradeoff_still_needs_state(self):
        with tempfile.TemporaryDirectory() as t:
            self.make_dir(t, lambda img, rep: exposure(img, 0.1), b_hash_shift=1)
            self.assertFalse(verify.verify_dir(t, {"PF-9999": {"metrics": ["cpu_work"], "tradeoff": "x"}})["closed"])

    def test_no_speedup_does_not_close(self):
        with tempfile.TemporaryDirectory() as t:
            self.make_dir(t, lambda img, rep: taa_noise(img, seed=50 + rep), b_cpu=10.0)
            self.assertFalse(verify.verify_dir(t, {"PF-9999": {"metrics": ["cpu_work"]}})["closed"])


if __name__ == "__main__":
    unittest.main()
