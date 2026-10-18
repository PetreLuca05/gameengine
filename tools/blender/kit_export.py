"""Deadground building kit exporter for Blender (docs/BUILDING_KIT.md "Blender workflow").

Batch:   blender -b assets/kits/building/src/wall_int_300.blend -P tools/blender/kit_export.py -- [options]
In Blender: Scripting > Open > Run Script, then View3D > N panel > "Deadground Kit" > Export piece.
Python (bpy module): import kit_export; kit_export.export_piece(piece_id, out_dir, validator)

Options (after --):
  --piece <id>          the piece id (default: the .blend file name)
  --out <dir>           where the .glb goes (default: assets/kits/building)
  --validate <exe>      run tools/validate_kit on the result (the build's validate_kit binary)

Conventions it enforces before exporting (the validator re-checks the file):
  * The piece lives in the positive octant: x 0..length, y 0..depth, z 0..height (Blender is Z up), the
    origin (0, 0, 0) at its corner. Its front, the side people see, faces +Y (Blender's Back view looks at
    it). The stock glTF exporter (+Y up) turns that into the engine's x 0..w, y 0..h, z -d..0, front -Z.
  * Objects: the render mesh named exactly like the piece, optional <id>_LOD1..3, collision UCX_<id>_NN
    (convex) / UBX_<id>_NN (box), socket empties S_<type>[_n]. Anything else is not exported.
  * Material slots named M_<set>; two UV maps (UV0 world scale, UV1 unwrapped without overlaps).
  * Scale / rotation applied (they are applied on the exported copy; the .blend is not saved).
"""
import os
import subprocess
import sys

import bpy
import mathutils

KIT_DIR_DEFAULT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "kits", "building"))
TOL = 0.001


class KitExportError(Exception):
    pass


def _kind(name, pid):
    if name == pid:
        return "lod0"
    if name.startswith(pid + "_LOD") and name[len(pid) + 4:] in ("1", "2", "3"):
        return "lod"
    if name.startswith("UCX_") or name.startswith("UBX_"):
        return "collision"
    if name.startswith("S_"):
        return "socket"
    return None


def collect(pid):
    """The piece's objects by kind; raises KitExportError with the fix when the scene does not follow the kit."""
    objs = {"lod0": [], "lod": [], "collision": [], "socket": [], "other": []}
    for ob in bpy.context.scene.objects:
        k = _kind(ob.name, pid)
        if k == "socket" and ob.type != "EMPTY":
            raise KitExportError("%s: sockets are empties (Add > Empty > Plain Axes)" % ob.name)
        if k in ("lod0", "lod", "collision") and ob.type != "MESH":
            raise KitExportError("%s: must be a mesh" % ob.name)
        objs[k if k else "other"].append(ob)
    if not objs["lod0"]:
        meshes = [o for o in objs["other"] if o.type == "MESH"]
        names = ", ".join(o.name for o in meshes) or "none"
        raise KitExportError("no mesh named '%s' (meshes: %s) - name the render mesh exactly like the piece" % (pid, names))
    return objs


def _world_bounds(obs):
    lo, hi = [1e30] * 3, [-1e30] * 3
    for ob in obs:
        for c in ob.bound_box:
            w = ob.matrix_world @ mathutils.Vector(c)
            for i in range(3):
                lo[i] = min(lo[i], w[i])
                hi[i] = max(hi[i], w[i])
    return lo, hi


def check(pid, objs):
    """Pre-export checks with Blender-side fixes (the validator is the authority on the file)."""
    problems = []
    bpy.context.view_layer.update()   # world matrices after any edit made from a script
    lo, hi = _world_bounds(objs["lod0"])
    if abs(lo[0]) > TOL or abs(lo[1]) > TOL or abs(lo[2]) > TOL:
        problems.append(pid + ": pivot: the render mesh spans x %.3f.., y %.3f.., z %.3f.. - move it into the positive octant with its corner on the origin "
                        "(select all, Object > Set Origin > Origin to 3D Cursor with the cursor at 0, then snap the corner to 0)" % (lo[0], lo[1], lo[2]))
    for ob in objs["lod0"] + objs["lod"]:
        if any(abs(s - 1.0) > 1e-4 for s in ob.scale) or any(abs(r) > 1e-4 for r in ob.rotation_euler):
            problems.append("%s: unapplied scale / rotation (exported applied; apply it in the .blend too: Ctrl+A > All Transforms)" % ob.name)
        for slot in ob.material_slots:
            if not slot.material or not slot.material.name.startswith("M_"):
                problems.append("%s: material slot '%s' - name it M_<set> (paint, wood, tile, concrete, brick, glass, metal, ...)"
                                % (ob.name, slot.material.name if slot.material else "(empty)"))
        if len(ob.data.uv_layers) < 2:
            problems.append("%s: %d UV map(s) - add a second (lightmap) UV map, unwrapped without overlaps" % (ob.name, len(ob.data.uv_layers)))
    big = max(hi[i] - lo[i] for i in range(3))
    if big > 60 or big < 0.004:
        problems.append(pid + ": scale: largest extent %.3f m - model in metres (Scene > Units: Metric, unit scale 1)" % big)
    return problems


def export_piece(pid=None, out_dir=None, validator=None, strict=True):
    """Exports the open scene as <out_dir>/<pid>.glb. Returns (glb path, problems found before export,
    validator output). strict: refuse to export with pre-export problems."""
    if not pid:
        pid = os.path.splitext(os.path.basename(bpy.data.filepath))[0] if bpy.data.filepath else None
    if not pid:
        raise KitExportError("no piece id: save the .blend as <piece id>.blend or pass --piece")
    out_dir = out_dir or KIT_DIR_DEFAULT
    objs = collect(pid)
    problems = check(pid, objs)
    if problems and strict:
        raise KitExportError("\n".join(problems))
    if bpy.ops.object.mode_set.poll():
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    for k in ("lod0", "lod", "collision", "socket"):
        for ob in objs[k]:
            ob.select_set(True)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, pid + ".glb")
    opts = dict(filepath=path, export_format="GLB", use_selection=True, export_yup=True, export_apply=True, export_normals=True,
                export_texcoords=True, export_materials="EXPORT", export_cameras=False, export_lights=False, export_extras=True,
                export_animations=False)
    known = {p.identifier for p in bpy.ops.export_scene.gltf.get_rna_type().properties}
    bpy.ops.export_scene.gltf(**{k: v for k, v in opts.items() if k in known})   # option names move between Blender versions
    report = ""
    if validator:
        r = subprocess.run([validator, "--kit", out_dir, path], capture_output=True, text=True)
        report = r.stdout.strip()
    return path, problems, report


# ------------------------------------------------------------------ UI (the N panel)

class DG_OT_kit_export(bpy.types.Operator):
    bl_idname = "deadground.kit_export"
    bl_label = "Export piece"
    bl_description = "Check the kit conventions and export this piece to assets/kits/building"

    def execute(self, context):
        try:
            path, problems, report = export_piece(validator=context.scene.get("dg_validate_kit") or None)
        except KitExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        self.report({"INFO"}, "exported %s%s" % (path, ("\n" + report) if report else ""))
        return {"FINISHED"}


class DG_PT_kit(bpy.types.Panel):
    bl_label = "Deadground Kit"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Deadground Kit"

    def draw(self, context):
        col = self.layout.column()
        col.label(text="Positive octant, front +Y, origin at the corner")
        col.operator("deadground.kit_export")


def register():
    for c in (DG_OT_kit_export, DG_PT_kit):
        try:
            bpy.utils.register_class(c)
        except ValueError:
            pass


def _cli():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    opt = {"--piece": None, "--out": None, "--validate": None}
    i = 0
    while i < len(argv):
        if argv[i] in opt and i + 1 < len(argv):
            opt[argv[i]] = argv[i + 1]
            i += 2
        else:
            i += 1
    try:
        path, problems, report = export_piece(opt["--piece"], opt["--out"], opt["--validate"])
    except KitExportError as e:
        print("kit_export: REFUSED\n%s" % e)
        sys.exit(1)
    print("kit_export: wrote %s" % path)
    if report:
        print(report)
        if " 0 problem(s)" not in report:
            sys.exit(1)


if __name__ == "__main__":
    if bpy.app.background:
        _cli()
    else:
        register()
