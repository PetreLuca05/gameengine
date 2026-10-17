#!/usr/bin/env python3
"""Console registry lint (docs/UI_WHEELS_HUD.md "Commands tab"): every cmd_register / cvar_reg*
call in src/ needs a literal name, a description (>= 8 characters) and a category from
assets/ui/command_categories.json (exact name, else the longest prefix). Exit 1 on a failure.
Run: python3 tools/tests/lint_commands.py [--list]"""
import json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CALL = re.compile(r"(?<![\w.>])(cmd_register|cvar_reg_bool|cvar_reg_int|cvar_reg_float|cvar_reg_string|cvar_reg)\s*\(")
STR = re.compile(r'"((?:[^"\\]|\\.)*)"')
# Registrations whose name is built at run time (each documented where it is made).
DYNAMIC_OK = {("src/input/input.c", "press action"), ("src/input/input.c", "release action"), ("src/core/cvar.c", None)}


def call_args(s, i):
    depth, j = 1, i
    while depth and j < len(s):
        c = s[j]
        if c == '"':
            j += 1
            while s[j] != '"':
                j += 2 if s[j] == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        j += 1
    return s[i:j - 1]


def registrations():
    for dp, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in sorted(files):
            if not f.endswith(".c"):
                continue
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, ROOT).replace("\\", "/")
            src = open(path, encoding="utf-8", errors="replace").read()
            for m in CALL.finditer(src):
                args = call_args(src, m.end())
                # Skip the definitions themselves (cvar.c: "Cvar *cvar_reg_bool(const char *name, ...)").
                if "const char *name" in args:
                    continue
                line = src.count("\n", 0, m.start()) + 1
                name = re.match(r'\s*"([^"]*)"', args)
                strs = STR.findall(args)
                yield rel, line, m.group(1), name.group(1) if name else None, strs[-1] if len(strs) >= (1 if not name else 2) else None


def load_categories():
    cfg = json.load(open(os.path.join(ROOT, "assets", "ui", "command_categories.json"), encoding="utf-8"))
    return cfg


def category_of(cfg, name):
    if name in cfg.get("names", {}):
        return cfg["names"][name]
    best, best_len = None, -1
    for c in cfg["categories"]:
        for p in c.get("prefixes", []):
            if name.startswith(p) and len(p) > best_len:
                best, best_len = c["id"], len(p)
    return best


def main():
    cfg = load_categories()
    ids = {c["id"] for c in cfg["categories"]}
    fails, n, by_cat = [], 0, {}
    for v in cfg.get("names", {}).values():
        if v not in ids:
            fails.append("command_categories.json: \"names\" uses unknown category %s" % v)
    for rel, line, kind, name, desc in registrations():
        n += 1
        if name is None:
            hint = (desc or "").split(" (")[0]
            if (rel, hint) in DYNAMIC_OK or (rel, None) in DYNAMIC_OK:
                continue
            fails.append("%s:%d: %s with a non-literal name" % (rel, line, kind))
            continue
        if not desc or len(desc.strip()) < 8:
            fails.append("%s:%d: %s \"%s\": missing / too short description" % (rel, line, kind, name))
        cat = category_of(cfg, name)
        if not cat:
            fails.append("%s:%d: %s \"%s\": no category (assets/ui/command_categories.json)" % (rel, line, kind, name))
        by_cat[cat] = by_cat.get(cat, 0) + 1
        if "--list" in sys.argv:
            print("%-12s %-32s %s" % (cat, name, desc))
    for f in fails:
        print("lint_commands: " + f)
    print("lint_commands: %d registrations, %d failure(s); per category: %s" % (n, len(fails), ", ".join("%s %d" % (k, v) for k, v in sorted(by_cat.items(), key=lambda x: str(x[0])))))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
