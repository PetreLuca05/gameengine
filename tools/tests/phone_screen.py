#!/usr/bin/env python3
"""The held phone model draws no screen content (docs/PHONE.md "The in-game phone"): runs
tools/tests/phone_screen.cfg and checks

  * phone_status "model_screen=<content>/<draws>": the screen quad is drawn, never with a
    texture, emissive or unlit material;
  * the screenshots (PIL + numpy, else skipped): the model's screen is dark glass by day (home and
    camera apps) and at night with no glow around it, while the HUD panel shows the white phone UI.

    cd build/bin && python ../../tools/tests/phone_screen.py
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        fails += 1


def main():
    exe = os.path.abspath("deadground.exe" if os.name == "nt" else "deadground")
    log = "phone_screen.log"
    if "--no-run" not in sys.argv:
        if os.path.exists(log):
            os.remove(log)
        subprocess.run([exe, "--test", "--nosound", "--log", log, "+set", "net_port", "27393", "+set", "phone_port", "47393",
                        "--script", os.path.join(HERE, "phone_screen.cfg")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    txt = open(log, encoding="utf-8", errors="replace").read()
    st = [dict(re.findall(r"(\w+)=(\"[^\"]*\"|\S+)", l)) for l in txt.splitlines() if "phone_status:" in l]
    check(len(st) >= 3, "phone_status lines (%d)" % len(st))
    if len(st) < 3:
        return
    for s, label in zip(st, ("day / home", "day / camera", "night / home")):
        content, draws = (int(v) for v in s["model_screen"].split("/"))
        check(draws > 0 and content == 0, "%s: the model's screen is drawn %d times, never with content" % (label, draws))
    check(st[1]["app"] == "camera" and st[1]["camera"] == "1", "the camera app runs (lens view for the panel)")
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        print("skip image checks (no PIL / numpy)")
        return

    def region(name, x0, y0, x1, y1):
        a = np.asarray(Image.open(name).convert("RGB")).astype(float)
        return a[y0:y1, x0:x1].reshape(-1, 3)

    # 1600 x 900: the held phone's screen spans about x 795..920, y 418..685; the HUD panel's
    # screen about x 1452..1562, y 580..775.
    for name, label in (("phone_screen_day.png", "day / home"), ("phone_screen_day_camera.png", "day / camera"), ("phone_screen_night.png", "night")):
        scr = region(name, 815, 450, 900, 660).mean(axis=0)
        panel = region(name, 1465, 700, 1550, 760).mean(axis=0) if "camera" not in name else None
        check(scr.mean() < 70, "%s: the model's screen is dark glass (%s)" % (label, scr.round(0)))
        if panel is not None:
            check(panel.min() > 200, "%s: the HUD panel shows the white phone UI (%s)" % (label, panel.round(0)))
    n_halo = region("phone_screen_night.png", 926, 470, 936, 520).mean()
    n_far = region("phone_screen_night.png", 1100, 470, 1140, 520).mean()
    check(n_halo < n_far + 8, "night: no glow around the model (%.1f next to it vs %.1f away)" % (n_halo, n_far))


if __name__ == "__main__":
    main()
    print("phone_screen: " + ("PASS" if not fails else "%d FAILED" % fails))
    sys.exit(1 if fails else 0)
