#!/usr/bin/env python3
"""Checks an inv_tab.cfg log (docs/INVENTORY.md "Screen"): Tab opened the inventory, and every later
Tab closed it, whatever was open on it (a selection, the right-click menu, the inspect window, the
pause menu under it, a looted crate).
Run: python3 tools/tests/inv_tab_check.py <log>"""
import re
import sys

LINE = re.compile(r"cam_track (\S+): \d+ frames, inventory (open|closed)")
WANT = {
    "opened": "open",
    "after_select": "closed",
    "context_open": "closed",
    "inspect_open": "closed",
    "from_pause": "closed",
    "crate_open": "open",
    "crate_after_tab": "closed",
}


def main(argv):
    got = {}
    for line in open(argv[1], encoding="utf-8", errors="replace"):
        m = LINE.search(line)
        if m:
            got[m.group(1)] = m.group(2)
    fails = [f"{k}: inventory {got.get(k, 'missing')}, want {v}" for k, v in WANT.items() if got.get(k) != v]
    for f in fails:
        print("inv_tab:", f)
    print(f"inv_tab: {len(WANT)} cases, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
