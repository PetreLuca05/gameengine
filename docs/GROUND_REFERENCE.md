# Ground reference: grass blades, terrain texturing, transitions

The art target for the ground (everything but trees): grass blades and their textures first, then terrain
layers, their variation and their transitions, then ground cover / shrubs / flowers. Five reference
screenshots of another game are the target **look**; nothing from them is used as an asset. Our textures
come from our own Higgsfield recipes (`tools/vfxgen`), our geometry from our procgen.

| | |
|---|---|
| References | `docs/reference/ground/ref{1..5}_*.{webp,png}` (reference only, never loaded by the game) |
| Regions / blade crops | `docs/reference/ground/regions.json` |
| Sampler | `py tools/ground_ref.py [--print]` → `stats.json`, `blade_sheet.png` |
| Status | analysis + target spec (this file). Implementation plan at the end; nothing generated yet |

| Ref | Image | Light | What it shows best |
|---|---|---|---|
| r1 | 1920×1080 promo still, crater in a meadow | low sun from the right, clear | blade shapes, root→tip gradient, dirt, crater rim transition, shrubs |
| r2 | 1280×720 gameplay (letterboxed) | overcast | wheat, field soil between stalks |
| r3 | 739×415 gameplay, prone | overcast | tall grass with seed plumes, pale ground patches, density |
| r4 | 739×415 gameplay | overcast | dry hillside, dirt patch with noisy border, macro variation |
| r5 | 620×300 gameplay | sun, clear | meadow tufts, flowers, worn path, mossy ground |

Colours below are sRGB means of masked pixels (`tools/ground_ref.py`: *green* = h 55-160°, s > 0.25;
*dry* = h 25-70°, 0.10 < s < 0.60, v > 0.42; *dark* = v < 0.45). They include the reference's lighting
and grade, so they are **targets for the lit, graded frame at a matching time of day**, not raw albedo. The
albedo targets in the spec divide out an estimated light level (sunlit ≈ 0.9-1.0 of the albedo value
after our exposure, overcast ≈ 0.7); the comparison view checks the lit result, not the albedo.

## 1. Blade texture authoring (most important)

Reference sheet: `docs/reference/ground/blade_sheet.png` (crops scaled to 256 px high, the colour mask under
each). The colour mask is noisy wherever the ground shares the blade hue (b5, b6, b9); the silhouette test
uses **hand-traced masks** (`docs/reference/ground/masks/<crop>.png`, drawn in the blade-iteration step),
not this mask.

| Crop | Type | Root | Mid | Tip | Coverage |
|---|---|---|---|---|---|
| b1 r1 fg clump | broad green | `#4f542e` h66 s.47 v.33 | `#555b30` h66 s.49 v.36 | `#676a3d` h63 s.46 v.42 | 0.53 |
| b2 r1 fg arch | broad green | `#5c6134` h66 s.48 v.39 | `#656c38` h67 s.51 v.43 | `#767946` h62 s.45 v.48 | 0.68 |
| b3 r1 tussock | ribbon (dry-variegated) | `#978d62` h48 s.36 v.59 | `#9d9164` h48 s.37 v.62 | `#a39369` h44 s.36 v.64 | 0.74 |
| b4 r1 tussock 2 | ribbon (green base) | `#8f8464` h44 s.30 v.56 | `#8d8760` h53 s.34 v.56 | `#8b885e` h58 s.35 v.55 | 0.61 |
| b5 r1 dark fountain | dark fountain | `#5f613f` h64 s.38 v.38 | `#7b7854` h57 s.35 v.49 | `#8e835d` h48 s.36 v.56 | 0.66 |
| b6 r4 straw | fine dry straw (overcast) | `#816f4d` h40 s.41 v.51 | `#726543` h45 s.42 v.45 | `#595430` h53 s.50 v.35 | 0.21 |
| b7 r3 plumes | tall stems + seed plume (overcast) | `#4e4d2d` h60 s.44 v.31 | `#4e4e26` h60 s.54 v.31 | `#65623d` h56 s.43 v.40 | 0.63 |
| b8 r2 wheat | wheat | `#8e7453` h34 s.42 v.56 | `#8d7250` h34 s.43 v.55 | `#907753` h35 s.43 v.57 | 0.48 |
| b9 r5 meadow | short meadow tuft (sun) | `#585432` h56 s.44 v.35 | `#7d6b49` h40 s.42 v.49 | `#84734f` h41 s.41 v.52 | 0.34 |

Per blade type (read off the crops at full size; widths are at the base, ratios are width : length):

| | Broad green (b1, b2) | Ribbon tussock (b3, b4, b5) | Fine straw (b6) | Tall stem + plume (b7) | Meadow tuft (b9) |
|---|---|---|---|---|---|
| Silhouette | long flat leaf, straight lower 60 %, **arches over** in the top third, some tips droop below horizontal | fountain: blades leave the root fan-wise (±50°) and arch outward; a few kinked / folded halfway | near-straight, slight kink; thread-thin | straight thin stem, small leaves low, feathery plume 8-15 % of the length | short dense fan, mixed straight / curved |
| Tip | long acute taper (last 25 %), no split | acute; dry tips curl | needle | plume: soft, ragged alpha | acute |
| Width : length | 1:30 - 1:45 | 1:35 - 1:50 | 1:80 - 1:120 | stem 1:150, plume 1:6 | 1:25 - 1:35 |
| Shapes per card | 4-6 distinct curves | 6-8 | 8-12 | 3-4 stems | 6-8 |
| Blades per card / overlap | 6-9, crossing in the lower half, 2-3 deep | 12-20 per clump, dense root fan, 3-4 deep at the base | 10-16, sparse crossing | 4-6 stems + 4-6 leaves | 10-14 |
| Gradient | dark olive root (v .33) → mid green → **lighter, yellower tip** (v +.09, h -4°) | olive-green root → cream / straw upper blade; the green fraction falls from root to tip | straw root lighter than the shaded tip in overcast (the tip is against the sky) | uniform olive, plume near white `#c8c4b0` | olive root → tan tips |
| Midrib | visible as a lighter line on lit broad blades (≈ +0.06 v), one edge darker (a fold) | longitudinal stripes (cream edges, green centre) on ≈ 40 % | none | none | faint |
| Edge | hard alpha, 1 px AA, no halo | hard | hard (sub-pixel at range: needs alpha-to-coverage / dither) | plume soft | hard |
| Speck / noise | low: soft value noise ±4 %, a few dark spots near the root | dry blades: fine brown streaks | low | plume grain | low |
| Dry ratio | ≈ 10 % of blades straw-tipped | 50-80 % cream / straw | 100 % | stems 30 % straw | 20-40 % tan tips |
| Card reuse | mirroring and rotation evident (neighbouring clumps repeat shapes); not objectionable | same | same | same | same |
| Apparent texel density | ≈ 6-10 px across a broad blade at 1-2 m in 1080p → ≈ 1 texel / mm needed near: **2K atlas, a 512 px cell per 0.5 m card** | same | thin: 2-3 px; must stay ≥ 1.5 px into the 3rd mip | | |

What the reference is *not*: no saturated grass green anywhere (the greenest blade is h 66°, s 0.5: an
olive / yellow-green), no uniform lawn, no blue-green.

## 2. Grass geometry

| | Measured / estimated (scale from the crater ≈ 2.2 m wide, the prone player, the standing character in r5) |
|---|---|
| Representation | camera-facing-ish crossed **cards per clump**, clumps of 2-4 cards; near blades show no visible card plane except where a card is edge-on |
| Blades per clump | broad 8-14, tussock 15-30, straw 20-40 (as 3-4 cards) |
| Height | broad 0.25-0.45 m; tussock 0.3-0.5 m; straw / plume grass 0.45-0.8 m (r3: the prone player is half hidden); meadow tuft 0.15-0.35 m; wheat 0.8-1.0 m |
| Width | broad 7-11 mm, tussock 5-8 mm, straw 2-4 mm |
| Bend / taper | broad: tip droop 20-40 % of height; taper starts at 60-75 % of length |
| Split tips | none seen |
| Clump spacing | meadow (r1 right, r5): 0.35-0.7 m between clump centres with bare ground between (clumps, not lawn); tall grass (r3, r4 hillside): continuous, 0.2-0.35 m |
| Density | **clumps per m²**: meadow 3-6, tall grass 8-14, sparse-on-dirt 0.5-1.5. Bare-ground fraction at eye level: r1 ≈ 45 %, r5 ≈ 40 %, r3 ≈ 15 % |
| Full density range | ≈ 25-35 m (r1, r3), thinning to ≈ 60-80 m; beyond, ground reads as terrain with a yellow-olive grass colour (r1 far field `#a6a565` h58 s.43 v.67: much lighter than the near ground, the visible tips dominate) |
| Distant grass → terrain | the terrain under the fade takes the **grass-top colour**, not the soil colour, so the fade line does not show a dark ring |

Ours today (`gen_tuft`): 5 crossed cards (60 tris), 0.5-0.65 m tall, 0.45-0.55 m wide, about 0.62 tufts / m²
on grass, fade 40-65 m, shadows to 15 m; 28 straight blades on a saturated green atlas (h 80-85°, s 0.5).

## 3. Grass colour and lighting

| | Reference |
|---|---|
| HSV (lit, sun) | broad blades h 62-67°, s 0.45-0.51, v 0.33-0.48; field far h 58 s .43 v .67 |
| HSV (overcast) | h 56-60°, s 0.44-0.54, v 0.30-0.40 (r3), mixed hill h 50 (r4) |
| Dry | h 44-48°, s 0.30-0.37, v 0.55-0.64 (cream / straw); wheat h 34-35°, s 0.42 |
| Root darkening | v × 0.75-0.8 over the bottom 20-30 % of the blade (plus clump AO) |
| Tip lighting | tips +0.06 .. +0.10 v and 3-5° yellower |
| Translucency | backlit blades in r1 glow yellow-green (h ≈ 70°, v up to 0.7) where the sun is behind them: wrap + transmission term on the blade, ≈ 0.35-0.5 of the direct light, tinted by the albedo squared |
| Specular | soft broad sheen on flat blades (r1 b2): low, roughness ≈ 0.55-0.65 on broad blades, ≈ 0.8 on dry |
| Contact AO | strong darkening at clump bases (the ground under a clump ≈ v 0.13-0.2 against 0.32 open) |
| Shadows | grass casts clearly visible shadows on the ground near the camera (r1: tussock shadows on the dirt). Distance ≥ 20 m on High |
| Ground colour matching | lower blades take on the ground hue (olive-brown) in r1; at 30 m+ the field colour is continuous between grass and ground |
| Macro variation | large patches (5-30 m) of greener vs straw-yellow grass (r1 mid field, r4 hillside): hue ±8°, v ±0.08 |
| Dry mix | per biome: r1 meadow ≈ 45 % dry blades, r4 ≈ 60 %, r3 ≈ 15 % |

## 4. Terrain textures

| Ground type | Seen in | Colour (lit) | Detail / scale | Notes |
|---|---|---|---|---|
| Grass ground (litter) | r1 fg, r5 | `#4d4926` h54 s.57 v.31 (r1, between blades) | small leaves, twigs, moss specks 1-3 cm; reddish dead leaves; **brown-olive, not green** | the main layer; mostly under grass so its colour drives the fade seam |
| Moss / dry moss ground | r4, r5 | `#75692f` h50 s.62 v.46; sunny `#776442` h39 s.46 v.47 | yellow-green moss mats 10-40 cm with lichen | on open spots between tussocks |
| Dirt (crater wall) | r1 | `#514a37` h40 s.40 v.32 (shaded, high v std 0.19) | lumpy clods, small roots, pale stones (white flecks 1-3 cm) | strong height detail: parallax target |
| Dry dirt (rim, worn) | r1, r4 | `#998b65` h42 s.38 v.60 (lit rim); r4 patch `#70644b` h41 s.36 v.44 | crusted, lighter, fine cracks | the transition material at grass→dirt |
| Pale soil / chalk | r3 | `#5e5c49` h53 s.28 v.37 (overcast) | low saturation, mottled white | bare patches between tall grass |
| Field soil | r2 | `#483623` h31 s.55 v.28 | dark brown, straw litter lines | under wheat |
| Path (worn) | r5 | `#615942` h46 s.35 v.38 | packed, small pebbles, sparse grass bits at the edge | |
| Rock (boulders) | r1 bg | grey, low saturation | out of reach of these shots; keep current rock layer, retint | |
| Mud / sand / gravel | — | not visible | keep current layers, retint into the same family | |

Texel density: near dirt shows detail at ≈ 2-4 mm (r1 crater wall at ≈ 3 m): **2K per 2-3 m tile** near,
far tiles 1K. Tiling: no repetition visible in any shot; large variation at three scales: 0.5-2 m (moss vs
litter vs dirt patches), 5-30 m (green / dry patches), 100 m+ (r1 background: valley floor lighter).
Slope: steep crater walls are bare dirt; hillsides (r4) keep grass. Moisture: none visible (dry summer).

### Transitions observed

| Pair | Where | Band | Character |
|---|---|---|---|
| grass ground → dirt (crater) | r1 | rim band 0.15-0.3 m of lighter dry dirt, grass thinning over ≈ 0.5 m | sharp lip (height break), the lighter rim follows the edge, a few grass clumps inside the dirt |
| grass → dirt patch | r4 | 0.3-0.8 m | **noise-broken**: lobed, crumbly outline, islands of moss / grass inside, pebbles and grey lichen at the border |
| grass → path | r5 | 0.5-1 m | worn edge: sparse short grass and moss bits on the path, path packed in the centre, pebbles scattered |
| tall grass → pale ground | r3 | 0.5-2 m | grass density fades by clumps (no sharp line), pale ground shows between |
| wheat → field soil | r2 | n/a | soil visible between stalks; straw litter on the soil |

Never a soft linear crossfade in any shot: every border is either a height break (crater), a noisy lobed
mask with crumbs (dirt patches) or a density fade of clumps (grass on pale ground).

## 5. Motion, rendering cues, what to exclude

- Motion: stills only. Keep our wind model (FOLIAGE_WIND.md); target a gentle main sway (r3 / r4 are
  calm). No change to amplitudes beyond per-asset tuning.
- Fades / LOD: r4 / r3 (older builds) show grass pop at ≈ 50-60 m and a terrain colour beyond; r1 shows
  grass to ≈ 80 m thinning smoothly. Target: full density to 30 m, dithered thinning to 70 m (Modern), the
  terrain grass-top term beyond.
- Shadows: grass shadows near (≤ 20 m), contact darkening always.
- Interaction bending: not visible; keep ours (displacement buffer).
- **Exclude from the spec**: r1's strong local contrast / sharpening and warm grade, r3 / r4's flat grey
  overcast grade and compression, the HUDs, the r2 lightning VFX, the letterbox. The comparison view
  compares at matched time of day and weather, and reports hue / saturation deltas (robust to exposure)
  ahead of value deltas.

## 6. Secondary plants (brief)

| | Reference | Target |
|---|---|---|
| Leafy shrub (r1 right) | pinnate leaflets 2-4 cm, 0.3-0.6 m tall, `#60713e` h80 s.48 v.44: the only clearly *green* thing, bluer than the grass; translucent leaves | new card shrub kind: 0.3-0.7 m, 8-14 leafy-sprig cards, density 0.05-0.2 / m² on meadow |
| Low bushes (r1 bg, r5) | round 0.5-1 m clumps, dark green | the existing procedural bush retuned (size, colour), not the imported tree-like bushes |
| Flowers (r5) | small pink / purple / yellow heads 1-2 cm on thin stems, 2-5 % of clumps, in groups | flower atlas cells mixed into meadow tufts (not a separate tree-species mesh) |
| Ferns / reeds | not visible | ferns: low-priority card kind for forest floors; reeds: tall straw variant near water |
| Wheat (r2) | ears 8-10 cm, straw `#8e7453` h34 s.42 (warmer than ours, h46), 0.8-1 m | retint + reshape `wheat.png`; keep the crop geometry, height 0.8-1.0 m |

Out of scope: trees, Tree It assets, tree shader parameters, tree impostors, the imported `bush_*` /
`flower_daisy` species (they share `WPO_TREE` / the tree pipeline; left as they are).

## 7. Target spec

### 7.1 Grass assets (`assets/foliage/grass/*.json`)

| Asset | Atlas cells | Cards / clump | Blades / card | Height (m) | Base width | Density (clumps / m²) | Mix |
|---|---|---|---|---|---|---|---|
| `grass_broad` | 4 (2 green, 1 green w/ dry tips, 1 mixed) | 3 | 6-9 | 0.25-0.45 | 7-11 mm | meadow 1.5, grass 2.5 | 10 % dry |
| `grass_tussock` | 4 (2 dry-variegated, 1 green fountain, 1 dark fountain) | 4 (fan) | 12-20 | 0.30-0.50 | 5-8 mm | meadow 2.0, grass 1.0 | 60 % dry |
| `grass_tall` | 4 (2 straw, 1 green stems, 1 stems + plumes) | 3 | 10-16 | 0.45-0.80 | 2-4 mm | tall meadow 6-10 | 30 % dry, 12 % plumes |
| `grass_short` (meadow tuft + flowers) | 4 (2 tuft, 1 tuft + pink / purple, 1 tuft + yellow) | 2 | 10-14 | 0.15-0.35 | 3-5 mm | meadow 2, path edges 1 | flowers 4 % |
| `wheat` | existing cell, regenerated | 6 (as now) | — | 0.8-1.0 | — | as now | — |

| Parameter (all JSON on the asset) | Value |
|---|---|
| Root → tip albedo (sun target, sRGB) | broad: root `#4a5030` → mid `#5c6435` → tip `#6f7340`; dry: root `#7d7650` → tip `#a8986c`; straw `#8a7a55` → `#9d8f66` |
| Root darkening | × 0.78 over the bottom 25 % (vertex term, on top of the atlas) |
| Per-clump colour variation | hue ±4°, sat ±8 %, value ±7 % (hash) |
| Macro variation | world fBm at 18 m and 60 m: hue ±8° toward straw, v ±8 % (shared with the terrain macro so the fade matches) |
| Ground-colour matching | lower 35 % of the blade mixed toward the terrain colour under the root: 0.35 (broad), 0.25 (tussock) |
| Translucency | `sss`: 0.45 (broad), 0.30 (dry), wrap 0.5, tint = albedo² × 1.2 |
| Specular | roughness 0.6 broad / 0.8 dry, specular 0.35 of F0 |
| Clump AO | 0.55 at the root → 1 at 40 % height, + a ground AO disc (terrain term, radius ≈ 0.8 × clump width) |
| Normals | blended 0.5 toward a clump-dome normal (soft, no card facets) |
| Fades (Modern) | full to 30 m, thinning to 70 m (tall 85 m); High 25 / 60; Medium 20 / 45; Low 12 / 30; Off 8 / 18 |
| Shadows | Modern / High 20 m, Medium 10 m, Low / Off none (the clump AO stays) |
| Wind | grass preset; tall grass main amplitude × 1.3, detail × 1.2; no change to the model |
| Beyond the fade | terrain grass term: the terrain takes the grass-top colour (density-weighted) and a darker AO speckle, crossfaded across the last 30 % of the fade |

### 7.2 Terrain layers (`assets/terrain/layers/*.json`)

Albedo targets (linear-safe sRGB, normalised by the generator; lit values above ÷ the light estimate):

| Layer | Replaces | Albedo mean (sRGB) | Tile (m) | Macro scale (m) | Height contrast | Variants |
|---|---|---|---|---|---|---|
| `ground_litter` | grass (0) | `#524c2c` | 2.5 | 40 | 0.4 | `ground_litter_dry` (moisture low / macro), `ground_moss` (curvature concave) |
| `meadow_ground` | meadow (1) | `#6b6236` | 3 | 40 | 0.4 | `ground_moss_dry` |
| `forest_floor` | forest (2) | keep, retint `#4a3d2c` | 3 | 30 | 0.5 | `forest_needles` |
| `dirt` | dirt (3) | `#5e5540` | 2.5 | 30 | 0.8 | `dirt_dry` (ridges / macro, `#9a8c66`) |
| `rock` | rock (4) | keep, desaturate to h40 s.12 | 6 | 60 | 1.0 | `rock_mossy` (moisture / up-facing) |
| `sand` | sand (5) | keep, retint `#a59a7a` | 4 | 50 | 0.3 | — |
| `gravel` | gravel (6) | keep | 2 | 30 | 0.9 | — |
| `dirt_road` / path | dirt_road (8) | `#6a614a` | 3 | 40 | 0.6 | `path_worn_edge` |
| `field_soil` | field (10), plowed (12) | `#4d3a26` | 3 | 40 | 0.6 | `field_straw` |
| `mud` | mud (13) | keep, retint `#3d3326` | 3 | 30 | 0.3 | — |
| `pale_soil` | new id (14) | `#77735c` | 3 | 40 | 0.5 | — |

Asphalt / concrete / crop and the building layers keep their textures; they get the same anti-tiling path.

### 7.3 Transitions (`assets/terrain/transitions/*.json`, family defaults + per-pair overrides)

| Pair | Mode | Width (m) | Edge noise (scale m / strength) | Transition set | Border scatter |
|---|---|---|---|---|---|
| grass family → dirt family | height + noise + crumbs | 0.6 | 1.5 / 0.6, crumbs 0.15 m | `grass_sparse_on_dirt` | dead grass bits, pebbles; lighter `dirt_dry` rim 0.2 m |
| grass → path | wear (packed centre → worn edge) + noise | 0.8 | 2 / 0.4 | `path_worn_edge` | pebbles, short grass bits |
| dirt → rock | height (rock wins), slope-aware | 0.4 | 1 / 0.5 | `pebble_band` | pebbles, rock chips |
| grass → rock | height + slope + crumbs | 0.5 | 1.2 / 0.6 | `rock_mossy` edge | moss specks |
| dirt → mud | moisture-driven, flow-aware | 1.0 | 2.5 / 0.5 | `mud_wet_edge` | — (puddles first here) |
| sand → rock | directional (downwind drift) | 0.8 | 2 / 0.4 | `sand_on_rock` | — |
| grass → field soil | straight-ish (farm), short noise | 0.3 | 0.5 / 0.3 | — | straw bits |
| any → snow (weather) | altitude band with temperature noise | 3 | 8 / 0.7 | — | — |

### 7.4 Texture asset list (grass first)

| # | Asset | Size | Maps | Generated | Images (est.) |
|---|---|---|---|---|---|
| 1 | `grass_broad` atlas | 2048 | albedo+alpha, normal, thickness (SSS), AO | Higgsfield blades on a plain backdrop, matted + assembled into cells | 4 + 2 retries |
| 2 | `grass_tussock` atlas | 2048 | same | same | 4 + 2 |
| 3 | `grass_tall` atlas (straw, stems, plumes) | 2048 | same | same | 4 + 2 |
| 4 | `grass_short` atlas (+ flowers) | 2048 | same | same | 4 + 1 |
| 5 | `wheat` | 1024 | same | same | 2 |
| 6 | shrub / fern / reed cards | 1024 each | same | same | 3 + 1 |
| 7 | terrain layers (11 above) | 2048 near / 1024 far | albedo, normal+rough+height, AO | tileable photo recipes (texgen pipeline) | 11 + 3 |
| 8 | macro maps per layer | 512 (tileable, 64-128 m) | colour + normal | procedural from the layer + noise (free) | 0 |
| 9 | transition sets (6) | 1024 | albedo, nrh | Higgsfield | 6 + 2 |
| 10 | scatter sprites (pebbles, leaves, twigs, grass bits, cracks) | 1024 atlas | albedo+alpha, nrh | Higgsfield on plain backdrop, matted | 5 + 1 |

Total ≈ 57 images at ≈ $0.19 = **≈ $11** (`higgsfield-ai/soul/standard`, 1080p). A first validation round
is 4 images (≈ $0.80): one broad-blade sheet, one tussock sheet, `ground_litter`, `dirt`.

## 8. Engine state that shapes the plan

- Grass is hardcoded C (`gen_tuft`, 5 cards / 60 tris), no JSON; no per-instance colour, no ground matching,
  no translucency; the lit pass cannot tell foliage from other surfaces (the G-buffer flags carry only
  viewmodel / dynamic / unlit). Grass vs trees is separable by `wpo & 0xFF == WPO_GRASS` (tuft, crop and the
  daisy species) vs `WPO_TREE`, so grass-only shading is gated on `WPO_GRASS` plus a new `VGM_GROUND_COVER`
  material bit (the daisy keeps its current path).
- Foliage cost today ≈ 3.2 ms in game against a 2.5 ms budget (VGEO.md, before the tree models). More
  clumps must come with **fewer triangles per clump** (3 cards / 36 tris instead of 60) and tighter fades on
  Medium and below.
- Terrain: one material id per 1 m sample (`MgRaster.mat`), blended in `surf_terrain` from the 4 nearest
  samples with a height bias; per-layer constants hardcoded in GLSL; one rotated anti-tile tap; no macro,
  curvature, moisture, wear or distance-to-border data; not hot reloadable. The frame descriptor set is full
  (16 bindings). Terrain is a legacy draw shaded by `lit.frag` in both renderers (one implementation).
- No fidelity knob for terrain; `r_surfaces` is a plain cvar. Grass has density / wind / impostor knobs only.
- Bug found on the way: `vgeo_gen_foliage_mask` tests `VG_FOLIAGE_CROP` twice (`vgeo_procgen_foliage.c:299`),
  so the procedural wheat fallback is empty (hidden by `wheat.png`).

## 9. Shader structure (`shaders/terrain/`)

`surf_terrain` in `surfaces.glsl` becomes a call into `shaders/terrain/terrain.glsl`; the layered (building
/ road) path stays, gaining only the tiling breaker.

| File | Content |
|---|---|
| `terrain_params.glsl` | the per-layer and per-pair parameter SSBO (from JSON, replaces `SURF_BASE` / `SURF_TILE` / … arrays), tier bits |
| `terrain_noise.glsl` | hash, value / gradient noise, fBm (shared with grass for the macro term) |
| `terrain_tile.glsl` | `tile_sample()`: cheap mode (per-cell rotation / mirror), hex bombing (3-cell, histogram-preserving blend with the layer's mean / variance), multi-scale (1, 0.23, 0.07), distance UV scaling with crossfade; macro / detail split |
| `terrain_weights.glsl` | material-map gather (4 → 9 samples on High+), top-N, signed-distance band (`aux.a`) → `transition_width` in metres, edge noise + crumbs, height blend (`height_contrast`, `blend_sharpness`, `height_offset`), slope / directional / flow / wear modes |
| `terrain_color.glsl` | macro colour map, global fBm HSV shifts, per-layer HSV jitter, altitude / slope / aspect tint, curvature (cavity darker / ridge lighter), moisture tint, sub-layer variants by mask |
| `terrain_detail.glsl` | parallax occlusion (≤ 8 m, 8-16 steps) fading to normals, detail normals, micro-roughness |
| `terrain_scatter.glsl` | per-cell scatter (pebbles, leaves, twigs, cracks) from the scatter atlas, border scatter |
| `terrain_grass.glsl` | grass-top term beyond the grass fade (density from the same placement rule) |
| `terrain.glsl` | `terrain_shade()`: ordered weights → taps → colour → detail → scatter → grass term → returns the `SurfOut` the weather pass consumes (wetness / puddles / snow stay in `weather.glsl`, gaining porosity per layer, low-border puddles and the snow band) |

New data, baked on the CPU at map load by a pure module `src/world/terrain/terrain_maps.c` (unit-tested
against a CPU reference) and uploaded as one RGBA8 1 m map (+ an RG8 at 1 m): R curvature, G moisture
(flow accumulation + low ground), B wear / paint mask, A signed distance to the nearest different material
(metres, ±4 m, so band widths are exact in metres at every LOD), RG2 macro variant / paint. Bindings: the
frame set grows from 16 to 19 (aux map, macro array, scatter atlas). Layer arrays go 2K / 1K with mips.

## 10. Per-tier cost (estimates, 1080p, terrain full-screen, RTX 3060 Ti class; to be measured)

Taps are per array (albedo + nrh) per pixel at the worst case.

| Tier | Techniques | Taps (worst) | Est. ms |
|---|---|---|---|
| Off / Low | 4-sample weights, height blend, simple-noise borders, per-cell rotation / mirror, macro colour (1 tap), top-2 | 2 × 1 + macro = 5 | 0.35-0.45 |
| Medium | + hex bombing (3), multi-scale (+1 far tap), slope / altitude tint, crumbs, signed-distance width | 2 × 3 + 1 + macro = 9 | 0.65-0.8 (budget 0.8) |
| High | + 9-sample weights, top-3, curvature / moisture tint, variants, parallax ≤ 8 m, scatter, transition sets | 3 × 3 + set 2 + scatter 1 + macro + POM (≈ 8 near) | 1.2-1.5 |
| Modern | + detail normals, directional / slope / curvature-aware bands, tessellation (optional, near chunks only), triplanar on slopes | High + 2 + triplanar on steep only | 1.6-2.0 (budget 2.0) |

Today: 4 layers × (1 + 2 triplanar) × 2 anti = up to 24 taps per array, ≈ 0.1-0.15 ms measured (mostly
1-2 layers per pixel). Tessellation is the risky item: terrain is a legacy indexed draw, so it means a
tessellation pipeline variant for near chunks; it is proposed as **optional** and will be dropped if it
cannot fit 2.0 ms. Grass: more clumps at fewer triangles each; target ≤ the current foliage cost on every
tier (the budget is already exceeded; Medium and below get tighter fades).

## 11. Implementation plan

Order as asked: analysis + spec (this) → placeholder side-by-side → grass blades → terrain variation →
transitions → secondary plants. Every step commits on its own.

1. **Verification setup.** This cloud container has no GPU, no Windows toolchain, no `.env`. Ubuntu offers
   `glslang-tools` and Mesa lavapipe (software Vulkan): build the game on Linux and render headless
   screenshots (correctness, validation layers, before / after, comparisons). GPU timings (the ms budgets)
   need a run on the Windows machine: `tools/tests/ground_perf.cfg` will be ready for it.
2. **Placeholder side-by-side.** `ground_ref` yard in the devroom (terrain strip on a small open-world
   raster: every layer, every pair, with toggles) + the reference viewer (side-by-side / overlay at matched
   camera / FOV / time of day, region colour sampler), current look captured as the "before".
3. **Grass.** JSON grass assets + the generator (cards, taper, arch, fan), per-instance variation, ground
   matching, root darkening, SSS / spec via a G-buffer foliage bit gated to `VGM_GROUND_COVER`, clump AO,
   fades / shadow rules per tier, the terrain grass term. Atlases: `foliage_atlas.py` procedural
   placeholders shaped to the spec first (free), then Higgsfield blades (after your go-ahead), blade
   comparison view + IoU / gradient / coverage tests.
4. **Terrain variation.** Parameter SSBO + JSON + hot reload, `terrain_maps.c` (curvature, moisture,
   signed distance, macro), hex bombing, multi-scale, macro, tints, variants, POM, scatter, fidelity knob
   `terrain_quality` (0-4) in every level JSON, `terrain_debug` views.
5. **Transitions.** Per-pair JSON, all modes, transition sets, border scatter, weather interaction,
   transition test strip + band debug view.
6. **Secondary plants.** Shrub / flower / fern / reed cards, wheat retint, procedural bush retune.
7. **Report.** `ground_match_report`, `docs/TERRAIN_SHADING.md`, snapshots in `docs/ground_match/`.

Tree safety: trees keep their materials and pipeline; every new term is gated on `WPO_GRASS` +
`VGM_GROUND_COVER` or on `MAT_TERRAIN`; a tree regression snapshot (vegetation map tree close-ups,
impostor distance) is captured before step 3 and compared bit-exactly after each step.
