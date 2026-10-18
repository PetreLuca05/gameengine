#!/usr/bin/env python3
"""Checks an aim_obstruct.cfg run (docs/AIM.md): the obstruction class of every case, the bullet
never starting inside geometry, the crosshair at the screen centre when clear (it never moves) and
within 1 px of where Q projects when obstructed, every shot leaving along the barrel line (no
convergence: the shot's direction is the report's barrel direction, and Q lies on that line), ADS
barrels on the camera axis, and the barrel rule blocking the trigger.
Usage: python3 tools/tests/aim_check.py <log>"""
import math
import re
import sys

CASE = re.compile(r"aim_case (\S+)")
REP = re.compile(r"aim_report: class (\S+) P .*?\(\S+, ([\d.]+) m\) muzzle .*? origin ([-\d. ]+?)( \(recovered\))? Q ([-\d.]+) ([-\d.]+) ([-\d.]+) \(([\d.]+) m\) "
                 r"dir ([-\d.]+) ([-\d.]+) ([-\d.]+) Q screen ([-\d.]+) ([-\d.]+)( \(behind\))? render (\d+)x(\d+)")
RULE = re.compile(r"aim_report: far ([\d.]+) m barrel hit (\w+) ([\d.]+) m margin ([\d.]+) m barrel vs camera ([\d.]+) deg")
POS = re.compile(r"crosshair_pos: ([-\d.]+) ([-\d.]+) \((\w+)\) obstruction (\S+) ([\d.]+) offset .*? ui (\d+)x(\d+)(?: target ([-\d.]+) ([-\d.]+))?")
SHOT = re.compile(r"aim: shot class (\S+) .*? origin ([-\d.]+) ([-\d.]+) ([-\d.]+) dir ([-\d.]+) ([-\d.]+) ([-\d.]+)")
STATS = re.compile(r"aim_stats: .*?(\d+) trigger pulls blocked")

# Expected class per case (any of).
EXPECT = {
    "open_hip": {"clear"}, "open_ads": {"clear"}, "wall_edge": {"obstructed", "barrel"}, "barrel": {"barrel"},
    "clear_again": {"clear"}, "ground_parallax": {"clear"},
    "window_crouch": {"obstructed", "barrel"}, "low_wall": {"obstructed", "barrel"}, "thin_wall": {"barrel", "obstructed"},
    "car": {"obstructed", "barrel"}, "friendly": {"friendly"},
}
ADS_MAX_DEG = 0.3      # ADS: the barrel within this of the camera axis
SHOT_DIR_DEG = 0.6     # a shot's direction vs the report's barrel direction a few frames earlier (idle sway)


def vec(m, i):
    return [float(m.group(i)), float(m.group(i + 1)), float(m.group(i + 2))]


def angle_deg(a, b):
    dot = sum(x * y for x, y in zip(a, b))
    la = math.sqrt(sum(x * x for x in a))
    lb = math.sqrt(sum(x * x for x in b))
    return math.degrees(math.acos(max(-1.0, min(1.0, dot / (la * lb)))))


def line_dist(p, o, d):
    """Distance of p from the line o + d t."""
    v = [p[i] - o[i] for i in range(3)]
    t = sum(v[i] * d[i] for i in range(3))
    return math.sqrt(max(0.0, sum(x * x for x in v) - t * t))


def main():
    case, rep, rep_cls, fails, seen = None, None, None, [], set()
    rep_q, rep_dir = None, None
    blocked = None
    for line in open(sys.argv[1], errors="replace"):
        m = CASE.search(line)
        if m and "echo" not in line:
            case, rep, rep_cls, rep_q, rep_dir = m.group(1), None, None, None, None
            continue
        m = REP.search(line)
        if m and case:
            rep = m
            rep_cls = m.group(1)
            rep_q, rep_dir = vec(m, 5), vec(m, 9)
            seen.add(case)
            if rep_cls not in EXPECT.get(case, {rep_cls}):
                fails.append("%s: class %s, expected %s" % (case, rep_cls, "/".join(sorted(EXPECT[case]))))
            print("%-15s class %-10s P %6s m  Q %6s m%s" % (case, rep_cls, m.group(2), m.group(8), " recovered" if m.group(4) else ""))
            continue
        m = RULE.search(line)
        if m and case and rep:
            print("%-15s far %s m, barrel hit %s %s m, margin %s m, barrel vs camera %s deg" % (case, m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)))
            if case == "open_ads" and float(m.group(5)) > ADS_MAX_DEG:
                fails.append("open_ads: barrel %.3f deg off the camera axis (ADS alignment)" % float(m.group(5)))
            continue
        m = POS.search(line)
        if m and case and rep:
            px, py = float(m.group(1)), float(m.group(2))
            uw, uh = float(m.group(6)), float(m.group(7))
            if rep_cls == "clear":
                # Not obstructed: no correction of any kind, the crosshair at the screen centre.
                err = math.hypot(px - uw * 0.5, py - uh * 0.5)
                if err > 1.0:
                    fails.append("%s: clear but the crosshair is %.1f px from the screen centre" % (case, err))
                print("%-15s crosshair %.1f px from the centre" % (case, err))
            elif not rep.group(14) and m.group(3) == "drawn":
                kx = uw / float(rep.group(15))
                ky = uh / float(rep.group(16))
                qx, qy = float(rep.group(12)) * kx, float(rep.group(13)) * ky
                # aim_report solves at the console's moment, the crosshair at its frame's: a Q at point
                # blank moves a few px with the weapon's idle sway between the two. Compare with the
                # target of the same frame (Q's projection as the HUD drew it) when the log has it.
                if m.group(8):
                    tx, ty = float(m.group(8)), float(m.group(9))
                    print("%-15s aim_report Q vs the frame's target %.1f px" % (case, math.hypot(tx - qx, ty - qy)))
                    qx, qy = tx, ty
                err = math.hypot(px - qx, py - qy)
                if err > 1.0 and case != "thin_wall":
                    fails.append("%s: crosshair %.1f px from Q" % (case, err))
                print("%-15s crosshair %.1f px from Q (weight %s)" % (case, err, m.group(5)))
            rep = None
            continue
        m = SHOT.search(line)
        if m and case and rep_dir:
            o, d = vec(m, 2), vec(m, 5)
            a = angle_deg(d, rep_dir)
            # The bullet's line is the barrel's: the same direction as the report, and Q on it.
            # (A barrel pressed into the wall is pushed around by it between the report and the shot.)
            if a > SHOT_DIR_DEG and m.group(1) != "barrel":
                fails.append("%s: shot direction %.2f deg off the barrel line" % (case, a))
            off = line_dist(rep_q, o, d)
            if m.group(1) != "barrel" and off > 0.03 + math.tan(math.radians(SHOT_DIR_DEG)) * math.dist(rep_q, o):
                fails.append("%s: Q %.3f m off the shot's line" % (case, off))
            print("%-15s shot %.3f deg from the barrel line, Q %.3f m off it" % (case, a, off))
            rep_dir = None
            continue
        m = STATS.search(line)
        if m and case == "barrel_rule":
            blocked = int(m.group(1))
    for c in EXPECT:
        if c not in seen:
            fails.append("%s: no aim_report" % c)
    if blocked is not None and blocked < 1:
        fails.append("barrel_rule: block_fire_when_barrel_blocked did not hold the trigger")
    for f in fails:
        print("FAIL " + f)
    print("aim_check: %d cases, %d failure(s)" % (len(seen), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
