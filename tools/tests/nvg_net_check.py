#!/usr/bin/env python3
"""Checks nvg_net.sh's client log: the host's goggles replicate (EF_NVG and EF_NVG_IR seen while down,
cleared after it switched them off). Run: python3 tools/tests/nvg_net_check.py <client log>"""
import re
import sys

REMOTE = re.compile(r"nvg_report: remote slot (\d+) EF_NVG (\d) EF_NVG_IR (\d)")


def main(argv):
    seen = [(int(m.group(2)), int(m.group(3))) for m in (REMOTE.search(l) for l in open(argv[1], encoding="utf-8", errors="replace")) if m]
    fails = []
    if len(seen) < 2:
        fails.append(f"{len(seen)} remote reports, want 2")
    else:
        if seen[0] != (1, 1):
            fails.append(f"while down: EF_NVG / EF_NVG_IR {seen[0]}, want (1, 1)")
        if seen[-1] != (0, 0):
            fails.append(f"after off: {seen[-1]}, want (0, 0)")
    for f in fails:
        print("nvg_net:", f)
    print(f"nvg_net: {len(seen)} reports, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
