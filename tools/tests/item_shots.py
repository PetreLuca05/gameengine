"""Writes a game script that screenshots items in the inspector (docs/ITEM_PIPELINE.md "Screenshots").

    python tools/tests/item_shots.py <out dir> [ids ...] [--views shaded,wear,matid,wire] [--angles side,34,front,top]
                                     [--before] [--fp] > shots.cfg
    cd build/bin && ./deadground --test --nosound --log shots.log --script shots.cfg

--before adds an "item_meshes 0" pass (the box placeholders, the 'before' shots); --fp adds a first-person
shot of every weapon (give + hip / ADS). ids default to every catalog entry.
"""
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ANGLES = {"side": (90, 0, 1.0), "34": (125, -18, 1.0), "front": (0, 0, 1.0), "top": (90, -80, 1.0), "close": (60, -12, 2.4), "rear": (-145, -15, 1.3)}


def main(argv):
    out = argv[1]
    args = argv[2:]

    def opt(name, default):
        if name in args:
            v = args[args.index(name) + 1]
            del args[args.index(name):args.index(name) + 2]
            return v
        return default

    views = opt("--views", "shaded").split(",")
    angles = opt("--angles", "side,34").split(",")
    before = "--before" in args
    fp = "--fp" in args
    ids = [a for a in args if not a.startswith("--")]
    cat = json.load(open(os.path.join(ROOT, "assets", "items", "catalog.json")))["items"]
    if not ids:
        ids = sorted(cat)
    lines = ["r_maxfps 60", "time_set 11:00", "time_pause", "weather_set clear 0", "hud_compass 0", "hud_coords 0", "hud_killcount 0", "setpos 0 0 -8 0 0", "wait 12"]
    for i in ids:
        insp = i if cat.get(i, {}).get("kind") != "attachment" else i   # attachments are inspected by "att_<id>"
        for view in views:
            lines.append("item_inspect %s %s" % (insp, view))
            for a in angles:
                y, p, z = ANGLES[a]
                lines += ["item_inspect_orbit %g %g %g" % (y, p, z), "wait 3", "screenshot %s/%s_%s_%s.png" % (out, i, view, a), "wait 2"]
        lines.append("item_inspect off")
    if before:
        lines.append("item_meshes 0")
        lines.append("bake_icons %s/icons_before" % out)
        lines.append("wait 120")
        lines.append("item_meshes 1")
    if fp:
        for i in ids:
            if cat.get(i, {}).get("kind") != "weapon":
                continue
            for state in ("1", "0") if before else ("1",):
                lines += ["item_meshes %s" % state, "inv_clear", "give %s" % i, "wield %s" % i, "wait 20", "screenshot %s/%s_fp_%s.png" % (out, i, "after" if state == "1" else "before"),
                          "wait 2", "+aim", "wait 25", "screenshot %s/%s_ads_%s.png" % (out, i, "after" if state == "1" else "before"), "wait 2",
                          "-aim", "wait 4"]
        lines.append("item_meshes 1")
    lines.append("quit")
    print("\n".join(lines))


if __name__ == "__main__":
    main(sys.argv)
