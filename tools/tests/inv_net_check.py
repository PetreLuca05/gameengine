#!/usr/bin/env python3
"""Checks the two-client loot race (tools/tests/inv_net.sh, docs/INVENTORY.md "Networking"):
  * conservation: the crate + both players' inventories hold the same items after the race as
    before it (nothing duplicated, nothing lost; rounds and attachments compared by item too; the
    default bag is made and dropped by the backpack rules: not counted);
  * convergence: each client's last dump of its own inventory equals the host's dump of it.
Run: python3 tools/tests/inv_net_check.py <dir with inv_host.log, inv_a.log, inv_b.log>"""
import collections, os, re, sys

ITEM = re.compile(r"uid\s+(\d+)\s+(\S+)\s+x(\d+)\s+in\s+(\d+)")
HEAD = re.compile(r"(inventory|loot): root (\S+), (\d+) items")


def dumps(path):
    """Every tree dump in a log, in order: (kind, {uid: (item, count, parent)})."""
    out, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        h = HEAD.search(line)
        if h:
            cur = (h.group(1), {})
            out.append(cur)
            continue
        m = ITEM.search(line)
        if m and cur is not None:
            cur[1][int(m.group(1))] = (m.group(2), int(m.group(3)), int(m.group(4)))
        elif cur is not None and "uid" not in line:
            cur = None
    return out


def totals(*trees):
    t = collections.Counter()
    for tr in trees:
        for item, count, _ in tr.values():
            if item != "sling_bag":
                t[item] += count
    return t


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    host = dumps(os.path.join(d, "inv_host.log"))
    if len(host) < 6:
        print(f"inv_net_check: FAIL the host log has {len(host)} tree dumps (expected 6)")
        return 1
    before = totals(host[0][1], host[1][1], host[2][1])
    after = totals(host[3][1], host[4][1], host[5][1])
    fails = []
    for item in sorted(set(before) | set(after)):
        if before[item] != after[item]:
            fails.append(f"{item}: {before[item]} before, {after[item]} after")
    for name, idx in (("a", 4), ("b", 5)):
        p = os.path.join(d, f"inv_{name}.log")
        if not os.path.exists(p):
            fails.append(f"no client log {p}")
            continue
        own = [t for k, t in dumps(p) if k == "inventory"]
        if not own:
            fails.append(f"client {name}: no inventory dump")
        elif own[-1] != host[idx][1]:
            fails.append(f"client {name}: its inventory differs from the host's ({len(own[-1])} vs {len(host[idx][1])} items)")
    taken = sum(totals(host[4][1], host[5][1]).values()) - sum(totals(host[0][1], host[1][1]).values())   # dumps: players, loot, loot, players
    for f in fails:
        print("inv_net_check: FAIL", f)
    print(f"inv_net_check: {sum(before.values())} items before / {sum(after.values())} after, {taken} moved to the players, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
