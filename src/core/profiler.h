// Engine profiler (docs/PERF_AUDIT.md): CPU zones per thread, jobs, blocking waits, allocations,
// counters, per-frame series and captures written as one JSON profile for tools/perf_audit.
//
//   PROF_BEGIN("anim"); ... PROF_END();          zone (names are string literals, nest freely)
//   PROF_WAIT_BEGIN(PW_GPU_FENCE); ... PROF_WAIT_END();   the thread is blocked (fence, I/O, jobs)
//   prof_count(PC_CVAR_FIND, 1);                  per-frame counter
//   prof_series("gpu_frame_ms", v);               one value per captured frame
//   prof_add_section("gpu", write_fn);            a module's own JSON block in the profile
//
// Recording is off unless a capture runs (prof_capture, perf_audit, perf_verify) and during startup
// (the startup critical path, until the first frame). Off, every probe is one predictable branch.
// The profiler never changes what the engine computes: it only reads clocks and counts.
//
// Fidelity-neutral fixes found by the audit can be switched off for A/B proofs (perf_verify):
// prof_fix_enabled(PFX_*) reads perf_fix_disable (docs/perf/FINDINGS.md lists the ids).
#ifndef DG_PROFILER_H
#define DG_PROFILER_H

#include "base.h"
#include <stdio.h>

typedef enum ProfWait {
    PW_GPU_FENCE,       // frame fence (CPU ahead of the GPU)
    PW_ACQUIRE,         // swapchain image acquire
    PW_PRESENT,         // queue present
    PW_READBACK,        // screenshot / capture readback fence
    PW_JOBS_WAIT,       // jobs_wait: the caller ran out of jobs to help with
    PW_PARFOR_TAIL,     // jobs_parallel_for: waiting for the last chunks
    PW_IO,              // blocking file reads
    PW_FPS_CAP,         // the frame limiter sleeping (excluded from the frame cost)
    PW_COUNT
} ProfWait;

typedef enum ProfCounter {
    PC_ALLOC_COUNT, PC_ALLOC_BYTES, PC_FREE_COUNT, PC_MEMCPY_CALLS, PC_MEMCPY_BYTES,
    PC_CVAR_FIND, PC_LOG_LINES, PC_IO_READS, PC_IO_BYTES,
    PC_JOBS, PC_PARFOR,
    PC_ANIM_EVAL, PC_ANIM_EVAL_OFFSCREEN,          // characters posed this frame / of them, none visible (client)
    PC_SKIN_PALETTES, PC_SKIN_PALETTE_BYTES,       // palettes uploaded / bytes
    PC_SKIN_POSES,                                 // character poses drawn (draw_char: every part shares one)
    PC_VFX_EMITTERS, PC_VFX_EMITTERS_CULLED,       // emitters updated / of them, off-screen
    PC_AUDIO_VOICES, PC_AUDIO_VOICES_SILENT,       // voices mixed / of them, inaudible (zero gain)
    PC_UI_TEXT, PC_UI_VERTS,                       // text runs drawn / UI vertices
    PC_DRAW_ITEMS, PC_SHADOW_ITEMS,                // CPU draw items submitted / to the sun shadow tiles
    PC_COUNT
} ProfCounter;

// Fixes from the audit (docs/perf/FINDINGS.md), on by default; perf_fix_disable "PF-0001,PF-0005"
// turns them off for the A side of a perf_verify run. All are fidelity neutral except the trade-offs
// at the end (PFX_TRADEOFF_FIRST ..), which "all" does not include.
typedef enum ProfFix {
    PFX_SHADOW_CLEAR_SKIP,    // PF-0001: empty shadow maps not re-cleared every frame
    PFX_CVAR_HASH,            // PF-0002: cvar_find through a hash index
    // PF-0003 / PF-0004 (vgeo palette upload / page streaming scratch) went with the virtualized
    // geometry renderer; their ids are not reused.
    PFX_FG_REPORT_LAZY,       // PF-0005: frame graph validation report built on demand
    PFX_SHADOW_RCV_CULL,      // PF-0006: sun cascade casters culled to the cascade's receiver volume
    PFX_SKIN_PALETTE_SHARE,   // PF-0007: parts of one character share one palette copy
    // PF-0008 (character draw poses on the job system) was dropped: the GPU frame regressed; the id is not reused.
    PFX_CLASSIC_CULL_BATCH,   // PF-0009: the classic renderer's views culled in one parallel pass
    PFX_WIND_TILE_JOBS,       // PF-0010: the wind tile window rows on the job system
    PFX_SKIN_SHADOW_CULL,     // PF-0011: skinned shadow casters culled per sun cascade
    PFX_SHADOW_LIGHT_ORDER,   // PF-0012: classic shadow instances drawn front to back from the light
    // Visual trade-offs (not fidelity neutral: perf_verify measures them, the images differ by design):
    // perf_fix_disable "all" leaves them on, only their own id turns them off.
    PFX_SHADOW_CASTER_LIMIT,  // PF-0013: grass / small plant shadows in the near cascades only (r_shadow_caster_limit)
    PFX_COUNT
} ProfFix;
#define PFX_TRADEOFF_FIRST PFX_SHADOW_CASTER_LIMIT

#ifdef DG_PROFILE   // the engine target; unit tests compiling engine files get the no-op probes below

extern volatile int dg_prof_recording;   // the one hot flag every probe reads
extern u32 dg_prof_fix_off;              // bit per ProfFix, from perf_fix_disable

void prof_init(void);                    // right after log_init: starts the startup recording
void prof_register(void);                // commands / cvars (after the cvar system)
void prof_shutdown(void);
void prof_thread_name(const char *name); // call once on each thread that records (workers, audio)

void prof_zone_begin(const char *name);
void prof_zone_end(void);
void prof_wait_begin(ProfWait w);
void prof_wait_end(void);
// Jobs (src/core/jobs.c): fn identifies the job (symbolized when the profile is written); submit is
// the prof_ticks() of jobs_submit (queue latency).
void prof_job_begin(const void *fn, u64 submit, bool parfor);
void prof_job_end(void);
u64 prof_ticks(void);

void prof_count_slow(ProfCounter c, i64 v);
DG_INLINE void prof_count(ProfCounter c, i64 v) { if (dg_prof_recording) prof_count_slow(c, v); }
void prof_series(const char *name, double v);   // this frame's value of a named series
DG_INLINE bool prof_fix_enabled(ProfFix f) { return !(dg_prof_fix_off & (1u << f)); }
DG_INLINE bool dg_prof_recording_on(void) { return dg_prof_recording != 0; }
const char *prof_fix_id(ProfFix f);             // "PF-0001"

#define PROF_BEGIN(name) do { if (dg_prof_recording) prof_zone_begin(name); } while (0)
#define PROF_END() do { if (dg_prof_recording) prof_zone_end(); } while (0)
#define PROF_WAIT_BEGIN(w) do { if (dg_prof_recording) prof_wait_begin(w); } while (0)
#define PROF_WAIT_END() do { if (dg_prof_recording) prof_wait_end(); } while (0)

// Main loop: once at the top of every frame (closes the previous frame of a capture).
void prof_frame_mark(void);

// Captures. A capture records frames [warmup, warmup + frames) after it starts and writes path.
typedef struct ProfCaptureDesc {
    char path[256];
    char scene[48], label[48], fidelity[16];
    int frames, warmup;
    int timeline_frames;     // full event timelines kept (median + worst frames), default 2
} ProfCaptureDesc;
bool prof_capture_start(const ProfCaptureDesc *d);
bool prof_capture_active(void);
bool prof_capture_done(void);            // the last capture finished and was written this frame
int prof_capture_frame(void);            // captured frames so far (-1 during warmup)
const char *prof_capture_path(void);

// Sections: modules add their own JSON object to every profile (gpu, fg, net, memory, culling).
// The writer gets the file positioned after `"key": ` and writes one JSON value.
typedef void (*ProfSectionFn)(FILE *f);
void prof_add_section(const char *key, ProfSectionFn fn);
// Frame hooks run at every captured frame mark (sample state into the module's own arrays).
typedef void (*ProfFrameFn)(int frame);
void prof_add_frame_hook(ProfFrameFn fn);
void prof_add_capture_hook(void (*begin)(int frames), void (*end)(void));

// JSON helpers for section writers.
void prof_json_str(FILE *f, const char *s);
void prof_json_floats(FILE *f, const float *v, int n);

#else

#define PROF_BEGIN(name) ((void)0)
#define PROF_END() ((void)0)
#define PROF_WAIT_BEGIN(w) ((void)0)
#define PROF_WAIT_END() ((void)0)
DG_INLINE void prof_count(ProfCounter c, i64 v) { (void)c; (void)v; }
DG_INLINE void prof_series(const char *name, double v) { (void)name; (void)v; }
DG_INLINE bool prof_fix_enabled(ProfFix f) { (void)f; return true; }
DG_INLINE bool dg_prof_recording_on(void) { return false; }
DG_INLINE u64 prof_ticks(void) { return 0; }
DG_INLINE void prof_job_begin(const void *fn, u64 submit, bool parfor) { (void)fn; (void)submit; (void)parfor; }
DG_INLINE void prof_job_end(void) {}
DG_INLINE void prof_thread_name(const char *name) { (void)name; }

#endif

#endif
