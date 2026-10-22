"""Builds detailed items: runs assets/src/items/<id>.py, saves <id>.blend, exports assets/items/meshes/<id>.dgim
and validates it (docs/ITEM_PIPELINE.md).

    python tools/blender/build_items.py [ids ...] [--all] [--no-blend] [--samples N]     (pip install bpy)
    blender -b -P tools/blender/build_items.py -- [ids ...] [--all]                       (a Blender install)
    python tools/blender/build_items.py --blend assets/src/items/rifle.blend               (export a hand-edited .blend: it becomes
                                                                                            the source, the .dgim goes to the builds)
    python tools/blender/build_items.py rifle --from-script                                (back to the script: the .blend is backed up
                                                                                            to tools/blender/cache/backup and rebuilt)
    blender -b -P tools/blender/build_items.py -- [ids ...] --blend-only                  (only save the .blend: no .dgim,
                                                                                            no validation; open_models.py)

An item script defines build(ctx) -> item_lib.Item. ctx: .id, .root (repo), .weapon / .item / .attachment
(the item's JSON objects, or None), .lib (item_lib). The .blend files are build products (git-ignored) until
one is saved from Blender: then that .blend is the item's source (item_source.py, catalog "path": "blend") and
builds export it instead of running the script. Every written .dgim is copied into this checkout's builds
(build*/bin, the playable mirror), where a running game reloads it.
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
import item_source  # noqa: E402

SRC = item_source.SRC
OUT = item_source.OUT


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


def build_one(item_id, save_blend=True, samples=48, blend_only=False):
    if item_source.is_edited(item_id):   # the hand-edited .blend is the source: never rebuilt from the script
        if blend_only:
            print("%-16s edited in Blender: its .blend is the source, kept" % item_id)
            return {}
        res = export_blend(item_source.blend_path(item_id), samples)
        if res["status"] != "ok":
            raise item_export.ExportError(res["error"])
        return res
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
    if blend_only:  # the authoring scene only (tools/blender/open_models.py): the tracked .dgim stays untouched
        item_export.find_root()
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(SRC, item_id + ".blend"), check_existing=False, compress=True)
        print("%-16s %3d objects  model %.1fs save %.1fs" % (item_id, len(bpy.data.objects), t1 - t0, time.time() - t1))
        return {}
    rep = {}
    item_export.export(os.path.join(OUT, item_id + ".dgim"), rep, os.path.join(SRC, item_id + ".blend") if save_blend else None, samples)
    item_source.deploy(os.path.join(OUT, item_id + ".dgim"))
    print("%-16s %2d parts  tris %6d / %6d / %6d  %5.1f KB  model %.1fs export %.1fs  sockets %s" %
          (item_id, rep["parts"], rep["tris"][0], rep["tris"][1], rep["tris"][2], rep["bytes"] / 1024.0, t1 - t0, time.time() - t1, ",".join(rep["sockets"])))
    return rep


def export_blend(path, samples=48):
    """A hand-edited scene -> its .dgim (+ the builds' copies). An item's own scene becomes its source.
    Prints "DG_EXPORT ok|error <json>" last (dg_item_addon.py reads it)."""
    t0 = time.time()
    res = dict(file=os.path.abspath(path))
    try:
        bpy.ops.wm.open_mainfile(filepath=path)
        root = item_export.find_root()
        item_id = root.name[5:]
        res["id"] = item_id
        if item_source.item_of_blend(path) == item_id and item_source.set_edited(item_id, True):
            print("%-16s the .blend is now the source (catalog \"path\": \"blend\", .gitignore exception)" % item_id)
        res["warnings"] = item_export.prepare_edited(root)
        for w in res["warnings"]:
            print("    note:  " + w)
        dgim = os.path.join(OUT, item_id + ".dgim")
        rep = item_export.export(dgim, {}, None, samples)
        res.update(parts=rep["parts"], tris=rep["tris"], kb=round(rep["bytes"] / 1024.0, 1))
        res["deployed"] = [os.path.relpath(d, ROOT) for d in item_source.deploy(dgim)]
        print("%-16s %2d parts  tris %6d / %6d / %6d  %5.1f KB  export %.1fs  -> %s" %
              (item_id, rep["parts"], rep["tris"][0], rep["tris"][1], rep["tris"][2], res["kb"], time.time() - t0, ", ".join(res["deployed"]) or "no build"))
        res["status"] = "ok"
    except Exception as e:  # item_export.ExportError and anything else: reported, not raised
        import traceback
        traceback.print_exc()
        res.update(status="error", error=str(e))
    res["seconds"] = round(time.time() - t0, 1)
    return res


def validate(ids):
    """validate_item on ids: (exit code, its error / warning lines)."""
    import contextlib
    import io
    import validate_item
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = validate_item.main(["validate_item"] + ids)
    sys.stdout.write(buf.getvalue())
    return rc, [l.strip() for l in buf.getvalue().splitlines() if l.strip().startswith(("error:", "warn:"))]


def main(argv):
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = argv[1:]
    save_blend = "--no-blend" not in argv
    blend_only = "--blend-only" in argv
    samples = 48
    if "--samples" in argv:
        samples = int(argv[argv.index("--samples") + 1])
    if "--blend" in argv:
        res = export_blend(argv[argv.index("--blend") + 1], samples)
        if res["status"] == "ok":
            rc, res["validation"] = validate([res["id"]])
            res["valid"] = rc == 0
        print("DG_EXPORT %s %s" % (res["status"], json.dumps(res)))
        return 0 if res["status"] == "ok" else 1
    ids = [a for a in argv if not a.startswith("--") and not a.isdigit()]
    if "--all" in argv or not ids:
        ids = sorted(f[:-3] for f in os.listdir(SRC) if f.endswith(".py") and not f.startswith("_"))
    if "--from-script" in argv:
        for i in ids:
            if item_source.is_edited(i):
                print("%-16s back to the script; the edited .blend is saved as %s" % (i, item_source.backup(i)))
                item_source.set_edited(i, False)
    failed = []
    for i in ids:
        try:
            build_one(i, save_blend, samples, blend_only)
        except Exception as e:  # keep going: report every broken item
            import traceback
            traceback.print_exc()
            failed.append((i, str(e)))
    for (i, e) in failed:
        print("FAILED %s: %s" % (i, e))
    if blend_only:
        return 1 if failed else 0
    import validate_item
    return validate_item.main(["validate_item"] + ids) or (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
