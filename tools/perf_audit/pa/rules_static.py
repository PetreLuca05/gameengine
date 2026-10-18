"""Static detection rules: source, shader and asset scans (no profile needed). They find candidates the
runtime rules then measure; their savings are estimates until a profile confirms them."""
import hashlib
import os
import re
import struct

from .findings import Finding

HOT = re.compile(r"(frame|tick|update|render|draw|record|step|exec|cull|submit|pump|present|_hud|hud_)", re.I)
COLD = re.compile(r"(init|load|create|build|bake|import|set_level|make|reload|shutdown|destroy|register|parse|read_|write_|save|cmd_|test|bench|dump|"
                  r"editor|preview|migrate|_level|level_|start|begin_run|alloc|library|debug|dbg)", re.I)
FUNC = re.compile(r"^(?:static\s+)?(?:inline\s+)?[\w\s\*]+?\b(\w+)\s*\(([^;{}]*)\)\s*\{", re.M)
ALLOC = re.compile(r"\b(malloc|calloc|realloc|SDL_malloc|SDL_calloc|SDL_realloc|strdup)\s*\(")
LOG = re.compile(r"^\s{4}(log_(?:info|debug|warn))\s*\(", re.M)
CVAR_FIND = re.compile(r"\bcvar_find\s*\(\s*\"([\w]+)\"")
DEBUG_FRAME = re.compile(r"^\s{4}(dbg_\w+)\s*\(", re.M)

RULES = []


def rule(rid, title):
    def deco(fn):
        fn.rid, fn.title, fn.kind = rid, title, "static"
        RULES.append(fn)
        return fn
    return deco


def functions(root):
    """(relpath, name, body, first line) of every C function under root/src."""
    for dp, _, files in os.walk(os.path.join(root, "src")):
        for f in sorted(files):
            if not f.endswith(".c"):
                continue
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, root).replace("\\", "/")
            s = open(path, encoding="utf-8", errors="replace").read()
            ms = list(FUNC.finditer(s))
            for i, m in enumerate(ms):
                end = ms[i + 1].start() if i + 1 < len(ms) else len(s)
                yield rel, m.group(1), s[m.end():end], s.count("\n", 0, m.start()) + 1


def hot(name):
    return HOT.search(name) and not COLD.search(name)


@rule("R08", "Allocations in per-frame functions (static)")
def s08_alloc(root, ctx):
    out = []
    for rel, name, body, line in functions(root):
        if not hot(name):
            continue
        for m in ALLOC.finditer(body):
            ln = line + body.count("\n", 0, m.start())
            ctxline = body[max(0, m.start() - 160):m.start()]
            fixed = "prof_fix_enabled" in ctxline or "scratch" in ctxline
            out.append(Finding("R08", "%s:%s" % (rel, name), "%s() calls %s in the frame loop%s" % (name, m.group(1), " (fixed: persistent buffer)" if fixed else ""),
                               "core/alloc", {"file": rel, "line": ln, "function": name}, {"static": {"call": m.group(1), "fixed": fixed}},
                               {"cpu_ms": 0.0005}, "medium", "S",
                               "Keep the buffer for the owner's lifetime (sized at init) or take it from g_engine.frame_arena.",
                               source="static", scene="static"))
            break
    return out


@rule("R19", "Unconditional logging in per-frame functions (static)")
def s19_log(root, ctx):
    out = []
    for rel, name, body, line in functions(root):
        if not hot(name):
            continue
        for m in LOG.finditer(body):
            ln = line + body.count("\n", 0, m.start())
            out.append(Finding("R19", "%s:%s" % (rel, name), "%s() logs unconditionally (%s) at its top level" % (name, m.group(1)), "core/log",
                               {"file": rel, "line": ln, "function": name}, {"static": {"call": m.group(1)}}, {"cpu_ms": 0.01}, "low", "S",
                               "Check it is not reached every frame; log on change / once.", source="static", scene="static", below_noise=True))
            break
    return out


@rule("R26", "Name lookups (cvar_find) in per-frame functions (static)")
def s26_cvar_find(root, ctx):
    out = []
    for rel, name, body, line in functions(root):
        if not hot(name):
            continue
        names = CVAR_FIND.findall(body)
        if names:
            out.append(Finding("R26", "%s:%s" % (rel, name), "%s() looks up %d cvar(s) by name: %s" % (name, len(names), ", ".join(sorted(set(names)))),
                               "core/cvar", {"file": rel, "line": line, "function": name}, {"static": {"cvars": sorted(set(names))}},
                               {"cpu_ms": 0.0025 * len(names)}, "high", "S",
                               "Cache the Cvar* at init (the hash index of PF-0002 already makes each lookup cheap).", source="static", scene="static",
                               below_noise=True))
    return out


@rule("R20", "Debug drawing reached unconditionally from per-frame code (static)")
def s20_debug(root, ctx):
    out = []
    for rel, name, body, line in functions(root):
        if not hot(name) or "debug" in rel or "editor" in rel:
            continue
        m = DEBUG_FRAME.search(body)
        if m:
            out.append(Finding("R20", "%s:%s" % (rel, name), "%s() calls %s at its top level every frame" % (name, m.group(1)), "debug",
                               {"file": rel, "function": name, "line": line + body.count("\n", 0, m.start())}, {"static": {"call": m.group(1)}},
                               {"cpu_ms": 0.005}, "low", "S", "Gate it on its debug cvar (or #if DG_DEV) so release frames skip it.",
                               source="static", scene="static", below_noise=True))
    return out


@rule("R17", "Audio occlusion rays per frame (static)")
def s17_audio(root, ctx):
    path = os.path.join(root, "src", "audio", "audio.c")
    if not os.path.exists(path):
        return []
    s = open(path, encoding="utf-8", errors="replace").read()
    if "raycast" in s or "phys_ray" in s:
        return [Finding("R17", "audio_rays", "Audio casts occlusion rays (check the rate per voice)", "audio", {"file": "src/audio/audio.c"}, {}, {},
                        "low", "M", "Amortize: one voice per frame, round robin; cache by listener cell.", source="static", scene="static")]
    return []   # no occlusion rays in the engine: not applicable (docs/PERF_AUDIT.md coverage)


OUT_DECL = re.compile(r"layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*out\s+\w+\s+(\w+)\s*;")
UNIFORM_IF = re.compile(r"\bif\s*\(\s*(?:pc|push|u|frame|ubo|params)\.\w+")


@rule("R21", "Shader outputs never written / uniform branches (static)")
def s21_shaders(root, ctx):
    out = []
    sroot = os.path.join(root, "shaders")
    for dp, _, files in os.walk(sroot):
        for f in sorted(files):
            if not f.endswith((".frag", ".comp", ".vert", ".mesh")):
                continue
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, root).replace("\\", "/")
            s = open(path, encoding="utf-8", errors="replace").read()
            code = re.sub(r"//[^\n]*", "", s)
            for m in OUT_DECL.finditer(code):
                var = m.group(2)
                rest = code[m.end():]
                if not re.search(r"\b%s\b" % re.escape(var), rest):   # never referenced again (an out argument counts as a write)
                    out.append(Finding("R21", "%s:%s" % (rel, var), "%s declares output '%s' (location %s) and never references it" % (rel, var, m.group(1)),
                                       "shaders", {"file": rel}, {"static": {"output": var}}, {"gpu_ms": 0.01}, "medium", "S",
                                       "Drop the output (and the attachment it feeds when no other pipeline writes it).", source="static",
                                       scene="static", below_noise=True))
            n = len(UNIFORM_IF.findall(code))
            if f.endswith(".frag") and n >= 6:
                out.append(Finding("R21", "%s:uniform_branches" % rel, "%s branches %d times on uniform values" % (rel, n), "shaders", {"file": rel},
                                   {"static": {"uniform_ifs": n}}, {"gpu_ms": 0.01}, "low", "M",
                                   "Uniform branches are coherent (cheap) on current GPUs; a permutation only pays when the branch keeps registers "
                                   "live (occupancy): measure the pass's fs invocations / ms before splitting.", source="static", scene="static",
                                   below_noise=True))
    return out


def png_size(path):
    with open(path, "rb") as f:
        h = f.read(24)
    if h[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", h[16:24])


@rule("R14", "Textures stored uncompressed / without mips / oversized (static, trade-offs)")
def s14_textures(root, ctx):
    out = []
    big = []
    for dp, _, files in os.walk(os.path.join(root, "assets")):
        if os.sep + "ui" in dp or "icons" in dp or "web" in dp:
            continue
        for f in files:
            if not f.lower().endswith(".png"):
                continue
            p = os.path.join(dp, f)
            sz = png_size(p)
            if not sz:
                continue
            w, h = sz
            if w * h >= 2048 * 2048 and not os.path.exists(os.path.splitext(p)[0] + ".ktx2"):
                big.append((w * h, os.path.relpath(p, root).replace("\\", "/"), w, h))
    big.sort(reverse=True)
    for px, rel, w, h in big[:8]:
        mb = px * 4 * 1.33 / 1048576.0
        out.append(Finding("R14", rel, "%s: %dx%d RGBA8 PNG (%.0f MB with mips in VRAM), no BC sibling" % (rel, w, h, mb), "assets/textures",
                           {"file": rel}, {"static": {"w": w, "h": h}}, {"mb": mb * 0.75}, "medium", "M",
                           "BC7 / BC5 compression (or a smaller size where the texel density is never reached). Lossy: changes the image.",
                           fidelity="tradeoff", source="static", scene="static", verify="changes the image: not a fidelity-neutral fix"))
    return out


@rule("R22", "Byte-identical assets stored (and loaded) more than once (static)")
def s22_duplicates(root, ctx):
    by = {}
    for dp, _, files in os.walk(os.path.join(root, "assets")):
        for f in files:
            p = os.path.join(dp, f)
            try:
                if os.path.getsize(p) < 256 * 1024:
                    continue
                with open(p, "rb") as fh:
                    hsh = hashlib.sha1(fh.read()).hexdigest()
            except OSError:
                continue
            by.setdefault(hsh, []).append(os.path.relpath(p, root).replace("\\", "/"))
    groups = {}
    for hsh, paths in sorted(by.items()):
        if len(paths) < 2:
            continue
        key = " / ".join(sorted({os.path.dirname(x) for x in paths}))
        groups.setdefault(key, []).append(sorted(paths))
    out = []
    for key, sets in sorted(groups.items()):
        mb = sum(os.path.getsize(os.path.join(root, ps[0])) / 1048576.0 * (len(ps) - 1) for ps in sets)
        names = [os.path.basename(ps[0]) for ps in sets]
        out.append(Finding("R22", key, "%d byte-identical files duplicated across %s (%.1f MB): %s" % (len(sets), key, mb, ", ".join(names[:5])), "assets",
                           {"file": key}, {"static": {"files": names}}, {"mb": mb}, "low", "S",
                           "Reference one copy. If only one directory is ever loaded (check the asset cache keys), the duplicate costs disk / "
                           "package size only.", source="static", scene="static"))
    return out


def run(root, ctx=None):
    ctx = ctx or {}
    out = []
    for r in RULES:
        out.extend(r(root, ctx))
    return out
