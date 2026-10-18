"""Builds detailed items: runs assets/src/items/<id>.py, saves <id>.blend, exports assets/items/meshes/<id>.dgim
and validates it (docs/ITEM_PIPELINE.md).

    python tools/blender/build_items.py [ids ...] [--all] [--no-blend] [--samples N]     (pip install bpy)
    blender -b -P tools/blender/build_items.py -- [ids ...] [--all]                       (a Blender install)
    python tools/blender/build_items.py --blend assets/src/items/rifle.blend               (export a hand-edited .blend)

An item script defines build(ctx) -> item_lib.Item. ctx: .id, .root (repo), .weapon / .item / .attachment
(the item's JSON objects, or None), .lib (item_lib). The .blend files are build products (git-ignored):
the scripts are the source; a hand-edited .blend exports with --blend.
"""
import importlib.util
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "validate_item"))
sys.path.insert(0, os.path.join(ROOT, "assets", "src", "items"))   # shared sub-assemblies (_parts.py)

import bpy  # noqa: E402

import item_export  # noqa: E402
import item_lib  # noqa: E402

SRC = os.path.join(ROOT, "assets", "src", "items")
OUT = os.path.join(ROOT, "assets", "items", "meshes")


class Ctx:
    pass


def load_json_objects():
    weapons, items, atts = {}, {}, {}
    wd = os.path.join(ROOT, "data", "weapons")
    for f in sorted(os.listdir(wd)):
        if f.endswith(".json"):
            j = json.load(open(os.path.join(wd, f)))
            j["_file"] = "data/weapons/" + f
            weapons[j.get("item", j["id"])] = j
    for it in json.load(open(os.path.join(ROOT, "data", "items.json")))["items"]:
        items[it["id"]] = it
    ad = os.path.join(ROOT, "assets", "attachments")
    for f in sorted(os.listdir(ad)):
        if f.endswith(".json"):
            j = json.load(open(os.path.join(ad, f)))
            j["_file"] = "assets/attachments/" + f
            atts[j["id"]] = j
    return weapons, items, atts


def build_one(item_id, save_blend=True, samples=48):
    path = os.path.join(SRC, item_id + ".py")
    spec = importlib.util.spec_from_file_location("item_" + item_id, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    weapons, items, atts = load_json_objects()
    ctx = Ctx()
    ctx.id, ctx.root, ctx.lib = item_id, ROOT, item_lib
    aid = item_id[4:] if item_id.startswith("att_") else item_id
    ctx.weapon = weapons.get(item_id)
    ctx.item = items.get(item_id)
    ctx.attachment = atts.get(aid) if item_id.startswith("att_") else None
    t0 = time.time()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    item = mod.build(ctx)
    item.finish(clear=False)
    t1 = time.time()
    rep = {}
    item_export.export(os.path.join(OUT, item_id + ".dgim"), rep, os.path.join(SRC, item_id + ".blend") if save_blend else None, samples)
    print("%-16s %2d parts  tris %6d / %6d / %6d  %5.1f KB  model %.1fs export %.1fs  sockets %s" %
          (item_id, rep["parts"], rep["tris"][0], rep["tris"][1], rep["tris"][2], rep["bytes"] / 1024.0, t1 - t0, time.time() - t1, ",".join(rep["sockets"])))
    return rep


def main(argv):
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = argv[1:]
    save_blend = "--no-blend" not in argv
    samples = 48
    if "--samples" in argv:
        samples = int(argv[argv.index("--samples") + 1])
    if "--blend" in argv:
        b = argv[argv.index("--blend") + 1]
        bpy.ops.wm.open_mainfile(filepath=b)
        root = item_export.find_root()
        rep = item_export.export(os.path.join(OUT, root.name[5:] + ".dgim"), {}, None, samples)
        print(rep)
        return 0
    ids = [a for a in argv if not a.startswith("--") and not a.isdigit()]
    if "--all" in argv or not ids:
        ids = sorted(f[:-3] for f in os.listdir(SRC) if f.endswith(".py") and not f.startswith("_"))
    failed = []
    for i in ids:
        try:
            build_one(i, save_blend, samples)
        except Exception as e:  # keep going: report every broken item
            import traceback
            traceback.print_exc()
            failed.append((i, str(e)))
    for (i, e) in failed:
        print("FAILED %s: %s" % (i, e))
    import validate_item
    return validate_item.main(["validate_item"] + ids) or (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
