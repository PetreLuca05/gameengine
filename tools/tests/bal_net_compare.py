#!/usr/bin/env python3
"""Compares a ballistics LAN run (tools/tests/bal_net.sh): every client-predicted hit must have a host hit with the same shot id within
0.25 m (0.5 m for a later impact of the same bullet: a ricochet or a penetration amplifies a tiny
difference), the host's shots must be replayed on the client, and the client must have scheduled
cracks / reports for the host's bullets."""
import math
import re
import sys

HIT = re.compile(r"ballistics: (host|predict|replay) shot ([0-9a-f]{4}) (\S+) (\S+) at ([\d.]+) m \(([-\d.]+) ([-\d.]+) ([-\d.]+)\)")


def hits(path):
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = HIT.search(line)
            if m:
                out.append((m.group(1), m.group(2), m.group(3), m.group(4), tuple(float(m.group(i)) for i in (6, 7, 8))))
    return out


def main():
    host, client = sys.argv[1], sys.argv[2]
    hh, ch = hits(host), hits(client)
    fails = 0
    seen = {}
    predicted = [h for h in ch if h[0] == "predict"]
    for p in predicted:
        same = [h for h in hh if h[0] == "host" and h[1] == p[1]]
        best = min((sum((a - b) ** 2 for a, b in zip(p[4], h[4])) ** 0.5 for h in same), default=None)
        n = seen.get(p[1], 0)
        seen[p[1]] = n + 1
        ok = best is not None and best <= (0.25 if n == 0 else 0.5)
        fails += not ok
        print(f"predict {p[1]} {p[2]} {p[3]} at {p[4]}: host delta {best if best is not None else 'none'} {'ok' if ok else 'FAIL'}")
    replays = {h[1] for h in ch if h[0] == "replay"}
    host_shots = {h[1] for h in hh if h[0] == "host" and not any(p[1] == h[1] for p in predicted)}
    print(f"host shots {sorted(host_shots)} replayed on the client {sorted(replays)}")
    with open(client, errors="replace") as f:
        text = f.read()
    cracks = len(re.findall(r"ballistics: crack shot", text))
    reports = len(re.findall(r"ballistics: report ", text))
    print(f"client: {len(predicted)} predicted hits, {cracks} cracks, {reports} reports")
    if not predicted:
        print("FAIL: no predicted hits on the client")
        fails += 1
    if host_shots and not replays:
        print("FAIL: the host's shots were not replayed on the client")
        fails += 1
    if host_shots and not reports:
        print("FAIL: no reports scheduled on the client")
        fails += 1
    print("bal_net:", "FAIL" if fails else "ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
