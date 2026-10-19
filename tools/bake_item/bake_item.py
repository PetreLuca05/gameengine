"""Mask baking for detailed items (docs/ITEM_PIPELINE.md "Baking").

No textures: every exported vertex carries five 0..255 masks the engine's material layering reads
(src/assets/item_mesh.c compose()):
  wear    convex edges (curvature), favouring exposed areas (AO): where finish rubs off to the substrate
  dirt    crevices and occluded areas: grime, packed dust
  ao      ambient occlusion (Cycles bake over the whole item, every part occluding every other)
  grunge  low-frequency 3D noise in item space: breaks up wear / dirt, varies albedo and roughness
  cavity  concave curvature: rust starts here

bake_ao() bakes into a corner colour attribute of one temporary object joining every part (Cycles, CPU);
curvature() works on the part topology. Both run inside Blender (bpy), called by item_export.py.
"""
import math

import bmesh
import bpy
import numpy as np
from mathutils import noise


def curvature(bm):
    """Per BMVert convexity in -1..1: < 0 convex (neighbours below the tangent plane), > 0 concave."""
    bm.verts.ensure_lookup_table()
    out = np.zeros(len(bm.verts), dtype=np.float32)
    for v in bm.verts:
        n = v.normal
        if n.length < 1e-8 or not v.link_edges:
            continue
        acc = 0.0
        for e in v.link_edges:
            d = e.other_vert(v).co - v.co
            L = d.length
            if L > 1e-9:
                acc += d.dot(n) / L
        out[v.index] = acc / len(v.link_edges)
    return out


def grunge(points, scale=28.0):
    """Fractal noise (0..1) at item-space points (N x 3): the same pattern for every LOD."""
    out = np.empty(len(points), dtype=np.float32)
    for i, p in enumerate(points):
        q = (p[0] * scale, p[1] * scale, p[2] * scale)
        out[i] = noise.fractal(q, 0.6, 2.1, 4, noise_basis='PERLIN_ORIGINAL')
    lo, hi = np.percentile(out, 2), np.percentile(out, 98)
    return np.clip((out - lo) / max(hi - lo, 1e-6), 0, 1)


def bake_ao(objects, distance, samples=48):
    """Bakes AO for every object together (they occlude each other). Returns {object name: corner AO array}."""
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = samples
    scene.render.bake.margin = 0
    if scene.world is None:
        scene.world = bpy.data.worlds.new("bake_world")
    scene.world.light_settings.distance = distance
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    attrs = {}
    for o in objects:
        me = o.data
        a = me.color_attributes.get("_ao") or me.color_attributes.new("_ao", 'BYTE_COLOR', 'CORNER')
        me.color_attributes.active_color = a
        if not me.materials:
            me.materials.append(bpy.data.materials.new("_bake"))
        o.select_set(True)
        attrs[o.name] = a
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.bake(type='AO', target='VERTEX_COLORS')
    out = {}
    for o in objects:
        a = o.data.color_attributes["_ao"]
        buf = np.empty(len(a.data) * 4, dtype=np.float32)
        a.data.foreach_get("color", buf)
        out[o.name] = buf.reshape(-1, 4)[:, 0].copy()
        o.data.color_attributes.remove(o.data.color_attributes["_ao"])
    return out


def masks(conv_v, ao_c, corner_vert, pos_c, grunge_c, up_c):
    """Per-corner masks from vertex convexity, corner AO / position noise / normal.y. Arrays in, u8 arrays out."""
    conv = conv_v[corner_vert]
    convex = np.clip(-conv * 2.4, 0, 1)
    concave = np.clip(conv * 2.4, 0, 1)
    wear = np.clip(convex * (0.55 + 0.45 * ao_c), 0, 1)
    # Grime: crevices, occlusion, a little more on upward faces (dust settles).
    dirt = np.clip((1.0 - ao_c) * 1.4 + concave * 0.7 + np.clip(up_c, 0, 1) * 0.12, 0, 1)
    to8 = lambda a: np.clip(np.round(a * 255.0), 0, 255).astype(np.uint8)
    return to8(wear), to8(dirt), to8(ao_c), to8(grunge_c), to8(concave)
