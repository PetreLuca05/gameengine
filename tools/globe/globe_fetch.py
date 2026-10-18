"""Downloads for the Maps tab globe (docs/MENU_GLOBE.md "Data"), cached under tools/globe/cache/
(git-ignored): every file is fetched once, a re-run only fetches what is missing.

Sources (all open data):
* Natural Earth 1:10m / 1:50m / 1:110m vectors and the 1:10m land cover raster (public domain),
  naturalearth.s3.amazonaws.com.
* geoBoundaries ROU ADM1 / ADM2 (judete, communes; CC BY 4.0), github.com/wmgeolab/geoBoundaries.
* NASA Blue Marble (4096 x 2048, public domain), mirrored by vasturiano/three-globe.
* AWS Terrain Tiles (Terrarium PNGs: SRTM, GMTED2010, ETOPO1, NED, ...), s3 elevation-tiles-prod.
* Sentinel-2 L2A true colour COGs (Copernicus Sentinel data, modified), s3 sentinel-cogs.
"""
import concurrent.futures as cf
import json
import math
import os
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.environ.get("DG_GLOBE_CACHE", os.path.join(HERE, "cache"))

NE = "https://naturalearth.s3.amazonaws.com"
GEOB = "https://media.githubusercontent.com/media/wmgeolab/geoBoundaries/main/releaseData/gbOpen"
BLUE_MARBLE = "https://raw.githubusercontent.com/vasturiano/three-globe/master/example/img/earth-blue-marble.jpg"
TERRARIUM = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
S2 = "https://sentinel-cogs.s3.us-west-2.amazonaws.com"

# Natural Earth layers (scale, theme, name).
NE_VECTORS = [
    ("110m", "physical", "ne_110m_coastline"),
    ("110m", "cultural", "ne_110m_admin_0_boundary_lines_land"),
    ("50m", "physical", "ne_50m_coastline"),
    ("50m", "physical", "ne_50m_lakes"),
    ("50m", "physical", "ne_50m_rivers_lake_centerlines"),
    ("50m", "cultural", "ne_50m_admin_0_boundary_lines_land"),
    ("50m", "cultural", "ne_50m_admin_1_states_provinces_lines"),
    ("10m", "physical", "ne_10m_land"),
    ("10m", "physical", "ne_10m_coastline"),
    ("10m", "physical", "ne_10m_lakes"),
    ("10m", "physical", "ne_10m_rivers_lake_centerlines"),
    ("10m", "physical", "ne_10m_glaciated_areas"),
    ("10m", "cultural", "ne_10m_admin_0_countries"),
    ("10m", "cultural", "ne_10m_admin_0_boundary_lines_land"),
    ("10m", "cultural", "ne_10m_admin_1_states_provinces"),
    ("10m", "cultural", "ne_10m_admin_2_counties"),
    ("10m", "cultural", "ne_10m_urban_areas"),
    ("10m", "cultural", "ne_10m_roads"),
    ("10m", "cultural", "ne_10m_populated_places"),
]
NE_RASTERS = [("10m", "raster", "NE1_HR_LC")]


def log(*a):
    print(*a, flush=True)


def path(*p):
    full = os.path.join(CACHE, *p)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    return full


def get(url, dest, tries=5, quiet=False):
    """url -> dest (atomic, retried with backoff). Returns dest, or None for a 404 / 403."""
    if os.path.exists(dest):
        return dest
    tmp = dest + ".part"
    for k in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
            os.replace(tmp, dest)
            if not quiet:
                log("  got", os.path.relpath(dest, CACHE))
            return dest
        except urllib.error.HTTPError as e:
            if e.code in (403, 404):
                return None
            err = e
        except Exception as e:  # network hiccup: back off and retry
            err = e
        time.sleep(2 ** k)
    raise RuntimeError("download failed: %s (%s)" % (url, err))


def unzip(zpath, sub):
    out = path(sub, ".done")
    if not os.path.exists(out):
        with zipfile.ZipFile(zpath) as z:
            z.extractall(os.path.dirname(out))
        open(out, "w").close()
    return os.path.dirname(out)


def natural_earth():
    for scale, theme, name in NE_VECTORS + NE_RASTERS:
        z = get("%s/%s_%s/%s.zip" % (NE, scale, theme, name), path("ne", name + ".zip"))
        unzip(z, os.path.join("ne", name))


def ne_shp(name):
    d = os.path.join(CACHE, "ne", name)
    for f in os.listdir(d):
        if f.endswith(".shp"):
            return os.path.join(d, f)
    raise FileNotFoundError(name)


def ne_raster(name):
    d = os.path.join(CACHE, "ne", name)
    for f in os.listdir(d):
        if f.lower().endswith(".tif"):
            return os.path.join(d, f)
    raise FileNotFoundError(name)


# (country, level, file suffix): Romania full detail (judete, communes), the US from TIGER (states
# full, counties simplified).
GEOB_FILES = [("ROU", "ADM1", ""), ("ROU", "ADM2", ""), ("USA", "ADM1", ""), ("USA", "ADM2", "_simplified")]


def geoboundaries():
    out = {}
    for iso, adm, suffix in GEOB_FILES:
        out[iso + "_" + adm] = get("%s/%s/%s/geoBoundaries-%s-%s%s.geojson" % (GEOB, iso, adm, iso, adm, suffix), path("geob", "%s_%s.geojson" % (iso, adm)))
    return out


def blue_marble():
    return get(BLUE_MARBLE, path("bm", "earth-blue-marble.jpg"))


# ------------------------------------------------------------------ terrain tiles (web mercator)

def lonlat_to_tile(lon, lat, z):
    n = 2 ** z
    x = (lon + 180.0) / 360.0 * n
    lat = max(-85.0511, min(85.0511, lat))
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
    return x, y


def terrarium_tiles(z, bbox=None):
    n = 2 ** z
    if bbox is None:
        return [(x, y) for y in range(n) for x in range(n)]
    w, s, e, nn = bbox
    x0, y0 = lonlat_to_tile(w, nn, z)
    x1, y1 = lonlat_to_tile(e, s, z)
    return [(x, y) for y in range(int(y0), min(n - 1, int(y1)) + 1) for x in range(int(x0), min(n - 1, int(x1)) + 1)]


def terrarium(z, tiles, workers=24):
    todo = [(x, y) for x, y in tiles if not os.path.exists(path("terrarium", str(z), "%d_%d.png" % (x, y)))]
    if todo:
        log("terrarium z%d: %d tiles (%d cached)" % (z, len(todo), len(tiles) - len(todo)))
    with cf.ThreadPoolExecutor(workers) as ex:
        futs = [ex.submit(get, TERRARIUM.format(z=z, x=x, y=y), path("terrarium", str(z), "%d_%d.png" % (x, y)), 6, True) for x, y in todo]
        for i, f in enumerate(cf.as_completed(futs)):
            f.result()
            if (i + 1) % 500 == 0:
                log("  %d / %d" % (i + 1, len(todo)))
    return {(x, y): path("terrarium", str(z), "%d_%d.png" % (x, y)) for x, y in tiles}


# ------------------------------------------------------------------ Sentinel-2 (sentinel-cogs)

def s2_list(prefix):
    """Common prefixes under an S3 prefix (one level)."""
    out, token = [], None
    while True:
        q = "%s/?list-type=2&delimiter=/&prefix=%s" % (S2, prefix)
        if token:
            q += "&continuation-token=" + urllib.request.quote(token)
        for k in range(6):
            try:
                with urllib.request.urlopen(q, timeout=60) as r:
                    root = ET.fromstring(r.read())
                break
            except Exception:
                if k == 5:
                    raise
                time.sleep(2 ** k)
        ns = {"s": root.tag.split("}")[0].strip("{")}
        out += [p.find("s:Prefix", ns).text for p in root.findall("s:CommonPrefixes", ns)]
        t = root.find("s:NextContinuationToken", ns)
        if t is None:
            return out
        token = t.text


def s2_scenes(mgrs, months, want=3):
    """Scenes of one MGRS tile in the given (year, month)s with their cloud cover and no-data share,
    best first: [(cloud %, nodata %, scene prefix)]. Months are searched in order until `want` clear,
    whole scenes are found. Cached as JSON."""
    cache = path("s2", "scenes_%s.json" % mgrs)
    if os.path.exists(cache):
        return json.load(open(cache))
    zone, band, sq = mgrs[:2], mgrs[2], mgrs[3:]
    found = []

    def meta(p):
        name = p.rstrip("/").split("/")[-1]
        m = get("%s/%s%s.json" % (S2, p, name), path("s2", "meta", name + ".json"), quiet=True)
        if not m:
            return None
        props = json.load(open(m)).get("properties", {})
        return (props.get("eo:cloud_cover", 100.0), props.get("s2:nodata_pixel_percentage", 100.0), p)

    for year, month in months:
        prefixes = s2_list("sentinel-s2-l2a-cogs/%d/%s/%s/%d/%d/" % (int(zone), band, sq, year, month))
        with cf.ThreadPoolExecutor(12) as ex:
            found += [m for m in ex.map(meta, prefixes) if m]
        if sum(1 for c, n, _ in found if c < 3.0 and n < 5.0) >= want:
            break
    found.sort(key=lambda s: (s[1] > 5.0, s[0] + 0.2 * s[1]))
    json.dump(found, open(cache, "w"), indent=0)
    return found


def s2_tiles_for(bbox):
    """MGRS 100 km squares that cover a bbox (sampled every ~10 km)."""
    import mgrs as mgrs_mod

    m = mgrs_mod.MGRS()
    w, s, e, n = bbox
    out = set()
    steps = 40
    for j in range(steps + 1):
        for i in range(steps + 1):
            lon = w + (e - w) * i / steps
            lat = s + (n - s) * j / steps
            out.add(m.toMGRS(lat, lon, MGRSPrecision=0))
    return sorted(out)


if __name__ == "__main__":
    what = sys.argv[1:] or ["ne", "geob", "bm"]
    if "ne" in what:
        natural_earth()
    if "geob" in what:
        geoboundaries()
    if "bm" in what:
        blue_marble()
    if "terrain" in what:
        terrarium(6, terrarium_tiles(6))
