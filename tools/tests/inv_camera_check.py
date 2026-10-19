#!/usr/bin/env python3
"""Checks an inv_camera.cfg log (docs/INVENTORY.md "Screen"): Tab opened the inventory and closed it
again; with it open the rendered camera (position, rotation, FOV) stays where it was with it closed
and drifts no more than it did closed (the idle first-person camera's own motion).
Run: python3 tools/tests/inv_camera_check.py <log>"""
import re
import sys

LINE = re.compile(r"cam_track (\S+): (\d+) frames, inventory (open|closed), pos (\S+) (\S+) (\S+) yaw (\S+) pitch (\S+) fov (\S+), "
                  r"max drift pos (\S+) m rot (\S+) deg fov (\S+) deg")
EPS_POS, EPS_DEG = 1e-3, 0.02   # the idle camera's frame-to-frame noise floor


def main(argv):
    runs = {}
    for line in open(argv[1], encoding="utf-8", errors="replace"):
        m = LINE.search(line)
        if m:
            v = [float(x) for x in m.groups()[3:]]
            runs[m.group(1)] = {"inv": m.group(3), "pos": v[0:3], "yaw": v[3], "pitch": v[4], "fov": v[5], "dpos": v[6], "drot": v[7], "dfov": v[8]}
    fails = []
    for k in ("closed", "open", "closed_again"):
        if k not in runs:
            fails.append(f"no cam_track {k} line")
    if not fails:
        c, o, a = runs["closed"], runs["open"], runs["closed_again"]
        if c["inv"] != "closed" or o["inv"] != "open" or a["inv"] != "closed":
            fails.append(f"Tab didn't toggle the inventory: {c['inv']} / {o['inv']} / {a['inv']}")
        tol_pos, tol_rot = c["dpos"] + EPS_POS, c["drot"] + EPS_DEG
        # The two tracks start at different phases of the head's idle sway (the camera is on the
        # head): their positions may differ by a bit more than one window's drift. A real jump (a
        # third-person showcase camera) is metres.
        sway = 1.5 * max(c["dpos"], o["dpos"]) + EPS_POS
        d = max(abs(o["pos"][i] - c["pos"][i]) for i in range(3))
        if d > sway:
            fails.append(f"opening moved the camera {d:.5f} m")
        dr = max(abs(o["yaw"] - c["yaw"]), abs(o["pitch"] - c["pitch"]))
        if dr > tol_rot:
            fails.append(f"opening turned the camera {dr:.4f} deg")
        if abs(o["fov"] - c["fov"]) > 1e-3 or o["dfov"] > c["dfov"] + 1e-3:
            fails.append(f"opening changed the FOV ({c['fov']} -> {o['fov']}, drift {o['dfov']})")
        if o["dpos"] > tol_pos or o["drot"] > tol_rot:
            fails.append(f"the camera moved while open: {o['dpos']:.5f} m / {o['drot']:.4f} deg (closed: {c['dpos']:.5f} / {c['drot']:.4f})")
        print(f"inv_camera_check: closed drift {c['dpos']:.5f} m {c['drot']:.4f} deg, open drift {o['dpos']:.5f} m {o['drot']:.4f} deg, fov {c['fov']:.2f} -> {o['fov']:.2f}")
    for f in fails:
        print("inv_camera_check: FAIL", f)
    print(f"inv_camera_check: {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
