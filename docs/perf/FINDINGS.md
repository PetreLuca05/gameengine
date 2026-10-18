# Tracked performance findings

Generated from `docs/perf/fixes.json` by `tools/perf_audit/perf_audit.py findings`. A finding is **closed** only when `perf_verify` showed the timing improvement AND every reference image pair passed AND the gameplay state hashes matched (docs/PERF_AUDIT.md "Closing a finding"). Fixes are switched off for A/B runs with `perf_fix_disable <id>`.

| Fix | Finding | Title | Status | Effort | Verified on | Result |
|---|---|---|---|---|---|---|
| PF-0001 | R01 clear-only shadow passes (shadow_sun at night / sun shadows off, shadow_moon by day, shadow_vm without a rig) | Empty shadow maps are not re-cleared every frame | implemented | S | - | - |
| PF-0002 | R26 cvar_find calls per frame (31 / frame in the dev room: a case-insensitive scan of ~700 cvars each) | cvar_find through a case-insensitive hash index | implemented | S | - | - |
| PF-0003 | R08 malloc / free in vg_palettes_record every frame | vgeo palette upload regions without a per-frame malloc | dropped | S | - | - |
| PF-0004 | R08 malloc / free in vg_stream_update every frame | vgeo streaming copy list without a per-frame malloc | dropped | S | - | - |
| PF-0005 | R20 fg:validate (check_graph: pass name copies, dead pass / unused resource analysis, pool scan) every frame although only the fidelity checks and benches read it | Frame graph validation report built on demand | implemented | S | - | - |

## PF-0001: Empty shadow maps are not re-cleared every frame

The sun cascade atlas, the moon map and the viewmodel shadow tile were transitioned, cleared to depth 1 and transitioned back every frame even when nothing is drawn into them (the whole atlas at night, the moon map all day). A map that was cleared and left empty last frame and draws nothing this frame keeps its content: the clear, both barriers and the empty rendering are skipped.

- Rule / finding: R01 R01 clear-only shadow passes (shadow_sun at night / sun shadows off, shadow_moon by day, shadow_vm without a rig)
- Code: `src/render/render.c shadow_clear_skip`, `src/render/vk_device.c (shadow_gen)`
- Metrics (perf_verify): `scopec:shadow_moon:clear_px`, `scope:shadow_moon`, `scopec:shadow_sun:clear_px`, `scope:shadow_sun`, `scope:shadow_vm`, `gpu_frame`
- Scenes: night_lights, devroom
- Why fidelity-neutral: The skipped map holds exactly the clear the next sample expects (depth 1, read as lit), in the same SHADER_READ_ONLY layout; the earlier barrier made that clear visible to every later read in submission order. A re-created map (resize, resolution change) bumps shadow_gen and is cleared again. shadow_drawn bits (validation) are unchanged.


## PF-0002: cvar_find through a case-insensitive hash index

cvar_find scanned every registered cvar with SDL_strcasecmp; 114 call sites, several per frame. The cvars now sit in a 2048-slot open-addressing index keyed by a case-insensitive FNV-1a hash of the name, filled by cvar_reg (cvars are never removed). MAX_CVARS rose from 512 to 768 (the lint counts 782 commands + cvars).

- Rule / finding: R26 R26 cvar_find calls per frame (31 / frame in the dev room: a case-insensitive scan of ~700 cvars each)
- Code: `src/core/cvar.c cvar_find / cvar_reg (index[])`
- Metrics (perf_verify): `zone_all:cvar_find`, `counter:cvar_find`, `cpu_work`
- Scenes: devroom, horde500
- Why fidelity-neutral: Returns the same pointer for every name (the index probes compare with the same SDL_strcasecmp; a name longer than the stored one misses in both paths). tests/test_profiler.c checks hash == scan, case-insensitive, and the miss.


## PF-0003: vgeo palette upload regions without a per-frame malloc

The VkBufferCopy region list of the skinning palette upload was malloc'd and freed every frame (max_slots + 1 entries). It is now allocated once with the palettes. Dropped with the virtualized geometry renderer (the classic renderer has no such path); the id is not reused.

- Rule / finding: R08 R08 malloc / free in vg_palettes_record every frame
- Code: `src/render/vgeo/vgeo_skin.c vg_palettes_record`, `src/render/vgeo/vgeo_skin.h VgPalettes.regions`
- Metrics (perf_verify): `zone_all:vgeo:palettes_record`, `site:vgeo_skin.c`, `cpu_work`
- Scenes: horde500, devroom
- Why fidelity-neutral: vkCmdCopyBuffer copies the regions into the command buffer at record time: the same regions, the same copies, the same data.


## PF-0004: vgeo streaming copy list without a per-frame malloc

The page copy list of the virtualized geometry streamer (2 x pages_per_frame VkBufferCopy) was malloc'd and freed every frame. It is now allocated with the stream. Dropped with the virtualized geometry renderer (the classic renderer has no such path); the id is not reused.

- Rule / finding: R08 R08 malloc / free in vg_stream_update every frame
- Code: `src/render/vgeo/vgeo_stream.c vg_stream_update`, `src/render/vgeo/vgeo_stream.h VgStream.copy_scratch`
- Metrics (perf_verify): `zone_all:vgeo:stream_update`, `site:vgeo_stream.c`, `cpu_work`
- Scenes: foliage, devroom
- Why fidelity-neutral: Same capacity (ppf_cap never changes after init), same contents, consumed by vkCmdCopyBuffer at record time.


## PF-0005: Frame graph validation report built on demand

fg_execute built the validation report every frame. It is now built by the first fg_last_report after the frame (render_graph_report: fidelity_test, bench_fidelity, the profile's fg section); fg_trim and fg_targets_changed build it before they drop pool images.

- Rule / finding: R20 R20 fg:validate (check_graph: pass name copies, dead pass / unused resource analysis, pool scan) every frame although only the fidelity checks and benches read it
- Code: `src/render/graph/fg.c fg_last_report / fg_execute / fg_trim / fg_targets_changed`
- Metrics (perf_verify): `zone:fg:validate`, `cpu_work`
- Scenes: devroom
- Why fidelity-neutral: Validation only: no rendering decision reads it. Between fg_execute and the next fg_begin the graph, the resources and the pool state it reads are unchanged, so an on-demand report is identical to the eager one.

