#!/usr/bin/env python3
"""Settings audit lint (docs/SETTINGS_AUDIT.md). Fails (exit 1) when:
  * a cvar_reg* call has no classification flag, or more than one (CVAR_EDITABLE / CVAR_TIER /
    CVAR_LOCKED; CVAR_RULE only with CVAR_LOCKED);
  * a TIER / LOCKED registration is archived (only player settings and the host's rules are saved);
  * a registration is missing from assets/ui/settings_audit.json, its class there differs from the
    flags, or the file names a cvar nobody registers;
  * assets/render/consistency.json names a cvar that is not CVAR_LOCKED, or a value without a reason;
  * a visibility table varies by level: lod.json tiers / eras other than 1, a grass asset whose
    per-level fades / density differ, a fidelity level table entry for a locked parameter;
  * the settings screen shows a cvar that is not CVAR_EDITABLE outside the dev-only Graphics > Debug
    page / developer rows, or the Graphics tab has groups other than Engine Fidelity / Display /
    Comfort;
  * docs/SETTINGS_AUDIT.md's table is out of date.
--write regenerates the doc table and the categories in settings_audit.json (run it after adding a
cvar, once its entry is in settings_audit.json). --list prints the table.
Run: python3 tools/tests/lint_settings.py [--write] [--list]"""
import glob, json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lint_commands as LC  # noqa: E402

CALL = re.compile(r"(?<![\w.>])(cvar_reg_bool|cvar_reg_int|cvar_reg_float|cvar_reg_string|cvar_reg)\s*\(")
IDX = {"cvar_reg_bool": 2, "cvar_reg_string": 2, "cvar_reg_int": 4, "cvar_reg_float": 4, "cvar_reg": 3}
CLASSES = ("EDITABLE", "TIER", "LOCKED", "LOCKED+RULE")
AUDIT = os.path.join(ROOT, "assets", "ui", "settings_audit.json")
DOC = os.path.join(ROOT, "docs", "SETTINGS_AUDIT.md")
TABLE_HEAD = "## 8. Full classification"
# Locked rows the settings screen may show inside developer-only blocks (dev builds keep them writable).
DEV_ROWS = {"cl_thirdperson"}
# Cvar names the screen mentions for other reasons (resolution read-back, server FOV override).
NOT_ROWS = {"sv_fov", "sv_fov_max", "developer", "r_fidelity_custom", "r_width", "r_height", "r_mode", "r_vsync", "r_upscaler", "r_motion_blur", "r_hdr_output",
            "r_volumetrics", "r_dof", "r_bloom", "r_shadow_cascades", "r_shadow_split2", "r_shadow_split4_1", "r_shadow_split4_2", "r_shadow_split4_3"}


def split_args(s, i):
    spans, depth, j, st = [], 0, i, i
    while j < len(s):
        c = s[j]
        if c == '"':
            j += 1
            while s[j] != '"':
                j += 2 if s[j] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                spans.append((st, j))
                return spans
            depth -= 1
        elif c == "," and depth == 0:
            spans.append((st, j))
            st = j + 1
        j += 1
    return spans


def registrations():
    """(rel, line, name, set of flag tokens)"""
    out = []
    for dp, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in sorted(files):
            if not f.endswith(".c"):
                continue
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, ROOT).replace("\\", "/")
            src = open(path, encoding="utf-8", errors="replace").read()
            for m in CALL.finditer(src):
                spans = split_args(src, m.end())
                first = src[spans[0][0]:spans[0][1]]
                if "const char *name" in first:
                    continue   # the definitions in cvar.c
                name = re.match(r'\s*"([^"]*)"', first)
                if not name:
                    continue   # dynamic names (lint_commands covers them)
                k = IDX[m.group(1)]
                expr = src[spans[k][0]:spans[k][1]] if k < len(spans) else ""
                toks = {t.strip() for t in expr.split("|") if t.strip()}
                if "A" in toks:   # game_post.c / game_fidelity.c: const u32 A = CVAR_ARCHIVE
                    toks.discard("A")
                    toks.add("CVAR_ARCHIVE")
                out.append((rel, src.count("\n", 0, m.start()) + 1, name.group(1), toks))
    return out


def class_of(toks):
    c = [t for t in ("CVAR_EDITABLE", "CVAR_TIER", "CVAR_LOCKED") if t in toks]
    if len(c) != 1:
        return None
    k = c[0][5:]
    return "LOCKED+RULE" if k == "LOCKED" and "CVAR_RULE" in toks else k


def doc_table(audit):
    rows = ["| Cvar | Category | Class | UI after the rebuild | Locked in release | Reason |", "|---|---|---|---|---|---|"]
    order = {c: i for i, c in enumerate(CLASSES)}
    for name, e in sorted(audit.items(), key=lambda kv: (order.get(kv[1]["class"], 9), kv[1].get("category", ""), kv[0])):
        cls = e["class"]
        locked = {"EDITABLE": "no", "TIER": "yes (fidelity writes)", "LOCKED": "yes", "LOCKED+RULE": "yes (+ server rule)"}[cls]
        rows.append("| `%s` | %s | %s | %s | %s | %s |" % (name, e.get("category", "?"), cls, e["ui"], locked, e["reason"].replace("|", "/")))
    counts = {c: sum(1 for e in audit.values() if e["class"] == c) for c in CLASSES}
    intro = ("%d cvars: %d EDITABLE, %d TIER, %d LOCKED, %d LOCKED + host rule. Generated by `tools/tests/lint_settings.py --write` from "
             "`assets/ui/settings_audit.json` (the class must match each registration's flag; `settings_audit` prints the same table in the game). "
             "**UI after the rebuild** \"—\" means the cvar is in no settings tab." % (len(audit), counts["EDITABLE"], counts["TIER"], counts["LOCKED"], counts["LOCKED+RULE"]))
    return intro + "\n\n" + "\n".join(rows) + "\n"


def func_body(src, name):
    m = re.search(r"\n(static )?\w[\w \*]*\b%s\([^)]*\) \{\n" % re.escape(name), src)
    if not m:
        return ""
    i, depth = m.end(), 1
    while i < len(src) and depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    return src[m.end():i]


def strip_dev_blocks(body):
    """Removes `if (dev_mode()) { ... }` blocks (developer rows)."""
    out, i = [], 0
    while True:
        m = re.search(r"if \(dev_mode\(\)\) \{", body[i:])
        if not m:
            out.append(body[i:])
            return "".join(out)
        a = i + m.start()
        out.append(body[i:a])
        j, depth = i + m.end(), 1
        while j < len(body) and depth:
            depth += {"{": 1, "}": -1}.get(body[j], 0)
            j += 1
        i = j


def main():
    write = "--write" in sys.argv
    fails = []
    regs = registrations()
    data = json.load(open(AUDIT, encoding="utf-8"))
    audit = data["cvars"]
    cats = LC.load_categories()
    seen = {}
    for rel, line, name, toks in regs:
        if name in seen:
            continue
        cls = class_of(toks)
        seen[name] = cls
        where = "%s:%d: %s" % (rel, line, name)
        if not cls:
            fails.append("%s: needs exactly one of CVAR_EDITABLE / CVAR_TIER / CVAR_LOCKED (has %s)" % (where, " | ".join(sorted(toks)) or "0"))
            continue
        if "CVAR_RULE" in toks and "CVAR_LOCKED" not in toks:
            fails.append("%s: CVAR_RULE without CVAR_LOCKED" % where)
        if cls in ("TIER", "LOCKED") and "CVAR_ARCHIVE" in toks:
            fails.append("%s: a %s cvar is never saved (drop CVAR_ARCHIVE)" % (where, cls))
        e = audit.get(name)
        if not e:
            fails.append("%s: not in assets/ui/settings_audit.json (add its class, UI place and reason)" % where)
        elif e.get("class") != cls:
            fails.append("%s: flags say %s, settings_audit.json says %s" % (where, cls, e.get("class")))
        elif len(e.get("reason", "")) < 8:
            fails.append("%s: settings_audit.json needs a reason" % where)
        if e:
            e["category"] = LC.category_of(cats, name) or "?"
    for name in sorted(set(audit) - set(seen)):
        fails.append("settings_audit.json: %s is not registered anywhere" % name)

    # consistency set
    cons = json.load(open(os.path.join(ROOT, "assets", "render", "consistency.json"), encoding="utf-8"))["values"]
    for name, e in cons.items():
        if seen.get(name) not in ("LOCKED", "LOCKED+RULE"):
            fails.append("consistency.json: %s is %s, not CVAR_LOCKED" % (name, seen.get(name, "unregistered")))
        if len(e.get("why", "")) < 8:
            fails.append("consistency.json: %s needs a \"why\"" % name)

    # visibility tables: the same at every level
    lod = json.load(open(os.path.join(ROOT, "assets", "render", "lod.json"), encoding="utf-8"))
    for k, v in lod.get("tiers", {}).items():
        if not k.startswith("_") and abs(float(v) - 1.0) > 1e-6:
            fails.append("lod.json: tiers.%s = %s (must be 1: the same ranges at every level)" % (k, v))
    for era, d in lod.get("eras", {}).items():
        if era.startswith("_"):
            continue
        for k, v in d.items():
            if abs(float(v) - 1.0) > 1e-6:
                fails.append("lod.json: eras.%s.%s = %s (must be 1)" % (era, k, v))
    for p in sorted(glob.glob(os.path.join(ROOT, "assets", "foliage", "grass", "*.json"))):
        g = json.load(open(p, encoding="utf-8"))
        t = g.get("tiers")
        if t and any(t[l] != t["modern"] for l in t):
            fails.append("%s: per-level fades / density differ (grass hides players: every level = modern)" % os.path.relpath(p, ROOT))
    locked_cvars = {n for n, c in seen.items() if c in ("LOCKED", "LOCKED+RULE")}
    fsrc = open(os.path.join(ROOT, "src", "render", "fidelity", "fidelity.c"), encoding="utf-8").read()
    feat_cvar = dict(re.findall(r'\[(FF_\w+)\] = \{ "\w+", "(\w+)"', fsrc))
    for ff, cvar in feat_cvar.items():
        if cvar in locked_cvars:
            fails.append("fidelity.c: %s (%s) is CVAR_LOCKED: a locked value cannot be a fidelity feature" % (ff, cvar))
        elif seen.get(cvar) not in ("TIER", "EDITABLE"):
            fails.append("fidelity.c: %s (%s) must be CVAR_TIER or CVAR_EDITABLE" % (ff, cvar))

    # the settings screen
    ui = open(os.path.join(ROOT, "src", "ui", "screens", "scr_settings.c"), encoding="utf-8").read()
    gfx = func_body(ui, "tab_graphics_normal")
    groups = re.findall(r'section\(c, "([^"]+)"\)', "".join(func_body(ui, f) for f in ("graphics_display", "graphics_comfort", "fidelity_card")))
    calls = re.findall(r"(\w+)\(c\);", gfx)
    if calls != ["graphics_header", "fidelity_card", "graphics_display", "graphics_comfort"]:
        fails.append("scr_settings.c: the Graphics tab must be header, Engine Fidelity, Display, Comfort (has %s)" % ", ".join(calls))
    if sorted(set(groups)) != ["Comfort", "Display", "Engine Fidelity"]:
        fails.append("scr_settings.c: Graphics groups must be Engine Fidelity / Display / Comfort (has %s)" % ", ".join(sorted(set(groups))))
    shown = ""
    for fn in re.findall(r"\nstatic void (\w+)\(GuiCursor \*c\)", ui):
        if fn.startswith("row_") or fn == "section":
            continue
        shown += strip_dev_blocks(func_body(ui, fn))
    for name in sorted(set(re.findall(r'"([a-z][a-z0-9_]+)"', shown))):
        if name in seen and name not in NOT_ROWS and name not in DEV_ROWS and seen[name] != "EDITABLE":
            fails.append("scr_settings.c: shows %s (%s), not a player setting" % (name, seen[name]))
    for ff in sorted(set(re.findall(r"\b(FF_\w+)\b", shown))):
        cvar = feat_cvar.get(ff)
        if cvar and seen.get(cvar) != "EDITABLE":
            fails.append("scr_settings.c: shows %s (%s, %s), not a player setting" % (ff, cvar, seen.get(cvar)))

    # the doc table
    table = doc_table(audit)
    doc = open(DOC, encoding="utf-8").read()
    i = doc.find(TABLE_HEAD)
    if i < 0:
        fails.append("docs/SETTINGS_AUDIT.md: no '%s' section" % TABLE_HEAD)
    else:
        nl = doc.index("\n", i) + 1
        new_doc = doc[:nl] + "\n" + table
        if write:
            open(DOC, "w", encoding="utf-8", newline="\n").write(new_doc)
            data["cvars"] = dict(sorted(audit.items()))
            json.dump(data, open(AUDIT, "w", encoding="utf-8", newline="\n"), indent=1, ensure_ascii=False)
            print("lint_settings: wrote %s and %s" % (os.path.relpath(DOC, ROOT), os.path.relpath(AUDIT, ROOT)))
        elif new_doc != doc:
            fails.append("docs/SETTINGS_AUDIT.md: the table is out of date (python3 tools/tests/lint_settings.py --write)")
    if "--list" in sys.argv:
        print(table)
    for f in fails:
        print("lint_settings: " + f)
    counts = {c: sum(1 for v in seen.values() if v == c) for c in CLASSES}
    print("lint_settings: %d cvars (%s), %d consistency values, %d failure(s)" % (len(seen), ", ".join("%s %d" % (k, v) for k, v in counts.items()), len(cons), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
