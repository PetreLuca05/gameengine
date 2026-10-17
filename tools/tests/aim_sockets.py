#!/usr/bin/env python3
"""Aim model data check (docs/AIM.md): every gun in data/weapons has a muzzle socket in its "hold"
block that agrees with the first-person "muzzle" (the same socket in first and third person), lies
in front of the grip along the barrel (-Z), and has a valid "aim" block (obstruct_near, max_range).
Run: python3 tools/tests/aim_sockets.py. Exit 1 on a failure."""
import glob, json, os, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def main():
    fails, n = [], 0
    for path in sorted(glob.glob(os.path.join(ROOT, "data", "weapons", "*.json"))):
        name = os.path.basename(path)
        d = json.load(open(path, encoding="utf-8"))
        if d.get("kind", "hitscan") == "melee":
            continue
        n += 1
        sock = d.get("hold", {}).get("sockets", {}).get("muzzle")
        fp = d.get("muzzle")
        if sock is None:
            fails.append("%s: no hold.sockets.muzzle" % name)
            continue
        if fp is not None and max(abs(a - b) for a, b in zip(fp, sock)) > 0.001:
            fails.append("%s: first-person muzzle %s != hold socket %s" % (name, fp, sock))
        grip = d["hold"]["sockets"].get("grip_r", {}).get("pos", [0, 0, 0])
        if sock[2] > grip[2] - 0.08:
            fails.append("%s: muzzle %s is not in front of the grip %s" % (name, sock, grip))
        aim = d.get("aim", {})
        near, rng = aim.get("obstruct_near", 0.25), aim.get("max_range", d.get("range", 200))
        if not (0.02 <= near <= 1.0):
            fails.append("%s: aim.obstruct_near %s out of 0.02..1" % (name, near))
        if rng < 10:
            fails.append("%s: aim.max_range %s < 10 m" % (name, rng))
    for f in fails:
        print("FAIL " + f)
    print("aim_sockets: %d guns, %d failure(s)" % (n, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
