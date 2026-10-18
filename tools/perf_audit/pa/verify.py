"""perf_verify analysis (docs/PERF_AUDIT.md "Closing a finding"): the A (fix off) / B (fix on) passes the
engine wrote with `perf_verify <PF-id>` -> timing, image and state verdicts. A finding is closed only
when its metric improved significantly AND every image pair passes AND the state hashes match."""
import json
import math
import os

from . import imgdiff
from .profile import Profile, median, mean

WAITS = ("wait:present", "wait:gpu_fence", "wait:acquire", "wait:fps_cap", "wait:readback")


def cpu_work(p):
    """Main-thread frame time minus the blocking waits (the CPU cost of the frame)."""
    frame = p.d["frames"]["cpu_ms"]
    out = list(frame)
    for w in WAITS:
        z = p.zone(w)
        if z:
            out = [max(0.0, a - b) for a, b in zip(out, z["incl_ms"])]
    return out


def metric_samples(p, metric):
    kind, _, name = metric.partition(":")
    if kind == "cpu_work":
        return cpu_work(p)
    if kind == "gpu_frame":
        return p.gpu_frame_ms()
    if kind == "zone":
        z = p.zone(name)
        return list(z["incl_ms"]) if z else [0.0] * p.frames
    if kind == "zone_all":    # every thread
        v = [0.0] * p.frames
        for z in p.zones():
            if z["name"] == name:
                v = [a + b for a, b in zip(v, z["incl_ms"])]
        return v
    if kind == "scopec":       # scopec:<scope>:<count> (clear_px, draws, barriers, ..): deterministic work
        sname, _, key = name.rpartition(":")
        s = p.gpu_scope(sname)
        return list(s.get(key, [])) if s else [0] * p.frames
    if kind == "scope":
        s = p.gpu_scope(name)
        if not s:
            return [0.0] * p.frames
        return [x for x in s["ms"] if x is not None and x >= 0]
    if kind == "counter":
        return list(p.counter(name))
    if kind == "site":
        n = sum(s["count"] for s in p.alloc_sites() if name in s["site"].replace("\\", "/"))
        return [n / max(1, p.frames)] * p.frames
    raise ValueError("unknown metric " + metric)


def mann_whitney_less(b, a):
    """One-sided p-value that samples b are stochastically smaller than a (normal approximation, ties
    averaged)."""
    if not a or not b:
        return 1.0
    allv = sorted([(v, 0) for v in a] + [(v, 1) for v in b])
    ranks = [0.0] * len(allv)
    i = 0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1][0] == allv[i][0]:
            j += 1
        r = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[k] = r
        i = j + 1
    rb = sum(r for r, (v, g) in zip(ranks, allv) if g == 1)
    n1, n2 = len(b), len(a)
    u = rb - n1 * (n1 + 1) / 2.0
    mu = n1 * n2 / 2.0
    sd = math.sqrt(n1 * n2 * (n1 + n2 + 1) / 12.0)
    if sd == 0:
        return 0.0 if mean(b) < mean(a) else 1.0
    z = (u - mu) / sd
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


def timing_verdict(a_profiles, b_profiles, metric, min_gain=0.0):
    a = [x for p in a_profiles for x in metric_samples(p, metric)]
    b = [x for p in b_profiles for x in metric_samples(p, metric)]
    ma, mb = median(a), median(b)
    pval = mann_whitney_less(b, a)
    deterministic = metric.startswith(("counter", "site", "scopec"))
    improved = (mean(b) < mean(a) - 1e-9) if deterministic else (pval < 0.05 and mb < ma - min_gain)
    return {"metric": metric, "a_median": round(ma, 5), "b_median": round(mb, 5), "a_mean": round(mean(a), 5), "b_mean": round(mean(b), 5),
            "delta": round(mb - ma, 5), "delta_pct": round(100.0 * (mb - ma) / ma, 2) if ma else 0.0, "p_value": round(pval, 5),
            "samples": [len(a), len(b)], "improved": improved}


def state_verdict(a_profiles, b_profiles):
    """Per scene / level (the hashes of different scenes never match): every group must be identical."""
    groups = {}
    for p in a_profiles:
        groups.setdefault(getattr(p, "key", ""), ([], []))[0].append(p)
    for p in b_profiles:
        groups.setdefault(getattr(p, "key", ""), ([], []))[1].append(p)
    if len(groups) <= 1:
        return _state_verdict_one(a_profiles, b_profiles)
    out = {"verdict": "identical", "ticks": 0, "pass": True, "scenes": {}}
    for key in sorted(groups):
        v = _state_verdict_one(*groups[key])
        out["scenes"][key] = v
        out["ticks"] += v.get("ticks", 0)
        if not v["pass"] and out["pass"]:
            out["verdict"] = "%s: %s" % (key, v["verdict"])
            out["pass"] = False
            if "first_diff_tick" in v:
                out["first_diff_tick"] = v["first_diff_tick"]
    return out


def _state_verdict_one(a_profiles, b_profiles):
    def hashes(p):
        s = p.state()
        return s.get("hashes") if isinstance(s, dict) else None
    ha = [hashes(p) for p in a_profiles]
    hb = [hashes(p) for p in b_profiles]
    if not ha or not hb or any(h is None for h in ha + hb):
        return {"verdict": "missing", "pass": False}
    n = min(len(h) for h in ha + hb)
    stable = all(h[:n] == ha[0][:n] for h in ha)
    same = all(h[:n] == ha[0][:n] for h in hb)
    first = next((i for i in range(n) if hb[0][i] != ha[0][i]), None)
    if not stable:   # the same build twice differs: a match or a mismatch of B proves nothing
        return {"verdict": "inconclusive: A runs differ from each other (non-deterministic scene)", "ticks": n, "pass": False, "first_diff_tick": first}
    if same:
        return {"verdict": "identical", "ticks": n, "pass": True}
    return {"verdict": "behaviour changed", "ticks": n, "first_diff_tick": first, "pass": False}


def image_verdict(manifest, heat_dir=None):
    """Pairs A_r / B_r per shot; the A0 / A1 pair (if any) gives the envelope."""
    by = {}
    for ps in manifest["passes"]:
        by.setdefault((ps["scene"], ps["level"]), {}).setdefault(ps["pass"], {})[ps["rep"]] = ps["shots"]
    out = []
    for (scene, level), passes in sorted(by.items()):
        a, b = passes.get("A", {}), passes.get("B", {})
        for rep in sorted(b):
            for k, (pa, pb) in enumerate(zip(a.get(rep, []), b.get(rep, []))):
                if not (os.path.exists(pa) and os.path.exists(pb)):
                    out.append({"scene": scene, "level": level, "rep": rep, "shot": k, "pass": False, "error": "missing shot"})
                    continue
                ia, ib = imgdiff.read_png(pa), imgdiff.read_png(pb)
                heat = os.path.join(heat_dir, "%s_%s_r%d_s%d_diff.png" % (scene, level, rep, k)) if heat_dir else None
                ab = imgdiff.compare(ia, ib, heat_path=heat)
                env = None
                other = [r for r in a if r != rep]
                if other and k < len(a[other[0]]) and os.path.exists(a[other[0]][k]):
                    env = imgdiff.compare(ia, imgdiff.read_png(a[other[0]][k]))
                ok = ab["pass"] or (env is not None and imgdiff.within_envelope(ab, env))
                out.append({"scene": scene, "level": level, "rep": rep, "shot": k, "b_vs_a": ab, "a_vs_a": env, "pass": ok})
    return out


def verify_dir(path, fixes_registry=None, heat=True):
    with open(os.path.join(path, "manifest.json"), encoding="utf-8") as f:
        man = json.load(f)
    fix = man.get("fix", "")
    reg = (fixes_registry or {}).get(fix, {})
    metrics = reg.get("metrics") or ["cpu_work", "gpu_frame"]
    a_prof, b_prof = [], []
    for ps in man["passes"]:
        if not os.path.exists(ps["profile"]):
            continue
        (a_prof if ps["pass"] == "A" else b_prof).append(Profile.load(ps["profile"]))
    timings = [timing_verdict(a_prof, b_prof, m) for m in metrics]
    primary = timings[0] if timings else {"improved": False}
    # Whole-frame guard: the fix must not make the frame slower: significant over the pooled frames
    # (p < 0.05, > 2 %) AND every B pass slower than every A pass (one disturbed pass, a background
    # job or a thermal dip, is not a regression).
    guards = []
    for m in ("cpu_work", "gpu_frame"):
        if a_prof and b_prof:
            t = timing_verdict(b_prof, a_prof, m)   # "A smaller than B" = B regressed
            pa_ = [median(metric_samples(p, m)) for p in a_prof]
            pb_ = [median(metric_samples(p, m)) for p in b_prof]
            consistent = min(pb_) > max(pa_) * 1.02
            guards.append({"metric": m, "b_regressed": t["improved"] and t["delta_pct"] < -2.0 and consistent, "a_median": t["b_median"],
                           "b_median": t["a_median"], "a_pass_medians": [round(x, 4) for x in pa_], "b_pass_medians": [round(x, 4) for x in pb_]})
    state = state_verdict(a_prof, b_prof)
    hd = os.path.join(path, "diff") if heat else None
    if hd:
        os.makedirs(hd, exist_ok=True)
    images = image_verdict(man, hd)
    images_ok = bool(images) and all(i["pass"] for i in images)
    regressed = any(g["b_regressed"] for g in guards)
    # A visual trade-off (fixes.json "tradeoff": what changes on screen) changes the image by design: the
    # image pairs are recorded (and the heat maps written) but do not decide; timing, the state hashes
    # and the guards still do.
    tradeoff = bool(reg.get("tradeoff"))
    closed = bool(primary.get("improved")) and (images_ok or tradeoff) and state["pass"] and not regressed
    res = {"fix": fix, "finding": reg.get("finding"), "closed": closed, "timing": timings, "guards": guards, "state": state,
           "images": images, "images_pass": images_ok, "tradeoff": tradeoff, "passes": {"A": len(a_prof), "B": len(b_prof)}}
    with open(os.path.join(path, "verify_result.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    return res
