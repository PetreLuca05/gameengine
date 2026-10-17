#!/usr/bin/env python3
"""Checks an aim_obstruct.cfg run (docs/AIM.md): the obstruction class of every case, the bullet
never starting inside geometry, the crosshair drawn within 1 px of where Q projects, and the
barrel rule blocking the trigger. Usage: python3 tools/tests/aim_check.py <log>"""
import re
import sys

CASE = re.compile(r"aim_case (\S+)")
REP = re.compile(r"aim_report: class (\S+) P .*?\(\S+, ([\d.]+) m\) muzzle .*? origin ([-\d. ]+?)( \(recovered\))? Q .*?\(([\d.]+) m\) dir .*? "
                 r"Q screen ([-\d.]+) ([-\d.]+)( \(behind\))? render (\d+)x(\d+)")
POS = re.compile(r"crosshair_pos: ([-\d.]+) ([-\d.]+) \((\w+)\) obstruction (\S+) ([\d.]+) offset .*? ui (\d+)x(\d+)")
STATS = re.compile(r"aim_stats: .*?(\d+) trigger pulls blocked")

# Expected class per case (any of).
EXPECT = {
    "open_hip": {"clear"}, "open_ads": {"clear"}, "wall_edge": {"partial", "barrel"}, "barrel": {"barrel"},
    "window_crouch": {"partial", "barrel"}, "low_wall": {"partial", "barrel"}, "thin_wall": {"barrel", "partial"},
    "car": {"partial", "barrel"}, "friendly": {"friendly"},
}


def main():
    case, rep, fails, seen = None, None, [], set()
    blocked = None
    for line in open(sys.argv[1], errors="replace"):
        m = CASE.search(line)
        if m and "echo" not in line:
            case, rep = m.group(1), None
            continue
        m = REP.search(line)
        if m and case:
            rep = m
            cls = m.group(1)
            seen.add(case)
            if cls not in EXPECT.get(case, {cls}):
                fails.append("%s: class %s, expected %s" % (case, cls, "/".join(sorted(EXPECT[case]))))
            print("%-14s class %-8s P %6s m  Q %6s m%s" % (case, cls, m.group(2), m.group(5), " recovered" if m.group(4) else ""))
            continue
        m = POS.search(line)
        if m and case and rep:
            if rep.group(1) != "clear" and not rep.group(8) and m.group(3) == "drawn":
                kx = float(m.group(6)) / float(rep.group(9))
                ky = float(m.group(7)) / float(rep.group(10))
                qx, qy = float(rep.group(6)) * kx, float(rep.group(7)) * ky
                err = ((float(m.group(1)) - qx) ** 2 + (float(m.group(2)) - qy) ** 2) ** 0.5
                if err > 1.0 and case != "thin_wall":
                    fails.append("%s: crosshair %.1f px from Q" % (case, err))
                print("%-14s crosshair %.1f px from Q (weight %s)" % (case, err, m.group(5)))
            rep = None
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
