# Surface textures

Terrain, roads, open-world buildings and the grass / wheat cards are textured. The open-world terrain itself is now shaded by the terrain set (docs/TERRAIN_SHADING.md) and the grass by the grass assets (docs/FOLIAGE_GRASS.md); this page covers the surface layers that roads and buildings keep, and the terrain fallback (`r_terrain 0`). Code:
`src/render/surface_tex.c` (arrays, material map, bindings), `shaders/surfaces.glsl` (shading),
`src/world/openworld.c` (layers per prim, the map upload), `src/render/vk_vgeo.c`
(`foliage_card_file`: grass / wheat cards). Tools: `tools/vfxgen/texgen.py` (+ `texgen_recipes.json`),
`tools/vfxgen/foliage_atlas.py`.

## What is textured

| Surface | How | Layers |
|---|---|---|
| Open-world terrain (chunks + far tiles) | `MAT_TERRAIN`: the material raster as a map, up to 4 layers blended per pixel | the 14 `MgMaterial`s |
| Road ribbons | `MAT_LAYERED`, layer in the vertex alpha | asphalt, dirt road, concrete (runways) |
| POI prims (buildings, walls, roofs, containers, props) | `MAT_LAYERED`, layer by shape + surface name (`prim_layer`) | below |
| Grass tufts, wheat clumps (vgeo masked cards) | alpha-masked card textures | `grass_blades.png`, `wheat.png` |

`prim_layer` (openworld.c): pitched roofs (wood wedges) = shingles; `brick`, `plaster`, `wood`; `metal_thin`
= corrugated sheet metal; `metal_thick` = rusted steel; `asphalt`, `gravel`; `concrete` / unnamed = concrete.
Glass, lamps, glossy paint (car bodies) and cloth stay plain (layer 255).

Not textured (by design or out of scope): the devroom grid materials, water (translucent, SSR), vgeo
test content (rock / torus / statue), glTF models without images (vehicles, barrel), characters, the
`map noise` / `map flat` test terrains.

## Layers (`enum SurfTexLayer`, render.h)

20 layers, two `sampler2DArray`s in frame set bindings 12 / 13, the terrain material map at 14:

- `<name>_albedo.jpg`: 1024 px sRGB albedo (JPEG q92).
- `<name>_nrh.png`: 512 px, R G tangent normal (0.5 flat, G = image up), B roughness, A height.
- `<name>_ao.png`: 512 px grey, the AO baked from the height ("Ambient occlusion" below). Loaded into the
  albedo array's alpha (bilinear up to 1024, wrapping), so it costs no GPU memory and no extra tap.
- `metal_metal.png`, `rust_metal.png`: 512 px, R bare-metal mask, G paint wear ("Metal" below). Appended
  to the nrh array as layers 20 and 21 (`STEX_METAL_LAYERS`, `SURF_METAL_SLOT`).

Mips are box-filtered at load (about 0.7 s for all 20 layers, on the first open-world map). The arrays
use the material sampler (`r_aniso`, `r_texture_quality`). Memory: about 143 MB (albedo 20 × 5.6 MB,
nrh 22 × 1.4 MB: the two metal mask layers add 2.7 MB, the AO nothing). `r_surfaces 0` turns the
shading off (vertex colours, for comparison).

| # | Layer | Tile (m) | Prompt (+ the shared style line) |
|---|---|---|---|
| 0 | grass | 2.5 | short dense green wild grass lawn seen from directly above, mixed blades and small clover |
| 1 | meadow | 3 | dry late-summer meadow grass from above, straw yellow and olive green blades |
| 2 | forest | 3 | forest floor from above, pine needles, dead leaves, twigs, moss, humus |
| 3 | dirt | 3 | bare brown earth from above, dry cracked dirt, pebbles, tiny roots |
| 4 | rock | 6 | natural rough grey granite boulder surface, cracks, lichen, no tiles / joints |
| 5 | sand | 4 | fine beige beach sand from above, subtle ripples |
| 6 | gravel | 2 | loose grey gravel from above, angular crushed stones |
| 7 | asphalt | 4 | old weathered dark grey asphalt from above, aggregate, small cracks, no markings |
| 8 | dirt_road | 4 | flat lay packed light brown earth with embedded small stones, no tracks |
| 9 | concrete | 4 | one continuous weathered grey concrete surface, no joints / slab lines |
| 10 | field | 3 | harvested wheat stubble from above |
| 11 | crop | 3 | young green crop plants from above covering dark soil |
| 12 | plowed | 4 | freshly plowed soil from above, clods and furrows |
| 13 | mud | 3 | uniform wet dark brown mud with small stones, no puddles |
| 14 | brick | 1.1 | red brick running bond, horizontal courses, front-on |
| 15 | plaster | 2.5 | weathered off-white stucco, front-on |
| 16 | wood | 1.4 | weathered grey-brown vertical planks, front-on |
| 17 | metal | 2 | painted corrugated sheet metal, vertical ribs, light grey, scratches, rust spots |
| 18 | rust | 2 | corroded steel, brown / orange rust patina, no paint |
| 19 | shingles | 1.6 | dark grey asphalt roof shingles in horizontal rows, front-on |

The exact prompts and the style line are in `tools/vfxgen/texgen_recipes.json`.

## Shading (`shaders/surfaces.glsl`, legacy lit pass only)

`lit.frag` defines `LIT_SURFACES`; the vgeo resolve and the skinned pass do not include it.

- **Terrain** (`surf_terrain`):
  - 4 material-map samples around the pixel, jittered by value noise (no grid-aligned borders);
  - weights sharpened by each layer's height (stones poke through dirt);
  - colour = the layer's base colour (`SURF_BASE`, the palette of `mapgen_material_color`) × the texture's
    ratio to its own mean (`SURF_CHROMA`, `SURF_CONTRAST` per layer) × the vertex alpha variation. Distant
    mipped terrain therefore keeps the old palette exactly;
  - planar from above; triplanar where the slope is steep (|n.y| < 0.8) within 160 m;
  - anti-tiling: a second tap at 0.47 × scale, rotated, takes over in noise patches (fades out 70-110 m);
  - normal: whiteout blend per projection; roughness from the layer.
- **Layered prims** (`surf_layered`): uv in metres (`face_uv_m`: u along the face, v up it, so courses and
  boards continue across wall pieces); colour = vertex colour × texture / mean; normal through a
  cotangent frame (no tangent attribute); roughness from the layer, metallic from the material unless
  the layer is a metal layer (below).
- **Ambient occlusion**: every tap carries the baked AO (albedo alpha), blended like the roughness, at a
  per-layer strength `SURF_AO` (1 + s (ao − 1); 1.2 brick / shingles, 1.0-1.1 rock, gravel, wood,
  metal, 0.5-0.8 soft ground). `lit_body.glsl` multiplies it into `ao` with the SSAO and the glTF AO,
  so it darkens the indirect diffuse and the specular occlusion only (direct sun / lights untouched,
  as for the glTF material AO, docs/PBR.md).
- **Metal** (`surf_layered`, layers `metal` = `metal_thin` corrugated sheet, `rust` = `metal_thick`):
  metallic = `SURF_METALNESS` (0.9 / 0.85) × mix(bare, wear, paint) × (0 on near-black prims), where
  `paint` = smoothstep(0.12, 0.3) of the vertex colour's saturation. Grey, unpainted prims (corrugated
  halls, warehouses, grey containers, tanks) are bare metal where the mask says so (corrugated sheet:
  everywhere but grime; rust: about 13 % thin-patina patches); painted prims (red / blue / green
  containers, yellow cranes, hulls) stay dielectric except where the paint is worn (scratches, chipped
  corrugation crests: 20 % of the sheet). Near-black prims (tyres, soot) never turn metal. Bare metal is
  30 % smoother. Without the mask files (alpha 0 in the array) the material's metallic stays.
- Every tap uses `textureGrad` (taps sit in per-pixel branches).

The G-buffer prepass keeps the geometric normal and the material roughness (SSR / AO inputs); the
surface AO and metal masks do not reach it.

Limits: there are no reflection probes, so bare metal indoors (the inside of a corrugated hall) reflects
the sky environment, attenuated only by the SSAO / baked AO specular occlusion and SSR where it hits.

Cost (1600 × 900, `tools/tests/tex_perf.cfg`, opaque pass, r_surfaces 1 vs 0): city street +0.07 ms,
suburbs +0.15 ms, forest +0.10 ms, high overview +0.09 ms. With the AO and metal (same cameras, RTX
3060 Ti): +0.02, +0.16, +0.07, +0.15 ms, i.e. within the run-to-run noise of the old numbers (the AO
rides in the albedo tap; only metal-layer pixels take one more nrh tap).

## Ambient occlusion and metal masks (`tools/vfxgen/surface_pbr.py`)

Derived offline from the existing layer images (no API calls, deterministic):

- **AO**: the height channel × the layer's `BUMP` (texgen.py; the relief the normal map was made with,
  in texels), wrap-around horizon AO: 16 directions, radii 1-32 px, per direction the highest horizon
  elevation (attenuated with distance), AO = 1 − mean sin(elevation), normalised so the median (open
  surface) is 1, then ^1.5. Means 0.91-0.97, 5th percentiles 0.67-0.88.
- **Metal masks**: `metal` (light grey corrugated sheet): bare = 1 − rust spots (saturated orange) −
  grime; wear = fine bright scratches + crests of the corrugation in noise patches + chips, × bare.
  `rust` (patina everywhere in the photo): bare where the patina is thinnest: the least saturated,
  least relief regions in noise patches; wear = bare.

`--sheet out.png` writes a contact sheet (albedo, AO, bare, wear) for checking.

## Regenerating

    py tools/vfxgen/texgen.py                    # missing images only (cached), then process all
    py tools/vfxgen/texgen.py --process-only     # never calls the API
    py tools/vfxgen/texgen.py --regen rock       # a new image for these recipes (paid)
    py tools/vfxgen/surface_pbr.py               # AO + metal masks from the processed layers (free; rerun after texgen)
    py tools/vfxgen/tree_pbr.py                  # tree bark normal + ORM maps (free, docs/VGEO.md "Tree models")
    py tools/vfxgen/foliage_atlas.py             # grass / wheat cards (procedural, free)

Rebuild afterwards (assets are copied into `build/bin`).

- **Generation:** `higgsfield-ai/soul/standard`, 1080p, 1:1, one image per recipe, about $0.19 each
  (`hf_client.py`, key in `.env`). Raw images are cached in `tools/vfxgen/cache/tex_<name>_<hash>/raw.png`
  (git-ignored). The hash covers the prompt, so editing a prompt regenerates that recipe.
- **Processing** (deterministic, free):
  1. Crop: stochastic recipes take a bottom-centred 60 % square (the photos have some perspective; that
     region changes scale least). Structured recipes (`"period": true`: brick, wood, metal, shingles) take
     an integer number of their pattern periods (autocorrelation).
  2. Flatten the low frequencies: per channel for stochastic recipes, so slow light / colour drifts leave
     no step for the seam.
  3. Tile. Stochastic: self-quilting along x then y. A minimum-error cut through a 1/8 overlap band
     leaves one irregular seam where both sides agree best. Structured: variance-preserving blend with a
     copy shifted by a whole number of periods.
  4. Height = band-passed luminance, normal from its gradient (strength `BUMP`), roughness = `ROUGH` ±
     cavity.
- Generated in this pass: 25 images for $4.70 (20 recipes + 5 re-prompts: the first rock was terrazzo
  slabs, concrete had slab joints, rust was orange paint, the dirt road was a perspective road shot,
  mud had a puddle).

## Foliage cards

`foliage_atlas.py` draws 512 px cards, 4× supersampled:

- **grass_blades.png:** 28 tapered, curving blades from the bottom edge. Dark base → green → lighter or dry
  tips, a midrib, value noise, back blades darker.
- **wheat.png:** 18 straw stalks with ears (alternating spikelets + awns) and a few low leaves.

Empty texels carry the nearest blade colour (no dark mip fringes). `foliage_card_file` loads them for
`test_tuft` and `crop_wheat` material 0 at their own size (the impostor bake uses them too). Without
the files, the procedural masks of `vgeo_gen_foliage_mask` stay. Tree kinds are untouched.

## Tests

Screenshots: `tools/tests/tex_zl.cfg` (zombieland: streets, ground, forest, farms, aerials),
`tex_veg.cfg` (vegetation map: meadow, tufts, wheat), `tex_dev.cfg` (devroom unchanged), `tex_fallback.cfg`
(r_vgeo 0). Perf: `tex_perf.cfg`. Run from `build/bin` with `--test --nosound --log <path> --script`.
AO / metal / bark maps: `pbr_surf.cfg` (port containers, cranes, warehouse, factory hall, city walls,
suburbs, forest at a low sun, r_vgeo 0), `pbr_bark.cfg` (vegetation trunk close-ups), `pbr_perf.cfg` (bark
map cost). For a before / after on one build, move the `*_ao.png` / `*_metal.png` files (from both
`assets/textures/surfaces` and `build/bin/assets/textures/surfaces`: `platform_data_path` falls back to
the source tree) or `build/bin/assets/treepbr` aside: the shading then is exactly the old one.
