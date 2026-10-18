"""Validates detailed item meshes (docs/ITEM_PIPELINE.md "Validation"). Plain Python, no Blender.

    python tools/validate_item/validate_item.py [ids ...]      (default: every entry of assets/items/catalog.json)

Per item (assets/items/catalog.json lists them with their kind, triangle budget, required parts / sockets):
  * the .dgim parses, indices are in range, normals are unit, no LOD is empty, LODs get coarser;
  * LOD 0 triangles inside the catalog budget for its tier;
  * every material slot is in assets/items/materials.json;
  * the sockets the game systems use exist and sit where the JSON puts them (2 mm): weapons grip_r / sight
    (the iron sight line) / muzzle (the barrel tip the game computes) / grip_l (the support hand) / one per
    attachment slot; attachments mount / sight / muzzle / grip_l; the phone screen / camera_lens;
  * the parts the animation / attachment systems drive exist: every "mech" role the weapon's box model has
    (slide, bolt, pump, mag), a part hidden by each attachment slot that hides box parts, folding or hidden
    iron sights when an optic can be mounted; plus the catalog's own list;
  * a convex hull per part (dropped items / physics props).
UV overlap does not apply: item meshes carry no UVs (per-vertex materials, MAT_VERTEX_PBR).
Exit status 1 on any error.
"""
import json
import math
import os
import struct
import sys
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SLOTS = ("optic", "muzzle", "underbarrel", "magazine", "stock")
TOL = 0.002

HDR = struct.Struct("<12I3f3f3f32s")
PART = struct.Struct("<32si3f4f16s16s16s3ff3f3fI3I3I3I3I")
SOCK = struct.Struct("<32si3f4f")
HULL = struct.Struct("<iII")
VERT = struct.Struct("<3H2h6B")


def cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def qrot(q, v):
    x, y, z, w = q
    # v' = v + 2w (q x v) + 2 q x (q x v)
    cx, cy, cz = y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]
    cx2, cy2, cz2 = y * cz - z * cy, z * cx - x * cz, x * cy - y * cx
    return (v[0] + 2 * (w * cx + cx2), v[1] + 2 * (w * cy + cy2), v[2] + 2 * (w * cz + cz2))


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def read_dgim(path):
    raw = open(path, "rb").read()
    h = HDR.unpack_from(raw, 0)
    magic, ver, npart, nsock, nmat, nhull, nv, ni, nhp, flags, pay_n, packed_n = h[:12]
    if magic != 0x4D494744 or ver != 2:
        raise ValueError("not a DGIM v2 file")
    if HDR.size + packed_n != len(raw):
        raise ValueError("file is %d bytes, the header says %d" % (len(raw), HDR.size + packed_n))
    data = zlib.decompress(raw[HDR.size:])
    if len(data) != pay_n:
        raise ValueError("payload inflates to %d bytes, the header says %d" % (len(data), pay_n))
    m = dict(lod_dist=h[12:15], bmin=h[15:18], bmax=h[18:21], id=cstr(h[21]), file_bytes=len(raw))
    o = 0
    m["mats"] = [cstr(data[o + 32 * i:o + 32 * i + 32]) for i in range(nmat)]
    o += 32 * nmat
    parts = []
    for i in range(npart):
        p = PART.unpack_from(data, o)
        o += PART.size
        parts.append(dict(name=cstr(p[0]), parent=p[1], pos=p[2:5], rot=p[5:9], mech=cstr(p[9]), hide=cstr(p[10]), fold=cstr(p[11]), fold_axis=p[12:15],
                          fold_angle=p[15], bmin=p[16:19], bmax=p[19:22], lod_count=p[22], v0=p[23:26], vn=p[26:29], i0=p[29:32], inn=p[32:35]))
    socks = []
    for i in range(nsock):
        s = SOCK.unpack_from(data, o)
        o += SOCK.size
        socks.append(dict(name=cstr(s[0]), part=s[1], pos=s[2:5], rot=s[5:9]))
    hulls = [HULL.unpack_from(data, o + HULL.size * i) for i in range(nhull)]
    o += HULL.size * nhull
    vo = o
    o += VERT.size * nv
    idx = struct.unpack_from("<%dH" % ni, data, o)
    o += 2 * ni
    if o + 12 * nhp != len(data):
        raise ValueError("size mismatch: %d bytes, layout says %d" % (len(data), o + 12 * nhp))
    m.update(parts=parts, sockets=socks, hulls=hulls, nv=nv, ni=ni, data=data, vo=vo, idx=idx)
    # Rest transforms (item space)
    for p in parts:
        if p["parent"] >= 0:
            pp = parts[p["parent"]]
            p["rest_pos"] = tuple(a + b for a, b in zip(pp["rest_pos"], qrot(pp["rest_rot"], p["pos"])))
            p["rest_rot"] = qmul(pp["rest_rot"], p["rot"])
        else:
            p["rest_pos"], p["rest_rot"] = p["pos"], p["rot"]
    for s in socks:
        if s["part"] >= 0:
            pp = parts[s["part"]]
            s["item_pos"] = tuple(a + b for a, b in zip(pp["rest_pos"], qrot(pp["rest_rot"], s["pos"])))
        else:
            s["item_pos"] = s["pos"]
    return m


def vertex(m, i):
    return VERT.unpack_from(m["data"], m["vo"] + VERT.size * i)


def item_triangles(m):
    """LOD 0 triangles in item space (rest pose)."""
    tris = []
    for p in m["parts"]:
        if p["lod_count"] == 0:
            continue
        lo, hi = p["bmin"], p["bmax"]
        pts = []
        for i in range(p["v0"][0], p["v0"][0] + p["vn"][0]):
            v = vertex(m, i)
            q = tuple(lo[k] + (hi[k] - lo[k]) * v[k] / 65535.0 for k in range(3))
            r = qrot(p["rest_rot"], q)
            pts.append((r[0] + p["rest_pos"][0], r[1] + p["rest_pos"][1], r[2] + p["rest_pos"][2]))
        idx = m["idx"][p["i0"][0]:p["i0"][0] + p["inn"][0]]
        for t in range(0, len(idx) - 2, 3):
            tris.append((pts[idx[t]], pts[idx[t + 1]], pts[idx[t + 2]]))
    return tris


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def point_tri(p, a, b, c):
    """Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5)."""
    ab, ac, ap = _sub(b, a), _sub(c, a), _sub(p, a)
    d1, d2 = _dot(ab, ap), _dot(ac, ap)
    if d1 <= 0 and d2 <= 0:
        return a
    bp = _sub(p, b)
    d3, d4 = _dot(ab, bp), _dot(ac, bp)
    if d3 >= 0 and d4 <= d3:
        return b
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        v = d1 / (d1 - d3) if d1 != d3 else 0.0
        return (a[0] + ab[0] * v, a[1] + ab[1] * v, a[2] + ab[2] * v)
    cp = _sub(p, c)
    d5, d6 = _dot(ab, cp), _dot(ac, cp)
    if d6 >= 0 and d5 <= d6:
        return c
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        w = d2 / (d2 - d6) if d2 != d6 else 0.0
        return (a[0] + ac[0] * w, a[1] + ac[1] * w, a[2] + ac[2] * w)
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        den2 = (d4 - d3) + (d5 - d6)
        w = (d4 - d3) / den2 if den2 else 0.0
        return (b[0] + (c[0] - b[0]) * w, b[1] + (c[1] - b[1]) * w, b[2] + (c[2] - b[2]) * w)
    if va + vb + vc == 0:
        return a   # degenerate (zero-area) triangle
    den = 1.0 / (va + vb + vc)
    v, w = vb * den, vc * den
    return (a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w)


def glue(socket_pos, tris):
    """Distance from a hand socket to the mesh surface (the hand has to sit on the model)."""
    best = 1e9
    for (a, b, c) in tris:
        # cheap reject on the triangle's bounding sphere
        cx, cy, cz = (a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3
        r = max(math.dist((cx, cy, cz), a), math.dist((cx, cy, cz), b), math.dist((cx, cy, cz), c))
        if math.dist((cx, cy, cz), socket_pos) - r > best:
            continue
        best = min(best, math.dist(socket_pos, point_tri(socket_pos, a, b, c)))
    return best


GLUE_TOL = 0.03   # m: a palm socket sits on the surface (grip / handguard / foregrip), the palm is ~2 cm thick


def load_json_objects():
    weapons, items, atts = {}, {}, {}
    wd = os.path.join(ROOT, "data", "weapons")
    for f in sorted(os.listdir(wd)):
        if f.endswith(".json"):
            j = json.load(open(os.path.join(wd, f)))
            weapons[j.get("item", j["id"])] = j
    for it in json.load(open(os.path.join(ROOT, "data", "items.json")))["items"]:
        items[it["id"]] = it
    ad = os.path.join(ROOT, "assets", "attachments")
    for f in sorted(os.listdir(ad)):
        if f.endswith(".json"):
            j = json.load(open(os.path.join(ad, f)))
            atts[j["id"]] = j
    return weapons, items, atts


def weapon_expect(w):
    """Socket positions the game derives from the weapon JSON (src/gameplay/items.c parse_weapon)."""
    exp = {"grip_r": tuple(w.get("grip", (0, 0, 0)))}
    boxes = w.get("model", [])
    if w.get("kind", "hitscan") != "melee":
        if "muzzle_override" in w:
            exp["muzzle"] = tuple(w["muzzle"])
        elif boxes:
            b = min(boxes, key=lambda b: b["offset"][2] - b["half"][2])
            exp["muzzle"] = (b["offset"][0], b["offset"][1], b["offset"][2] - b["half"][2] - 0.01)
        isg = w.get("iron_sights")
        if isg:
            exp["sight"] = (0.0, isg["line"], isg.get("rear_z", 0.0))
    gl = (w.get("hold", {}).get("sockets", {}) or {}).get("grip_l")
    if isinstance(gl, dict) and "pos" in gl:
        exp["grip_l"] = tuple(gl["pos"])
    for k, sl in (w.get("attachment_slots") or {}).items():
        k = {"sight": "optic", "scope": "optic"}.get(k, k)
        if "pos" in sl:
            exp[k] = tuple(sl["pos"])
    return exp


def check(item_id, cat, objs, lib, verbose=False):
    errs, warns = [], []
    path = os.path.join(ROOT, "assets", "items", "meshes", item_id + ".dgim")
    if not os.path.exists(path):
        return ["missing %s (tools/blender/build_items.py %s)" % (os.path.relpath(path, ROOT), item_id)], [], None
    try:
        m = read_dgim(path)
    except Exception as e:
        return ["parse: %s" % e], [], None
    weapons, items, atts = objs
    kind = cat.get("kind", "item")
    budget = cat.get("budget")
    # geometry
    tris = [0, 0, 0]
    for p in m["parts"]:
        if p["lod_count"] == 0:
            errs.append("part %s has no LOD" % p["name"])
        for l in range(p["lod_count"]):
            if p["vn"][l] == 0 or p["inn"][l] == 0:
                errs.append("part %s LOD %d is empty" % (p["name"], l))
            for k in range(p["inn"][l]):
                if m["idx"][p["i0"][l] + k] >= p["vn"][l]:
                    errs.append("part %s LOD %d: index out of range" % (p["name"], l))
                    break
            tris[l] += p["inn"][l] // 3
    bad_n = 0
    for i in range(0, m["nv"], max(1, m["nv"] // 4000)):
        v = vertex(m, i)
        if abs(v[3]) + abs(v[4]) > 32767 * 2 + 2:
            bad_n += 1
        if v[5] >= len(m["mats"]):
            errs.append("vertex material slot out of range")
            break
    if bad_n:
        errs.append("%d sampled normals are not octahedral-encoded" % bad_n)
    if budget and not (budget[0] <= tris[0] <= budget[1]):
        errs.append("LOD 0 has %d triangles, budget %d..%d (%s)" % (tris[0], budget[0], budget[1], kind))
    if not (tris[0] > tris[1] > tris[2] > 0):
        warns.append("LODs do not get coarser: %s" % tris)
    for mt in m["mats"]:
        if mt not in lib:
            errs.append("material slot '%s' is not in assets/items/materials.json" % mt)
    hulled = {h[0] for h in m["hulls"]}
    for i, p in enumerate(m["parts"]):
        if i not in hulled:
            warns.append("part %s has no convex hull" % p["name"])
    socks = {s["name"]: s for s in m["sockets"]}
    parts = {p["name"]: p for p in m["parts"]}

    def at(name, want, tol=TOL):
        s = socks.get(name)
        if not s:
            errs.append("socket %s missing" % name)
            return
        d = math.dist(s["item_pos"], want)
        if d > tol:
            errs.append("socket %s at (%.4f %.4f %.4f), the JSON says (%.4f %.4f %.4f): %.1f mm off" % ((name,) + tuple(s["item_pos"]) + tuple(want) + (d * 1000,)))

    # Hand glue: the hand sockets sit on the detailed geometry (tests the meshes against the hold data).
    hands = [h for h in ("grip_r", "grip_l") if h in socks and h not in cat.get("no_glue", [])]
    if hands:
        tris_ = item_triangles(m)
        for h in hands:
            d = glue(socks[h]["item_pos"], tris_)
            if d > GLUE_TOL:
                errs.append("hand glue: socket %s is %.1f mm from the mesh (> %.0f mm): the hand would float" % (h, d * 1000, GLUE_TOL * 1000))
            elif verbose:
                print("    glue %s: %.1f mm" % (h, d * 1000))
    for s in cat.get("sockets", []):
        if s not in socks:
            errs.append("socket %s missing (catalog)" % s)
    for p in cat.get("parts", []):
        if p not in parts:
            errs.append("part %s missing (catalog)" % p)
    if kind == "weapon":
        w = weapons.get(item_id)
        if not w:
            errs.append("no weapon JSON for %s" % item_id)
        else:
            for name, pos in weapon_expect(w).items():
                at(name, pos)
            roles = {b.get("mech") for b in w.get("model", []) if b.get("mech")}
            if w.get("iron_sights", {}).get("mech"):
                roles.add(w["iron_sights"]["mech"])
            have = {p["mech"] for p in m["parts"]}
            for r in sorted(roles):
                if r not in have:
                    errs.append("no part with mech '%s' (the box model has one: the fire animation moves it)" % r)
            hides = {p["hide"] for p in m["parts"]} | {p["fold"] for p in m["parts"]}
            for k, sl in (w.get("attachment_slots") or {}).items():
                k = {"sight": "optic", "scope": "optic"}.get(k, k)
                if sl.get("hide_parts") and k not in hides:
                    errs.append("slot %s hides box parts, but no mesh part has hide '%s'" % (k, k))
            if "optic" in (w.get("attachment_slots") or {}) and w.get("iron_sights") and "optic" not in hides:
                errs.append("an optic can be mounted but no iron sight part folds / hides for it (fold or hide 'optic')")
    elif kind == "attachment":
        a = atts.get(cat.get("attachment", item_id[4:]))   # per-weapon variants ("mesh_for") name their attachment
        if not a:
            errs.append("no attachment JSON for %s" % item_id)
        else:
            mp = tuple(a.get("mount", {}).get("pos", (0, 0, 0)))
            at("mount", mp)
            if "sight_socket" in a:
                at("sight", tuple(a["sight_socket"]))
            if isinstance(a.get("grip_l"), dict) and "pos" in a["grip_l"]:
                at("grip_l", tuple(a["grip_l"]["pos"]))
            if a.get("slot") == "muzzle" and "muzzle_length" in a:
                at("muzzle", (mp[0], mp[1], mp[2] - a["muzzle_length"]), 0.003)
    else:
        it = items.get(item_id)
        if not it:
            errs.append("no items.json entry for %s" % item_id)
        elif it.get("camera"):
            # src/gameplay/items.c parse_camera: the screen / lens defaults derive from the body box.
            cam = it["camera"]
            b = cam.get("body", {})
            h = b.get("half", (0.036, 0.075, 0.004))
            c = b.get("center", (-h[0], h[1] - 0.02, 0))
            screen = cam.get("screen", {}).get("pos", (c[0], c[1], c[2] + h[2] + 0.0005))
            lens = cam.get("lens", (c[0] - h[0] + 0.012, c[1] + h[1] - 0.014, c[2] - h[2] - 0.0005))
            at("screen", tuple(screen), 0.0015)
            at("camera_lens", tuple(lens), 0.0015)
        if "grip_r" not in socks:
            errs.append("socket grip_r missing")
        elif math.dist(socks["grip_r"]["item_pos"], (0, 0, 0)) > TOL:
            errs.append("grip_r must be the item origin")
    info = dict(tris=tris, parts=len(m["parts"]), sockets=sorted(socks), mats=m["mats"], kb=m["file_bytes"] / 1024.0)
    return errs, warns, info


def main(argv):
    cat = json.load(open(os.path.join(ROOT, "assets", "items", "catalog.json")))["items"]
    lib = json.load(open(os.path.join(ROOT, "assets", "items", "materials.json")))["materials"]
    objs = load_json_objects()
    ids = [a for a in argv[1:] if not a.startswith("-")] or sorted(cat)
    verbose = "-v" in argv
    fails = 0
    for i in ids:
        c = cat.get(i)
        if c is None:
            print("%-16s FAIL not in assets/items/catalog.json" % i)
            fails += 1
            continue
        errs, warns, info = check(i, c, objs, lib, verbose)
        status = "FAIL" if errs else "ok  "
        line = "%-16s %s" % (i, status)
        if info:
            line += " tris %6d / %6d / %6d  parts %2d  %6.0f KB" % (info["tris"][0], info["tris"][1], info["tris"][2], info["parts"], info["kb"])
        print(line)
        for e in errs:
            print("    error: " + e)
        for w in warns:
            print("    warn:  " + w)
        if verbose and info:
            print("    sockets: " + " ".join(info["sockets"]))
            print("    materials: " + " ".join(info["mats"]))
        fails += 1 if errs else 0
    print("validate_item: %d item(s), %d failed" % (len(ids), fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
