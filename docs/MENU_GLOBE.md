# Maps tab globe

The main menu's Maps tab shows a world globe behind its panels. The map list (left), the details
and session settings (right) and the tab bar are the menu's own widgets, unchanged
(docs/MENU_LOBBY.md). The globe and the selection lines follow the Maps tab design:
- a grey globe on black (limb darkening, two rim rings, stars when far, a graticule that fades as it zooms);
- white vector lines;
- the country and unit dimming, the unit's moving hatch and outline;
- a glow on the place, and lock lines that sweep in from the screen edges and frame it.

| Part | Code | Data |
|---|---|---|
| Projection, flights, tile levels, visible box (pure) | `src/ui/globe/globe_math.c` | |
| Loading, tile streaming, drawing, input, cvars | `src/ui/globe/globe.c` | `assets/globe/{tiles.dgt, vectors.dgv, regions.json, heights.json, height_*.png}` |
| Maps tab hook | `src/ui/mainmenu/mm_maps.c` (`mm_maps_background`), `mainmenu.c` | `assets/maps/*.json` `"globe"` |
| Offscreen UI layer, grey textures | `src/ui/gui/gui_draw.c` (`gd_offscreen_begin`, `gd_tris`, `gd_polyline`), `src/render/render_ui.c`, `render.c` (`render_ui_offscreen`), `vk_resources.c` (`render_texture_create_gray`), `shaders/post/ui_blur_down.comp` | |
| Data build | `tools/globe/build_globe.py` (`globe_fetch.py`, `globe_raster.py`, `globe_vectors.py`) | cache: `tools/globe/cache/` (git-ignored) |
| Tests | `tests/test_globe.c` (ctest `globe`), `tests/test_lobby.c` (every map's place and region) | |

## What it does

Opening the tab shows the whole globe over the Atlantic. After a beat it flies to the selected map.
Picking another map flies again. Each map file names its place:

```json
"globe": { "place": "Constanța", "lon": 28.634, "lat": 44.173, "region": "ro_ct" }
```

A **region** (`assets/globe/regions.json`) is the admin layer the camera dives into: the country,
the unit (a US state or a Romanian județ), and the radii of the legs (design pixels at a 900 px tall
screen, scaled by the screen height):

| Leg | To | Time | Notes |
|---|---|---|---|
| 1 overview | the country's whole admin layer (`overview`) | `mm_globe_travel` s (1.8), hold 0.9 s | pulls back mid-flight (`dip`), states / județe fade in |
| 2 unit | `focus` | 1.3 s, hold 0.7 s | the rest of the world dims, the unit's hatch and outline draw in, its name shows; the detail layer (counties / communes) is in by `detail_from` (none: with the admin layer) |
| 3 place | the map's lon / lat at `city_radius` | 1.4 s | the glow, then the lock lines frame it with the place name, coordinates and height |

A map in the region already shown takes one leg (1.1 s, a small pull back). Dragging the globe moves
it (up to ~260 design px, then rubber-band resistance). On release a damped spring brings it back,
keeping the fling. Until the first flight the globe turns slowly (`mm_globe_rotate`). The footer
hint ends with the phase (Standby / Travelling / Locked). The data credits sit under the globe.

The maps and their places: Story: 10th Floor → New York (region `us_ny`). Every dev map is a town
in Județul Constanța (region `ro_ct`): Dev Room → Constanța, Dev Room Vegetation → Medgidia,
Vegetation Valley → Hârșova, Terrain Yard → Cernavodă, Water Yard → Năvodari, Streaming Test
Terrain → Mangalia, Flat Ground → Vama Veche, Hand Pose Editor → Eforie.

## Rendering

The globe draws into an **offscreen UI layer**. `gd_offscreen_begin(target, clear)` sends the
drawing to an RGBA8 item-view target the size of the screen (`render_ui3d_target_create`).
`render_ui_offscreen` records it in a `ui_offscreen` pass (pipeline `PIPE_UI_RGBA8`) before the
backdrop blur. The frame's **blurred backdrop is then made from that target** instead of the scene
(`ui_blur_down.comp` mode 2), so the side panels blur the globe like they blur the dev room on the
other tabs. The target is then shown full screen (`gd_image`) under the menu's shading and panels.
The dev room still renders behind it. Note: with HDR output, the panels blur the globe's sRGB
values.

- **Texture:** an equirectangular tile pyramid streamed from `tiles.dgt`.
  - Level L has 2^(L+1) x 2^L tiles of 360 / 2^(L+1) degrees. Each tile is a 512 x 512 grey JPEG:
    510 texels inside plus a 1 texel border copied from its neighbours, so bilinear filtering never
    seams.
  - Levels 0..4 cover the world (16320 x 8160 at level 4, ~2.4 km per texel). Deeper levels exist
    only inside the regions' detail boxes: New York State to level 8, New York City and Județul
    Constanța to level 10 (~38 m per texel).
  - Each frame the globe walks the quadtree to the coarsest level with at least 0.9 texels per
    pixel at the centre (deeper only where tiles exist). A tile that is not loaded yet draws a
    sub-rect of its nearest loaded ancestor.
  - Each tile is a lon / lat grid mesh (4..32 cells a side) with the limb darkening
    (0.45 + 0.55 z^0.6) and the texture opacity (`mm_globe_texture`, 0.55) in its vertex colours.
    Vertices behind the limb are pushed onto it.
  - Tiles load 3 per frame, coarsest first. Each is decoded (stb_image), box-filtered into 10
    mips and uploaded as an R8 texture whose view swizzles to (r, r, r, 1)
    (`render_texture_create_gray`, ~350 KB).
  - The cache holds up to 160 tiles. It evicts the least recently drawn, never one drawn in the
    last 3 frames, and never levels 0..1 (the fallback).
- **Lines:** `gd_polyline` with mitred joins and a 1 px feather.
  - Each layer has levels of detail by pixels per degree. A level is simplified to ~0.6 px at the
    densest zoom it serves, and the globe picks the first level whose limit is at least the
    current pixels per degree.
  - Chunks of at most 192 points are culled by their box against the visible box
    (`globe_visible_box`). Points closer than ~1.2 px on screen are merged.
  - The globe view also draws the back hemisphere faintly.
- **Dimming:** three triangle layers per region (`mask:<region>:out / :country / :unit`) in black at
  0.62 / 0.30 / 0.08 x the highlight.
- **Hatch:** diagonal screen lines cut to the unit by the even-odd crossings of its projected outline.
- **Glow:** a ring fan with the alpha in the vertex colours.

## Data

`python tools/globe/build_globe.py [fetch] [world] [regions] [tiles] [vectors] [heights] [all]`
(numpy, scipy, Pillow, rasterio, shapely >= 2.1, pyshp, mgrs). The downloads are about 2 GB and are
cached in `tools/globe/cache/`. Every stage reuses the cached output of the one before; delete a
cache file to rebuild that stage. A full build takes about an hour, most of it Sentinel-2 reads.

| Source | Use | Licence |
|---|---|---|
| NASA Blue Marble (4096 x 2048, three-globe's mirror) | world albedo | public domain |
| Natural Earth 1:10m land cover raster `NE1_HR_LC` (21600 x 10800) | world land detail | public domain |
| Natural Earth 1:110m / 1:50m / 1:10m vectors | coasts, borders, lakes, rivers; land, lakes and glaciers rasterised for the texture | public domain |
| AWS Terrain Tiles (Terrarium PNG: SRTM, GMTED2010, ETOPO1, NED, ...) | height map with ocean depths: zoom 6 for the world, zoom 10..12 for the regions | attribution (AWS Open Data registry) |
| Copernicus Sentinel-2 L2A COGs (`sentinel-cogs` on S3) | regional imagery (true colour + scene classification) | Copernicus data licence, attribution |
| geoBoundaries ROU ADM1 / ADM2, USA ADM1 (TIGER) / ADM2 | județe, communes, states, counties, the region masks | CC BY 4.0 |

### Rasters

- **World** (`world_l4.npy`): albedo x relief, as a luminance like the design's Blue Marble grey.
  - Land is Blue Marble filled over the land mask only (normalized convolution), so coasts stay
    sharp. It carries Natural Earth I's land cover detail, glaciers are lifted to white, and the
    lows are lifted (^0.85) so land reads above the shelf seas.
  - The sea is Blue Marble's water tone x a shelf factor from the depth x its own relief.
  - Relief is a three-light hillshade (NW key, W and N fills) of the height map. The
    exaggeration grows as a texel gets coarser: 1.4 x (m per texel / 38)^0.36.
  - Past web mercator's ±85.05° the heights are blurred along the parallel. The last 1.5° at each
    pole eases to the row mean.
- **Regions** (`region_<id>_<k>.npz`): one per detail box, at its deepest level.
  - Per MGRS square, the least cloudy scenes of May .. October 2023 / 2024 (summer first) are read
    from the COG overview nearest the target resolution, only over the scene's footprint: up to 5
    scenes under 10 % cloud, or 14 under 40 % for a small box (a city, under 12 Mpx), where clouds
    over it on one date are filled from others.
  - Pixels the scene classification (SCL) marks as cloud, cirrus, cloud shadow or no data are
    dropped, and the masks are eroded 3 texels off the scene edges.
  - Each scene's luminance is matched to the world base at low frequency (no blocks where dates
    meet). The first valid pixel wins, best scene first, and a square only takes more scenes while
    its footprint has gaps.
  - The regional height map (terrarium zoom level + 2) adds a light hillshade on land. The patch
    feathers into the world over the outer 4 % of its box.
- **Tiles:** levels 0..4 are cut from the world. Every tile a detail box touches, at every level
  up to the box's, is the world (its level image, or level 4 upsampled) with the patches painted
  over it, coarse boxes first. The patches are Gaussian pre-filtered for coarser levels. JPEG
  quality is 84.

### `tiles.dgt` (little endian)

`'DGGT'`, version 1, tile size 512, border 1, count, max level, 0, 0. Then the index (level, x, y,
size: u32; offset: u64; sorted by level, row, column) and the JPEG blobs.

### `vectors.dgv`

The header is `'DGGV'`, version 1, then the counts of layers, lods, lines, points and labels. The
tables follow in that order:

| Table | Fields |
|---|---|
| layers | `{char name[40]; u32 kind (0 lines, 1 triangles), first_lod, lod_count, pad}` |
| lods | `{f32 max_ppd; u32 first_line, line_count, pad}` |
| lines | `{u32 first_point, point_count; f32 west, south, east, north}` |
| points | `{u16 x, y}`: lon = west + x / 65535 x (east - west), the same for lat |
| labels | `{char layer[24], code[8], name[40]; f32 lon, lat}` |

| Layer | Source | Levels (px / degree) |
|---|---|---|
| `coast`, `borders`, `lakes`, `rivers` | Natural Earth 110m (≤ 10), 50m (≤ 40), 10m | 10, 40, 160, 640 |
| `us_states`, `ro_judete` | geoBoundaries ADM1, unique boundaries (shared edges once) | 10 .. 2560 |
| `us_counties`, `ro_communes` | geoBoundaries ADM2 (coverage-simplified 0.00012°) | 10 .. 2560 |
| `unit:<region>` | the unit's rings | 10 .. 2560 |
| `mask:<region>:out / :country / :unit` | the world minus the country (10° cells), the country minus the unit (4°), the unit (1°): constrained Delaunay triangles; the country without islets under ~10 km², simplified 0.004°, the unit 0.0006° | one |

Labels hold every unit of each admin layer: its code, its name and a point inside it.

### Heights

`heights.json` lists 16-bit PNGs of metres + 11000: the world at 4096 x 2048 and each detail box at
up to 2048 texels across. The lock label reads the finest grid holding the place.

## Adding a place or a region

- **A map:** add `"globe": { "place", "lon", "lat", "region" }` to `assets/maps/<id>.json`.
  `test_lobby` checks every map has one and that its region exists.
- **A region:** add it to `assets/globe/regions.json`. Name its unit by ISO 3166-2 code
  (`shapeISO` in geoBoundaries) and its admin layer. The vector builder knows `us_states` and
  `ro_judete`; another country needs its geoBoundaries ADM1 / ADM2 in `globe_fetch.GEOB_FILES` and
  `globe_vectors.build`. Give it `detail` boxes, then run `build_globe.py regions tiles vectors
  heights`.

## Settings and commands

| Cvar | Default | |
|---|---|---|
| `mm_globe_rotate` | 1 | turn slowly until a map is picked |
| `mm_globe_texture` | 0.55 | texture opacity |
| `mm_globe_lines` | 1.0 | line weight (0.5 .. 2.5) |
| `mm_globe_travel` | 1.8 | seconds of the first leg into a region |
| `mm_globe_detail` | 1.0 | lakes, rivers, counties / communes |

Commands:
- `mm_globe_dump`: one log line with the phase, map, view, highlight, lock, loaded tiles, layers and
  height.
- `mm_globe_finish`: jump to the end of the current flight (for scripted screenshots).
- `mm_map <id>`: select a map; the globe flies there.

## Verification

- **ctest `globe`** (`test_globe`):
  - the projection and its inverse, including deep zoom and the antipode;
  - flights: three legs, the pull back, landing exactly, the time taken, the short way round;
  - tile sizes, levels and bounds, and the visible box (a city, a hemisphere, the date line);
  - the shipped `tiles.dgt`: the index sorted and unique, levels 0..4 complete, every tile a
    512 x 512 one-component JPEG, regional levels present;
  - the shipped `vectors.dgv`: its size matches its counts, every layer the game names exists,
    lods are ordered, chunk ranges are valid, triangle chunks hold whole triangles, and each
    region's layers exist.
- **ctest `lobby`:** every map has a place and its region.
- **Scripted run:** `tools/tests/mm_globe.cfg`
  (`deadground --test --menu --nosound --log mm_globe.log --script tools/tests/mm_globe.cfg`)
  takes these screenshots, each with an `mm_globe_dump` line: the idle globe, the US overview
  mid-flight, the New York, Constanța and Hârșova locks. They were checked on Linux (lavapipe).
- **Cost** (`mm_globe_dump` ms, CPU, tile loads included): about 8..9 ms at a country overview
  (the Romanian communes or the US states, ~140..200 k offscreen vertices), 1.5..3 ms locked on a
  place. The offscreen layer holds 393 k vertices and 1 M indices.
