#!/usr/bin/env python3
"""Compares a ballistics LAN run (tools/tests/bal_net.sh): every client shot must leave from the same
origin along the same barrel direction on the host (aim_debug 2 "aim: shot" / "aim: host p1 shot"
lines, docs/AIM.md) with no barrel claim clamped, every client-predicted hit must have a host hit with the same shot id within
0.25 m (0.5 m for a later impact of the same bullet: a ricochet or a penetration amplifies a tiny
difference), the host's shots must be replayed on the client, and the client must have scheduled
cracks / reports for the host's bullets."""
import math
import re
import sys

SHOT = re.compile(r"aim: (?:host p\d+ )?shot class \S+ .*?origin ([-\d.]+) ([-\d.]+) ([-\d.]+) dir ([-\d.]+) ([-\d.]+) ([-\d.]+)")
HIT = re.compile(r"ballistics: (host|predict|replay) shot ([0-9a-f]{4}) (\S+) (\S+) at ([\d.]+) m \(([-\d.]+) ([-\d.]+) ([-\d.]+)\)")


def hits(path):
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = HIT.search(line)
            if m:
                out.append((m.group(1), m.group(2), m.group(3), m.group(4), tuple(float(m.group(i)) for i in (6, 7, 8))))
    return out


def shots(path, host):
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = SHOT.search(line)
            if m and (("aim: host" in line) == host):
                out.append([float(m.group(i)) for i in range(1, 7)])
    return out


def main():
    host, client = sys.argv[1], sys.argv[2]
    hh, ch = hits(host), hits(client)
    fails = 0
    # The aim model: the client's shots (its own log) against the host's firing of them, in order.
    cs, hs = shots(client, False), shots(host, True)
    for i, c in enumerate(cs):
        if i >= len(hs):
            print(f"FAIL: client shot {i} never fired on the host")
            fails += 1
            continue
        h = hs[i]
        do = sum((c[k] - h[k]) ** 2 for k in range(3)) ** 0.5
        # (the logged unit vectors are rounded: compare their angle, not their dot)
        cross = [c[4] * h[5] - c[5] * h[4], c[5] * h[3] - c[3] * h[5], c[3] * h[4] - c[4] * h[3]]
        da = math.degrees(math.atan2(math.sqrt(sum(x * x for x in cross)), sum(c[k] * h[k] for k in range(3, 6))))
        ok = do <= 0.002 and da <= 0.02
        fails += not ok
        print(f"shot {i}: origin {do * 1000:.1f} mm, direction {da:.4f} deg apart {'ok' if ok else 'FAIL'}")
    # The hold aim (docs/AIM.md "Networking"): honest claims are never clamped by the host.
    with open(host, errors="replace") as f:
        clamps = sum(1 for line in f if "aim: host" in line and "(clamped)" in line)
    print(f"host: {len(hs)} client shots checked, {clamps} barrel claims clamped")
    if clamps:
        print("FAIL: the host clamped honest barrel claims")
        fails += 1
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
