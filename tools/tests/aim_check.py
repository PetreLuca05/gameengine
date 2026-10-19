#!/usr/bin/env python3
"""Checks an aim_obstruct.cfg run (docs/AIM.md) or, with --accuracy, an aim_accuracy.cfg run.

aim_obstruct: the obstruction class of every case; the crosshair EXACTLY at the screen centre
(<= 0.01 px) whenever the barrel line ends at P (clear) and within 1 px of where Q projects when it
does not; every shot leaving along the barrel of the gameplay pose (within 0.02 deg: the drawn gun's
sway / kick never steers it), landing on P in the open; ADS barrels on the camera axis; the wall
push at its maximum (the hold aim faded out, the bullet along the pulled-up barrel, never above
it); the crosshair returning to the centre monotonically; the barrel rule blocking the trigger.

aim_accuracy: every shot (rifle hip, ADS irons, ADS red dot at 2 / 5 / 25 / 100 / 300 m, no spread)
clear, the crosshair exactly centred, the impact on P within max(1 cm, 0.01 % of D) without
ballistics; with ballistics on the impact's height above P matches the zero / drop path within 2 cm
at 25 / 100 m (300 m reported).
Usage: python3 tools/tests/aim_check.py <log> [--accuracy]"""
import json
import math
import os
import re
import sys

CASE = re.compile(r"aim_case (\S+)")
REP = re.compile(r"aim_report: class (\S+) P ([-\d.]+) ([-\d.]+) ([-\d.]+) \(\S+, ([\d.]+) m\) muzzle .*? origin ([-\d. ]+?)( \(recovered\))? Q ([-\d.]+) ([-\d.]+) ([-\d.]+) "
                 r"\(([\d.]+) m\) dir ([-\d.]+) ([-\d.]+) ([-\d.]+) Q screen ([-\d.]+) ([-\d.]+)( \(behind\))? render (\d+)x(\d+)")
RULE = re.compile(r"aim_report: far ([\d.]+) m barrel hit (\w+) ([\d.]+) m margin ([\d.]+) m barrel vs camera ([\d.]+) deg")
HOLD = re.compile(r"aim_report: hold ([\d.]+) deg( \(at the limit\))? miss ([\d.]+) m tol ([\d.]+) m pull ([\d.]+) weight ([\d.]+) max ([\d.]+) deg drawn vs barrel ([\d.]+) deg")
POS = re.compile(r"crosshair_pos: ([-\d.]+) ([-\d.]+) \((\w+)\) obstruction (\S+) ([\d.]+) offset .*? ui (\d+)x(\d+) target ([-\d.]+) ([-\d.]+)")
SHOT = re.compile(r"aim: shot class (\S+) P ([\d.]+) m .*? origin ([-\d.]+) ([-\d.]+) ([-\d.]+) dir ([-\d.]+) ([-\d.]+) ([-\d.]+) pose ([-\d.]+) ([-\d.]+) ([-\d.]+) "
                  r"hold ([\d.]+) deg miss ([\d.]+)")
IMPACT = re.compile(r"aim: shot impact ([-\d.]+) ([-\d.]+) ([-\d.]+) P at ([-\d.]+) ([-\d.]+) ([-\d.]+)")
BAL = re.compile(r"ballistics: host shot \S+ \S+ \S+ at ([\d.]+) m \(([-\d.]+) ([-\d.]+) ([-\d.]+)\)")
STATS = re.compile(r"aim_stats: .*?(\d+) trigger pulls blocked")

# Expected class per case (any of).
EXPECT = {
    "open_hip": {"clear"}, "open_ads": {"clear"}, "wall_edge": {"obstructed", "barrel"}, "barrel": {"barrel"},
    "wall_push_max": {"barrel", "obstructed"}, "wall_push_static": {"barrel", "obstructed"},
    "clear_again": {"clear"}, "partial_clear": {"obstructed", "barrel"}, "partial_clear_back": {"clear"},
    "near_unreached": {"obstructed"}, "ground_ahead": {"clear"},
    "window_crouch": {"obstructed", "barrel"}, "low_wall": {"obstructed", "barrel"}, "thin_wall": {"barrel", "obstructed"},
    "car": {"obstructed", "barrel"}, "friendly": {"friendly"},
    "idle_centre": {"clear"}, "walk_centre": {"clear"}, "recoil_centre": {"clear"},
}
CENTRE_PX = 0.01       # clear: the crosshair this close to the screen centre (exact)
Q_PX = 1.0             # obstructed: the crosshair this close to Q's projection
ADS_MAX_DEG = 0.3      # ADS: the barrel within this of the camera axis
SHOT_DIR_DEG = 0.02    # a shot's direction vs the gameplay pose's barrel (the same bits but for the claim's packing)
PUSH_PITCH = 28.0      # holdpoint.json collision pitch: the barrel at the maximum push
SWAY_SHOWN_DEG = 0.05  # the *_centre cases: the drawn barrel moved at least this far off the gameplay one (proof)
IMPACT_CASES = {"open_hip", "idle_centre", "recoil_centre"}   # head-on panels: the impact itself on P


def reach_tol(far):
    """aim_reach_tol (gameplay/aim.c): max(5 mm, far x tan 0.1 deg)."""
    return max(0.005, far * math.tan(math.radians(0.1)))


def vec(m, i):
    return [float(m.group(i)), float(m.group(i + 1)), float(m.group(i + 2))]


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def angle_deg(a, b):
    dot = sum(x * y for x, y in zip(a, b))
    la = math.sqrt(sum(x * x for x in a))
    lb = math.sqrt(sum(x * x for x in b))
    return math.degrees(math.acos(max(-1.0, min(1.0, dot / (la * lb)))))


def line_dist(p, o, d):
    """Distance of p from the line o + d t."""
    v = sub(p, o)
    t = sum(v[i] * d[i] for i in range(3))
    return math.sqrt(max(0.0, sum(x * x for x in v) - t * t))


def parse(path):
    """Events per case, in log order: ('rep', dict) with the report's lines merged, ('pos', m), ('shot', m), ('bal', m)."""
    cases, order, case, cur, blocked = {}, [], None, None, None
    for line in open(path, errors="replace"):
        m = CASE.search(line)
        if m and "echo" not in line:
            case, cur = m.group(1), None
            if case not in cases:
                cases[case] = []
                order.append(case)
            continue
        if not case:
            continue
        m = REP.search(line)
        if m:
            cur = {"rep": m}
            cases[case].append(("rep", cur))
            continue
        m = RULE.search(line)
        if m and cur is not None:
            cur["rule"] = m
            continue
        m = HOLD.search(line)
        if m and cur is not None:
            cur["hold"] = m
            continue
        m = POS.search(line)
        if m:
            cases[case].append(("pos", m))
            continue
        m = SHOT.search(line)
        if m:
            cases[case].append(("shot", {"m": m, "imp": None}))
            continue
        m = IMPACT.search(line)
        if m:
            for kind, x in reversed(cases[case]):
                if kind == "shot":
                    if x["imp"] is None:
                        x["imp"] = m
                    break
            continue
        m = BAL.search(line)
        if m:
            cases[case].append(("bal", m))
            continue
        m = STATS.search(line)
        if m and case == "barrel_rule":
            blocked = int(m.group(1))
    return cases, order, blocked


def centre_err(pos):
    return math.hypot(float(pos.group(1)) - float(pos.group(6)) * 0.5, float(pos.group(2)) - float(pos.group(7)) * 0.5)


def check_obstruct(path):
    cases, order, blocked = parse(path)
    fails = []
    for case in order:
        ev = cases[case]
        last = None   # the last report of this case
        sway_shown = 0.0
        back = []
        for kind, x in ev:
            if kind == "rep":
                last = x
                m = x["rep"]
                cls = m.group(1)
                if cls not in EXPECT.get(case, {cls}):
                    fails.append("%s: class %s, expected %s" % (case, cls, "/".join(sorted(EXPECT[case]))))
                line = "%-18s class %-10s P %7s m  Q %7s m%s" % (case, cls, m.group(5), m.group(11), " recovered" if m.group(7) else "")
                r, h = x.get("rule"), x.get("hold")
                if r:
                    line += "  barrel vs camera %s deg" % r.group(5)
                if h:
                    line += "  hold %s deg%s miss %s tol %s pull %s drawn %s deg" % (h.group(1), " (limit)" if h.group(2) else "", h.group(3), h.group(4), h.group(5), h.group(8))
                    sway_shown = max(sway_shown, float(h.group(8)))
                    if cls == "clear" and float(h.group(3)) > float(h.group(4)):
                        fails.append("%s: clear but the barrel line misses P by %s m (tolerance %s)" % (case, h.group(3), h.group(4)))
                print(line)
                if case == "open_ads" and r and float(r.group(5)) > ADS_MAX_DEG:
                    fails.append("open_ads: barrel %.3f deg off the camera axis" % float(r.group(5)))
                if case == "wall_push_max" and h:
                    if float(h.group(5)) < 0.99:
                        fails.append("wall_push_max: pull %s (expected >= 0.99)" % h.group(5))
                    if r and abs(float(r.group(5)) - PUSH_PITCH) > 3.0:
                        fails.append("wall_push_max: barrel %.2f deg off the camera ray, the pull pitches it %.0f" % (float(r.group(5)), PUSH_PITCH))
                if case == "wall_push_static" and h and float(h.group(8)) > 0.05:
                    fails.append("wall_push_static: the drawn barrel %s deg off the bullet's (holdpoint_static 1)" % h.group(8))
                if case == "near_unreached" and h:
                    if float(h.group(5)) > 0.05:
                        fails.append("near_unreached: pull %s (expected none)" % h.group(5))
                    if float(h.group(3)) <= float(h.group(4)):
                        fails.append("near_unreached: the line reaches P (miss %s <= %s)" % (h.group(3), h.group(4)))
            elif kind == "pos":
                if case == "partial_clear_back" and last is None:
                    back.append(centre_err(x))
                    continue
                if last is None:
                    continue
                m = last["rep"]
                cls = m.group(1)
                if cls == "clear":
                    err = centre_err(x)
                    if err > CENTRE_PX:
                        fails.append("%s: clear but the crosshair is %.3f px from the screen centre" % (case, err))
                    print("%-18s crosshair %.3f px from the centre" % (case, err))
                    if case == "partial_clear_back":
                        back.append(err)
                elif not m.group(17) and x.group(3) == "drawn":
                    tx, ty = float(x.group(8)), float(x.group(9))
                    err = math.hypot(float(x.group(1)) - tx, float(x.group(2)) - ty)
                    if err > Q_PX:
                        fails.append("%s: crosshair %.2f px from Q" % (case, err))
                    ce = centre_err(x)
                    print("%-18s crosshair %.2f px from Q, %.1f px from the centre (weight %s)" % (case, err, ce, x.group(5)))
                    if case == "partial_clear" and ce < 5.0:
                        fails.append("partial_clear: obstructed but the crosshair only %.1f px from the centre" % ce)
                last = None
            elif kind == "shot":
                im, x = x["imp"], x["m"]
                o, d, pose = vec(x, 3), vec(x, 6), vec(x, 9)
                if im is None:
                    fails.append("%s: a shot without its impact line" % case)
                    continue
                imp, p = vec(im, 1), vec(im, 4)
                a = angle_deg(d, pose)
                if a > SHOT_DIR_DEG:
                    fails.append("%s: shot %.3f deg off the gameplay pose's barrel" % (case, a))
                di = math.dist(imp, p)
                pd = float(x.group(2))
                line = "%-18s shot class %-10s %.4f deg from the pose barrel, impact %.4f m from P (%.2f m)" % (case, x.group(1), a, di, pd)
                if case in IMPACT_CASES and x.group(1) == "clear" and di > max(0.01, 1e-4 * pd):
                    fails.append("%s: the shot lands %.3f m from P" % (case, di))
                # Every clear shot: its line ends at P (on a grazing surface the impact itself can slide).
                if x.group(1) == "clear" and float(x.group(13)) > reach_tol(pd):
                    fails.append("%s: the clear shot's line passes %.4f m from P (tolerance %.4f)" % (case, float(x.group(13)), reach_tol(pd)))
                line += ", line %.4f m from P" % float(x.group(13))
                if case == "wall_push_max":
                    ap = angle_deg(d, sub(p, o))
                    line += ", %.1f deg off the line to P" % ap
                    if ap < 10.0:
                        fails.append("wall_push_max: the shot only %.1f deg off the line to P (it must follow the pulled-up barrel)" % ap)
                print(line)
        if case == "partial_clear_back":
            if len(back) < 4:
                fails.append("partial_clear_back: %d crosshair samples" % len(back))
            else:
                if not (back[0] >= back[1] >= back[2]):
                    fails.append("partial_clear_back: the crosshair's distance to the centre %s is not falling" % ", ".join("%.2f" % b for b in back[:3]))
                if back[-1] > CENTRE_PX:
                    fails.append("partial_clear_back: %.3f px from the centre after 1 s" % back[-1])
            print("%-18s distance to the centre %s px" % (case, " -> ".join("%.2f" % b for b in back)))
        if case in ("idle_centre", "walk_centre", "recoil_centre"):
            print("%-18s the drawn barrel up to %.3f deg off the bullet's" % (case, sway_shown))
            if sway_shown < SWAY_SHOWN_DEG:
                fails.append("%s: the drawn barrel never left the gameplay one (%.3f deg): sway / kick not on" % (case, sway_shown))
    for c in EXPECT:
        if c not in cases:
            fails.append("%s: no aim_report" % c)
    if blocked is not None and blocked < 1:
        fails.append("barrel_rule: block_fire_when_barrel_blocked did not hold the trigger")
    for f in fails:
        print("FAIL " + f)
    print("aim_check: %d cases, %d failure(s)" % (len(order), len(fails)))
    return 1 if fails else 0


def zero_path(d):
    """5.56 M855 from the rifle (900 m/s), zeroed at 100 m: height of the path above the barrel line
    through P at distance d (tests/data/ballistics_ref.json drop, bore line, ICAO)."""
    ref = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "data", "ballistics_ref.json")
    pts = next(c for c in json.load(open(ref))["cases"] if c["cartridge"] == "556_m855")["points"]
    xs = sorted(int(k) for k in pts)
    drop = lambda x: 0.0 if x <= 0 else (pts[str(xs[0])]["drop"] * (x / xs[0]) ** 2 if x <= xs[0] else
                                            next(pts[str(a)]["drop"] + (pts[str(b)]["drop"] - pts[str(a)]["drop"]) * (x - a) / (b - a)
                                                 for a, b in zip(xs, xs[1:]) if a <= x <= b))
    return d * drop(100) / 100.0 - drop(d)


def check_accuracy(path):
    cases, order, _ = parse(path)
    fails, n = [], 0
    for case in order:
        ev = cases[case]
        bal = "_bal" in case
        last, shot = None, None
        for kind, x in ev:
            if kind == "rep":
                last = x
                if x["rep"].group(1) != "clear":
                    fails.append("%s: class %s" % (case, x["rep"].group(1)))
            elif kind == "pos" and last is not None:
                if last["rep"].group(1) == "clear" and centre_err(x) > CENTRE_PX:
                    fails.append("%s: the crosshair %.3f px from the centre" % (case, centre_err(x)))
                last = None
            elif kind == "shot":
                im, x = x["imp"], x["m"]
                shot = (x, im)
                n += 1
                a = angle_deg(vec(x, 6), vec(x, 9))
                if a > SHOT_DIR_DEG:
                    fails.append("%s: shot %.3f deg off the gameplay pose's barrel" % (case, a))
                if im is None:
                    fails.append("%s: a shot without its impact line" % case)
                elif not bal:
                    imp, p = vec(im, 1), vec(im, 4)
                    d, di = float(x.group(2)), math.dist(imp, p)
                    lim = max(0.01, 1e-4 * d)
                    print("%-22s P %7.2f m  impact %.4f m from P (limit %.4f)  hold %s deg" % (case, d, di, lim, x.group(12)))
                    if di > lim:
                        fails.append("%s: impact %.4f m from P" % (case, di))
            elif kind == "bal" and bal and shot is not None and shot[1] is not None:
                p, hit = vec(shot[1], 4), vec(x, 2)
                d = float(shot[0].group(2))
                dy = hit[1] - p[1]
                want = zero_path(d)
                print("%-22s P %7.2f m  impact %+.4f m above P, the zero / drop path %+.4f m (diff %.4f)" % (case, d, dy, want, dy - want))
                if d < 200 and abs(dy - want) > 0.02:
                    fails.append("%s: impact %.3f m above P, the path %.3f m" % (case, dy, want))
                shot = None
    if n == 0:
        fails.append("no shots logged")
    for f in fails:
        print("FAIL " + f)
    print("aim_check --accuracy: %d shots, %d failure(s)" % (n, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(check_accuracy(sys.argv[1]) if "--accuracy" in sys.argv[2:] else check_obstruct(sys.argv[1]))
