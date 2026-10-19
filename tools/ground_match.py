#!/usr/bin/env python3
"""Ground match (docs/GROUND_REFERENCE.md "Comparing"): comparison sheets and the match report.

    py tools/ground_match.py blades              docs/ground_match/blade_compare_<atlas>.png: the reference crop with
                                                 its traced blades, our cells with their fan envelope over the
                                                 reference's, root -> tip value / hue profiles (reference vs ours)
    py tools/ground_match.py terrain             docs/ground_match/terrain_compare.png: each reference ground region
                                                 beside our layer for it (texture crop at the reference's scale),
                                                 mean colour swatches, hue / saturation / value deltas
    py tools/ground_match.py cfg                 tools/tests/ground_match.cfg: the presets of views.json captured
                                                 (ground_ref_cam conditions) as ground_match_<r>.png
    py tools/ground_match.py report <shots dir>  docs/ground_match/report.md + compare_<r>.jpg: our captures' region
                                                 statistics against the reference regions (hue / saturation /
                                                 value deltas, tolerances), the texture checks, the grass spec

Tolerances (lit frame, exposure independent first): hue 8 deg, saturation 0.08, value 0.12.
"""
import glob
import json
import math
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "vfxgen"))
sys.path.insert(0, os.path.join(ROOT, "tools", "tests"))
import ground_ref  # noqa: E402

REF = os.path.join(ROOT, "docs", "reference", "ground")
OUT = os.path.join(ROOT, "docs", "ground_match")
TOL = {"hue": 8.0, "sat": 0.08, "val": 0.12}


def load_json(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def ref_image(regions, key):
    return np.asarray(Image.open(os.path.join(REF, regions["images"][key]["file"])).convert("RGB")).astype(np.float32) / 255


def hsv_stats(px):
    h = ground_ref.hsv(px)
    a = np.radians(h[:, 0])
    hm = math.degrees(math.atan2(np.sin(a).mean(), np.cos(a).mean())) % 360
    return hm, float(h[:, 1].mean()), float(h[:, 2].mean())


def hue_delta(a, b):
    d = abs(a - b) % 360
    return min(d, 360 - d)


def plot(draw, box, series, ylim, labels):
    """Line plot in box (x0, y0, x1, y1): series = [(values, colour)], x = root .. tip."""
    x0, y0, x1, y1 = box
    draw.rectangle(box, outline=(90, 90, 90))
    for vals, col in series:
        pts = []
        for i, v in enumerate(vals):
            x = x0 + (x1 - x0) * i / max(1, len(vals) - 1)
            y = y1 - (y1 - y0) * (v - ylim[0]) / (ylim[1] - ylim[0])
            pts.append((x, min(max(y, y0), y1)))
        draw.line(pts, fill=col, width=2)
    draw.text((x0 + 4, y0 + 2), labels, fill=(220, 220, 220))
    draw.text((x0 + 4, y1 - 12), "root", fill=(150, 150, 150))
    draw.text((x1 - 24, y1 - 12), "tip", fill=(150, 150, 150))


def profile(cell_rgba):
    """Value / hue profile, 16 bands root -> tip, of the plant pixels of a cell (or a crop + mask)."""
    rgb, m = cell_rgba
    rows = np.nonzero(m.any(1))[0]
    if len(rows) == 0:
        return [0] * 16, [0] * 16
    top, bot = rows.min(), rows.max() + 1
    vals, hues = [], []
    for i in range(16):
        r1 = bot - int((bot - top) * i / 16)
        r0 = bot - int((bot - top) * (i + 1) / 16)
        px = rgb[r0:r1][m[r0:r1]]
        if len(px) == 0:
            vals.append(vals[-1] if vals else 0)
            hues.append(hues[-1] if hues else 0)
            continue
        h, s, v = hsv_stats(px)
        vals.append(v)
        hues.append(h)
    return vals, hues


def cmd_blades():
    import ground_tex_check as T
    import groundgen as GG
    regions = load_json(os.path.join(REF, "regions.json"))
    blades = {b["id"]: b for b in regions["blades"]}
    recipes = GG.load_recipes()
    os.makedirs(OUT, exist_ok=True)
    made = []
    for name, r in recipes.items():
        if r["kind"] != "blade_atlas" or not r.get("ref_blades"):
            continue
        atlas = os.path.join(GG.OUT_DIRS[r["_group"]], name + "_albedo.png")
        if not os.path.exists(atlas):
            continue
        im = np.asarray(Image.open(atlas).convert("RGBA")).astype(np.float32) / 255
        cols, rows = r["grid"]
        cw = im.shape[1] // cols
        H = 300
        sheet = Image.new("RGB", (1800, H + 260), (24, 24, 26))
        d = ImageDraw.Draw(sheet)
        x = 10
        b = blades[r["ref_blades"][0]]
        x0, y0, x1, y1 = b["box"]
        crop = ref_image(regions, b["image"])[y0:y1, x0:x1]
        tm = T.traced_mask(b)
        cim = Image.fromarray((crop * 255).astype(np.uint8))
        ov = np.asarray(cim).copy()
        ov[tm] = (ov[tm] * 0.4 + np.array([255, 60, 60]) * 0.6).astype(np.uint8)
        w = int(round(cim.width * H / cim.height))
        sheet.paste(Image.fromarray(ov).resize((w, H)), (x, 20))
        d.text((x, 4), f"reference {b['id']} (traced blades red)", fill=(230, 230, 230))
        x += w + 16
        env_ref = T.envelope(tm)
        ref_prof = profile((crop, ground_ref.mask_of(crop.reshape(-1, 3), "plant").reshape(crop.shape[:2])))
        ours = []
        for i in range(len(r["cells"])):
            yy, xx = divmod(i, cols)
            cell = im[yy * cw:(yy + 1) * cw, xx * cw:(xx + 1) * cw]
            bg = np.array([0.32, 0.30, 0.25])
            comp = cell[..., :3] * cell[..., 3:] + bg * (1 - cell[..., 3:])
            env = T.envelope(cell[..., 3] > 0.5)
            iou = T.iou(env, env_ref)
            tile = Image.fromarray((comp * 255).astype(np.uint8)).resize((H, H))
            # the fan envelopes: ours green, the reference's red outline
            e = Image.fromarray(np.kron(env, np.ones((H // 8, H // 8))).astype(np.uint8) * 60).resize((H, H))
            tile = Image.composite(Image.new("RGB", (H, H), (60, 200, 60)), tile, e.point(lambda v: 70 if v else 0))
            dd = ImageDraw.Draw(tile)
            k = H // 8
            for ey in range(8):
                for ex in range(8):
                    if env_ref[ey, ex]:
                        dd.rectangle((ex * k, ey * k, ex * k + k - 1, ey * k + k - 1), outline=(230, 70, 60))
            sheet.paste(tile, (x, 20))
            d.text((x, 4), f"cell {i}: fan IoU {iou:.2f}", fill=(230, 230, 230))
            x += H + 8
            ours.append(profile((cell[..., :3], cell[..., 3] > 0.5)))
        # profiles: value and hue, reference white, ours coloured
        cols_ = [(120, 200, 120), (200, 200, 90), (110, 170, 230), (230, 140, 90)]
        plot(d, (10, H + 40, 880, H + 240), [(ref_prof[0], (240, 240, 240))] + [(o[0], cols_[k % 4]) for k, o in enumerate(ours)], (0, 1), "value root -> tip (reference white)")
        plot(d, (900, H + 40, 1780, H + 240), [(ref_prof[1], (240, 240, 240))] + [(o[1], cols_[k % 4]) for k, o in enumerate(ours)], (20, 100), "hue (deg) root -> tip")
        p = os.path.join(OUT, f"blade_compare_{name}.png")
        sheet.save(p, optimize=True)
        made.append(p)
    print("\n".join(os.path.relpath(m, ROOT) for m in made))


def cmd_terrain():
    import groundgen as GG
    regions = load_json(os.path.join(REF, "regions.json"))
    recipes = GG.load_recipes()
    rows = [(r["ref_region"], name, r) for name, r in recipes.items() if r["kind"] in ("terrain_layer", "transition") and r.get("ref_region")]
    S = 200
    sheet = Image.new("RGB", (4 * S + 60, len(rows) * (S + 30) + 20), (24, 24, 26))
    d = ImageDraw.Draw(sheet)
    stats = load_json(os.path.join(REF, "stats.json"))["regions"]
    for i, (rid, name, r) in enumerate(rows):
        reg = next(x for x in regions["regions"] if x["id"] == rid)
        x0, y0, x1, y1 = reg["box"]
        crop = ref_image(regions, reg["image"])[y0:y1, x0:x1]
        y = 20 + i * (S + 30)
        sheet.paste(Image.fromarray((crop * 255).astype(np.uint8)).resize((S, S)), (10, y))
        tex = os.path.join(GG.OUT_DIRS[r["_group"]], name + "_albedo.jpg")
        ti = Image.open(tex).convert("RGB")
        # a crop of the texture at about the reference's scale: 1.5 m wide
        frac = min(1.0, 1.5 / r["tile_m"])
        tc = ti.crop((0, 0, int(ti.width * frac), int(ti.height * frac))).resize((S, S))
        sheet.paste(tc, (20 + S, y))
        rs = stats[rid]
        th, ts, tv = hsv_stats(np.asarray(ti).reshape(-1, 3).astype(np.float32) / 255)
        sw_r = Image.new("RGB", (S // 2, S), tuple(int(c * 255) for c in rs["srgb"]))
        sw_o = Image.new("RGB", (S // 2, S), tuple(int(c) for c in np.asarray(ti).reshape(-1, 3).mean(0)))
        sheet.paste(sw_r, (30 + 2 * S, y))
        sheet.paste(sw_o, (30 + 2 * S + S // 2, y))
        dh, ds = hue_delta(th, rs["hsv_mean"][0]), ts - rs["hsv_mean"][1]
        d.text((40 + 3 * S, y + 10), f"{name}\nvs {rid}\nhue {th:.0f} / {rs['hsv_mean'][0]:.0f} (d {dh:.0f})\nsat {ts:.2f} / {rs['hsv_mean'][1]:.2f} (d {ds:+.2f})\n"
               f"val {tv:.2f} / {rs['hsv_mean'][2]:.2f} (lit)", fill=(230, 230, 230))
        d.text((10, y - 14), "reference | ours (1.5 m) | mean ref / ours", fill=(160, 160, 160))
    p = os.path.join(OUT, "terrain_compare.png")
    sheet.save(p, optimize=True)
    print(os.path.relpath(p, ROOT))


def cmd_cfg():
    views = load_json(os.path.join(REF, "views.json"))["views"]
    lines = ["// Ground match captures (tools/ground_match.py cfg writes this file; docs/GROUND_REFERENCE.md):",
             "// each reference view's map / pose / FOV / time / weather, one screenshot each. Run from any directory:",
             "//   deadground --test --nosound --log <log> --script tools/tests/ground_match.cfg ; py tools/ground_match.py report <dir>",
             "set r_maxfps 60", "set cl_camera_socket 0", "set cl_fp_body 0"]
    cur = None
    for k, v in views.items():
        if v["map"] != cur:
            lines += [f"map {v['map']}", "wait 20"]
            cur = v["map"]
        lines += [f"set sv_fov {v['fov']:.1f}", f"time_set {v['time']} instant", "time_pause", f"weather_set {v['weather']} 0", "weather_lock 1",
                  "setpos %g %g %g %g %g" % (*v["pos"], v["yaw"], v["pitch"]), "wait 40", f"screenshot ground_match_{k}.png", "wait 2"]
    lines.append("quit")
    p = os.path.join(ROOT, "tools", "tests", "ground_match.cfg")
    with open(p, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(os.path.relpath(p, ROOT))


def cmd_report(shots):
    views = load_json(os.path.join(REF, "views.json"))["views"]
    regions = load_json(os.path.join(REF, "regions.json"))
    stats = load_json(os.path.join(REF, "stats.json"))["regions"]
    lines = ["# Ground match report", "", "Written by `tools/ground_match.py report`. Our captures (`tools/tests/ground_match.cfg`, the presets of "
             "`docs/reference/ground/views.json`) against the reference regions (`tools/ground_ref.py`). Values are lit, graded frames: hue and "
             f"saturation first (exposure-independent), value informative. Tolerances: hue {TOL['hue']:.0f} deg, saturation {TOL['sat']}, value {TOL['val']}.", "",
             "| View | Region | Reference HSV | Ours HSV | d hue | d sat | d val | |", "|---|---|---|---|---|---|---|---|"]
    fails = 0
    for k, v in views.items():
        p = os.path.join(shots, f"ground_match_{k}.png")
        if not os.path.exists(p):
            lines.append(f"| {k} | (no capture) | | | | | | |")
            continue
        ours = np.asarray(Image.open(p).convert("RGB")).astype(np.float32) / 255
        H, W = ours.shape[:2]
        ref = ref_image(regions, k)
        # side by side, with the regions outlined
        a = Image.fromarray((ref * 255).astype(np.uint8)).resize((960, int(960 * ref.shape[0] / ref.shape[1])))
        b = Image.fromarray((ours * 255).astype(np.uint8)).resize((960, 540))
        sbs = Image.new("RGB", (1920, max(a.height, 540)), (0, 0, 0))
        sbs.paste(a, (0, 0))
        sbs.paste(b, (960, 0))
        dd = ImageDraw.Draw(sbs)
        for rid, box in v["regions"].items():
            reg = next(x for x in regions["regions"] if x["id"] == rid)
            rx0, ry0, rx1, ry1 = reg["box"]
            sx, sy = a.width / ref.shape[1], a.height / ref.shape[0]
            dd.rectangle((rx0 * sx, ry0 * sy, rx1 * sx, ry1 * sy), outline=(255, 220, 0))
            dd.rectangle((960 + box[0] * 960, box[1] * 540, 960 + box[2] * 960, box[3] * 540), outline=(255, 220, 0))
            x0, y0, x1, y1 = int(box[0] * W), int(box[1] * H), int(box[2] * W), int(box[3] * H)
            px = ours[y0:y1, x0:x1].reshape(-1, 3)
            px = px[ground_ref.mask_of(px, reg["mask"])]
            rs = stats[rid]
            if len(px) < 50:
                lines.append(f"| {k} | {rid} | {rs['hsv_mean']} | (too few pixels) | | | | |")
                continue
            h, s, vv = hsv_stats(px)
            dh, ds, dv = hue_delta(h, rs["hsv_mean"][0]), s - rs["hsv_mean"][1], vv - rs["hsv_mean"][2]
            ok = dh <= TOL["hue"] and abs(ds) <= TOL["sat"]
            fails += not ok
            lines.append(f"| {k} | {rid} | {rs['hsv_mean'][0]:.0f} {rs['hsv_mean'][1]:.2f} {rs['hsv_mean'][2]:.2f} | {h:.0f} {s:.2f} {vv:.2f} | {dh:.0f} | {ds:+.2f} | {dv:+.2f} | "
                         f"{'ok' if ok else 'off'}{'' if abs(dv) <= TOL['val'] else ' (value)'} |")
        sbs.save(os.path.join(OUT, f"compare_{k}.jpg"), quality=85)
    lines += ["", f"{fails} region(s) outside the hue / saturation tolerance.", "",
              "Side by side: `compare_<view>.jpg` (reference left, ours right, regions outlined). Blade and terrain sheets: "
              "`blade_compare_*.png`, `terrain_compare.png` (`py tools/ground_match.py blades|terrain`). Texture checks: `gen_report.md`, "
              "`py tools/tests/ground_tex_check.py`. Grass density / height against the spec: `test_grass`.", ""]
    with open(os.path.join(OUT, "report.md"), "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("\n".join(lines))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    c = sys.argv[1]
    if c == "blades":
        cmd_blades()
    elif c == "terrain":
        cmd_terrain()
    elif c == "cfg":
        cmd_cfg()
    elif c == "report":
        cmd_report(sys.argv[2] if len(sys.argv) > 2 else ".")
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
