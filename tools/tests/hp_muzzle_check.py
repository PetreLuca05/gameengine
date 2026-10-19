#!/usr/bin/env python3
"""Checks an hp_muzzle.cfg run (docs/HANDPOSE.md "Muzzle"): the rifle's muzzle socket after each
editor step (typed grab, typed value, tip / bore snaps, undo / redo) to 0.1 mm; one tick after an
edit the game holds the edited socket and its shots leave the marker (the held socket in the world
and the aim claim the UserCmd carries); a mounted suppressor moves the shot origin to its front face
and leaves the socket; the save into handpose_editor_dir changes exactly the hold.sockets.muzzle
line of the shipped file; the screenshots exist.
Usage (from build/bin): python ../../tools/tests/hp_muzzle_check.py t_hp_muzzle.log t_hp_muzzle"""
import math
import os
import re
import sys

STEP = re.compile(r"hp_muzzle_step (\S+)")
VAL = re.compile(r"hpe muzzle: (\S+) ([-\d.]+) ([-\d.]+) ([-\d.]+) \(item space, m\)( edited)?( selected)?; file (.*)$")
WORLD = re.compile(r"hpe muzzle:   world ([-\d.]+) ([-\d.]+) ([-\d.]+)  barrel ([-\d.]+) ([-\d.]+) ([-\d.]+)  effective ([-\d.]+) ([-\d.]+) ([-\d.]+)(?: device (\S+))?")
GAME = re.compile(r"hpe muzzle:   game socket ([-\d.]+) ([-\d.]+) ([-\d.]+)  shoots from ([-\d.]+) ([-\d.]+) ([-\d.]+) \(([\d.]+) mm from the effective marker\)")
CLAIM = re.compile(r"hpe muzzle:   aim claim .*? world ([-\d.]+) ([-\d.]+) ([-\d.]+) \(([\d.]+) mm from the effective marker, (-?\d+) ticks old\)")
AIM = re.compile(r"aim_report: class \S+ P .*? muzzle ([-\d.]+) ([-\d.]+) ([-\d.]+) origin")
SAVED = re.compile(r"hpe: saved muzzle (\[[^\]]*\]) -> (\S+)")

FILE = "data/weapons/02_rifle.json"
EXPECT = {   # item space, m (the socket, not the device)
    "start": (0.0, 0.06, -0.62),
    "moved": (0.0, 0.06, -0.65),        # G -Z 0.03: 3 cm forward along the barrel
    "moved_tick": (0.0, 0.06, -0.65),
    "typed": (0.01, 0.07, -0.6),
    "tip": (0.0, 0.05, -0.52),          # the barrel part's front face
    "typed2": (0.004, 0.062, -0.5305),
    "bore": (0.0, 0.05, -0.5305),       # its centre line, z kept
    "bore_tick": (0.0, 0.05, -0.5305),
    "undo1": (0.004, 0.062, -0.5305),
    "undo2": (0.0, 0.05, -0.52),
    "redo": (0.004, 0.062, -0.5305),
    "device": (0.004, 0.062, -0.5305),
    "device_off": (0.004, 0.062, -0.5305),
    "save": (0.004, 0.062, -0.5305),
}
EDITED = {"start": False, "save": False}   # the others: edited
GAME_STEPS = ("start", "moved_tick", "bore_tick", "device", "device_off")   # a few ticks after the edit
VAL_EPS = 0.00006       # m: the files keep 0.1 mm
SHOT_MM = 1.0           # the held socket in the world vs the marker (the same pose)
CLAIM_MM = 2.0          # the aim claim (last tick's pose, the player's view) vs the marker
AIM_MM = 25.0           # aim_report (the previous tick's claim) vs the shot origin: the editor's body
                        # animation moves the gun up to ~1 cm per tick (a fallback muzzle is 10+ cm off)
SHOTS = ("start", "moved", "bore", "device")


def vec(m, i):
    return tuple(float(m.group(i + k)) for k in range(3))


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def parse(path):
    steps, cur = {}, None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = STEP.search(line)
        if m:
            cur = m.group(1)
            steps[cur] = {"lines": []}
            continue
        if cur is None:
            continue
        s = steps[cur]
        s["lines"].append(line)
        for key, rx in (("val", VAL), ("world", WORLD), ("game", GAME), ("claim", CLAIM), ("aim", AIM), ("saved", SAVED)):
            m = rx.search(line)
            if m:
                s[key] = m   # the last one of the step
    return steps


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    log, out_dir = sys.argv[1], sys.argv[2]
    steps = parse(log)
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)
        return cond

    for name, want in EXPECT.items():
        s = steps.get(name)
        if not check(s is not None and "val" in s, "%s: no step / no hpe muzzle print" % name):
            continue
        v = vec(s["val"], 2)
        check(dist(v, want) <= VAL_EPS, "%s: muzzle %s, expected %s" % (name, v, want))
        check(bool(s["val"].group(5)) == EDITED.get(name, True), "%s: edited flag %s" % (name, bool(s["val"].group(5))))
        print("%-10s muzzle %8.4f %8.4f %8.4f%s" % (name, v[0], v[1], v[2], " edited" if s["val"].group(5) else ""))

    # One tick after an edit: the game's held socket = the edited one (no device), the shots leave the
    # marker, the aim claim agrees.
    for name in GAME_STEPS:
        s = steps.get(name, {})
        if not check("game" in s and "world" in s, "%s: no game-side print (gun not in hand?)" % name):
            continue
        g, w = s["game"], s["world"]
        sock, shot_mm = vec(g, 1), float(g.group(7))
        device = w.group(10)
        if name == "device":
            check(device == "suppressor", "device: the suppressor is not reported on the marker (%s)" % device)
            barrel = vec(w, 4)
            ahead = dot(sub(vec(w, 7), vec(w, 1)), barrel)
            check(ahead > 0.02, "device: the effective muzzle is %.1f mm along the barrel from the socket (expected in front)" % (ahead * 1000))
            check(sock[2] < EXPECT[name][2] - 0.02, "device: the game's socket %s is not the device's front face" % (sock,))
        else:
            check(device is None, "%s: a device is reported (%s)" % (name, device))
            check(dist(sock, EXPECT[name]) <= VAL_EPS, "%s: the game holds %s, the editor %s" % (name, sock, EXPECT[name]))
        check(shot_mm <= SHOT_MM, "%s: the shots leave %.2f mm from the marker" % (name, shot_mm))
        if check("claim" in s, "%s: no aim claim" % name):
            c = s["claim"]
            check(float(c.group(4)) <= CLAIM_MM and int(c.group(5)) <= 2, "%s: the aim claim is %s mm from the marker (%s ticks old)" % (name, c.group(4), c.group(5)))
        print("%-10s game socket %8.4f %8.4f %8.4f  shot %.2f mm  claim %s mm%s" % (name, sock[0], sock[1], sock[2], shot_mm,
                                                                             s["claim"].group(4) if "claim" in s else "-", "  device " + device if device else ""))

    # aim_report (the crosshair solve with the UserCmd's claim): its muzzle is the game's shot origin,
    # up to the claim's age (built from the previous tick's pose, while the body animation moves the
    # gun up to ~1 cm per tick). The exact check is the same-tick one above (claim = marker).
    for name in ("start", "moved_tick"):
        s = steps.get(name, {})
        if check("aim" in s and "game" in s, "%s: no aim_report" % name):
            d = dist(vec(s["aim"], 1), vec(s["game"], 4)) * 1000
            check(d <= AIM_MM, "%s: aim_report's muzzle is %.1f mm from the game's shot origin" % (name, d))
            print("%-10s aim_report muzzle %.1f mm from the shot origin" % (name, d))

    for name, verb in (("undo1", "undo"), ("undo2", "undo"), ("redo", "redo")):
        check(any("hpe: %s (rifle: muzzle)" % verb in l for l in steps.get(name, {}).get("lines", [])), "%s: no '%s (rifle: muzzle)'" % (name, verb))

    # The save: into handpose_editor_dir, exactly the hold.sockets.muzzle line of the shipped file.
    s = steps.get("save", {})
    if check("saved" in s, "save: no 'saved muzzle'"):
        check(s["saved"].group(1) == "[0.004, 0.062, -0.5305]", "save: wrote %s" % s["saved"].group(1))
        check(s["saved"].group(2).replace("\\", "/").startswith(out_dir), "save: went to %s, not %s/" % (s["saved"].group(2), out_dir))
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", FILE)
    dst = os.path.join(out_dir, FILE)
    if check(os.path.exists(dst), "save: %s missing" % dst):
        a = open(src, encoding="utf-8", newline="").read().split("\n")
        b = open(dst, encoding="utf-8", newline="").read().split("\n")
        diff = [i for i in range(max(len(a), len(b))) if i >= len(a) or i >= len(b) or a[i] != b[i]]
        sockets = next((i for i, l in enumerate(a) if '"sockets"' in l), -1)
        line = next((i for i in range(sockets + 1, len(a)) if '"muzzle"' in a[i]), -1) if sockets >= 0 else -1
        ok = check(len(diff) == 1 and diff[0] == line, "save: lines %s differ (expected only %d, hold.sockets.muzzle)" % ([d + 1 for d in diff], line + 1))
        if ok:
            check('"muzzle": [0.004, 0.062, -0.5305]' in b[line] and b[line].split('"')[0] == a[line].split('"')[0],
                  "save: the muzzle line is '%s'" % b[line].strip())
            print("save       %s line %d: %s" % (dst, line + 1, b[line].strip()))

    for shot in SHOTS:
        png = "t_hp_muzzle_%s.png" % shot
        paths = [png, os.path.join(os.path.dirname(os.path.abspath(log)), png)]
        check(any(os.path.exists(p) and os.path.getsize(p) > 10000 for p in paths), "screenshot %s missing" % png)

    for f in fails:
        print("FAIL", f)
    print("hp_muzzle: %s (%d failures)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
