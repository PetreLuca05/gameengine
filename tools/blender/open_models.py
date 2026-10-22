#!/usr/bin/env python3
"""Opens Deadground's models in Blender (docs/ITEM_PIPELINE.md "Open models in Blender"). System Python 3,
standard library only; Blender runs headless for the builds and detached for the window.

    open_models.py <id> [<id> ...]        build what is stale, open them (one window; one item opens its own .blend)
    open_models.py all [--kind K[,K]]     every model in one scene, tools/blender/cache/all[_<kinds>].blend
    open_models.py --list [--kind K]      the ids it knows, by kind

    --no-open        build / assemble only, no Blender window
    --no-copy        leave the clipboard alone (else the opened model's folder is copied to it)
    --rebuild        rebuild the item .blend files and the combined scene even when up to date (never an edited item)
    --from-script    rebuild the items from their scripts, edited ones too: their .blend goes to
                     tools/blender/cache/backup first and the script is the source again
    --blender <exe>  this Blender (else $BLENDER, else the newest installed, else PATH)
    --verbose        every line of Blender's output

An id can also be a kind (`weapon`: all of them), `kind:id` (when two kinds share an id) or a glob (`att_*`,
`kit:wall_*`). Wrappers: tools/blender/open_models.bat (cmd / PowerShell), tools/blender/open_models.sh (bash);
in game, `blender_open` (src/game/game_blender.c).

Items (weapons, attachments, items: assets/src/items/<id>.py) build headless through build_items.py
--blend-only into the git-ignored assets/src/items/<id>.blend when it is missing or older than its script,
a shared _*.py it imports, item_lib.py or its weapon / attachment JSON (the tracked .dgim is not touched).
One item opens with tools/blender/dg_item_addon.py: saving the scene (Ctrl+S) makes the .blend the item's
source (catalog "path": "blend", tracked in git; item_source.py) and exports it to the game in the background.
An edited item is never rebuilt from its script, unless --from-script.
Everything else is imported as it is: characters (assets/human/skins.json), animation clips (the other
assets/human/*.fbx, armatures only: not in `all`), vehicles, props, foliage, Tree It trees, building kit pieces
(their src/<piece>.blend when there is one), the PBR test models and any other model under assets/.
Several models go into one combined scene (tools/blender/models_scene.py), cached in tools/blender/cache/
(git-ignored, outside assets/ so builds do not copy it) and rebuilt when a source changes.
"""
import difflib
import fnmatch
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ITEMS_SRC = os.path.join(ROOT, "assets", "src", "items")
CACHE = os.path.join(HERE, "cache")
BUILD_ITEMS = os.path.join(HERE, "build_items.py")
SCENE_SCRIPT = os.path.join(HERE, "models_scene.py")
ADDON = os.path.join(HERE, "dg_item_addon.py")
MODEL_EXTS = (".blend", ".fbx", ".glb", ".gltf", ".obj")

# Kind order: --list, and the blocks of the combined scene. Kinds found elsewhere under assets/ follow.
KINDS = [
    ("weapon", "Weapons"), ("attachment", "Attachments"), ("item", "Items"), ("character", "Characters"),
    ("vehicle", "Vehicles"), ("prop", "Props"), ("foliage", "Foliage"), ("tree", "Trees"),
    ("kit", "Building kit"), ("pbr_test", "PBR test models"), ("anim", "Animation clips"),
]
NOT_IN_ALL = {"anim"}   # armatures without a mesh: open them by id or with --kind anim
# assets/<dir>: kind, and a fix for sources in other conventions (models_scene.py import_model)
DIR_KINDS = {
    "vehicles": ("vehicle", None),
    "models": ("prop", None),
    "models/foliage": ("foliage", None),
    "models/pbr": ("pbr_test", None),
    # Tree It: centimetres, Y up (docs/VGEO.md "Tree models"). Blender's FBX importer scales but keeps Y up;
    # its OBJ importer turns Y up into Z up but keeps the centimetres.
    "treefbx": ("tree", {".fbx": {"rot_x": 90.0}, ".obj": {"scale": 0.01}}),
}
SKIP_DIRS = {"src", "cache", "human", "skins", "kits"}   # handled on their own (or build products)


class Model:
    def __init__(self, mid, kind, path, fmt, group=None, fix=None, deps=None, note=None):
        self.id, self.kind, self.path, self.fmt = mid, kind, path, fmt
        self.group, self.fix, self.deps, self.note = group, fix, deps or [], note

    @property
    def qid(self):
        return "%s:%s" % (self.kind, self.id)


def rel(p):
    return os.path.relpath(p, ROOT).replace("\\", "/")


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def model_id(stem):
    return re.sub(r"\s+", "_", stem.strip()).lower()


# ------------------------------------------------------------------ the registry

def item_json_deps():
    """item id -> its weapon / attachment JSON (build_items.load_json_objects reads the same files)."""
    deps = {}
    wd = os.path.join(ROOT, "data", "weapons")
    for f in sorted(os.listdir(wd)) if os.path.isdir(wd) else []:
        if f.endswith(".json"):
            try:
                j = load_json(os.path.join(wd, f))
                deps[j.get("item", j.get("id"))] = os.path.join(wd, f)
            except (ValueError, OSError):
                pass
    ad = os.path.join(ROOT, "assets", "attachments")
    for f in sorted(os.listdir(ad)) if os.path.isdir(ad) else []:
        if f.endswith(".json"):
            deps["att_" + f[:-5]] = os.path.join(ad, f)
    return deps


IMPORT_RE = re.compile(r"^\s*(?:from\s+(_\w+)\s+import|import\s+(_\w+))", re.M)


def script_deps(path, seen=None):
    """The script, the shared _*.py modules it imports (transitively) and item_lib.py."""
    seen = seen if seen is not None else set()
    if path in seen or not os.path.isfile(path):
        return seen
    seen.add(path)
    try:
        src = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return seen
    for m in IMPORT_RE.finditer(src):
        script_deps(os.path.join(ITEMS_SRC, (m.group(1) or m.group(2)) + ".py"), seen)
    return seen


def scan_items(out):
    cat_path = os.path.join(ROOT, "assets", "items", "catalog.json")
    catalog = load_json(cat_path).get("items", {}) if os.path.isfile(cat_path) else {}
    jdeps = item_json_deps()
    lib = os.path.join(HERE, "item_lib.py")
    ids = sorted(f[:-3] for f in os.listdir(ITEMS_SRC) if f.endswith(".py") and not f.startswith("_")) if os.path.isdir(ITEMS_SRC) else []
    for i in ids:
        script = os.path.join(ITEMS_SRC, i + ".py")
        deps = sorted(script_deps(script)) + [lib] + ([jdeps[i]] if i in jdeps else [])
        kind = catalog.get(i, {}).get("kind", "item")
        edited = catalog.get(i, {}).get("path") == "blend"   # item_source.py: the .blend is the source
        out.append(Model(i, kind, os.path.join(ITEMS_SRC, i + ".blend"), "item", deps=[] if edited else deps, note="edited" if edited else None))


def scan_human(out, missing):
    human = os.path.join(ROOT, "assets", "human")
    used = set()
    skins_path = os.path.join(human, "skins.json")
    skins = load_json(skins_path).get("skins", []) if os.path.isfile(skins_path) else []
    for s in skins:
        p = os.path.join(ROOT, s.get("mesh", ""))
        used.add(os.path.normcase(os.path.abspath(p)))
        if os.path.isfile(p) and p.lower().endswith(MODEL_EXTS):
            out.append(Model(model_id(s["id"]), "character", p, fmt_of(p), note=s.get("name")))
        else:
            missing.append("character %s: %s" % (s.get("id"), s.get("mesh")))
    for p in sorted(glob.glob(os.path.join(ROOT, "assets", "skins", "*"))):
        if p.lower().endswith(MODEL_EXTS):
            used.add(os.path.normcase(os.path.abspath(p)))
            out.append(Model(model_id(os.path.splitext(os.path.basename(p))[0]), "character", p, fmt_of(p)))
    # The other FBX files next to the characters are animation clips (armatures, no mesh).
    for p in sorted(glob.glob(os.path.join(human, "*.fbx"))):
        if os.path.normcase(os.path.abspath(p)) not in used:
            out.append(Model(model_id(os.path.splitext(os.path.basename(p))[0]), "anim", p, "fbx"))


def scan_kits(out):
    kits = os.path.join(ROOT, "assets", "kits")
    for kd in sorted(glob.glob(os.path.join(kits, "*"))):
        if not os.path.isdir(kd):
            continue
        meta = load_json(os.path.join(kd, "kit.json")) if os.path.isfile(os.path.join(kd, "kit.json")) else {}
        pieces = meta.get("pieces", {})
        for p in sorted(glob.glob(os.path.join(kd, "*.glb")) + glob.glob(os.path.join(kd, "*.gltf"))):
            pid = os.path.splitext(os.path.basename(p))[0]
            src = os.path.join(kd, "src", pid + ".blend")   # a hand-made piece's Blender source wins over its export
            path = src if os.path.isfile(src) else p
            out.append(Model(pid, "kit", path, fmt_of(path), group=pieces.get(pid, {}).get("category", "other")))


def scan_dirs(out):
    """Every other model file under assets/: kind from DIR_KINDS, else the folder name."""
    assets = os.path.join(ROOT, "assets")
    for dp, dns, fns in os.walk(assets):
        rd = os.path.relpath(dp, assets).replace("\\", "/")
        if rd == ".":
            dns[:] = [d for d in dns if d not in SKIP_DIRS]
            continue
        kind, fixes = DIR_KINDS.get(rd, (None, None))
        if kind is None:
            top = rd.split("/")[0]
            kind = DIR_KINDS.get(top, (model_id(top), None))[0]
        for f in sorted(fns):
            ext = os.path.splitext(f)[1].lower()
            if ext in MODEL_EXTS:
                p = os.path.join(dp, f)
                out.append(Model(model_id(os.path.splitext(f)[0]), kind, p, fmt_of(p), fix=(fixes or {}).get(ext)))


def fmt_of(path):
    ext = os.path.splitext(path)[1].lower()
    return {".blend": "blend", ".fbx": "fbx", ".glb": "gltf", ".gltf": "gltf", ".obj": "obj"}[ext]


def registry():
    models, missing = [], []
    scan_items(models)
    scan_human(models, missing)
    scan_dirs(models)
    scan_kits(models)
    order = {k: i for i, (k, _) in enumerate(KINDS)}
    extra = sorted({m.kind for m in models if m.kind not in order})
    for k in extra:
        order[k] = len(order)
    models.sort(key=lambda m: (order[m.kind], m.group or "", m.id))
    return models, missing, [k for (k, _) in KINDS] + extra


def kind_label(k):
    return dict(KINDS).get(k, k.replace("_", " ").capitalize())


# ------------------------------------------------------------------ Blender

def blender_version_key(exe):
    m = re.search(r"Blender[ _-]?(\d+)\.(\d+)", exe, re.I)
    if m:
        return (int(m.group(1)), int(m.group(2)))
    d = os.path.dirname(exe)   # a portable / Steam install: its version folder sits next to the exe
    vs = [tuple(int(x) for x in n.split(".")) for n in os.listdir(d) if re.fullmatch(r"\d+\.\d+", n)] if os.path.isdir(d) else []
    return max(vs) if vs else (0, 0)


def find_blender(explicit):
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit("open_models: --blender %s: no such file" % explicit)
        return explicit
    env = os.environ.get("BLENDER")
    if env:
        if os.path.isfile(env):
            return env
        print("open_models: $BLENDER=%s is not a file, looking elsewhere" % env)
    found = []
    if os.name == "nt":
        bases = [os.environ.get(v) for v in ("ProgramFiles", "ProgramFiles(x86)", "ProgramW6432")]
        bases.append(os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs"))
        for b in filter(None, bases):
            found += glob.glob(os.path.join(b, "Blender Foundation", "Blender*", "blender.exe"))
            found += glob.glob(os.path.join(b, "Steam", "steamapps", "common", "Blender", "blender.exe"))
    elif sys.platform == "darwin":
        for b in ("/Applications", os.path.expanduser("~/Applications")):
            found += glob.glob(os.path.join(b, "Blender*.app", "Contents", "MacOS", "Blender"))
    else:
        found += glob.glob("/opt/blender*/blender") + glob.glob(os.path.expanduser("~/blender*/blender"))
    found = sorted(set(found), key=blender_version_key)
    if found:
        return found[-1]
    w = shutil.which("blender")
    if w:
        return w
    sys.exit("open_models: no Blender found: install it, set BLENDER=<path to blender.exe> or pass --blender <exe>")


def run_blender(blender, args, verbose, what):
    """Runs Blender headless, echoing its output (only this tool's lines and errors unless --verbose)."""
    cmd = [blender, "-b", "--factory-startup"] + args
    keep = re.compile(r"^(models:|FAILED|\w*Error\b|Error:|\S+\s+\d+ objects\s+model)")
    tail, in_tb = [], False
    t = time.time()
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    for raw in p.stdout:
        line = raw.decode("utf-8", "replace").rstrip()
        tail = (tail + [line])[-40:]
        in_tb = line.startswith("Traceback") or (in_tb and line[:1].isspace())
        if verbose or in_tb or keep.match(line):
            print("  " + line, flush=True)
    rc = p.wait()
    if rc != 0 and not verbose:
        print("open_models: %s: Blender exited with %d; its last lines:" % (what, rc))
        for line in tail:
            print("  | " + line)
    print("open_models: %s %.1fs" % (what, time.time() - t), flush=True)
    return rc


def open_gui(blender, target, item=False):
    """Starts the Blender window detached: the terminal (or the game) returns at once. An item's own scene
    opens with the export add-on (dg_item_addon.py)."""
    exe = blender
    launcher = os.path.join(os.path.dirname(blender), "blender-launcher.exe")
    if os.name == "nt" and os.path.isfile(launcher):
        exe = launcher   # no console window (it forwards the arguments to blender.exe)
    cmd = [exe, target, "--python", SCENE_SCRIPT] + (["--python", ADDON] if item else []) + ["--", "view"]
    print("open_models: opening %s in %s" % (rel(target), blender), flush=True)
    io = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT, close_fds=True)
    if os.name == "nt":
        flags = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
        try:
            subprocess.Popen(cmd, creationflags=flags | 0x01000000, **io)   # + CREATE_BREAKAWAY_FROM_JOB: outlives the terminal
        except OSError:
            subprocess.Popen(cmd, creationflags=flags, **io)
    else:
        subprocess.Popen(cmd, start_new_session=True, **io)


def model_dir(sel):
    """The folder of what is opened: the model's own, or the one the models share."""
    dirs = [os.path.dirname(os.path.abspath(m.path)) for m in sel]
    try:
        return os.path.commonpath(dirs)
    except ValueError:   # on different drives
        return dirs[0]


def copy_to_clipboard(text):
    """-> True when the text is on the clipboard (clip / pbcopy / wl-copy / xclip / xsel)."""
    if os.name == "nt":
        cmds, data = [["clip"]], text.encode("utf-16-le")   # clip takes UTF-16 LE (a BOM would be copied too)
    else:
        cmds = [["pbcopy"]] if sys.platform == "darwin" else [["wl-copy"], ["xclip", "-selection", "clipboard"], ["xsel", "-ib"]]
        data = text.encode("utf-8")
    for c in cmds:
        if not shutil.which(c[0]):
            continue
        try:
            if subprocess.run(c, input=data, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5).returncode == 0:
                return True
        except (OSError, subprocess.SubprocessError):
            pass
    return False


# ------------------------------------------------------------------ build / assemble

def mtime(p):
    try:
        return os.path.getmtime(p)
    except OSError:
        return 0.0


def stale_items(items, rebuild, from_script=False):
    """Items to build: missing or older than a source. An edited item (its .blend is the source) only with
    --from-script."""
    if from_script:
        return list(items)
    return [m for m in items if m.note != "edited" and (rebuild or mtime(m.path) == 0.0 or mtime(m.path) < max(mtime(d) for d in m.deps))]


def build_items(blender, items, rebuild, verbose, from_script=False):
    for m in items:
        if m.note == "edited" and not from_script:
            print("open_models: %s is edited in Blender: its .blend is the source (--from-script rebuilds it)" % m.id, flush=True)
    todo = stale_items(items, rebuild, from_script)
    if not todo:
        return []
    print("open_models: building %d item .blend file(s) headless: %s" % (len(todo), " ".join(m.id for m in todo)), flush=True)
    t0 = time.time() - 1.0
    run_blender(blender, ["-P", BUILD_ITEMS, "--"] + [m.id for m in todo] + (["--from-script"] if from_script else ["--blend-only"]), verbose,
                "item build")
    failed = [m for m in todo if mtime(m.path) < t0]
    for m in failed:
        old = " (an older .blend is used)" if mtime(m.path) else ""
        print("open_models: %s did not build%s" % (m.id, old))
    return failed


def cache_name(sel, whole):
    if whole is not None:   # 'all' or whole kinds
        return "all" + ("_" + "+".join(whole) if whole else "")
    ids = [m.id for m in sel]
    name = "sel_" + "+".join(ids)
    if len(name) > 60 or not re.fullmatch(r"[\w+.-]+", name):
        name = "sel_%d_%s" % (len(ids), hashlib.sha1("\n".join(m.qid for m in sel).encode()).hexdigest()[:8])
    return name


def prune_selections(keep=8):
    sels = sorted(glob.glob(os.path.join(CACHE, "sel_*.blend")), key=mtime, reverse=True)
    for p in sels[keep:]:
        for q in (p, p[:-6] + ".json"):
            try:
                os.remove(q)
            except OSError:
                pass


def assemble(blender, sel, name, rebuild, verbose):
    os.makedirs(CACHE, exist_ok=True)
    out = os.path.join(CACHE, name + ".blend")
    man_path = os.path.join(CACHE, name + ".json")
    ids = {}
    for m in sel:   # one collection per model: a plain id unless two kinds share it
        ids[m.id] = ids.get(m.id, 0) + 1
    man = {"out": out, "verbose": verbose, "models": [
        {"id": m.id if ids[m.id] == 1 else m.qid, "kind": m.kind, "kind_label": kind_label(m.kind), "group": m.group or m.kind,
         "path": m.path, "fmt": "blend" if m.fmt == "item" else m.fmt, "fix": m.fix} for m in sel]}
    newest = max([mtime(m.path) for m in sel] + [mtime(SCENE_SCRIPT)])
    try:
        same = load_json(man_path) == man
    except (ValueError, OSError):
        same = False
    if not rebuild and same and mtime(out) >= newest:
        print("open_models: %s is up to date (%d models)" % (rel(out), len(sel)))
        return out
    with open(man_path, "w", encoding="utf-8") as f:
        json.dump(man, f, indent=1)
    print("open_models: assembling %d models into %s" % (len(sel), rel(out)), flush=True)
    t = time.time()
    rc = run_blender(blender, ["-P", SCENE_SCRIPT, "--", "assemble", man_path], verbose, "assembly")
    if mtime(out) < t - 1.0:
        os.remove(man_path)
        sys.exit("open_models: the combined scene was not written (Blender exit %d)" % rc)
    return out


# ------------------------------------------------------------------ selection

def select(models, kinds, tokens, kinds_filter):
    """-> (models, whole): every token resolved (or an exit with suggestions); whole = None, or the kinds
    when the selection is 'all' / whole kinds only ([] = 'all' without --kind)."""
    chosen, errors = [], []
    whole = []
    if not tokens:
        tokens = ["all"]
    for tok in tokens:
        t = tok.lower()
        if t == "all":
            chosen += [m for m in models if (m.kind in kinds_filter if kinds_filter else m.kind not in NOT_IN_ALL)]
            whole = whole if whole is None else whole + list(kinds_filter)
            continue
        if t in kinds:
            chosen += [m for m in models if m.kind == t]
            whole = whole if whole is None else whole + [t]
            continue
        whole = None
        kind, pat = (t.split(":", 1) + [None])[:2] if ":" in t else (None, t)
        if kind is not None and kind not in kinds:
            errors.append("%s: unknown kind '%s' (kinds: %s)" % (tok, kind, ", ".join(kinds)))
            continue
        pool = [m for m in models if kind is None or m.kind == kind]
        if any(c in pat for c in "*?["):
            hit = [m for m in pool if fnmatch.fnmatchcase(m.id, pat)]
            if not hit:
                errors.append("%s: matches nothing" % tok)
            chosen += hit
            continue
        hit = [m for m in pool if m.id == pat]
        if len(hit) > 1:
            errors.append("%s is ambiguous: %s" % (tok, " ".join(m.qid for m in hit)))
        elif hit:
            chosen += hit
        else:
            close = difflib.get_close_matches(pat, sorted(set(m.id for m in pool)) + kinds, n=5, cutoff=0.5)
            close += [m.id for m in pool if pat in m.id and m.id not in close][:8 - len(close)]
            errors.append("unknown model '%s'%s" % (tok, (": did you mean " + ", ".join(close) + "?") if close else
                                                    " (open_models.py --list shows every id)"))
    if errors:
        for e in errors:
            print("open_models: " + e)
        sys.exit(2)
    seen, sel = set(), []
    for m in chosen:
        if m.qid not in seen:
            seen.add(m.qid)
            sel.append(m)
    if whole is not None and "all" in (t.lower() for t in tokens) and not kinds_filter:
        whole = []   # 'all' plus kinds is still everything
    if whole:
        whole = [k for k in kinds if k in whole]   # one name per set of kinds, in kind order
    return sel, whole


def print_list(models, kinds, missing, kinds_filter):
    width = shutil.get_terminal_size((100, 20)).columns - 1
    total = 0
    for k in kinds:
        if kinds_filter and k not in kinds_filter:
            continue
        ms = [m for m in models if m.kind == k]
        if not ms:
            continue
        total += len(ms)
        head = "%s (%d%s): " % (k, len(ms), ", not in 'all'" if k in NOT_IN_ALL else "")
        line, lines = head, []
        for m in ms:
            if len(line) + len(m.id) + 1 > width and line.strip():
                lines.append(line.rstrip())
                line = " " * 4
            line += m.id + " "
        lines.append(line.rstrip())
        print("\n".join(lines))
    print("%d models. Open: open_models.py <id ...> | <kind> | all [--kind K]" % total)
    for s in missing:
        print("missing source: " + s)


class SafeOut:
    """stdout that never raises: the reader (the game's blender_open) may be gone before the window opens."""

    def __init__(self, f):
        self.f = f

    def write(self, s):
        try:
            return self.f.write(s)
        except (OSError, ValueError):
            return len(s)

    def flush(self):
        try:
            self.f.flush()
        except (OSError, ValueError):
            pass

    def __getattr__(self, name):
        return getattr(self.f, name)


def main(argv):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    sys.stdout = SafeOut(sys.stdout)
    args, opts = [], {"kind": []}
    flags = {"--no-open", "--no-copy", "--rebuild", "--verbose", "--list", "--from-script"}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print(__doc__)
            return 0
        if a in flags:
            opts[a] = True
        elif a in ("--blender", "--kind"):
            if i + 1 >= len(argv):
                sys.exit("open_models: %s needs a value" % a)
            i += 1
            if a == "--kind":
                opts["kind"] += [k.strip().lower() for k in argv[i].split(",") if k.strip()]
            else:
                opts["blender"] = argv[i]
        elif a.startswith("--kind="):
            opts["kind"] += [k.strip().lower() for k in a[7:].split(",") if k.strip()]
        elif a.startswith("--blender="):
            opts["blender"] = a[10:]
        elif a.startswith("-"):
            print("open_models: unknown option %s\n" % a)
            print(__doc__)
            return 2
        else:
            args.append(a)
        i += 1
    models, missing, kinds = registry()
    for k in opts["kind"]:
        if k not in kinds:
            print("open_models: unknown kind '%s' (kinds: %s)" % (k, ", ".join(kinds)))
            return 2
    if opts.get("--list") or (args and args[0].lower() == "list"):
        print_list(models, kinds, missing, opts["kind"])
        return 0
    if not args and not opts["kind"]:
        print(__doc__)
        return 2
    sel, whole = select(models, kinds, args, opts["kind"])
    if not sel:
        print("open_models: nothing selected")
        return 2
    blender = find_blender(opts.get("blender"))
    t0 = time.time()
    rc = 0
    items = [m for m in sel if m.fmt == "item"]
    failed = build_items(blender, items, opts.get("--rebuild"), opts.get("--verbose"), opts.get("--from-script")) if items else []
    if failed:
        rc = 1
        dropped = [m for m in failed if mtime(m.path) == 0.0]
        sel = [m for m in sel if m not in dropped]
        if not sel:
            return 1
    if len(sel) == 1 and sel[0].fmt in ("item", "blend"):
        target = sel[0].path   # its own scene: edits there are the ones build_items.py --blend exports
    else:
        target = assemble(blender, sel, cache_name(sel, whole), opts.get("--rebuild"), opts.get("--verbose"))
        prune_selections()
    print("open_models: ready in %.1fs: %s" % (time.time() - t0, rel(target)), flush=True)
    if not opts.get("--no-open"):
        open_gui(blender, target, len(sel) == 1 and sel[0].fmt == "item")
        if not opts.get("--no-copy"):
            d = model_dir(sel)
            print("open_models: %s %s" % ("copied to the clipboard:" if copy_to_clipboard(d) else "could not copy to the clipboard:", d), flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
