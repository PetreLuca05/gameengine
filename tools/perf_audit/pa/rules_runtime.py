"""Runtime detection rules: each reads one profile and returns findings (docs/PERF_AUDIT.md "Detection
rules"). A rule never guesses about the image: a rule whose fix could change a pixel or a gameplay
outcome reports with fidelity="tradeoff" (listed, not recommended).

Add a rule: write `def rNN_name(p, ctx)` returning a list of Finding, decorate it with @rule("RNN", ...),
add a synthetic-profile test in tests/test_rules.py. See docs/PERF_AUDIT.md "Adding a rule"."""
from .findings import Finding
from .profile import mean, median, pct, mad

RULES = []

# Passes whose inputs are fixed while the clock and the weather are paused (every scene pauses
# both): recomputing them every frame redoes the same work unless the pass caches on its inputs.
STATIC_WHEN_FROZEN = {
    "sky_view": "sky view LUT (depends on the sun / moon direction, the camera height and the aerosols)",
    "sky_clouds": "cloud map (cloud cover, wind offset, time)",
    "sky_cshadow": "cloud shadow map (cloud map + camera texel snap)",
    "sky_luts": "transmittance / multiple scattering LUTs",
}
# Scopes recorded outside the frame graph whose purpose is depth into a shadow map.
SHADOW_SCOPES = {"shadow_sun", "shadow_moon", "shadow_vm", "light_shadows"}


def rule(rid, title, kind="runtime"):
    def deco(fn):
        fn.rid, fn.title, fn.kind = rid, title, kind
        RULES.append(fn)
        return fn
    return deco


def noise_ms(p):
    """Run-to-run noise of a frame time: 2 x the robust deviation of the CPU frame time."""
    return 2.0 * mad(p.cpu_ms())


def scope_stats(p, s):
    runs = p.scope_runs(s)
    n = max(1, len(runs))
    def avg(key):
        v = s.get(key, [])
        return sum(v[i] for i in runs if i < len(v) and v[i] is not None and v[i] >= 0) / n if runs else 0.0
    return {
        "runs": len(runs), "frames": p.frames, "ms": avg("ms"), "draws": avg("draws") + avg("draws_indirect"), "dispatches": avg("dispatches"),
        "fs": avg("fs_invocations"), "cs": avg("cs_invocations"), "vs": avg("vs_invocations"), "clears": avg("clears"), "clear_px": avg("clear_px"),
        "barriers": avg("barriers"), "image_barriers": avg("image_barriers"), "pipelines": avg("pipeline_binds"), "descsets": avg("descriptor_binds"),
        "copies": avg("copies"), "copy_bytes": avg("copy_bytes"), "renderings": avg("renderings"),
    }


# ---------------------------------------------------------------------------------------- GPU


@rule("R01", "Passes with no visible contribution")
def r01_zero_contribution(p, ctx):
    out = []
    fg = p.fg()
    for name in fg.get("dead_passes", []):
        s = p.gpu_scope(name)
        ms = scope_stats(p, s)["ms"] if s else 0.0
        out.append(Finding("R01", "dead:" + name, "Frame graph pass '%s' writes nothing anyone reads" % name, "render/fg",
                           {"pass": name}, {p.key: {"gpu_ms": round(ms, 4), "fg_check": "dead pass"}}, {"gpu_ms": ms}, "high", "S",
                           "Do not schedule the pass when its outputs have no consumer (fg_pass returns -1 for it), or consume the output.",
                           scene=p.key))
    for s in p.gpu_scopes():
        st = scope_stats(p, s)
        if st["runs"] < max(3, p.frames // 2):
            continue
        work = st["draws"] + st["dispatches"] + st["copies"]
        shaded = st["draws"] + st["dispatches"]   # copies / blits invoke no shader by design
        if work == 0 and st["clears"] > 0:
            # Clear-only: a target cleared every frame with nothing drawn into it.
            out.append(Finding("R01", "clear_only:" + s["name"], "'%s' only clears its target (no draw / dispatch)" % s["name"], "render",
                               {"pass": s["name"]}, {p.key: {"gpu_ms": round(st["ms"], 4), "clears_per_frame": st["clears"], "clear_px": st["clear_px"],
                                                             "runs": st["runs"]}},
                               {"gpu_ms": st["ms"]}, "high", "S",
                               "Keep the cleared content: skip the clear (and its barriers) while the target stays empty and still holds the clear.",
                               scene=p.key, fix_id="PF-0001" if s["name"] in ("shadow_sun", "shadow_moon", "shadow_vm") else None))
        elif shaded > 0 and st["fs"] == 0 and st["cs"] == 0 and st["vs"] == 0 and p.gpu().get("pipeline_stats") and st["ms"] > 0.02:
            out.append(Finding("R01", "no_invocations:" + s["name"], "'%s' records work that invokes no shader" % s["name"], "render",
                               {"pass": s["name"]}, {p.key: dict(st)}, {"gpu_ms": st["ms"]}, "medium", "S",
                               "Every draw / dispatch of the pass is empty (culled to nothing / zero groups): skip the recording when its count is 0.",
                               scene=p.key))
    return out


@rule("R02", "Per-frame recomputation of static data")
def r02_static_recompute(p, ctx):
    out = []
    for name, what in STATIC_WHEN_FROZEN.items():
        s = p.gpu_scope(name)
        if not s:
            continue
        st = scope_stats(p, s)
        rate = st["runs"] / max(1, p.frames)
        if rate >= 0.4 and st["ms"] > 0.005:
            out.append(Finding("R02", "frozen:" + name, "'%s' recomputed %s with unchanged inputs" % (name, "every frame" if rate >= 0.8 else "every other frame"),
                               "render/sky", {"pass": name},
                               {p.key: {"runs_per_frame": round(rate, 2), "gpu_ms": round(st["ms"], 4), "inputs": what}}, {"gpu_ms": st["ms"] * rate},
                               "medium", "M",
                               "Hash the pass inputs (the uniforms it reads, quantized as the shader reads them) and skip the dispatch when the hash "
                               "matches the content already in the target. The output is bit-identical by construction.", scene=p.key))
    sun = p.gpu_scope("shadow_sun")
    if sun:
        st = scope_stats(p, sun)
        if st["ms"] > 0.15 and st["draws"] > 20:
            out.append(Finding("R02", "static_cascades", "Static shadow casters re-rendered into every cascade every frame", "render/shadows",
                               {"pass": "shadow_sun", "file": "src/render/render.c"},
                               {p.key: {"gpu_ms": round(st["ms"], 4), "draws": round(st["draws"], 1), "shadow_items": round(p.counter_mean("shadow_items"), 1)}},
                               {"gpu_ms": st["ms"] * 0.6}, "medium", "L",
                               "Cache the static casters per cascade (re-render a cascade only when its snapped matrix or a static caster changes) and "
                               "draw only the dynamic casters over a copy of the cached depth. Identical depth by construction.", scene=p.key))
    return out


@rule("R03", "Render targets larger or wider than their consumers need")
def r03_over_resolution(p, ctx):
    out = []
    fg = p.fg()
    res, passes = fg.get("resources", []), fg.get("passes", [])
    rw, rh = fg.get("render_w", 0), fg.get("render_h", 0)
    for i, r in enumerate(res):
        nm = r["name"].lower()
        if r["kind"] == "import" or not rw:
            continue
        full = r["w"] >= rw and r["h"] >= rh
        if full and any(t in nm for t in ("half", "quarter", "_lo", "low")):
            out.append(Finding("R03", "named_low:" + r["name"], "'%s' is named low-resolution but allocated at %ux%u (render size)" % (r["name"], r["w"], r["h"]),
                               "render/fg", {"resource": r["name"]}, {p.key: {"w": r["w"], "h": r["h"], "bytes": r["bytes"]}},
                               {"mb": r["bytes"] * 0.75 / 1048576.0}, "low", "S",
                               "Check the size class (FG_SIZE_HALF / QUARTER): if the consumers sample it at the lower rate, allocate it there.",
                               scene=p.key))
        # Written at full resolution, read only by passes whose own outputs are all smaller.
        readers = [ps for ps in passes if any(u["res"] == i and not u["write"] for u in ps["uses"])]
        if not readers or not full:
            continue
        sizes = []
        for ps in readers:
            outs = [res[u["res"]] for u in ps["uses"] if u["write"] and u["res"] < len(res)]
            sizes.append(max([o["w"] * o["h"] for o in outs], default=rw * rh))
        if max(sizes) * 4 <= r["w"] * r["h"]:
            out.append(Finding("R03", "consumers_smaller:" + r["name"], "'%s' (%ux%u) only feeds passes writing at most a quarter of its pixels" % (
                r["name"], r["w"], r["h"]), "render/fg", {"resource": r["name"]},
                {p.key: {"bytes": r["bytes"], "reader_max_px": max(sizes)}}, {"mb": r["bytes"] * 0.75 / 1048576.0}, "low", "M",
                "If every reader downsamples it on read (point / bilinear taps at the lower rate), produce it at the reader resolution. Verify by image "
                "diff: a reader with a wide filter is a trade-off.", scene=p.key))
    return out


@rule("R04", "Barrier batches that could merge")
def r04_barriers(p, ctx):
    fg = p.fg()
    passes, out = fg.get("passes", []), []
    mergeable = 0
    for a, b in zip(passes, passes[1:]):
        wa = {u["res"] for u in a["uses"] if u["write"]}
        rb = {u["res"] for u in b["uses"]}
        if not (wa & rb):
            mergeable += 1
    total = sum(scope_stats(p, s)["barriers"] for s in p.gpu_scopes())
    if mergeable >= 4 and total > 0:
        est = mergeable * 0.003   # ~3 us of pipeline drain per avoidable barrier batch (conservative)
        out.append(Finding("R04", "fg_independent_pairs", "%d adjacent frame graph passes are independent but barrier separately" % mergeable,
                           "render/fg", {"file": "src/render/graph/fg.c", "function": "pass_barriers"},
                           {p.key: {"independent_adjacent_pairs": mergeable, "barrier_calls_per_frame": round(total, 1)}}, {"gpu_ms": est}, "low", "M",
                           "Emit the barriers of a run of independent passes as one vkCmdPipelineBarrier2 before the run (fg_execute look-ahead).",
                           scene=p.key, below_noise=True))
    return out


@rule("R05", "Redundant clears, unused or stale transient targets")
def r05_clears_transients(p, ctx):
    out = []
    fg = p.fg()
    for name in fg.get("unused_res", []):
        out.append(Finding("R05", "unused:" + name, "Frame graph resource '%s' is declared but used by no pass" % name, "render/fg", {"resource": name},
                           {p.key: {"fg_check": "unused"}}, {"mb": 0.0}, "high", "S", "Remove the declaration (or the condition that drops its passes).",
                           scene=p.key))
    if fg.get("stale_mb", 0) > 1.0:
        out.append(Finding("R05", "stale_pool", "%.1f MB of pooled frame graph images unused by the current graph" % fg["stale_mb"], "render/fg",
                           {"file": "src/render/graph/fg.c"}, {p.key: {"stale_images": fg.get("stale_images"), "stale_mb": fg["stale_mb"]}},
                           {"mb": fg["stale_mb"]}, "medium", "S", "fg_trim after level / setting changes (the 240-frame eviction keeps them meanwhile).",
                           scene=p.key))
    for s in p.gpu_scopes():
        st = scope_stats(p, s)
        if st["clears"] >= 1 and st["clear_px"] > 0 and st["fs"] >= 1.0 * st["clear_px"] and st["draws"] == 1 and st["runs"] > p.frames // 2:
            out.append(Finding("R05", "clear_then_full:" + s["name"], "'%s' clears a target its single full-screen draw overwrites" % s["name"], "render",
                               {"pass": s["name"]}, {p.key: {"clear_px": st["clear_px"], "fs_invocations": st["fs"]}}, {"gpu_ms": st["ms"] * 0.05},
                               "low", "S", "Use LOAD_OP_DONT_CARE when every pixel is written (blending off).", scene=p.key))
    return out


@rule("R24", "Pipeline / descriptor binding churn")
def r24_binding_churn(p, ctx):
    out = []
    for s in p.gpu_scopes():
        st = scope_stats(p, s)
        if st["draws"] >= 50 and st["pipelines"] >= 0.5 * st["draws"]:
            out.append(Finding("R24", "pipes:" + s["name"], "'%s' rebinds a pipeline for %.0f%% of its draws" % (s["name"], 100 * st["pipelines"] / st["draws"]),
                               "render", {"pass": s["name"]}, {p.key: {"draws": st["draws"], "pipeline_binds": st["pipelines"], "descriptor_binds": st["descsets"]}},
                               {"cpu_ms": st["pipelines"] * 0.0005}, "medium", "M",
                               "Sort the draw list by pipeline (then material) and bind on change only. Draw order inside one depth-tested opaque pass "
                               "does not change the image; keep blended draws in their order.", scene=p.key))
    return out


@rule("R25", "GPU work outside any profiled scope")
def r25_unscoped(p, ctx):
    s = p.gpu_scope("(unscoped)")
    if not s:
        return []
    st = scope_stats(p, s)
    if st["draws"] + st["dispatches"] + st["copies"] < 1:
        return []
    return [Finding("R25", "unscoped", "Commands recorded outside every GPU scope (unattributed cost)", "render/profiler", {"file": "src/render/render.c"},
                    {p.key: st}, {}, "low", "S", "Wrap the recording in vkp_scope_begin / end so the audit can rank it.", scene=p.key,
                    verify="no engine change (profiler coverage)", below_noise=True)]


# ---------------------------------------------------------------------------------------- CPU


@rule("R06", "Main thread blocked on the GPU, I/O or the swapchain")
def r06_waits(p, ctx):
    out = []
    gpu = median(p.gpu_frame_ms())
    cpu = median(p.cpu_ms())
    for w in ("gpu_fence", "acquire", "present", "readback", "io"):
        z = p.zone("wait:" + w)
        if not z:
            continue
        ms = mean(z["incl_ms"])
        if ms < 0.2:
            continue
        gpu_bound = gpu > 0 and gpu >= cpu - ms - 0.1
        if w in ("gpu_fence", "acquire", "present") and gpu_bound:
            # The GPU is the bottleneck: the wait is the symptom, the GPU findings are the cause.
            out.append(Finding("R06", "wait:" + w, "Main thread waits %.2f ms on %s (GPU bound: GPU %.2f ms vs CPU work %.2f ms)" % (ms, w, gpu, cpu - ms),
                               "frame", {"zone": "wait:" + w}, {p.key: {"wait_ms": round(ms, 3), "gpu_ms": round(gpu, 3), "cpu_ms": round(cpu, 3)}}, {},
                               "high", "M", "Nothing to fix on the CPU side; the GPU findings of this scene are the lever.", scene=p.key,
                               verify="no change (classification)", below_noise=True))
            continue
        out.append(Finding("R06", "wait:" + w, "Main thread blocked %.2f ms per frame on %s" % (ms, w), "frame", {"zone": "wait:" + w},
                           {p.key: {"wait_ms": round(ms, 3), "max_ms": round(z.get("max_ms", 0), 3), "gpu_ms": round(gpu, 3)}}, {"cpu_ms": ms},
                           "medium" if w != "io" else "high", "M",
                           {"io": "Move the read to a job (async load, completion callback) or preload it before the frame loop.",
                            "readback": "Read back a frame later (keep the buffer, map it when its fence has signalled) instead of waiting now.",
                            "present": "Check the present mode: FIFO with a full queue blocks here; mailbox / an extra swapchain image.",
                            }.get(w, "Record the next frame's CPU work before waiting (frames in flight), or move work off the critical path."),
                           scene=p.key))
    return out


@rule("R07", "Job system utilization and serialized sections")
def r07_jobs(p, ctx):
    out = []
    cpu = p.cpu_ms()
    frame = median(cpu)
    workers = [t for t in p.threads() if t.get("class") == 1]
    if workers and frame > 0:
        busy = mean([mean(t.get("job_busy_ms", [])) for t in workers])
        util = busy / frame
        jw = p.zone_mean("wait:jobs_wait") + p.zone_mean("wait:parfor_tail")
        main_excl = sorted(((mean(z["excl_ms"]), z["name"]) for z in p.zones(cat="zone", thread="main")), reverse=True)[:5]
        if util < 0.25 and main_excl and main_excl[0][0] > 1.0:
            out.append(Finding("R07", "underused_workers", "Workers busy %.0f%% of the frame while the main thread runs serial zones" % (100 * util),
                               "core/jobs", {"zone": main_excl[0][1]},
                               {p.key: {"workers": len(workers), "worker_busy_ms": round(busy, 3), "frame_ms": round(frame, 3),
                                        "top_main_zones_ms": [[n, round(v, 3)] for v, n in main_excl]}},
                               {"cpu_ms": main_excl[0][0] * 0.5}, "low", "L",
                               "The largest main-thread zones are candidates for jobs_parallel_for (per-index outputs keep results thread-count "
                               "independent: no behaviour change).", scene=p.key))
        if jw > 0.2:
            out.append(Finding("R07", "dependency_stalls", "Main thread stalls %.2f ms per frame waiting for jobs (nothing left to help with)" % jw,
                               "core/jobs", {"file": "src/core/jobs.c"}, {p.key: {"jobs_wait_ms": round(p.zone_mean("wait:jobs_wait"), 3),
                                                                                  "parfor_tail_ms": round(p.zone_mean("wait:parfor_tail"), 3)}},
                               {"cpu_ms": jw * 0.5}, "medium", "M",
                               "Smaller chunks at the end of parallel_for (guided scheduling) or start independent main-thread work before waiting.",
                               scene=p.key))
    for j in p.jobs():
        if j["count"] >= 5 * p.frames and j["total_ms"] / max(1, j["count"]) < 0.01:
            out.append(Finding("R07", "tiny_jobs:" + j["fn"], "Job %s runs %d times for %.1f us each (queue overhead dominates)" % (
                j["fn"], j["count"], 1000.0 * j["total_ms"] / j["count"]), "core/jobs", {"function": j["fn"]},
                {p.key: {"count": j["count"], "avg_us": round(1000.0 * j["total_ms"] / j["count"], 2), "queue_ms_avg": j.get("queue_ms_avg")}},
                {"cpu_ms": j["count"] / p.frames * 0.002}, "medium", "S", "Batch the work into fewer, larger jobs.", scene=p.key))
    return out


@rule("R08", "Allocations in the hot path")
def r08_hot_alloc(p, ctx):
    out = []
    for s in p.alloc_sites():
        if s["frames"] < max(2, int(0.5 * p.frames)):
            continue   # load-time / occasional allocations are not hot-path findings
        per_frame = s["count"] / max(1, p.frames)
        site = s["site"].replace("\\", "/")
        rel = site[site.find("src/"):] if "src/" in site else site
        fix_id = {"vgeo_skin.c": "PF-0003", "vgeo_stream.c": "PF-0004"}.get(rel.split(":")[0].split("/")[-1])
        out.append(Finding("R08", rel, "malloc in the frame loop at %s (%.1f calls / frame, %.0f B / frame)" % (rel, per_frame, s["bytes"] / max(1, p.frames)),
                           "core/alloc", {"file": rel.split(":")[0], "line": rel.split(":")[-1], "zone": s.get("zone")},
                           {p.key: {"calls_per_frame": round(per_frame, 2), "bytes_per_frame": round(s["bytes"] / max(1, p.frames)), "zone": s.get("zone")}},
                           {"cpu_ms": per_frame * 0.0004}, "high", "S",
                           "Keep the buffer for the owner's lifetime (sized at init) or take it from g_engine.frame_arena.", scene=p.key, fix_id=fix_id))
    total = p.counter_mean("alloc_count")
    if total > 50:
        out.append(Finding("R08", "total", "%.0f wrapped allocations per frame in total" % total, "core/alloc", {},
                           {p.key: {"alloc_count": round(total, 1), "alloc_bytes": round(p.counter_mean("alloc_bytes"))}}, {"cpu_ms": total * 0.0004},
                           "medium", "M", "See the per-site findings; route per-frame scratch through the frame arena.", scene=p.key))
    return out


@rule("R10", "Redundant skinning palette work")
def r10_skinning(p, ctx):
    evals, pals = p.counter_mean("anim_eval"), p.counter_mean("skin_palettes")
    if evals <= 0 or pals <= evals * 1.1:
        return []
    return [Finding("R10", "palettes_vs_evals", "%.0f palettes uploaded per frame for %.0f evaluated poses" % (pals, evals), "anim/skinning",
                    {"file": "src/render/vgeo/vgeo_skin.c"}, {p.key: {"palettes": round(pals, 1), "anim_evals": round(evals, 1)}},
                    {"cpu_ms": (pals - evals) * 0.002}, "medium", "M",
                    "Upload a palette once per pose and let the main, shadow and viewmodel passes share the slot (the bone SSBO already holds "
                    "this frame's palettes).", scene=p.key)]


@rule("R12", "Off-screen animation on clients")
def r12_offscreen_anim(p, ctx):
    evals, off = p.counter_mean("anim_eval"), p.counter_mean("anim_eval_offscreen")
    if evals <= 0 or off < 1 or off / evals < 0.1:
        return []
    per = p.zone_mean("anim:post_pass") / evals if evals else 0.0
    return [Finding("R12", "client_offscreen", "Clients pose %.0f off-screen characters per frame (%.0f%% of the evaluations)" % (off, 100 * off / evals),
                    "anim", {"function": "chars_present_tick", "file": "src/gameplay/characters.c"},
                    {p.key: {"evals": round(evals, 1), "offscreen": round(off, 1), "ms_per_eval": round(per, 4)}}, {"cpu_ms": off * per * 0.8},
                    "medium", "M",
                    "On clients (no hitboxes there), evaluate culled characters at the lowest LOD rate; the frozen render pose is already what "
                    "they show. Verify the state hash on the host (unchanged) and an image diff (nothing on screen changes).", scene=p.key)]


@rule("R13", "VFX updating while culled")
def r13_vfx_culled(p, ctx):
    upd, culled = p.counter_mean("vfx_emitters"), p.counter_mean("vfx_emitters_culled")
    if upd <= 0 or culled <= 0:
        return []
    return [Finding("R13", "culled_emitters", "%.0f of %.0f VFX emitters update while culled" % (culled, upd), "vfx", {"file": "src/vfx/vfx_sim.c"},
                    {p.key: {"emitters": upd, "culled": culled}}, {"cpu_ms": p.zone_mean("vfx:update") * culled / upd}, "medium", "M",
                    "Fast-forward culled emitters on becoming visible (deterministic seeds) instead of simulating them every tick.", scene=p.key)]


@rule("R15", "Streaming prefetch too late or too early")
def r15_streaming(p, ctx):
    wait, ev, req = p.series("vgeo_stream_waiting"), p.series("vgeo_stream_evicted"), p.series("vgeo_stream_requested")
    if not wait:
        return []
    w, e, r = mean(wait), mean(ev), mean(req)
    out = []
    if w > 0.5:
        out.append(Finding("R15", "late", "%.1f vgeo pages per frame still waiting (requested late: coarser clusters shown meanwhile)" % w, "render/vgeo",
                           {"file": "src/render/vgeo/vgeo_stream.c"}, {p.key: {"waiting": round(w, 2), "requested": round(r, 2)}}, {}, "medium", "M",
                           "Request pages from the camera's predicted position (velocity x latency); raise pages_per_frame while the queue grows.",
                           scene=p.key, below_noise=True))
    if e > 0.5 and r > 0.5:
        out.append(Finding("R15", "thrash", "%.1f evictions per frame with %.1f requests (pool thrash)" % (e, r), "render/vgeo",
                           {"file": "src/render/vgeo/vgeo_stream.c"}, {p.key: {"evicted": round(e, 2), "requested": round(r, 2)}}, {}, "medium", "M",
                           "Evict by last use with a hysteresis (pages used in the last N frames stay).", scene=p.key, below_noise=True))
    return out


@rule("R18", "UI rebuilt every frame without changes")
def r18_ui(p, ctx):
    ui = p.zone_mean("ui_render") + p.zone_mean("focus_hud")
    text = p.counter("ui_text")
    if ui < 0.25 or not text:
        return []
    same = sum(1 for a, b in zip(text, text[1:]) if a == b) / max(1, len(text) - 1)
    if same < 0.8:
        return []
    return [Finding("R18", "relayout", "UI rebuilds %.0f text runs every frame (%.0f%% of frames unchanged), %.2f ms" % (mean(text), 100 * same, ui), "ui",
                    {"file": "src/ui/gui/gui_draw.c", "function": "gd_text"}, {p.key: {"ui_ms": round(ui, 3), "text_runs": round(mean(text), 1)}},
                    {"cpu_ms": ui * 0.5}, "low", "L",
                    "Cache the shaped glyph runs per (string, font, size) and reuse the vertex block of panels whose inputs did not change.",
                    scene=p.key)]


@rule("R19", "String formatting / logging in hot paths")
def r19_logging(p, ctx):
    lines = p.counter_mean("log_lines")
    if lines < 0.05:
        return []
    return [Finding("R19", "log_in_frame", "%.2f log lines per frame during a steady scene" % lines, "core/log", {"file": "src/core/log.c"},
                    {p.key: {"log_lines_per_frame": round(lines, 3)}}, {"cpu_ms": lines * 0.02}, "high", "S",
                    "Log on change / once (each line formats, writes stdout + the file and flushes).", scene=p.key)]


@rule("R23", "memcpy volume per frame")
def r23_memcpy(p, ctx):
    b = p.counter_mean("memcpy_bytes")
    if b < 8 * 1048576:
        return []
    return [Finding("R23", "volume", "%.1f MB copied per frame by wrapped memcpy" % (b / 1048576.0), "core", {},
                    {p.key: {"memcpy_mb": round(b / 1048576.0, 2), "calls": round(p.counter_mean("memcpy_calls"))}}, {"cpu_ms": b / 1048576.0 * 0.08},
                    "medium", "M", "Write the data where it is consumed (mapped buffers) instead of staging copies.", scene=p.key)]


@rule("R11", "Shadow casters drawn into cascades")
def r11_shadow_casters(p, ctx):
    items, draws = p.counter_mean("shadow_items"), p.counter_mean("draw_items")
    if draws <= 0 or items < 2.5 * draws:
        return []
    return [Finding("R11", "caster_ratio", "%.0f shadow caster items per frame for %.0f scene draw items" % (items, draws), "render/shadows",
                    {"file": "src/render/render.c", "function": "draw_sun_casters"}, {p.key: {"shadow_items": round(items), "draw_items": round(draws)}},
                    {"gpu_ms": scope_stats(p, p.gpu_scope("shadow_sun"))["ms"] * 0.2 if p.gpu_scope("shadow_sun") else 0.0}, "low", "M",
                    "Cull casters against the receiver volume (camera frustum extruded towards the light) per cascade, not only the light box.",
                    scene=p.key)]


@rule("R09", "Per-entity cost of the hot loops (data layout candidates)")
def r09_layout(p, ctx):
    out = []
    pairs = [("anim:post_pass", "anim_eval", 0.02, "characters (CharacterInst pointers, Xform pose arrays)", "src/gameplay/characters.c"),
             ("vfx:update", "vfx_emitters", 0.01, "VFX instances / CPU particles (array of structs)", "src/vfx/vfx_sim.c"),
             ("sim:horde", None, 0.0, "horde zombies", "src/gameplay/zombies")]
    for zone, counter, budget, what, f in pairs:
        ms = p.zone_mean(zone)
        n = p.counter_mean(counter) if counter else 0.0
        if not counter or n <= 0 or ms <= 0:
            continue
        per = ms / n
        if per > budget:
            out.append(Finding("R09", zone, "%s: %.1f us per item (%.0f items, %.2f ms)" % (zone, per * 1000, n, ms), "cpu/layout", {"zone": zone, "file": f},
                               {p.key: {"ms": round(ms, 3), "items": round(n, 1), "us_per_item": round(per * 1000, 2)}}, {"cpu_ms": ms * 0.25}, "low", "L",
                               "Profile with a sampling profiler (cache-miss counters need admin rights on Windows: not available here) and move the "
                               "fields the loop touches into hot SoA arrays; the math is unchanged.", scene=p.key))
    return out


@rule("R20", "Validation / debug work every frame")
def r20_debug_work(p, ctx):
    out = []
    for z in p.zones(cat="zone"):
        nm = z["name"]
        if not any(k in nm for k in ("validate", "check", "debug", "dbg")):
            continue
        calls = z["calls"]
        frames_with = sum(1 for c in calls if c)
        ms = mean(z["incl_ms"])
        if frames_with >= 0.9 * p.frames and ms > 0.0:
            out.append(Finding("R20", nm, "'%s' runs every frame (%.3f ms) although only tests / benches read it" % (nm, ms), "debug", {"zone": nm},
                               {p.key: {"ms": round(ms, 4), "frames": frames_with}}, {"cpu_ms": ms}, "high", "S",
                               "Build it on demand (first request after the frame) or only while a check is running.", scene=p.key,
                               fix_id="PF-0005" if nm == "fg:validate" else None, below_noise=ms < noise_ms(p)))
    return out


@rule("R26", "Name lookups in the frame loop")
def r26_lookups(p, ctx):
    n = p.counter_mean("cvar_find")
    if n < 5:
        return []
    scan_us = 2.5   # ~700 cvars x case-insensitive compare, linear (measured order of magnitude)
    return [Finding("R26", "cvar_find", "%.0f cvar_find calls per frame" % n, "core/cvar", {"file": "src/core/cvar.c", "function": "cvar_find"},
                    {p.key: {"calls_per_frame": round(n, 1)}}, {"cpu_ms": n * scan_us / 1000.0}, "high", "S",
                    "Index the cvars by a case-insensitive name hash (same result), and cache Cvar* at init in the per-frame callers.", scene=p.key,
                    fix_id="PF-0002")]


# ---------------------------------------------------------------------------------------- net


@rule("R16", "Snapshot data sent though it rarely changes")
def r16_net(p, ctx):
    net = p.net()
    if not net or not net.get("snapshots"):
        return []
    out = []
    snaps = net["snapshots"]
    wall_s = max(1e-3, p.d.get("meta", {}).get("wall_ms", 0) / 1000.0)
    for name, s in net.get("sections", {}).items():
        if name in ("header", "entities") or not s["sent"]:
            continue
        rate = s["changed"] / s["sent"]
        if rate < 0.01 and s["bytes"] / s["sent"] >= 4:
            bps = s["bytes"] * (1 - rate) / wall_s
            out.append(Finding("R16", "section:" + name, "Snapshot section '%s' (%.0f B) changes in %.1f%% of snapshots but is always sent" % (
                name, s["bytes"] / s["sent"], 100 * rate), "net", {"file": "src/net/net.c", "function": "net_host_send_snapshots"},
                {p.key: {"sent": s["sent"], "changed": s["changed"], "bytes_per_snapshot": round(s["bytes"] / s["sent"], 1)}}, {"bytes_s": bps},
                "high", "M", "Send it on change (a version byte + resend until acked) like the weather block. Protocol change: bump DG_PROTOCOL_VERSION.",
                scene=p.key))
    for fname, f in net.get("fields", {}).items():
        wb = f.get("with_base", 0)
        if wb < 20:
            continue
        for member, ch in f.get("members", {}).items():
            rate = ch / wb
            if rate < 0.01:
                size = {"max_hp": 4, "armor": 4, "slot": 1, "count": 2, "model": 2, "shape": 1, "emissive": 1, "half": 12, "color": 4,
                        "light_color": 12, "light_radius": 4, "light_intensity": 4, "light_group": 4, "type": 1, "item": 2, "hp": 4}.get(member, 4)
                out.append(Finding("R16", "member:%s.%s" % (fname, member), "'%s' rides along with every '%s' update but changed in %.1f%% of them" % (
                    member, fname, 100 * rate), "net", {"file": "src/net/replication.c", "function": "write_fields"},
                    {p.key: {"group_sends_with_base": wb, "member_changed": ch}}, {"bytes_s": size * wb / wall_s}, "medium", "M",
                    "Split the member into its own NF_ bit (or a sub-mask like NF_ATTACH). Protocol change: bump DG_PROTOCOL_VERSION.", scene=p.key))
    events = [m for m in net.get("messages", []) if m["type"] == 6 and m["reliable"]]   # MSG_EVENT
    for m in events:
        if m["count"] >= 2 * snaps:
            out.append(Finding("R16", "events_unbatched", "%d reliable MSG_EVENT messages (one per event, full GameEvent each)" % m["count"], "net",
                               {"file": "src/net/net.c", "function": "net_host_send_events"},
                               {p.key: {"messages": m["count"], "bytes": m["bytes"], "snapshots": snaps}},
                               {"bytes_s": m["count"] * 10 / wall_s}, "medium", "M",
                               "Batch a tick's events per client into one reliable message (count + events): same events, fewer headers / acks.",
                               scene=p.key))
    return out


# ---------------------------------------------------------------------------------------- trade-offs


@rule("T01", "Heavy passes (quality trade-offs, not recommended)")
def t01_heavy(p, ctx):
    scopes = sorted(((scope_stats(p, s)["ms"], s["name"]) for s in p.gpu_scopes() if s["name"] != "(unscoped)"), reverse=True)[:4]
    out = []
    for ms, name in scopes:
        if ms < 0.3:
            continue
        out.append(Finding("T01", "heavy:" + name, "'%s' costs %.2f ms: a lower setting would save part of it" % (name, ms), "render", {"pass": name},
                           {p.key: {"gpu_ms": round(ms, 3)}}, {"gpu_ms": ms * 0.4}, "medium", "S",
                           "Lower its resolution / sample count / distance (changes the image: Engine Fidelity's job, not this audit's).",
                           fidelity="tradeoff", scene=p.key, verify="changes the image: not a fidelity-neutral fix"))
    return out


def run(profiles, ctx=None):
    ctx = ctx or {}
    out = []
    for p in profiles:
        for r in RULES:
            try:
                out.extend(r(p, ctx))
            except (KeyError, TypeError, ValueError, ZeroDivisionError, IndexError) as e:
                out.append(Finding("R00", "%s:%s" % (r.rid, p.key), "rule %s failed on %s: %s" % (r.rid, p.path, e), "tools/perf_audit", {}, {}, {}, "low",
                                   "S", "Fix the rule (tools/perf_audit/pa/rules_runtime.py).", scene=p.key, verify="tool", below_noise=True))
    return out
