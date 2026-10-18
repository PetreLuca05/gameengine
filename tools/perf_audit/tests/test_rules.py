"""Every detection rule on synthetic profiles: it fires on the pattern it targets and stays quiet on a clean
profile (docs/PERF_AUDIT.md "Adding a rule")."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import synth as S                              # noqa: E402
from pa import rules_runtime as RR, rules_static as RS, findings as F, report   # noqa: E402
from pa.profile import Profile                 # noqa: E402


def run(d):
    return RR.run([Profile(d, "synthetic.json")])


def rules_of(fs):
    return {f.rule for f in fs}


class CleanProfile(unittest.TestCase):
    def test_clean_is_quiet(self):
        d = S.base()
        S.zone(d, "game_frame", 2.0)
        S.scope(d, "opaque", 1.0, draws=100, pipeline_binds=10, fs=2e6, vs=1e5)
        fs = [f for f in run(d) if f.fidelity == "none"]
        self.assertEqual(fs, [], [f.title for f in fs])


class Gpu(unittest.TestCase):
    def test_r01_dead_pass(self):
        d = S.base()
        d["fg"]["dead_passes"] = ["ssr"]
        S.scope(d, "ssr", 0.4, dispatches=2, cs=1e6)
        f = [x for x in run(d) if x.rule == "R01"]
        self.assertEqual(len(f), 1)
        self.assertAlmostEqual(f[0].savings["gpu_ms"], 0.4, places=3)

    def test_r01_clear_only(self):
        d = S.base()
        S.scope(d, "shadow_moon", 0.05, clears=1, clear_px=2048 * 2048, renderings=1)
        f = [x for x in run(d) if x.rule == "R01"]
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0].fix_id, "PF-0001")

    def test_r01_copy_pass_is_not_empty(self):
        d = S.base()
        S.scope(d, "scene_copy", 0.2, copies=1)
        self.assertNotIn("R01", rules_of(run(d)))

    def test_r02_frozen_inputs(self):
        d = S.base()
        S.scope(d, "sky_view", 0.1, dispatches=1, cs=1e5)
        f = [x for x in run(d) if x.rule == "R02"]
        self.assertTrue(f and "every frame" in f[0].title)

    def test_r02_static_cascades(self):
        d = S.base()
        S.scope(d, "shadow_sun", 1.5, draws=400, vs=1e6)
        self.assertIn("static_cascades", {x.key for x in run(d) if x.rule == "R02"})

    def test_r03_named_low_at_full(self):
        d = S.base()
        d["fg"]["resources"] = [{"name": "ao_half", "kind": "transient", "w": 1920, "h": 1080, "bytes": 1920 * 1080 * 4, "bpp": 4, "format": 0, "d": 1,
                                 "mips": 1, "first": 0, "last": 1, "phys": 0}]
        self.assertIn("R03", rules_of(run(d)))

    def test_r04_independent_passes(self):
        d = S.base()
        d["fg"]["resources"] = [{"name": "r%d" % i, "kind": "transient", "w": 64, "h": 64, "bytes": 1, "bpp": 4, "format": 0, "d": 1, "mips": 1, "first": i,
                                 "last": i, "phys": i} for i in range(6)]
        d["fg"]["passes"] = [{"name": "p%d" % i, "post": False, "uses": [{"res": i, "access": "storage", "write": True, "ref": False}]} for i in range(6)]
        S.scope(d, "p0", 0.1, barriers=1, dispatches=1, cs=1)
        self.assertIn("R04", rules_of(run(d)))

    def test_r05_unused_and_stale(self):
        d = S.base()
        d["fg"]["unused_res"] = ["bloom_tmp"]
        d["fg"]["stale_mb"] = 24.0
        keys = {x.key for x in run(d) if x.rule == "R05"}
        self.assertIn("unused:bloom_tmp", keys)
        self.assertIn("stale_pool", keys)

    def test_r24_binding_churn(self):
        d = S.base()
        S.scope(d, "opaque", 1.0, draws=200, pipeline_binds=180, fs=1e6)
        self.assertIn("R24", rules_of(run(d)))

    def test_t01_tradeoff_listed_not_main(self):
        d = S.base()
        S.scope(d, "gtao", 1.2, dispatches=3, cs=2e6)
        t = [x for x in run(d) if x.rule == "T01"]
        self.assertTrue(t and t[0].fidelity == "tradeoff")


class Cpu(unittest.TestCase):
    def test_r06_io_wait(self):
        d = S.base()
        S.zone(d, "wait:io", 0.8, cat="wait")
        f = [x for x in run(d) if x.rule == "R06"]
        self.assertTrue(f and f[0].savings["cpu_ms"] > 0.7)

    def test_r06_gpu_bound_fence_is_classified(self):
        d = S.base()
        d["series"]["gpu_frame_ms"] = [12.0] * S.N
        S.zone(d, "wait:gpu_fence", 3.0, cat="wait")
        f = [x for x in run(d) if x.rule == "R06"][0]
        self.assertEqual(f.savings["cpu_ms"], 0.0)
        self.assertIn("GPU bound", f.title)

    def test_r07_dependency_stall(self):
        d = S.base()
        S.zone(d, "wait:jobs_wait", 0.5, cat="wait")
        self.assertIn("dependency_stalls", {x.key for x in run(d) if x.rule == "R07"})

    def test_r08_hot_alloc_site(self):
        d = S.base()
        d["alloc_sites"] = [{"site": "C:/x/src/render/render.c:118", "zone": "render_end", "count": S.N, "bytes": 64 * S.N, "frames": S.N},
                            {"site": "C:/x/src/assets/assets.c:10", "zone": "", "count": 1, "bytes": 99, "frames": 1}]
        f = [x for x in run(d) if x.rule == "R08"]
        self.assertEqual(len(f), 1)
        self.assertIsNone(f[0].fix_id)

    def test_r10_palettes(self):
        d = S.base()
        S.counter(d, "anim_eval", 50)
        S.counter(d, "skin_palettes", 120)
        self.assertIn("R10", rules_of(run(d)))

    def test_r12_offscreen_client_anim(self):
        d = S.base()
        S.counter(d, "anim_eval", 100)
        S.counter(d, "anim_eval_offscreen", 40)
        S.zone(d, "anim:post_pass", 2.0)
        self.assertIn("R12", rules_of(run(d)))

    def test_r19_logging(self):
        d = S.base()
        S.counter(d, "log_lines", 1)
        self.assertIn("R19", rules_of(run(d)))

    def test_r20_validation_every_frame(self):
        d = S.base()
        S.zone(d, "fg:validate", 0.05)
        f = [x for x in run(d) if x.rule == "R20"]
        self.assertTrue(f and f[0].fix_id == "PF-0005")

    def test_r23_memcpy(self):
        d = S.base()
        S.counter(d, "memcpy_bytes", 32 * 1048576)
        self.assertIn("R23", rules_of(run(d)))

    def test_r26_lookups(self):
        d = S.base()
        S.counter(d, "cvar_find", 40)
        f = [x for x in run(d) if x.rule == "R26"]
        self.assertTrue(f and f[0].fix_id == "PF-0002")

    def test_r27_startup_io(self):
        d = S.base()
        d["startup"] = {"total_ms": 9000.0, "zones": [[0, "assets_init", 100.0, 4000.0, 1], [0, "io", 200.0, 300.0, 2], [0, "io", 900.0, 120.0, 2]]}
        f = [x for x in run(d) if x.rule == "R27"]
        self.assertTrue(f and f[0].evidence["startup"]["io_ms"] == 420.0)


class Net(unittest.TestCase):
    def net(self):
        secs = {k: {"sent": 300, "bytes": 0, "changed": 300} for k in ("header", "local", "spectate", "time", "weather", "vehicles", "entities")}
        secs["vehicles"] = {"sent": 300, "bytes": 300 * 96, "changed": 1}
        fields = {"hp": {"sent": 400, "bytes": 400 * 12, "with_base": 400, "members": {"hp": 400, "max_hp": 0, "armor": 1}}}
        return {"snapshots": 300, "entities_considered": 1000, "entities_written": 500, "entity_header_bytes": 3000, "sections": secs, "fields": fields,
                "messages": [{"type": 6, "reliable": True, "count": 900, "bytes": 900 * 80}]}

    def test_r16(self):
        d = S.base()
        d["net"] = self.net()
        keys = {x.key for x in run(d) if x.rule == "R16"}
        self.assertIn("section:vehicles", keys)
        self.assertIn("member:hp.max_hp", keys)
        self.assertIn("member:hp.armor", keys)
        self.assertNotIn("member:hp.hp", keys)
        self.assertIn("events_unbatched", keys)


class Static(unittest.TestCase):
    def test_static_scan_on_a_fake_tree(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "src", "x"))
            os.makedirs(os.path.join(root, "shaders"))
            os.makedirs(os.path.join(root, "assets", "a"))
            os.makedirs(os.path.join(root, "assets", "b"))
            with open(os.path.join(root, "src", "x", "y.c"), "w") as f:
                f.write('void thing_update(float dt) {\n    char *p = malloc(10);\n    log_info("tick");\n    Cvar *c = cvar_find("r_x");\n}\n'
                        'void thing_init(void) {\n    void *q = malloc(5);\n}\n')
            with open(os.path.join(root, "shaders", "s.frag"), "w") as f:
                f.write("layout(location = 0) out vec4 o_color;\nlayout(location = 1) out vec4 o_unused;\nvoid main() { o_color = vec4(1); }\n")
            blob = os.urandom(300 * 1024)
            for sub in ("a", "b"):
                with open(os.path.join(root, "assets", sub, "big.bin"), "wb") as f:
                    f.write(blob)
            fs = RS.run(root)
            keys = {(x.rule, x.key) for x in fs}
            self.assertIn(("R08", "src/x/y.c:thing_update"), keys)
            self.assertNotIn(("R08", "src/x/y.c:thing_init"), keys)
            self.assertIn(("R19", "src/x/y.c:thing_update"), keys)
            self.assertIn(("R26", "src/x/y.c:thing_update"), keys)
            self.assertIn(("R21", "shaders/s.frag:o_unused"), keys)
            self.assertNotIn(("R21", "shaders/s.frag:o_color"), keys)
            self.assertTrue(any(r == "R22" for r, _ in keys))


class Ranking(unittest.TestCase):
    def test_merge_and_rank(self):
        a = F.Finding("R01", "k", "t", "s", {}, {"a": 1}, {"gpu_ms": 0.5}, "high", "S", "fix", scene="x/modern")
        b = F.Finding("R01", "k", "t", "s", {}, {"b": 2}, {"gpu_ms": 0.9}, "high", "S", "fix", scene="y/low")
        c = F.Finding("R07", "k2", "t", "s", {}, {}, {"cpu_ms": 2.0}, "low", "L", "fix", scene="x/modern")
        m = F.merge_all([a, b, c])
        self.assertEqual(len(m), 2)
        top = F.ranked(m)[0]
        self.assertEqual(top.rule, "R01")
        self.assertAlmostEqual(top.savings["gpu_ms"], 0.9)
        self.assertEqual(sorted(top.scenes), ["x/modern", "y/low"])

    def test_report_writes(self):
        d = S.base()
        S.counter(d, "cvar_find", 40)
        S.scope(d, "gtao", 1.2, dispatches=3, cs=2e6)
        p = Profile(d, "s.json")
        fs = F.merge_all(RR.run([p]))
        with tempfile.TemporaryDirectory() as t:
            main, trade = report.write_audit(os.path.join(t, "a.md"), os.path.join(t, "a.json"), fs, [p], {"date": "x"})
            txt = open(os.path.join(t, "a.md")).read()
            self.assertIn("Top", txt)
            self.assertIn("NOT recommended", txt)
            self.assertTrue(main and trade)


if __name__ == "__main__":
    unittest.main()
