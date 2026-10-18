"""Broken kit pieces for tests/test_kit.c (docs/BUILDING_KIT.md "Round-trip test"): one convention broken
per file, each must be rejected with its own message. Run: python tools/kit_gen/kit_bad_fixtures.py"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit_gen as K

OUT = os.path.join(K.ROOT, "tests", "data", "kit_bad")
COL = {"M_paint": [0.8, 0.8, 0.8, 0.8, 0.0]}


def wall(pid, x0=0.0, scale=1.0, slot="M_paint"):
    p = K.Piece(pid, "wall")
    p.box(x0 * scale, 0, -0.2 * scale, (x0 + 1.0) * scale, 2.7 * scale, 0, slot)
    return p


def rename_lod0(name):
    def f(g):
        g["nodes"][0]["name"] = name
    return f


def drop_uv1(g):
    for m in g["meshes"]:
        for pr in m["primitives"]:
            pr["attributes"].pop("TEXCOORD_1", None)


def dup_lod(g):
    g["nodes"].append({"name": g["nodes"][0]["name"] + "_LOD1", "mesh": g["nodes"][0]["mesh"]})
    g["scenes"][0]["nodes"].append(len(g["nodes"]) - 1)


# An L-shaped (non-convex) prism as a UCX hull.
L_P = [0, 0, 0, 1, 0, 0, 1, 0, -0.2, 0.2, 0, -0.2, 0.2, 0, -1, 0, 0, -1,
       0, 1, 0, 1, 1, 0, 1, 1, -0.2, 0.2, 1, -0.2, 0.2, 1, -1, 0, 1, -1]
L_I = [0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 6, 8, 7, 6, 9, 8, 6, 10, 9, 6, 11, 10,
       0, 6, 7, 0, 7, 1, 1, 7, 8, 1, 8, 2, 2, 8, 9, 2, 9, 3, 3, 9, 10, 3, 10, 4, 4, 10, 11, 4, 11, 5, 5, 11, 6, 5, 6, 0]

FIXTURES = [  # file stem, piece, writer kwargs
    ("good_wall_100", wall("good_wall_100"), {}),
    ("bad_pivot", wall("bad_pivot", x0=0.5), {}),
    ("bad_scale_cm", wall("bad_scale_cm", scale=100.0), {}),
    ("BadName", wall("BadName"), {}),
    ("bad_slot", wall("bad_slot", slot="Material.001"), {}),
    ("bad_unnamed", wall("bad_unnamed"), {"post": rename_lod0("Cube")}),
    ("bad_hull", wall("bad_hull"), {"extra_hulls": [("UCX_bad_hull_09", L_P, L_I)]}),
    ("bad_nouv1", wall("bad_nouv1"), {"post": drop_uv1}),
    ("bad_density", wall("bad_density"), {"uv_scale": 0.1}),
    ("bad_lod", wall("bad_lod"), {"post": dup_lod}),
    ("bad_socket", K.Piece("bad_socket", "door_leaf").box(0, 0, -0.045, 0.86, 2.08, 0, "M_paint"), {}),
    ("bad_nocollision", K.Piece("bad_nocollision", "wall").box(0, 0, -0.2, 1.0, 2.7, 0, "M_paint", collide=False), {}),
]

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for stem, piece, kw in FIXTURES:
        K.write_glb(os.path.join(OUT, stem + ".glb"), piece, COL, **kw)
    print("kit_bad_fixtures: %d files -> %s" % (len(FIXTURES), os.path.relpath(OUT, K.ROOT)))
