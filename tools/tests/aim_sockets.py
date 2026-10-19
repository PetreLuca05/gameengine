#!/usr/bin/env python3
"""Aim model data check (docs/AIM.md): every gun in data/weapons has a muzzle socket in its "hold"
block that agrees with the first-person "muzzle" (the same socket in first and third person), lies
in front of the grip along the barrel (-Z), and has a valid "aim" block (obstruct_near, obstruct_margin(_rel), max_range).
The hold aim: every gun's hip and crouch hold (assets/human/holdpoint.json + the weapon's own
"holdpoint" override) reaches an aim point 2 m ahead within wpn_hold_aim_max (12 deg) with its longest
allowed muzzle device fitted (the same fixed-point solve as aim_hold_solve, gameplay/aim.c).
Run: python3 tools/tests/aim_sockets.py. Exit 1 on a failure."""
import glob, json, math, os, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HOLD_AIM_MAX_DEG = 12.0
REACH_D = 2.0


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qaxis(axis, ang):
    s = math.sin(ang * 0.5)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(ang * 0.5))


def qrot(q, v):
    x, y, z, w = q
    t = (2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0]))
    return (v[0] + w * t[0] + y * t[2] - z * t[1], v[1] + w * t[1] + z * t[0] - x * t[2], v[2] + w * t[2] + x * t[1] - y * t[0])


def hp_euler_deg(e):
    """holdpoint.h hp_euler_deg: (pitch, yaw, roll) degrees -> quat_euler(yaw, pitch, roll)."""
    r = math.radians
    return qmul(qmul(qaxis((0, 1, 0), r(e[1])), qaxis((1, 0, 0), r(e[0]))), qaxis((0, 0, 1), r(e[2])))


def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def scale(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def norm(a): return scale(a, 1.0 / math.sqrt(dot(a, a)))


def from_to_rot(a, b, v):
    """v turned by the shortest rotation taking unit a onto unit b."""
    c = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    q = norm4((c[0], c[1], c[2], 1.0 + dot(a, b)))
    return qrot(q, v)


def norm4(q):
    l = math.sqrt(sum(x * x for x in q))
    return tuple(x / l for x in q)


def hold_aim_deg(muzzle, barrel, pivot, p):
    """The turn (deg) that puts the bore line through p (aim_hold_solve without the limit)."""
    mc = sub(muzzle, pivot)
    b = sub(mc, scale(barrel, dot(mc, barrel)))
    u = barrel
    for _ in range(40):
        u = norm(sub(sub(p, pivot), from_to_rot(barrel, u, b)))
    return math.degrees(math.acos(max(-1.0, min(1.0, dot(barrel, u)))))


def hold_check(name, d, styles, devices, fails):
    """The hip / crouch hold of this gun aimed at a point REACH_D ahead (camera space, the hold point's camera)."""
    h = d.get("hold", {})
    st = styles.get(h.get("style", "rifle"))
    if not st:
        return None
    sk = h.get("sockets", {})
    grip_r = tuple(sk.get("grip_r", {}).get("pos", [0, 0, 0]))
    gl = sk.get("grip_l", {}).get("pos")
    pivot = tuple(sk["pivot"]) if "pivot" in sk else (scale(add(grip_r, tuple(gl)), 0.5) if gl and any(gl) else add(grip_r, (0, -0.02, 0.07)))
    muzzle = tuple(sk["muzzle"])
    # The longest allowed muzzle device: its front face (attachments.c: placed 1 cm behind the muzzle).
    allow = d.get("attachments", {}).get("muzzle", {}).get("allow", [])
    dev_len = max([devices.get(a, 0.0) for a in allow] + [0.0])
    if dev_len > 0:
        muzzle = (muzzle[0], muzzle[1], muzzle[2] + 0.01 - dev_len)
    over = d.get("holdpoint", {}).get("states", {})
    worst = 0.0
    for state in ("hip", "crouch"):
        pose = over.get(state) or st["states"].get(state) or st["states"]["hip"]
        rot = hp_euler_deg(pose.get("rot", [0, 0, 0]))
        pos = pose.get("pos", [0, 0, 0])
        hold = (pos[0], pos[1], -pos[2])
        item_t = sub(hold, qrot(rot, grip_r))   # the anchor (grip_r) is a point on the hold point
        m = add(item_t, qrot(rot, muzzle))
        bvec = qrot(rot, (0, 0, -1))
        pv = add(item_t, qrot(rot, pivot))
        ang = hold_aim_deg(m, bvec, pv, (0.0, 0.0, -REACH_D))
        worst = max(worst, ang)
        if ang > HOLD_AIM_MAX_DEG:
            fails.append("%s: the %s hold needs %.1f deg to reach a %.0f m aim point (limit %.0f, device %.3f m)" % (name, state, ang, REACH_D, HOLD_AIM_MAX_DEG, dev_len))
    return worst


def main():
    fails, n = [], 0
    styles = json.load(open(os.path.join(ROOT, "assets", "human", "holdpoint.json"), encoding="utf-8"))["styles"]
    devices = {}
    for path in glob.glob(os.path.join(ROOT, "assets", "attachments", "*.json")):
        a = json.load(open(path, encoding="utf-8"))
        if a.get("slot") == "muzzle":
            devices[a["id"]] = float(a.get("muzzle_length", 0.0))
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
        margin, rel = aim.get("obstruct_margin", 0.75), aim.get("obstruct_margin_rel", 0.05)
        if not (0.1 <= margin <= 5.0) or not (0.0 <= rel <= 0.3):
            fails.append("%s: aim.obstruct_margin %s / obstruct_margin_rel %s out of 0.1..5 / 0..0.3" % (name, margin, rel))
        ang = hold_check(name, d, styles, devices, fails)
        if ang is not None:
            print("  %-16s hip / crouch hold reaches a %.0f m aim point with a %.2f deg turn" % (name, REACH_D, ang))
    for f in fails:
        print("FAIL " + f)
    print("aim_sockets: %d guns, %d failure(s)" % (n, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
