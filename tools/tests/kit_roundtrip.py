"""Building kit round trip (docs/BUILDING_KIT.md "Round-trip test"): a kit piece goes into Blender and comes
back through tools/blender/kit_export.py, edited; the validator passes it; broken variants (pivot moved,
centimetre scale, renamed mesh, unnamed material slot) are refused by the exporter and rejected by the
validator with their own messages. The engine side (kit_reload rebuilding every instance) is
tools/tests/kit_reload.cfg.

Needs Blender's Python: either `blender -b -P tools/tests/kit_roundtrip.py -- <validate_kit>` or a Python with
the bpy module (pip install bpy): `python tools/tests/kit_roundtrip.py <validate_kit>`.
Exit 1 on a failure.
"""
import json, os, shutil, struct, subprocess, sys, tempfile

import bpy

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "blender"))
import kit_export as KE  # noqa: E402

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
VALIDATOR = args[0] if args else None
KIT = os.path.join(ROOT, "assets", "kits", "building")
fails = 0


def check(cond, msg):
    global fails
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        fails += 1


def glb_json(path):
    with open(path, "rb") as f:
        data = f.read()
    n = struct.unpack("<I", data[12:16])[0]
    return json.loads(data[20:20 + n])


def fresh_import(piece):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=os.path.join(KIT, piece + ".glb"))
    return {o.name: o for o in bpy.context.scene.objects}


scratch = tempfile.mkdtemp(prefix="kit_rt_")
shutil.copy(os.path.join(KIT, "kit.json"), scratch)
shutil.copytree(os.path.join(KIT, "materials"), os.path.join(scratch, "materials"))
PID = "wall_int_100"

# 1. Import -> edit (taller by 0.1 m, a new socket) -> export -> valid.
obs = fresh_import(PID)
check(PID in obs and any(n.startswith("UBX_" + PID) for n in obs), "imported %s with its collision (%s)" % (PID, ", ".join(sorted(obs))))
mesh = obs[PID]
check(len(mesh.data.uv_layers) == 2, "both UV maps came in (%d)" % len(mesh.data.uv_layers))
lo, hi = KE._world_bounds([mesh])
check(abs(lo[0]) < 1e-3 and abs(lo[1]) < 1e-3 and abs(lo[2]) < 1e-3 and abs(hi[0] - 1.0) < 1e-3 and abs(hi[1] - 0.2) < 1e-3 and abs(hi[2] - 2.7) < 1e-3,
      "in Blender: positive octant, 1.0 x 0.2 deep x 2.7 high (%.3f %.3f %.3f)" % (hi[0], hi[1], hi[2]))
for v in mesh.data.vertices:
    if v.co.z > 2.69:
        v.co.z += 0.1
for n, ob in obs.items():
    if n.startswith("UBX_"):
        for v in ob.data.vertices:
            if v.co.z > 2.69:
                v.co.z += 0.1
bpy.ops.object.empty_add(type="PLAIN_AXES", location=(0.5, 0.2, 1.2))
bpy.context.active_object.name = "S_prop"
path, problems, report = KE.export_piece(PID, scratch, VALIDATOR)
check(not problems, "exporter: no convention problems (%s)" % "; ".join(problems))
g = glb_json(path)
names = [n.get("name") for n in g["nodes"]]
check(PID in names and "S_prop" in names and any(n.startswith("UBX_") for n in names), "exported nodes: %s" % names)
acc = g["accessors"][g["meshes"][g["nodes"][names.index(PID)]["mesh"]]["primitives"][0]["attributes"]["POSITION"]]
check(abs(acc["max"][1] - 2.8) < 1e-3 and abs(acc["min"][2] + 0.2) < 1e-3 and abs(acc["max"][2]) < 1e-3,
      "engine space: height 2.8, z -0.2..0 (front -Z) (max %s min %s)" % (acc["max"], acc["min"]))
sock = g["nodes"][names.index("S_prop")]["translation"]
check(abs(sock[0] - 0.5) < 1e-3 and abs(sock[1] - 1.2) < 1e-3 and abs(sock[2] + 0.2) < 1e-3, "socket in engine space (0.5, 1.2, -0.2): %s" % sock)
if VALIDATOR:
    check(" 0 problem(s)" in report, "validator: %s" % report.splitlines()[-1] if report else "validator: no output")

# 2. Broken variants: refused by the exporter (strict) and rejected by the validator (non-strict export).
CASES = [
    ("pivot", lambda o: setattr(o[PID], "location", (0.5, 0, 0)), "pivot"),
    ("scale_cm", lambda o: setattr(o[PID], "scale", (100, 100, 100)), "scale"),
    ("unnamed", lambda o: setattr(o[PID], "name", "Cube"), "no mesh named"),
    ("slot", lambda o: setattr(o[PID].material_slots[0].material, "name", "Material.001"), "material slot"),
]
for tag, breaker, expect in CASES:
    obs = fresh_import(PID)
    breaker(obs)
    try:
        KE.export_piece(PID, scratch, None, strict=True)
        check(False, "%s: the exporter refused" % tag)
    except KE.KitExportError as e:
        check(expect in str(e), "%s: exporter refused (%s)" % (tag, str(e).splitlines()[0][:110]))
    if VALIDATOR and tag != "unnamed":
        _, _, report = KE.export_piece(PID, scratch, VALIDATOR, strict=False)
        vexpect = {"pivot": "pivot off grid", "scale_cm": "scale", "slot": "material slot 'Material.001'"}[tag]
        check(vexpect in report, "%s: validator rejected (%s)" % (tag, (report.splitlines() or ["-"])[0][:110]))

shutil.rmtree(scratch, ignore_errors=True)
print("kit_roundtrip: %d failure(s)" % fails)
sys.exit(1 if fails else 0)
