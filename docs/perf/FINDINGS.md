# Tracked performance findings

Generated from `docs/perf/fixes.json` by `tools/perf_audit/perf_audit.py findings`. A finding is **closed** only when `perf_verify` showed the timing improvement AND every reference image pair passed AND the gameplay state hashes matched (docs/PERF_AUDIT.md "Closing a finding"). Fixes are switched off for A/B runs with `perf_fix_disable <id>`. A **visual trade-off** (marked in the title) changes the image by design: its image pairs are recorded but do not decide, its visual difference is documented with screenshots, and `perf_fix_disable all` leaves it on.

| Fix | Finding | Title | Status | Effort | Verified on | Result |
|---|---|---|---|---|---|---|
| PF-0001 | R01 clear-only shadow passes (shadow_sun at night / sun shadows off, shadow_moon by day, shadow_vm without a rig) | Empty shadow maps are not re-cleared every frame | verified on Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores (close on the target machine) | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | CLOSED: scopec:shadow_moon:clear_px 4194304.0 -> 0.0 (-100.00%, p=0); images pass; state identical |
| PF-0002 | R26 cvar_find calls per frame (31 / frame in the dev room: a case-insensitive scan of ~700 cvars each) | cvar_find through a case-insensitive hash index | verified on Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores (close on the target machine) | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | CLOSED: zone_all:cvar_find 0.08996 -> 0.01043 (-88.41%, p=0); images pass; state identical |
| PF-0003 | R08 malloc / free in vg_palettes_record every frame | vgeo palette upload regions without a per-frame malloc | dropped | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | NOT CLOSED: zone_all:vgeo:palettes_record 0.88855 -> 0.91525 (+3.00%, p=0.628); images pass; state identical |
| PF-0004 | R08 malloc / free in vg_stream_update every frame | vgeo streaming copy list without a per-frame malloc | dropped | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | CLOSED: zone_all:vgeo:stream_update 0.97765 -> 0.0366 (-96.26%, p=0); images pass; state identical |
| PF-0005 | R20 fg:validate (check_graph: pass name copies, dead pass / unused resource analysis, pool scan) every frame although only the fidelity checks and benches read it | Frame graph validation report built on demand | verified on Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores (close on the target machine) | S | Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores | CLOSED: zone:fg:validate 0.01746 -> 0.0 (-100.00%, p=0); images pass; state identical |
| PF-0006 | R11 shadow caster items far above the scene items / R02 static casters in every cascade (F-7b209b, F-b96020): every caster inside a cascade's light box was drawn, also those whose shadow falls only on ground outside the view | Sun cascade casters culled to the cascade's receiver volume | closed | M | DESKTOP-SN4N74U | CLOSED: scope:shadow_sun 1.687 -> 1.5465 (-8.33%, p=0); images pass; state identical |
| PF-0007 | R10 duplicate skinning palettes (F-5c8ab1): every part of a modular character copied the same matrices into the frame's bone buffer (1927 copies, 3.4 MB per frame with 500 zombies), then again into the GPU buffer, the previous-palette buffer and its GPU copy | Parts of one character share one palette copy | closed | S | DESKTOP-SN4N74U | CLOSED: zone:render:prepare 1.2605 -> 0.75955 (-39.74%, p=0); images pass; state identical |
| PF-0008 | R07 workers idle while the main thread runs serial zones (F-66ec5c): game_render is the largest main-thread zone with a horde (5.2 ms, 500 zombies), most of it chars_render interpolating every bone of every drawn character and building its skinning matrices one character after the other | Character draw poses on the job system | dropped | M | DESKTOP-SN4N74U | NOT CLOSED: zone:game_render 5.803 -> 3.5205 (-39.33%, p=0); images pass; state identical |
| PF-0009 | R07 main thread stalls waiting for jobs (F-37a7e1): in the vegetation scenes the classic cull ran one jobs_parallel_for per view (main view, up to 4 sun cascades, the scope PiP), each ending in a tail wait for its slowest cell before the next view could start (0.5 - 0.7 ms of wait:parfor_tail per frame, render:prepare 3.8 ms) | The classic renderer's views culled in one parallel pass | closed | M | DESKTOP-SN4N74U | CLOSED: zone:render:prepare 3.258 -> 2.646 (-18.78%, p=0); images pass; state identical |
| PF-0010 | R07 workers idle while the main thread runs serial zones (F-66ec5c): weather_frame -> wind_game_frame -> wind_tile_update evaluated the 64 x 64 wind window (wind_at: gust fronts, three turbulence noises, sources; a damped spring per texel) on the main thread every frame, ~1.6 ms of game_render in every scene (found by zoning game_render on the vegetation map) | Wind tile rows on the job system | closed | S | DESKTOP-SN4N74U | CLOSED: zone:game_render 1.594 -> 0.96035 (-39.75%, p=0); images pass; state identical |
| PF-0011 | R11 shadow casters drawn into cascades (F-7b209b): the skinned shadow pass (every zombie's parts, instanced) was drawn whole into each of the 4 sun cascades and the moon map, so a 500-zombie horde was skinned 5 times for the shadows; the raster and classic casters were already culled per tile | Skinned shadow casters culled per sun cascade | closed | M | DESKTOP-SN4N74U | CLOSED: scope:shadow_sun 0.3625 -> 0.3092 (-14.70%, p=0); images pass; state identical |
| PF-0012 | R02 shadow casters in every cascade (F-b96020) / alpha-test overdraw in the dense vegetation zones: the pine forest interior's sun shadow pass ran 670 M alpha-tested fragment invocations per frame (~40 x the 4 tiles' 16.7 M texels; 14 - 21 ms of GPU at Modern), instances in cell order, small meshes before trees | Classic shadow instances drawn front to back from the light | closed | S | DESKTOP-SN4N74U | CLOSED: scope:shadow_sun 29.5 -> 27.49 (-6.81%, p=1e-05); images pass; state identical |
| PF-0013 | R02 shadow casters in every cascade (F-b96020) / alpha-test overdraw in the dense vegetation zones: after PF-0012 the pine forest's sun shadow pass still ran ~660 M alpha-tested fragment invocations per frame (~40 x overdraw of leaf / grass cards in the 4 x 2048^2 tiles, 14 - 30 ms at Modern); 2 cascades instead of 4 cut 58 % but blurred every tree shadow | Grass and small plant shadows in the near cascades only (per-class caster cascade limit) **(visual trade-off)** | closed | M | DESKTOP-SN4N74U | CLOSED: scope:shadow_sun 3.009 -> 2.546 (-15.39%, p=0); images differ (visual trade-off, by design); state identical |
| PF-0024 | R27 startup critical path (F-0714da): main thread blocked 682 ms on file reads during startup (4864 ms to the first frame); the loads also decoded every texture file one after the other on the main thread: the manifest textures (assets_init), the ~100 VFX atlas files (game_init), and at the first open-world map the 20 surface layers (albedo + AO + nrh + metal masks) and the 8 grass atlases + nta maps | Texture files decoded on the job system at load | implemented, measured A / B (a load-time fix: perf_verify captures frames only) | S | DESKTOP-SN4N74U | A / B (2 x 3 alternating runs): manifest -39 .. -48%, VFX atlas -48 .. -66%, grass atlases -46%, surfaces -69 .. -74%; images identical |
| PF-0035 | R14 textures stored uncompressed (F-1a9828 .. F-a94519, static): every content texture was RGBA8 (only assets/textures/hazard.ktx2 pre-encoded). The 4096^2 Ch*_1001 maps the rule names are source art (the game loads their 1024 / 2048 skin atlases); the textures actually resident are the terrain (216 MB) and surface arrays, the tree / grass / foliage / impostor module textures and the skins: 581 MB of RGBA8 levels in a vegetation session | World, terrain and foliage textures block-compressed (BC7, encoded on the jobs, cached on disk) **(visual trade-off)** | implemented, measured A / B (VRAM, GPU, images; a load-time format change: perf_verify does not reload textures) | L | DESKTOP-SN4N74U | VRAM -435 MB allocated / -543 MB usage on the vegetation map (-27 / -30%), GPU frame -2.6 .. -8.2%; images differ (visual trade-off, by design) |

## PF-0001: Empty shadow maps are not re-cleared every frame

The sun cascade atlas, the moon map and the viewmodel shadow tile were transitioned, cleared to depth 1 and transitioned back every frame even when nothing is drawn into them (the whole atlas at night, the moon map all day). A map that was cleared and left empty last frame and draws nothing this frame keeps its content: the clear, both barriers and the empty rendering are skipped.

- Rule / finding: R01 R01 clear-only shadow passes (shadow_sun at night / sun shadows off, shadow_moon by day, shadow_vm without a rig)
- Code: `src/render/render.c shadow_clear_skip`, `src/render/vk_device.c (shadow_gen)`
- Metrics (perf_verify): `scopec:shadow_moon:clear_px`, `scope:shadow_moon`, `scopec:shadow_sun:clear_px`, `scope:shadow_sun`, `scope:shadow_vm`, `gpu_frame`
- Scenes: night_lights, devroom
- Why fidelity-neutral: The skipped map holds exactly the clear the next sample expects (depth 1, read as lit), in the same SHADER_READ_ONLY layout; the earlier barrier made that clear visible to every later read in submission order. A re-created map (resize, resolution change) bumps shadow_gen and is cleared again. shadow_drawn bits (validation) are unchanged.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): CLOSED: scopec:shadow_moon:clear_px 4194304.0 -> 0.0 (-100.00%, p=0); images pass; state identical

## PF-0002: cvar_find through a case-insensitive hash index

cvar_find scanned every registered cvar with SDL_strcasecmp; 114 call sites, several per frame. The cvars now sit in a 2048-slot open-addressing index keyed by a case-insensitive FNV-1a hash of the name, filled by cvar_reg (cvars are never removed). MAX_CVARS rose from 512 to 768 (the lint counts 782 commands + cvars).

- Rule / finding: R26 R26 cvar_find calls per frame (31 / frame in the dev room: a case-insensitive scan of ~700 cvars each)
- Code: `src/core/cvar.c cvar_find / cvar_reg (index[])`
- Metrics (perf_verify): `zone_all:cvar_find`, `counter:cvar_find`, `cpu_work`
- Scenes: devroom, horde500
- Why fidelity-neutral: Returns the same pointer for every name (the index probes compare with the same SDL_strcasecmp; a name longer than the stored one misses in both paths). tests/test_profiler.c checks hash == scan, case-insensitive, and the miss.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): CLOSED: zone_all:cvar_find 0.08996 -> 0.01043 (-88.41%, p=0); images pass; state identical

## PF-0003: vgeo palette upload regions without a per-frame malloc

The VkBufferCopy region list of the skinning palette upload was malloc'd and freed every frame (max_slots + 1 entries). It is now allocated once with the palettes. Dropped with the virtualized geometry renderer (the classic renderer has no such path); the id is not reused.

- Rule / finding: R08 R08 malloc / free in vg_palettes_record every frame
- Code: `src/render/vgeo/vgeo_skin.c vg_palettes_record`, `src/render/vgeo/vgeo_skin.h VgPalettes.regions`
- Metrics (perf_verify): `zone_all:vgeo:palettes_record`, `site:vgeo_skin.c`, `cpu_work`
- Scenes: horde500, devroom
- Why fidelity-neutral: vkCmdCopyBuffer copies the regions into the command buffer at record time: the same regions, the same copies, the same data.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): NOT CLOSED: zone_all:vgeo:palettes_record 0.88855 -> 0.91525 (+3.00%, p=0.628); images pass; state identical

## PF-0004: vgeo streaming copy list without a per-frame malloc

The page copy list of the virtualized geometry streamer (2 x pages_per_frame VkBufferCopy) was malloc'd and freed every frame. It is now allocated with the stream. Dropped with the virtualized geometry renderer (the classic renderer has no such path); the id is not reused.

- Rule / finding: R08 R08 malloc / free in vg_stream_update every frame
- Code: `src/render/vgeo/vgeo_stream.c vg_stream_update`, `src/render/vgeo/vgeo_stream.h VgStream.copy_scratch`
- Metrics (perf_verify): `zone_all:vgeo:stream_update`, `site:vgeo_stream.c`, `cpu_work`
- Scenes: foliage, devroom
- Why fidelity-neutral: Same capacity (ppf_cap never changes after init), same contents, consumed by vkCmdCopyBuffer at record time.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): CLOSED: zone_all:vgeo:stream_update 0.97765 -> 0.0366 (-96.26%, p=0); images pass; state identical

## PF-0005: Frame graph validation report built on demand

fg_execute built the validation report every frame. It is now built by the first fg_last_report after the frame (render_graph_report: fidelity_test, bench_fidelity, the profile's fg section); fg_trim and fg_targets_changed build it before they drop pool images.

- Rule / finding: R20 R20 fg:validate (check_graph: pass name copies, dead pass / unused resource analysis, pool scan) every frame although only the fidelity checks and benches read it
- Code: `src/render/graph/fg.c fg_last_report / fg_execute / fg_trim / fg_targets_changed`
- Metrics (perf_verify): `zone:fg:validate`, `cpu_work`
- Scenes: devroom
- Why fidelity-neutral: Validation only: no rendering decision reads it. Between fg_execute and the next fg_begin the graph, the resources and the pool state it reads are unchanged, so an on-demand report is identical to the eager one.

- Verification run (Linux container, Mesa lavapipe 25.2 (software Vulkan), 4 cores, 2026-10-18): CLOSED: zone:fg:validate 0.01746 -> 0.0 (-100.00%, p=0); images pass; state identical

## PF-0006: Sun cascade casters culled to the cascade's receiver volume

Per cascade k below the last, the light-space convex hull of the view slice up to split k (and a 3 m box around the eye: the first-person rig) is extruded along the light into world planes, grown by the filter's reach (normal offset + (1.5 x PCSS max texels + 6) texels + 0.25 m). Classic instances, placement cells / chunks and raster draws whose bounds lie outside it are skipped in that cascade.

- Rule / finding: R11 R11 shadow caster items far above the scene items / R02 static casters in every cascade (F-7b209b, F-b96020): every caster inside a cascade's light box was drawn, also those whose shadow falls only on ground outside the view
- Code: `src/render/shadow_csm.c csm_receiver_planes`, `src/render/render.c compute_matrices (shadow_rcv), draw_items`, `src/render/vk_classic.c cull_cells / cull_instance (ClCull.rcv)`
- Metrics (perf_verify): `scope:shadow_sun`, `scopec:shadow_sun:vs_invocations`, `gpu_frame`, `zone:render:prepare`
- Scenes: foliage, devroom
- Why fidelity-neutral: lighting.glsl samples cascade k < last only for receivers at a selection distance <= split k (its range, the blend from k - 1, the fallback from k - 1), i.e. visible points of the view slice, plus the rig near the eye (shadow_factor_vm). Every tap of every filter (hard, tent, PCSS search and disc) lies within the margin of the receiver's projection, and a caster outside the extruded hull projects outside it: no texel any receiver reads changes. The last cascade (particles, decals, froxels sample it anywhere in its tile) is never culled, nor any cascade while the scope PiP or a phone camera samples them. tests/test_shadow_csm.c: 36000 receiver / caster pairs, none culled.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: scope:shadow_sun 1.687 -> 1.5465 (-8.33%, p=0); images pass; state identical

## PF-0007: Parts of one character share one palette copy

render_draw_skinned compares the palette with the copy it made for the previous draw (same bone count, memcmp); when equal, the draw takes that copy's offset instead of appending a new one. The bone buffer, its GPU upload and the previous-palette copies shrink by the duplicates. skin_poses now counts character poses drawn (draw_char), so R10 compares uploads with poses.

- Rule / finding: R10 R10 duplicate skinning palettes (F-5c8ab1): every part of a modular character copied the same matrices into the frame's bone buffer (1927 copies, 3.4 MB per frame with 500 zombies), then again into the GPU buffer, the previous-palette buffer and its GPU copy
- Code: `src/render/render.c render_draw_skinned`, `src/gameplay/characters.c draw_char / draw_char_fp (PC_SKIN_POSES)`
- Metrics (perf_verify): `zone:render:prepare`, `counter:skin_palette_bytes`, `counter:memcpy_bytes`, `cpu_work`
- Scenes: horde500
- Why fidelity-neutral: Only bit-identical palettes are shared: the skinning shaders read the same matrices at the shared offset. The previous-palette lookup (velocity) stores the offset / count each draw got, so last frame's matrices of every part are the same values too.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: zone:render:prepare 1.2605 -> 0.75955 (-39.74%, p=0); images pass; state identical

## PF-0008: Character draw poses on the job system

chars_render first reads the interpolation instant and the seat fix (vehicles) of every drawn character on the main thread, then interpolates the poses and builds the skinning matrices (skel_inst_set_pose) with jobs_parallel_for over the characters (ragdolls stay on the main thread: they read the physics world). The part selection, LOD choice and every draw call stay serial, in the same entity order. Dropped: perf_verify (horde500, 3 x A/B) measured game_render -39 % and CPU work -21 %, but the GPU frame +11 % in every B pass (shadow_sun +0.16 ms; the GPU reads the host-visible bone / instance buffers while 11 workers run), so the guard failed. The code was reverted; the id is not reused.

- Rule / finding: R07 R07 workers idle while the main thread runs serial zones (F-66ec5c): game_render is the largest main-thread zone with a horde (5.2 ms, 500 zombies), most of it chars_render interpolating every bone of every drawn character and building its skinning matrices one character after the other
- Code: `src/gameplay/characters.c chars_render / pose_for_draw / pose_range`
- Metrics (perf_verify): `zone:game_render`, `cpu_work`
- Scenes: horde500
- Why fidelity-neutral: Each job writes only its own character's pose_draw and skeleton instance from inputs no job writes (pose_prev / pose_cur, the tick, the seat fix taken before), with the same arithmetic as the serial loop: the poses, palettes and the draw order are identical whatever the thread count.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): NOT CLOSED: zone:game_render 5.803 -> 3.5205 (-39.33%, p=0); images pass; state identical

## PF-0009: The classic renderer's views culled in one parallel pass

vkc_frame builds every view's cull state first (the same ClCull per view as before), the cell tests of each view into its own task list, then culls the instances of every view in one jobs_parallel_for over the concatenated task lists, each view writing into its own output buffer. The outputs are packed per view in task order and emitted in the old order (PiP, main, cascades).

- Rule / finding: R07 R07 main thread stalls waiting for jobs (F-37a7e1): in the vegetation scenes the classic cull ran one jobs_parallel_for per view (main view, up to 4 sun cascades, the scope PiP), each ending in a tail wait for its slowest cell before the next view could start (0.5 - 0.7 ms of wait:parfor_tail per frame, render:prepare 3.8 ms)
- Code: `src/render/vk_classic.c cull_views / cull_range_multi / cell_tasks / cascade_view (vkc_frame)`
- Metrics (perf_verify): `zone:render:prepare`, `cpu_work`
- Scenes: foliage
- Why fidelity-neutral: cull_instance is unchanged and reads only shared, frame-constant data; every view's tasks are the same cells in the same order with the same output offsets, packed the same way, so each view's instance stream, batch list and the frame's instance buffer layout are identical to the per-view passes whatever the thread count.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: zone:render:prepare 3.258 -> 2.646 (-18.78%, p=0); images pass; state identical

## PF-0010: Wind tile rows on the job system

wind_tile_update is split into begin (window origin, the global bend spring), rows and end (the new origin). wind_game_frame runs the 64 rows with jobs_parallel_for; the pure module keeps the one-call form (tests/test_wind.c).

- Rule / finding: R07 R07 workers idle while the main thread runs serial zones (F-66ec5c): weather_frame -> wind_game_frame -> wind_tile_update evaluated the 64 x 64 wind window (wind_at: gust fronts, three turbulence noises, sources; a damped spring per texel) on the main thread every frame, ~1.6 ms of game_render in every scene (found by zoning game_render on the vegetation map)
- Code: `src/world/wind.c wind_tile_begin / wind_tile_rows / wind_tile_end (wind_tile_update = the three in a row)`, `src/game/game_wind.c wind_game_frame`
- Metrics (perf_verify): `zone:game_render`, `cpu_work`
- Scenes: foliage, devroom
- Why fidelity-neutral: Row j writes only the texels of world row oz + j (64 consecutive rows map to 64 distinct slots) and reads the field and the previous window origin, which only end changes: every texel gets the same wind_at / spring arithmetic, so the uploaded RGBA16F window is identical whatever the thread count.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: zone:game_render 1.594 -> 0.96035 (-39.75%, p=0); images pass; state identical

## PF-0011: Skinned shadow casters culled per sun cascade

After the shadow pass's instance records, each active cascade (and the moon map) gets its own copy of the pass with only the casters whose bounding sphere (hips, character height) touches the cascade's light box and its receiver volume (PF-0006): the same batches in the same order, with the instances kept. The culling-mask path (per-draw shadow draws) and the local light tiles keep the whole pass. The skinning instance buffer grows to 8 x MAX_SKIN_DRAWS records.

- Rule / finding: R11 R11 shadow casters drawn into cascades (F-7b209b): the skinned shadow pass (every zombie's parts, instanced) was drawn whole into each of the 4 sun cascades and the moon map, so a 500-zombie horde was skinned 5 times for the shadows; the raster and classic casters were already culled per tile
- Code: `src/render/render.c cull_skin_shadow / render_end_frame (skin_shadow_k)`, `render.c build_skin_pass (g_skin_shadow_order)`
- Metrics (perf_verify): `scope:shadow_sun`, `gpu_frame`, `scopec:shadow_sun:vs_invocations`
- Scenes: horde500
- Why fidelity-neutral: A shadow record depends on its draw only, so a kept instance draws exactly as before; a dropped one lies outside the tile's light box (it rasterizes no texel of the tile: the tile scissor and the box clip it) or outside the receiver volume (no receiver reads the texels it would write). Depth is a min over the kept fragments: identical.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: scope:shadow_sun 0.3625 -> 0.3092 (-14.70%, p=0); images pass; state identical

## PF-0012: Classic shadow instances drawn front to back from the light

A sun cascade's instance stream is emitted with each batch ordered front to back from the light (32 light-depth buckets over the view's depth range, stable within a bucket) and the batches ordered by mesh size (trees before bushes and grass). Canopies are drawn first, so the alpha-tested fragments behind already written depth fail the early depth test before their shader runs.

- Rule / finding: R02 R02 shadow casters in every cascade (F-b96020) / alpha-test overdraw in the dense vegetation zones: the pine forest interior's sun shadow pass ran 670 M alpha-tested fragment invocations per frame (~40 x the 4 tiles' 16.7 M texels; 14 - 21 ms of GPU at Modern), instances in cell order, small meshes before trees
- Code: `src/render/vk_classic.c emit_view_light_order / cmp_batch_size (cull_views, vkc_frame)`
- Metrics (perf_verify): `scope:shadow_sun`, `scopec:shadow_sun:fs_invocations`, `gpu_frame`
- Scenes: veg_forest_pine, veg_forest_broadleaf
- Why fidelity-neutral: The shadow pass is depth only: every texel ends at the minimum depth of the fragments that pass their alpha test, and which fragments exist, their depths and their alpha tests do not depend on the draw order. The same instances and batches are drawn, so the maps are bit-identical; only the order (and the number of fragment shader invocations) changes.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: scope:shadow_sun 29.5 -> 27.49 (-6.81%, p=1e-05); images pass; state identical

## PF-0013: Grass and small plant shadows in the near cascades only (per-class caster cascade limit)

Every vgeo material carries a sun shadow caster class (ground / small / tree / none) from its asset: grass assets' and model foliage blocks' "shadow_class", the procedural kinds (tuft ground, bush and crop small), the Tree It roles (bush small, flower ground, the rest tree); a mesh takes the least limited class of its materials. Per Engine Fidelity tier (r_shadow_caster_limit, TIER: 1 at High / Modern, 2 at Off .. Medium) assets/render/shadow_casters.json gives each class its last cascade and a distance cap. A sun cascade view skips the classes that do not cast into it (whole placement cells / chunks by their class mask, then per instance) and dithers the limited classes out over the last 30 % of their end distance (the shadow-distance fade path). In PCSS wide mode the limited classes also cast into the next cascade, so their reach stays.

- Rule / finding: R02 R02 shadow casters in every cascade (F-b96020) / alpha-test overdraw in the dense vegetation zones: after PF-0012 the pine forest's sun shadow pass still ran ~660 M alpha-tested fragment invocations per frame (~40 x overdraw of leaf / grass cards in the 4 x 2048^2 tiles, 14 - 30 ms at Modern); 2 cascades instead of 4 cut 58 % but blurred every tree shadow
- Code: `src/render/shadow_casters.c scc_view / scc_alpha (assets/render/shadow_casters.json)`, `src/render/vk_classic.c caster_limits / cascade_view / cull_instance / cell_tasks (cell_scc)`, `src/render/vgeo/vgeo_scene.c mesh_foliage (VGMF_SHADOW_CLASS)`, `src/render/vgeo/grass_asset.c, vgeo_procgen_foliage.c, vgeo_tree.c, src/assets/assets.c (shadow_class)`
- Metrics (perf_verify): `scope:shadow_sun`, `scopec:shadow_sun:fs_invocations`, `gpu_frame`, `cpu_work`, `zone:render:prepare`
- Scenes: veg_forest_pine, veg_forest_broadleaf, veg_forest_edge, veg_meadow, veg_wheat, veg_horde_forest
- **Visual trade-off** (not fidelity neutral): Grass / ground cover casts sun shadows only into cascade 0 (to ~10 m at High / Modern, capped at 15 m on Off .. Medium) and small plants (bushes, ferns, reeds, wheat, small Tree It bushes) into cascades 0 - 1 (~30 m, capped at 30 m), each dithering out over the last 30 % of that distance; before, grass shadows reached 30 m and bushes 60 m. Tree, character, building and terrain shadows and every plant itself are unchanged. Visible only as fewer grass / bush shadow speckles on open ground beyond 10 / 30 m (docs/SHADOWS.md "Caster cascade limits"). Measured (one process, A / B alternating, 2 reps, Modern + Low, RTX 3060 Ti): the meadow's shadow pass -38 % at Modern (fragments -52 %, GPU frame -5 %) and -84 % at Low, horde_foliage -28 % / -47 %, wheat -8 % / -16 %; in the forest interiors fragments only -1 .. -3 % (their ~660 M fragments are tree leaf cards, which keep every cascade), shadow pass -1 .. -9 % at Modern, -2 .. -20 % at Low; CPU work -4 .. -26 % (fewer cascade instances to cull). docs/SHADOWS.md has the table.
- Screenshots: `docs/shadow_screenshots/caster_limit_meadow_1730.jpg`, `docs/shadow_screenshots/caster_limit_meadow_1300.jpg`, `docs/shadow_screenshots/caster_limit_wheat_1730.jpg`, `docs/shadow_screenshots/caster_limit_pine_1300_close.jpg`
- What stays identical: The game state (no rendering decision feeds the simulation: the state hashes must match), every non-foliage shadow, tree shadows in every cascade, every main-view / PiP / local-light draw. The image differs by design where grass / small plant shadows fall beyond their end distance.

- Verification run (DESKTOP-SN4N74U, 2026-10-18): CLOSED: scope:shadow_sun 3.009 -> 2.546 (-15.39%, p=0); images differ (visual trade-off, by design); state identical

## PF-0024: Texture files decoded on the job system at load

Each loader first lists its files, decodes them with jobs_parallel_for (stbi_load plus the loader's own resampling: the VFX fit-to-page halving, the surface layer resample and AO-into-alpha, the grass box downsample), then runs its old serial loop in the old order taking the decoded pixels. The manifest's decode happens before the loop that creates the textures (a path decoded but not consumed, e.g. listed twice, is freed and decoded again in the loop). perf_fix_disable PF-0024 runs every decode inside the loop again; a +set perf_fix_disable on the command line is applied before the startup loads, so the A side of a startup measurement loads the old way. docs/TEXTURE_MIPS.md "Decoding on the jobs".

- Rule / finding: R27 R27 startup critical path (F-0714da): main thread blocked 682 ms on file reads during startup (4864 ms to the first frame); the loads also decoded every texture file one after the other on the main thread: the manifest textures (assets_init), the ~100 VFX atlas files (game_init), and at the first open-world map the 20 surface layers (albedo + AO + nrh + metal masks) and the 8 grass atlases + nta maps
- Code: `src/assets/assets.c assets_init (texture_predecode: predecode_job / texture_decode)`, `src/render/vk_vfx.c render_vfx_set_library (vfx_decode_job)`, `src/render/surface_tex.c render_surfaces_load (surf_load_job)`, `src/render/vk_vgeo.c foliage_build_grass (grass_tex_job)`, `src/core/profiler.c prof_register (+set perf_fix_disable taken before the startup loads)`
- Metrics (perf_verify): `log:assets manifest textures ms`, `log:vfx atlas ms`, `log:surfaces ms`, `log:grass ms`, `startup (prof: startup)`
- Scenes: startup, map vegetation (first load)
- Why fidelity-neutral: The same decode and resampling functions per file with the same arguments, each writing only its own output: the pixels are identical whatever the thread count, and the consumers (texture creation, atlas packing, layer arrays, materials) run in the old order, so texture ids, atlas rects and the uploaded data are identical. Screenshots of 7 views (tools/tests/tex_bc.cfg): A vs B of one repetition never differ more than two repetitions of the same side (identical in 5 views; the wheat and ground views carry run-to-run noise).

- Verification run (DESKTOP-SN4N74U, 2026-10-18): A / B by tools/tests/tex_bc_compare.py --a "+set perf_fix_disable PF-0024" --b "" (two sets of 3 alternating runs, medians): manifest textures 259 -> 134 ms (-39 .. -48%), VFX atlas 483 -> 162 ms (-48 .. -66%), grass 1899 -> 1017 ms (-46%), surfaces 1370 -> 351 ms (-69 .. -74%); startup -9% (within the noise); images identical (within run-to-run noise)

## PF-0035: World, terrain and foliage textures block-compressed (BC7, encoded on the jobs, cached on disk)

Each loader keeps building its RGBA8 mip chain as before (the GPU blit chain's filter on the CPU for world textures, the alpha-weighted / coverage-preserving chains of the vgeo module, the box chains of the terrain and surface arrays); texc_encode encodes every level to BC7 (sRGB for colour, UNORM with the rotated modes for data maps) with jobs_parallel_for over block rows, and the image is created in BC7 with those levels. Results are cached in assets/cache/tex/<key>.bct, key = a 64-bit hash of the levels' texels and sizes, the kind and the codec version. r_texture_compress 0 (dev builds; world textures switch at once, the rest at their next load), perf_fix_disable PF-0035 and a device without BC7 sampling keep RGBA8. r_texture_quality works on BC7 chains as on RGBA8 (dropped levels skipped, lowering copies blocks, raising re-reads the cache).

- Rule / finding: R14 R14 textures stored uncompressed (F-1a9828 .. F-a94519, static): every content texture was RGBA8 (only assets/textures/hazard.ktx2 pre-encoded). The 4096^2 Ch*_1001 maps the rule names are source art (the game loads their 1024 / 2048 skin atlases); the textures actually resident are the terrain (216 MB) and surface arrays, the tree / grass / foliage / impostor module textures and the skins: 581 MB of RGBA8 levels in a vegetation session
- Code: `src/render/bc_codec.c (BC7 modes 1/3/4/5/6/7, BC1/BC3/BC4/BC5; decoders)`, `src/render/tex_compress.c texc_encode / texc_encode_array / texc_mip_chain (jobs, disk cache, r_texture_compress, tex_compress_stats, tex_cache_clear)`, `src/assets/assets.c upload_rgba / assets_reload_textures (world textures, the format switch)`, `src/render/vk_resources.c render_texture_replace(_raw)`, `src/render/terrain_tex.c tt_upload_layers / tt_upload_2d_content, src/render/surface_tex.c upload_layers`, `src/render/vgeo/vgeo_tex.c vg_tex_create (VgTextures.compress), src/render/vk_vgeo.c module_tex_compress`
- Metrics (perf_verify): `vram_usage_mb`, `vram_allocated_mb`, `gpu_frame`, `scope:opaque`, `log:texc sets / encode ms / cache read ms`
- Scenes: devroom, foliage (vegetation map: meadow, ground, pine views)
- **Visual trade-off** (not fidelity neutral): Lossy: every compressed texture is BC7 (8 bits per texel), so texels differ slightly from the RGBA8 source: full frames of the vegetation map 35.5 - 37.1 dB PSNR against RGBA8 (two identical runs: 82 - 100 dB; mean 1.7 - 2.2 levels of 255; grass / leaf card silhouettes where alpha crosses the cut-off, fine grain on terrain, bark and grass), dev room views unchanged (49.3 dB, the same as two identical runs); at 2 x zoom the textures look the same (docs/TEXTURE_MIPS.md "Block compression"). Gains (RTX 3060 Ti): texture level data 581 -> 145 MB, VMA allocated on the vegetation map 1614 -> 1179 MB (-27%), vram_usage 1823 -> 1280 MB (-30%), dev room -31 MB; GPU frame (uncapped, 240 frames x 3 alternating runs) meadow -4.0%, view down at the ground -8.2% (its opaque pass -19%), pine forest -2.6%. Cost: the first load of a texture encodes it (vegetation map ~26 s on 12 threads, once per content change), later loads hash the levels and read the cache (warm loads: manifest +48 ms, trees +13%, grass +10%, surfaces +36%, terrain +6%).
- Screenshots: `docs/tex_bc_screenshots/bc_ground_zoom.jpg`, `docs/tex_bc_screenshots/bc_pine_bark.jpg`, `docs/tex_bc_screenshots/bc_meadow.jpg`, `docs/tex_bc_screenshots/bc_wheat.jpg`, `docs/tex_bc_screenshots/bc_dev_character.jpg`
- What stays identical: The game state (no rendering decision feeds the simulation), the mip chains' filters, the texture sizes and sampling, the r_texture_quality levels; textures that are not compressed (VFX atlas, classic impostors, UI, KTX2 files, runtime data maps). The image differs by design: the BC7 approximation of each texel (codec error bounds in tests/test_bc_codec.c; bitstreams checked bit-exact against Pillow's BC7 decoder by tools/tests/bc_crosscheck.py).

- Verification run (DESKTOP-SN4N74U, 2026-10-18): A / B by tools/tests/tex_bc_compare.py (r_texture_compress 0 vs 1, alternating) and uncapped GPU captures (240 frames x 3 alternating runs): vegetation VMA allocated 1614 -> 1179 MB (-27%), usage 1823 -> 1280 MB (-30%); GPU frame -2.6 .. -8.2%; images differ (visual trade-off, by design: 35.5 - 37.1 dB on vegetation views)
