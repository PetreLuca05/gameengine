#!/usr/bin/env python3
"""Checks a tools/tests/viewmodel_sway.cfg log (docs/FIRE_ANIM.md "Viewmodel sway"):
  * off: every capture is zero (cl_viewmodel_sway 0 leaves the hold point's own sway);
  * on: turning right trails (yaw left > 0.5 deg, roll clockwise, drag left); strafing right rolls
    into the strafe and shifts left; walking bobs (amplitude ~1); sprinting fully blends the pose
    weight; aimed, the turn is the look sway's ADS share of the hip turn (0.1 .. 0.3);
  * standing still the gun breathes (the idle layer 0.2 .. 0.8 deg, varying), the other layers still;
  * a 3 m drop floats the gun up while falling and dips it 2 .. 6 cm on landing (pitch down), one
    bounce; a jump dips it with the nose up; crouching dips it (standing up: test_weapon_sway);
  * the camera is never touched by default (the applied camera dip stays 0) while the sway's own
    landing camera dip registers.
Usage: python viewmodel_sway_check.py <log>"""
import re
import sys

CAP = re.compile(r"vms_capture (on|off) (\w+)")
TOT = re.compile(r"viewmodel_sway_info: (on|off), total rot (\S+) (\S+) (\S+) deg pos (\S+) (\S+) (\S+) mm, sprint (\S+), bob amp (\S+)")
LAY = re.compile(r"viewmodel_sway_info:\s+(look|move|bob|air|idle)\s+rot (\S+) (\S+) (\S+) deg pos (\S+) (\S+) (\S+) mm")
CAMR = re.compile(r"viewmodel_sway_info:\s+idle w \S+, ads \S+, camera dip (\S+) deg \(applied (\S+)\)")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    caps, seqs, cur = {}, {}, None
    for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
        m = CAP.search(line)
        if m:
            cur = (m.group(1), m.group(2))
            seqs[cur] = []
            continue
        if not cur:
            continue
        m = TOT.search(line)
        if m:
            v = [float(x) for x in m.groups()[1:]]
            frame = {"rot": v[0:3], "pos": v[3:6], "sprint": v[6], "bob": v[7], "layers": {}}
            caps.setdefault(cur, frame)   # the first (single captures: the only one)
            seqs[cur].append(frame)
            continue
        m = LAY.search(line)
        if m and seqs[cur]:
            v = [float(x) for x in m.groups()[1:]]
            seqs[cur][-1]["layers"][m.group(1)] = {"rot": v[0:3], "pos": v[3:6]}
            continue
        m = CAMR.search(line)
        if m and seqs[cur]:
            seqs[cur][-1]["cam"] = float(m.group(1))
            seqs[cur][-1]["cam_applied"] = float(m.group(2))
    fails = []

    def check(ok, msg):
        print(("PASS " if ok else "FAIL ") + msg)
        if not ok:
            fails.append(msg)

    names = ["rest", "turn", "overshoot", "strafe", "walk", "sprint", "ads_rest", "ads_turn", "idle", "drop", "jump", "crouch", "stand"]
    for n in names:
        check(("off", n) in caps and ("on", n) in caps, f"captured {n} off + on")
    if fails:
        return 1
    for n in names:
        c = caps[("off", n)]
        check(max(abs(x) for x in c["rot"] + c["pos"]) == 0.0, f"off {n}: zero")
    on = {n: caps[("on", n)] for n in names}
    t = on["turn"]
    check(t["rot"][1] > 0.5 and t["rot"][2] < -0.2 and t["pos"][0] < -1.0, f"turn right: yaw {t['rot'][1]} roll {t['rot'][2]} drag {t['pos'][0]} mm")
    s = on["strafe"]
    check(s["rot"][2] < -1.5 and s["pos"][0] < -5.0, f"strafe right: roll {s['rot'][2]} x {s['pos'][0]} mm")
    w = on["walk"]
    check(0.8 < w["bob"] < 1.2, f"walk bob amplitude {w['bob']}")
    check(on["sprint"]["sprint"] > 0.99, f"sprint pose weight {on['sprint']['sprint']}")
    lt, la = on["turn"]["layers"]["look"]["rot"][1], on["ads_turn"]["layers"]["look"]["rot"][1]
    r = la / lt if lt else 0
    check(0.1 <= r <= 0.3, f"aimed turn = {r:.2f} x the hip turn (look layer)")
    rl = on["rest"]["layers"]
    check(max(abs(x) for k in ("look", "move", "bob") for x in rl[k]["rot"] + rl[k]["pos"]) < 0.01, "at rest: look / move / bob still")

    def seq(n):
        return seqs[("on", n)]
    # Breathing standing still.
    idle = [max(abs(x) for x in f["layers"]["idle"]["rot"]) for f in seq("idle")[6:]]
    pitch = [f["layers"]["idle"]["rot"][0] for f in seq("idle")[6:]]
    check(0.2 < max(idle) < 0.8 and max(pitch) - min(pitch) > 0.2, f"standing still: breathing peak {max(idle):.2f} deg, pitch range {max(pitch) - min(pitch):.2f}")
    # The drop: float up in the air, then the landing dip (2 .. 6 cm, pitch down) and one bounce.
    ys = [f["layers"]["air"]["pos"][1] for f in seq("drop")]
    ps = [f["layers"]["air"]["rot"][0] for f in seq("drop")]
    lo = min(range(len(ys)), key=lambda i: ys[i])
    check(lo > 0 and max(ys[:lo]) > 3.0, f"falling: the gun floats up {max(ys[:lo]) if lo else 0:.1f} mm")
    check(-60.0 < ys[lo] < -20.0 and min(ps) < -3.0, f"landing: dip {ys[lo]:.1f} mm, pitch {min(ps):.2f} deg")
    check(max(ys[lo:]) > 1.0, f"landing: it bounces ({max(ys[lo:]):.1f} mm)")
    cams = [f.get("cam", 0.0) for f in seq("drop")]
    applied = [f.get("cam_applied", 0.0) for f in seq("drop")]
    check(min(cams) < -0.3, f"the sway's landing camera dip registers ({min(cams):.2f} deg)")
    check(max(abs(a) for a in applied) == 0.0, "the camera is untouched by default (applied dip 0)")
    j = seq("jump")
    jd = min(f["layers"]["air"]["pos"][1] for f in j[:15])
    jn = max(f["layers"]["air"]["rot"][0] for f in j[:15])
    check(jd < -3.0 and jn > 0.5, f"jump: the gun dips {jd:.1f} mm, nose up {jn:.2f} deg")
    cd = min(f["layers"]["air"]["pos"][1] for f in seq("crouch"))
    su = max(f["layers"]["air"]["pos"][1] for f in seq("stand"))
    check(cd < -4.0, f"crouch dip {cd:.1f} mm")
    # (A scripted -crouch leaves the player crouched in --test runs: the stand lift is the unit test's.)
    print(f"INFO stand lift {su:.1f} mm (0 while the game keeps the player crouched)")
    print("viewmodel_sway_check:", "FAIL" if fails else "PASS", f"({len(fails)} failures)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
