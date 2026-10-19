#!/usr/bin/env python3
"""Inventory lint (docs/INVENTORY.md):
  * theme tokens: the inventory screen (src/ui/inventory/*.c) and the HUD's weapon list and vitals
    (src/ui/hud.c draw_weapon_list / weapon_row / draw_vitals) take every colour from the theme
    (TH->...): no UI_RGBA(...) or 0xAARRGGBB literals unless the line says "lint:ok";
  * the deleted slots, fields and APIs are gone everywhere (code, tests, scripts, data): the old
    equipment slots (melee, earpiece, face cover, eyewear, tactical rig, armband, secure container),
    pockets, the hotbar, weight, nesting, sort, the showcase camera;
  * no "weight" key in any item, weapon or attachment JSON;
  * data/items.json: every "equip" names one of the six slots, every non-weapon / attachment gives a
    grid size, only backpacks have a "container" (one grid, ten columns), exactly one is the default;
  * data/containers.json: 'body' first (equipment, no grid), every other root one grid.
Exit 1 on a failure. Run: python3 tools/tests/lint_inventory.py"""
import json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SLOTS = {"primary", "secondary", "holster", "backpack", "headwear", "body_armor"}
COLOR = re.compile(r"UI_RGBA\s*\(|\b0x[0-9A-Fa-f]{8}u?\b")
# What the inventory simplification deleted: none of it may come back (this file names them).
GONE = re.compile(r"\b(EQ_MELEE|EQ_EARPIECE|EQ_FACE_COVER|EQ_EYEWEAR|EQ_RIG|EQ_ARMBAND|EQ_SECURE|IQ_POCKETS|IQ_RIG|IQ_BACKPACK|IQ_SECURE|InvQuickTarget|"
                  r"InvOrder|inv_order_\w+|inv_quick_parent|inv_find_space_order|INV_HOTBAR|inv_hot|inv_hot_c|inv_hot_find|inv_dev_hot|II_BIND|II_SORT|II_INSERT|"
                  r"inv_bindable|inv_weight|inv_tree_weight|inv_item_weight|inv_parent_weight|no_containers|inv_add_with|inv_give_with|"
                  r"inv_quick_order|inv_showcase|showcase_camera|hotbar_slot)\b")
GONE_JSON = re.compile(r'"(earpiece|face_cover|eyewear|tactical_rig|secure_container|no_containers|max_item|hotbar)"\s*:|"equip"\s*:\s*"(melee|earpiece|face_cover|eyewear|tactical_rig|armband|secure_container)"')
SCAN = ("src", "tests", "tools", "data", "assets/ui", "assets/items", "assets/attachments", "assets/kits", "assets/story")
HUD_FUNCS = ("static void weapon_row(", "static void draw_weapon_list(", "static void draw_vitals(")


def hud_bodies(path):
    """Line numbers of the HUD functions the lint covers (their bodies: up to the closing brace at column 0)."""
    out, inside = set(), False
    for n, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
        if line.startswith(HUD_FUNCS):
            inside = True
        if inside:
            out.add(n)
            if line.startswith("}"):
                inside = False
    return out


def main():
    fails = []
    this = os.path.relpath(os.path.abspath(__file__), ROOT).replace("\\", "/")
    hud = os.path.join(ROOT, "src", "ui", "hud.c")
    hud_lines = hud_bodies(hud) if os.path.exists(hud) else set()
    for top in SCAN:
        for dp, dirs, files in os.walk(os.path.join(ROOT, top)):
            dirs[:] = [d for d in dirs if d not in ("cache", "__pycache__")]
            for f in sorted(files):
                if not f.endswith((".c", ".h", ".py", ".cfg", ".sh", ".json")):
                    continue
                path = os.path.join(dp, f)
                rel = os.path.relpath(path, ROOT).replace("\\", "/")
                if rel == this:
                    continue
                script = f.endswith((".c", ".h", ".py", ".cfg", ".sh"))
                for n, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
                    if script and GONE.search(line) and "DG_PROTOCOL_VERSION" not in line:   # the protocol history names what each version changed
                        fails.append(f"{rel}:{n}: a deleted slot / field / API: {line.strip()[:120]}")
                    if f.endswith(".json") and GONE_JSON.search(line):
                        fails.append(f"{rel}:{n}: a deleted slot / key: {line.strip()[:120]}")
                    themed = (rel.startswith("src/ui/inventory/") and rel.endswith(".c")) or (rel == "src/ui/hud.c" and n in hud_lines)
                    if themed and COLOR.search(line) and "lint:ok" not in line:
                        fails.append(f"{rel}:{n}: colour literal (use a theme token, TH->...): {line.strip()}")
    # No weight anywhere in the item data.
    for top in ("data", "assets/attachments"):
        for dp, _, files in os.walk(os.path.join(ROOT, top)):
            for f in files:
                if f.endswith(".json") and '"weight"' in open(os.path.join(dp, f), encoding="utf-8", errors="replace").read():
                    fails.append(f"{os.path.relpath(os.path.join(dp, f), ROOT)}: a \"weight\" key (weight is gone)")
    items = json.load(open(os.path.join(ROOT, "data", "items.json"), encoding="utf-8"))["items"]
    defaults = 0
    for it in items:
        iid = it.get("id", "?")
        kind = it.get("kind", "misc")
        if kind not in ("weapon", "attachment"):
            g = it.get("grid")
            if not (isinstance(g, list) and len(g) == 2 and all(isinstance(v, int) and 1 <= v <= 16 for v in g)):
                fails.append(f"data/items.json: {iid}: no \"grid\": [w, h]")
        if "equip" in it and it["equip"] not in SLOTS:
            fails.append(f"data/items.json: {iid}: unknown equip slot {it['equip']}")
        c = it.get("container")
        if c is not None:
            comps = c.get("compartments") or []
            if it.get("equip") != "backpack":
                fails.append(f"data/items.json: {iid}: only backpacks have a grid")
            if len(comps) != 1 or comps[0].get("w") != 10:
                fails.append(f"data/items.json: {iid}: a backpack is one grid, ten columns wide")
        if it.get("default"):
            defaults += 1
    if defaults != 1:
        fails.append(f"data/items.json: {defaults} default backpacks (want 1)")
    roots = json.load(open(os.path.join(ROOT, "data", "containers.json"), encoding="utf-8"))["containers"]
    if not roots or roots[0].get("id") != "body" or not roots[0].get("equip") or roots[0].get("compartments"):
        fails.append("data/containers.json: 'body' must be the first root (a zeroed tree's root): equipment, no grid")
    for r in roots[1:]:
        if len(r.get("compartments") or []) != 1:
            fails.append(f"data/containers.json: {r.get('id')}: one grid (no compartments)")
    for f in fails:
        print("lint_inventory:", f)
    print(f"lint_inventory: {len(items)} items, {len(roots)} root containers, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
