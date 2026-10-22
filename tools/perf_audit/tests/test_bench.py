"""The FPS benchmark report (perf_audit.py bench): fps / 1 % low from the frame periods, the 300 fps verdict,
the tour row and the baseline comparison."""
import contextlib
import io
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import synth as S                       # noqa: E402
import perf_audit                       # noqa: E402
from pa.profile import Profile          # noqa: E402

BUDGET = 1000.0 / 300.0


def profile(scene, frame_ms, gpu_ms, spike_every=0, spike_ms=0.0, level="modern"):
    d = S.base(frames=100, scene=scene, fidelity=level)
    d["frames"]["cpu_ms"] = [spike_ms if spike_every and i % spike_every == 0 else frame_ms for i in range(100)]
    d["series"]["gpu_frame_ms"] = [gpu_ms] * 100
    return Profile(d)


def report(profiles, base=None):
    with contextlib.redirect_stdout(io.StringIO()):
        return perf_audit.bench_report(profiles, BUDGET, base)


class BenchTest(unittest.TestCase):
    def test_fps_and_low(self):
        _, _, m = report([profile("dveg_pine", 4.0, 3.9, spike_every=10, spike_ms=8.0)])
        r = m["dveg_pine/modern"]
        self.assertAlmostEqual(r["fps_avg"], 1000.0 * 100 / (90 * 4.0 + 10 * 8.0), places=3)
        self.assertAlmostEqual(r["fps_1low"], 125.0, places=3)   # p99 = a spike frame

    def test_tour_pools_every_scene(self):
        _, _, m = report([profile("dveg_hall", 2.0, 1.5), profile("dveg_pine", 4.0, 3.9)])
        self.assertEqual(m["TOUR/modern"]["frames"], 200)
        self.assertAlmostEqual(m["TOUR/modern"]["fps_avg"], 1000.0 * 200 / (100 * 2.0 + 100 * 4.0), places=3)

    def test_verdict(self):
        text, _, _ = report([profile("dveg_hall", 3.0, 2.0), profile("dveg_pine", 4.0, 3.9)])
        self.assertIn("| dveg_hall/modern | 333.3 |", text)
        self.assertIn("no (x1.20)", text)

    def test_baseline_regression(self):
        _, _, base = report([profile("dveg_pine", 4.0, 3.9)])
        _, regress, _ = report([profile("dveg_pine", 4.4, 4.3)], base)   # -9 % fps
        self.assertEqual(regress, 2)   # the scene and the tour
        _, regress, _ = report([profile("dveg_pine", 4.1, 4.0)], base)   # -2.4 %: within 5 %
        self.assertEqual(regress, 0)


if __name__ == "__main__":
    unittest.main()
