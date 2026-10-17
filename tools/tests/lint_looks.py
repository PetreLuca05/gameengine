#!/usr/bin/env python3
"""Looks lint (docs/LOOKS.md): the five Looks are fixed, compiled-in constants.

  1. no disk I/O: the look sources never open, read or write a file or parse JSON
     (src/render/post/look.c, looks.c, look.h, post_look.c, src/game/game_look.c)
  2. exactly five tables in looks.c (static const), LOOK_COUNT 5
  3. the dev tools (look_tune, look_dump, look_format, look_fields, the tuned override) exist only
     inside `#if DG_LOOK_TUNE` blocks, and CMakeLists.txt turns DG_LOOK_TUNE off for Release builds
  4. nothing but game_look.c sets r_look (the one persisted Look setting)
  5. the LUT shaper of shaders/post/look.glsl equals look.h (LOOK_EV_MIN / MAX, LOOK_LUT_N)

Exit 1 on a failure. Run: python3 tools/tests/lint_looks.py
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SOURCES = ["src/render/post/look.c", "src/render/post/looks.c", "src/render/post/look.h", "src/render/post/post_look.c", "src/game/game_look.c"]
IO = re.compile(r"\b(fopen|fread|fwrite|fprintf|fputs|_wfopen|CreateFile\w*|cJSON_\w+|stbi_\w+|platform_read\w*|platform_write\w*|file_read\w*|"
                r"file_write\w*|config_save|config_load|json_\w+)\s*\(")
DEV = re.compile(r"\b(look_tune_edit|look_tune_reset|look_tune_dirty|look_format|look_fields|LookField|s_tuned\w*|cmd_look_tune|cmd_look_dump|"
                 r"\"look_tune\"|\"look_dump\")")
failures = []


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def dev_lines(src):
    """Line numbers (1-based) inside `#if DG_LOOK_TUNE` (not its #else)."""
    inside, stack = set(), []
    for n, line in enumerate(src.splitlines(), 1):
        s = line.strip()
        if s.startswith("#if"):
            stack.append("DG_LOOK_TUNE" in s and "!" not in s)
        elif s.startswith("#else") and stack:
            stack[-1] = False if stack[-1] else stack[-1]
        elif s.startswith("#endif") and stack:
            stack.pop()
        elif any(stack):
            inside.add(n)
    return inside


for rel in SOURCES:
    src = strip_comments(read(rel))
    for n, line in enumerate(src.splitlines(), 1):
        m = IO.search(line)
        if m:
            failures.append("%s:%d: file / JSON I/O in a look source (%s)" % (rel, n, m.group(1)))
    dev = dev_lines(src)
    for n, line in enumerate(src.splitlines(), 1):
        m = DEV.search(line)
        if m and n not in dev:
            failures.append("%s:%d: dev-only %s outside #if DG_LOOK_TUNE" % (rel, n, m.group(1)))

looks = strip_comments(read("src/render/post/looks.c"))
tables = re.search(r"static const LookParams k_looks\[LOOK_COUNT\] = \{(.*?)\n\};", looks, re.S)
ids = re.findall(r'\.id = "(\w+)"', tables.group(1)) if tables else []
if ids != ["realism", "pubg", "cinematic", "vibrant", "gritty"]:
    failures.append("src/render/post/looks.c: expected the five static const tables realism, pubg, cinematic, vibrant, gritty, found %s" % ids)
if not re.search(r"LOOK_GRITTY, LOOK_COUNT \}", read("src/render/post/look.h")):
    failures.append("src/render/post/look.h: LookId must end LOOK_GRITTY, LOOK_COUNT (five looks)")

# The GPU shaper (shaders/post/look.glsl) must match the CPU bake (look.h).
hdr, glsl = read("src/render/post/look.h"), read("shaders/post/look.glsl")
for c_name, g_name in (("LOOK_EV_MIN", "LOOK_EV_MIN"), ("LOOK_EV_MAX", "LOOK_EV_MAX"), ("LOOK_LUT_N", "LOOK_LUT_N")):
    cv = re.search(r"#define %s \(?(-?[\d.]+)f?\)?" % c_name, hdr)
    gv = re.search(r"const float %s = (-?[\d.]+);" % g_name, glsl)
    if not cv or not gv or float(cv.group(1)) != float(gv.group(1)):
        failures.append("shaders/post/look.glsl: %s %s != look.h %s" % (g_name, gv.group(1) if gv else "?", cv.group(1) if cv else "?"))

cmake = read("CMakeLists.txt")
if not re.search(r"\$<\$<NOT:\$<CONFIG:Release>>:DG_LOOK_TUNE=1>", cmake):
    failures.append("CMakeLists.txt: DG_LOOK_TUNE must be defined for non-Release configurations only")

for dp, _, files in os.walk(os.path.join(ROOT, "src")):
    for f in files:
        if not f.endswith(".c"):
            continue
        rel = os.path.relpath(os.path.join(dp, f), ROOT).replace("\\", "/")
        if rel == "src/game/game_look.c":
            continue
        src = strip_comments(read(rel))
        if re.search(r'cvar_set\w*\([^)]*"r_look"', src) or re.search(r'cvar_find\("r_look"\)', src):
            failures.append("%s: sets r_look (only game_look.c owns it; use game_look_set)" % rel)

for f in failures:
    print("FAIL " + f)
print("lint_looks: %d source(s), %d look table(s), %d failure(s)" % (len(SOURCES), len(ids), len(failures)))
sys.exit(1 if failures else 0)
