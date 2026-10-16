#!/usr/bin/env python3
"""Spectating determinism / latency check (docs/SPECTATE.md "Tests").

Reads the target's and the observer's logs from tools/tests/spec_net.sh (spec_dump 1):
  spec_dump local    slot S seq N ...   the target's own predicted state after command N
  spec_dump observer slot S seq N ...   the observer's replica after the same command
and compares them field by field per seq: state hashes (whole PlayerSim, movement, weapon,
recoil), the eye, spread, ammo, health, equipped slot, reload / use timers, bone pose hash.
Also checks the observer's spec_stats: measured input -> screen delay against
2 x fakelag + 1 tick, and the largest correction.

usage: spec_compare.py target.log observer.log [--lag 80] [--pop 0.05]
"""
import re
import sys

FIELDS = ["sim", "move", "wpn", "rc", "eye", "spread", "mag", "ammo", "hp", "slot", "reload", "use", "pose"]
DUMP = re.compile(r"spec_dump (local|observer) slot (\d+) seq (\d+) sim (\w+) move (\w+) wpn (\w+) rc (\w+) eye (\S+) (\S+) (\S+) "
                  r"spread (\S+) mag (-?\d+) ammo (-?\d+) hp (\S+) slot (\d+) reload (\S+) use (\S+) pose (\w+)")
STATS = re.compile(r"spec_stats: target (-?\d+) delay measured (\S+) ms estimate (\S+) ms, max correction (\S+) m, rewinds (\d+), "
                   r"corrections > (\S+) m: (\d+), stalls (\d+), catchups (\d+)")


def read(path, who):
    out = {}
    with open(path, errors="replace") as f:
        for line in f:
            m = DUMP.search(line)
            if not m or m.group(1) != who:
                continue
            g = m.groups()
            rec = {"sim": g[3], "move": g[4], "wpn": g[5], "rc": g[6], "eye": (g[7], g[8], g[9]), "spread": g[10], "mag": g[11],
                   "ammo": g[12], "hp": g[13], "slot": g[14], "reload": g[15], "use": g[16], "pose": g[17]}
            out[int(g[2])] = rec   # the last dump of a seq wins (the confirmed state)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    lag = 80.0
    pop = 0.05
    for i, a in enumerate(sys.argv):
        if a == "--lag":
            lag = float(sys.argv[i + 1])
        if a == "--pop":
            pop = float(sys.argv[i + 1])
    if len(args) < 2:
        print(__doc__)
        return 2
    target, observer = read(args[0], "local"), read(args[1], "observer")
    common = sorted(set(target) & set(observer))
    ok = True
    print("spec_compare: %d target ticks, %d observer ticks, %d in common" % (len(target), len(observer), len(common)))
    if not common:
        print("FAIL: no common ticks (spec_dump on both? did the observer spectate the target?)")
        return 1
    same = {f: 0 for f in FIELDS}
    first_bad = {}
    run = best = 0
    for s in common:
        a, b = target[s], observer[s]
        all_same = True
        for f in FIELDS:
            if a[f] == b[f]:
                same[f] += 1
            else:
                all_same = False
                first_bad.setdefault(f, (s, a[f], b[f]))
        run = run + 1 if all_same else 0
        best = max(best, run)
    for f in FIELDS:
        pct = 100.0 * same[f] / len(common)
        extra = ""
        if f in first_bad:
            s, x, y = first_bad[f]
            extra = "   first difference at seq %d: target %s observer %s" % (s, x, y)
        print("  %-7s %6.2f %% identical%s" % (f, pct, extra))
    print("  longest bit-identical run: %d ticks (requirement: 600)" % best)
    if best < 600:
        ok = False
        print("FAIL: fewer than 600 consecutive bit-identical ticks")
    # Latency / corrections from the observer's last stats line.
    stats = None
    with open(args[1], errors="replace") as f:
        for line in f:
            m = STATS.search(line)
            if m:
                stats = m.groups()
    if stats:
        meas, est, maxcorr = float(stats[1]), float(stats[2]), float(stats[3])
        bound = 2.0 * lag + 1000.0 / 60.0
        print("  delay: measured %.1f ms (same clock), estimate %.1f ms, bound 2 x %.0f + 1 tick = %.1f ms -> %s" %
              (meas, est, lag, bound, "PASS" if meas <= bound else "FAIL"))
        print("  largest correction %.3f m (threshold %.3f m) -> %s; rewinds %s, corrections over threshold %s, stalls %s, catch-ups %s" %
              (maxcorr, pop, "PASS" if maxcorr <= pop else "FAIL", stats[4], stats[6], stats[7], stats[8]))
        ok = ok and meas <= bound and maxcorr <= pop
    else:
        ok = False
        print("FAIL: no spec_stats in the observer log")
    print("spec_compare: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
