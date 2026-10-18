# Texture mips, filtering and texture quality

How material textures get their mip chains, how they are filtered, and what `r_texture_quality` does.
Mesh LOD is separate (docs/LOD.md, docs/VGEO.md).

## Mip chains

| Source | Chain | Code |
|---|---|---|
| PNG / JPG / TGA (manifest, glTF maps: base colour, metallic-roughness, normal, occlusion, emissive) | GPU blits, one per level (`record_texture_upload`); CPU box chain (`vg_tex_build_mips`) when the format has no linear blits | `texture_load_path` / `texture_load_memory` (src/assets/assets.c) -> `render_texture_create_world` |
| KTX2 | the file's levels; `levelCount 0` ("generate at load") on RGBA8 gets the blit chain, other formats keep one level (warning) | `ktx2_load` -> `render_texture_create_raw_world` |
| Surface / terrain arrays, VFX atlas, vgeo module textures, classic impostors | CPU box chains | surface_tex.c, terrain_tex.c, vk_vfx.c, vgeo_tex.c, vk_classic.c |
| UI atlases, font, phone, avatars, cookie, puddle mask | none (screen-sized or data) | `render_texture_create(..., false)` |

Normal-map mips are box-filtered without renormalising; `pbr_normal` (shaders/pbr_maps.glsl) normalises after
the tangent transform, so shortened texels only soften the map at distance (the intended look).

## Filtering

- `sampler_world` (world textures) and `sampler_linear` (everything else that samples material-like
  data): trilinear, anisotropic at `r_aniso` (capped by the device), `maxLod` unclamped.
- The vgeo module's own samplers (`VgTextures.clamp` / `.repeat`: tree and foliage textures, impostor
  atlases) follow `r_aniso` too (`vg_tex_set_anisotropy`, re-applied by `vkg_samplers_changed`).
- `sampler_clamp` is level 0 only (data maps, post inputs).

### vgeo resolve (`r_vgeo 1`)

`vgeo_gbuffer.frag` / `vgeo_lit.frag` are full-screen passes over the visibility buffer: a 2x2 quad can
span several triangles (or empty pixels), so implicit derivatives would pick wrong mips along every edge.
`vg_resolve_pixel` also evaluates the barycentrics one pixel right and one pixel down on the same
triangle's plane: `g_duv_dx / dy` (uv) and `g_dp_dx / dy` (world position). Material samples go through
`vg_tex_map` (uv x `tex_scale`) or `vg_tex_grad` (triplanar: `LIT_TEX_W` in lit_body.glsl with
`LIT_DPDX / LIT_DPDY`) = `textureGrad`, which keeps anisotropic filtering. The forward bodies are
unchanged (`LIT_TEX_W` defaults to `LIT_TEX`, the gradient arguments are never expanded).

## Texture quality (`r_texture_quality` 0 low, 1 medium, 2 high)

World textures (manifest and glTF material textures; `GpuTexture.world`) keep the top `2 - quality`
levels **out of memory** (`texture_drop`: never the last level): a 2048 map at low is resident as 512
(1/16 of the memory). Their sampler (`sampler_world`) has no LOD clamp. Everything else keeps its whole
chain and gets the quality as a min LOD on `sampler_linear` (as before); terrain halves its arrays at low
when it loads (terrain_tex.c).

- At creation the dropped levels are box-filtered away on the CPU (sRGB in linear light) and the rest
  is uploaded and mipmapped; KTX2 skips the dropped levels.
- **Lower** at runtime (`vkr_textures_quality_changed`, from `apply_setting_changes` after a device
  wait): every world texture is copied into a smaller image (`vkCmdCopyImage` of its remaining levels,
  one waited submit for all) and the old images are freed. Textures still uploading are trimmed on the
  next frame (`texq_retry`). The startup config lands here too: the assets load before the config is
  read, so a saved Low trims them in the first frame.
- **Raise** at runtime: the dropped levels must come from the source. The renderer sets `tex_reload`;
  `assets_reload_textures` (main loop, between frames) re-decodes every world texture whose
  `render_texture_reload_needed` is true (the file, or the glTF image by index: parsed again, buffers
  only for buffer-view images) and hands it to `render_texture_reload(_raw)`, which replaces the image
  in the same texture id and set. The vgeo table shows white for a replaced texture until its upload
  is recorded (`vkg_texture_replaced`).

There is no distance-based texture streaming or sparse residency: all resident levels stay resident.
