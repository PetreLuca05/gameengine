"""Which file is an item's source, and where an exported .dgim goes (docs/ITEM_PIPELINE.md "Edit items in Blender").
Standard library only: open_models.py (system Python) and build_items.py / dg_item_addon.py (Blender) share it.

An item is built from its script (assets/src/items/<id>.py, catalog "path": "procedural") until its scene,
assets/src/items/<id>.blend, is saved from Blender: then the .blend is the source (catalog "path": "blend",
tracked in git through a "!" line in .gitignore) and nothing rebuilds it from the script again
(open_models.py <id> --from-script does, after a backup).
"""
import glob
import json
import os
import shutil
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "assets", "src", "items")
OUT = os.path.join(ROOT, "assets", "items", "meshes")
CATALOG = os.path.join(ROOT, "assets", "items", "catalog.json")
GITIGNORE = os.path.join(ROOT, ".gitignore")
BACKUP = os.path.join(HERE, "cache", "backup")
EDITED = "blend"


def blend_path(item_id):
    return os.path.join(SRC, item_id + ".blend")


def item_of_blend(path):
    """The item id when path is an item's own scene (assets/src/items/<id>.blend), else None."""
    p = os.path.abspath(path)
    if os.path.normcase(os.path.dirname(p)) != os.path.normcase(SRC) or not p.endswith(".blend"):
        return None
    return os.path.basename(p)[:-6]


def _catalog():
    with open(CATALOG, encoding="utf-8") as f:
        return json.load(f)


def _write_catalog(cat):
    with open(CATALOG, "w", encoding="utf-8", newline="") as f:
        f.write(json.dumps(cat, indent=2, ensure_ascii=False) + "\n")


def is_edited(item_id, cat=None):
    c = (cat or _catalog()).get("items", {}).get(item_id, {})
    return c.get("path") == EDITED


def edited_ids():
    return sorted(i for i, c in _catalog().get("items", {}).items() if c.get("path") == EDITED)


def _gitignore_line(item_id):
    return "!/assets/src/items/%s.blend" % item_id


def set_edited(item_id, edited):
    """Marks (or unmarks) the .blend as the item's source: the catalog's "path" and the .gitignore exception.
    Returns True when something changed."""
    changed = False
    cat = _catalog()
    c = cat.get("items", {}).get(item_id)
    if c is not None and (c.get("path") == EDITED) != edited:
        c["path"] = EDITED if edited else "procedural"
        _write_catalog(cat)
        changed = True
    line = _gitignore_line(item_id)
    head = "# Hand-edited item scenes: the .blend is the source (tools/blender/item_source.py)"
    with open(GITIGNORE, encoding="utf-8", newline="") as f:
        text = f.read()
    lines = text.splitlines()
    nl = "\r\n" if "\r\n" in text else "\n"
    if edited and line not in lines:
        add = ([head] if head not in lines else []) + [line]
        text = text.rstrip("\r\n") + nl + nl.join(add) + nl
        changed = True
    elif not edited and line in lines:
        lines = [l for l in lines if l != line]
        if not any(l.startswith("!/assets/src/items/") for l in lines):
            lines = [l for l in lines if l != head]
        text = nl.join(lines) + nl
        changed = True
    else:
        return changed
    with open(GITIGNORE, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return changed


def backup(item_id):
    """Copies the item's .blend to tools/blender/cache/backup/<id>-<time>.blend; returns the copy (None: no .blend)."""
    src = blend_path(item_id)
    if not os.path.isfile(src):
        return None
    os.makedirs(BACKUP, exist_ok=True)
    dst = os.path.join(BACKUP, "%s-%s.blend" % (item_id, time.strftime("%Y%m%d-%H%M%S")))
    shutil.copy2(src, dst)
    return dst


def deploy_dirs():
    """The asset folders of this checkout's builds (build*/bin, the playable mirror): the game reads those first."""
    dirs = glob.glob(os.path.join(ROOT, "build*", "bin", "assets", "items", "meshes"))
    dirs += glob.glob(os.path.join(ROOT, "openable builds", "*", "assets", "items", "meshes"))
    return sorted(d for d in dirs if os.path.isdir(d))


def deploy(dgim):
    """Copies an exported .dgim next to every build's copy, so a running game reloads it (item_mesh.c polls)."""
    out = []
    for d in deploy_dirs():
        dst = os.path.join(d, os.path.basename(dgim))
        try:
            shutil.copyfile(dgim, dst + ".tmp")
            os.replace(dst + ".tmp", dst)   # whole: the game never reads a half-written file
            out.append(dst)
        except OSError as e:
            print("item_source: could not copy to %s: %s" % (dst, e))
    return out
