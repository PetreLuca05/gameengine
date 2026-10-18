"""Checks an item_nearplane.cfg log (docs/ITEM_PIPELINE.md): every weapon reported in both states, no
vertex inside the near plane. Exit 1 otherwise.

    cd build/bin && ./deadground --test --nosound --log np.log --script ../../tools/tests/item_nearplane.cfg
    python tools/tests/item_nearplane.py build/bin/np.log
"""
import re
import sys

WEAPONS = ["pistol", "rifle", "shotgun", "crossbow", "bat", "knife", "smg", "lmg", "dmr", "sniper"]


def main(argv):
    text = open(argv[1], errors="replace").read()
    rx = re.compile(r"item_nearplane (\w+) (\w+): (\d+) / (\d+) vertices inside the near plane \(([\d.]+) m\), nearest ([-\d.]+) m")
    seen = {}
    fail = 0
    for m in rx.finditer(text):
        w, state, bad, total, zn, nearest = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), float(m.group(5)), float(m.group(6))
        seen[(w, state)] = bad
        print("%-9s %-4s %6d vertices  nearest %.3f m (near plane %.3f)  %s" % (w, state, total, nearest, zn, "CLIPS %d" % bad if bad else "ok"))
        fail += 1 if bad else 0
    for w in WEAPONS:
        for s in ("hip", "ads"):
            if (w, s) not in seen:
                print("%-9s %-4s missing from the log" % (w, s))
                fail += 1
    print("item_nearplane: %d problem(s)" % fail)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
