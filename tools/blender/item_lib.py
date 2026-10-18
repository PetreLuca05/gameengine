"""Procedural modelling kit for detailed items (docs/ITEM_PIPELINE.md).

Item scripts (assets/src/items/<id>.py) build their model with this module: solids (bevelled boxes,
profile extrusions, cylinders, lathes, text) in ENGINE ITEM SPACE (metres, origin = grip_r, +Y up,
-Z forward, +X right), optionally cut by other solids (exact booleans), added to named parts with
pivots, roles and material slots; sockets are named frames. Item.finish() turns it into a Blender
scene that follows the export conventions (tools/blender/item_export.py):

    ITEM_<id>            empty at the item origin (grip_r)
      P_<part>           mesh, origin = the part pivot; custom props mech / hide / fold / fold_axis / fold_angle / detail
        S_<socket>       empty (+ under a part, or under the root)

Blender is Z-up: engine (x, y, z) is Blender (x, -z, y), so a gun points along Blender +Y and stands up
in the viewport; the exporter converts back.

Every solid is built from bmesh operations (no context-dependent bpy.ops), so the scripts run the same
under `blender -b -P` and the `bpy` Python module.
"""
import math

import bmesh
import bpy
from mathutils import Matrix, Vector

# Engine item space -> Blender (Z up): x, y, z -> x, -z, y.
E2B = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
B2E = E2B.inverted()

DEFAULT_BEVEL = 0.0006   # 0.6 mm: machined edges catch light at first-person distance
DEFAULT_SEGS = 2
# Detail scale (item_export quality): extra bevel segments and the radial segment multiplier applied to
# every solid, so the whole library can be refined at once (first-person silhouettes, edge highlights).
BEVEL_SEGS_ADD = 1
RADIAL_MULT = 1.5


def radial(n):
    return max(6, int(round(n * RADIAL_MULT / 4.0)) * 4) if n > 6 else n


def v(x, y=None, z=None):
    if y is None:
        return Vector(x)
    return Vector((x, y, z))


# ------------------------------------------------------------------ solids


class Solid:
    """A closed bmesh in engine item space with one material slot per face (names, resolved by the Item)."""

    def __init__(self, bm=None, mat="steel_parkerized"):
        self.bm = bm or bmesh.new()
        self.mat = mat
        self.face_mats = None   # optional per-face material names (set by paint())

    def copy(self):
        s = Solid(self.bm.copy(), self.mat)
        s.face_mats = list(self.face_mats) if self.face_mats else None
        return s

    # transforms (engine space)
    def move(self, d):
        bmesh.ops.translate(self.bm, vec=Vector(d), verts=self.bm.verts)
        return self

    def rotate(self, axis, deg, center=(0, 0, 0)):
        m = Matrix.Rotation(math.radians(deg), 4, Vector(axis))
        bmesh.ops.rotate(self.bm, cent=Vector(center), matrix=m, verts=self.bm.verts)
        return self

    def scale(self, s, center=(0, 0, 0)):
        if isinstance(s, (int, float)):
            s = (s, s, s)
        c = Vector(center)
        for vt in self.bm.verts:
            d = vt.co - c
            vt.co = Vector((c.x + d.x * s[0], c.y + d.y * s[1], c.z + d.z * s[2]))
        return self

    def mirror_x(self):
        """Mirror copy across x = 0 (fused): symmetric parts are modelled on the right side."""
        geom = self.bm.verts[:] + self.bm.edges[:] + self.bm.faces[:]
        bmesh.ops.mirror(self.bm, geom=geom, axis='X', merge_dist=1e-6)
        return self

    def cut(self, *cutters):
        """Exact boolean difference (machined pockets, ports, holes, slots)."""
        for c in cutters:
            if c is None:
                continue
            self._boolean(c, 'DIFFERENCE')
        return self

    def union(self, *others):
        for o in others:
            self._boolean(o, 'UNION')
        return self

    def intersect(self, other):
        return self._boolean(other, 'INTERSECT')

    def _boolean(self, other, op):
        before = self.bm.calc_volume(signed=False)
        keep = self.bm.copy()
        self._boolean_solver(other, op, 'EXACT')
        after = self.bm.calc_volume(signed=False)
        # Guard: a cut never removes more than the cutter; a union never loses volume. A broken result
        # (degenerate input) retries with the float solver, else keeps the solid uncut and says so.
        cv = other.bm.calc_volume(signed=False)
        bad = (op == 'DIFFERENCE' and after < before - cv * 1.05 - 1e-12) or (op == 'UNION' and after < before * 0.98) or len(self.bm.faces) < 4
        if bad and op != 'INTERSECT':
            self.bm.free()
            self.bm = keep.copy()
            self._boolean_solver(other, op, 'FLOAT')
            after = self.bm.calc_volume(signed=False)
            if (op == 'DIFFERENCE' and after < before - cv * 1.05 - 1e-12) or len(self.bm.faces) < 4:
                print("item_lib: %s boolean failed (%d faces), kept the solid as it was" % (op.lower(), len(keep.faces)))
                self.bm.free()
                self.bm = keep
                return self
        keep.free()
        return self

    def _boolean_solver(self, other, op, solver):
        ma = bpy.data.meshes.new("_bool_a")
        mb = bpy.data.meshes.new("_bool_b")
        self.bm.to_mesh(ma)
        other.bm.to_mesh(mb)
        oa = bpy.data.objects.new("_bool_a", ma)
        ob = bpy.data.objects.new("_bool_b", mb)
        col = bpy.context.scene.collection
        col.objects.link(oa)
        col.objects.link(ob)
        mod = oa.modifiers.new("b", 'BOOLEAN')
        mod.object = ob
        mod.operation = op
        mod.solver = solver
        if solver == 'EXACT':
            mod.use_self = True   # bevelled concave outlines can self-intersect slightly
            mod.use_hole_tolerant = True
        dg = bpy.context.evaluated_depsgraph_get()
        res = bpy.data.meshes.new_from_object(oa.evaluated_get(dg))
        self.bm.free()
        self.bm = bmesh.new()
        self.bm.from_mesh(res)
        clean(self.bm, 1e-7)
        for o in (oa, ob):
            col.objects.unlink(o)
            bpy.data.objects.remove(o)
        for m in (ma, mb, res):
            bpy.data.meshes.remove(m)
        self.face_mats = None
        return self

    def bevel(self, width=DEFAULT_BEVEL, segs=DEFAULT_SEGS, angle=30.0):
        """Bevels the edges sharper than angle (degrees)."""
        bevel_edges(self.bm, width, segs, angle)
        return self

    def paint(self, mat, pred):
        """Assigns mat to the faces whose centre satisfies pred(center, normal) (labels, stripes, painted marks)."""
        if self.face_mats is None:
            self.face_mats = [self.mat] * len(self.bm.faces)
        self.bm.faces.ensure_lookup_table()
        for i, f in enumerate(self.bm.faces):
            if pred(f.calc_center_median(), f.normal):
                self.face_mats[i] = mat
        return self


def bevel_edges(bm, width, segs, angle=30.0):
    if width <= 0 or segs <= 0:
        return
    segs += BEVEL_SEGS_ADD
    lim = math.radians(angle)
    edges = [e for e in bm.edges if len(e.link_faces) == 2 and e.calc_face_angle(0.0) > lim]
    if not edges:
        return
    bmesh.ops.bevel(bm, geom=edges, offset=width, offset_type='OFFSET', segments=segs, profile=0.5, affect='EDGES', clamp_overlap=True,
                    loop_slide=True, harden_normals=False)
    clean(bm)


def clean(bm, dist=2e-6):
    """Merges the near-coincident vertices bevels / booleans leave (they break the exact boolean solver)."""
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=dist)
    bmesh.ops.dissolve_degenerate(bm, dist=dist, edges=bm.edges[:])


def box(center, half, bevel=DEFAULT_BEVEL, segs=DEFAULT_SEGS, mat="steel_parkerized"):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for vt in bm.verts:
        vt.co = Vector((center[0] + vt.co.x * 2 * half[0], center[1] + vt.co.y * 2 * half[1], center[2] + vt.co.z * 2 * half[2]))
    s = Solid(bm, mat)
    if bevel > 0:
        s.bevel(min(bevel, min(half) * 0.9), segs)
    return s


def _poly_face(bm, pts3):
    vs = [bm.verts.new(p) for p in pts3]
    return bm.faces.new(vs)


def _extrude_poly(pts2, to3, depth_vec, mat, bevel, segs, angle=30.0):
    """pts2: polygon (CCW in its plane); to3(p2) -> Vector on the start plane; extruded by depth_vec."""
    bm = bmesh.new()
    f = _poly_face(bm, [to3(p) for p in pts2])
    bmesh.ops.recalc_face_normals(bm, faces=[f])
    r = bmesh.ops.extrude_face_region(bm, geom=[f])
    nv = [g for g in r['geom'] if isinstance(g, bmesh.types.BMVert)]
    bmesh.ops.translate(bm, vec=depth_vec, verts=nv)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    s = Solid(bm, mat)
    if bevel > 0:
        s.bevel(bevel, segs, angle)
    return s


def side_prism(pts_zy, x0, x1, bevel=DEFAULT_BEVEL, segs=DEFAULT_SEGS, mat="steel_parkerized", angle=30.0):
    """Side silhouette (z, y) points extruded across x0..x1: receivers, stocks, grips, triggers."""
    return _extrude_poly(pts_zy, lambda p: Vector((x0, p[1], p[0])), Vector((x1 - x0, 0, 0)), mat, bevel, segs, angle)


def front_prism(pts_xy, z0, z1, bevel=DEFAULT_BEVEL, segs=DEFAULT_SEGS, mat="steel_parkerized", angle=30.0):
    """Cross-section (x, y) points extruded along z0..z1: rails, handguards, barrels with flats."""
    return _extrude_poly(pts_xy, lambda p: Vector((p[0], p[1], z0)), Vector((0, 0, z1 - z0)), mat, bevel, segs, angle)


def top_prism(pts_xz, y0, y1, bevel=DEFAULT_BEVEL, segs=DEFAULT_SEGS, mat="steel_parkerized", angle=30.0):
    """Plan (x, z) outline extruded up y0..y1: blades, plates, cases seen from above."""
    return _extrude_poly(pts_xz, lambda p: Vector((p[0], y0, p[1])), Vector((0, y1 - y0, 0)), mat, bevel, segs, angle)


def _frame(axis):
    a = Vector(axis).normalized()
    t = Vector((0, 1, 0)) if abs(a.y) < 0.9 else Vector((1, 0, 0))
    u = a.cross(t).normalized()
    w = a.cross(u).normalized()
    return a, u, w


def lathe(profile, p0, axis=(0, 0, -1), segs=32, mat="steel_parkerized", bevel=0.0, cap=True, angle=30.0):
    """Revolves profile [(r, t)] (t = distance along axis from p0) around the axis: barrels, knobs, caps, bottles.
    Radii of 0 close the ends; otherwise cap=True fills the end rings."""
    a, u, w = _frame(axis)
    p0 = Vector(p0)
    segs = radial(segs)
    bm = bmesh.new()
    rings = []
    for (r, t) in profile:
        ring = []
        if r <= 1e-7:
            ring = [bm.verts.new(p0 + a * t)]
        else:
            for i in range(segs):
                ang = 2 * math.pi * i / segs
                ring.append(bm.verts.new(p0 + a * t + (u * math.cos(ang) + w * math.sin(ang)) * r))
        rings.append(ring)
    for ra, rb in zip(rings, rings[1:]):
        if len(ra) == 1 and len(rb) == 1:
            continue
        if len(ra) == 1:
            for i in range(segs):
                bm.faces.new((ra[0], rb[i], rb[(i + 1) % segs]))
        elif len(rb) == 1:
            for i in range(segs):
                bm.faces.new((ra[(i + 1) % segs], ra[i], rb[0]))
        else:
            for i in range(segs):
                bm.faces.new((ra[i], rb[i], rb[(i + 1) % segs], ra[(i + 1) % segs]))
    if cap:
        if len(rings[0]) > 1:
            bm.faces.new(list(reversed(rings[0])))
        if len(rings[-1]) > 1:
            bm.faces.new(rings[-1])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    s = Solid(bm, mat)
    if bevel > 0:
        s.bevel(bevel, 2, angle)
    return s


def cyl(p0, p1, r, segs=32, mat="steel_parkerized", bevel=DEFAULT_BEVEL, r1=None):
    """Cylinder (or cone with r1) from p0 to p1."""
    p0, p1 = Vector(p0), Vector(p1)
    d = p1 - p0
    L = d.length
    rr = r if r1 is None else r1
    s = lathe([(r, 0.0), (rr, L)], p0, d, segs, mat)
    if bevel > 0:
        s.bevel(min(bevel, r * 0.4), 2)
    return s


def tube(p0, p1, r_out, r_in, segs=32, mat="steel_parkerized", bevel=DEFAULT_BEVEL):
    p0, p1 = Vector(p0), Vector(p1)
    L = (p1 - p0).length
    prof = [(r_in, 0.0), (r_out, 0.0), (r_out, L), (r_in, L)]
    s = _ring_lathe(prof, p0, p1 - p0, segs, mat)
    if bevel > 0:
        s.bevel(min(bevel, (r_out - r_in) * 0.4), 2)
    return s


def _ring_lathe(profile, p0, axis, segs, mat):
    """Closed profile loop [(r, t)] revolved (a ring section: tubes, nuts, scope rings)."""
    a, u, w = _frame(axis)
    p0 = Vector(p0)
    segs = radial(segs)
    bm = bmesh.new()
    rings = []
    for (r, t) in profile:
        rings.append([bm.verts.new(p0 + a * t + (u * math.cos(2 * math.pi * i / segs) + w * math.sin(2 * math.pi * i / segs)) * r) for i in range(segs)])
    n = len(rings)
    for k in range(n):
        ra, rb = rings[k], rings[(k + 1) % n]
        for i in range(segs):
            bm.faces.new((ra[i], rb[i], rb[(i + 1) % segs], ra[(i + 1) % segs]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    return Solid(bm, mat)


def ring_lathe(profile, p0, axis=(0, 0, -1), segs=32, mat="steel_parkerized", bevel=0.0):
    s = _ring_lathe(profile, p0, axis, segs, mat)
    if bevel > 0:
        s.bevel(bevel, 2)
    return s


def sphere(c, r, segs=24, rings=12, mat="steel_parkerized"):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=segs, v_segments=rings, radius=r)
    bmesh.ops.translate(bm, vec=Vector(c), verts=bm.verts)
    return Solid(bm, mat)


def torus(c, axis, R, r, segs=32, tsegs=12, mat="steel_parkerized"):
    prof = [(R + r * math.cos(2 * math.pi * k / tsegs), r * math.sin(2 * math.pi * k / tsegs)) for k in range(tsegs)]
    return _ring_lathe(prof, c, axis, segs, mat)


def rounded_rect(w, h, r, n=4, cx=0.0, cy=0.0):
    """CCW outline of a w x h rectangle with corner radius r (n segments per corner)."""
    r = min(r, w * 0.5 - 1e-6, h * 0.5 - 1e-6)
    pts = []
    for (x, y, a0) in ((w / 2 - r, h / 2 - r, 0), (-w / 2 + r, h / 2 - r, 90), (-w / 2 + r, -h / 2 + r, 180), (w / 2 - r, -h / 2 + r, 270)):
        for k in range(n + 1):
            a = math.radians(a0 + 90 * k / n)
            pts.append((cx + x + r * math.cos(a), cy + y + r * math.sin(a)))
    return pts


def circle_pts(r, n=24, cx=0.0, cy=0.0, a0=0.0):
    return [(cx + r * math.cos(a0 + 2 * math.pi * k / n), cy + r * math.sin(a0 + 2 * math.pi * k / n)) for k in range(n)]


def arc_pts(cx, cy, r, a0, a1, n):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * k / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * k / n))) for k in range(n + 1)]


def text(body, pos, size, normal=(1, 0, 0), up=(0, 1, 0), depth=0.0002, mat="engrave", align='CENTER'):
    """Raised lettering (serials, markings, labels) laid on the plane through pos facing normal."""
    cu = bpy.data.curves.new("_txt", 'FONT')
    cu.body = body
    cu.size = size
    cu.extrude = depth * 0.5
    cu.align_x = align
    cu.align_y = 'CENTER'
    cu.resolution_u = 3
    ob = bpy.data.objects.new("_txt", cu)
    bpy.context.scene.collection.objects.link(ob)
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    bpy.context.scene.collection.objects.unlink(ob)
    bpy.data.objects.remove(ob)
    bpy.data.curves.remove(cu)
    bm = bmesh.new()
    bm.from_mesh(me)
    bpy.data.meshes.remove(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-7)
    # Font space: x right, y up, z out of the text -> the given frame.
    n = Vector(normal).normalized()
    u = Vector(up).normalized()
    r = u.cross(n).normalized()
    m = Matrix(((r.x, u.x, n.x), (r.y, u.y, n.y), (r.z, u.z, n.z))).to_4x4()
    m.translation = Vector(pos) + n * depth * 0.5
    bmesh.ops.transform(bm, matrix=m, verts=bm.verts)
    return Solid(bm, mat)


def array(solid, count, step):
    """count copies of solid, step apart (fused into one solid without booleans: for cutters / repeated details)."""
    out = Solid(bmesh.new(), solid.mat)
    me = bpy.data.meshes.new("_arr")
    for i in range(count):
        c = solid.copy().move(Vector(step) * i)
        c.bm.to_mesh(me)
        out.bm.from_mesh(me)
        c.bm.free()
    bpy.data.meshes.remove(me)
    return out


def merge(*solids):
    """Several solids as one (no boolean: overlapping shells are fine for cutters / single-material parts).
    None when there are none (cut() skips None)."""
    solids = [s for s in solids if s is not None]
    if not solids:
        return None
    out = Solid(bmesh.new(), solids[0].mat)
    me = bpy.data.meshes.new("_mrg")
    for s in solids:
        s.bm.to_mesh(me)
        out.bm.from_mesh(me)
    bpy.data.meshes.remove(me)
    return out


# ------------------------------------------------------------------ common mechanical details


def label_band(solid_cyl, y0, y1, mat):
    """Paints the faces of a Y-axis cylinder / bottle between heights y0..y1 (a wrap-around label)."""
    return solid_cyl.paint(mat, lambda c, n: y0 <= c.y <= y1 and abs(n.y) < 0.6)


def screw(pos, normal, r=0.0022, head=0.0008, slot=True, hexsock=False, mat="steel_blued", segs=16):
    """Pan / cap screw head proud of a surface (normal = out of it)."""
    n = Vector(normal).normalized()
    s = lathe([(0, 0), (r, 0), (r, head * 0.6), (r * 0.85, head), (0, head)], pos, n, segs, mat)
    if slot:
        a, u, w = _frame(n)
        c = box((0, 0, 0), (r * 1.2, r * 0.18, head), 0, 0, mat)
        m = Matrix((u, w, a)).transposed().to_4x4()
        m.translation = Vector(pos) + n * head
        bmesh.ops.transform(c.bm, matrix=m, verts=c.bm.verts)
        s.cut(c)
    elif hexsock:
        h = lathe([(0, -0.001), (r * 0.45, -0.001), (r * 0.45, head * 0.7), (0, head * 0.7)], Vector(pos) + n * head * 0.5, n, 6, mat)
        s.cut(h)
    return s


def pin(pos, axis, r, length, mat="steel_blued", segs=12):
    """Cross pin / roll pin seen on both sides (a short cylinder through)."""
    a = Vector(axis).normalized()
    p = Vector(pos)
    return cyl(p - a * length * 0.5, p + a * length * 0.5, r, segs, mat, bevel=r * 0.25)


def picatinny(z0, z1, y_base, x=0.0, mat="aluminium_anodized", height=0.0094, slots=True, axis_up=(0, 1, 0)):
    """MIL-STD-1913 rail along z (z0 > z1 or z0 < z1), base at y_base: 21.2 mm across the top, dovetail
    flanks, cross slots 5.23 mm wide on a 10.01 mm pitch. Returns a Solid (top rail, pointing +Y)."""
    za, zb = min(z0, z1), max(z0, z1)
    w = 0.0212 * 0.5
    pts = [(-w * 0.78, 0.0), (w * 0.78, 0.0), (w * 0.78, height * 0.32), (w, height * 0.55), (w, height * 0.72), (w * 0.72, height),
           (-w * 0.72, height), (-w, height * 0.72), (-w, height * 0.55), (-w * 0.78, height * 0.32)]
    pts = [(x + p[0], y_base + p[1]) for p in pts]
    s = front_prism(pts, za, zb, bevel=0.0004, segs=1, mat=mat)
    if slots:
        pitch, sw = 0.01001, 0.00523
        n = int((zb - za - 0.004) / pitch)
        if n > 0:
            first = za + ((zb - za) - (n - 1) * pitch) * 0.5
            cut = box((x, y_base + height * 0.62 + 0.004, first), (w * 1.3, 0.004, sw * 0.5), 0, 0, mat)
            s.cut(array(cut, n, (0, 0, pitch)))
    return s


def mlok_slots(z0, z1, center, normal, mat="polymer_matte", depth=0.01):
    """M-LOK cutters: 7 x 32 mm slots on a 40 mm pitch along z, through a face centred at center facing normal."""
    za, zb = min(z0, z1), max(z0, z1)
    n = int((zb - za + 0.008) / 0.04)
    if n <= 0:
        return None
    first = za + ((zb - za) - (n - 1) * 0.04) * 0.5
    nv = Vector(normal).normalized()
    side = nv.cross(Vector((0, 0, 1))).normalized()
    cutters = []
    for i in range(n):
        c = Vector(center)
        c.z = first + i * 0.04
        pts = rounded_rect(0.007, 0.032, 0.0035, 3)
        # outline in (side, z) plane, extruded along -normal through the wall
        bm = bmesh.new()
        f = bm.faces.new([bm.verts.new(c + nv * depth * 0.5 + side * p[0] + Vector((0, 0, p[1]))) for p in pts])
        r = bmesh.ops.extrude_face_region(bm, geom=[f])
        bmesh.ops.translate(bm, vec=-nv * depth, verts=[g for g in r['geom'] if isinstance(g, bmesh.types.BMVert)])
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
        cutters.append(Solid(bm, mat))
    return merge(*cutters)


def knurl(solid_cyl_p0, p1, r, count=36, depth=0.0004, mat="steel_blued"):
    """Straight knurl grooves around a cylinder p0 -> p1 (cutters)."""
    p0, p1 = Vector(solid_cyl_p0), Vector(p1)
    a, u, w = _frame(p1 - p0)
    L = (p1 - p0).length
    cs = []
    for k in range(count):
        ang = 2 * math.pi * k / count
        d = u * math.cos(ang) + w * math.sin(ang)
        c = p0 + a * (L * 0.5) + d * r
        g = box((0, 0, 0), (depth, r * 0.06, L * 0.5 + 0.001), 0, 0, mat)
        m = Matrix((d, d.cross(a), a)).transposed().to_4x4()
        m.translation = c
        bmesh.ops.transform(g.bm, matrix=m, verts=g.bm.verts)
        cs.append(g)
    return merge(*cs)


def serrations(z0, z1, y0, y1, x, pitch=0.0015, depth=0.0005, mat="steel_parkerized", side=1):
    """Vertical grip serrations (cutters) on the face x (side = +1 right face, -1 left)."""
    n = int(abs(z1 - z0) / pitch)
    cut = box((x, (y0 + y1) * 0.5, min(z0, z1) + pitch * 0.5), (depth, (y1 - y0) * 0.5, pitch * 0.25), 0, 0, mat)
    return array(cut, n, (0, 0, pitch))


def stipple(x, y0, y1, z0, z1, pitch=0.0018, r=0.00055, depth=0.0004, side=1, inside=None, mat="polymer_matte", shear=0.0):
    """Grip stippling (cutters): a staggered field of small conical dimples on the face x (side +1 = a +X
    face, -1 = a -X face) over y0..y1 / z0..z1; inside(y, z) -> bool limits it to a panel shape; shear
    shifts z with depth below y1 (raked grips)."""
    cs = []
    row = 0
    y = y1 - pitch * 0.5
    while y > y0:
        z = z0 + pitch * (0.5 if row % 2 else 0.0) + (y1 - y) * shear
        while z < z1 + (y1 - y) * shear:
            if inside is None or inside(y, z):
                cs.append(lathe([(r, 0.0), (r * 0.25, depth * 2.0)], (x + side * depth, y, z), (-side, 0, 0), 6, mat, cap=True))
            z += pitch
        y -= pitch * 0.866
        row += 1
    return merge(*cs) if cs else None


# ------------------------------------------------------------------ items / parts / sockets


class Part:
    def __init__(self, item, name, parent=None, pivot=(0, 0, 0), mech="", hide="", fold="", fold_axis=(1, 0, 0), fold_angle=0.0,
                 detail=False):
        self.item = item
        self.name = name
        self.parent = parent
        self.pivot = Vector(pivot)
        self.mech, self.hide, self.fold = mech, hide, fold
        self.fold_axis, self.fold_angle = Vector(fold_axis), fold_angle
        self.detail = detail   # small detail: dropped from the coarsest LOD
        self.solids = []

    def add(self, *solids, mat=None):
        for s in solids:
            if s is None:
                continue
            if mat:
                s.mat = mat
            self.solids.append(s)
        return self


class Item:
    """One item scene. kind: weapon | attachment | item (budgets / validation, tools/validate_item)."""

    def __init__(self, item_id, kind="item"):
        self.id = item_id
        self.kind = kind
        self.parts = []
        self.sockets = []   # (name, part, pos, rot_euler_deg)
        self.mats = []

    def part(self, name, parent=None, **kw):
        p = Part(self, name, parent, **kw)
        self.parts.append(p)
        return p

    def bounds(self):
        mn = Vector((1e9, 1e9, 1e9))
        mx = Vector((-1e9, -1e9, -1e9))
        for p in self.parts:
            for so in p.solids:
                for vt in so.bm.verts:
                    mn = Vector(map(min, mn, vt.co))
                    mx = Vector(map(max, mx, vt.co))
        return mn, mx

    def translate(self, d, keep=()):
        """Moves every solid, pivot and socket but the ones named in keep (item scripts model around a
        convenient origin first)."""
        d = Vector(d)
        for p in self.parts:
            p.pivot = p.pivot + d
            for so in p.solids:
                so.move(d)
        self.sockets = [(n, pt, pos if n in keep else pos + d, rot) for (n, pt, pos, rot) in self.sockets]

    def hold_small(self):
        """Places a palm-held item against the right palm: grip_r (the origin) is the palm centre and the
        palm faces -X in item space (items.c item_palm_frame: back of the hand +X), so the item's +X face
        sits on the origin, the palm a little below its middle, centred front to back."""
        mn, mx = self.bounds()
        c = (mn + mx) * 0.5
        hx, hy = (mx.x - mn.x) * 0.5, (mx.y - mn.y) * 0.5
        self.translate(Vector((-hx + 0.002, hy * 0.2, 0.0)) - c, keep=("grip_r",))

    def socket(self, name, pos, rot=(0, 0, 0), part=None):
        """rot: engine-space Euler degrees (x pitch, y yaw, z roll) of the socket frame."""
        self.sockets.append((name, part, Vector(pos), Vector(rot)))

    def finish(self, clear=True):
        if clear:
            bpy.ops.wm.read_factory_settings(use_empty=True)
        scene = bpy.context.scene
        root = bpy.data.objects.new("ITEM_" + self.id, None)
        root.empty_display_type = 'ARROWS'
        root.empty_display_size = 0.05
        root["kind"] = self.kind
        scene.collection.objects.link(root)
        objs = {}
        mats = {}

        def material(name):
            if name not in mats:
                m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
                col = library_color(name)
                m.diffuse_color = (col[0], col[1], col[2], 1.0)
                mats[name] = m
            return mats[name]

        for p in self.parts:
            bm = bmesh.new()
            me = bpy.data.meshes.new("P_" + p.name)
            names = []
            face_mat = []
            tmp = bpy.data.meshes.new("_tmp")
            for s in p.solids:
                n0 = len(bm.faces)
                s.bm.to_mesh(tmp)
                bm.from_mesh(tmp)
                fm = s.face_mats or [s.mat] * (len(bm.faces) - n0)
                face_mat.extend(fm[:len(bm.faces) - n0] + [s.mat] * max(0, len(bm.faces) - n0 - len(fm)))
            bpy.data.meshes.remove(tmp)
            for nm in face_mat:
                if nm not in names:
                    names.append(nm)
            bm.faces.ensure_lookup_table()
            for f, nm in zip(bm.faces, face_mat):
                f.material_index = names.index(nm)
            bmesh.ops.translate(bm, vec=-p.pivot, verts=bm.verts)
            bmesh.ops.transform(bm, matrix=E2B, verts=bm.verts)
            bm.to_mesh(me)
            bm.free()
            for nm in names:
                me.materials.append(material(nm))
            for poly in me.polygons:
                poly.use_smooth = True
            ob = bpy.data.objects.new("P_" + p.name, me)
            scene.collection.objects.link(ob)
            parent = objs[p.parent.name] if p.parent is not None else root
            ob.parent = parent
            ppiv = p.parent.pivot if p.parent is not None else Vector((0, 0, 0))
            ob.location = (E2B @ (p.pivot - ppiv).to_4d()).to_3d()
            for k in ("mech", "hide", "fold"):
                if getattr(p, k):
                    ob[k] = getattr(p, k)
            if p.fold:
                ob["fold_axis"] = list(p.fold_axis)
                ob["fold_angle"] = float(p.fold_angle)
            if p.detail:
                ob["detail"] = 1
            objs[p.name] = ob
        for (name, part, pos, rot) in self.sockets:
            e = bpy.data.objects.new("S_" + name, None)
            e.empty_display_type = 'ARROWS'
            e.empty_display_size = 0.015
            scene.collection.objects.link(e)
            base = part.pivot if part is not None else Vector((0, 0, 0))
            e.parent = objs[part.name] if part is not None else root
            e.location = (E2B @ (pos - base).to_4d()).to_3d()
            # engine-space rotation -> Blender: R_b = E2B R_e E2B^-1
            re = (Matrix.Rotation(math.radians(rot.y), 4, 'Y') @ Matrix.Rotation(math.radians(rot.x), 4, 'X') @
                  Matrix.Rotation(math.radians(rot.z), 4, 'Z'))
            e.rotation_mode = 'QUATERNION'
            e.rotation_quaternion = (E2B @ re @ B2E).to_quaternion()
        return root


_LIB = None


def library():
    global _LIB
    if _LIB is None:
        import json
        import os
        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, "..", "..", "assets", "items", "materials.json")) as f:
            _LIB = json.load(f)["materials"]
    return _LIB


def library_color(name):
    m = library().get(name)
    if not m:
        return (1.0, 0.0, 1.0)
    return tuple(m.get("color", (0.5, 0.5, 0.5)))
