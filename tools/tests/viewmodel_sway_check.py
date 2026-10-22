#!/usr/bin/env python3
"""Checks a tools/tests/viewmodel_sway.cfg log (docs/FIRE_ANIM.md "Viewmodel sway"):
  * off: every capture is zero (cl_viewmodel_sway 0 leaves the hold point's own sway);
  * on: turning right trails (yaw left > 0.5 deg, roll clockwise, drag left); strafing right rolls
    into the strafe and shifts left; walking bobs (amplitude ~1); sprinting fully blends the pose
    weight; aimed, the turn is the look sway's ADS share of the hip turn (0.1 .. 0.3).
Usage: python viewmodel_sway_check.py <log>"""
import re
import sys

CAP = re.compile(r"vms_capture (on|off) (\w+)")
TOT = re.compile(r"viewmodel_sway_info: (on|off), total rot (\S+) (\S+) (\S+) deg pos (\S+) (\S+) (\S+) mm, sprint (\S+), bob amp (\S+)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    caps, cur = {}, None
    for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
        m = CAP.search(line)
        if m:
            cur = (m.group(1), m.group(2))
            continue
        m = TOT.search(line)
        if m and cur:
            v = [float(x) for x in m.groups()[1:]]
            caps[cur] = {"rot": v[0:3], "pos": v[3:6], "sprint": v[6], "bob": v[7]}
            cur = None
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    names = ["rest", "turn", "overshoot", "strafe", "walk", "sprint", "ads_rest", "ads_turn"]
    for n in names:
        check(("off", n) in caps and ("on", n) in caps, f"captured {n} off + on")
    if fails:
        return 1
    for n in names:
        c = caps[("off", n)]
        check(max(abs(x) for x in c["rot"] + c["pos"]) == 0.0, f"off {n}: zero")
    on = {n: caps[("on", n)] for n in names}
    t = on["turn"]
    check(t["rot"][1] > 0.5 and t["rot"][2] < -0.2 and t["pos"][0] < -1.0, f"turn right: yaw {t['rot'][1]} roll {t['rot'][2]} drag {t['pos'][0]} mm")
    s = on["strafe"]
    check(s["rot"][2] < -1.5 and s["pos"][0] < -5.0, f"strafe right: roll {s['rot'][2]} x {s['pos'][0]} mm")
    w = on["walk"]
    check(0.8 < w["bob"] < 1.2, f"walk bob amplitude {w['bob']}")
    check(on["sprint"]["sprint"] > 0.99, f"sprint pose weight {on['sprint']['sprint']}")
    r = on["ads_turn"]["rot"][1] / t["rot"][1] if t["rot"][1] else 0
    check(0.1 <= r <= 0.3, f"aimed turn = {r:.2f} x the hip turn")
    check(max(abs(x) for x in on["rest"]["rot"] + on["rest"]["pos"]) < 0.01, "at rest: still")
    print("viewmodel_sway_check:", "FAIL" if fails else "PASS", f"({len(fails)} failures)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
