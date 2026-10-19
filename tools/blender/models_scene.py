"""Blender side of tools/blender/open_models.py (docs/ITEM_PIPELINE.md "Open models in Blender").

    blender -b --factory-startup -P tools/blender/models_scene.py -- assemble <manifest.json>
        One scene from the manifest's models, saved to the manifest's "out":
        * every model in its own collection (named by its id) under a collection per kind;
        * laid out on the ground (Blender XY, Z up): a block of rows per kind, each kind under a heading,
          left to right with spacing from the bounds, a new row per group (the kit's categories);
        * a text label under each model (collection "Labels");
        * each model hangs under an empty "place_<id>" at its grid spot (move the empty to move the model);
          a building kit piece's LOD1..3 and collision meshes (UCX_ / UBX_ / UCL_) are hidden.
    blender <file.blend> --python tools/blender/models_scene.py -- view
        The GUI open: frames every 3D view on the visible models.

The manifest: {"out": path, "models": [{"id", "kind", "kind_label", "group", "path", "fmt", "fix"}]},
fmt blend (append every object) | fbx | gltf | obj, fix {"rot_x": degrees, "scale": s} for sources in other
conventions (Tree It: centimetres, Y up). The combined file is a viewing copy: edit an item's own
assets/src/items/<id>.blend (open_models.py <id>), or better its script.
"""
import json
import math
import os
import sys
import time

import bpy
from mathutils import Euler, Matrix, Vector

HIDE_PREFIXES = ("UCX_", "UBX_", "UCL_")   # building kit collision (docs/BUILDING_KIT.md)


def log(msg):
    print("models: " + msg, flush=True)


def layer_collection(lc, coll):
    if lc.collection == coll:
        return lc
    for c in lc.children:
        r = layer_collection(c, coll)
        if r:
            return r
    return None


def hidden_by_rule(ob):
    n = ob.name
    if n.startswith(HIDE_PREFIXES):
        return True
    base = n.rsplit(".", 1)[0] if "." in n else n   # Blender's .001 suffix on a name clash
    return len(base) > 5 and base[-5:-1] == "_LOD" and base[-1] in "123"


def import_model(m, coll):
    """Brings one model into coll; returns its new objects."""
    path, fmt = m["path"], m["fmt"]
    before = set(bpy.data.objects)
    if fmt == "blend":
        with bpy.data.libraries.load(path, link=False) as (src, dst):
            dst.objects = list(src.objects)
        new = [o for o in dst.objects if o is not None]
        for o in new:
            coll.objects.link(o)
        return new
    vl = bpy.context.view_layer
    vl.active_layer_collection = layer_collection(vl.layer_collection, coll)
    if fmt == "fbx":
        # The C++ importer (Blender 4.5+) is several times faster and reads ASCII FBX (Tree It); the add-on otherwise.
        if hasattr(bpy.ops.wm, "fbx_import"):
            bpy.ops.wm.fbx_import(filepath=path)
        else:
            bpy.ops.import_scene.fbx(filepath=path)
    elif fmt == "gltf":
        bpy.ops.import_scene.gltf(filepath=path)
    elif fmt == "obj":
        bpy.ops.wm.obj_import(filepath=path)
    else:
        raise ValueError("unknown format " + fmt)
    new = [o for o in bpy.data.objects if o not in before]
    for o in new:   # importers that make their own collections: everything goes to the model's
        if coll not in o.users_collection:
            coll.objects.link(o)
        for c in list(o.users_collection):
            if c != coll:
                c.objects.unlink(o)
    fix = m.get("fix") or {}
    if fix:
        rot = Matrix.Rotation(math.radians(fix.get("rot_x", 0.0)), 4, "X")
        sc = Matrix.Scale(fix.get("scale", 1.0), 4)
        for o in new:
            if o.parent is None:
                o.matrix_world = sc @ rot @ o.matrix_world
    return new


def bounds(objs):
    bpy.context.view_layer.update()
    lo, hi = Vector((1e9, 1e9, 1e9)), Vector((-1e9, -1e9, -1e9))
    found = False
    for o in objs:
        if o.type not in ("MESH", "CURVE", "SURFACE", "META", "FONT", "ARMATURE") or hidden_by_rule(o):
            continue
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
            found = True
    if not found:   # empties only: their positions
        for o in objs:
            w = o.matrix_world.translation
            lo = Vector(map(min, lo, w - Vector((0.05, 0.05, 0.05))))
            hi = Vector(map(max, hi, w + Vector((0.05, 0.05, 0.05))))
            found = True
    if not found:
        return Vector((0, 0, 0)), Vector((0.1, 0.1, 0.1))
    return lo, hi


def text(name, body, size, loc, coll, align="CENTER"):
    cu = bpy.data.curves.new(name, "FONT")
    cu.body = body
    cu.size = size
    cu.align_x = align
    cu.align_y = "TOP"
    ob = bpy.data.objects.new(name, cu)
    ob.location = loc
    coll.objects.link(ob)
    return ob


def frame_views(lo, hi):
    """Points every 3D view of the file at the box lo..hi (a 3/4 view from the front, -Y, above)."""
    centre = (lo + hi) * 0.5
    radius = max((hi - lo).length * 0.5, 0.05)
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type != "VIEW_3D":
                continue
            for sp in area.spaces:
                if sp.type != "VIEW_3D":
                    continue
                sp.clip_start = min(0.01, radius * 0.005)   # an item fills the view without clipping...
                sp.clip_end = max(100.0, radius * 20.0)     # ... the whole layout too, with depth precision to spare
                r3d = sp.region_3d
                if r3d is None:
                    continue
                r3d.view_perspective = "PERSP"
                r3d.view_location = centre
                r3d.view_rotation = Euler((math.radians(58), 0.0, math.radians(18))).to_quaternion()
                r3d.view_distance = radius * 2.4


def label_width(s, size):
    return len(s) * size * 0.62   # Blender's default font: about 0.6 em a character


def assemble(manifest_path):
    t0 = time.time()
    man = json.load(open(manifest_path, encoding="utf-8"))
    out = man["out"]
    models = man["models"]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    labels = bpy.data.collections.new("Labels")
    scene.collection.children.link(labels)

    # ---- import, per kind (manifest order)
    kinds, by_kind = [], {}
    failed = []
    for m in models:
        k = m["kind"]
        if k not in by_kind:
            kc = bpy.data.collections.new(m.get("kind_label", k))
            scene.collection.children.link(kc)
            by_kind[k] = (kc, [])
            kinds.append(k)
        kc, lst = by_kind[k]
        mc = bpy.data.collections.new(m["id"])
        kc.children.link(mc)
        t = time.time()
        try:
            objs = import_model(m, mc)
        except Exception as e:  # keep going: report every model that would not load
            failed.append((m["id"], str(e)))
            log("FAILED %s (%s): %s" % (m["id"], m["path"], e))
            bpy.data.collections.remove(mc)
            continue
        if not objs:
            failed.append((m["id"], "no objects"))
            log("FAILED %s: no objects in %s" % (m["id"], m["path"]))
            bpy.data.collections.remove(mc)
            continue
        lo, hi = bounds(objs)
        lst.append(dict(m=m, coll=mc, objs=objs, lo=lo, hi=hi))
        # Out of the view layer until the end: every import re-syncs the view layer, which made the kit's
        # 280 imports slow down with the scene's size (0.1 -> 0.3 s each).
        layer_collection(bpy.context.view_layer.layer_collection, mc).exclude = True
        dt = time.time() - t
        if dt > 1.0 or man.get("verbose"):
            log("%-28s %4d objects  %.1fs" % (m["id"], len(objs), dt))

    # ---- layout: a block per kind (rows along +X, the next row towards -Y), the kinds one under the other
    cursor_y = 0.0
    all_lo, all_hi = Vector((1e9, 1e9, 1e9)), Vector((-1e9, -1e9, -1e9))
    for k in kinds:
        kc, lst = by_kind[k]
        if not lst:
            continue
        sizes = sorted(max(e["hi"].x - e["lo"].x, e["hi"].y - e["lo"].y) for e in lst)
        typical = sizes[len(sizes) // 2]
        lsize = min(max(typical * 0.09, 0.012), 0.5)   # label height
        gap = min(max(typical * 0.3, 0.04), 3.0)
        cells = []
        for e in lst:
            w = max(e["hi"].x - e["lo"].x, label_width(e["m"]["id"], lsize))
            d = e["hi"].y - e["lo"].y
            cells.append((w, d))
        area = sum((w + gap) * (d + gap + lsize * 2) for (w, d) in cells)
        row_w = max(max(w for (w, _) in cells), math.sqrt(area) * 1.6)
        head = lsize * 3.0
        text("title_" + k, "%s (%d)" % (kc.name, len(lst)), head, (0.0, cursor_y, 0.0), labels, align="LEFT")
        cursor_y -= head * 1.8
        x, row = 0.0, []
        prev_group = None

        def flush(row, y):
            depth = max(e["hi"].y - e["lo"].y for (e, _, _) in row)
            for (e, cx, w) in row:
                off = Vector((cx - (e["lo"].x + e["hi"].x) * 0.5, y - e["hi"].y, -e["lo"].z))
                place = bpy.data.objects.new("place_" + e["m"]["id"], None)
                place.empty_display_type = "PLAIN_AXES"
                place.empty_display_size = max(lsize, 0.01)
                place.location = off
                e["coll"].objects.link(place)
                for o in e["objs"]:
                    if o.parent is None:
                        o.parent = place
                        o.matrix_parent_inverse = Matrix.Identity(4)
                text("label_" + e["m"]["id"], e["m"]["id"], lsize, (cx, y - depth - lsize * 0.6, 0.0), labels)
            return y - depth - lsize * 2.2 - gap

        for e, (w, d) in zip(lst, cells):
            grp = e["m"].get("group")
            if row and (x + w > row_w or grp != prev_group):
                cursor_y = flush(row, cursor_y)
                x, row = 0.0, []
            row.append((e, x + w * 0.5, w))
            x += w + gap
            prev_group = grp
        if row:
            cursor_y = flush(row, cursor_y)
        all_lo = Vector(map(min, all_lo, Vector((0.0, cursor_y, 0.0))))
        all_hi = Vector(map(max, all_hi, Vector((row_w, 0.0, max(e["hi"].z - e["lo"].z for e in lst)))))
        cursor_y -= head * 2.0

    vl = bpy.context.view_layer
    for k in kinds:
        for e in by_kind[k][1]:
            layer_collection(vl.layer_collection, e["coll"]).exclude = False
    vl.update()
    for k in kinds:
        for e in by_kind[k][1]:
            for o in e["objs"]:
                if hidden_by_rule(o):   # the kit's LODs and collision (the eye icon: still in the file)
                    o.hide_set(True)
                    o.hide_render = True
    if all_lo.x < all_hi.x:
        frame_views(all_lo, all_hi)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tmp = out + ".tmp.blend"
    bpy.ops.wm.save_as_mainfile(filepath=tmp, check_existing=False, compress=False)
    os.replace(tmp, out)
    n = sum(len(by_kind[k][1]) for k in kinds)
    log("%d models in %d kinds (%s), %d failed -> %s  %.1fs" % (
        n, len(kinds), ", ".join("%s %d" % (k, len(by_kind[k][1])) for k in kinds), len(failed), out, time.time() - t0))
    return 1 if failed else 0


def view():
    objs = [o for o in bpy.context.scene.objects if o.visible_get() and o.type != "FONT"]
    if objs:
        lo, hi = bounds(objs)
        frame_views(lo, hi)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if argv and argv[0] == "assemble":
        sys.exit(assemble(argv[1]))
    if argv and argv[0] == "view":
        try:
            view()
        except Exception as e:  # never stop the GUI from opening
            print("models_scene view: %s" % e)
        return
    print(__doc__)


if __name__ == "__main__":
    main()
