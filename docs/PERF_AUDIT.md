# Performance audit

A repeatable audit that finds and ranks optimizations which **do not change the rendered image or the
gameplay outcome**: redundant work, bad scheduling, memory traffic, sync stalls, over-allocated
resources, missed culling, duplicated computation. Quality trade-offs (resolution, samples,
compression, precision, distances) are listed separately and marked **not recommended**: they are the
Engine Fidelity tables' job (docs/ENGINE_FIDELITY.md).

Guardrail: the tool never changes engine behaviour. It reads clocks, counters, cvars, profiles,
screenshots, sources and assets. Fixes are separate changes that reference a finding id and carry a
`perf_fix_disable` switch so `perf_verify` can prove them.

| Piece | Where |
|---|---|
| CPU profiler: zones, jobs, waits, counters, series, allocation sites, captures, JSON writer | `src/core/profiler.{h,c}`, `src/core/prof_alloc.h` (force-included) |
| GPU scopes: timestamps, pipeline statistics, command counts, VRAM | `src/render/vk_prof.{h,c}`, scopes in `render.c`, one per frame graph pass in `fg.c` |
| Frame graph introspection (passes, accesses, resources, lifetimes, report) | `src/render/graph/fg.c` (`write_profile`, `fg_last_report`) |
| Net field statistics | `src/net/net_prof.{h,c}` (hooks in `net.c`, `replication.c`) |
| Benchmark scenes, `perf_audit` / `perf_verify` / `perf_scenes`, state hash | `assets/perf/scenes.json`, `src/game/game_perf.c` |
| Analyzer, rules, reports, image diff, verify, CI gate | `tools/perf_audit/` (`perf_audit.py`, `pa/`, `tests/`) |
| Run scripts | `tools/perf_audit/{run_audit,run_verify,first_run}.sh`, `lan2_{host,client}.cfg` |
| Reports, findings registry | `docs/perf/AUDIT_<date>.{md,json}`, `docs/perf/timeline/*.html`, `docs/perf/fixes.json`, `docs/perf/FINDINGS.md` |
| CI | `.github/workflows/perf_audit.yml` |
| Tests | `tests/test_profiler.c` (ctest `profiler`), `tools/perf_audit/tests` (ctest `perf_audit_tools`) |

## Quick start

```sh
.toolchain/b.sh
tools/perf_audit/run_audit.sh                 # every scene, Modern + Low, 2-client LAN -> docs/perf/AUDIT_<date>.md
tools/perf_audit/run_verify.sh PF-0001        # A/B proof of one fix -> docs/perf/FINDINGS.md
tools/perf_audit/first_run.sh                 # both, for every implemented fix (the first run on a new machine)
```

In game (console): `perf_scenes`, `perf_audit [all|scene,..] [frames] [out] [levels=modern,low] [settle=120] [refs] [quit]`,
`perf_verify <PF-xxxx|all> [scenes] [frames] [out] [reps=2] [levels=modern] [settle=120] [quit]`,
`prof_capture <frames> <path.json> [label] [warmup] [scene]` (any moment of play),
`perf_fix_disable PF-0001,PF-0005` (A side of a manual comparison; `all` = every fix).

Analyzer: `python tools/perf_audit/perf_audit.py analyze <profile dir>` / `scan` / `verify <dir> --update` /
`findings` / `ci <dir>` / `timeline <profile> <out.html>` (standard library only).

## Methodology

1. **Measure without changing behaviour.** Probes compile in with `DG_PROFILE` (every config of the
   engine target; unit tests that compile engine files get no-op probes). While no capture runs every
   probe is one predictable branch on `dg_prof_recording`. The probe cost while recording is measured at
   every capture start (`meta.prof_overhead_ns`, ~65-105 ns per begin / end pair).
2. **Fixed scenes, deterministic workload.** `assets/perf/scenes.json`: dev room, horde 500, foliage
   (vegetation map), 500 zombies on the vegetation map (`horde_foliage`), the vegetation map's
   density zones (`veg_*`: pine / broadleaf forest interiors, forest edge, meadow, wheat field, dead
   wood, pond shore, the valley from the south rim, hordes in the pine forest and on the meadow),
   water + rain (water yard), night with every light, vehicle yard, a still camera without clouds
   (`sky_static`: what stops changing when nothing moves, R02), plus the 2-client LAN run
   (`lan2_host` / `lan2_client`). The file's `reset` lines run before every scene's setup (cvars a
   setup may change that the other scenes do not set). Every run pauses the clock and the weather, sets
   `dev_fixed_fps 60` (exactly one sim tick per frame: the same work in every run whatever the frame
   rate), drops vsync and the frame cap, settles 120 frames (streaming, LOD fades, exposure, temporal
   histories), then captures N frames along the scene's camera spline (or an orbit around the player
   on maps without fixed coordinates), at Modern and Low.
3. **One JSON profile per scene and level** (format below).
4. **Rules -> findings**, merged across scenes (the same finding id keeps the largest savings and every
   scene it appears in). Every finding states location, evidence, estimated savings (ms CPU / GPU, MB,
   bytes/s), confidence, **fidelity impact** (`none` for the main list, `tradeoff` otherwise), effort
   (S / M / L), how it is verified and a concrete fix.
5. **Ranking:** `(cpu_ms + gpu_ms + 0.02 x MB + bytes_s / 50000) x confidence / effort`, confidence
   high 0.9 / medium 0.6 / low 0.3, effort S 1 / M 3 / L 8. Timing-derived savings inside the run-to-run
   noise (2 x the robust deviation of the frame time) are kept but marked *below noise* and weighted 0.1:
   a hot-path allocation or an every-frame validation is a finding by definition (rules R08, R20) even
   when one frame cannot resolve its cost.
6. **Proof** (`perf_verify`, below): a finding is closed only when its metric improved significantly
   and both the image diff and the gameplay state hash pass.

### Software renderer runs

The engine also builds and runs on Linux with Mesa lavapipe (CPU Vulkan) under Xvfb. That is how the tool
was exercised end to end while it was written: real CPU zones, counters, allocation sites, frame graph,
command counts, screenshots and state hashes; **GPU timings are emulated** (relative cost only, never a
savings figure for the target). Reports made that way say so in their header. Target numbers come from
`run_audit.sh` on the Windows machine.

## Instrumentation

### CPU

| Data | Source |
|---|---|
| Main loop zones: `input`, `cbuf`, `menus`, `net`, `game_frame`, `job_completions`, `render_begin`, `game_render`, `ui_render`, `render_end` | `src/app/main.c` |
| Sim tick phases: `sim_tick`, `sim:host_tick`, `sim:players`, `sim:daynight_weather`, `sim:spectate`, `sim:entities`, `sim:projectiles`, `sim:horde`, `sim:anim_hitboxes`, `sim:chunks`, `sim:openworld`, `sim:water`, `sim:physics`, `sim:lagcomp`, `sim:events_net`, `sim:client_chars`, `sim:anim_present`, `focus_hud` | `src/game/game.c` |
| Render CPU: `render:prepare`, `render:record_pre`, `render:frame_graph`, `fg:validate` | `render.c`, `fg.c` |
| `anim:post_pass` (pose evaluations), `vfx:update` | `characters.c`, `vfx.c` |
| Jobs: `job:<fn>` per job (queue latency = submit -> start), `parfor:<fn>` per `jobs_parallel_for` participant; worker busy time per frame | `src/core/jobs.c` |
| Blocking waits: `wait:gpu_fence`, `acquire`, `present`, `readback`, `jobs_wait` (nothing left to help with: a dependency stall), `parfor_tail`, `io`, `fps_cap` | `render.c`, `jobs.c`, `platform.c`, `main.c` |
| Allocations: count / bytes per frame, call site (`file:line`) + innermost zone, frames seen | `prof_alloc.h` wraps `malloc`/`calloc`/`realloc`/`free` in every engine `.c` (`DG_PROFILE_ALLOC`, CMake option, default on) |
| `memcpy` calls / bytes per frame | `prof_alloc.h` |
| Counters: `cvar_find`, `log_lines`, `io_reads` / `io_bytes`, `jobs`, `parfor`, `anim_eval`, `anim_eval_offscreen` (clients), `skin_palettes`, `vfx_emitters`, `audio_voices`, `ui_text`, `draw_items`, `shadow_items` | `profiler.h` `PC_*` |
| Process private bytes every 4 frames (`series.process_mb`) | `profiler.c` |
| Startup critical path: zones from `prof_init` to the first frame (`startup` section) | `main.c` |

### GPU

Every frame graph pass and every pass recorded outside it (`uploads`, `vfx_sim`, `shadow_sun`,
`shadow_moon`, `shadow_vm`, `light_shadows`, `rain_occlusion`, `post_graph`) is a scope with:

- timestamps (ms per frame, `-1` when it did not run that frame);
- pipeline statistics (`pipelineStatisticsQuery`, enabled when supported): vertex / fragment /
  compute invocations and clipping primitives: the overdraw proxy is fragment invocations / pixels
  of the target, the occupancy proxy compute invocations / ms;
- commands recorded inside it: draws (direct / indirect), dispatches, pipeline binds, descriptor
  binds, push constants, barrier calls and image barriers, renderings, clears and cleared pixels
  (`LOAD_OP_CLEAR` and clear commands), copies and bytes copied. `vk_prof.h` routes the volk entry
  points of every render file through counting macros (`VKP_NO_COUNT` opts a file out);
- commands outside any scope land in `(unscoped)` (rule R25 asks for a scope).

Series: `gpu_frame_ms`, `vram_usage_mb`, `vram_budget_mb`, `vram_allocated_mb` (VMA heap budgets).

Frame graph section (`fg`): passes with their accesses (resource, access kind, read / write), resources
(kind, format, bytes per texel, size, mips, bytes, first / last pass, physical image), dead passes,
unused resources, pool / stale memory. Bandwidth per pass = bytes of the resources it reads + writes.

### Netcode

Host, per snapshot section (`header`, `local`, `spectate`, `time`, `weather`, `vehicles`, `entities`):
sent, bytes, and how many times its bytes differed from that client's previous snapshot. Entity deltas:
per field sent / bytes, and for grouped fields (`type`, `item`, `hp`, `vis`, `light`) how often each
member actually changed when the group went out. Messages per type and channel.

### Gameplay state hash (perf_verify)

Every captured frame (= one sim tick) hashes the replicated state of the whole world: `repl_capture`
of every entity (positions, rotations, flags, hp, items, attachments, animation state ids; records are
zeroed first, so no padding enters the hash) and the tick (`state` section).

### Not measured (coverage)

| Asked for | Status |
|---|---|
| Hardware cache-miss counters | Not available: Windows PMCs need admin rights (the toolchain is portable, no admin). R09 reports per-item loop cost and points to a sampling profiler. |
| Heap fragmentation | Not measured (process private bytes over time only). |
| Buffers written and never read back | No probe (would need descriptor tracking); R25 covers unscoped work only. |
| VFX emitters updating while culled, silent audio voices | Counters exist (`vfx_emitters_culled`, `audio_voices_silent`) without a probe yet: R13 stays silent until one feeds them. |
| Audio occlusion rays | The engine casts none (R17 checks the source). |
| Shader permutation compile time | Pipeline creation is inside `render_init` (startup zone), not split per pipeline. |
| Duplicate assets in memory | Static only (byte-identical files, R22); no runtime content hash per loaded asset. |

## Profile format (`dg-perf-profile` v1)

```
meta      scene, label, fidelity, build, config, os, frames, warmup, cpu_threads, prof_overhead_ns, wall_ms, truncated, fixes_disabled
frames    cpu_ms[frames]                      main loop period (includes wait:fps_cap; analyzer subtracts it)
series    name -> [frames] (-1 = not sampled)
counters  name -> [frames]
threads   [{index, name, class (0 main, 1 worker, 2+ other), events, truncated, job_busy_ms[frames]}]
zones     [{name, thread (main / workers / name), cat (zone / wait / job / parfor), total_ms, max_ms, incl_ms[], excl_ms[], calls[]}]
jobs      [{fn, parfor, count, total_ms, max_ms, queue_ms_avg, queue_ms_max}]
alloc_sites [{site, zone, count, bytes, frames}]
startup   {total_ms, zones: [[thread, name, start_ms, dur_ms, depth]]}
timeline  [{frame, ms, events: [[thread, name, start_us, dur_us, depth]]}]   median and worst frame
gpu, fg, net, state   module sections (above)
```

## Detection rules

Runtime rules (`pa/rules_runtime.py`) read one profile; static rules (`pa/rules_static.py`) scan
sources, shaders and assets. Every rule has a synthetic-profile test (`tools/perf_audit/tests`).

| Rule | Detects | Data | Fidelity |
|---|---|---|---|
| R01 | Passes with zero contribution: frame graph dead passes; scopes that only clear (nothing drawn, every frame); scopes whose draws invoke no shader | fg report, GPU scopes | none |
| R02 | Per-frame recomputation of static data: sky view / clouds / cloud shadow / LUT passes re-run with the clock and weather frozen; static shadow casters re-rendered into every cascade (propose cached static cascades) | GPU scopes | none (cache keyed on the inputs: bit-identical) |
| R03 | Over-resolution / over-wide targets: named half / quarter but allocated at render size; full-size targets whose readers all write a quarter of the pixels or less | fg resources | none when readers point-sample at their rate; else trade-off (verify) |
| R04 | Barrier batches that could merge (adjacent independent passes) | fg passes, barrier counts | none |
| R05 | Unused transients, stale pool memory, clears before a full overwrite | fg report, GPU scopes | none |
| R06 | Main thread blocked on fence / acquire / present / readback / I/O (classified GPU-bound when the GPU is the bottleneck) | wait zones, gpu_frame_ms | none |
| R07 | Workers idle while the main thread runs serial zones; dependency stalls (`jobs_wait`, parfor tail); jobs too small for the queue overhead | threads, jobs, waits | none |
| R08 | Allocations in the hot path (sites seen in at least half the frames); static: allocations in per-frame functions | alloc sites, sources | none |
| R09 | Hot loops whose per-item cost points at data layout (anim, VFX) | zones / counters | none |
| R10 | Redundant skinning: more palettes uploaded than poses evaluated (parts / passes not sharing) | counters | none |
| R11 | Shadow caster items far above scene items (casters outside every receiver) | counters, shadow scope | none |
| R12 | Clients posing off-screen characters at full rate (no hitboxes there) | counters, anim zone | none (state hash on the host; image diff) |
| R13 | VFX emitters updating while culled | counters | none |
| R14 | Textures stored uncompressed / without mips / oversized | assets | **trade-off** (compression and resizing change pixels) |
| R15 | (retired with the virtualized geometry streamer) | - | - |
| R16 | Snapshot sections changing in < 1 % of snapshots but always sent; grouped-field members that ride along unchanged; one reliable message per event | net section | none (protocol change: bump `DG_PROTOCOL_VERSION`) |
| R17 | Audio occlusion rays per frame instead of amortized | sources | none |
| R18 | UI rebuilt every frame with unchanged text runs | UI zones, counters | none |
| R19 | Logging / formatting in hot paths (log lines during a steady scene; unconditional `log_*` in per-frame functions) | counters, sources | none |
| R20 | Validation / debug work every frame (zones named validate / check / debug); unconditional debug draw in per-frame code | zones, sources | none |
| R21 | Shader outputs never referenced; fragment shaders branching heavily on uniforms (permutation candidates: measure first) | shaders | none |
| R22 | Byte-identical assets stored twice | assets | none |
| R23 | memcpy volume per frame | counters | none |
| R24 | Pipeline / descriptor binding churn (rebinding for most draws) | GPU scopes | none (opaque draw order inside a depth-tested pass) |
| R25 | GPU work outside any scope (unattributed) | GPU scopes | coverage |
| R26 | Name lookups in the frame loop (`cvar_find`) | counters, sources | none |
| R27 | Startup critical path: main thread blocked on file reads during startup, slowest init stages | startup section | none |
| T01 | Heaviest passes: lower settings would save part of them | GPU scopes | **trade-off** |

Half-precision math, fewer samples, lower resolutions, shorter distances and texture compression are
never in the main list: none of them can be proven pixel-identical.

## Verification (closing a finding)

`perf_verify <PF-id> [scenes] [frames] [out] [reps=2]` runs the fix's scenes as **A B A B ...**: A with
`perf_fix_disable <id>`, B with every fix on (alternating cancels drift: thermals, background load). Each
pass reloads the map (the same start state), settles, captures a profile (timings, counters, state hash
per tick), then takes the reference screenshots: the camera holds each key for at least 24 frames (TAA,
exposure and LOD fades converge), the shot waits for the same TAA jitter phase (`render_frame_index() % 8`)
in every pass, the 2D UI is hidden (`render_set_ui_hidden`). `perf_audit.py verify <dir> --update` then
decides:

- **Timing**: the fix's metrics (`docs/perf/fixes.json`: `scope:<pass>`, `zone:<name>`,
  `counter:<name>`, `site:<file>`, `cpu_work`, `gpu_frame`): one-sided Mann-Whitney U over the per-frame
  samples of all A vs all B passes, p < 0.05 and a lower median (counters and allocation sites are
  deterministic: B lower than A). Guards: neither whole-frame metric (CPU work, GPU) may regress by
  more than 2 % with p < 0.05.
- **Image** (`pa/imgdiff.py`): per shot, B vs A of the same repetition. Pass when at most 0.01 % of
  the pixels differ by more than 4/255 on a channel (TAA / dithering residue) and the block SSIM on luma
  (8 x 8, the perceptual metric) is at least 0.999; or when B vs A stays inside the A vs A' envelope
  (the same build twice: animation timing, histories) with 1.5 x slack. Diff heat maps go to `<dir>/diff`.
- **State**: the per-tick hashes of every B pass must equal A's, and the A passes must agree with each
  other (else the scene is not deterministic and the check is *inconclusive*, which does not close).
  Compared per scene / level (different scenes never share hashes). `horde_foliage` is not
  deterministic between map loads (the second load of a process differs from tick 0): prove fixes on
  the deterministic scenes and time them there.

`closed` = timing improved AND every image pair passes AND the state is identical AND no guard
regressed. A **visual trade-off** (a `fixes.json` entry with `"tradeoff"`: what changes on screen, and
`"screenshots"`) is the exception: it changes the image by design, so its image pairs are recorded
(heat maps in `<dir>/diff`) but do not decide; timing, state and the guards still do, and the visual
difference is documented with before / after screenshots instead. Trade-off fixes sit after
`PFX_TRADEOFF_FIRST` in `ProfFix`; `perf_fix_disable all` (the CI's image check) leaves them on. `--update` records the run in `docs/perf/fixes.json` and regenerates `docs/perf/FINDINGS.md`;
`--target` (or `PERF_TARGET=1` in `run_verify.sh`) marks the machine as the target hardware, the only
place a pass sets the status to `closed`. The harness itself is tested on a known-neutral change
(sub-tolerance TAA-like noise) and known-visible changes (+0.1 EV exposure, a darkened 8 x 8 patch, a
state hash that changes at one tick, a fix with no speed-up): `tools/perf_audit/tests/test_verify.py`.

## Implementing a fix

1. Find the finding id in `docs/perf/AUDIT_<date>.md`.
2. Implement the fix behind `prof_fix_enabled(PFX_<NAME>)` (add the id to `ProfFix` / `k_fix_ids` in
   the profiler) so the old path stays reachable for the A side.
3. Add the entry to `docs/perf/fixes.json`: rule, finding, code, metrics (primary first), scenes and
   why it cannot change a pixel or a tick.
4. `tools/perf_audit/run_verify.sh PF-xxxx` on the target machine; commit `docs/perf/`.
5. Once closed and released, the fallback path may be deleted (keep the id reserved).

## Adding a rule

1. Write `def rNN_name(p, ctx)` in `pa/rules_runtime.py` (a `Profile`: `zone`, `zones`, `counter`,
   `series`, `gpu_scopes`, `scope_stats`, `fg`, `net`, `alloc_sites`, `jobs`) or `def sNN(root, ctx)` in
   `pa/rules_static.py`, decorated with `@rule("RNN", "title")`. Return `Finding`s with a stable `key`
   (the location), evidence keyed by `p.key`, savings, confidence, effort, fix, and `fidelity="tradeoff"`
   whenever the fix could change a pixel or a tick.
2. If the rule needs data the profile lacks, add a counter (`PC_*` + its name in `k_counter_names`), a
   series (`prof_series`), a zone, or a section (`prof_add_section`).
3. Add a positive and a negative case to `tools/perf_audit/tests/test_rules.py` (`synth.py` builds
   profiles), run `python tools/perf_audit/tests/run_tests.py`.
4. List it in the table above.

## First run (2026-10-18)

Made where the tool was written: a Linux container without a GPU, the engine on Mesa lavapipe (Vulkan
on 4 CPU cores) under Xvfb, 1600 x 900. CPU zones, counters, allocation sites, frame graph, command
counts, screenshots and state hashes are real; GPU milliseconds are the software rasterizer's.
Report: [perf/AUDIT_2026-10-18.md](perf/AUDIT_2026-10-18.md); fixes: [perf/FINDINGS.md](perf/FINDINGS.md).

- Scenes captured: devroom, horde500, night_lights, vehicle_yard (Modern and Low). `foliage` and
  `water_weather` (the open-world maps) crash inside lavapipe's JIT; the unmodified base commit
  (2cd1d95) crashes the same way on `map vegetation` without any capture, so it is the software
  driver, not the instrumentation. `lan2` was not exercised: on this Linux container the client never
  completes the ENet connect to a local host (loopback UDP works; the host, running at ~100 ms frames
  and waiting with `net_wait_clients`, logs no incoming peer). The LAN tests (`tools/tests/*_net.sh`)
  run on Windows, where the first run captures `lan2`; the net section's rule (R16) is tested on
  synthetic profiles.
- The five fixes were verified A/B (2 x A/B, 40 frames, the dev room): state hashes identical in
  every pass, every reference image inside the A vs A' envelope (lavapipe frames differ run to run by
  ~0.3 % of the pixels), timings as recorded in FINDINGS.md. PF-0003 removes its allocation but its
  time gain is below this machine's noise: not closed.
- On the target machine: `tools/perf_audit/first_run.sh` (the full audit, then `run_verify.sh` for
  every fix with `PERF_TARGET=1`) replaces these numbers and closes the findings there.

## Target run (2026-10-18, Windows, RTX 3060 Ti)

The first run on real hardware (Windows 11, RTX 3060 Ti, driver 581.80, 12 threads, 1600 x 900);
its report replaced the lavapipe one in [perf/AUDIT_2026-10-18.md](perf/AUDIT_2026-10-18.md). The
machine was shared (other builds and games ran at the same time, the GPU at 30 - 40 % load from
outside the runs): absolute GPU milliseconds of separate runs varied up to 2x, so every claim rests on
`perf_verify`'s alternating A / B passes in one process. CPU work was stable to a few percent.

- Focus: big hordes in dense vegetation. The vegetation map's zones (`veg_*`) showed the sun shadow
  pass as the dominant GPU cost in the forest interiors: ~660 M alpha-tested fragment invocations per
  frame for the 4 x 2048^2 cascade tiles (~40 x overdraw of leaf cards), 14 - 30 ms at Modern; the main
  view's opaque + prepass 4 - 6 ms. On the CPU: the classic cull (5 views, ~15 ms of worker time per
  frame), the wind tile, the skinning palettes and the character draw poses of a horde.
- Fixes PF-0006, PF-0007, PF-0009, PF-0010, PF-0011, PF-0012 closed (images and state identical);
  PF-0008 (character poses on the jobs) was dropped: CPU -21 % but GPU frame +11 % in every B pass.
  All fixes on vs all off (one process, alternating): CPU work -20 .. -33 % in every vegetation zone
  and in the hordes (FINDINGS.md has each fix's numbers).
- Visible trade-offs, not applied (measured in the pine forest at Modern, shadow pass): 2 cascades
  instead of 4: -58 % (fragment invocations 663 M -> 268 M); the 2048 atlas (Shadow Resolution
  High instead of Very High): -53 % (159 M); `r_vgeo_shadow_error_px` 2 -> 8 and Soft Shadow
  Quality Low: no measurable change. Grass / bush casters limited to the near cascades cut the
  same fragments without the resolution loss on trees: built as PF-0013, a visual trade-off
  (docs/SHADOWS.md "Caster cascade limits", FINDINGS.md): meadow shadow pass -38 % at Modern, -84 % at
  Low; the forest interiors only -1 .. -9 % (their fragments are tree leaf cards).
- `horde_foliage` is not deterministic between map loads (above, Verification).
- Sky / frame graph: PF-0026 skips the sky passes whose inputs stand still (bit-identical: the key is
  every uniform the shader reads). R02 assumed the clock and the weather freeze them all; in fact the
  cloud layer drifts with the wind even with the weather locked, so in the benchmark scenes only the
  sky view LUT (a constant camera altitude) stands still: `sky_view` -20 % (0.031 -> 0.025 ms, the
  aerial perspective is jittered under TAA and runs); a still camera without a cloud layer
  (`sky_static`) skips everything but that and the easing: the four sky passes 0.086 -> 0.057 ms
  (measured serially, `r_async_compute 0`; the empty pass nodes keep their timestamps). The
  savings are small: the sky was already cheap. PF-0027 runs the sky, the screen-space chain and the froxels
  on the async compute queue next to the shadow maps / the opaque pass: GPU frame -3.6 % pooled
  (the dev room -5 .. -10 %, the meadow -4 %, the pine forest -0.5 .. -5 %), docs/RENDERING_POST.md
  "Async compute".

## CI

`.github/workflows/perf_audit.yml`:

- **tools** (every push / PR, hosted Ubuntu): the analyzer tests, the console registry lint, the static scan.
- **nightly** (schedule + manual, self-hosted Windows runner labelled `self-hosted, windows, gpu` on the
  game PC): build with the portable toolchain (repository variable `DG_TOOLCHAIN`), the full audit,
  `perf_verify all` (every fix off vs on: image and state must match), then
  `perf_audit.py ci --threshold 5`: fails when a scene's CPU work or GPU p50 regressed more than 5 %
  against `docs/perf/baseline.json` or a diff failed. The report, timelines, profiles and diff heat maps
  are uploaded as artifacts. First run: dispatch with `write_baseline: true` (and commit the baseline).

GitHub-hosted runners have no GPU: timings there would measure a software rasterizer, so the gate only
runs on the self-hosted runner.
