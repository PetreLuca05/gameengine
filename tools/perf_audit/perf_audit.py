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
from pa.profile import load_dir, median, Profile   # noqa: E402

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
    date = a.date or datetime.date.today().isoformat()
    os.makedirs(a.out, exist_ok=True)
    md = os.path.join(a.out, "AUDIT_%s.md" % date)
    js = os.path.join(a.out, "AUDIT_%s.json" % date)
    meta = {"date": date, "profiles": len(profiles), "machine": a.machine, "rules_runtime": [r.rid for r in rules_runtime.RULES],
            "rules_static": [r.rid for r in rules_static.RULES]}
    main, trade = report.write_audit(md, js, fs, profiles, meta, a.note, status, a.top)
    tdir = os.path.join(a.out, "timeline")
    os.makedirs(tdir, exist_ok=True)
    for p in profiles:
        timeline.write(os.path.join(tdir, "%s_%s%s.html" % (p.scene, p.fidelity, ("_" + p.label.replace(" ", "")) if a.label_in_name else "")), p)
    print("analyze: %d profile(s), %d fidelity-neutral finding(s), %d trade-off(s) -> %s" % (len(profiles), len(main), len(trade), md))
    for i, f in enumerate(main[:a.top], 1):
        print("  %2d. %s %s %-60s %s" % (i, f.id, f.rule, f.title[:60], report.fmt_savings(f.savings)))
    return 0


def cmd_scan(a):
    fs = F.ranked(F.merge_all(rules_static.run(ROOT)))
    for f in fs:
        print("%s %s [%s] %s" % (f.id, f.rule, f.fidelity, f.title))
    return 0


def summarize(res):
    t = res["timing"][0] if res["timing"] else {}
    return "%s: %s %s -> %s (%+.2f%%, p=%.3g); images %s; state %s" % (
        "CLOSED" if res["closed"] else "NOT CLOSED", t.get("metric", "?"), t.get("a_median"), t.get("b_median"), t.get("delta_pct", 0.0), t.get("p_value", 1.0),
        "pass" if res["images_pass"] else "FAIL", res["state"]["verdict"])


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
        ok = res["images_pass"] and res["state"]["pass"]
        fail += not ok
        print("ci: verify %s: images %s, state %s" % (vd, "pass" if res["images_pass"] else "FAIL", res["state"]["verdict"]))
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
    p.set_defaults(fn=cmd_ci)
    p = sub.add_parser("timeline")
    p.add_argument("profile")
    p.add_argument("out")
    p.set_defaults(fn=cmd_timeline)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
