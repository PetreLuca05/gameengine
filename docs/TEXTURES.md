# Surface textures

Terrain, roads, open-world buildings and the grass / wheat cards are textured. Code:
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
`map noise` / `map flat` test terrains, the r_vgeo 0 fallback's foliage (it skips masked meshes).

## Layers (`enum SurfTexLayer`, render.h)

20 layers, two `sampler2DArray`s in frame set bindings 12 / 13, the terrain material map at 14:

- `<name>_albedo.jpg`: 1024 px sRGB albedo (JPEG q92).
- `<name>_nrh.png`: 512 px, R G tangent normal (0.5 flat, G = image up), B roughness, A height.

Mips are box-filtered at load (about 0.7 s for all 20 layers, on the first open-world map). The arrays
use the material sampler (`r_aniso`, `r_texture_quality`). Memory: about 140 MB (albedo 20 × 5.6 MB,
nrh 20 × 1.4 MB). `r_surfaces 0` turns the shading off (vertex colours, for comparison).

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
  cotangent frame (no tangent attribute); roughness from the layer, metallic from the material.
- Every tap uses `textureGrad` (taps sit in per-pixel branches).

The G-buffer prepass keeps the geometric normal and the material roughness (SSR / AO inputs).

Cost (1600 × 900, `tools/tests/tex_perf.cfg`, opaque pass, r_surfaces 1 vs 0): city street +0.07 ms,
suburbs +0.15 ms, forest +0.10 ms, high overview +0.09 ms.

## Regenerating

    py tools/vfxgen/texgen.py                    # missing images only (cached), then process all
    py tools/vfxgen/texgen.py --process-only     # never calls the API
    py tools/vfxgen/texgen.py --regen rock       # a new image for these recipes (paid)
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
