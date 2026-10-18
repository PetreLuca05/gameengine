#!/usr/bin/env python3
"""Checks an inv_drop.cfg log (docs/INVENTORY.md): the G tap dropped the held rifle and it came back onto the
hotbar when picked up; the discarded hiking pack came back whole, its contents in the same cells.
Run: python3 tools/tests/inv_drop_check.py <log>"""
import re
import sys

ITEM = re.compile(r"uid\s+(\d+)\s+(\S+)\s+x(\d+)\s+in\s+(\d+)\s+(slot \S+|c\d+ \(\d+,\d+\))")
HEAD = re.compile(r"inventory: root (\S+), (\d+) items")
HOT = re.compile(r"hotbar((?: \d+:\d+)+)")


def dumps(path):
    """Every inventory dump: ({uid: (item, count, parent, place)}, {slot: uid})."""
    out, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        if HEAD.search(line):
            cur = ({}, {})
            out.append(cur)
            continue
        if cur is None:
            continue
        m = ITEM.search(line)
        if m:
            cur[0][int(m.group(1))] = (m.group(2), int(m.group(3)), int(m.group(4)), m.group(5))
            continue
        h = HOT.search(line)
        if h:
            for e in h.group(1).split():
                k, u = e.split(":")
                cur[1][int(k)] = int(u)
            cur = None
    return out


def items(d):
    return sorted((it, n) for it, n, _, _ in d[0].values())


def contents(d, item):
    """What lies in the first instance of item: [(item, count, place)] sorted."""
    tops = [u for u, (it, _, _, _) in d[0].items() if it == item]
    if not tops:
        return None
    return sorted((it, n, pl) for u, (it, n, p, pl) in d[0].items() if p == tops[0])


def main(argv):
    ds = dumps(argv[1])
    fails = []
    if len(ds) < 5:
        print(f"inv_drop_check: FAIL {len(ds)} inventory dumps (expected 5)")
        return 1
    before, dropped, back, discarded, whole = ds[:5]
    bound = lambda d, item: any(d[0].get(u, ("",))[0] == item for u in d[1].values() if u)
    if not bound(before, "rifle"):
        fails.append("the rifle is not on the hotbar before the drop")
    if any(it == "rifle" for it, _ in items(dropped)):
        fails.append("G tap: the rifle is still in the inventory")
    if not any(it == "rifle" for it, _ in items(back)) or not bound(back, "rifle"):
        fails.append("picked up: the rifle is not back on the hotbar")
    if any(it in ("backpack_hiking", "medkit", "ammo_rifle", "scrap") for it, _ in items(discarded)):
        fails.append("discard: part of the pack stayed: %s" % items(discarded))
    c0, c1 = contents(before, "backpack_hiking"), contents(whole, "backpack_hiking")
    if not c0:
        fails.append("the pack held nothing before (setup)")
    elif c0 != c1:
        fails.append("picked up: the pack's contents differ: %s -> %s" % (c0, c1))
    if items(whole) != items(before):
        fails.append("items after the round trip differ: %s vs %s" % (items(before), items(whole)))
    for f in fails:
        print("inv_drop_check: FAIL", f)
    print("inv_drop_check: pack contents %s, %d failure(s)" % (c0, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
