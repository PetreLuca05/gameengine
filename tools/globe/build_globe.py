#!/usr/bin/env python3
"""Builds the Maps tab globe's data (docs/MENU_GLOBE.md "Data") from open data on the web:

    python tools/globe/build_globe.py [fetch] [world] [regions] [tiles] [vectors] [heights] [all]

  fetch    downloads (cached in tools/globe/cache/, about 2 GB with the Sentinel-2 metadata)
  world    the level-4 world texture (16320 x 8160) + its height map (cache/world_l4*.npy)
  regions  every detail box of assets/globe/regions.json: Sentinel-2 + 30 m height (cache/region_*.npz)
  tiles    assets/globe/tiles.dgt (the streamed tile pyramid, JPEG)
  vectors  assets/globe/vectors.dgv (coasts, borders, lakes, rivers, admin layers, focus masks, labels)
  heights  assets/globe/height_*.png + heights.json (elevation read-outs)

Needs numpy, scipy, Pillow, rasterio, shapely >= 2.1, pyshp, mgrs. Every stage reuses the cached
output of the one before it; delete a cache file to rebuild that stage.
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import globe_fetch as gf  # noqa: E402

OUT = os.path.join(ROOT, "assets", "globe")


def regions():
    return json.load(open(os.path.join(OUT, "regions.json")))["regions"]


def detail_entries():
    out = []
    for rid, r in regions().items():
        for k, e in enumerate(r.get("detail", [])):
            out.append(("%s_%d" % (rid, k), e))
    return out


def patches(world):
    import globe_raster as gr

    out = []
    for name, e in detail_entries():
        img, alpha, dem, bbox = gr.region_patch(e, world, gf.path("region_%s.npz" % name))
        out.append((img, alpha, dem, bbox, e["level"]))
    # Coarse boxes first: a deeper box (a city inside its state) paints over them.
    out.sort(key=lambda p: p[4])
    return out


def main(argv):
    stages = argv or ["all"]
    if "all" in stages:
        stages = ["fetch", "world", "regions", "tiles", "vectors", "heights"]
    t0 = time.time()
    if "fetch" in stages:
        gf.natural_earth()
        gf.geoboundaries()
        gf.blue_marble()
        gf.terrarium(6, gf.terrarium_tiles(6))
    world = None
    if any(s in stages for s in ("world", "regions", "tiles", "heights")):
        import globe_raster as gr

        world = gr.world_composite(gf.path("world_l4.npy"))
    if any(s in stages for s in ("regions", "tiles", "heights")):
        ps = patches(world)
    if "tiles" in stages:
        import globe_raster as gr

        tiles = gr.world_tiles(world)
        gr.region_tiles(tiles, gr.world_level_images(world), world, ps)
        gr.write_pack(tiles, os.path.join(OUT, "tiles.dgt"))
    if "vectors" in stages:
        import globe_vectors as gv

        gv.build(regions(), os.path.join(OUT, "vectors.dgv"))
    if "heights" in stages:
        import globe_raster as gr

        gr.write_heights(gr.world_dem(gf.path("world_l4.npy")), ps, OUT)
    print("done in %.0f s" % (time.time() - t0))


if __name__ == "__main__":
    main(sys.argv[1:])
