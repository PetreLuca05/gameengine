#!/usr/bin/env python3
"""Checks an nvg_toggle.cfg log (docs/NIGHT_OPTICS.md "Gameplay rules"): 8 without goggles does
nothing; with them it turns them on and ADS stays at 0; off again, ADS works; an empty battery
refuses; nvg_recharge uses a battery_aa and fills it; running empty switches them off.
Run: python3 tools/tests/nvg_check.py <log>"""
import re
import sys

REP = re.compile(r"nvg_report: want (\d) ir (\d) flip (\S+) ads_blocked (\d) ads_t (\S+) item (\d+) \((\S+)\) charge (\S+)")
BAT = re.compile(r"nvg_battery: (\S+) \((\d+) AA")
REC = re.compile(r"nvg_recharge: (.*)")


def main(argv):
    reps, bats, recs = [], [], []
    for line in open(argv[1], encoding="utf-8", errors="replace"):
        m = REP.search(line)
        if m:
            reps.append(dict(want=int(m.group(1)), ads_blocked=int(m.group(4)), ads=float(m.group(5)), item=m.group(7), charge=float(m.group(8))))
        m = BAT.search(line)
        if m:
            bats.append((float(m.group(1)), int(m.group(2))))
        m = REC.search(line)
        if m:
            recs.append(m.group(1).strip())
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    check(len(reps) == 6, f"{len(reps)} nvg_report lines, want 6")
    if len(reps) == 6:
        r = reps
        check(r[0]["want"] == 0 and r[0]["item"] == "-", "1: no goggles: 8 does nothing")
        check(r[1]["want"] == 1 and r[1]["ads_blocked"] == 1 and r[1]["ads"] < 0.01, f"2: on, ADS refused (ads_t {r[1]['ads']})")
        check(r[2]["want"] == 0 and r[2]["ads_blocked"] == 0 and r[2]["ads"] > 0.5, f"3: off, ADS works (ads_t {r[2]['ads']})")
        check(r[3]["want"] == 0 and r[3]["charge"] == 0.0, "4: an empty battery refuses")
        check(r[4]["want"] == 1 and r[4]["charge"] > 0.99, "4: after the swap it turns on")
        check(r[5]["want"] == 0 and r[5]["charge"] == 0.0, "5: empty: switched off")
    check(recs[:1] == ["ok"], f"nvg_recharge: {recs}")
    check(len(bats) >= 3 and bats[1][0] > 0.99 and bats[1][1] == 1, f"battery after the swap: {bats[1:2]} (want full, 1 AA left)")
    for f in fails:
        print("nvg_check:", f)
    print(f"nvg_check: {len(reps)} reports, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
