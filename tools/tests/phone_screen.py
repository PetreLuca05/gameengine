#!/usr/bin/env python3
"""Phone screen look (docs/PHONE.md "The in-game phone"): runs tools/tests/phone_screen.cfg and
checks the white emissive screen by day and at night, the bloom halo and the screen light.

  * phone_status "level" (the displayed screen level = emissive x exposure) stays inside
    [phone_screen_min, phone_screen_max]: at the minimum by day, higher (up to the cap) at night;
  * the screenshots: the screen is white (near-neutral, bright) day and night; at night the pixels
    just outside the phone glow (bloom) far above the dark scene around them; the screen light
    brightens the hand.

PIL + numpy when available (else the image checks are skipped and reported).
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
    check(len(st) >= 4, "phone_status lines (%d)" % len(st))
    if len(st) < 4:
        return
    lo, hi = 1.15, 2.4   # phone_screen_min / phone_screen_max defaults
    day, night, cam, dusk = (float(s["level"]) for s in st[:4])
    evd, evn = float(st[0]["ev"]), float(st[1]["ev"])
    check(abs(day - lo) < 0.02, "day: the screen at the minimum level (%.3f, EV %.2f)" % (day, evd))
    check(night > day * 1.5 and night <= hi + 1e-3, "night: brighter, capped (%.3f <= %.2f, EV %.2f)" % (night, hi, evn))
    check(lo - 1e-3 <= dusk <= hi + 1e-3 and lo - 1e-3 <= cam <= hi + 1e-3, "dusk / camera app inside [min, max] (%.3f, %.3f)" % (dusk, cam))
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        print("skip image checks (no PIL / numpy)")
        return

    def img(n):
        return np.asarray(Image.open(n).convert("RGB")).astype(float)

    def region(a, x0, y0, x1, y1):
        return a[y0:y1, x0:x1].reshape(-1, 3)

    # 1600 x 900: the held phone's screen spans about x 795..920, y 418..685 (home screen: the
    # blank band under the app grid is pure screen white).
    for name, label in (("phone_screen_day.png", "day"), ("phone_screen_night.png", "night")):
        a = img(name)
        s = region(a, 805, 600, 905, 630).mean(axis=0)
        check(s.min() > 185 and s.max() - s.min() < 18, "%s: the screen is white (%s)" % (label, s.round(0)))
    n = img("phone_screen_night.png")
    halo = region(n, 926, 470, 936, 520).mean()     # just right of the phone body
    far = region(n, 1100, 470, 1140, 520).mean()     # the night scene farther away
    check(halo > far + 12, "night: bloom around the screen (%.1f next to it vs %.1f away)" % (halo, far))
    d = img("phone_screen_day.png")
    dhalo, dfar = region(d, 926, 470, 936, 520).mean(), region(d, 1100, 470, 1140, 520).mean()
    check(dhalo < dfar + 25, "day: no blown-out glow (%.1f next to it vs %.1f away)" % (dhalo, dfar))
    lit, unlit = img("phone_screen_night.png"), img("phone_screen_night_nolight.png")
    hand_l, hand_u = region(lit, 935, 560, 975, 640).mean(), region(unlit, 935, 560, 975, 640).mean()
    check(hand_l > hand_u + 3, "night: the screen light brightens the hand (%.1f vs %.1f without)" % (hand_l, hand_u))


if __name__ == "__main__":
    main()
    print("phone_screen: " + ("PASS" if not fails else "%d FAILED" % fails))
    sys.exit(1 if fails else 0)
