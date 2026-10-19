#!/usr/bin/env python3
"""Checks a plate_key.cfg log (docs/DAMAGE.md "Armor"): with a plate carrier and three plates, each V
press puts one plate in (0, 50, 100, 150 of 200); with none left V does nothing; without the carrier
the armor is capped at one plate and V with full armor does nothing.
Run: python3 tools/tests/plate_key_check.py <log>"""
import re
import sys

LINE = re.compile(r"slot 0 .* armor (\S+) / (\S+)")
WANT = [(0, 200), (50, 200), (100, 200), (150, 200), (150, 200), (50, 50), (50, 50)]


def main(argv):
    got = []
    for line in open(argv[1], encoding="utf-8", errors="replace"):
        m = LINE.search(line)
        if m:
            got.append((round(float(m.group(1))), round(float(m.group(2)))))
    fails = []
    if len(got) != len(WANT):
        fails.append(f"{len(got)} status lines, want {len(WANT)}")
    for i, (g, w) in enumerate(zip(got, WANT)):
        if g != w:
            fails.append(f"status {i + 1}: armor {g[0]} / {g[1]}, want {w[0]} / {w[1]}")
    for f in fails:
        print("plate_key:", f)
    print(f"plate_key: {len(WANT)} steps, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
