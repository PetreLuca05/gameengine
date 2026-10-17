# glTF PBR materials

The lighting was already PBR (GGX / Schlick / Smith in `shaders/lit_body.glsl` `brdf()`, sky environment
reflections, SSR, AO). glTF materials now bring their maps too, on every static mesh path: the legacy
lit / G-buffer draws (`render.c` `draw_items`), the vgeo resolve (`shaders/vgeo/vg_resolve.glsl`) and the vgeo
era fallback (`r_vgeo 0`). A material without maps renders exactly as before.

## What is imported (`src/assets/assets.c` `gltf_material`)

| glTF | Material (`render.h`) | Shading (`shaders/pbr_maps.glsl`) |
|---|---|---|
| `baseColorTexture` (sRGB) x `baseColorFactor` | `texture`, `MAT_TEXTURED`, `color` | unchanged |
| `metallicRoughnessTexture` (linear) | `mr_tex`, `MAT_MR_TEX` | roughness x= g, metallic x= b |
| `normalTexture` (linear), `scale` | `normal_tex`, `normal_scale`, `MAT_NORMAL_TEX` | tangent space, xy x scale |
| `occlusionTexture` (linear r), `strength` | `occlusion_tex`, `occlusion_strength`, `MAT_OCCLUSION_TEX` | 1 + strength (r - 1), x SSAO |
| `emissiveTexture` (sRGB) x `emissiveFactor` x `KHR_materials_emissive_strength` | `emissive_tex`, `emissive_rgb`, `emissive`, `MAT_EMISSIVE_TEX` | tex x colour x scalar |
| `alphaMode` MASK + `alphaCutoff` | `MAT_ALPHA_MASK`, `alpha_cutoff` | discard (own pipelines) |
| `doubleSided` | `MAT_TWO_SIDED` | no culling, back faces flip the normal |

Images can be files next to the .gltf (percent-decoded), `.glb` buffer views or base64 `data:` uris; each
image loads once per file and colour space (a shared ORM image for occlusion + metallic-roughness is
one texture).

## Normal maps: no tangent attribute

`Vertex` has no tangent and the mesh / vgeo cluster formats stay as they are:

- Legacy path: the cotangent frame from screen derivatives of world position and uv (`cotangent_frame`
  in `common.glsl`, shared with the surface textures). Exact for any uv layout (mirrored uvs included),
  costs a few ALU ops and works for every mesh source (glTF, procedural, vgeo fallback LODs).
- vgeo resolve: a full-screen pass, so neighbouring pixels can belong to other triangles or instances and
  screen derivatives would break at every edge. The resolve already fetches the triangle's three
  vertices, so it computes the triangle's dP/du, dP/dv (`g_tangent`, `g_bitangent`) and
  `vg_tangent_frame` makes them orthogonal to the interpolated normal.

Convention: glTF (OpenGL) normal maps, +y = image up = -v. glTF TANGENT attributes are ignored; the
derived frame matches them for well-formed assets (MikkTSpace differences are invisible at these
scales).

## Emissive

The lit pass re-shades every surface with its own material (the G-buffer prepass only feeds the
screen-space passes), so the emissive colour stays full RGB in the forward lit shading:
`tex.rgb x emissive_rgb x emissive`. Without an emissive texture emission stays albedo-tinted
(`base.rgb x emissive`) as before. The G-buffer's scalar emissive (material.a, 0..1 of 8) gets the
scalar x the brightest channel of the sample; nothing in the post stack needs its colour, so the
G-buffer layout is unchanged.

## Renderer plumbing

- Set 3 of `layout_lit` (`R->mat_layout`): an array of 4 combined image samplers (metallic-roughness,
  normal, occlusion, emissive). `vkr_material_set` caches one descriptor set per combination of map
  textures (`MAX_MAT_SETS`, set 0 = all white); maps still uploading sample white (normal: flat) until
  ready; sets go when a texture they hold is destroyed, and are rewritten with the material sampler.
  `lit.frag` / `gbuffer.frag` / `lit_mask.frag` / `gbuffer_mask.frag` declare it (`PBR_MAPS`); skin shaders
  do not.
- `PushLit.pbr` (push constants now exactly 128 bytes): normal_scale, occlusion_strength, emissive_rgb,
  alpha_cutoff.
- Pipelines `PIPE_LIT_2S` / `PIPE_PREPASS_2S` (doubleSided) and `PIPE_LIT_MASK` / `PIPE_PREPASS_MASK`
  (MASK, discard, two-sided): `draw_items` switches to them per draw and back, so the common pipelines
  keep early depth.
- vgeo: `VgMaterial` grew by 28 bytes (144 B with the wind parameters) (`mr_tex`, `normal_tex`, `occlusion_tex`, `emissive_tex`,
  `normal_scale`, `occlusion_strength`, `emissive_rgb`). The map indices are renderer texture ids in the
  vgeo texture table (binding 23), entered like the albedo (`tex_use`). MASK maps to `VGM_MASKED`
  (mask = albedo alpha) and doubleSided to `VGM_TWO_SIDED`, so every vgeo raster path and the shadows
  respect them. The era fallback draws the maps through the legacy path.

## Robustness

The vgeo resolve bounds every texture-table index it takes from a material record (albedo and the
four maps) to `VG_TEX_COUNT`, so a pixel that decodes a record that is not a real material can at
worst sample white, never index past the table. Empty pixels are covered too: the resolve passes
`discard` them, which the Vulkan 1.3 target compiles to `OpDemoteToHelperInvocation`, so those lanes
keep running the shading body as helper invocations; `vg_resolve_pixel` gives every input a default
(no flags, texture index 0) before it reads the visibility buffer. Before that the helpers sampled
`vg_tex[]` with undefined (register garbage) indices whenever their undefined flags said so: with the
maps in the G-buffer resolve this lost the device about a second after `spawn_horde 200` (skinned
characters fill the screen with resolve work). On `VK_ERROR_DEVICE_LOST`, `VK_CHECK` logs
`VK_EXT_device_fault` data (description, fault addresses, vendor records) before the fatal error.
`--test` runs (or `DG_NO_FATAL_DIALOG=1`) abort without the fatal message box.

## Other content using the same terms

- **Tree bark** (docs/VGEO.md "Tree models", "Bark maps"): the imported trees' opaque materials carry a
  normal map and an ORM map (r AO, g roughness, b metallic 0) as vgeo module textures
  (`assets/treepbr`, `tools/vfxgen/tree_pbr.py`), through the same `MAT_NORMAL_TEX` / `MAT_MR_TEX` /
  `MAT_OCCLUSION_TEX` path of the vgeo resolve. Module textures (index >= `VG_TEX_GAME`) are not legacy
  renderer textures, so the r_vgeo 0 fallback draws a material's maps only when every index is a
  renderer id.
- **Surface layers** (docs/TEXTURES.md "Ambient occlusion", "Metal"): the baked layer AO multiplies the
  SSAO exactly like `pbr_occlusion` (`ao *= surf_ao` in `lit_body.glsl`), and the metal layers replace the
  material metallic (`surf_metal`).

## Limits

- KHR_texture_transform: a uniform scale without offset / rotation on the base colour becomes
  `tex_scale` (all maps share one uv); anything else is logged and ignored. Only TEXCOORD_0.
- MASK on the legacy path: sun / local light shadows stay solid (the shadow pipelines have no fragment
  stage). The vgeo path has masked shadows.
- MASK single-sided is drawn two-sided; alphaMode BLEND relies on the existing blended path
  (`baseColorFactor` alpha < 1), a texture-only BLEND alpha draws opaque (logged).
- Skinned characters (`SkinnedDraw`) keep albedo-only materials.

## Test content

`tools/gen_pbr_test.py` writes `assets/models/pbr/` (no third-party packages):

- `pbr_ball`: a UV sphere with the full set (albedo, an ORM image used as occlusion + metallic-roughness,
  normal, emissive + `KHR_materials_emissive_strength`): painted / bare-steel panels, recessed seams,
  rivets, a cyan emissive strip.
- `pbr_swatches`: gold metal and red plastic spheres, roughness 0.05 .. 1.
- `pbr_mask`: a doubleSided MASK grille (embedded `data:` image) on a stand.

Dev room "PBR shelf" in the post corner (-19, 0, -17): swatches, the ball as a prop (legacy path), the
same ball as a vgeo placement, the grille, a lamp on the light schedule. `tools/tests/pbr_shelf.cfg`
takes day / night shots with `r_vgeo` 1 and 0.

Cost: none measurable for content without maps (identical-level A/B, RTX 3060 Ti, 1600x900). The
devroom now has a vgeo instance, which runs the vgeo resolve passes there (about 0.2 ms, the same as any
vgeo placement).
