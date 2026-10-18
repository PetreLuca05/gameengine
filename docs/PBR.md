# glTF PBR materials

The lighting was already PBR (GGX / Schlick / Smith in `shaders/lit_body.glsl` `brdf()`, sky environment
reflections, SSR, AO). glTF materials now bring their maps too, on every static mesh path: the legacy
lit / G-buffer draws (`render.c` `draw_items`) and the classic renderer of the vgeo content
(docs/CLASSIC_RENDERER.md; the nanite resolve that also shaded them lives on branch `nanite`). A material without maps renders exactly as before.

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
  costs a few ALU ops and works for every mesh source (glTF, procedural, the classic renderer's LODs).

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
  respect them. The classic renderer samples them in its own prepass / lit shaders
  (docs/CLASSIC_RENDERER.md).

## Robustness

The classic renderer resolves every map index of a `VgMaterial` before it binds it (`mat_maps`: a renderer
id or a wrapped module slot); a map that does not resolve drops its `MAT_*_TEX` flag, so every sampled slot
is a valid texture (the nanite resolve's own guards live on branch `nanite`). On `VK_ERROR_DEVICE_LOST`, `VK_CHECK` logs
`VK_EXT_device_fault` data (description, fault addresses, vendor records) before the fatal error.
`--test` runs (or `DG_NO_FATAL_DIALOG=1`) abort without the fatal message box.

## Skinned characters

Textured skins carry normal, ORM (r occlusion, g roughness, b metallic) and optional emissive atlases built
from the FBX maps (docs/CHARACTERS.md "PBR maps"). `SkinnedDraw` has `mr_tex` / `normal_tex` / `occlusion_tex`
/ `emissive_tex` / `normal_scale` with the same `MAT_*_TEX` flags:

- Legacy skinned path (every character, phone views, first-person arms): `skin.frag` and `gbuffer_skin.frag`
  define `PBR_MAPS` (cotangent frame from screen derivatives, as `lit.frag`). `build_skin_pass` resolves the
  map set (`vkr_material_set`) per batch (per albedo + mesh, and a mesh has one set of maps) and
  `draw_skin_pass_f` binds it as set 3; `normal_scale` rides in `SkinInstance.info.w` (`skin.vert` passes
  it flat as `PBR_PARAMS.x`). The G-buffer skin pass uses `vec4(params.xyz, 1)` as `GB_PARAMS` (its `w`
  is the LOD fade, not a uv scale). The CPU skinning fallback draws without maps.

## Other content using the same terms

- **Tree bark** (docs/VGEO.md "Tree models", "Bark maps"): the imported trees' opaque materials carry a
  normal map and an ORM map (r AO, g roughness, b metallic 0) as vgeo module textures
  (`assets/treepbr`, `tools/vfxgen/tree_pbr.py`), through the same `MAT_NORMAL_TEX` / `MAT_MR_TEX` /
  `MAT_OCCLUSION_TEX` path. Module textures (index >= `VG_TEX_GAME`) are wrapped as renderer textures by
  the classic renderer.
- **Surface layers** (docs/TEXTURES.md "Ambient occlusion", "Metal"): the baked layer AO multiplies the
  SSAO exactly like `pbr_occlusion` (`ao *= surf_ao` in `lit_body.glsl`), and the metal layers replace the
  material metallic (`surf_metal`).

## Limits

- KHR_texture_transform: a uniform scale without offset / rotation on the base colour becomes
  `tex_scale` (all maps share one uv); anything else is logged and ignored. Only TEXCOORD_0.
- MASK on the legacy path: sun / local light shadows stay solid (the shadow pipelines have no fragment
  stage). vgeo content (the classic renderer) has masked shadows.
- MASK single-sided is drawn two-sided; alphaMode BLEND relies on the existing blended path
  (`baseColorFactor` alpha < 1), a texture-only BLEND alpha draws opaque (logged).
- Skinned characters take maps only through their skin atlases (docs/CHARACTERS.md "PBR maps"): the skin
  shaders have occlusion strength 1, a white emissive colour and no alpha cutoff (only `normal_scale` is
  per instance).

## Test content

`tools/gen_pbr_test.py` writes `assets/models/pbr/` (no third-party packages):

- `pbr_ball`: a UV sphere with the full set (albedo, an ORM image used as occlusion + metallic-roughness,
  normal, emissive + `KHR_materials_emissive_strength`): painted / bare-steel panels, recessed seams,
  rivets, a cyan emissive strip.
- `pbr_swatches`: gold metal and red plastic spheres, roughness 0.05 .. 1.
- `pbr_mask`: a doubleSided MASK grille (embedded `data:` image) on a stand.

Dev room "PBR shelf" in the post corner (-19, 0, -17): swatches, the ball as a prop (legacy path), the
same ball as a vgeo placement, the grille, a lamp on the light schedule. `tools/tests/pbr_shelf.cfg`
takes day / night shots.

Cost: none measurable for content without maps (identical-level A/B, RTX 3060 Ti, 1600x900). The
devroom now has a vgeo instance (the classic renderer draws it).
