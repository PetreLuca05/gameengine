"""Rasters of the Maps tab globe (docs/MENU_GLOBE.md "Data"): the grey world texture and the regional
detail patches, cut into the equirectangular tile pyramid the game streams (assets/globe/tiles.dgt).

Pyramid: level L has 2^(L+1) x 2^L tiles, each 360 / 2^(L+1) degrees square, stored 512 x 512 with a
1 texel border taken from the neighbours (510 texels inside), so bilinear filtering never seams.
Levels 0..WORLD_LEVEL cover the world (16320 x 8160 at level 4, about 2.4 km per texel); deeper levels
exist only inside the regions' detail boxes (assets/globe/regions.json, level 10 = about 38 m).

The grey value is a luminance like the design's (NASA Blue Marble's): albedo x relief shading.
* World albedo: Blue Marble (4096) filled per land / water so coasts stay sharp (Natural Earth 10m
  land, lakes, glaciers rasterised at 2x), with the land cover detail of Natural Earth I (21600).
* Regional albedo: Sentinel-2 L2A true colour (the least cloudy summer scene per MGRS square,
  10 m data read from the COG overviews), matched to the world's tone at low frequency.
* Relief: multi-directional hillshade of the AWS Terrain Tiles height map (bathymetry included),
  exaggerated more where a texel is coarse.
"""
import io
import json
import math
import os
import struct

import numpy as np
from PIL import Image
from scipy import ndimage

import globe_fetch as gf

Image.MAX_IMAGE_PIXELS = None

TILE = 512
INNER = 510
WORLD_LEVEL = 4
WORLD_W = (2 ** (WORLD_LEVEL + 1)) * INNER   # 16320
WORLD_H = WORLD_W // 2
JPEG_QUALITY = 84


def px_per_deg(level):
    return (2 ** (level + 1)) * INNER / 360.0


def log(*a):
    print(*a, flush=True)


# ------------------------------------------------------------------ height (AWS terrarium)

def terrarium_decode(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return a[..., 0] * 256.0 + a[..., 1] + a[..., 2] / 256.0 - 32768.0


def merc_y(lat, z):
    lat = np.clip(lat, -85.0511, 85.0511)
    return (1.0 - np.arcsinh(np.tan(np.radians(lat))) / math.pi) / 2.0 * (2 ** z) * 256.0


def dem_equirect(z, bbox, w, h):
    """Height (m, negative below sea level) on a w x h equirectangular grid over bbox, bilinear from
    the terrarium tiles of zoom z (mercator x is linear in longitude: rows then columns)."""
    west, south, east, north = bbox
    tiles = gf.terrarium_tiles(z, None if z <= 6 and bbox == (-180, -90, 180, 90) else (west, south, east, north))
    files = gf.terrarium(z, tiles)
    xs = sorted({t[0] for t in tiles})
    ys = sorted({t[1] for t in tiles})
    x0, y0 = xs[0], ys[0]
    mos = np.zeros(((ys[-1] - y0 + 1) * 256, (xs[-1] - x0 + 1) * 256), np.float32)
    for (x, y), f in files.items():
        mos[(y - y0) * 256:(y - y0 + 1) * 256, (x - x0) * 256:(x - x0 + 1) * 256] = terrarium_decode(f)
    lons = west + (np.arange(w) + 0.5) * (east - west) / w
    lats = north - (np.arange(h) + 0.5) * (north - south) / h
    mx = (lons + 180.0) / 360.0 * (2 ** z) * 256.0 - x0 * 256.0 - 0.5
    my = merc_y(lats, z) - y0 * 256.0 - 0.5
    out = np.empty((h, w), np.float32)
    cols = np.arange(mos.shape[1], dtype=np.float64)
    for j in range(h):
        r = min(max(my[j], 0.0), mos.shape[0] - 1.001)
        r0 = int(r)
        t = r - r0
        row = mos[r0] * (1.0 - t) + mos[r0 + 1] * t
        out[j] = np.interp(mx, cols, row)
    return out


def hillshade(dem, lats, ppd, exag):
    """Relief factor (flat = 1): three lights (NW key, W and N fills), slopes in metres on the
    equirectangular grid (x shrinks with cos(latitude))."""
    dy = 111320.0 / ppd
    dx = dy * np.maximum(np.cos(np.radians(lats)), 0.05)[:, None]
    gy, gx = np.gradient(dem)
    sx = gx / dx * exag            # dz / d(east)
    sy = -gy / dy * exag           # dz / d(north): rows go south
    inv = 1.0 / np.sqrt(sx * sx + sy * sy + 1.0)
    nx, ny, nz = -sx * inv, -sy * inv, inv
    shade = np.zeros_like(dem)
    flat = 0.0
    for az, alt, wgt in ((315.0, 45.0, 0.6), (270.0, 55.0, 0.2), (0.0, 55.0, 0.2)):
        a, e = math.radians(az), math.radians(alt)
        lx, ly, lz = math.sin(a) * math.cos(e), math.cos(a) * math.cos(e), math.sin(e)
        shade += wgt * np.maximum(nx * lx + ny * ly + nz * lz, 0.0)
        flat += wgt * lz
    return shade / flat


def hillshade_strips(dem, lats, ppd, exag, rows=1024):
    """hillshade() in row strips (one row of overlap each side): the world does not fit in memory
    a dozen times over."""
    h = dem.shape[0]
    out = np.empty(dem.shape, np.float32)
    for r0 in range(0, h, rows):
        a, b = max(0, r0 - 1), min(h, r0 + rows + 1)
        hs = hillshade(np.asarray(dem[a:b], np.float32), lats[a:b], ppd, exag)
        out[r0:min(h, r0 + rows)] = hs[r0 - a:r0 - a + min(rows, h - r0)]
    return out


def polar_fill(dem, lats, limit=84.9):
    """Past web mercator's +-85.05 the tiles repeat their last row (streaks): those rows become the
    last good row blurred along the parallel, easing into it."""
    for north in (True, False):
        bad = np.where(lats > limit)[0] if north else np.where(lats < -limit)[0]
        if not len(bad):
            continue
        edge = bad[-1] + 1 if north else bad[0] - 1
        smooth = ndimage.gaussian_filter1d(dem[edge], 120.0, mode="wrap")
        for k, r in enumerate(bad[::-1] if north else bad):
            t = min(1.0, (k + 1) / 40.0)
            dem[r] = dem[edge] * (1.0 - t) + smooth * t


def relief_exaggeration(ppd):
    m_per_px = 111320.0 / ppd
    return 1.4 * (m_per_px / 38.0) ** 0.36


# ------------------------------------------------------------------ vectors -> masks

def rasterize(shapes, bbox, w, h, ss=2):
    """Coverage 0..1 of shapes on the grid (supersampled ss x ss)."""
    from rasterio import features, transform

    tr = transform.from_bounds(*bbox, w * ss, h * ss)
    m = features.rasterize(((g, 1) for g in shapes), out_shape=(h * ss, w * ss), transform=tr, dtype=np.uint8, all_touched=False)
    if ss > 1:
        m = m.reshape(h, ss, w, ss).mean(axis=(1, 3))
    return m.astype(np.float32)


def shp_geoms(name, bbox=None):
    import shapefile
    from shapely.geometry import box, shape

    clip = box(*bbox) if bbox else None
    out = []
    for sr in shapefile.Reader(gf.ne_shp(name)).iterShapeRecords():
        if sr.shape.shapeType == 0:   # NULL shape
            continue
        g = shape(sr.shape.__geo_interface__)
        if clip is not None:
            if not g.intersects(clip):
                continue
        out.append(g)
    return out


def water_masks(bbox, w, h):
    """(land, water inside land = lakes, glacier) coverage on the grid."""
    land = rasterize(shp_geoms("ne_10m_land", bbox), bbox, w, h)
    lakes = rasterize(shp_geoms("ne_10m_lakes", bbox), bbox, w, h)
    ice = rasterize(shp_geoms("ne_10m_glaciated_areas", bbox), bbox, w, h)
    return np.clip(land - lakes, 0, 1), lakes, ice


# ------------------------------------------------------------------ helpers

def lum(rgb):
    rgb = np.asarray(rgb, np.float32) / 255.0
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def guided_fill(img, mask, sigma):
    """img averaged over mask only (normalized convolution), defined everywhere."""
    num = ndimage.gaussian_filter(img * mask, sigma, mode="wrap")
    den = ndimage.gaussian_filter(mask, sigma, mode="wrap")
    return num / np.maximum(den, 1e-4)


def resize(a, w, h):
    return np.asarray(Image.fromarray(a.astype(np.float32), mode="F").resize((w, h), Image.BICUBIC), np.float32)


def downsample2(a):
    h, w = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
    a = a[:h, :w]
    return a.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))


# ------------------------------------------------------------------ world

def world_composite(cache_npy):
    """The level-4 world (WORLD_H x WORLD_W float32 0..1), cached as .npy (its height map beside it).
    Built in stages that free their inputs: the full grid is 0.5 GB per float plane."""
    if os.path.exists(cache_npy):
        return np.load(cache_npy, mmap_mode="r")
    W, H = WORLD_W, WORLD_H
    full = (-180.0, -90.0, 180.0, 90.0)
    lats = 90.0 - (np.arange(H) + 0.5) * 180.0 / H
    dem_path = cache_npy.replace(".npy", "_dem.npy")
    if not os.path.exists(dem_path):
        log("world: height map (terrarium z6)")
        np.save(dem_path, dem_equirect(6, full, W, H))
    dem = np.array(np.load(dem_path, mmap_mode="r"), np.float32)
    polar_fill(dem, lats)
    log("world: masks")
    land, lakes, ice = water_masks(full, W, H)
    log("world: albedo")
    bm = lum(Image.open(gf.blue_marble()).convert("RGB"))
    bh, bw = bm.shape
    land_s = resize(land, bw, bh)
    bm_land = resize(guided_fill(bm, land_s, 1.5), W, H)
    ne1 = lum(Image.open(gf.ne_raster("NE1_HR_LC")).convert("RGB").resize((W, H), Image.LANCZOS))
    detail = ne1 / np.maximum(guided_fill(ne1, land, 6.0), 1e-3)
    del ne1
    np.clip(detail, 0.7, 1.35, out=detail)
    detail **= 0.7
    albedo = bm_land * np.where(land > 0.5, detail, 1.0).astype(np.float32)
    del detail, bm_land
    albedo = albedo * (1.0 - ice) + np.maximum(albedo, 0.86) * ice
    del ice
    log("world: relief")
    ppd = px_per_deg(WORLD_LEVEL)
    relief = hillshade_strips(dem, lats, ppd, relief_exaggeration(ppd))
    # Land reads above the sea (Blue Marble's forests are darker than its shelf seas): lows lifted.
    land_tone = np.maximum(albedo * np.clip(1.0 + 0.95 * (relief - 1.0), 0.25, 1.9), 0.0) ** 0.85
    del albedo
    out = land_tone * land
    del land_tone
    bm_water = resize(guided_fill(bm, 1.0 - land_s, 2.0), W, H)
    shelf = 0.9 + 0.22 * np.exp(-np.maximum(-dem, 0.0) / 250.0)
    water = 0.62 * bm_water * shelf * (1.0 + 0.45 * (relief - 1.0))
    del shelf, relief
    out += (water * (1.0 - lakes) + bm_water * 0.95 * lakes) * (1.0 - land)
    del water, bm_water
    # The last degree at each pole: towards the row's mean (the sources pinch there).
    for r in range(H):
        t = (abs(lats[r]) - 88.5) / 1.5
        if t > 0:
            out[r] = out[r] * (1.0 - min(1.0, t)) + out[r].mean() * min(1.0, t)
    np.clip(out, 0.0, 1.0, out=out)
    np.save(cache_npy, out.astype(np.float32))
    return out


def world_dem(cache_npy):
    return np.load(cache_npy.replace(".npy", "_dem.npy"), mmap_mode="r")


# ------------------------------------------------------------------ tiles

def tile_bbox(level, x, y):
    d = 360.0 / (2 ** (level + 1))
    west = -180.0 + x * d
    north = 90.0 - y * d
    return (west, north - d, west + d, north)


def cut(img, level_w, x, y):
    """Tile (x, y) of a full-world level image with the 1 texel border (lon wraps, lat clamps)."""
    h = img.shape[0]
    cols = (np.arange(x * INNER - 1, (x + 1) * INNER + 1)) % level_w
    rows = np.clip(np.arange(y * INNER - 1, (y + 1) * INNER + 1), 0, h - 1)
    return img[np.ix_(rows, cols)]


def encode(tile):
    b = io.BytesIO()
    Image.fromarray(np.clip(tile * 255.0 + 0.5, 0, 255).astype(np.uint8), mode="L").save(b, "JPEG", quality=JPEG_QUALITY, optimize=True)
    return b.getvalue()


def world_tiles(world):
    """{(level, x, y): jpeg} for levels 0..WORLD_LEVEL."""
    out = {}
    img = np.asarray(world, np.float32)
    for level in range(WORLD_LEVEL, -1, -1):
        lw = (2 ** (level + 1)) * INNER
        for y in range(2 ** level):
            for x in range(2 ** (level + 1)):
                out[(level, x, y)] = encode(cut(img, lw, x, y))
        log("  level %d: %d tiles" % (level, 2 ** (2 * level + 1)))
        if level:
            img = downsample2(img)
    return out


def write_pack(tiles, path):
    """assets/globe/tiles.dgt: 'DGGT', version, tile size, border, count, max level, then the index
    (level, x, y, size, offset u64; sorted by level, y, x) and the JPEG blobs."""
    keys = sorted(tiles, key=lambda k: (k[0], k[2], k[1]))
    head = 32 + 24 * len(keys)
    index, blobs, off = [], [], head
    for k in keys:
        b = tiles[k]
        index.append(struct.pack("<IIIIQ", k[0], k[1], k[2], len(b), off))
        blobs.append(b)
        off += len(b)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sIIIIIII", b"DGGT", 1, TILE, 1, len(keys), max(k[0] for k in keys), 0, 0))
        f.write(b"".join(index))
        f.write(b"".join(blobs))
    log("wrote %s: %d tiles, %.1f MB" % (path, len(keys), off / 1e6))


# ------------------------------------------------------------------ regions (Sentinel-2 + 30 m height)

S2_MONTHS = [(2024, 6), (2024, 7), (2024, 8), (2023, 6), (2023, 7), (2023, 8), (2024, 9), (2023, 9), (2024, 5), (2023, 5), (2024, 10), (2023, 10)]


def grid_lonlat(bbox, w, h):
    west, south, east, north = bbox
    lons = west + (np.arange(w) + 0.5) * (east - west) / w
    lats = north - (np.arange(h) + 0.5) * (north - south) / h
    return lons, lats


def s2_window(src, bbox, w, h):
    """The part of the w x h grid over bbox a scene covers: (row0, col0, rows, cols) or None."""
    from rasterio.warp import transform_bounds

    sb = transform_bounds(src.crs, "EPSG:4326", *src.bounds, densify_pts=21)
    west, south, east, north = max(bbox[0], sb[0]), max(bbox[1], sb[1]), min(bbox[2], sb[2]), min(bbox[3], sb[3])
    if east <= west or north <= south:
        return None
    c0 = max(0, int(math.floor((west - bbox[0]) / (bbox[2] - bbox[0]) * w)))
    c1 = min(w, int(math.ceil((east - bbox[0]) / (bbox[2] - bbox[0]) * w)))
    r0 = max(0, int(math.floor((bbox[3] - north) / (bbox[3] - bbox[1]) * h)))
    r1 = min(h, int(math.ceil((bbox[3] - south) / (bbox[3] - bbox[1]) * h)))
    if c1 - c0 < 2 or r1 - r0 < 2:
        return None
    return r0, c0, r1 - r0, c1 - c0


def s2_read(scene, bbox, w, h, band="TCI", nearest=False):
    """A scene's band (TCI: 3 x uint8 true colour; SCL: the scene classification) on the part of
    the w x h EPSG:4326 grid over bbox it covers, read from the COG overview nearest the target
    resolution: (array [bands, rows, cols], (row0, col0)) or (None, None)."""
    import rasterio
    from rasterio.enums import Resampling
    from rasterio.warp import reproject, transform_bounds
    from rasterio.windows import from_bounds as win_from_bounds
    from rasterio import transform as rtr

    url = "/vsicurl/%s/%s%s.tif" % (gf.S2, scene, band)
    with rasterio.Env(GDAL_DISABLE_READDIR_ON_OPEN="EMPTY_DIR", CPL_VSIL_CURL_ALLOWED_EXTENSIONS=".tif", GDAL_HTTP_MAX_RETRY="5", GDAL_HTTP_RETRY_DELAY="2"):
        with rasterio.open(url) as src:
            sub = s2_window(src, bbox, w, h)
            if sub is None:
                return None, None
            r0, c0, rows, cols = sub
            dx, dy = (bbox[2] - bbox[0]) / w, (bbox[3] - bbox[1]) / h
            sbox = (bbox[0] + c0 * dx, bbox[3] - (r0 + rows) * dy, bbox[0] + (c0 + cols) * dx, bbox[3] - r0 * dy)
            b = transform_bounds("EPSG:4326", src.crs, *sbox, densify_pts=21)
            win = win_from_bounds(*b, transform=src.transform).intersection(rasterio.windows.Window(0, 0, src.width, src.height))
            if win.width < 2 or win.height < 2:
                return None, None
            native = abs(src.transform.a)
            f = max(1, int(dy * 111320.0 / native / 1.2))
            ow, oh = max(2, int(win.width / f)), max(2, int(win.height / f))
            rs = Resampling.nearest if nearest else Resampling.average
            arr = src.read(window=win, out_shape=(src.count, oh, ow), resampling=rs)
            wt = src.window_transform(win) * rtr.Affine.scale(win.width / ow, win.height / oh)
            out = np.zeros((src.count, rows, cols), np.uint8)
            dst_tr = rtr.from_bounds(*sbox, cols, rows)
            for k in range(src.count):
                reproject(arr[k], out[k], src_transform=wt, src_crs=src.crs, dst_transform=dst_tr, dst_crs="EPSG:4326",
                          resampling=Resampling.nearest if nearest else Resampling.bilinear, src_nodata=0, dst_nodata=0)
    return out, (r0, c0)


# Scene classification (L2A SCL) classes kept: vegetation, bare soil, water, unclassified, snow, dark
# area. Dropped: no data, saturated, cloud shadow, cloud (medium / high), thin cirrus.
SCL_OK = np.zeros(256, bool)
SCL_OK[[2, 4, 5, 6, 7, 11]] = True


def s2_mosaic(bbox, w, h, base, per_tile=5, max_cloud=10.0):
    """Least cloudy summer scenes per MGRS square, cloud-masked per pixel (SCL), each scene's tone
    matched to the world base at low frequency (no blocks where dates meet), first valid pixel wins
    (best scene first). A square takes more scenes only while its footprint has gaps (clouds)."""
    lumv = np.zeros((h, w), np.float32)
    have = np.zeros((h, w), bool)
    sig = max(6.0, w / 80.0)
    for mg in gf.s2_tiles_for(bbox):
        scenes = [s for s in gf.s2_scenes(mg, S2_MONTHS, want=per_tile) if s[0] < max_cloud]
        foot = None
        used = 0
        for cloud, nodata, scene in scenes:
            if used >= per_tile:
                break
            if foot is not None:
                r0, c0, fp = foot
                gap = (fp & ~have[r0:r0 + fp.shape[0], c0:c0 + fp.shape[1]]).sum()
                if gap < 0.002 * fp.sum():
                    break
            try:
                rgb, o = s2_read(scene, bbox, w, h)
                if rgb is None:
                    break
                scl, o2 = s2_read(scene, bbox, w, h, "SCL", nearest=True)
            except Exception as e:  # a missing / broken COG: the next scene
                log("    skip %s (%s)" % (scene, e))
                continue
            used += 1
            r0, c0 = o
            rows, cols = rgb.shape[1:]
            data = rgb.min(axis=0) > 0
            if scl is None or scl.shape[1:] != (rows, cols) or o2 != o:
                continue
            valid = data & SCL_OK[scl[0]]
            # Off the jagged scene edges and cloud rims (a few texels).
            valid = ndimage.binary_erosion(valid, iterations=3)
            fp = ndimage.binary_erosion(data, iterations=3)
            if foot is None:
                foot = (r0, c0, fp)
            else:
                fr0, fc0, ff = foot
                if (fr0, fc0) == (r0, c0) and ff.shape == fp.shape:
                    foot = (r0, c0, ff | fp)
            hv = have[r0:r0 + rows, c0:c0 + cols]
            fill = valid & ~hv
            if not fill.any():
                continue
            lv = lum(np.moveaxis(rgb, 0, -1))
            vf = valid.astype(np.float32)
            bsub = np.asarray(base[r0:r0 + rows, c0:c0 + cols], np.float32)
            gain = guided_fill(bsub, vf, sig) / np.maximum(guided_fill(lv, vf, sig), 1e-3)
            lsub = lumv[r0:r0 + rows, c0:c0 + cols]
            lsub[fill] = (lv * np.clip(gain, 0.3, 3.5))[fill]
            hv |= fill
        log("  s2 %s: %d scenes used of %d clear" % (mg, used, len(scenes)))
    return lumv, have


def sample(img, bbox_img, lons, lats, sigma=0.0):
    """Bilinear lookup of a georeferenced image (bbox_img) at a lon x lat grid, optionally blurred."""
    h, w = img.shape
    west, south, east, north = bbox_img
    x = (lons - west) / (east - west) * w - 0.5
    y = (north - lats) / (north - south) * h - 0.5
    src = ndimage.gaussian_filter(np.asarray(img, np.float32), sigma) if sigma > 0.3 else img
    yy, xx = np.meshgrid(y, x, indexing="ij")
    return ndimage.map_coordinates(src, [yy, xx], order=1, mode="nearest").astype(np.float32)


def world_sample(world, lons, lats):
    """The world level image (wraps in longitude) at a grid."""
    x = (lons + 180.0) / 360.0 * world.shape[1] - 0.5
    y = (90.0 - lats) / 180.0 * world.shape[0] - 0.5
    x0, x1 = int(max(0, math.floor(x.min()) - 2)), int(min(world.shape[1], math.ceil(x.max()) + 3))
    y0, y1 = int(max(0, math.floor(y.min()) - 2)), int(min(world.shape[0], math.ceil(y.max()) + 3))
    sub = np.asarray(world[y0:y1, x0:x1], np.float32)
    yy, xx = np.meshgrid(y - y0, x - x0, indexing="ij")
    return ndimage.map_coordinates(sub, [yy, xx], order=1, mode="nearest").astype(np.float32)


def region_patch(entry, world, cache_npy):
    """One detail box at its deepest level: grey, feather alpha and height, cached as .npz."""
    if os.path.exists(cache_npy):
        d = np.load(cache_npy)
        return d["img"], d["alpha"], d["dem"], tuple(d["bbox"])
    bbox = tuple(entry["bbox"])
    level = entry["level"]
    ppd = px_per_deg(level)
    w = int(round((bbox[2] - bbox[0]) * ppd))
    h = int(round((bbox[3] - bbox[1]) * ppd))
    lons, lats = grid_lonlat(bbox, w, h)
    log("region %s level %d: %d x %d" % (bbox, level, w, h))
    dem = dem_equirect(min(14, level + 2), bbox, w, h)
    base = world_sample(world, lons, lats)
    # Small boxes (a city) can afford many scenes: clouds over it in one date are filled from others.
    small = w * h < 12e6
    s2, have = s2_mosaic(bbox, w, h, base, per_tile=14 if small else 5, max_cloud=40.0 if small else 10.0)
    land, lakes, _ = water_masks(bbox, w, h)
    hv = have.astype(np.float32)
    img = np.where(have, s2, base).astype(np.float32)
    relief = hillshade(dem, lats, ppd, relief_exaggeration(ppd) * 0.6)
    img *= np.where(land > 0.5, np.clip(1.0 + 0.6 * (relief - 1.0), 0.5, 1.6), 1.0)
    np.clip(img, 0.0, 1.0, out=img)
    # Feather: 1 inside, 0 at the box edge (and where Sentinel-2 had nothing).
    ramp = max(6.0, min(w, h) * 0.04)
    ex = np.minimum(np.arange(w) + 0.5, w - np.arange(w) - 0.5) / ramp
    ey = np.minimum(np.arange(h) + 0.5, h - np.arange(h) - 0.5) / ramp
    alpha = np.clip(np.minimum(ey[:, None], ex[None, :]), 0, 1).astype(np.float32)
    alpha *= np.clip(ndimage.gaussian_filter(hv, ramp * 0.25), 0, 1) if not have.all() else 1.0
    np.savez(cache_npy, img=img, alpha=alpha, dem=dem.astype(np.float32), bbox=np.array(bbox))
    return img, alpha, dem, bbox


def tiles_in(level, bbox):
    d = 360.0 / (2 ** (level + 1))
    x0 = int(math.floor((bbox[0] + 180.0) / d))
    x1 = int(math.floor((bbox[2] + 180.0 - 1e-9) / d))
    y0 = int(math.floor((90.0 - bbox[3]) / d))
    y1 = int(math.floor((90.0 - bbox[1] - 1e-9) / d))
    return [(x, y) for y in range(max(0, y0), min(2 ** level - 1, y1) + 1) for x in range(max(0, x0), min(2 ** (level + 1) - 1, x1) + 1)]


def tile_grid(level, x, y):
    """lon / lat of the 512 x 512 texel centres of tile (x, y) (border texels included)."""
    d = 360.0 / (2 ** (level + 1))
    step = d / INNER
    west, north = -180.0 + x * d, 90.0 - y * d
    lons = west + (np.arange(TILE) - 0.5) * step
    lats = north - (np.arange(TILE) - 0.5) * step
    return ((lons + 180.0) % 360.0) - 180.0, np.clip(lats, -89.999, 89.999)


def region_tiles(tiles, world_levels, world, patches):
    """Every tile a detail box touches, at every level up to the box's: the base (the world level
    image, or the level-4 world upsampled deeper) with the patches over it, coarse boxes first."""
    keys = set()
    for img, alpha, dem, bbox, level in patches:
        for L in range(0, level + 1):
            keys |= {(L, x, y) for x, y in tiles_in(L, bbox)}
    log("region tiles: %d" % len(keys))
    blurred = {}
    for L, x, y in sorted(keys):
        lons, lats = tile_grid(L, x, y)
        if L <= WORLD_LEVEL:
            base = world_sample(world_levels[L], lons, lats)
        else:
            base = world_sample(world, lons, lats)
        for k, (img, alpha, dem, bbox, level) in enumerate(patches):
            if L > level:
                continue
            tb = tile_bbox(L, x, y)
            if tb[2] < bbox[0] or tb[0] > bbox[2] or tb[3] < bbox[1] or tb[1] > bbox[3]:
                continue
            f = 2 ** (level - L)
            if (k, L) not in blurred:
                s = 0.5 * f if f > 1 else 0.0
                blurred[(k, L)] = (ndimage.gaussian_filter(img, s) if s else img, ndimage.gaussian_filter(alpha, s) if s else alpha)
            bi, ba = blurred[(k, L)]
            a = sample(ba, bbox, lons, lats)
            inside = ((lons >= bbox[0]) & (lons <= bbox[2]))[None, :] & ((lats >= bbox[1]) & (lats <= bbox[3]))[:, None]
            a = np.where(inside, a, 0.0)
            base = base * (1.0 - a) + sample(bi, bbox, lons, lats) * a
        tiles[(L, x, y)] = encode(base)


def world_level_images(world):
    out = {WORLD_LEVEL: np.asarray(world, np.float32)}
    for L in range(WORLD_LEVEL - 1, -1, -1):
        out[L] = downsample2(out[L + 1])
    return out


def write_heights(world_dem, patches, out_dir):
    """Height maps for the game (elevation read-outs): the world at 4096 x 2048 and each detail box
    at up to 2048 texels across, 16-bit PNGs of (metres + 11000), listed in heights.json."""
    def png(a, path):
        v = np.clip(np.round(np.asarray(a, np.float32) + 11000.0), 0, 65535).astype(np.uint16)
        Image.fromarray(v).save(path)

    os.makedirs(out_dir, exist_ok=True)
    grids = []
    wd = resize(np.asarray(world_dem, np.float32), 4096, 2048)
    png(wd, os.path.join(out_dir, "height_world.png"))
    grids.append({"file": "assets/globe/height_world.png", "bbox": [-180, -90, 180, 90]})
    for k, (img, alpha, dem, bbox, level) in enumerate(patches):
        h, w = dem.shape
        s = min(1.0, 2048.0 / max(w, h))
        name = "height_%d.png" % k
        png(resize(dem, max(2, int(w * s)), max(2, int(h * s))), os.path.join(out_dir, name))
        grids.append({"file": "assets/globe/" + name, "bbox": [round(v, 6) for v in bbox]})
    json.dump({"_doc": "Globe height maps (docs/MENU_GLOBE.md): 16-bit PNG, value = metres + 11000, equirectangular over bbox [west, south, east, north]. Finest grid containing a point wins.",
               "offset": 11000, "grids": grids}, open(os.path.join(out_dir, "heights.json"), "w"), indent=1)
