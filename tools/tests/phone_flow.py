#!/usr/bin/env python3
"""Phone flow without a device (docs/PHONE.md "Tests"): runs tools/tests/phone_flow.cfg and checks
its phone_status lines + screenshots.

  * equip -> one frame later: the in-game phone (home, HUD panel), no portal, no server (equip never
    pairs), and the phone already raised (the script never presses aim: no "hold right mouse");
  * "Connect your phone" -> the Connect portal (modal, server + code); decline -> phone_mode ingame,
    the portal closes, the panel stays; Camera -> photo -> Gallery lists it -> the viewer -> back,
    back -> home; re-equip: no portal;
  * the HUD panel at 16:9 / 21:9 / 4:3: ~22 % of the height, phone aspect, bottom right above
    the item readout (PIL + numpy; skipped without them);
  * static: no phone focus bind in the input map, the phone code never reads aim.

    cd build/bin && python ../../tools/tests/phone_flow.py
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        fails += 1


def src(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def static_checks():
    names = re.search(r"k_action_names\[ACT_COUNT\] = \{(.*?)\};", src("src/input/input.c"), re.S).group(1)
    check("phonepair" not in names and "phonefocus" not in names and "phonecamera" not in names, "input map: no phone focus / pair bind")
    phone = "".join(src("src/gameplay/phone/" + f) for f in ("phone.c", "phone_ui.c", "phone_os.c"))
    check("ACT_AIM" not in phone and "BTN_AIM" not in phone, "the phone code never reads aim")
    w = src("src/gameplay/weapons.c")
    m = re.search(r"bool aim = idef->camera[^;]*;", w)
    check(bool(m) and "BTN_AIM" not in m.group(0), "camera items are raised without the aim button")


def main():
    static_checks()
    exe = os.path.abspath("deadground.exe" if os.name == "nt" else "deadground")
    log = "phone_flow.log"
    if "--no-run" not in sys.argv:
        if os.path.exists(log):
            os.remove(log)
        subprocess.run([exe, "--test", "--nosound", "--log", log, "+set", "net_port", "27391", "+set", "phone_port", "47391",
                        "--script", os.path.join(HERE, "phone_flow.cfg")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    txt = open(log, encoding="utf-8", errors="replace").read()
    st = [dict(re.findall(r"(\w+)=(\"[^\"]*\"|\S+)", l)) for l in txt.splitlines() if "phone_status:" in l]
    check(len(st) >= 12, "phone_status lines (%d)" % len(st))
    if len(st) < 12:
        return
    check(st[0]["flow"] == "idle" and st[0]["app"] == "off" and st[0]["panel"] == "0", "before equip: idle, no panel")
    s = st[1]
    check(s["flow"] == "local" and s["app"] == "home" and s["panel"] == "1" and s["portal"] == "0" and s["screen"] == "none" and s["token"] == "0",
          "one frame after equip: the in-game phone (home, panel), no portal, no code")
    check(st[2]["raised"] == "1" and all(x["raised"] == "1" for x in st[4:10]), "the phone is raised (focused pose) without any aim input, and stays raised")
    check(st[2]["panel_t"] == "1.00", "the panel has slid in")
    s = st[3]
    check(s["flow"] == "pairing" and s["portal"] == "1" and s["screen"] == "phone" and s["server"] == "1" and s["token"] == "1",
          "Connect your phone -> the Connect portal (modal), server up, a code")
    s = st[4]
    check(s["flow"] == "local" and s["app"] == "home" and s["panel"] == "1" and s["token"] == "0" and s["portal"] == "0" and s["mode"] == "ingame" and s["screen"] == "none",
          "decline -> phone_mode ingame, the portal closes, home, panel stays")
    check("phone_card: card=ingame mode=ingame" in txt, "card: In-game phone")
    check(st[5]["app"] == "camera" and st[5]["camera"] == "1", "home -> Camera")
    check(int(st[6]["gallery"]) == int(st[5]["gallery"]) + 1, "shutter -> a photo in the gallery (%s -> %s)" % (st[5]["gallery"], st[6]["gallery"]))
    check(st[7]["app"] == "gallery" and st[7]["viewer"] == "-1", "Camera -> Home -> Gallery")
    check(st[8]["viewer"] == "0", "the first cell (the new photo) opens in the viewer")
    check(st[9]["app"] == "home", "back, back -> home")
    check(st[10]["app"] == "off" and st[10]["panel"] == "0", "put away: screen off, no panel")
    check(st[11]["flow"] == "local" and st[11]["app"] == "home" and st[11]["portal"] == "0" and st[11]["screen"] == "none", "re-equip: no portal")
    check(not any(x["screen"] == "phone" for i, x in enumerate(st) if i != 3), "equipping never opened the portal")
    check("photo " in txt and "phone: deleted" not in txt, "photo saved")
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        print("skip panel image checks (no PIL / numpy)")
        return
    for w, h in ((1600, 900), (2100, 900), (1200, 900)):
        name = "phone_flow_panel_%dx%d.png" % (w, h)
        a = np.asarray(Image.open(name).convert("RGB")).astype(int)
        check(a.shape[1] == w and a.shape[0] == h, "%s: %dx%d" % (name, a.shape[1], a.shape[0]))
        # The panel's white home screen: the bright block in the right half, lower part.
        bright = (a.min(axis=2) > 238)
        bright[: int(h * 0.35)] = False
        bright[:, : w // 2] = False
        rows = np.where(bright.sum(axis=1) > 20)[0]
        cols = np.where(bright.sum(axis=0) > 40)[0]
        if not len(rows) or not len(cols):
            check(False, "%s: panel found" % name)
            continue
        x0, x1, y0, y1 = cols.min(), cols.max(), rows.min(), rows.max()
        ph, pw = y1 - y0 + 1, x1 - x0 + 1
        check(abs(ph / h - 0.22) < 0.03, "%s: panel height %.1f %% of the screen" % (name, 100.0 * ph / h))
        check(abs(pw / ph - 640 / 1136) < 0.05, "%s: phone aspect (%.3f)" % (name, pw / ph))
        check(x1 > w * 0.9 and y1 < h * 0.92, "%s: bottom right, above the readout (right %d, bottom %d)" % (name, x1, y1))


if __name__ == "__main__":
    main()
    print("phone_flow: " + ("PASS" if not fails else "%d FAILED" % fails))
    sys.exit(1 if fails else 0)
