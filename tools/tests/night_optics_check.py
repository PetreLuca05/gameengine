#!/usr/bin/env python3
"""Checks a night_optics.cfg log (docs/NIGHT_OPTICS.md):
  goggles  the ABC settles at night (no whiteout), the own muzzle flash washes the tube out, it recovers;
           at noon the tube protection shuts them off (want 0 at the end)
  thermal  the sensor runs (powered, AGC range finite); the spot meter reads a person warmer than the
           air, a zombie colder than a person, the glass pane near the air (not the person behind it),
           the polished plate colder than the air (the sky it mirrors); at noon the scope still works
  probe    zombie face vs human face as configured (assets/thermal/entities.json)
Run: python3 tools/tests/night_optics_check.py <log>"""
import math
import re
import sys

NVG = re.compile(r"nvg_debug: flux mean (\S+) current (\S+)\s+gain (\S+) .* whiteout (\S+) flicker")
TH = re.compile(r"thermal_debug: AGC (\S+) (\S+) \.\. (\S+) C\s+spot (\S+) C")
SENS = re.compile(r"thermal_debug: sensor (\S+) powered (\d)")
AIR = re.compile(r"thermal_debug: air (\S+) C")
PROBE = re.compile(r"thermal_probe: \d+ (human|zombie) face (\S+) C")
REP = re.compile(r"nvg_report: want (\d)")


def main(argv):
    nvg, th, sens, air, probe, rep = [], [], [], [], {}, []
    for line in open(argv[1], encoding="utf-8", errors="replace"):
        m = NVG.search(line)
        if m: nvg.append(dict(mean=float(m.group(1)), gain=float(m.group(3)), white=float(m.group(4))))
        m = TH.search(line)
        if m: th.append(dict(lo=float(m.group(2)), hi=float(m.group(3)), spot=float(m.group(4))))
        m = SENS.search(line)
        if m: sens.append((m.group(1), int(m.group(2))))
        m = AIR.search(line)
        if m: air.append(float(m.group(1)))
        m = PROBE.search(line)
        if m: probe[m.group(1)] = float(m.group(2))
        m = REP.search(line)
        if m: rep.append(int(m.group(1)))
    fails = []

    def check(c, msg):
        if not c:
            fails.append(msg)

    check(len(nvg) >= 4, f"{len(nvg)} nvg_debug lines, want 4")
    if len(nvg) >= 4:
        check(nvg[0]["white"] < 0.05 and nvg[0]["gain"] > 22.0, f"night: settled, no whiteout ({nvg[0]})")
        check(nvg[1]["white"] > 0.3, f"the muzzle flash washes the tube out ({nvg[1]['white']})")
        check(nvg[2]["white"] < 0.05, f"recovered after the flash ({nvg[2]['white']})")
        check(nvg[3]["white"] > 0.5 or (rep and rep[-1] == 0), f"noon: whiteout / shut off ({nvg[3]['white']})")
    check(rep and rep[-1] == 0, f"noon: the tube protection switched the goggles off (want {rep[-1] if rep else '?'})")
    check(len(th) == 6, f"{len(th)} thermal_debug AGC lines, want 6")
    check(all(s[0] == "on" and s[1] == 1 for s in sens), f"the sensor runs: {sens}")
    if len(th) == 6 and air:
        a = air[0]
        person, zombie, glass, plate, yard, day = th
        for t in th:
            check(all(math.isfinite(x) for x in t.values()), f"finite readings {t}")
        check(person["spot"] > a + 8.0, f"a person reads warm ({person['spot']:.1f} C, air {a:.1f})")
        check(zombie["spot"] < person["spot"] - 4.0, f"a zombie reads colder than a person ({zombie['spot']:.1f} vs {person['spot']:.1f})")
        check(abs(glass["spot"] - a) < 8.0 and glass["spot"] < person["spot"] - 6.0, f"glass shows its own temperature, not the person ({glass['spot']:.1f})")
        check(plate["spot"] < a - 3.0, f"polished metal mirrors the cold sky ({plate['spot']:.1f} C, air {a:.1f})")
        check(day["hi"] > day["lo"] + 1.5, f"noon: the scope still images ({day})")
    if "human" in probe and "zombie" in probe:
        check(probe["human"] - probe["zombie"] > 6.0, f"zombie vs human face: {probe}")
    else:
        fails.append(f"thermal_probe lines missing: {probe}")
    for f in fails:
        print("night_optics:", f)
    print(f"night_optics: {len(nvg)} nvg / {len(th)} thermal readings, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
