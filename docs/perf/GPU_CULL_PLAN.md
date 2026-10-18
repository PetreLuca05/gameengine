# GPU culling / Hi-Z / far-tree impostor shadows: implementation plan (items #4, #6, #7)

Synthesized on 2026-10-18 by a design workflow (4 readers, 3 designs, 2 judges) over the classic renderer.
Status: S0 merged (4ede801 + 09993b5). S1, S2, S4 were started in parallel and stopped before any change
(paused by the user); resume with S1 / S2 / S4, then S3, S5, S6, S7 (only if S2's probe says go), S8, S9.
Each step: implement in a worktree (reset to main first), adversarial review (3 lenses), fix, then the
main session merges and measures with perf_verify. Line numbers below refer to the tree of 2026-10-18.

## Architecture

BASE. Checked at HEAD bd4b4b6 plus the uncommitted working tree. Line numbers are the working tree's; the area maps cited 591a12f.
- Brief correction: drawIndirectCount, shaderDrawParameters (gl_DrawID), descriptor indexing and samplerFilterMinmax are NOT enabled. vk_device.c chains a VkPhysicalDeviceVulkan12Features that now sets only timelineSemaphore (PF-0027). multiDrawIndirect and drawIndirectFirstInstance are enabled only when supported. The plan uses none of the missing features. The CPU records fixed command lists and the GPU writes their instanceCount / firstInstance, the vk_vfx.c pattern.
- PF-0027 (async compute) moved the shadow maps to an fg marker. On normal frames the marker sits before the prepass; on async frames it sits after prepass + post_screen_space (render.c:2428-2429, 2477, 2486-2487). Every new cull dispatch is therefore recorded in the pre batch, after vkg_record_foliage / rain_occlusion (render.c:2786-2789) and before build_frame_graph (render.c:2796). That point precedes both marker positions. It runs on the graphics queue.
- ProfFix uses 23 of 32 bits (profiler.h:54-81). This plan adds 6, for 29.
- Modern shadow tiles are 4096^2 (modern.json:31, shs_res_texels shadow_settings.c:95-98), not the documented 2048^2. S2 measures this.
- The plan assumes the uncommitted work lands first:
  - ShadowHold stores Lfit;
  - receiver planes are off for staggered far tiles;
  - draw_view pushes the PF-0029 shadow mip bias (vk_classic.c ~1547-1554).
  S3's hold-key bit and S4's section helper build on it.

PF IDS
- Neutral ids go after PFX_ASYNC_COMPUTE and before PFX_TRADEOFF_FIRST. Their k_fix_ids strings go after "PF-0027" (profiler.c:77-78):
  - PF-0017 PFX_CLASSIC_CULL_FAST;
  - PF-0018 PFX_CLASSIC_GPU_CULL_SHADOW;
  - PF-0019 PFX_CLASSIC_GPU_CULL_MAIN;
  - PF-0031 PFX_OCCLUSION_CLASSIC;
  - PF-0032 PFX_OCCLUSION_SKIN.
- The one trade-off id is appended after PFX_ANIM_SHARED_POSE: PF-0030 PFX_TREE_SHADOW_IMPOSTOR.
- #4's gap fill and its last-cascade substitution share PF-0030. A gap-fill-only id adds GPU time, so it could never close perf_verify (the 2 % GPU guard); merging them also frees PF-0032 for zombies.
- Ungated steps: S0 (bug fixes and capability flags), S2 (dev tools only) and S4 (a data-layout refactor that registration-time code cannot A/B in one process). None of them changes a perf decision. They are verified build against build, plus the existing perf_verify runs.

END-STATE FRAME
1. Scope 'uploads' (render.c:2709-2711): vkr_record_uploads, vkam_record, then vkcc_record_uploads. It applies the mirror patches: dst-offset vkCmdCopyBuffer from a staging ring mapped per frame slot, then a TRANSFER -> COMPUTE barrier.
2. render:prepare. compute_matrices sets the PF-0028 holds and the PF-0006 planes. Then vkc_frame (vk_classic.c:1198) builds one ClCull per view as today: PiP (when render_scope_cull is true), main, then the redrawn cascades.
   - CPU views use the PF-0017 fast cull_views, or today's paths when the gates are off.
   - GPU views run cell_tasks only, then build:
     - 64-instance work chunks with deterministic candidate bases;
     - the mesh set M_v, from per-chunk mesh bitsets;
     - a slot table: the full LOD superset x fade, plus impostor slots;
     - command runs per (pipeline group, mesh_drawn, material), with static args from record-time lod_resident_pick;
     - a ClCullGpu UBO;
     - a part bound known on the CPU: 2 or 3 x candidates.
3. Pre batch: vfx_sim, foliage_wind, rain_occlusion, then the new scope 'classic_cull':
   - K1 cull: one 32 B decision record written once per candidate. Under PF-0031, the pass-1 test against last frame's pyramid.
   - K2 stable scan.
   - K3 expand, in task order.
   - K4 stable LSD radix sort on (view, slot, bucket).
   - K5 emit 48 B ClInst records in today's layout.
   - K6 indirect args.
   - K7 stats to the readback.
   - skin_occl early pass (PF-0032).
   - Then a barrier to DRAW_INDIRECT | VERTEX_ATTRIBUTE_INPUT.
4. Frame graph:
   - [shadows marker];
   - prepass: DrawItems, classic early, skinned early;
   - occl_hiz: MAX pyramid, kept as next frame's history;
   - occl_cull_late;
   - prepass_late: depth LOAD;
   - post_screen_space;
   - [shadows marker on async frames];
   - opaque: classic union args at depth EQUAL;
   - opaque_dynamic: skinned union;
   - scope_pip: the GPU PiP view, no occlusion.
   Sun tiles: vkc_draw_shadow -> draw_view_gpu for GPU cascades, including the CP_IMP_SHADOW runs. Local light tiles are unchanged: CPU cull at record time into C.inst[slot].
5. A readback 2 frames late feeds classic_stats, R->stats and count_view. visibility_probe and lod_dolly request a CPU reference cull of the same frame. classic_fade_probe stays on the CPU.

KEY RULES
- Instance data: ClInst (48 B, vk_classic.c:85) and the instance-rate binding 1 are unchanged, so classic_vert.glsl and classic_impostor.vert are unchanged. firstInstance carries the ranges.
- Determinism:
  - decision records are written once at fixed indices, so count and scatter cannot diverge (no recompute);
  - the stable scan and radix sort reproduce the CPU's per-slot order (bucket, then task order);
  - no atomic decides order. Atomics are used only for exact min / max.
- Capacity:
  - the CPU-known bound sizes the output before recording;
  - writes are clamped, with an overflow counter (must be 0) and a slot-miss counter (must be 0);
  - above 128 MB, that frame uses the CPU path.
- Math:
  - classic_cull_math.h (C) and classic_cull_math.glsl (GLSL) use the same operation order, and every GLSL float is `precise`;
  - C keeps the CRT log2f / sqrtf, so the A side is bit-unchanged;
  - the GPU path is classed near-identical and proven by classic_cull_check, which compares coverage, not keys;
  - escalation, if the check shows frequent differences: emulated correctly rounded division / sqrt.
- Hi-Z:
  - a MAX pyramid of pass-1 depth (a subset of the final occluders);
  - the late test uses this frame's jittered matrix + 1 texel;
  - conservative bounds: the sphere including max_wpo, and impostor parts dilated;
  - never applied to PiP, phone or shadow views;
  - a stale history only moves work between passes, never pixels.
- #4:
  - last cascade only;
  - iw = 1 - f_sd * f_near replaces the shadow-distance return and alpha factor;
  - complementary dither through emit_part;
  - flat quad-plane depth, so the hardware depth bias still applies, oriented on row 2 of the cascade's own (held) matrix;
  - no bake change;
  - impostor receivers get a self-skip, and the hold key gets a fix bit.

MOLTENVK / CAPABILITY FALLBACK
- R->gpu_cull_ok = has_first_instance && has_push_descriptor && (!R->portability || --gpu-cull). When false, PF-0018 / 0019 / 0031 fall back to the CPU path, sped up by PF-0017, with identical output.
- Without multiDrawIndirect: vkCmdDrawIndexedIndirect with drawCount 1 per sub-draw, never vkCmdDrawIndexed.
- R->occl_ok = !R->portability || --occlusion gates PF-0031 / 0032, because of the TBDR depth + G-buffer store / load round trip.
- PF-0030 needs no device feature and runs everywhere.
- No new shader uses drawIndirectCount, gl_DrawID, descriptor indexing, min/max samplers, int64, subgroups, scalar / nonuniform layout or 16-bit types. Shared memory stays at 16 KB or less, and msl_check.py gates every shader in CI.
- S9 measures Apple GPUs before any portability default changes.

EXPECTATIONS (RTX 3060 Ti)
- Modern forests are GPU-bound (veg_forest_pine: GPU 35.7 ms vs CPU 4.5 ms, AUDIT_2026-10-18.md:28).
- #6 gives about 0 ms of frame time there and costs +0.1-0.3 ms GPU. Its CPU win (about -0.5..-1.5 ms main thread and most of the classic worker time) shows in the CPU-bound Low hordes, on Mac (through PF-0017), and as workers freed for animation.
- #4 is the only GPU-time lever: net about -0.4..-1.5 ms on cascade-3 redraw frames, and it closes the 105-150 m gap where trees cast no shadow.
- #7 is gated on the probe: likely under 1 ms in porous forests, 1-3 ms behind terrain and in towns.
- The dominant cost stays outside all three items: leaf-card fill in the near cascades, about 70 % of shadow_sun.

JUDGE ERRORS FIXED
- lod_iv_pack empty-interval sentinel: S0.
- The normal-atlas alpha is not free (vg_tex_build_mips weights rgb by alpha, vgeo_tex.c:52-58): no bake change.
- gl_FragDepth bypasses the hardware depth bias: flat quad depth.
- Registration-time gates: avoided.
- Outputs sized from readback: replaced by the CPU bound.
- COUNT / EMIT double evaluation: replaced by decision records.
- The 2048-slot key cap: 32-bit keys, with the pass count known on the CPU.
- Fade merge: only masked materials in pass 2, only clip groups in the main view.
- Impostor quads outside the culling sphere: dilated Hi-Z bound.
- Superset without miss detection: full LOD superset of M_v, plus a miss counter.
- Parallel emit races: per-view scratch, ranges reserved first.
- hiz corner fold: done in occl_pyramid.comp.
- HEAD drift (async queue, marker, 23 ids): re-based.
- alpha = casc_a: only the shadow-distance factor is replaced.
- The !MDI fallback: indirect draws with drawCount 1.
- Self-shadow in the substitution band: push parameter plus a screenshot gate.
- Cascade 3 interval 4: dropped.
- The check compares coverage, not keys.

## Steps

### S0: Groundwork for #4, #6, #7: capabilities, two bug fixes, the shared cull math

- PF id: none (bug fixes and a pure refactor; precedent: the jobs race fix cfdf088 landed without an id)
- Depends on: -
- Files: src/render/vk_device.c, src/render/vk_internal.h, src/render/vk_classic.c, src/render/vk_vgeo.c, src/render/lod_fade.c, src/render/lod_fade.h, src/render/classic_cull_math.h, tests/test_lod.c, tests/test_classic_cull.c, CMakeLists.txt

(a) Capability flags:
- store R->has_mdi (multiDrawIndirect) and R->has_first_instance (drawIndirectFirstInstance) where vk_device.c enables them, next to has_push_descriptor in RenderState, and log one line;
- derive R->gpu_cull_ok = has_first_instance && has_push_descriptor && (!R->portability || has_arg("--gpu-cull"));
- derive R->occl_ok = !R->portability || has_arg("--occlusion") (the --no-async-compute precedent);
- request no new features.

(b) Stale whole-cell limits:
- move the per-chunk pass of vkc_chunk_loaded (vk_classic.c:593) into cl_chunk_limits(s, h);
- add vkc_scene_committed(s), which reruns it for every used chunk, called after vgeo_scene_commit in foliage_commit (vk_vgeo.c:526-529) and at vk_vgeo.c:350.
Today the only caller is vk_vgeo.c:1113. So after grass_update_tier (vk_vgeo.c:881) or render_foliage_wind_set, the cell_fade / cell_shadow / cell_scc values and the ClChunk bounds stay stale, and cell_tasks (vk_classic.c:885-888) drops whole cells too early.

(c) Empty-interval sentinel:
- lod_iv_pack(lo, hi) returns 0 when both ends round to 0 (lod_fade.c:193-196), and 0 means 'whole coverage' (lod_iv_kept; cl_faded, classic_common.glsl:113-118);
- so the emits at vk_classic.c:798-801 draw a whole extra impostor or coarser LOD when iw or cw is in (0, 1/512);
- add lod_iv_empty(lo, hi) (the quantized ends are equal) and emit no part for an empty interval. This also skips harmless empty a == b > 0 parts.

(d) Pure refactor:
- move the arithmetic of cull_instance (vk_classic.c:742-828) and cl_foliage_alpha (:714-723) into static inline functions in src/render/classic_cull_math.h, with the same expression order;
- cull_instance calls them, still with the CRT log2f / sqrtf;
- this file is the template for the GLSL twin used from S5 on.

Effort: 2-3 days.

**Fidelity:** Bug fix, not gated.
- (a) and (d): bit-identical.
- (b): identical in every steady state. It changes output only after a runtime grass tier or wind change, where it restores what the code intends.
- (c): removes double LOD / impostor draws for instances in a 1/512-wide band. Near-identical: a few pixels, inside the perf_verify image gate.

**Verify:** Build with .toolchain/b.sh, then run ctest:
- test_lod: a fine sweep of iw and cw over (0, 1/256] in 1e-6 steps, plus random values. The parts' coverage sums to 1 at every one of the 256 thresholds, and no empty part is emitted.
- test_classic_cull: the factored functions give the same bits as an in-test copy of the old cull_instance on 1M random instances and views.

Other checks:
- tools/tests/lod_*.cfg + lod_check.py.
- The CI fidelity image check (perf_fix_disable all), build against build, stays inside the A-vs-A' envelope.
- Script on map vegetation: change Engine Fidelity mid-run, then compare the visibility_probe grass counts with a fresh map load. They must be equal.
- --validation run.
- The capability line appears in the log, including on the macOS CI job.

### S1: #6 CPU side: fast classic cull path. It is also the A side and fallback of every GPU path, and the Mac path.

- PF id: PF-0017 PFX_CLASSIC_CULL_FAST (neutral)
- Depends on: S0
- Files: src/core/profiler.h, src/core/profiler.c, src/render/vk_classic.c, src/render/classic_cull_math.h, tests/test_classic_cull.c, docs/perf/fixes.json

Applies to the PF-0009 batched path (cull_views, vk_classic.c:950-994). PF-0009 off keeps cull_cells.

(i) cell_tasks (:853-896) for all views runs in one jobs_parallel_for over the views:
- each view writes its own task array;
- the arrays are concatenated in today's view order (PiP, main, cascades), so task order and offsets equal today's.

(ii) Per-task inside bit:
- set when the cell or chunk box lies inside the 6 planes, the nrcv receiver planes and the light sphere, with an epsilon;
- cull_instance then skips sphere_in, the rcv test and the sphere test;
- valid because the VgCell bounds contain every culling sphere, WPO margin included (vgeo_scene.c:270-278, 378-393), and S0 keeps the ClChunk boxes fresh.

(iii) Commuting early rejects go first:
- flags and layers, then pivot distance, the shadow-distance cut, PF-0013 scc_alpha and the foliage alpha;
- pos is a raw f32 in VgInstance, so none of these needs a decode;
- decode, sphere and plane tests come after. The outcome is identical because every reject is pure.

(iv) Packing by prefix sum instead of memmove (:975-985).

(v) emit_view / emit_view_depth_order run per view on the jobs:
- they move off the globals: C.tmp / tmp_key / tmp_n, swapped by cull_views (:986-992), the static bucket / count2, and the shared C.count (:1019-1124);
- key histograms become sparse: lists of touched keys instead of memsetting and scanning mesh_cap<<4 (x 32) entries;
- every inst_alloc range is reserved in the old view order before the parallel section, because inst_alloc (:999-1015) grows by realloc and copy.

(vi) Each view scatters into a cached array, then one sequential memcpy goes into the mapped stream.

Also: classic_stats prints an FNV-1a hash of each view's batch list and ClInst stream. S5 and S6 reuse it for identity checks.

Gain:
- -0.3..-0.7 ms main thread (classic:cull_views is 0.54-1.87 ms today);
- -2..-5 worker-ms;
- frame time improves only where CPU-bound (veg_horde_forest at Low: CPU 9.98 vs GPU 6.53 ms) and on MoltenVK.

Effort: 2-3 days.

**Fidelity:** IDENTICAL (bit-exact): same tests and float operations, same batch lists and instance bytes.

**Verify:** - perf_verify PF-0017 on devroom, foliage, veg_forest_pine and veg_horde_forest (Low). Metrics: zone:classic:cull_views, zone:render:prepare, cpu_work. Image pairs should be pixel-identical; state hashes identical.
- The classic_stats stream hashes are equal with PF-0017 on and off, and with the jobs forced to 1 thread and to N threads.
- test_classic_cull: on random cells the inside-skip gives exactly the output of the full tests.
- --validation run.

### S2: Measurement for #4 and #7: per-cascade shadow cost, tile size, and an occlusion probe that gives #7 its go / no-go

- PF id: none (dev tools only; normal frames unchanged)
- Depends on: S0
- Files: src/render/render.c, src/render/vk_classic.c, src/render/vk_occlusion.c, src/render/vk_occlusion.h, shaders/occl_pyramid.comp, shaders/occl_probe.comp, assets/ui/command_categories.json, docs/perf/FINDINGS.md

(a) Dev command shadow_split_scopes <frames>:
- records the sun atlas as one rendering instance per redrawn tile, with scopes shadow_sun_c0..c3;
- keeps the PF-0028 LOAD / per-tile clear logic (render.c ~2803-2840);
- classic_stats prints parts and tris per cascade.
It settles 4096 vs 2048 tiles and cascade 3's real share, which sizes #4. The c0..c3 scopes are measurement-only; perf_audit rules key on shadow_sun.

(b) Occlusion pyramid module, new src/render/vk_occlusion.c:
- a renderer-owned R32F image, ceil(render/2) with a full mip chain, STORAGE | SAMPLED;
- created and dropped with the render targets, imported each frame with fg_import + layout_io. Renderer-owned, not an fg_history, so the pre-batch cull can sample last frame's copy.
- shaders/occl_pyramid.comp: the MAX twin of shaders/post/hiz.comp that also folds the odd x odd corner (hiz.comp:24-26 skips it). Shared-memory reduction of 16 KB or less, no subgroups, its own push-descriptor layout (the vk_alpha_mask.c pattern; the post layout has only 2 SSBOs).
- fg pass 'occl_hiz' right after 'prepass' (render.c:2477-2481) and before post_screen_space (:2486), so water (vk_water.c writes depth later) never occludes.
- The name falls outside fid_pass_is_post (fidelity.c:778-786).

(c) Dev command occlusion_probe <frames>:
- schedules occl_hiz plus a probe compute;
- the probe tests the main-view ClInst stream (C.inst[slot] gets STORAGE usage, inst_alloc vk_classic.c:1004) and the skinned pass-0 spheres (build_skin_pass, render.c:1037) against the pyramid, using fd->view_proj plus 1 texel;
- it counts occluded parts and tris per class (tree, grass, bush, prop, impostor) and occluded zombies;
- readback 2 frames later, logged per scene.
Run it on devroom, horde500, veg_forest_pine, veg_forest_broadleaf, veg_forest_edge, veg_overlook and veg_horde_forest.

Go criterion for S7 / S8: occluded tris x 2 passes are worth at least ~0.5 ms at the scene's measured triangle rate.

Effort: 3-4 days.

**Fidelity:** IDENTICAL: nothing changes unless a dev command is active.

**Verify:** - Build against build: the fidelity image check is identical.
- test_fidelity graph checks: occl_hiz is scheduled only while the probe reads it, so there is no dead pass, including at Fidelity Off.
- --validation while the probe runs (layout of the imported pyramid across frames).
- msl_check.py.
- Record in docs/perf: the tile size per scene; per-cascade ms, tris and scopec fs_invocations; the occluded share per class. Rejections behind the dev room hall should be well above 0, and about 0 in veg_meadow.

### S3: #4 Far-tree impostor shadows in the last cascade: closes the 105-150 m gap and substitutes mesh casters beyond the cascade-3 blend

- PF id: PF-0030 PFX_TREE_SHADOW_IMPOSTOR (trade-off, after PFX_TRADEOFF_FIRST)
- Depends on: S0, S2
- Files: src/core/profiler.h, src/core/profiler.c, src/render/vk_classic.c, src/render/render.c, src/render/lod_fade.c, src/render/lod_fade.h, assets/render/lod.json, shaders/classic_impostor_shadow.vert, shaders/classic_impostor_shadow.frag, shaders/classic_impostor.glsl, shaders/classic_impostor_lit.frag, shaders/lit_body.glsl, tests/test_lod.c, docs/SHADOWS.md, docs/LOD.md, docs/perf/fixes.json, docs/shadow_screenshots

Built on the CPU path first; S5's GPU twin includes the same rule from day one.

CULL
- ClCull gains imp_shadow, imp_x, near_band, end_m, end_band and push.
- cascade_view (vk_classic.c:1175) sets imp_shadow only for the last active cascade, k = ncasc-1, with ncasc >= 2.
- imp_x = that cascade's blend start, split[ncasc-2] * (1 - border), about 63 m at the locked 150 m. It tracks the fidelity tiers.
- In cull_instance (:770-801), for imp_shadow views and meshes with cm->imp_set, coarse_tris >= CL_IMPOSTOR_TRIS, class SCC_TREE and shadow_max_dist > 0:
  - f_sd = today's shadow-distance factor (0 beyond sd);
  - f_near = clamp01((imp_x - dist) / near_band);
  - iw = 1 - f_sd * f_near. This replaces both the return at :773 and the alpha multiply at :774;
  - beyond end_m: return. alpha is multiplied by the end band.
- casc_a and the foliage density alpha stay in alpha; only the shadow-distance factor is replaced. Density-thinned trees therefore never cast an impostor.
- The existing part code (:797-801) then emits impostor [0, iw), coarser [iw, mid), finer [mid, 1) through emit_part. The key, the fade bit and the PF-0012 buckets come for free, and with S0 there are no empty parts.

CELLS
- vkc_chunk_loaded and vkc_scene_committed also store C.cell_imp_shadow[ci] and ClChunk.imp_shadow = end_m for cells that hold such a mesh.
- For imp_shadow views, cell_tasks (:856-889) uses max(cell_shadow * lim_k, cell_imp_shadow). A cell_shadow of 0 keeps its 'unlimited' meaning.
- The tree class has no PF-0013 end (shadow_casters.json: tree cascade -1).

DRAW
- New pipeline CP_IMP_SHADOW = cl_pipeline("classic_impostor_shadow.vert", "classic_impostor_shadow.frag", impostor, shadow, two-sided, LESS_OR_EQUAL, depth write, 0 colours). Added to the enum at :105 and created next to :348.
- Created unconditionally, together with each mesh's fitted radius: the radial bound of coverage >= 0.5 over all frames, computed once in vkc_register_mesh (:570-583). There is no registration-time gate.
- draw_view pass 2: impostor batches are no longer skipped at :1492, and an impostor loop runs where pass 2 returns today (:1566).
- Bindings: set 3 = cm->imp_set.
- Push constants: model[0] = impostor centre + radius; model[1].x = grid; m[5] = fitted radius; m[6] = cascade texels per metre (tile / (2 * shadow_radius[k])); m[7] = push; m[15] = 1; flags[3] = cascade.
- One vkCmdDraw(6, count, 0, first) per batch.

SHADERS
- Vertex:
  - M = shadow_pass_vp(pc.flags.w) (common.glsl:190-196);
  - light axis a = normalize(row 2 of M), from the cascade's own, possibly held, matrix, never frame.sun_dir (Lfit clamp, PF-0028 holds);
  - a light-facing quad at the rotated impostor centre, moved push * h along a (h = imp_radius * sc);
  - half size = h * fitted radius / cos(frame spacing);
  - flat quad-plane depth, so draw_sun_casters' dynamic depth bias applies and early-z stays.
- Fragment:
  - cl_impostor_cover, the alpha-only twin of cl_impostor_sample (classic_impostor.glsl:29-61), taking the 3 frames around the inverse-rotated light axis;
  - mip from atlas texels per metre / cascade texels per metre (not frame.proj);
  - discard when cover < 0.5 or cl_faded.
- No gl_FragDepth and no bake change.

RECEIVERS
- classic_impostor_lit.frag (main and PiP impostors, v_world = quad point) offsets only its shadow lookup (lit_body.glsl:265) towards the light, by self_skip * h with self_skip >= 1 - push.
- Push-gated, so with the fix off it compiles to plain v_world.
- Mesh trees between imp_x and their impostor switch receive a planar self-shadow beyond centre + push * h. push 0.5 keeps the noon ground shadow under the crown. This is judged by screenshots.

PARAMETERS
- lod.json "impostor_shadow": {near_band 10, end_m 180, end_band 0.1, push 0.5, self_skip 0.6}, parsed in lod_fade.c. Locked; no new cvar.
- The PF-0028 hold key (render.c:1528: ncasc | close_fit<<3 | tile<<4 | shadow_gen<<20) gets the fix bit at bit 18, so a toggle redraws held tiles.

GAIN
- Visual: trees cast shadows from 105 to 180 m. Cascade 3 also feeds the froxels (vol_inject.comp:26).
- Performance: about 64 % of today's 105 m caster disk is beyond imp_x, and each of those trees becomes one quad layer instead of 10-40x card overdraw.
- Estimate: net -0.4..-1.5 ms on cascade-3 redraw frames in forest interiors, more at forest edges and overlooks. Cascades 0-2 are unchanged.

Effort: 6-8 days.

**Fidelity:** TRADE-OFF. The A side reproduces today's casters exactly.

B changes cascade 3 only:
- trees beyond about 53-63 m cast 64 px-frame impostor silhouettes: about 37 cm atlas texels against 6.6 cm cascade-3 texels at 4096 tiles;
- those shadows have no wind sway;
- the 3-frame blend ghosts silhouettes;
- a planar self-shadow cut appears on mesh crowns in the substitution band;
- god rays from trees beyond 63 m change;
- trees now cast at 105-180 m.

**Verify:** - test_lod: the impostor-shadow handoff, near band and end band give a coverage sum of 1 at all 256 thresholds.
- perf_verify PF-0030 on veg_forest_pine, veg_forest_broadleaf, veg_forest_edge and veg_overlook:
  - timing: scope:shadow_sun, scopec:shadow_sun:fs_invocations, gpu_frame;
  - state identical;
  - images recorded, since this is a trade-off.
- The A side with perf_fix_disable PF-0030 is pixel-identical to the pre-S3 build.
- Screenshots in docs/shadow_screenshots:
  - forest edge, overlook, and the shadow reference corner (-50, 0, -11);
  - sun at 10, 30 and 60 degrees, and behind the camera (impostor self-skip);
  - wind on, PF-0028 held frames, froxel god rays with fog;
  - r_lod_fade_debug showing no double or missing shadow across the 53-63 m and 89-105 m bands.
- shadow_check_sampled logs no error.
- tools/tests/shadow_ref.py and shadow_stability.py.
- msl_check.py.
- --validation.

### S4: #6 enabler: per-mesh LOD arena, so one indirect call covers every (LOD, fade) sub-draw of a mesh / material run

- PF id: none (data-layout refactor: classic meshes register once per process, so a registration-time gate cannot be A/B'd inside one perf_verify process)
- Depends on: S0
- Files: src/render/vk_classic.c, src/render/vk_resources.c, docs/CLASSIC_RENDERER.md

- One GpuMesh per ClMesh holds every LOD, instead of one render_mesh_create per LOD (vk_resources.c:415-439; vk_classic.c:436 / 460).
- ClLod gains vbase and ibase. Indices stay relative to their own LOD and are drawn with vertexOffset = vbase.
- Decoded DAG cuts are concatenated at registration (:547-549).
- Exported LODs are all built together at first use (build_lods :1127-1134). The finer-LOD prefetch becomes moot.
- PF-0016 twins get the same layout.
- draw_view and vkc_draw_local_shadow bind VB / IB once per drawn mesh and call vkCmdDrawIndexed(index_count, count, ibase + first_index, vbase, first), in the same order as today.
- lod_ready, ready_frame + 2 and lod_resident_pick are unchanged.
- The per-section bind / push block of draw_view (vk_classic.c:1508-1557, including the PF-0029 shadow mip-bias push) moves into cl_section_state(), so draw_view_gpu reuses exactly the same state.

Cost: exported meshes keep their finer LODs resident. Record the classic_stats GPU MB, which is 55-93 MB today.

Effort: 2-3 days.

**Fidelity:** IDENTICAL in steady state: same vertices, indices and draw sequence. Streaming transients differ only in that a resident LOD is drawn earlier, never later.

**Verify:** - Fidelity image check against the S3 build, identical in devroom, vegetation and wateryard.
- tools/tests/lod_*.cfg + lod_check.py.
- perf_verify PF-0009, PF-0012, PF-0015 and PF-0016 still close: image and state unchanged.
- classic_stats GPU MB before and after, recorded in CLASSIC_RENDERER.md.
- --validation.

### S5: #6 Persistent GPU instance mirror, plus GPU culling and indirect draws for the sun cascades

- PF id: PF-0018 PFX_CLASSIC_GPU_CULL_SHADOW (neutral)
- Depends on: S1, S3, S4
- Files: src/core/profiler.h, src/core/profiler.c, src/render/vk_classic_cull.c, src/render/vk_classic_cull.h, src/render/vk_classic.c, src/render/vk_vgeo.c, src/render/render.c, shaders/classic_cull.comp, shaders/classic_cull_scan.comp, shaders/classic_cull_sort.comp, shaders/classic_cull_math.glsl, assets/ui/command_categories.json, docs/perf/fixes.json, docs/perf/baseline.json, docs/CLASSIC_RENDERER.md

MIRROR (new src/render/vk_classic_cull.c, vkcc_*)
- Maintained whenever R->gpu_cull_ok, so A/B toggles act at once.
- ClGpuInst records, 48 B, in a device-local STORAGE | TRANSFER_DST buffer:
  - pos.xyz and sc, then the quaternion, decoded on the CPU by vgeo_instance_decode;
  - mesh | iflags<<16, flags_phase, cell.
- Indexed like the scene: static i, then instance_count + placement i (vgeo_scene.h:19-22).
- Grows to the high-water mark (copy + vkr_defer_buffer).
- Writers:
  - static region: vkc_scene_committed;
  - placement ranges: vkc_chunk_loaded. A reused slot is rewritten before any cull in the same frame.
- Per-mesh table: VgMesh centre, radius, max_wpo, fades, shadow_max_dist, fflags, impostor scale; plus ClMesh lod_error[4], lod_count, big (>= 256 tris), imp_set, PF-0030 capability. Re-uploaded on registration, wind change and grass tier change.
- Patches come from a staging ring per frame slot, recorded in 'uploads'. stage_buffer_copy cannot be used: it copies to offset 0 and its barrier reaches only vertex / index input (vk_resources.c:405-413, 1055-1063).
- If the ring budget is exceeded, that frame's GPU views take the CPU path.
- The dormant vgeo_scene upload path is NOT revived (vkDeviceWaitIdle at commit, page pool, per-call mallocs).

CPU, PER REDRAWN CASCADE
- Held cascades get no work.
- cell_tasks unchanged, parallel under PF-0017. Tasks are split into work chunks of at most 64 instances: {view, mirror first, n, cand_base}.
- M_v = OR of the per-chunk and per-static-cell mesh bitsets, built in vkc_chunk_loaded / vkc_scene_committed and rebuilt when mesh_cap grows.
- Slots: every (mesh in M_v, LOD from the era-first LOD to lod_count-1, fade 0 or 1), plus impostor slots for PF-0030 meshes in the imp_shadow cascade. This is the full LOD superset, so a miss is impossible by construction.
- Commands: (slot, section of the drawn LOD), where the drawn LOD = lod_resident_pick on mesh_drawn at record time, exactly as draw_view does at :1492-1503.
- Commands are grouped into runs per (group, mesh_drawn, material), using the group rule at :1513-1515:
  - a masked material's fade 0 and fade 1 share CP_SHADOW_MASKED and merge with m[15] = 1;
  - an unmasked material's fade 0 (CP_SHADOW) and fade 1 (CP_SHADOW_MASKED) are separate runs.
- Runs are ordered by mesh radius descending, then mesh (cmp_batch_size :1060).
- Static args per command: indexCount, firstIndex = ibase + first_index, vertexOffset = vbase.
- ClCullGpu (std140): ClCull (:691-706), plus the depth row (row 2 of shadow_vp_ndc[k], only when PF-0012 is on), plus the PF-0030 fields.
- Capacity: parts <= 2 x candidates (3 in the imp_shadow cascade), known before recording. The single device-local output buffer (VERTEX | STORAGE) is grown before recording. One buffer is enough: the graphics queue and the chain's first barrier order it after the previous frame's draws. Above 128 MB the frame uses the CPU path.

GPU, NEW SCOPE 'classic_cull'
Recorded in the pre batch after rain_occlusion (render.c:2787-2789) and before build_frame_graph (:2796), on the graphics queue only.
1. Barrier: DRAW_INDIRECT | VERTEX_ATTRIBUTE_INPUT | COMPUTE -> COMPUTE | TRANSFER. Fill the counters.
2. K1 classic_cull.comp, CULL mode, one thread per candidate:
   - the GLSL twin in classic_cull_math.glsl (every float `precise`, the operation order of classic_cull_math.h);
   - writes a 32 B decision record once, at the candidate's index: {mirror index, lod | part mask, alpha, sd = sc * shrink, iv[3], z};
   - writes the part count;
   - per depth-ordered view, z min / max through ordered-uint atomics.
3. K2: stable scan of the part counts.
4. K3: expand the parts in task order, with key = view | slot<<5 | bucket. 32 buckets as in emit_view_depth_order (:1080-1093), or bucket 0 when PF-0012 is off.
5. K4: stable LSD radix sort, 8-bit digits, with the number of passes taken from the key width the CPU knows. 32-bit keys, so there is no slot cap.
6. K5: emit ClInst {pos, sd; quat; phase, alpha, iv, debug} exactly as emit_part (:728-738). Slot first / count come from run boundaries.
7. K6: args. Each command gets instanceCount and firstInstance through its cmd -> slot entry.
8. K7: stats into a readback buffer per slot, read when the slot comes round (the vk_vfx.c pattern).
9. Writes are clamped, with an overflow counter (must be 0) and a slot-miss counter (must be 0).
10. Barrier: COMPUTE -> DRAW_INDIRECT (INDIRECT_COMMAND_READ) | VERTEX_ATTRIBUTE_INPUT.
The shaders use no subgroup, int64, scalar, nonuniform or 16-bit features, and at most 16 KB of shared memory.

DRAW
- vkc_draw_shadow (vk_classic.c:1607) -> draw_view_gpu, run by run:
  - cl_section_state() binds and pushes;
  - then vkCmdDrawIndexedIndirect(args, off, n_sub, 20) when R->has_mdi, otherwise n_sub calls with drawCount 1.
- CP_IMP_SHADOW runs use vkCmdDrawIndirect(args, off, 1, 16).
- Binding 1 is chosen per view by ClView.gpu. Local light tiles keep C.inst[slot] (vk_classic.c:1614).

STATS
- The readback feeds C.view[].instances / lod_n / tris_shadow, the R->stats triangles and classic_stats ('gpu, 2 frames late').
- VKP counts each indirect call as one draw (vk_prof.h:39-45). Noted in fixes.json; baseline.json is re-recorded.

CHECK: new dev command classic_cull_check <frames>, with a description and a category.
- Mirror mode: byte-compares the mirror against freshly encoded CPU tables.
- Cull mode: per GPU view, compares the GPU part list with cull_views run on the same ClCull in the same frame:
  - parts are matched by (instance, LOD or impostor);
  - coverage (alpha, iv) is compared, not keys, because a fade-class flip at alpha = 1 is visually neutral;
  - it reports missing and extra parts, coverage differences, misses and overflow.

ESCALATION, only if coverage differences are frequent: correctly rounded division / sqrt emulation in the twin.

GAIN
- CPU: the cascades are about 46 % of parts (19.9k of 43.3k in cs.log) and test up to 16 receiver planes. Expect about -0.5..-1 ms main thread and -40..-60 % classic worker time, plus about -0.95 MB/frame of write-combined stores.
- GPU: +0.1..0.3 ms.

Effort: 10-14 days.

**Fidelity:** NEAR-IDENTICAL (neutral; inside the perf_verify image gate).
- Cascade maps are depth-only and order-independent.
- The instance set is identical, except culling spheres exactly tangent to a plane, which have no visible pixels.
- GLSL division / sqrt / log2 are not correctly rounded, so about 1e-5 of parts move a dither-interval edge by 1/256: a few shadow texels.
- Claimed IDENTICAL only if classic_cull_check reports 0 coverage differences.

**Verify:** - classic_cull_check, mirror mode: byte-equal after map loads, the chunk-streaming tour (devroom_veg_tour.cfg) and a grass-tier commit.
- classic_cull_check, cull mode, on every redrawn cascade in devroom, vegetation, devroom_vegetation and wateryard:
  - 0 missing or extra parts;
  - coverage differences counted, expected below 1e-4 of parts;
  - 0 misses, 0 overflow.
- perf_verify PF-0018 on veg_forest_pine, veg_horde_forest (Low) and foliage:
  - metrics zone:classic:cull_views, zone:render:prepare, cpu_work, scope:classic_cull;
  - gpu_frame guard (use cap=60 to separate submission-rate effects);
  - images within the gate, state identical.
- Toggle matrix: PF-0006, 0012, 0013, 0016, 0028 and 0030, each with PF-0018 on vs off; images within the gate.
- Forced !has_mdi path, and a forced low cap (deterministic CPU fallback): images unchanged.
- --validation with sync validation.
- msl_check.py.
- lint_commands.py.

### S6: #6 GPU culling of the main view and the scope PiP. Removes most of the CPU classic cull and the per-frame instance copy.

- PF id: PF-0019 PFX_CLASSIC_GPU_CULL_MAIN (neutral)
- Depends on: S5
- Files: src/core/profiler.h, src/core/profiler.c, src/render/vk_classic_cull.c, src/render/vk_classic.c, shaders/classic_cull.comp, shaders/classic_cull_math.glsl, src/game/game_settings_audit.c, src/game/game.c, docs/perf/fixes.json, docs/CLASSIC_RENDERER.md

The same chain, with the main / PiP rules:
- the impostor switch: unscaled radius, the tree or small threshold x (impostor_grid >> 16) / 16, iw from lod_band_weight;
- the foliage alpha and shrink;
- PF-0015 depth buckets, log2(max(clip w, 0.25)) from row 3 of view_proj_nj, when that gate is on;
- a part bound of 3 x candidates.

Order and depth-EQUAL contract:
- The stable radix order reproduces the CPU's order inside each slot, so the order inside every instanced draw equals today's.
- Only the order between runs (mesh, material) differs from today's (batch, section), and only across parts of different (LOD, fade) slots, which matters only at exact coplanar depth ties.
- The prepass (CP_GB*) and lit pass (CP_LIT*, depth EQUAL) draw the same commands from the same output ranges in the same order, so the invariant gl_Position contract holds (classic_vert.glsl:23).
- Fade and non-fade merge only in the clip groups, via m[15] = 1. cl_faded is its only reader (classic_common.glsl:113).

Impostor runs:
- CP_IMP_GB / CP_IMP_LIT / CP_PIP_IMP, with vkCmdDrawIndirect(args, off, 2, 16) covering fade and non-fade.

PiP:
- Keeps render_scope_cull before setup_scope_pip, i.e. last frame's scope camera, so tests stay comparable.

CPU consumers:
- g_main_cull stays set for classic_fade_probe (CPU reference).
- visibility_probe (game_settings_audit.c:108) and lod_dolly (game.c:1583) request a CPU reference cull of the probed frame: exact numbers, same frame.
- classic_stats, R->stats and perf_stop read the readback.
- C.inst then serves only the local light tiles (legacy rules).

Gain:
- classic:cull_views falls to roughly cell_tasks + command building, about 0.2-0.5 ms.
- Classic worker time drops about 85-95 %, and about 2 MB/frame of host-visible instance writes go away.
- GPU: +0.05-0.15 ms.
- Frame time improves only where CPU-bound. This step is also the prerequisite of S7.

Effort: 4-6 days.

**Fidelity:** NEAR-IDENTICAL (neutral, like PF-0015).
- Same parts, with the same S5 rounding residue.
- Per-slot order is identical.
- The cross-run order changes only exact-depth-tie pixels in the prepass G-buffer.
- Images within the gate; state unaffected (render-only).

**Verify:** - classic_cull_check for main and PiP: coverage differences as in S5, and per-slot order equal to the CPU path.
- perf_verify PF-0019 on devroom, foliage, veg_forest_pine and veg_horde_forest (Low):
  - metrics zone:render:prepare, cpu_work, scope:classic_cull, scope:prepass, scope:opaque, gpu_frame guard;
  - images within the gate or envelope, state identical.
- tools/tests/lod_*.cfg + lod_check.py.
- run_settings_tests.py: visibility_probe counts equal to the CPU path.
- lod_dolly logs.
- A scoped-weapon test (PiP).
- tools/tests/perf_compare.cfg.
- --validation.
- baseline.json re-recorded.

### S7: #7 Two-pass Hi-Z occlusion for classic instances in the main view (only if the S2 probe says go)

- PF id: PF-0031 PFX_OCCLUSION_CLASSIC (neutral)
- Depends on: S2, S6
- Files: src/core/profiler.h, src/core/profiler.c, src/render/vk_occlusion.c, src/render/vk_classic_cull.c, src/render/vk_classic.c, src/render/render.c, shaders/classic_cull.comp, shaders/occl_late.comp, shaders/occl_pyramid.comp, tests/test_occlusion.c, tools/tests/occl_pop.cfg, docs/perf/fixes.json, docs/CLASSIC_RENDERER.md

PASS 1 (inside classic_cull K1)
- The test runs when the history is valid: the pyramid was built last frame at the same size, R->camera_cut is false, and have_prev is set.
- Each main-view candidate's bound is tested against LAST frame's occl_hiz:
  - sampled in the pre batch after a COMPUTE -> COMPUTE barrier;
  - with prev_vp_j = T(prev_jitter) * prev_view_proj_nj, the matrix that rasterized that depth.
- Occluded candidates keep their decision record and go to the deferred list D, counted per slot.
- With an invalid history, everything is early.

BOUND
- The frustum sphere (c, r including max_wpo, vk_classic.c:753-754).
- Parts with iw > 0 are dilated to |impostor centre - c| + imp_radius * sc * sqrt(2), because impostor quads are not inside the culling sphere.

TEST
- Project the 8 corners of the bound's AABB. Any corner with w <= znear counts as visible.
- Dilate the rect by 1 texel and pick the mip where it spans at most 2x2 texels.
- 4 texelFetch reads. Occluded iff the nearest z > their max + eps.
- Each slot reserves count1 + countD output entries.

GRAPH (build_frame_graph)
- 'prepass': DrawItems, classic early (args_pre), skinned.
- 'occl_hiz': the S2 pass, scheduled every frame while the gate is on. The pyramid also becomes next frame's history. Optionally rebuild it after prepass_late if late counts run high.
- 'occl_cull_late': compute on the graphics queue, a buffer-only live side-effect pass:
  - vkCmdDispatchIndirect over D against this frame's pyramid, with fd->view_proj (the prepass's jittered matrix);
  - survivors are stably sorted and scattered to base + count1;
  - writes args_late {count2, base + count1} and args_lit {count1 + count2, base};
  - barrier COMPUTE -> DRAW_INDIRECT | VERTEX_ATTRIBUTE_INPUT.
- 'prepass_late': begin_gbuffer (render.c:1847) gains a depth LOAD variant (today depth always CLEARs, :1862), with the colour attachments loaded.
- post_screen_space.
- 'opaque': classic args_lit.

CORRECTNESS
- Pass-1 depth D1 comes from a subset of the final occluders, drawn with the same jittered projection.
- So an instance culled late lies strictly behind D1 >= the final depth at every pixel it covers, and never wins LESS_OR_EQUAL.
- The stale previous pyramid only decides early vs late: it costs time, never pixels. This honours the two-pass lock (SETTINGS_AUDIT.md:107).
- PiP, phone and shadow views are never occlusion-culled.
- The pass names fall outside fid_pass_is_post, so they also run at Fidelity Off.

DEFAULTS AND SCHEDULING
- Off when !R->occl_ok.
- On async frames, the async screen-space chain starts after prepass_late.

GAIN
- Probe-gated estimate: -0.3..-1.0 ms of prepass + opaque in forest interiors, -1..-3 ms behind terrain and in towns.
- Fixed cost about 0.1-0.3 ms.

Effort: 6-8 days.

**Fidelity:** IDENTICAL except exact depth ties. The late pass draws after the early pass, so coplanar ties between visible instances can resolve differently from the single-pass order. Images within the gate; no frame-late pop by construction.

**Verify:** - test_occlusion: a pure C model of the pyramid and the test, fuzzed with random depth buffers, odd sizes, jitter and near-plane-crossing spheres. It must never cull a sphere that has a covered pixel nearer than its nearest depth.
- tools/tests/occl_pop.cfg (r_maxfps 60), per-frame screenshots with PF-0031 on vs off, inside the gate including the first frame after each cut:
  - strafe out of cover at the dev room aim stand (x 104..128, z 40) and the hall wall;
  - trunks in veg_forest_pine;
  - setpos teleport (cut), r_render_scale change, scope ADS, an open phone view.
- perf_verify PF-0031 on devroom, veg_forest_pine and veg_overlook:
  - metrics scope:prepass, scope:prepass_late, scope:occl_hiz, scope:opaque, gpu_frame;
  - images, state.
- test_fidelity clean graphs, including at Engine Fidelity Off.
- Probe counts: rejections above 0 behind the hall, about 0 in veg_meadow.
- --validation.
- msl_check.py.

### S8: #7 Two-pass Hi-Z occlusion for skinned characters (zombies) in the main view (only if the S2 probe says go)

- PF id: PF-0032 PFX_OCCLUSION_SKIN (neutral)
- Depends on: S2, S7
- Files: src/core/profiler.h, src/core/profiler.c, src/render/render.c, src/render/vk_occlusion.c, shaders/skin_occl.comp, shaders/occl_late.comp, tools/tests/occl_pop.cfg, docs/perf/fixes.json

Records and spheres:
- build_skin_pass for pass 0 (render.c:1026-1037) also writes each record's sphere (d->center, d->radius, already its frustum bound) into a host buffer per slot.
- f->instances grows from MAX_SKIN_DRAWS x 8 to x 10 (render.c:201), adding a region the GPU compacts into.

Early pass, shaders/skin_occl.comp in the 'classic_cull' scope, one workgroup per batch:
- each record is tested against last frame's pyramid with prev_vp_j;
- a stable shared-memory prefix sum copies visible records unchanged to [b0, b0 + n1);
- the other records go to D.

Late pass, in occl_cull_late:
- D is retested with this frame's sphere and fd->view_proj;
- survivors are copied, stably, to [b0 + n1, b0 + n1 + n2).

Draws:
- draw_skin_pass_f (render.c:1134-1159) gets an indirect variant: one vkCmdDrawIndexedIndirect per batch with firstInstance = b0.
- skin.vert reads skin_inst.inst[gl_InstanceIndex] (skin.vert:27) and stays unchanged.
- prepass draws the early records, prepass_late the late ones, opaque_dynamic the union.

Unchanged:
- the shadow passes and the PF-0011 copies;
- the PiP and phone views, which use the original records;
- the CPU-skinning path.

Scheduling:
- The pyramid and prepass split from S7 are scheduled whenever PF-0031 or PF-0032 is on, so this id does not need the GPU classic cull.
- If the probe shows zombies dominate, this step can come before S7.

Gain:
- A hidden zombie is skinned 0 times instead of twice, in hordes behind buildings and terrain.
- About 0 in porous forests.

Effort: 3-5 days.

**Fidelity:** IDENTICAL except exact depth ties, by the S7 argument. Records are copied unchanged, so velocity and the previous-frame palette are preserved. Requires d->radius to bound the animated pose, which the frustum cull already relies on.

**Verify:** - perf_verify PF-0032 on horde500 (the dev room horde yard (0, 0, 50)) and devroom: metrics scope:prepass, scope:opaque_dynamic, gpu_frame; images and state identical.
- A zombie-behind-wall pop test in occl_pop.cfg.
- tools/tests/revive_loop.cfg and spec_net.sh (skinned paths).
- PiP and phone views still draw the full lists.
- --validation.
- msl_check.py.

### S9: MoltenVK fallback check and enablement, defaults and documentation

- PF id: none (capability defaults for PF-0017, 0018, 0019, 0030, 0031, 0032)
- Depends on: S5, S6, S7, S8
- Files: .github/workflows/platforms.yml, tools/macos/msl_check.py, src/render/vk_device.c, docs/PLATFORM_MACOS.md, docs/CLASSIC_RENDERER.md, docs/SHADOWS.md, docs/LOD.md, docs/VGEO.md, docs/PERF_AUDIT.md, CLAUDE.md

Mac (MoltenVK):
- The macOS CI job passes msl_check.py on every new .comp / .vert / .frag and logs the capability line.
- On portability the defaults are: CPU path with PF-0017, plus PF-0030. GPU cull and occlusion stay off through gpu_cull_ok / occl_ok.
- On Apple hardware, measure --gpu-cull (MoltenVK encodes each indirect sub-draw as its own Metal draw) and --occlusion (TBDR store / load) with perf_verify PF-0018 / 0019 / 0031 / 0032. Flip the portability defaults only where B wins.

Docs:
- CLASSIC_RENDERER.md: GPU path, arena, stats lag; fix '3 sun cascades'.
- SHADOWS.md: impostor casters; preset table to 4 cascades / 150 m.
- LOD.md: the impostor_shadow block.
- VGEO.md: the 96 px / 180 m switch is outdated.
- PERF_AUDIT.md, fixes.json, FINDINGS.md via perf_verify --update.
- PLATFORM_MACOS.md: capabilities.
- CLAUDE.md map row.

Effort: 2-3 days.

**Fidelity:** IDENTICAL (no new behaviour).

**Verify:** - The macOS CI job is green: msl_check, CPU-path images, capability log.
- Mac perf_verify reports attached before any portability default changes.
- lint_commands.py and lint_settings.py show no new cvars.

## Open questions

- Ungated steps. The plan leaves S0 (bug fixes and capability flags), S2 (dev tools only) and S4 (the LOD arena data layout) without a PF id. A registration-time layout cannot be A/B'd inside one perf_verify process, and 6 ids are spent on decisions that can be. Is that acceptable, or should one of them take an id at the cost of skinned occlusion (PF-0032)?
- Uncommitted render.c / vk_classic.c changes in the working tree: ShadowHold.L, receiver planes off for staggered far tiles, the PF-0029 shadow mip-bias push. The plan assumes they land first. S3's hold-key bit 18 relies on the new key layout at render.c:1528.
- Shadow tile size. Is the documented 4 x 2048^2 a level sun_shadow override in the perf scenes, or is Modern really 4096^2 (modern.json:31)? S2 measures it; the #4 cost estimate scales with tile area.
- Identity bar for the GPU cull. Is near-identical (about 1e-5 of parts moving one 1/256 dither step, inside the image gate) acceptable for PF-0018 / 0019? The alternative is emulated correctly rounded division / sqrt in the twin, plus a shared polynomial log2. The log2 change would alter the CPU A side by ulps and need a two-build envelope.
- #4 visual acceptance needs a human screenshot review. Open points: the planar self-shadow on mesh crowns between about 53 m and each tree's impostor switch; no wind sway on impostor shadows; changed god rays beyond 63 m; whether the main-view impostor gain of 1.2 (lod.json) needs retuning once impostors receive self-skip shadows. If rejected, a separate RG8 coverage + front-depth atlas (with the depth bias emulated) or runtime light-space impostors are the fallbacks.
- MoltenVK. Using MDI and the GPU cull on Apple GPUs needs Mac hardware measurements. Not documented in the repo, and to be verified in MVKDevice.mm: drawIndirectCount is absent and multi-draw is encoded per command.
- PF bit budget. This plan brings the u32 mask to 29 of 32 bits. The deferred lit-pass vertex variant without the previous-frame WPO, and any later items, need either a wider mask or retired ids.

## Dropped ideas

- drawIndirectCount / vkCmdDrawIndexedIndirectCount fast path: not enabled (vk_device.c chains a Vulkan12Features with only timelineSemaphore), absent on MoltenVK, and unnecessary once commands are grouped into MDI runs per mesh and material.
- Design 2's GPU cell culling (K1) and output buffers sized from the readback: the frame can overflow and drop visible parts, which breaks the identity claim and the two-pass no-pop rule. The CPU keeps cell_tasks, so the bound is known before recording.
- Evaluating the cull rules twice, as COUNT/EMIT or K3/K4: two dispatches can round differently and write outside the counted ranges. Replaced by decision records written once.
- Atomic-cursor scatter for the main view: the order inside a bucket would vary from frame to frame and make prepass depth ties flicker. Replaced by a stable scan and LSD radix sort, used for every view.
- A baked impostor depth in the normal-atlas alpha: vg_tex_build_mips weights rgb by alpha (vgeo_tex.c:52-58), so main-view impostor shading would change even with the fix off.
- gl_FragDepth / depth_greater impostor casters in v1: a written depth bypasses the hardware depth bias (render.c draw_sun_casters). A flat quad-plane depth with push and self-skip is used instead.
- Design 2's global mesh arena with material segments across meshes: without descriptor indexing, material binds split the calls anyway. A per-mesh arena gives the same call reduction more simply.
- Reviving the dormant vgeo_scene GPU upload path (vgeo_scene_init with a context): it brings vkDeviceWaitIdle at every commit, the page-pool upload and per-call mallocs. A classic-owned mirror with a staging ring replaces it.
- PF gates applied at registration time: meshes register once per process, so perf_verify cannot A/B them.
- Design 3's key superset narrowed by the cells' distance interval: no runtime miss detection, and a miss silently drops parts. The full LOD superset of M_v plus a miss counter is used instead.
- Cascade 3 at interval 4: PF-0028 uses one shared r_shadow_far_interval and the due formula (render.c:1535) would also halve cascade 2's rate.
- A separate PF id for #4b substitution: a gap-fill-only id adds GPU time and could never close perf_verify (2 % GPU guard). Both live in PF-0030, which frees an id for skinned occlusion.
- Per-instance visibility bits for the Hi-Z early pass (Design 3): they need resets on placement slot reuse and a retest. Testing against the previous pyramid needs no per-instance state.
- Occlusion for the shadow side (#7c: light-space Hi-Z, receiver masks from camera depth): porous canopies, cascade 3 must keep every caster for the froxels, decals and particles, and the masks would be one frame late or need reordering. Research only.
- Hi-Z for the PiP, phone and shadow views: these views have different cameras, and main-view occlusion must never remove the shadows of hidden casters.
- Moving the local light tiles to the GPU: they follow the legacy LOD rules and need per-tile scissors. They stay on the CPU stream C.inst[slot].
- Running the cull on the PF-0027 async queue: the cull is small, both the shadows and the prepass depend on it right away, and keeping one queue lets a single output buffer serve every frame.
- Lit-pass vertex variant without the previous-frame WPO (Design 3 aside): identical and cheap, but no PF id is left, and drivers may already strip the unused output. Deferred: check pipeline statistics first.
- Runtime light-space impostors and billboard-cloud shadow proxies: kept only as fallbacks if the PF-0030 screenshots are rejected.
- An RG8 coverage + depth shadow atlas: a follow-up only if flat-quad impostor shadows look wrong. It would need a VG_TREE_VERSION bump, rebuilt caches and depth bias emulated in the shader.
