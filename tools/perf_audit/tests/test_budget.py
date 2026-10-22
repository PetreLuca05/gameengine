"""The frame budget gate (perf_audit.py budget / ci --budget-ms): p99, not the average, decides."""
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


def profile(frame_ms, gpu_ms, spike_every=0, spike_ms=0.0):
    d = S.base(frames=100)
    d["frames"]["cpu_ms"] = [spike_ms if spike_every and i % spike_every == 0 else frame_ms for i in range(100)]
    d["series"]["gpu_frame_ms"] = [gpu_ms] * 100
    return Profile(d)


def check(profiles, ms):
    with contextlib.redirect_stdout(io.StringIO()):
        return perf_audit.budget_check(profiles, ms)


class BudgetTest(unittest.TestCase):
    def test_within_budget(self):
        self.assertEqual(check([profile(3.0, 2.9)], 1000.0 / 300.0), 0)

    def test_gpu_over_budget(self):
        self.assertEqual(check([profile(3.0, 3.6)], 1000.0 / 300.0), 1)

    def test_tick_spikes_fail_the_p99(self):
        # 2.5 ms frames on average, but every 5th frame (a 60 Hz sim tick at 300 fps) takes 4 ms
        self.assertEqual(check([profile(2.5, 2.0, spike_every=5, spike_ms=4.0)], 1000.0 / 300.0), 1)

    def test_rare_spike_within_p99(self):
        # one frame in 100 over budget: the p99 is the 99th of 100 sorted frames, still within
        self.assertEqual(check([profile(2.5, 2.0, spike_every=100, spike_ms=9.0)], 1000.0 / 300.0), 0)


if __name__ == "__main__":
    unittest.main()
