#!/usr/bin/env python3
"""Inventory lint (docs/INVENTORY.md):
  * theme tokens: the inventory screen (src/ui/inventory/*.c) takes every colour from gui_theme()
    (TH->...): no UI_RGBA(...) or 0xAARRGGBB literals unless the line says "lint:ok";
  * weight is informational: ItemDef.inv_weight and the inv_*_weight() totals are read only by
    the inventory model, the inventory UI, item loading and the tests, never by movement,
    stamina, physics or anything else (speed is identical with an empty and a full inventory);
  * every item in data/items.json gives a grid size and a weight (or is a weapon / attachment,
    which take the class defaults), and every "equip" names a known slot.
Exit 1 on a failure. Run: python3 tools/tests/lint_inventory.py"""
import json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SLOTS = {"primary", "secondary", "holster", "melee", "headwear", "earpiece", "face_cover", "eyewear", "body_armor", "tactical_rig", "backpack",
         "armband", "secure_container"}
WEIGHT = re.compile(r"\binv_weight\b|\binv_(tree|item|parent)_weight\s*\(")
WEIGHT_OK = ("src/gameplay/items.c", "src/gameplay/items.h", "src/gameplay/inventory/", "src/ui/inventory/", "src/game/game.c")
COLOR = re.compile(r"UI_RGBA\s*\(|\b0x[0-9A-Fa-f]{8}u?\b")


def main():
    fails = []
    for dp, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in sorted(files):
            if not f.endswith((".c", ".h")):
                continue
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, ROOT).replace("\\", "/")
            for n, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
                if WEIGHT.search(line) and not rel.startswith(WEIGHT_OK):
                    fails.append(f"{rel}:{n}: reads the inventory weight (informational only): {line.strip()}")
                if rel.startswith("src/ui/inventory/") and rel.endswith(".c") and COLOR.search(line) and "lint:ok" not in line:
                    fails.append(f"{rel}:{n}: colour literal (use a theme token, TH->...): {line.strip()}")
    items = json.load(open(os.path.join(ROOT, "data", "items.json"), encoding="utf-8"))["items"]
    for it in items:
        iid = it.get("id", "?")
        kind = it.get("kind", "misc")
        if kind not in ("weapon", "attachment"):
            g = it.get("grid")
            if not (isinstance(g, list) and len(g) == 2 and all(isinstance(v, int) and 1 <= v <= 16 for v in g)):
                fails.append(f"data/items.json: {iid}: no \"grid\": [w, h]")
            if not isinstance(it.get("weight"), (int, float)):
                fails.append(f"data/items.json: {iid}: no \"weight\"")
        if "equip" in it and it["equip"] not in SLOTS:
            fails.append(f"data/items.json: {iid}: unknown equip slot {it['equip']}")
        c = it.get("container")
        if c is not None and not c.get("compartments"):
            fails.append(f"data/items.json: {iid}: a container without compartments")
    roots = json.load(open(os.path.join(ROOT, "data", "containers.json"), encoding="utf-8"))["containers"]
    if not roots or roots[0].get("id") != "body":
        fails.append("data/containers.json: 'body' must be the first root (a zeroed tree's root)")
    for f in fails:
        print("lint_inventory:", f)
    print(f"lint_inventory: {len(items)} items, {len(roots)} root containers, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
