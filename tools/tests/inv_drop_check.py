#!/usr/bin/env python3
"""Checks an inv_drop.cfg log (docs/INVENTORY.md): the G tap dropped the held rifle and it came back into
the primary slot when picked up; the discarded hiking pack dropped empty (its contents stayed, in the
default bag) and, picked up again, went back on with them.
Run: python3 tools/tests/inv_drop_check.py <log>"""
import re
import sys

ITEM = re.compile(r"uid\s+(\d+)\s+(\S+)\s+x(\d+)\s+in\s+(\d+)\s+(slot \S+|c\d+ \(\d+,\d+\))")
HEAD = re.compile(r"inventory: root (\S+), (\d+) items")


def dumps(path):
    """Every inventory dump: {uid: (item, count, parent, place)}."""
    out, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        if HEAD.search(line):
            cur = {}
            out.append(cur)
            continue
        m = ITEM.search(line) if cur is not None else None
        if m:
            cur[int(m.group(1))] = (m.group(2), int(m.group(3)), int(m.group(4)), m.group(5))
    return out


def worn(d, slot):
    return [it for it, _, _, pl in d.values() if pl == "slot " + slot]


def count(d, item):
    return sum(n for it, n, _, _ in d.values() if it == item)


def inside(d, pack):
    """The items whose parent is the worn pack (by item id)."""
    tops = [u for u, (it, _, _, pl) in d.items() if it == pack and pl == "slot backpack"]
    return sorted(it for it, _, p, _ in d.values() if tops and p == tops[0])


def main(argv):
    ds = dumps(argv[1])
    fails = []
    if len(ds) < 5:
        print(f"inv_drop_check: FAIL {len(ds)} inventory dumps (expected 5)")
        return 1
    before, dropped, back, discarded, again = ds[:5]
    if worn(before, "primary") != ["rifle"]:
        fails.append("the rifle is not in the primary slot before the drop")
    if count(dropped, "rifle"):
        fails.append("G tap: the rifle is still in the inventory")
    if worn(back, "primary") != ["rifle"]:
        fails.append("picked up: the rifle is not back in the primary slot")
    if count(discarded, "backpack_hiking") or worn(discarded, "backpack") != ["sling_bag"]:
        fails.append("discarded: the hiking pack is still carried, or the default bag isn't worn")
    for item, n in (("medkit", 2), ("ammo_rifle", 60), ("scrap", 7)):
        if count(discarded, item) != n:
            fails.append(f"discarded: {item} x{count(discarded, item)} (want {n}: the contents stay)")
        if count(again, item) != n:
            fails.append(f"picked up: {item} x{count(again, item)} (want {n})")
    if worn(again, "backpack") != ["backpack_hiking"]:
        fails.append("picked up: the hiking pack isn't worn again")
    if inside(again, "backpack_hiking") != sorted(["medkit", "ammo_rifle", "scrap"]):
        fails.append(f"picked up: the pack holds {inside(again, 'backpack_hiking')}")
    for f in fails:
        print("inv_drop_check: FAIL", f)
    print(f"inv_drop_check: {len(ds)} dumps, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
