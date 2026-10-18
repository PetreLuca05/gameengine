# Texture mips, filtering, texture quality and block compression

How material textures get their mip chains, how they are filtered, what `r_texture_quality` does, and
how the content textures are stored block-compressed (BC7). Mesh LOD is separate (docs/LOD.md,
docs/VGEO.md).

## Mip chains

| Source | Chain | Code |
|---|---|---|
| PNG / JPG / TGA (manifest, glTF maps: base colour, metallic-roughness, normal, occlusion, emissive) | RGBA8: GPU blits, one per level (`record_texture_upload`); CPU box chain (`vg_tex_build_mips`) when the format has no linear blits. BC7: the same filter on the CPU (`texc_mip_chain`, sRGB in linear light), each level encoded | `texture_load_path` / `texture_load_memory` (src/assets/assets.c) -> `render_texture_create_world` / `_raw_world` |
| KTX2 | the file's levels, as stored; `levelCount 0` ("generate at load") on RGBA8 gets the chain above, other formats keep one level (warning) | `ktx2_load` -> `render_texture_create_raw_world` |
| Surface / terrain arrays, VFX atlas, vgeo module textures, classic impostors | CPU box chains (the module's alpha-weighted / coverage-preserving ones); the arrays and module textures each level encoded when BC7 is on | surface_tex.c, terrain_tex.c, vk_vfx.c, vgeo_tex.c, vk_classic.c |
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
  is uploaded and mipmapped; KTX2 and BC7 chains skip the dropped levels.
- **Lower** at runtime (`vkr_textures_quality_changed`, from `apply_setting_changes` after a device
  wait): every world texture is copied into a smaller image (`vkCmdCopyImage` of its remaining levels,
  one waited submit for all; BC7 levels copy as blocks) and the old images are freed. Textures still
  uploading are trimmed on the next frame (`texq_retry`). The startup config lands here too: the assets
  load before the config is read, so a saved Low trims them in the first frame.
- **Raise** at runtime: the dropped levels must come from the source. The renderer sets `tex_reload`;
  `assets_reload_textures` (main loop, between frames) re-decodes every world texture whose
  `render_texture_reload_needed` is true (the file, or the glTF image by index: parsed again, buffers
  only for buffer-view images) and hands it to `render_texture_reload(_raw)` (a BC7 texture: its chain
  from the cache), which replaces the image in the same texture id and set. The vgeo table shows white
  for a replaced texture until its upload is recorded (`vkg_texture_replaced`).

There is no distance-based texture streaming or sparse residency: all resident levels stay resident.

## Block compression (BC7, `r_texture_compress`, PF-0035)

The content textures are stored as BC7 (1 byte per texel instead of 4). Each loader keeps building its
own RGBA8 mip chain exactly as before; `texc_encode` (src/render/tex_compress.c) encodes every level of
it, on the job system, and the image is created in the BC7 format with those levels.

| Kind | Format | Textures |
|---|---|---|
| colour: sRGB albedo, + alpha coverage / baked AO | `VK_FORMAT_BC7_SRGB_BLOCK` | world textures with `srgb` (skins, glTF base colour / emissive), terrain / surface albedo arrays (AO in alpha), the scatter atlas, tree albedo, grass atlases, foliage masks, impostor albedo atlases |
| data: normal maps, ORM, nrh (normal xy + roughness + height), nta, macro | `VK_FORMAT_BC7_UNORM_BLOCK`, encoded with `BC_DATA` | world textures without `srgb`, terrain nrh / macro arrays, surface nrh (+ metal masks), tree bark normal / ORM, grass nta maps, impostor normal atlases |

Not compressed: KTX2 files (their stored format), the VFX atlas (flipbooks with motion vectors and
six-way lightmaps share pages), the classic renderer's impostor atlases (vk_classic.c), UI / font / phone
textures, the terrain blend / attribute / paint maps (`texelFetch`, painted at runtime), render targets,
1-texel placeholders.

- **Encoder** (src/render/bc_codec.c, pure): BC7 modes 6 (one RGBA subset, 4-bit indices), 5 and 4
  (a rotated channel with its own indices: alpha, or for data maps any channel), 1 and 3 (two RGB subsets
  on the partitions whose subsets lie closest to a line) and 7 (two RGBA subsets for blocks with alpha).
  Each candidate is fitted by principal axis + least-squares refits and scored by the exact squared
  error of its decoded texels; the best one is written. Effort `BC_Q_NORMAL` (the engine's), `FAST`,
  `HIGH`. BC1 / BC3 / BC4 / BC5 encoders are there too (tested; the engine uses BC7 only: every texture
  is RGBA, the shaders read all four channels or the normal's z). Decoders for all of them.
- **Cache**: `<writable>/assets/cache/tex/<key>.bct` (the build's `bin/assets/cache`, git-ignored). The
  key is a 64-bit hash over every level's texels and size, the kind / effort and `BC_CODEC_VERSION`:
  only an identical input reads a file, a changed source simply misses (stale files are never read;
  `tex_cache_clear` deletes them). Written beside and renamed, so another process never reads half a
  file. `tex_compress_stats` prints sets from the cache / encoded, the hash and read times, RGBA8 vs BC7
  bytes.
- **Cost**: the first load of a texture encodes it (vegetation map, cold cache: 143 sets, 581 MB of
  RGBA8 levels in ~26 s on 12 threads, most of it the terrain and surface arrays); later loads hash the
  levels (~65 ms for that map) and read the cache (145 MB, 0.7 - 0.9 s on the shared test machine). The
  uploads are 4 x smaller.
- **Switch**: `r_texture_compress 0` keeps RGBA8 (the A / B reference; CVAR_LOCKED, dev builds). World
  textures follow at once (`assets_reload_textures` replaces every one in the other format:
  `render_texture_replace(_raw)`); the rest at its next load (the next map; trees, grass and surfaces
  load once per session). `+set r_texture_compress 0` on the command line is taken before the startup
  loads (`texc_init`). `perf_fix_disable PF-0035` does the same. A device that cannot sample BC7 (feature
  query at init) stays on RGBA8.

### Measurements (RTX 3060 Ti, 1600 x 900, shared machine; tools/tests/tex_bc_compare.py, 3 alternating runs)

| | RGBA8 | BC7 | |
|---|---|---|---|
| texture level data, vegetation session (texc stats) | 581 MB | 145 MB | -436 MB |
| VMA allocated, vegetation map | 1614 MB | 1179 MB | -435 MB (-27 %) |
| VMA usage (`vram_usage_mb`), vegetation map | 1823 MB | 1280 MB | -543 MB (-30 %) |
| VMA allocated, dev room | 957 MB | 926 MB | -31 MB (the skins) |
| GPU frame, meadow (uncapped, 240 frames x 3 alternating runs) | 10.28 ms | 9.86 ms | -4.0 % |
| GPU frame, view down at the ground (its opaque pass) | 8.52 ms (3.35) | 7.82 ms (2.70) | -8.2 % (-19 %) |
| GPU frame, pine forest | 22.92 ms | 22.32 ms | -2.6 % |
| texture loads, warm cache: manifest / trees / grass / surfaces / terrain | 63 / 377 / 941 / 262 / 864 ms | 111 / 425 / 1035 / 357 / 915 ms | +48 ms / +13 / +10 / +36 / +6 % |

The loads hash the chains and read the cache instead of uploading 4 x the bytes (the startup's medians
moved within the machine's noise: 4686 -> 4497 ms).

### Image difference

Full frames, BC7 vs RGBA8 (fixed sim step; tools/tests/tex_bc.cfg views): the dev room views 49.3 dB
PSNR, the same as two identical RGBA8 runs (unchanged); vegetation views 35.5 - 37.1 dB (two identical
runs: 82 - 100 dB), mean 1.7 - 2.2 levels of 255, 2.8 - 4.3 % of the channel values off by more than 8:
grass / leaf card silhouettes where alpha moves across the cut-off and the fine grain of terrain, bark
and grass; the view down at the grass 33.7 dB (two RGBA8 runs of it already differ at 38 dB). At 2 x
zoom the textures look the same:

- `docs/tex_bc_screenshots/bc_ground_zoom.jpg`: terrain + grass up close (RGBA8 | BC7 | difference x 8)
- `docs/tex_bc_screenshots/bc_pine_bark.jpg`: bark and leaf cards in the pine forest, 2 x
- `docs/tex_bc_screenshots/bc_meadow.jpg`, `bc_wheat.jpg`: open ground, grass, trees at range
- `docs/tex_bc_screenshots/bc_dev_character.jpg`: a teammate's skin (no visible change)

Codec on assets (tests/test_bc_codec.c, 512 x 512 windows): BC7 45.2 dB on the dirt albedo photo, 40.1 on
the grass atlas (RGBA), 35.3 on a terrain nrh map and 35.2 on a bark normal map (data effort); BC5 39.7
on that normal's xy, BC4 52.5 on an AO map.

### Tests

- `test_bc_codec` (ctest `bc_codec`): every BC7 mode / partition / rotation decodes to exactly the error
  the encoder scored; solid colours (BC7 within 1, BC4 exact); ramps and two-subset blocks; binary
  alpha keeps every texel on its side of 0.5; BC4 within 10 % of an exhaustive search; PSNR floors on
  real assets; determinism.
- `python tools/tests/bc_crosscheck.py`: the encoded test images and a sheet of every mode decoded by
  Pillow's BCn decoders: BC7 bit-exact, BC1 / BC3 / BC4 / BC5 within 1.
- `python tools/tests/tex_bc_compare.py` (tools/tests/tex_bc.cfg): A / B runs (VRAM, GPU, load lines,
  screenshots, diff heat maps).

## Decoding on the jobs (PF-0024)

The texture files the loads read one after the other on the main thread are decoded on the job system
first, then consumed in the old order (the same `stbi_load` and resampling per file: the same pixels,
the same texture ids, the same atlas packing):

- the asset manifest's textures (`assets_init`, startup: `texture_predecode`),
- the VFX atlas files (`render_vfx_set_library`, startup: decoded and halved to fit a page),
- the surface layers (`render_surfaces_load`: albedo + AO, nrh, metal masks per layer),
- the grass atlases and nta maps (`foliage_build_grass`).

`perf_fix_disable PF-0024` (also from the command line, before the startup loads) decodes them in order
again. Measured (tools/tests/tex_bc_compare.py --a "+set perf_fix_disable PF-0024" --b "", two sets of 3
alternating runs, medians): manifest textures 259 -> 134 ms (-39 .. -48 %), VFX atlas 483 -> 162 ms
(-48 .. -66 %), grass atlases 1899 -> 1017 ms (-46 %), surfaces 1370 -> 351 ms (-69 .. -74 %); the startup
-9 % (within the shared machine's noise). Screenshots: A vs B of one repetition never differ more than
two repetitions of the same side (identical in 5 of 7 views; the wheat and ground views carry
run-to-run noise).
