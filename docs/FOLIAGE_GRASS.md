# Grass and ground cover

Grass on the open-world maps is a set of data-driven assets (`assets/foliage/grass/*.json`): clumps of
crossed, fanned, bent cards, each card showing one cell of the asset's atlas, placed per terrain material
at the densities of the asset's table and shaded by the ground-cover terms (`MAT_COVER`). The art target is
`docs/GROUND_REFERENCE.md` 7.1. Trees, bushes and the daisy species keep the tree pipeline
(docs/VGEO.md "Tree models"); the procedural `test_tuft` stays as GPU test / bench content.

| | |
|---|---|
| Assets | `assets/foliage/grass/{grass_broad,grass_tussock,grass_tall,grass_short,wheat}.json` |
| Atlases | `assets/foliage/tex/<atlas>_albedo.png` (sRGB, coverage alpha), `<atlas>_nta.png` (normal xy, thickness, AO): `tools/vfxgen/groundgen.py` (recipes `assets/foliage/gen`) |
| Pure C | `src/render/vgeo/grass_asset.c` (JSON, clump mesh, material, cover parameters, clumping noise) |
| Renderer | `vk_vgeo.c` (`foliage_build_grass`, `grass_update_tier`, `render_grass_fade`), `ground.c` (cover table, ground colours, macro), `vk_classic.c` (the nta map in the material set) |
| Shaders | `shaders/cover.glsl` (the lit passes), `terrain/terrain_ground.glsl`, `terrain/terrain_macro.glsl` |
| Placement | `src/world/openworld_foliage.c` (`grass_cover`) |
| Tests | `tests/test_grass.c` (ctest `grass`), `tools/tests/ground_tex_check.py` (atlases), `tools/ground_match.py blades` |

## Asset JSON

| Block | Keys | |
|---|---|---|
| | `atlas`, `grid` [cols, rows], `variants` | atlas files, cells, clump meshes per asset (`<asset>_v<k>` foliage types) |
| `clump` | `cards`, `segments`, `height_m` [lo, hi], `width_m`, `fan_deg`, `lean`, `bend_m`, `base_jitter_m`, `normal_dome`, `mirror`, `cells` | cards fanned around the root (alternating sides by `fan_deg`), quadratic tip bend; normals blended toward the clump dome; cells mirrored at random |
| `placement` | `density` {material: clumps / m2}, `clump_scale_m`, `clump_amount`, `bare`, `scale`, `slope_max` | full-density table per terrain material (MgMaterial names), clumping noise (bare patches below `bare`), instance scale, steepest ground |
| `shading` | `roughness`, `specular`, `sss`, `sss_wrap`, `alpha_cutoff`, `root_dark`, `root_extent`, `ground_match`, `ground_extent`, `var_hue_deg`, `var_sat`, `var_val`, `macro`, `ao_root` | below |
| `terrain` | `top_color`, `coverage` | the terrain's grass term beyond the fade |
| `tiers` | `off` .. `modern`: `full_m`, `end_m`, `shadow_m`, `density` | fades (material `fade_start` / `fade_end`), sun shadow distance (0: none), placement density x |
| `wind` | `main`, `detail`, `flutter` | x the grass preset's stiffnesses (docs/FOLIAGE_WIND.md) |

Mesh: one quad column per card, `segments` rows (2 x segments triangles), uvs inside the card's cell (v =
cell bottom at the root). Vertex colour: R height along the card (0 root .. 1 tip: the wind's flutter weight
and the shading's root -> tip parameter), G wind phase, B 0, A the card's hash. 3 cards x 3 segments = 18
triangles a clump (the tuft had 60).

## Placement

`grass_cover` (per streamed 32 m chunk): a jittered 0.35 m grid; a cell holds a clump with probability
sum_i density_i(material) x keep_i(x, z) x level density x cell area, the asset picked in proportion; slope,
water and keep-outs as before. `keep` is the clumping noise (two octaves at `clump_scale_m`, smooth step
above `bare`): clumps and bare ground as in the reference meadows instead of a uniform lawn. `test_grass`
checks the clumps per m2 against `grass_expected_density` (5 %) and the meadow total against the spec
(2..6 / m2). Bushes and daisies keep their 1.1 m grid; without grass assets the old tufts / wheat return.

Defaults at Modern: grass about 3.6 clumps / m2 (broad 3.0, tussock 0.8, short 2.0, tall 0.35 before
clumping), meadow about 3.3 (broad 1.5, tussock 2.0, tall 1.5, short 1.5), fields about 2.1 (wheat); the
spec's meadow range is 3-6 (`test_grass` prints them). At 12-30 triangles a clump this is about twice the
old tufts' triangles per m2 (0.7 x 60) at Modern; Medium and below cut it with the level density and the
shorter fades.

## Shading (`shaders/cover.glsl`, `MAT_COVER`)

`cover_base` on the textured albedo, then the lighting:

| Term | Parameters | |
|---|---|---|
| Per clump / card variation | `var_hue_deg`, `var_sat`, `var_val` | from the instance hash and the card hash |
| Macro variation | `macro` | the shared field (`terrain_macro.glsl`, terrain.json `macro`): green / straw patches over 50..500 m, the same the terrain uses |
| Ground-colour matching | `ground_match`, `ground_extent` | the lower blade toward the ground colour under the root (`terrain_ground_color`: material map + the layers' mean albedo + macro) |
| Root darkening | `root_dark`, `root_extent` | albedo x root_dark at the root, 1 above root_extent |
| Clump AO | `ao_root`, the nta map's AO | indirect light only (as the material AO) |
| Translucency | `sss`, `sss_wrap`, the nta map's thickness | wrapped back lighting + forward scattering toward the viewer, albedo-tinted; `r_grass_sss` |
| Specular | `specular`, `roughness` | x the BRDF's specular (`g_spec_scale`) |
| Normals | | both sides lit from above (the back-face flip keeps the dome up) |

Everything is gated on `MAT_COVER` (with a 3-bit cover id: up to 8 assets); `g_spec_scale` stays 1.0 for
every other material, so trees and props render exactly as before. Debug: `r_grass_debug` 1 root -> tip,
2 matched ground colour, 3 variation, 4 thickness, 5 macro field.

## Levels

The materials' fades and shadow distance follow `r_fidelity` (`grass_update_tier`; a commit on change), the
placement density the level's `density` (chunks generated after a change); `r_foliage_density` thins on the
GPU on top. Atlases load at half size (`r_grass_tex 1`: full, 4x the memory). Defaults (broad / tussock):

| Level | Full density to | Thinned out at | Shadows to | Density |
|---|---|---|---|---|
| Off | 8 m | 18 m | none | 0.35 |
| Low | 12 m | 30 m | none | 0.5 |
| Medium | 20 m | 45 m | 10 m | 0.75 |
| High | 25 m | 60 m | 20 m | 0.9 |
| Modern | 30 m | 70 m | 20 m | 1.0 |

Beyond, the terrain's grass term takes over (docs/TERRAIN_SHADING.md).

## Secondary plants

The same format places the secondary plants: `shrub_leafy` (pinnate-leaflet sprigs, 0.3-0.6 m, sparse on
meadows / grass / forest edges: 0.06-0.15 clumps / m2 before clumping, the reference's greener, bluer
shrubs), `fern` (forest floor, 0.5 / m2), `reed` (mud, 1.4 / m2). Their atlases are in `assets/foliage/gen`
(`shrub_leafy`, `fern`, `reed`); they take more translucency and specular than the grass, little ground
matching, and fade 10-20 m later. The eight assets use all eight cover ids. Flowers are cells of
`grass_short` (pink / purple / yellow heads in 2 of its 4 cells). The imported bush and daisy species stay
on the tree pipeline (their own wind, impostors).
