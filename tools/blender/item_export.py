"""Item export: a conforming Blender scene -> .dgim (docs/ITEM_PIPELINE.md "Conventions").

Conventions (enforced, errors stop the export):
  * one root empty ITEM_<id> at the world origin, unrotated, unscaled: the item origin = the grip_r socket
    (weapons: where the right hand holds the grip; items: where the hand holds them);
  * Blender Z up, the item's forward = Blender +Y (engine -Z), right = +X; metres;
  * parts are mesh objects P_<name> under the root or another part (the hierarchy is kept: a follower under
    its magazine), object origin = the part pivot, scale 1 (applied); custom properties:
      mech  slide | bolt | pump | mag (fire animation travel / wobble, docs/FIRE_ANIM.md) or a static role name
            (trigger, hammer, safety, mag_release, charging_handle, dust_cover, ...)
      hide  attachment slot whose attachment hides the part (magazine, stock, optic, muzzle, underbarrel)
      fold  attachment slot that folds it (flip-up irons: optic), fold_axis (engine space) / fold_angle (deg)
      detail 1: small detail, dropped from the coarsest LOD
  * sockets are empties S_<name> under the root or a part (engine socket names: grip_r, grip_l, sight,
    muzzle, optic, magazine, ...);
  * material slots are named after assets/items/materials.json keys ('.001' suffixes are ignored).

Per part: evaluated mesh (modifiers applied), triangulated, split normals from a 35 degree sharp angle,
masks baked (tools/bake_item: AO over the whole item, curvature, dirt, grunge, cavity), LOD 1 / 2 by
collapse decimation (masks transferred from LOD 0), a convex hull (<= 64 points) for physics props.
"""
import math
import os
import struct
import sys
import zlib

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector, kdtree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bake_item"))
import bake_item  # noqa: E402

E2B = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
B2E = E2B.inverted()
LOD_RATIOS = (1.0, 0.3, 0.09)
SHARP_DEG = 35.0
MAX_HULL = 64
SLOTS = ("optic", "muzzle", "underbarrel", "magazine", "stock")
VDT = np.dtype([("pos", "<u2", 3), ("oct", "<i2", 2), ("mat", "u1"), ("wear", "u1"), ("dirt", "u1"), ("ao", "u1"), ("grunge", "u1"), ("cav", "u1")])


def oct_encode(n):
    """Unit normals (N x 3) -> octahedral i16 x 2 (the engine's oct_decode)."""
    n = n / np.maximum(np.abs(n).sum(axis=1, keepdims=True), 1e-12)
    x, y, z = n[:, 0].copy(), n[:, 1].copy(), n[:, 2]
    neg = z < 0
    tx = (1.0 - np.abs(y)) * np.where(x >= 0, 1.0, -1.0)
    ty = (1.0 - np.abs(x)) * np.where(y >= 0, 1.0, -1.0)
    x = np.where(neg, tx, x)
    y = np.where(neg, ty, y)
    return np.stack([np.round(np.clip(x, -1, 1) * 32767), np.round(np.clip(y, -1, 1) * 32767)], axis=1).astype("<i2")
MECH = ("", "slide", "bolt", "pump", "mag")


class ExportError(Exception):
    pass


def _clean(name):
    base = name.split(".")[0]
    return base


def find_root():
    roots = [o for o in bpy.data.objects if o.name.startswith("ITEM_") and o.parent is None]
    if len(roots) != 1:
        raise ExportError("expected exactly one root empty ITEM_<id>, found %d" % len(roots))
    return roots[0]


def collect(root):
    parts, sockets = [], []

    def walk(o, parent_idx):
        for c in sorted(o.children, key=lambda x: x.name):
            if c.name.startswith("P_"):
                if c.type != 'MESH':
                    raise ExportError("%s: parts must be meshes" % c.name)
                parts.append((c, parent_idx))
                walk(c, len(parts) - 1)
            elif c.name.startswith("S_"):
                sockets.append((c, parent_idx))
                walk(c, parent_idx)
            else:
                raise ExportError("%s: children of the item are P_<part> meshes or S_<socket> empties" % c.name)

    walk(root, -1)
    return parts, sockets


def engine_xform(ob):
    """matrix_local (Blender, relative to the parent object) -> engine (pos, quat xyzw)."""
    m = B2E @ ob.matrix_local @ E2B
    loc, rot, sc = m.decompose()
    if abs(sc.x - 1) > 1e-4 or abs(sc.y - 1) > 1e-4 or abs(sc.z - 1) > 1e-4:
        raise ExportError("%s: scale %.4f %.4f %.4f (apply the scale)" % (ob.name, sc.x, sc.y, sc.z))
    return loc, rot


def _triangulated_copy(ob, ratio, name):
    """A temporary object: the part's evaluated mesh (decimated by ratio), triangulated, sharp edges marked."""
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    tmp = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(tmp)
    tmp.matrix_world = ob.matrix_world.copy()
    ratio = max(ratio, min(1.0, 20.0 / max(1, len(me.polygons))))   # small parts keep a recognisable shape
    if ratio < 0.999 and len(me.polygons) > 24:
        mod = tmp.modifiers.new("dec", 'DECIMATE')
        mod.decimate_type = 'COLLAPSE'
        mod.ratio = ratio
        mod.use_collapse_triangulate = True
        dg = bpy.context.evaluated_depsgraph_get()
        me2 = bpy.data.meshes.new_from_object(tmp.evaluated_get(dg))
        tmp.modifiers.clear()
        tmp.data = me2
        bpy.data.meshes.remove(me)
        me = me2
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-7)
    bmesh.ops.dissolve_degenerate(bm, dist=1e-8, edges=bm.edges)
    bmesh.ops.triangulate(bm, faces=bm.faces[:], quad_method='BEAUTY', ngon_method='EAR_CLIP')
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = True
    me.set_sharp_from_angle(angle=math.radians(SHARP_DEG))
    # Face-area weighted normals: large flat faces keep their own normal, the bevel strips take the
    # blend (no shading gradients across flat panels next to a bevel).
    mod = tmp.modifiers.new("wn", 'WEIGHTED_NORMAL')
    mod.mode = 'FACE_AREA'
    mod.weight = 80
    mod.keep_sharp = True
    dg = bpy.context.evaluated_depsgraph_get()
    me2 = bpy.data.meshes.new_from_object(tmp.evaluated_get(dg))
    tmp.modifiers.clear()
    tmp.data = me2
    bpy.data.meshes.remove(me)
    return tmp


def _mesh_arrays(tmp):
    me = tmp.data
    nl, nv = len(me.loops), len(me.vertices)
    co = np.empty(nv * 3, dtype=np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    lv = np.empty(nl, dtype=np.int32)
    me.loops.foreach_get("vertex_index", lv)
    cn = np.empty(nl * 3, dtype=np.float32)
    me.corner_normals.foreach_get("vector", cn)
    cn = cn.reshape(-1, 3)
    fm = np.empty(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("material_index", fm)
    ls = np.empty(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("loop_start", ls)
    lm = np.repeat(fm, 3)   # triangulated: 3 loops per face, in order
    names = [(_clean(m.name) if m else "fallback") for m in me.materials] or ["fallback"]
    return co, lv, cn, lm, names


def export(out_path, report=None, blend_path=None, ao_samples=48):
    root = find_root()
    item_id = root.name[5:]
    if root.matrix_world != Matrix.Identity(4):
        raise ExportError("ITEM_%s must sit at the origin, unrotated, unscaled (it is the grip_r socket)" % item_id)
    parts, sockets = collect(root)
    if not parts:
        raise ExportError("no P_<part> meshes under ITEM_%s" % item_id)
    if blend_path:
        bpy.ops.wm.save_as_mainfile(filepath=blend_path, check_existing=False, compress=True)

    # ---- per part, per LOD temporary meshes
    lods = []   # [part][lod] -> tmp object
    for (ob, _) in parts:
        row = []
        for li, ratio in enumerate(LOD_RATIOS):
            if li == 2 and ob.get("detail"):
                row.append(None)
                continue
            row.append(_triangulated_copy(ob, ratio, "_lod%d_%s" % (li, ob.name)))
        lods.append(row)

    # ---- AO: LOD 0 of every part together
    lod0 = [r[0] for r in lods]
    bmin = Vector((1e9, 1e9, 1e9))
    bmax = Vector((-1e9, -1e9, -1e9))
    for o in lod0:
        for vt in o.data.vertices:
            w = B2E @ (o.matrix_world @ vt.co)
            bmin = Vector(map(min, bmin, w))
            bmax = Vector(map(max, bmax, w))
    size = (bmax - bmin).length
    # The authoring parts would coincide with the copies (and occlude them completely): out of the bake.
    hidden = [ob for (ob, _) in parts if not ob.hide_render]
    for ob in hidden:
        ob.hide_render = True
    try:
        ao = bake_item.bake_ao(lod0, distance=max(0.008, min(0.08, size * 0.12)), samples=ao_samples)
    finally:
        for ob in hidden:
            ob.hide_render = False

    # LOD 0 corner AO + world positions: the source the coarser LODs sample.
    src_pos, src_ao = [], []
    for o in lod0:
        co, lv, cn, lm, names = _mesh_arrays(o)
        mw = np.array(o.matrix_world, dtype=np.float32)
        wp = co[lv] @ mw[:3, :3].T + mw[:3, 3]
        src_pos.append(wp)
        src_ao.append(ao[o.name])
    src_pos = np.concatenate(src_pos)
    src_ao = np.concatenate(src_ao)
    kd = kdtree.KDTree(len(src_pos))
    for i, p in enumerate(src_pos):
        kd.insert(p, i)
    kd.balance()

    mats = []
    verts_out, idx_out, hull_pts = [], [], []
    part_recs, hull_recs = [], []
    v_total = i_total = 0
    tris = [0, 0, 0]
    for pi, ((ob, parent), row) in enumerate(zip(parts, lods)):
        pos, rot = engine_xform(ob)
        rec = dict(name=ob.name[2:], parent=parent, pos=pos, rot=rot, mech=str(ob.get("mech", "")), hide=str(ob.get("hide", "")),
                   fold=str(ob.get("fold", "")), fold_axis=list(ob.get("fold_axis", (1, 0, 0))), fold_angle=float(ob.get("fold_angle", 0.0)),
                   v0=[], vn=[], i0=[], inn=[])
        if rec["hide"] and rec["hide"] not in SLOTS:
            raise ExportError("%s: hide '%s' is not an attachment slot %s" % (ob.name, rec["hide"], SLOTS))
        if rec["fold"] and rec["fold"] not in SLOTS:
            raise ExportError("%s: fold '%s' is not an attachment slot" % (ob.name, rec["fold"]))
        lod_data = []
        for li, tmp in enumerate(row):
            if tmp is None:
                continue
            co, lv, cn, lm, names = _mesh_arrays(tmp)
            if len(lv) == 0:
                continue
            slot_ids = []
            for n in names:
                if n not in mats:
                    mats.append(n)
                slot_ids.append(mats.index(n))
            cmat = np.array(slot_ids, dtype=np.int32)[lm]
            # Part-local engine coordinates (the object's own frame, Blender axes converted).
            co_e = co @ np.array(B2E, dtype=np.float32)[:3, :3].T
            cn_e = cn @ np.array(B2E, dtype=np.float32)[:3, :3].T
            mw = np.array(tmp.matrix_world, dtype=np.float32)
            wp = co[lv] @ mw[:3, :3].T + mw[:3, 3]
            wn = cn @ mw[:3, :3].T
            if li == 0:
                cao = ao[tmp.name]
            else:
                cao = np.array([src_ao[kd.find(p)[1]] for p in wp], dtype=np.float32)
            # Masks
            bm = bmesh.new()
            bm.from_mesh(tmp.data)
            bm.normal_update()
            conv = bake_item.curvature(bm)
            bm.free()
            ep = wp @ np.array(B2E, dtype=np.float32)[:3, :3].T   # item-space corner positions (noise is in item space)
            g = bake_item.grunge(ep)
            up = (wn @ np.array(B2E, dtype=np.float32)[:3, :3].T)[:, 1]
            wear, dirt, ao8, gr8, cav8 = bake_item.masks(conv, cao, lv, ep, g, up)
            # Dedup corners into vertices: (position index, quantised normal, material).
            key_n = np.round(cn_e * 512).astype(np.int32)
            vmap = {}
            corners = []   # first corner of every vertex
            local_i = []
            for c in range(len(lv)):
                k = (int(lv[c]), int(key_n[c, 0]), int(key_n[c, 1]), int(key_n[c, 2]), int(cmat[c]))
                vi = vmap.get(k)
                if vi is None:
                    vi = len(corners)
                    vmap[k] = vi
                    corners.append(c)
                local_i.append(vi)
            if len(corners) > 65535:
                raise ExportError("%s LOD %d: %d vertices (16-bit indices: split the part)" % (ob.name, li, len(corners)))
            cc = np.array(corners, dtype=np.int64)
            lod_data.append(dict(pos=co_e[lv[cc]], nrm=cn_e[cc], mat=cmat[cc], wear=wear[cc], dirt=dirt[cc], ao=ao8[cc], grunge=gr8[cc], cav=cav8[cc],
                                 idx=np.array(local_i, dtype=np.uint16)))
            tris[li] += len(local_i) // 3
            if li == 0:
                # Convex hull of the full-detail part (part-local engine space), thinned to MAX_HULL points.
                pts = [Vector(p) for p in co_e]
                hb = bmesh.new()
                for p in pts:
                    hb.verts.new(p)
                if len(pts) >= 4:
                    try:
                        r = bmesh.ops.convex_hull(hb, input=hb.verts[:])
                        hv = [g for g in r["geom"] if isinstance(g, bmesh.types.BMVert)]
                    except Exception:
                        hv = list(hb.verts)
                    hp = [v.co.copy() for v in hv]
                    if len(hp) > MAX_HULL:
                        step = len(hp) / MAX_HULL
                        hp = [hp[int(i * step)] for i in range(MAX_HULL)]
                    hull_recs.append((pi, len(hull_pts), len(hp)))
                    hull_pts.extend(hp)
                hb.free()
        if not lod_data:
            raise ExportError("%s: empty mesh" % ob.name)
        allp = np.concatenate([d["pos"] for d in lod_data])
        pmin, pmax = Vector(allp.min(axis=0)), Vector(allp.max(axis=0))
        lo = np.array(pmin, dtype=np.float64)
        ext = np.maximum(np.array(pmax - pmin, dtype=np.float64), 1e-9)
        for d in lod_data:
            q = np.clip(np.round((d["pos"] - lo) / ext * 65535.0), 0, 65535).astype("<u2")
            o = oct_encode(d["nrm"])
            rows = np.zeros(len(q), dtype=VDT)
            rows["pos"] = q
            rows["oct"] = o
            for k, src in (("mat", "mat"), ("wear", "wear"), ("dirt", "dirt"), ("ao", "ao"), ("grunge", "grunge"), ("cav", "cav")):
                rows[k] = d[src]
            rec["v0"].append(v_total)
            rec["vn"].append(len(rows))
            rec["i0"].append(i_total)
            rec["inn"].append(len(d["idx"]))
            verts_out.append(rows.tobytes())
            idx_out.append(d["idx"].astype("<u2").tobytes())
            v_total += len(rows)
            i_total += len(d["idx"])
        rec["bmin"], rec["bmax"] = pmin, pmax
        part_recs.append(rec)

    for row in lods:
        for t in row:
            if t is not None:
                me = t.data
                bpy.data.objects.remove(t)
                bpy.data.meshes.remove(me)

    sock_recs = []
    for (e, parent) in sockets:
        pos, rot = engine_xform(e)
        sock_recs.append((e.name[2:], parent, pos, rot))

    radius = (bmax - bmin).length * 0.5
    lod_dist = (0.0, radius * 40.0, radius * 120.0)

    def s32(t, n=32):
        b = t.encode("utf-8")[:n - 1]
        return b + b"\0" * (n - len(b))

    pay = bytearray()
    for m in mats:
        pay += s32(m)
    for r in part_recs:
        q = r["rot"]
        nl = len(r["v0"])
        pad = lambda a: list(a) + [0] * (3 - len(a))
        pay += s32(r["name"]) + struct.pack("<i", r["parent"]) + struct.pack("<3f", *r["pos"]) + struct.pack("<4f", q.x, q.y, q.z, q.w)
        pay += s32(r["mech"], 16) + s32(r["hide"], 16) + s32(r["fold"], 16)
        pay += struct.pack("<3f", *r["fold_axis"]) + struct.pack("<f", r["fold_angle"])
        pay += struct.pack("<3f", *r["bmin"]) + struct.pack("<3f", *r["bmax"])
        pay += struct.pack("<I", nl)
        pay += struct.pack("<3I", *pad(r["v0"])) + struct.pack("<3I", *pad(r["vn"])) + struct.pack("<3I", *pad(r["i0"])) + struct.pack("<3I", *pad(r["inn"]))
    for (name, parent, pos, q) in sock_recs:
        pay += s32(name) + struct.pack("<i", parent) + struct.pack("<3f", *pos) + struct.pack("<4f", q.x, q.y, q.z, q.w)
    for (p, first, cnt) in hull_recs:
        pay += struct.pack("<iII", p, first, cnt)
    pay += b"".join(verts_out)
    pay += b"".join(idx_out)
    for p in hull_pts:
        pay += struct.pack("<3f", p.x, p.y, p.z)
    packed = zlib.compress(bytes(pay), 9)
    out = bytearray()
    out += struct.pack("<12I", 0x4D494744, 2, len(part_recs), len(sock_recs), len(mats), len(hull_recs), v_total, i_total, len(hull_pts), 0, len(pay),
                       len(packed))
    out += struct.pack("<3f", *lod_dist)
    out += struct.pack("<3f", *bmin) + struct.pack("<3f", *bmax)
    out += s32(item_id)
    out += packed
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(out)
    info = dict(id=item_id, kind=root.get("kind", "item"), parts=len(part_recs), sockets=[s[0] for s in sock_recs], materials=mats, tris=tris,
                bytes=len(out), bounds=(tuple(bmin), tuple(bmax)))
    if report is not None:
        report.update(info)
    return info
