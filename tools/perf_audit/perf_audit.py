#!/usr/bin/env python3
"""Performance audit analyzer (docs/PERF_AUDIT.md). Python 3.8+, standard library only.

  perf_audit.py analyze <profile dir> [--out docs/perf] [--date YYYY-MM-DD] [--no-static] [--note TEXT]
      runtime + static rules -> docs/perf/AUDIT_<date>.md / .json and docs/perf/timeline/<scene>_<level>.html
  perf_audit.py scan                       static rules only (source, shaders, assets), printed
  perf_audit.py verify <verify dir> [--update] [--machine NAME]
      the A / B passes of `perf_verify <PF-id>` -> timing / image / state verdict (verify_result.json);
      --update records the run in docs/perf/fixes.json and regenerates docs/perf/FINDINGS.md
  perf_audit.py findings                   regenerate docs/perf/FINDINGS.md from docs/perf/fixes.json
  perf_audit.py ci <profile dir> [--baseline docs/perf/baseline.json] [--threshold 5] [--write-baseline]
      regression gate: exit 1 when a scene's CPU work or GPU p50 regressed more than threshold %
  perf_audit.py bench <bench dir> [--baseline docs/perf/bench_baseline.json] [--threshold 5] [--gate] [--write-baseline]
      the FPS benchmark (`perf_bench`, map devroom_vegetation): every scene's fps / 1 % low / CPU / GPU,
      the 300 fps verdict, the change against the reference machine's baseline -> <dir>/bench_report.md
  perf_audit.py timeline <profile.json> <out.html>
The tool never changes the engine: it reads profiles, screenshots, sources and assets."""
import argparse
import datetime
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

from pa import findings as F            # noqa: E402
from pa import report, rules_runtime, rules_static, timeline, verify   # noqa: E402
from pa.profile import load_dir, median, pct, Profile   # noqa: E402

REGISTRY = os.path.join(ROOT, "docs", "perf", "fixes.json")


def load_registry():
    if not os.path.exists(REGISTRY):
        return {}
    with open(REGISTRY, encoding="utf-8") as f:
        return json.load(f)


def save_registry(reg):
    with open(REGISTRY, "w", encoding="utf-8", newline="\n") as f:
        json.dump(reg, f, indent=1)
        f.write("\n")


def cmd_analyze(a):
    profiles = []
    for d in a.dirs:
        profiles += load_dir(d) if os.path.isdir(d) else [Profile.load(d)]
    if not profiles and not a.static_only:
        print("analyze: no profiles under %s" % ", ".join(a.dirs))
    if a.symbols:
        resolve_symbols(profiles, a.symbols)
    fs = rules_runtime.run(profiles)
    if not a.no_static:
        fs += rules_static.run(ROOT)
    fs = F.merge_all(fs)
    reg = load_registry()
    status = {}
    for fid, f in reg.items():
        if not fid.startswith("_"):
            for x in fs:
                if x.fix_id == fid:
                    status[x.id] = f.get("status", "implemented")
    notes = list(a.note or [])
    try:
        with open(os.path.join(ROOT, "assets", "perf", "scenes.json"), encoding="utf-8") as f:
            ids = [s["id"] for s in json.load(f)["scenes"]] + ["lan2_host", "lan2_client"]
        have = {p.scene for p in profiles}
        missing = [i for i in ids if i not in have]
        if profiles and missing:
            notes.append("Coverage: no profile for %s (crashed, skipped or not run; see each scene's audit.log)." % ", ".join(missing))
    except (OSError, ValueError, KeyError):
        pass
    date = a.date or datetime.date.today().isoformat()
    os.makedirs(a.out, exist_ok=True)
    md = os.path.join(a.out, "AUDIT_%s.md" % date)
    js = os.path.join(a.out, "AUDIT_%s.json" % date)
    meta = {"date": date, "profiles": len(profiles), "machine": a.machine, "rules_runtime": [r.rid for r in rules_runtime.RULES],
            "rules_static": [r.rid for r in rules_static.RULES]}
    main, trade = report.write_audit(md, js, fs, profiles, meta, notes, status, a.top, reg)
    tdir = os.path.join(a.out, "timeline")
    os.makedirs(tdir, exist_ok=True)
    for p in profiles:
        timeline.write(os.path.join(tdir, "%s_%s%s.html" % (p.scene, p.fidelity, ("_" + p.label.replace(" ", "")) if a.label_in_name else "")), p)
    print("analyze: %d profile(s), %d fidelity-neutral finding(s), %d trade-off(s) -> %s" % (len(profiles), len(main), len(trade), md))
    for i, f in enumerate(main[:a.top], 1):
        print("  %2d. %s %s %-60s %s" % (i, f.id, f.rule, f.title[:60], report.fmt_savings(f.savings)))
    return 0


def resolve_symbols(profiles, exe):
    """Non-Windows profiles name jobs fn_+0x<offset> (no symbols at run time): addr2line -f -e <exe>."""
    import re
    import subprocess
    offs = set()
    for p in profiles:
        for j in p.jobs():
            offs.update(re.findall(r"fn_\+(0x[0-9a-f]+)", j["fn"]))
        for z in p.d.get("zones", []):
            offs.update(re.findall(r"fn_\+(0x[0-9a-f]+)", z["name"]))
    if not offs:
        return
    offs = sorted(offs)
    try:
        out = subprocess.run(["addr2line", "-f", "-e", exe] + offs, capture_output=True, text=True, timeout=60).stdout.split("\n")
    except (OSError, subprocess.SubprocessError):
        print("analyze: addr2line unavailable, job names stay offsets")
        return
    names = {o: "%s (%s)" % (out[2 * i], os.path.basename(out[2 * i + 1].split(":")[0])) for i, o in enumerate(offs) if 2 * i + 1 < len(out) and out[2 * i] != "??"}

    def sub(t):
        return re.sub(r"fn_\+(0x[0-9a-f]+)", lambda m: names.get(m.group(1), m.group(0)), t)
    for p in profiles:
        for j in p.jobs():
            j["fn"] = sub(j["fn"])
        for z in p.d.get("zones", []):
            z["name"] = sub(z["name"])
        p._zones = {(z["name"], z.get("thread", "main")): z for z in p.d.get("zones", [])}


def cmd_scan(a):
    fs = F.ranked(F.merge_all(rules_static.run(ROOT)))
    for f in fs:
        print("%s %s [%s] %s" % (f.id, f.rule, f.fidelity, f.title))
    return 0


def summarize(res):
    t = res["timing"][0] if res["timing"] else {}
    img = "pass" if res["images_pass"] else ("differ (visual trade-off, by design)" if res.get("tradeoff") else "FAIL")
    return "%s: %s %s -> %s (%+.2f%%, p=%.3g); images %s; state %s" % (
        "CLOSED" if res["closed"] else "NOT CLOSED", t.get("metric", "?"), t.get("a_median"), t.get("b_median"), t.get("delta_pct", 0.0), t.get("p_value", 1.0),
        img, res["state"]["verdict"])


def cmd_verify(a):
    reg = load_registry()
    res = verify.verify_dir(a.dir, reg, heat=not a.no_heat)
    print("verify %s: %s" % (res["fix"], summarize(res)))
    for t in res["timing"]:
        print("  timing %-28s A %.4f  B %.4f  (%+.2f%%, p=%.4f) %s" % (t["metric"], t["a_median"], t["b_median"], t["delta_pct"], t["p_value"],
                                                                  "improved" if t["improved"] else "no significant change"))
    for g in res["guards"]:
        print("  guard  %-28s %s" % (g["metric"], "REGRESSED" if g["b_regressed"] else "ok"))
    for i in res["images"]:
        ab = i.get("b_vs_a") or {}
        print("  image  %s %s r%d s%d: %s (max %s, over %.6f, ssim %.5f)" % (i["scene"], i["level"], i["rep"], i["shot"], "pass" if i["pass"] else "FAIL",
                                                                             ab.get("max_diff"), ab.get("over", 0), ab.get("ssim", 0)))
    print("  state  %s" % json.dumps(res["state"]))
    if a.update and res["fix"] in reg:
        e = reg[res["fix"]]
        run = {"date": datetime.date.today().isoformat(), "machine": a.machine, "summary": summarize(res), "closed": res["closed"],
               "timing": res["timing"], "state": res["state"]["verdict"], "images_pass": res["images_pass"], "dir": os.path.relpath(a.dir, ROOT).replace("\\", "/")}
        v = e.setdefault("verification", {"runs": []})
        v.setdefault("runs", []).append(run)
        v["summary"], v["machine"] = run["summary"], a.machine
        if res["closed"] and a.target:
            e["status"] = "closed"
        elif res["closed"]:
            e["status"] = "verified on %s (close on the target machine)" % a.machine
        save_registry(reg)
        report.write_findings_md(os.path.join(ROOT, "docs", "perf", "FINDINGS.md"), reg)
        print("  recorded in docs/perf/fixes.json, FINDINGS.md regenerated")
    return 0 if res["closed"] else 2


def cmd_findings(a):
    report.write_findings_md(os.path.join(ROOT, "docs", "perf", "FINDINGS.md"), load_registry())
    print("docs/perf/FINDINGS.md regenerated")
    return 0


def scene_metrics(profiles):
    out = {}
    for p in profiles:
        out[p.key] = {"cpu_work_ms": median(verify.cpu_work(p)), "gpu_ms": median(p.gpu_frame_ms())}
    return out


def budget_check(profiles, budget_ms):
    """A frame-time budget (docs/PERF_AUDIT.md "Frame budget"): every scene's p99 frame period (without
    the limiter's sleep), CPU work and GPU frame within budget_ms. 300 fps constant = 3.33 ms at p99, not
    on average: a 1-in-5 sim tick frame or a 1-per-second hitch fails it."""
    fail = 0
    print("budget %.2f ms: scene  frame p50 / p95 / p99  |  cpu work p50 / p99  |  gpu p50 / p99" % budget_ms)
    for p in sorted(profiles, key=lambda q: q.key):
        f, w, g = p.cpu_ms(), verify.cpu_work(p), p.gpu_frame_ms()
        worst = max(pct(f, 99), pct(w, 99), pct(g, 99))
        bad = worst > budget_ms
        fail += bad
        print("budget: %-28s %6.2f / %6.2f / %6.2f  |  %6.2f / %6.2f  |  %6.2f / %6.2f  %s" % (
            p.key, median(f), pct(f, 95), pct(f, 99), median(w), pct(w, 99), median(g), pct(g, 99),
            "OVER (x%.2f)" % (worst / budget_ms) if bad else "ok"))
    return fail


def cmd_budget(a):
    fail = budget_check(load_dir(a.dir), a.ms)
    print("budget: %s" % ("FAIL (%d scenes over)" % fail if fail else "pass"))
    return 1 if fail else 0


BENCH_BASELINE = os.path.join(ROOT, "docs", "perf", "bench_baseline.json")


def bench_metrics(profiles):
    """perf_bench profiles -> {scene/level: metrics}, plus TOUR/<level> over every frame of that level.
    fps_avg = frames / time; fps_1low = the fps of the 99th percentile frame time (the "1 % low");
    frame = the frame period (no frame cap in a bench), cpu = CPU work (waits removed), gpu = GPU frame."""
    def m(f, w, g):
        f = [x for x in f if x and x > 0]
        if not f:
            return None
        return {"frames": len(f), "fps_avg": 1000.0 * len(f) / sum(f), "fps_1low": 1000.0 / pct(f, 99), "frame_p50_ms": median(f),
                "frame_p99_ms": pct(f, 99), "cpu_p50_ms": median(w), "cpu_p99_ms": pct(w, 99), "gpu_p50_ms": median(g), "gpu_p99_ms": pct(g, 99)}
    out, pooled = {}, {}
    for p in profiles:
        f, w, g = p.cpu_ms(), verify.cpu_work(p), p.gpu_frame_ms()
        r = m(f, w, g)
        if not r:
            continue
        out[p.key] = r
        t = pooled.setdefault("TOUR/" + p.fidelity, ([], [], []))
        t[0].extend(f), t[1].extend(w), t[2].extend(g)
    for k, (f, w, g) in pooled.items():
        out[k] = m(f, w, g)
    return out


def bench_report(profiles, budget_ms, base=None, threshold=5.0):
    """The table (markdown) and the number of regressions past threshold % (fps avg or 1 % low)."""
    cur = bench_metrics(profiles)
    dev = profiles[0].gpu().get("device", "?") if profiles else "?"
    rw = profiles[0].gpu().get("render_w", 0) if profiles else 0
    rh = profiles[0].gpu().get("render_h", 0) if profiles else 0
    lines = ["# FPS benchmark (devroom_vegetation)", "", "GPU %s, render %d x %d; 1 %% low = the fps of the 99th percentile frame time; "
             "300 fps constant = frame p99 <= %.2f ms." % (dev, rw, rh, budget_ms), ""]
    hdr = "| Scene | fps avg | 1 % low | frame p50 / p99 ms | CPU p50 / p99 ms | GPU p50 / p99 ms | bound | 300 fps |"
    if base:
        hdr += " vs baseline (avg / 1 % low) |"
    lines += [hdr, "|" + "---|" * (hdr.count("|") - 1)]
    regress = 0
    try:   # the tour's order (scenes.json "bench")
        with open(os.path.join(ROOT, "assets", "perf", "scenes.json"), encoding="utf-8") as f:
            tour = json.load(f).get("bench", [])
    except (OSError, ValueError):
        tour = []
    rank = lambda k: (k.startswith("TOUR"), tour.index(k.split("/")[0]) if k.split("/")[0] in tour else len(tour), k)   # noqa: E731
    order = sorted(cur, key=rank)
    for k in order:
        r = cur[k]
        bound = "GPU" if r["gpu_p50_ms"] > 0.85 * r["frame_p50_ms"] else "CPU"
        row = "| %s | %.1f | %.1f | %.2f / %.2f | %.2f / %.2f | %.2f / %.2f | %s | %s |" % (
            ("**%s**" % k) if k.startswith("TOUR") else k, r["fps_avg"], r["fps_1low"], r["frame_p50_ms"], r["frame_p99_ms"], r["cpu_p50_ms"],
            r["cpu_p99_ms"], r["gpu_p50_ms"], r["gpu_p99_ms"], bound, "yes" if r["frame_p99_ms"] <= budget_ms else "no (x%.2f)" % (r["frame_p99_ms"] / budget_ms))
        if base:
            b = base.get(k)
            if b:
                da = 100.0 * (r["fps_avg"] - b["fps_avg"]) / b["fps_avg"]
                dl = 100.0 * (r["fps_1low"] - b["fps_1low"]) / b["fps_1low"]
                bad = da < -threshold or dl < -threshold
                regress += bad
                row += " %+.1f %% / %+.1f %%%s |" % (da, dl, " **REGRESSED**" if bad else "")
            else:
                row += " new |"
        lines.append(row)
    return "\n".join(lines) + "\n", regress, cur


def cmd_bench(a):
    profiles = [p for p in load_dir(a.dir) if p.scene.startswith(a.prefix)]
    if not profiles:
        print("bench: no perf_bench profiles under %s" % a.dir)
        return 1
    base = None
    if not a.write_baseline and os.path.exists(a.baseline):
        with open(a.baseline, encoding="utf-8") as f:
            bj = json.load(f)
        base = bj.get("scenes")
        print("bench: baseline %s (%s, %s, %s)" % (a.baseline, bj.get("date"), bj.get("machine"), bj.get("gpu")))
    text, regress, cur = bench_report(profiles, a.ms, base, a.threshold)
    print(text)
    with open(os.path.join(a.dir, "bench_report.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    if a.write_baseline:
        with open(a.baseline, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"date": datetime.date.today().isoformat(), "machine": a.machine, "gpu": profiles[0].gpu().get("device", "?"),
                       "render": [profiles[0].gpu().get("render_w", 0), profiles[0].gpu().get("render_h", 0)], "scenes": cur}, f, indent=1)
            f.write("\n")
        print("bench: baseline written (%d entries) -> %s" % (len(cur), a.baseline))
        return 0
    if regress:
        print("bench: %d entr%s regressed more than %.1f %% against the baseline" % (regress, "y" if regress == 1 else "ies", a.threshold))
    return 1 if (a.gate and regress) else 0


def cmd_ci(a):
    profiles = load_dir(a.dir)
    cur = scene_metrics(profiles)
    if a.write_baseline:
        with open(a.baseline, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"date": datetime.date.today().isoformat(), "machine": a.machine, "scenes": cur}, f, indent=1)
        print("ci: baseline written (%d scene runs) -> %s" % (len(cur), a.baseline))
        return 0
    if not os.path.exists(a.baseline):
        print("ci: no baseline %s (run with --write-baseline on the CI machine first)" % a.baseline)
        return 1
    with open(a.baseline, encoding="utf-8") as f:
        base = json.load(f)["scenes"]
    fail = 0
    for k, m in sorted(cur.items()):
        b = base.get(k)
        if not b:
            print("ci: %-28s new scene (no baseline)" % k)
            continue
        for key in ("cpu_work_ms", "gpu_ms"):
            if b[key] <= 0:
                continue
            d = 100.0 * (m[key] - b[key]) / b[key]
            bad = d > a.threshold
            fail += bad
            print("ci: %-28s %-12s %8.3f -> %8.3f (%+.1f%%) %s" % (k, key, b[key], m[key], d, "REGRESSION" if bad else "ok"))
    for vd in a.verify or []:
        res = verify.verify_dir(vd, load_registry(), heat=False)
        ok = (res["images_pass"] or res.get("tradeoff")) and res["state"]["pass"]   # a visual trade-off differs by design
        fail += not ok
        print("ci: verify %s: images %s, state %s" % (vd, "pass" if res["images_pass"] else "FAIL", res["state"]["verdict"]))
    if a.budget_ms:
        fail += budget_check(profiles, a.budget_ms)
    print("ci: %s" % ("FAIL (%d)" % fail if fail else "pass"))
    return 1 if fail else 0


def cmd_timeline(a):
    timeline.write(a.out, Profile.load(a.profile))
    print("timeline -> %s" % a.out)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("analyze")
    p.add_argument("dirs", nargs="*", default=[])
    p.add_argument("--out", default=os.path.join(ROOT, "docs", "perf"))
    p.add_argument("--date")
    p.add_argument("--machine", default=os.environ.get("COMPUTERNAME", os.environ.get("HOSTNAME", "unknown")))
    p.add_argument("--note", action="append")
    p.add_argument("--top", type=int, default=25)
    p.add_argument("--no-static", action="store_true")
    p.add_argument("--static-only", action="store_true")
    p.add_argument("--label-in-name", action="store_true")
    p.add_argument("--symbols", help="the engine binary: resolve fn_+0x.. job names with addr2line (non-Windows profiles)")
    p.set_defaults(fn=cmd_analyze)
    p = sub.add_parser("scan")
    p.set_defaults(fn=cmd_scan)
    p = sub.add_parser("verify")
    p.add_argument("dir")
    p.add_argument("--update", action="store_true")
    p.add_argument("--target", action="store_true", help="this machine is the target hardware: a pass closes the finding")
    p.add_argument("--machine", default=os.environ.get("COMPUTERNAME", os.environ.get("HOSTNAME", "unknown")))
    p.add_argument("--no-heat", action="store_true")
    p.set_defaults(fn=cmd_verify)
    p = sub.add_parser("findings")
    p.set_defaults(fn=cmd_findings)
    p = sub.add_parser("ci")
    p.add_argument("dir")
    p.add_argument("--baseline", default=os.path.join(ROOT, "docs", "perf", "baseline.json"))
    p.add_argument("--threshold", type=float, default=5.0)
    p.add_argument("--write-baseline", action="store_true")
    p.add_argument("--machine", default=os.environ.get("COMPUTERNAME", "ci"))
    p.add_argument("--verify", action="append", help="perf_verify output dirs whose image / state diffs must pass")
    p.add_argument("--budget-ms", type=float, default=0.0, help="also fail a scene whose p99 frame / CPU work / GPU exceeds this (3.33 = 300 fps)")
    p.set_defaults(fn=cmd_ci)
    p = sub.add_parser("budget")
    p.add_argument("dir")
    p.add_argument("--ms", type=float, default=1000.0 / 300.0, help="frame budget (default 3.33 ms: 300 fps)")
    p.set_defaults(fn=cmd_budget)
    p = sub.add_parser("bench")
    p.add_argument("dir")
    p.add_argument("--baseline", default=BENCH_BASELINE, help="the reference machine's run (docs/perf/bench_baseline.json)")
    p.add_argument("--threshold", type=float, default=5.0, help="a regression: fps avg or 1 %% low lower by more than this %%")
    p.add_argument("--gate", action="store_true", help="exit 1 on a regression")
    p.add_argument("--write-baseline", action="store_true", help="this run becomes the baseline")
    p.add_argument("--machine", default=os.environ.get("COMPUTERNAME", os.environ.get("HOSTNAME", "unknown")))
    p.add_argument("--prefix", default="dveg_", help="the bench scenes' id prefix")
    p.add_argument("--ms", type=float, default=1000.0 / 300.0)
    p.set_defaults(fn=cmd_bench)
    p = sub.add_parser("timeline")
    p.add_argument("profile")
    p.add_argument("out")
    p.set_defaults(fn=cmd_timeline)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
