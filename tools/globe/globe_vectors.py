"""Vectors of the Maps tab globe (docs/MENU_GLOBE.md "Data"): assets/globe/vectors.dgv.

Layers (lines unless noted), each in levels of detail picked by the globe's pixels per degree (a
level is simplified to ~0.6 px at the densest zoom it serves, Douglas-Peucker in degrees):
  coast, borders, lakes, rivers           Natural Earth 1:110m / 1:50m / 1:10m (public domain)
  us_states, us_counties                  geoBoundaries USA ADM1 / ADM2 (US Census TIGER, CC BY 4.0)
  ro_judete, ro_communes                  geoBoundaries ROU ADM1 / ADM2 (CC BY 4.0)
  unit:<region>                           the region's unit outline (state / judet), closed rings
  mask:<region>:out / :country / :unit    triangles (kind 1): the world minus the country, the
                                          country minus the unit, the unit (the focus dimming)
Labels: every unit of the region's layer (code, name, a point inside it).

Binary (little endian): header 'DGGV', version 1, counts (layers, lods, lines, points, labels, 0, 0);
layers {char name[40]; u32 kind, first_lod, lod_count, pad}; lods {f32 max_ppd; u32 first_line,
line_count, pad}; lines {u32 first_point, point_count; f32 west, south, east, north}; points
{u16 x, y}: the point inside its line's box (lon = west + x / 65535 * (east - west)); labels
{char layer[24], code[8], name[40]; f32 lon, lat}. A line is at most CHUNK points (long coasts are
cut, the cuts share their end point) so the game culls per chunk; a triangle layer's 'lines' are
batches of whole triangles (3 points each) per grid cell.
"""
import json
import math
import struct

import numpy as np
import shapely
from shapely.geometry import LineString, MultiLineString, MultiPolygon, Polygon, box, shape

import globe_fetch as gf

CHUNK = 192
# Level ladder: the densest zoom (px / degree) each level serves; the last one serves everything above
# (the deepest dive, a city at 85000 px radius on a 1080p screen, is ~1800 px / degree). Natural
# Earth's 1:10m has no more to give past 640.
PPD = [10.0, 40.0, 160.0, 640.0, 2560.0]
PPD_NE = PPD[:4]


def log(*a):
    print(*a, flush=True)


def tol_for(ppd):
    return 0.6 / min(ppd, 12000.0)


def ne_lines(name):
    import shapefile

    out = []
    for sr in shapefile.Reader(gf.ne_shp(name)).iterShapeRecords():
        if sr.shape.shapeType == 0:   # NULL shape
            continue
        g = shape(sr.shape.__geo_interface__)
        out += list(lines_of(g))
    return out


def ne_polys(name, where=None):
    import shapefile

    rd = shapefile.Reader(gf.ne_shp(name))
    fields = [f[0] for f in rd.fields[1:]]
    out = []
    for sr in rd.iterShapeRecords():
        if sr.shape.shapeType == 0:
            continue
        rec = dict(zip(fields, sr.record))
        if where and not where(rec):
            continue
        out.append(shape(sr.shape.__geo_interface__))
    return out


def lines_of(g):
    if g.is_empty:
        return
    if isinstance(g, LineString):
        yield g
    elif isinstance(g, (Polygon,)):
        yield LineString(g.exterior.coords)
        for r in g.interiors:
            yield LineString(r.coords)
    elif hasattr(g, "geoms"):
        for p in g.geoms:
            yield from lines_of(p)


def geob(iso_adm):
    d = json.load(open(gf.path("geob", iso_adm + ".geojson")))
    return [(f["properties"], shape(f["geometry"])) for f in d["features"]]


def boundaries(polys, tol=0.0):
    """Unique boundary lines of a polygon coverage (shared edges once), merged into long lines."""
    if tol > 0:
        polys = list(shapely.coverage_simplify(np.array(polys, dtype=object), tol, simplify_boundary=True))
    u = shapely.union_all([p.boundary for p in polys])
    u = shapely.line_merge(u)
    return list(lines_of(u))


class Writer:
    def __init__(self):
        self.layers, self.lods, self.lines, self.points, self.labels = [], [], [], [], []

    def _chunk(self, coords):
        n = len(coords)
        if n < 2:
            return
        step = CHUNK - 1
        for a in range(0, n - 1, step):
            seg = coords[a:min(n, a + CHUNK)]
            if len(seg) < 2:
                continue
            arr = np.asarray(seg, np.float64)
            self.lines.append((len(self.points), len(seg), arr[:, 0].min(), arr[:, 1].min(), arr[:, 0].max(), arr[:, 1].max()))
            self.points += [(float(x), float(y)) for x, y in seg]

    def line_layer(self, name, levels):
        """levels: [(max_ppd, [LineString])]."""
        first = len(self.lods)
        for ppd, lines in levels:
            l0 = len(self.lines)
            for ln in lines:
                coords = list(ln.coords)
                if len(coords) >= 2:
                    self._chunk(coords)
            self.lods.append((ppd, l0, len(self.lines) - l0))
        self.layers.append((name, 0, first, len(self.lods) - first))
        log("  %-22s %s" % (name, " ".join("%d" % self.lods[first + k][2] for k in range(len(levels)))))

    def tri_layer(self, name, geom, cell):
        """Constrained Delaunay triangles of geom, cut on a cell-degree grid (one batch per cell)."""
        first = len(self.lods)
        l0 = len(self.lines)
        w, s, e, n = geom.bounds
        ntri = 0
        for gx in range(int(math.floor(w / cell)), int(math.ceil(e / cell))):
            for gy in range(int(math.floor(s / cell)), int(math.ceil(n / cell))):
                c = box(gx * cell, gy * cell, (gx + 1) * cell, (gy + 1) * cell)
                piece = geom.intersection(c)
                if piece.is_empty or piece.area <= 0:
                    continue
                tris = shapely.constrained_delaunay_triangles(piece)
                pts = []
                for t in getattr(tris, "geoms", [tris]):
                    if t.is_empty or t.area <= 0:
                        continue
                    cs = list(t.exterior.coords)[:3]
                    pts += cs
                if not pts:
                    continue
                arr = np.asarray(pts, np.float64)
                self.lines.append((len(self.points), len(pts), arr[:, 0].min(), arr[:, 1].min(), arr[:, 0].max(), arr[:, 1].max()))
                self.points += [(float(x), float(y)) for x, y in pts]
                ntri += len(pts) // 3
        self.lods.append((PPD[-1], l0, len(self.lines) - l0))
        self.layers.append((name, 1, first, 1))
        log("  %-22s %d triangles" % (name, ntri))

    def label(self, layer, code, name, lon, lat):
        self.labels.append((layer, code, name, lon, lat))

    def write(self, path):
        def s(text, n):
            b = text.encode("utf-8")[: n - 1]
            return b + b"\0" * (n - len(b))

        with open(path, "wb") as f:
            f.write(struct.pack("<4sIIIIIIII", b"DGGV", 1, len(self.layers), len(self.lods), len(self.lines), len(self.points), len(self.labels), 0, 0))
            for name, kind, first, count in self.layers:
                f.write(s(name, 40) + struct.pack("<IIII", kind, first, count, 0))
            for ppd, first, count in self.lods:
                f.write(struct.pack("<fIII", min(ppd, 3.0e38), first, count, 0))
            q = np.zeros((len(self.points), 2), np.uint16)
            pts = np.asarray(self.points, np.float64)
            for fp, cnt, w, so, e, n in self.lines:
                # The box widened by a hair so quantising never rounds a point out of it.
                w, so, e, n = np.float32(w) - 1e-5, np.float32(so) - 1e-5, np.float32(e) + 1e-5, np.float32(n) + 1e-5
                f.write(struct.pack("<IIffff", fp, cnt, w, so, e, n))
                seg = pts[fp:fp + cnt]
                q[fp:fp + cnt, 0] = np.clip(np.round((seg[:, 0] - w) / max(e - w, 1e-9) * 65535.0), 0, 65535)
                q[fp:fp + cnt, 1] = np.clip(np.round((seg[:, 1] - so) / max(n - so, 1e-9) * 65535.0), 0, 65535)
            f.write(q.tobytes())
            for layer, code, name, lon, lat in self.labels:
                f.write(s(layer, 24) + s(code, 8) + s(name, 40) + struct.pack("<ff", lon, lat))
        size = 36 + 56 * len(self.layers) + 16 * len(self.lods) + 24 * len(self.lines) + 4 * len(self.points) + 80 * len(self.labels)
        log("wrote %s: %d layers, %d chunks, %d points, %.1f MB" % (path, len(self.layers), len(self.lines), len(self.points), size / 1e6))


def simplified(lines, ppd):
    t = tol_for(ppd)
    out = []
    for ln in lines:
        g = ln.simplify(t, preserve_topology=False)
        out += [x for x in lines_of(g) if x.length > t * 0.5]
    return out


def ladder(sources, ppds=PPD):
    """sources: [(max_ppd, lines)] with coarser sources first; each PPD level takes the first source
    serving it (its max_ppd >= the level's), simplified for that level."""
    levels = []
    for ppd in ppds:
        src = next(lines for mp, lines in sources if mp >= ppd)
        levels.append((ppd, simplified(src, ppd)))
    return levels


def build(regions, out_path):
    w = Writer()
    log("vectors: Natural Earth")
    coast = [(10.0, ne_lines("ne_110m_coastline")), (40.0, ne_lines("ne_50m_coastline")), (1e9, ne_lines("ne_10m_coastline"))]
    w.line_layer("coast", ladder(coast, PPD_NE))
    borders = [(10.0, ne_lines("ne_110m_admin_0_boundary_lines_land")), (40.0, ne_lines("ne_50m_admin_0_boundary_lines_land")),
               (1e9, ne_lines("ne_10m_admin_0_boundary_lines_land"))]
    w.line_layer("borders", ladder(borders, PPD_NE))
    w.line_layer("lakes", ladder([(40.0, ne_lines("ne_50m_lakes")), (1e9, ne_lines("ne_10m_lakes"))], PPD_NE))
    w.line_layer("rivers", ladder([(40.0, ne_lines("ne_50m_rivers_lake_centerlines")), (1e9, ne_lines("ne_10m_rivers_lake_centerlines"))], PPD_NE))

    log("vectors: admin layers (geoBoundaries)")
    layers = {
        "us_states": ("USA_ADM1", "us_counties", "USA_ADM2"),
        "ro_judete": ("ROU_ADM1", "ro_communes", "ROU_ADM2"),
    }
    units = {}
    for lname, (adm1, dname, adm2) in layers.items():
        feats = geob(adm1)
        polys = [g for _, g in feats]
        units[lname] = feats
        full = boundaries(polys)
        w.line_layer(lname, ladder([(1e9, full)]))
        for props, g in feats:
            p = g.representative_point() if g.geom_type != "MultiPolygon" else max(g.geoms, key=lambda q: q.area).representative_point()
            code = props.get("shapeISO", "") or ""
            w.label(lname, code.split("-")[-1], props.get("shapeName", ""), p.x, p.y)
        log("  (%s: %d units)" % (lname, len(feats)))
        dpolys = [g for _, g in geob(adm2)]
        w.line_layer(dname, ladder([(1e9, boundaries(dpolys, 0.00012))]))

    log("vectors: region masks")
    for rid, r in regions.items():
        feats = units[r["layer"]]
        unit = next(g for p, g in feats if (p.get("shapeISO") or "") == r["unit"])
        country = shapely.union_all([g for _, g in feats]).buffer(0)
        # The masks dim the world around the unit: the unit close to its outline (~0.7 px at the
        # deepest dive), the country coarser and without its specks (islets under ~10 km2).
        unit_s = unit.simplify(0.0006)
        parts = [q for q in getattr(country, "geoms", [country]) if q.area > 0.001]
        country_s = MultiPolygon(parts).simplify(0.004).buffer(0)
        country_s = shapely.union_all([country_s, unit_s])
        w.line_layer("unit:" + rid, ladder([(1e9, list(lines_of(unit)))]))
        world = box(-180.0, -89.9, 180.0, 89.9)
        w.tri_layer("mask:%s:out" % rid, world.difference(country_s), 10.0)
        w.tri_layer("mask:%s:country" % rid, country_s.difference(unit_s), 4.0)
        w.tri_layer("mask:%s:unit" % rid, unit_s, 1.0)
    w.write(out_path)
