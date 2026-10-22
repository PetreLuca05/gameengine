"""Deadground item editing in the Blender window (docs/ITEM_PIPELINE.md "Edit items in Blender").

open_models.py <id> starts Blender with this script on the item's own scene, assets/src/items/<id>.blend:

    blender assets/src/items/smg.blend --python tools/blender/dg_item_addon.py

* Saving (Ctrl+S) exports the item to the game: a headless Blender runs build_items.py --blend on the saved
  file (bake, .dgim, validation, copies into the builds), so this window stays usable. A running game
  reloads the mesh within a second or two (item_mesh.c). The first save makes the .blend the item's source
  (item_source.py): its script no longer rebuilds it.
* Sidebar (N) > Deadground: the item, "Export on save", Export now, the last export's result, a material
  picker over assets/items/materials.json, Make part (selected objects -> P_ parts of the item), Add socket.
* The viewport's bottom left shows the export state.
"""
import json
import os
import subprocess
import sys
import threading
import time

import bpy
import blf
from bpy.app.handlers import persistent

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD_ITEMS = os.path.join(HERE, "build_items.py")
sys.path.insert(0, HERE)
import item_source  # noqa: E402

SHOW_SECONDS = 12.0   # the overlay line stays this long after an export


def _materials():
    try:
        with open(os.path.join(ROOT, "assets", "items", "materials.json"), encoding="utf-8") as f:
            return {k: v for k, v in json.load(f)["materials"].items() if not k.startswith("_")}
    except (OSError, ValueError, KeyError):
        return {}


LIB = _materials()


def find_root():
    roots = [o for o in bpy.data.objects if o.name.startswith("ITEM_") and o.parent is None]
    return roots[0] if len(roots) == 1 else None


class Export:
    """The export run: one at a time; a save during a run queues one more."""
    proc = None
    lines = []
    pending = None      # a file saved while a run was going
    started = 0.0
    finished = 0.0
    result = None       # the DG_EXPORT json of the last run
    status = ""         # one line: the overlay and the panel


def _reader(proc, lines):
    for raw in proc.stdout:
        lines.append(raw.decode("utf-8", "replace").rstrip())


def start_export(path):
    if Export.proc is not None:
        Export.pending = path
        return
    cmd = [bpy.app.binary_path, "-b", "--factory-startup", "-P", BUILD_ITEMS, "--", "--blend", path]
    flags = 0x08000000 if os.name == "nt" else 0   # CREATE_NO_WINDOW
    Export.lines = []
    Export.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=flags)
    threading.Thread(target=_reader, args=(Export.proc, Export.lines), daemon=True).start()
    Export.started = time.time()
    Export.status = "Exporting %s..." % os.path.basename(path)
    print("dg_item: " + Export.status)
    if not bpy.app.timers.is_registered(_poll):
        bpy.app.timers.register(_poll, first_interval=0.25)
    _redraw()


def _summary(res):
    if res.get("status") != "ok":
        return "Export failed: %s" % res.get("error", "see the console")
    s = "Exported %s in %.0f s: %d parts, %d tris" % (res["id"], res.get("seconds", 0), res["parts"], res["tris"][0])
    s += ", in the game" if res.get("deployed") else " (no build to copy into: run the build)"
    if not res.get("valid", True):
        s += ", validation: %d problem(s)" % sum(1 for l in res.get("validation", []) if l.startswith("error:"))
    return s


def _poll():
    p = Export.proc
    if p is None:
        return None
    if p.poll() is None:
        Export.status = "Exporting... %.0f s" % (time.time() - Export.started)
        _redraw()
        return 0.25
    time.sleep(0.05)   # the reader's last lines
    res = None
    for l in Export.lines:
        if l.startswith("DG_EXPORT "):
            try:
                res = json.loads(l.split(" ", 2)[2])
            except ValueError:
                pass
    if res is None:
        tail = [l for l in Export.lines if l.strip()][-3:]
        res = dict(status="error", error="Blender exited with %d: %s" % (p.returncode, " | ".join(tail)))
    Export.result, Export.proc, Export.finished = res, None, time.time()
    Export.status = _summary(res)
    print("dg_item: " + Export.status)
    for l in Export.lines:
        if l.strip().startswith(("note:", "error:", "warn:")) or "Error" in l:
            print("dg_item:   " + l.strip())
    _redraw()
    if Export.pending:
        path, Export.pending = Export.pending, None
        start_export(path)
        return None
    return SHOW_SECONDS   # one more call clears the overlay


def _redraw():
    wm = bpy.context.window_manager
    for w in (wm.windows if wm else []):
        for a in w.screen.areas:
            if a.type == 'VIEW_3D':
                a.tag_redraw()


@persistent
def on_save(path, *_):
    wm = bpy.context.window_manager
    if not wm.dg_export_on_save or not path or find_root() is None:
        return
    start_export(path)


# ------------------------------------------------------------------ overlay

def _draw_overlay():
    busy = Export.proc is not None
    if not busy and (not Export.status or time.time() - Export.finished > SHOW_SECONDS):
        return
    font = 0
    ui = bpy.context.preferences.system.ui_scale
    blf.size(font, 14 * ui)
    ok = busy or (Export.result or {}).get("status") == "ok"
    blf.color(font, *((1.0, 0.85, 0.3, 1.0) if busy else (0.55, 1.0, 0.55, 1.0) if ok else (1.0, 0.4, 0.4, 1.0)))
    blf.position(font, 20 * ui, 40 * ui, 0)
    blf.draw(font, "Deadground: " + Export.status)


# ------------------------------------------------------------------ operators

class DG_OT_export(bpy.types.Operator):
    """Save the scene and export the item to the game"""
    bl_idname = "dg.export_item"
    bl_label = "Save + export to game"

    def execute(self, context):
        if find_root() is None:
            self.report({'ERROR'}, "no ITEM_<id> root empty in this scene")
            return {'CANCELLED'}
        if not bpy.data.filepath:
            self.report({'ERROR'}, "save the scene first (assets/src/items/<id>.blend)")
            return {'CANCELLED'}
        bpy.ops.wm.save_mainfile()
        if not context.window_manager.dg_export_on_save:   # else the save already started it
            start_export(bpy.data.filepath)
        return {'FINISHED'}


def _material_items(self, context):
    return [(k, k, "%s (rough %.2f, metal %.0f)" % (k, v.get("rough", 0.5), v.get("metal", 0))) for k, v in sorted(LIB.items())]


def library_material(name):
    m = bpy.data.materials.get(name)
    if m is None:
        m = bpy.data.materials.new(name)
        c = LIB.get(name, {}).get("color", (1.0, 0.0, 1.0))
        m.diffuse_color = (c[0], c[1], c[2], 1.0)
    return m


class DG_OT_assign_material(bpy.types.Operator):
    """Assign the game material: to the selected faces in Edit Mode, else to the whole selected objects"""
    bl_idname = "dg.assign_material"
    bl_label = "Assign"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        mat = library_material(context.window_manager.dg_material)
        obs = [o for o in context.selected_objects if o.type == 'MESH'] or ([context.active_object] if context.active_object else [])
        if not obs:
            self.report({'ERROR'}, "select a mesh")
            return {'CANCELLED'}
        edit = context.mode == 'EDIT_MESH'
        if edit:
            bpy.ops.object.mode_set(mode='OBJECT')
        for o in obs:
            me = o.data
            slot = next((i for i, m in enumerate(me.materials) if m == mat), -1)
            if slot < 0:
                me.materials.append(mat)
                slot = len(me.materials) - 1
            for p in me.polygons:
                if p.select or not edit:
                    p.material_index = slot
        if edit:
            bpy.ops.object.mode_set(mode='EDIT')
        return {'FINISHED'}


class DG_OT_make_part(bpy.types.Operator):
    """Make the selected objects parts of the item: P_ names, under the item root (or the active part), in place"""
    bl_idname = "dg.make_part"
    bl_label = "Make part"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        root = find_root()
        if root is None:
            self.report({'ERROR'}, "no ITEM_<id> root empty in this scene")
            return {'CANCELLED'}
        act = context.active_object
        parent = act if act and act.name.startswith("P_") and not act.select_get() else root
        n = 0
        for o in context.selected_objects:
            if o is root or o.type != 'MESH':
                continue
            if not o.name.startswith("P_"):
                o.name = "P_" + o.name
            if o.parent is None or o.parent == parent or not o.parent.name.startswith(("P_", "ITEM_")):
                w = o.matrix_world.copy()
                o.parent = parent
                o.matrix_world = w
            n += 1
        self.report({'INFO'}, "%d part(s) under %s" % (n, parent.name))
        return {'FINISHED'}


class DG_OT_add_socket(bpy.types.Operator):
    """Add a socket (S_<name> empty) at the 3D cursor, under the active part or the item root"""
    bl_idname = "dg.add_socket"
    bl_label = "Add socket"
    bl_options = {'REGISTER', 'UNDO'}
    name: bpy.props.StringProperty(name="Socket", default="socket")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        root = find_root()
        if root is None:
            self.report({'ERROR'}, "no ITEM_<id> root empty in this scene")
            return {'CANCELLED'}
        act = context.active_object
        parent = act if act and act.name.startswith("P_") else root
        e = bpy.data.objects.new("S_" + self.name.strip().removeprefix("S_"), None)
        e.empty_display_type = 'ARROWS'
        e.empty_display_size = 0.02
        (root.users_collection[0] if root.users_collection else context.scene.collection).objects.link(e)
        e.parent = parent
        e.matrix_world.translation = context.scene.cursor.location
        return {'FINISHED'}


# ------------------------------------------------------------------ panel

class DG_PT_item(bpy.types.Panel):
    bl_label = "Deadground item"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Deadground"

    def draw(self, context):
        lay = self.layout
        wm = context.window_manager
        root = find_root()
        if root is None:
            lay.label(text="No ITEM_<id> root in this scene", icon='ERROR')
            return
        iid = root.name[5:]
        col = lay.column(align=True)
        col.label(text="Item: %s" % iid, icon='OBJECT_DATA')
        own = item_source.item_of_blend(bpy.data.filepath or "") == iid
        if not own:
            col.label(text="Not assets/src/items/%s.blend" % iid, icon='INFO')
        elif item_source.is_edited(iid):
            col.label(text="Source: this .blend", icon='CHECKMARK')
        else:
            col.label(text="Source: %s.py (saving takes over)" % iid, icon='SCRIPT')
        lay.prop(wm, "dg_export_on_save")
        lay.operator(DG_OT_export.bl_idname, icon='EXPORT')
        if Export.status:
            box = lay.box()
            for line in _wrap(Export.status, 38):
                box.label(text=line)
            for l in ((Export.result or {}).get("validation") or [])[:6] + ((Export.result or {}).get("warnings") or [])[:6]:
                for line in _wrap(l, 38):
                    box.label(text=line)
        box = lay.box()
        box.label(text="Game material", icon='MATERIAL')
        row = box.row(align=True)
        row.prop(wm, "dg_material", text="")
        row.operator(DG_OT_assign_material.bl_idname)
        box = lay.box()
        box.label(text="Parts / sockets", icon='OUTLINER_OB_EMPTY')
        row = box.row(align=True)
        row.operator(DG_OT_make_part.bl_idname)
        row.operator(DG_OT_add_socket.bl_idname)


def _wrap(s, n):
    out, line = [], ""
    for w in s.split():
        if line and len(line) + 1 + len(w) > n:
            out.append(line)
            line = w
        else:
            line = (line + " " + w).strip()
    return out + ([line] if line else [])


CLASSES = (DG_OT_export, DG_OT_assign_material, DG_OT_make_part, DG_OT_add_socket, DG_PT_item)
_overlay = []


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    bpy.types.WindowManager.dg_export_on_save = bpy.props.BoolProperty(
        name="Export on save", default=True, description="Ctrl+S exports the item to the game (headless, in the background)")
    bpy.types.WindowManager.dg_material = bpy.props.EnumProperty(name="Material", items=_material_items,
                                                                 description="assets/items/materials.json")
    if on_save not in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.append(on_save)
    if not bpy.app.background:
        _overlay.append(bpy.types.SpaceView3D.draw_handler_add(_draw_overlay, (), 'WINDOW', 'POST_PIXEL'))
        bpy.app.timers.register(_show_sidebar, first_interval=0.5)


def _show_sidebar():
    for w in bpy.context.window_manager.windows:
        for a in w.screen.areas:
            if a.type == 'VIEW_3D':
                a.spaces.active.show_region_ui = True
                for r in a.regions:
                    if r.type == 'UI':
                        try:
                            r.active_panel_category = DG_PT_item.bl_category   # Blender 4.2+
                        except (AttributeError, TypeError):
                            pass
    return None


def unregister():
    if on_save in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.remove(on_save)
    for h in _overlay:
        bpy.types.SpaceView3D.draw_handler_remove(h, 'WINDOW')
    _overlay.clear()
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
