// Engine profiler (profiler.h, docs/PERF_AUDIT.md). Every thread records into its own event buffer
// (no locks on the hot path); the main thread turns the buffers into per-frame aggregates and writes
// the JSON profile when a capture ends. Wrapped allocations (prof_alloc.h) land in a per-thread
// call-site table. This file uses the real allocator: (malloc)(n) skips the accounting macros.
#include "engine.h"
#include "profiler.h"
#include "cvar.h"
#include "log.h"
#include "platform.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#endif

#define PROF_MAX_THREADS 32
#define PROF_STACK 64
#define PROF_SITES 1024             // per thread, power of two
#define PROF_MAX_SERIES 64
#define PROF_MAX_SECTIONS 16
#define PROF_MAX_HOOKS 16
#define PROF_MAX_KEYS 1024          // aggregation table (zone name x thread class), power of two
#define PROF_MAX_JOBS 256
#define PROF_STARTUP_EVENTS 16384
#define PROF_MAX_STARTUP 4096

volatile int dg_prof_recording;
u32 dg_prof_fix_off;

enum { EV_BEGIN, EV_END };
enum { CAT_ZONE, CAT_WAIT, CAT_JOB, CAT_PARFOR };

typedef struct ProfEvent {
    u64 t;
    const char *name;
    const void *fn;
    u32 frame;
    u8 kind, cat;
    u16 depth;
    u32 aux_us;               // jobs: queue latency
} ProfEvent;

typedef struct ProfSite { const char *file; const char *zone; int line; u32 count, frames, last_frame; u64 bytes; } ProfSite;

typedef struct ProfThread {
    int index;
    char name[32];
    ProfEvent *ev;
    u32 count, cap;
    bool truncated;
    int depth;
    const char *stack[PROF_STACK];
    i64 counters[PC_COUNT];
    ProfSite *sites;
    u32 site_count;
    bool sites_full;
} ProfThread;

typedef enum { PS_IDLE, PS_STARTUP, PS_WARMUP, PS_RECORD } ProfState;

typedef struct ProfSeries { char name[40]; float *v; } ProfSeries;
typedef struct ProfSection { char key[24]; ProfSectionFn fn; } ProfSection;

static const char *k_wait_names[PW_COUNT] = { "gpu_fence", "acquire", "present", "readback", "jobs_wait", "parfor_tail", "io", "fps_cap" };
static const char *k_counter_names[PC_COUNT] = {
    "alloc_count", "alloc_bytes", "free_count", "memcpy_calls", "memcpy_bytes", "cvar_find", "log_lines", "io_reads", "io_bytes", "jobs", "parfor",
    "anim_eval", "anim_eval_offscreen", "skin_palettes", "skin_palette_bytes", "vfx_emitters", "vfx_emitters_culled", "audio_voices",
    "audio_voices_silent", "ui_text", "ui_verts", "draw_items", "shadow_items",
};
static const char *k_fix_ids[PFX_COUNT] = { "PF-0001", "PF-0002", "PF-0003", "PF-0004", "PF-0005" };

static struct {
    bool init;
    SDL_Mutex *lock;
    ProfThread *threads[PROF_MAX_THREADS];
    SDL_AtomicInt thread_count;
    u64 freq;
    ProfState state;
    volatile u32 frame;            // capture frame the events belong to
    ProfCaptureDesc desc;
    int warm_left, cur;
    bool done_flag;
    u64 frame_t0, record_t0;
    u64 *frame_start;              // ticks per captured frame
    double *frame_ms;
    i64 *counters;                 // [PC_COUNT][frames]
    i64 counter_last[PC_COUNT];
    ProfSeries series[PROF_MAX_SERIES];
    int series_count;
    ProfSection sections[PROF_MAX_SECTIONS];
    int section_count;
    ProfFrameFn frame_hooks[PROF_MAX_HOOKS];
    int frame_hook_count;
    void (*cap_begin[PROF_MAX_HOOKS])(int frames);
    void (*cap_end[PROF_MAX_HOOKS])(void);
    int cap_hook_count;
    double overhead_ns;
    // Startup critical path (prof_init .. first frame).
    struct { int thread; const char *name; double start_ms, dur_ms; int depth; } *startup;
    int startup_count;
    double startup_total_ms;
    Cvar *fix_disable;
} P;

static DG_THREAD_LOCAL ProfThread *tl_thread;

// ------------------------------------------------------------------ threads / events

u64 prof_ticks(void) { return SDL_GetPerformanceCounter(); }
static double ticks_ms(u64 d) { return (double)d * 1000.0 / (double)P.freq; }

static ProfThread *thread_get(void) {
    if (tl_thread) return tl_thread;
    if (!P.init) return NULL;
    SDL_LockMutex(P.lock);
    int i = SDL_GetAtomicInt(&P.thread_count);
    ProfThread *t = NULL;
    if (i < PROF_MAX_THREADS) {
        t = (ProfThread *)(calloc)(1, sizeof(ProfThread));
        if (t) {
            t->index = i;
            snprintf(t->name, sizeof(t->name), i == 0 ? "main" : "thread_%d", i);
            P.threads[i] = t;
            SDL_SetAtomicInt(&P.thread_count, i + 1);
        }
    }
    SDL_UnlockMutex(P.lock);
    tl_thread = t;
    return t;
}

void prof_thread_name(const char *name) {
    ProfThread *t = thread_get();
    if (t) dg_strcpy(t->name, sizeof(t->name), name);
}

static u32 cap_for(const ProfThread *t) {
    if (P.state == PS_STARTUP) return PROF_STARTUP_EVENTS;
    u32 frames = (u32)DG_MAX(P.desc.frames, 1);
    return t->index == 0 ? DG_CLAMP(frames * 3000u, 65536u, 4u << 20) : DG_CLAMP(frames * 800u, 16384u, 1u << 20);
}

static void push(ProfThread *t, u8 kind, u8 cat, const char *name, const void *fn, u32 aux) {
    if (t->count >= t->cap) {
        if (t->cap == 0 && !t->ev) {
            t->cap = cap_for(t);
            t->ev = (ProfEvent *)(malloc)(sizeof(ProfEvent) * t->cap);
            if (!t->ev) { t->cap = 0; t->truncated = true; return; }
        } else {
            t->truncated = true;
            return;
        }
    }
    ProfEvent *e = &t->ev[t->count++];
    e->t = SDL_GetPerformanceCounter();
    e->name = name;
    e->fn = fn;
    e->frame = P.frame;
    e->kind = kind;
    e->cat = cat;
    e->depth = (u16)t->depth;
    e->aux_us = aux;
}

static void begin(u8 cat, const char *name, const void *fn, u32 aux) {
    ProfThread *t = thread_get();
    if (!t) return;
    push(t, EV_BEGIN, cat, name, fn, aux);
    if (t->depth < PROF_STACK) t->stack[t->depth] = name;
    t->depth++;
}

static void end(void) {
    ProfThread *t = thread_get();
    if (!t || t->depth <= 0) return;   // begun before the recording started
    t->depth--;
    push(t, EV_END, 0, NULL, NULL, 0);
}

void prof_zone_begin(const char *name) { begin(CAT_ZONE, name, NULL, 0); }
void prof_zone_end(void) { end(); }
void prof_wait_begin(ProfWait w) { begin(CAT_WAIT, k_wait_names[DG_CLAMP((int)w, 0, PW_COUNT - 1)], NULL, 0); }
void prof_wait_end(void) { end(); }

void prof_job_begin(const void *fn, u64 submit, bool parfor) {
    if (!dg_prof_recording) return;
    u64 now = SDL_GetPerformanceCounter();
    double q = submit && now > submit ? ticks_ms(now - submit) * 1000.0 : 0.0;
    begin(parfor ? CAT_PARFOR : CAT_JOB, NULL, fn, (u32)DG_MIN(q, 4.0e9));
}
void prof_job_end(void) { if (dg_prof_recording) end(); }

void prof_count_slow(ProfCounter c, i64 v) {
    ProfThread *t = thread_get();
    if (t && (unsigned)c < PC_COUNT) t->counters[c] += v;
}

void prof_series(const char *name, double v) {
    if (P.state != PS_RECORD || P.cur < 0 || P.cur >= P.desc.frames) return;
    int k = 0;
    while (k < P.series_count && strcmp(P.series[k].name, name)) k++;
    if (k == P.series_count) {
        if (k >= PROF_MAX_SERIES) return;
        dg_strcpy(P.series[k].name, sizeof(P.series[k].name), name);
        P.series[k].v = (float *)(calloc)((size_t)P.desc.frames, sizeof(float));
        for (int i = 0; P.series[k].v && i < P.desc.frames; i++) P.series[k].v[i] = -1.0f;   // -1: not sampled that frame
        P.series_count++;
    }
    if (P.series[k].v) P.series[k].v[P.cur] = (float)v;
}

const char *prof_fix_id(ProfFix f) { return (unsigned)f < PFX_COUNT ? k_fix_ids[f] : "?"; }

// ------------------------------------------------------------------ allocations (prof_alloc.h)

static void note_alloc(size_t n, const char *file, int line) {
    ProfThread *t = thread_get();
    if (!t) return;
    t->counters[PC_ALLOC_COUNT]++;
    t->counters[PC_ALLOC_BYTES] += (i64)n;
    if (!t->sites) t->sites = (ProfSite *)(calloc)(PROF_SITES, sizeof(ProfSite));
    if (!t->sites) return;
    u32 h = dg_hash_u32((u32)(uintptr_t)file ^ (u32)line * 2654435761u) & (PROF_SITES - 1);
    for (u32 probe = 0; probe < PROF_SITES; probe++, h = (h + 1) & (PROF_SITES - 1)) {
        ProfSite *s = &t->sites[h];
        if (!s->file) {
            if (t->site_count >= PROF_SITES * 3 / 4) { t->sites_full = true; return; }
            s->file = file;
            s->line = line;
            s->zone = t->depth > 0 && t->depth <= PROF_STACK ? t->stack[t->depth - 1] : NULL;
            s->last_frame = ~0u;
            t->site_count++;
        }
        if (s->file == file && s->line == line) {
            s->count++;
            s->bytes += n;
            if (s->last_frame != P.frame) { s->frames++; s->last_frame = P.frame; }
            return;
        }
    }
}

void *dg_prof_malloc(size_t n, const char *file, int line) {
    void *p = (malloc)(n);
    if (dg_prof_recording) note_alloc(n, file, line);
    return p;
}
void *dg_prof_calloc(size_t c, size_t n, const char *file, int line) {
    void *p = (calloc)(c, n);
    if (dg_prof_recording) note_alloc(c * n, file, line);
    return p;
}
void *dg_prof_realloc(void *p, size_t n, const char *file, int line) {
    void *q = (realloc)(p, n);
    if (dg_prof_recording) note_alloc(n, file, line);
    return q;
}
void dg_prof_free(void *p) {
    if (p && dg_prof_recording) prof_count_slow(PC_FREE_COUNT, 1);
    (free)(p);
}
void dg_prof_memcpy_count(size_t n) {
    ProfThread *t = thread_get();
    if (!t) return;
    t->counters[PC_MEMCPY_CALLS]++;
    t->counters[PC_MEMCPY_BYTES] += (i64)n;
}

// ------------------------------------------------------------------ registration

void prof_add_section(const char *key, ProfSectionFn fn) {
    if (P.section_count >= PROF_MAX_SECTIONS) return;
    dg_strcpy(P.sections[P.section_count].key, sizeof(P.sections[0].key), key);
    P.sections[P.section_count++].fn = fn;
}
void prof_add_frame_hook(ProfFrameFn fn) { if (P.frame_hook_count < PROF_MAX_HOOKS) P.frame_hooks[P.frame_hook_count++] = fn; }
void prof_add_capture_hook(void (*b)(int frames), void (*e)(void)) {
    if (P.cap_hook_count >= PROF_MAX_HOOKS) return;
    P.cap_begin[P.cap_hook_count] = b;
    P.cap_end[P.cap_hook_count++] = e;
}

// ------------------------------------------------------------------ JSON helpers

void prof_json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

void prof_json_floats(FILE *f, const float *v, int n) {
    fputc('[', f);
    for (int i = 0; i < n; i++) fprintf(f, i ? ",%.4g" : "%.4g", (double)v[i]);
    fputc(']', f);
}

static void json_doubles(FILE *f, const double *v, int n) {
    fputc('[', f);
    for (int i = 0; i < n; i++) fprintf(f, i ? ",%.4f" : "%.4f", v[i]);
    fputc(']', f);
}

// ------------------------------------------------------------------ symbols (job functions)

static const char *symbol_name(const void *fn, char *buf, size_t size) {
#if defined(_WIN32)
    static bool sym_init;
    if (!sym_init) {
        sym_init = true;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);   // may already be initialised (crash.c): harmless
    }
    union { SYMBOL_INFO info; char raw[sizeof(SYMBOL_INFO) + 128]; } sym;
    memset(&sym, 0, sizeof(sym));
    sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
    sym.info.MaxNameLen = 127;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)fn, &disp, &sym.info)) {
        dg_strcpy(buf, size, sym.info.Name);
        return buf;
    }
#endif
    snprintf(buf, size, "fn_%p", fn);
    return buf;
}

// ------------------------------------------------------------------ process memory

static double process_private_mb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) return (double)pmc.PrivateUsage / (1024.0 * 1024.0);
    return -1.0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1.0;
    unsigned long size = 0, rss = 0;
    int n = fscanf(f, "%lu %lu", &size, &rss);
    fclose(f);
    return n == 2 ? (double)rss * 4096.0 / (1024.0 * 1024.0) : -1.0;
#endif
}

// ------------------------------------------------------------------ aggregation

typedef struct AggKey {
    char name[80];
    int tclass;                  // 0 main, 1 workers, 2+ other threads (index + 2)
    u8 cat;
    float *incl, *excl;
    u32 *calls;
    double total_ms, max_ms;
} AggKey;

typedef struct JobAgg { const void *fn; bool parfor; u32 count; double total_ms, max_ms, queue_ms, queue_max_ms; } JobAgg;

typedef struct Agg {
    AggKey *keys;                // PROF_MAX_KEYS, open addressing
    int key_count;
    JobAgg jobs[PROF_MAX_JOBS];
    int job_count;
    float *busy;                 // [threads][frames]
    struct { const void *fn; char name[96]; } syms[PROF_MAX_JOBS];
    int sym_count;
} Agg;

static const char *sym_cached(Agg *a, const void *fn) {
    for (int i = 0; i < a->sym_count; i++) if (a->syms[i].fn == fn) return a->syms[i].name;
    if (a->sym_count >= PROF_MAX_JOBS) return "fn_?";
    a->syms[a->sym_count].fn = fn;
    return symbol_name(fn, a->syms[a->sym_count++].name, sizeof(a->syms[0].name));
}

static int thread_class(const ProfThread *t) {
    if (t->index == 0) return 0;
    if (!strncmp(t->name, "dg_worker", 9)) return 1;
    return t->index + 2;
}

static void event_label(Agg *a, const ProfEvent *e, char *out, size_t size) {
    switch (e->cat) {
    case CAT_WAIT: snprintf(out, size, "wait:%s", e->name); break;
    case CAT_JOB: snprintf(out, size, "job:%s", sym_cached(a, e->fn)); break;
    case CAT_PARFOR: snprintf(out, size, "parfor:%s", sym_cached(a, e->fn)); break;
    default: snprintf(out, size, "%s", e->name ? e->name : "?"); break;
    }
}

static AggKey *agg_key(Agg *a, const char *label, int tclass, u8 cat, int frames) {
    u32 h = (dg_hash_str(label) ^ (u32)tclass * 0x9E3779B9u) & (PROF_MAX_KEYS - 1);
    for (int probe = 0; probe < PROF_MAX_KEYS; probe++, h = (h + 1) & (PROF_MAX_KEYS - 1)) {
        AggKey *k = &a->keys[h];
        if (!k->incl) {
            if (a->key_count >= PROF_MAX_KEYS * 3 / 4) return NULL;
            dg_strcpy(k->name, sizeof(k->name), label);
            k->tclass = tclass;
            k->cat = cat;
            k->incl = (float *)(calloc)((size_t)frames, sizeof(float));
            k->excl = (float *)(calloc)((size_t)frames, sizeof(float));
            k->calls = (u32 *)(calloc)((size_t)frames, sizeof(u32));
            a->key_count++;
            return k;
        }
        if (k->tclass == tclass && !strcmp(k->name, label)) return k;
    }
    return NULL;
}

static JobAgg *job_agg(Agg *a, const void *fn, bool parfor) {
    for (int i = 0; i < a->job_count; i++) if (a->jobs[i].fn == fn && a->jobs[i].parfor == parfor) return &a->jobs[i];
    if (a->job_count >= PROF_MAX_JOBS) return NULL;
    JobAgg *j = &a->jobs[a->job_count++];
    memset(j, 0, sizeof(*j));
    j->fn = fn;
    j->parfor = parfor;
    return j;
}

// Walks every thread's events: inclusive / exclusive time and calls per key and frame, job stats,
// worker busy time per frame.
static void aggregate(Agg *a, int frames, int nthreads) {
    a->keys = (AggKey *)(calloc)(PROF_MAX_KEYS, sizeof(AggKey));
    a->busy = (float *)(calloc)((size_t)nthreads * (size_t)frames, sizeof(float));
    if (!a->keys || !a->busy) return;
    for (int ti = 0; ti < nthreads; ti++) {
        ProfThread *t = P.threads[ti];
        if (!t || !t->ev) continue;
        int tclass = thread_class(t);
        struct { u32 ev; double child; } st[PROF_STACK];
        int sp = 0;
        for (u32 i = 0; i < t->count; i++) {
            const ProfEvent *e = &t->ev[i];
            if (e->kind == EV_BEGIN) {
                if (sp < PROF_STACK) { st[sp].ev = i; st[sp].child = 0; }
                sp++;
                continue;
            }
            if (sp <= 0) continue;
            sp--;
            if (sp >= PROF_STACK) continue;
            const ProfEvent *b = &t->ev[st[sp].ev];
            double dur = ticks_ms(e->t - b->t);
            if (sp > 0 && sp - 1 < PROF_STACK) st[sp - 1].child += dur;
            if (b->frame >= (u32)frames) continue;
            char label[80];
            event_label(a, b, label, sizeof(label));
            AggKey *k = agg_key(a, label, tclass, b->cat, frames);
            if (k) {
                k->incl[b->frame] += (float)dur;
                k->excl[b->frame] += (float)(dur - st[sp].child);
                k->calls[b->frame]++;
                k->total_ms += dur;
                if (dur > k->max_ms) k->max_ms = dur;
            }
            if (b->cat == CAT_JOB || b->cat == CAT_PARFOR) {
                JobAgg *j = job_agg(a, b->fn, b->cat == CAT_PARFOR);
                if (j) {
                    j->count++;
                    j->total_ms += dur;
                    if (dur > j->max_ms) j->max_ms = dur;
                    j->queue_ms += b->aux_us / 1000.0;
                    if (b->aux_us / 1000.0 > j->queue_max_ms) j->queue_max_ms = b->aux_us / 1000.0;
                }
                if (sp == 0) a->busy[ti * frames + (int)b->frame] += (float)dur;
            }
        }
    }
}

static void agg_free(Agg *a) {
    if (a->keys)
        for (int i = 0; i < PROF_MAX_KEYS; i++) {
            (free)(a->keys[i].incl);
            (free)(a->keys[i].excl);
            (free)(a->keys[i].calls);
        }
    (free)(a->keys);
    (free)(a->busy);
}

// ------------------------------------------------------------------ writing

static void write_timeline(FILE *f, Agg *a, int frame, int nthreads) {
    fprintf(f, "{\"frame\":%d,\"ms\":%.4f,\"events\":[", frame, P.frame_ms[frame]);
    u64 t0 = P.frame_start[frame];
    bool first = true;
    for (int ti = 0; ti < nthreads; ti++) {
        ProfThread *t = P.threads[ti];
        if (!t || !t->ev) continue;
        u32 st[PROF_STACK];
        int sp = 0;
        for (u32 i = 0; i < t->count; i++) {
            const ProfEvent *e = &t->ev[i];
            if (e->kind == EV_BEGIN) {
                if (sp < PROF_STACK) st[sp] = i;
                sp++;
                continue;
            }
            if (sp <= 0) continue;
            sp--;
            if (sp >= PROF_STACK) continue;
            const ProfEvent *b = &t->ev[st[sp]];
            if (b->frame != (u32)frame) continue;
            char label[80];
            event_label(a, b, label, sizeof(label));
            double start = b->t >= t0 ? ticks_ms(b->t - t0) * 1000.0 : -ticks_ms(t0 - b->t) * 1000.0;
            fprintf(f, "%s[%d,", first ? "" : ",", ti);
            prof_json_str(f, label);
            fprintf(f, ",%.1f,%.1f,%d]", start, ticks_ms(e->t - b->t) * 1000.0, sp);
            first = false;
        }
    }
    fprintf(f, "]}");
}

static int cmp_double_idx(const void *x, const void *y) {
    double a = P.frame_ms[*(const int *)x], b = P.frame_ms[*(const int *)y];
    return a < b ? -1 : (a > b ? 1 : 0);
}

typedef struct SiteOut { const char *file, *zone; int line; u32 count, frames; u64 bytes; } SiteOut;
static int cmp_site(const void *x, const void *y) {
    const SiteOut *a = (const SiteOut *)x, *b = (const SiteOut *)y;
    return a->count != b->count ? (a->count > b->count ? -1 : 1) : 0;
}

static void write_profile(void) {
    int frames = P.desc.frames, nthreads = SDL_GetAtomicInt(&P.thread_count);
    FILE *f = fopen(P.desc.path, "wb");
    if (!f) { log_error("prof: cannot write %s", P.desc.path); return; }
    Agg a;
    memset(&a, 0, sizeof(a));
    aggregate(&a, frames, nthreads);

    fprintf(f, "{\n\"format\":\"dg-perf-profile\",\"version\":1,\n\"meta\":{");
    fprintf(f, "\"scene\":");
    prof_json_str(f, P.desc.scene);
    fprintf(f, ",\"label\":");
    prof_json_str(f, P.desc.label);
    fprintf(f, ",\"fidelity\":");
    prof_json_str(f, P.desc.fidelity);
    fprintf(f, ",\"build\":");
    prof_json_str(f, DG_VERSION);
#if defined(NDEBUG)
    fprintf(f, ",\"config\":\"release\"");
#else
    fprintf(f, ",\"config\":\"debug\"");
#endif
#if defined(_WIN32)
    fprintf(f, ",\"os\":\"windows\"");
#else
    fprintf(f, ",\"os\":\"other\"");
#endif
    fprintf(f, ",\"frames\":%d,\"warmup\":%d,\"cpu_threads\":%d,\"prof_overhead_ns\":%.1f,\"wall_ms\":%.3f", frames, P.desc.warmup,
            SDL_GetNumLogicalCPUCores(), P.overhead_ns, ticks_ms(prof_ticks() - P.record_t0));
    bool truncated = false;
    for (int ti = 0; ti < nthreads; ti++) truncated |= P.threads[ti] && P.threads[ti]->truncated;
    fprintf(f, ",\"truncated\":%s,\"fixes_disabled\":%u}", truncated ? "true" : "false", dg_prof_fix_off);

    fprintf(f, ",\n\"frames\":{\"cpu_ms\":");
    json_doubles(f, P.frame_ms, frames);
    fprintf(f, "}");

    fprintf(f, ",\n\"series\":{");
    for (int k = 0; k < P.series_count; k++) {
        fprintf(f, "%s\n ", k ? "," : "");
        prof_json_str(f, P.series[k].name);
        fputc(':', f);
        prof_json_floats(f, P.series[k].v, frames);
    }
    fprintf(f, "}");

    fprintf(f, ",\n\"counters\":{");
    for (int c = 0; c < PC_COUNT; c++) {
        fprintf(f, "%s\n \"%s\":[", c ? "," : "", k_counter_names[c]);
        for (int i = 0; i < frames; i++) fprintf(f, i ? ",%lld" : "%lld", (long long)P.counters[c * frames + i]);
        fputc(']', f);
    }
    fprintf(f, "}");

    fprintf(f, ",\n\"threads\":[");
    for (int ti = 0; ti < nthreads; ti++) {
        ProfThread *t = P.threads[ti];
        fprintf(f, "%s\n {\"index\":%d,\"name\":", ti ? "," : "", ti);
        prof_json_str(f, t ? t->name : "?");
        fprintf(f, ",\"class\":%d,\"events\":%u,\"truncated\":%s,\"job_busy_ms\":", t ? thread_class(t) : -1, t ? t->count : 0, t && t->truncated ? "true" : "false");
        if (a.busy) prof_json_floats(f, a.busy + ti * frames, frames);
        else fprintf(f, "[]");
        fputc('}', f);
    }
    fprintf(f, "]");

    fprintf(f, ",\n\"zones\":[");
    bool first = true;
    for (int i = 0; a.keys && i < PROF_MAX_KEYS; i++) {
        AggKey *k = &a.keys[i];
        if (!k->incl) continue;
        fprintf(f, "%s\n {\"name\":", first ? "" : ",");
        prof_json_str(f, k->name);
        const char *cls = k->tclass == 0 ? "main" : (k->tclass == 1 ? "workers" : (P.threads[k->tclass - 2] ? P.threads[k->tclass - 2]->name : "?"));
        fprintf(f, ",\"thread\":");
        prof_json_str(f, cls);
        static const char *cats[] = { "zone", "wait", "job", "parfor" };
        fprintf(f, ",\"cat\":\"%s\",\"total_ms\":%.4f,\"max_ms\":%.4f,\"incl_ms\":", cats[k->cat & 3], k->total_ms, k->max_ms);
        prof_json_floats(f, k->incl, frames);
        fprintf(f, ",\"excl_ms\":");
        prof_json_floats(f, k->excl, frames);
        fprintf(f, ",\"calls\":[");
        for (int j = 0; j < frames; j++) fprintf(f, j ? ",%u" : "%u", k->calls[j]);
        fprintf(f, "]}");
        first = false;
    }
    fprintf(f, "]");

    fprintf(f, ",\n\"jobs\":[");
    for (int i = 0; i < a.job_count; i++) {
        JobAgg *j = &a.jobs[i];
        fprintf(f, "%s\n {\"fn\":", i ? "," : "");
        prof_json_str(f, sym_cached(&a, j->fn));
        fprintf(f, ",\"parfor\":%s,\"count\":%u,\"total_ms\":%.4f,\"max_ms\":%.4f,\"queue_ms_avg\":%.4f,\"queue_ms_max\":%.4f}", j->parfor ? "true" : "false", j->count,
                j->total_ms, j->max_ms, j->count ? j->queue_ms / j->count : 0.0, j->queue_max_ms);
    }
    fprintf(f, "]");

    // Allocation call sites, merged over threads, most frequent first.
    int cap = 0;
    for (int ti = 0; ti < nthreads; ti++) if (P.threads[ti]) cap += (int)P.threads[ti]->site_count;
    SiteOut *so = cap ? (SiteOut *)(calloc)((size_t)cap, sizeof(SiteOut)) : NULL;
    int ns = 0;
    bool sites_full = false;
    for (int ti = 0; so && ti < nthreads; ti++) {
        ProfThread *t = P.threads[ti];
        if (!t || !t->sites) continue;
        sites_full |= t->sites_full;
        for (int s = 0; s < PROF_SITES; s++) {
            ProfSite *x = &t->sites[s];
            if (!x->file || !x->count) continue;
            int k = 0;
            while (k < ns && !(so[k].file == x->file && so[k].line == x->line)) k++;
            if (k == ns) { so[ns] = (SiteOut){ x->file, x->zone, x->line, 0, 0, 0 }; ns++; }
            so[k].count += x->count;
            so[k].bytes += x->bytes;
            so[k].frames = DG_MAX(so[k].frames, x->frames);
        }
    }
    if (so) qsort(so, (size_t)ns, sizeof(SiteOut), cmp_site);
    fprintf(f, ",\n\"alloc_sites_full\":%s,\"alloc_sites\":[", sites_full ? "true" : "false");
    for (int i = 0; i < DG_MIN(ns, 300); i++) {
        char site[300];
        snprintf(site, sizeof(site), "%s:%d", so[i].file, so[i].line);
        fprintf(f, "%s\n {\"site\":", i ? "," : "");
        prof_json_str(f, site);
        fprintf(f, ",\"zone\":");
        prof_json_str(f, so[i].zone ? so[i].zone : "");
        fprintf(f, ",\"count\":%u,\"bytes\":%llu,\"frames\":%u}", so[i].count, (unsigned long long)so[i].bytes, so[i].frames);
    }
    fprintf(f, "]");
    (free)(so);

    fprintf(f, ",\n\"startup\":{\"total_ms\":%.3f,\"zones\":[", P.startup_total_ms);
    for (int i = 0; i < P.startup_count; i++) {
        fprintf(f, "%s\n [%d,", i ? "," : "", P.startup[i].thread);
        prof_json_str(f, P.startup[i].name ? P.startup[i].name : "?");
        fprintf(f, ",%.3f,%.3f,%d]", P.startup[i].start_ms, P.startup[i].dur_ms, P.startup[i].depth);
    }
    fprintf(f, "]}");

    // Timelines: the median frame and the worst frame (flame graph / pass timeline HTML).
    fprintf(f, ",\n\"timeline\":[");
    int *order = (int *)(malloc)(sizeof(int) * (size_t)frames);
    if (order && frames > 0) {
        for (int i = 0; i < frames; i++) order[i] = i;
        qsort(order, (size_t)frames, sizeof(int), cmp_double_idx);
        int picks[2] = { order[frames / 2], order[frames - 1] };
        int n = DG_CLAMP(P.desc.timeline_frames, 1, 2);
        for (int i = 0; i < n; i++) {
            if (i && picks[1] == picks[0]) break;
            fprintf(f, "%s\n ", i ? "," : "");
            write_timeline(f, &a, picks[i], nthreads);
        }
    }
    (free)(order);
    fprintf(f, "]");

    for (int s = 0; s < P.section_count; s++) {
        fprintf(f, ",\n\"%s\":", P.sections[s].key);
        P.sections[s].fn(f);
    }
    fprintf(f, "\n}\n");
    fclose(f);
    agg_free(&a);
    log_info("prof: wrote %s (%d frames, %d threads%s)", P.desc.path, frames, nthreads, truncated ? ", TRUNCATED: raise the buffers" : "");
}

// ------------------------------------------------------------------ capture state machine

static void quiesce(void) {
    // Workers that read the flag just before it dropped finish their push: wait a moment.
    u64 t0 = prof_ticks();
    while (ticks_ms(prof_ticks() - t0) < 2.0) SDL_CPUPauseInstruction();
}

static void reset_threads(bool free_buffers) {
    int n = SDL_GetAtomicInt(&P.thread_count);
    for (int i = 0; i < n; i++) {
        ProfThread *t = P.threads[i];
        if (!t) continue;
        if (free_buffers) {
            (free)(t->ev);
            t->ev = NULL;
            t->cap = 0;
        }
        t->count = 0;
        t->truncated = false;
        t->depth = 0;
        if (t->sites) memset(t->sites, 0, sizeof(ProfSite) * PROF_SITES);
        t->site_count = 0;
        t->sites_full = false;
    }
}

static i64 counter_sum(int c) {
    i64 s = 0;
    int n = SDL_GetAtomicInt(&P.thread_count);
    for (int i = 0; i < n; i++) if (P.threads[i]) s += P.threads[i]->counters[c];
    return s;
}

static void finish_startup(void) {
    dg_prof_recording = 0;
    quiesce();
    u64 now = prof_ticks();
    P.startup_total_ms = ticks_ms(now - P.record_t0);
    P.startup = (void *)(calloc)(PROF_MAX_STARTUP, sizeof(*P.startup));
    int n = SDL_GetAtomicInt(&P.thread_count);
    for (int ti = 0; P.startup && ti < n; ti++) {
        ProfThread *t = P.threads[ti];
        if (!t || !t->ev) continue;
        u32 st[PROF_STACK];
        int sp = 0;
        for (u32 i = 0; i < t->count && P.startup_count < PROF_MAX_STARTUP; i++) {
            const ProfEvent *e = &t->ev[i];
            if (e->kind == EV_BEGIN) {
                if (sp < PROF_STACK) st[sp] = i;
                sp++;
                continue;
            }
            if (sp <= 0) continue;
            sp--;
            if (sp >= PROF_STACK) continue;
            const ProfEvent *b = &t->ev[st[sp]];
            double dur = ticks_ms(e->t - b->t);
            if (dur < 0.05 && sp > 1) continue;   // keep the file small: tiny nested zones dropped
            P.startup[P.startup_count].thread = ti;
            P.startup[P.startup_count].name = b->cat == CAT_ZONE ? b->name : (b->cat == CAT_WAIT ? "wait" : "job");
            P.startup[P.startup_count].start_ms = ticks_ms(b->t - P.record_t0);
            P.startup[P.startup_count].dur_ms = dur;
            P.startup[P.startup_count].depth = sp;
            P.startup_count++;
        }
    }
    reset_threads(true);
    P.state = PS_IDLE;
    log_info("prof: startup %.1f ms to the first frame (%d zones kept for the profile's startup section)", P.startup_total_ms, P.startup_count);
}

static void free_capture(void) {
    (free)(P.frame_start);
    (free)(P.frame_ms);
    (free)(P.counters);
    P.frame_start = NULL;
    P.frame_ms = NULL;
    P.counters = NULL;
    for (int k = 0; k < P.series_count; k++) (free)(P.series[k].v);
    P.series_count = 0;
}

static void start_recording(void) {
    int frames = P.desc.frames;
    P.frame_start = (u64 *)(calloc)((size_t)frames, sizeof(u64));
    P.frame_ms = (double *)(calloc)((size_t)frames, sizeof(double));
    P.counters = (i64 *)(calloc)((size_t)frames * PC_COUNT, sizeof(i64));
    if (!P.frame_start || !P.frame_ms || !P.counters) {
        log_error("prof: out of memory for %d frames", frames);
        free_capture();
        P.state = PS_IDLE;
        return;
    }
    // Probe cost: begin / end pairs into the main thread buffer (discarded below).
    P.frame = 0;
    P.state = PS_RECORD;
    dg_prof_recording = 1;
    u64 t0 = prof_ticks();
    for (int i = 0; i < 2000; i++) { prof_zone_begin("prof_overhead"); prof_zone_end(); }
    P.overhead_ns = ticks_ms(prof_ticks() - t0) * 1.0e6 / 2000.0;
    dg_prof_recording = 0;
    quiesce();
    reset_threads(true);
    for (int c = 0; c < PC_COUNT; c++) P.counter_last[c] = counter_sum(c);
    for (int h = 0; h < P.cap_hook_count; h++) if (P.cap_begin[h]) P.cap_begin[h](frames);
    P.cur = 0;
    P.frame = 0;
    P.record_t0 = P.frame_t0 = prof_ticks();
    P.frame_start[0] = P.frame_t0;
    dg_prof_recording = 1;
}

static void end_frame(u64 now) {
    int i = P.cur;
    P.frame_ms[i] = ticks_ms(now - P.frame_t0);
    for (int c = 0; c < PC_COUNT; c++) {
        i64 s = counter_sum(c);
        P.counters[c * P.desc.frames + i] = s - P.counter_last[c];
        P.counter_last[c] = s;
    }
    if ((i & 3) == 0) prof_series("process_mb", process_private_mb());
    for (int h = 0; h < P.frame_hook_count; h++) P.frame_hooks[h](i);
}

void prof_frame_mark(void) {
    if (!P.init) return;
    P.done_flag = false;
    if (P.state == PS_STARTUP) { finish_startup(); return; }
    if (P.state == PS_WARMUP) {
        if (--P.warm_left <= 0) start_recording();
        return;
    }
    if (P.state != PS_RECORD) return;
    u64 now = prof_ticks();
    end_frame(now);
    if (++P.cur >= P.desc.frames) {
        dg_prof_recording = 0;
        quiesce();
        for (int h = 0; h < P.cap_hook_count; h++) if (P.cap_end[h]) P.cap_end[h]();
        write_profile();
        reset_threads(true);
        free_capture();
        P.state = PS_IDLE;
        P.cur = -1;
        P.done_flag = true;
        return;
    }
    P.frame = (u32)P.cur;
    P.frame_t0 = now;
    P.frame_start[P.cur] = now;
}

bool prof_capture_start(const ProfCaptureDesc *d) {
    if (!P.init || P.state == PS_WARMUP || P.state == PS_RECORD) { log_warn("prof: a capture is already running"); return false; }
    if (P.state == PS_STARTUP) finish_startup();
    P.desc = *d;
    P.desc.frames = DG_CLAMP(P.desc.frames, 1, 100000);
    P.desc.warmup = DG_MAX(P.desc.warmup, 1);
    if (P.desc.timeline_frames <= 0) P.desc.timeline_frames = 2;
    P.warm_left = P.desc.warmup;
    P.cur = -1;
    P.state = PS_WARMUP;
    log_info("prof: capture '%s' (%s, %s): %d warmup + %d frames -> %s", P.desc.scene, P.desc.label, P.desc.fidelity, P.desc.warmup, P.desc.frames, P.desc.path);
    return true;
}

bool prof_capture_active(void) { return P.state == PS_WARMUP || P.state == PS_RECORD; }
bool prof_capture_done(void) { return P.done_flag; }
int prof_capture_frame(void) { return P.state == PS_RECORD ? P.cur : -1; }
const char *prof_capture_path(void) { return P.desc.path; }

// ------------------------------------------------------------------ init / commands

void prof_init(void) {
    memset(&P, 0, sizeof(P));
    P.lock = SDL_CreateMutex();
    P.freq = SDL_GetPerformanceFrequency();
    P.init = true;
    P.cur = -1;
    tl_thread = NULL;
    thread_get();   // the main thread is thread 0
    P.state = PS_STARTUP;
    P.record_t0 = prof_ticks();
    dg_prof_recording = 1;
}

static void fix_disable_changed(Cvar *cv) {
    u32 m = 0;
    for (int f = 0; f < PFX_COUNT; f++)
        if (strstr(cv->str, k_fix_ids[f]) || !strcmp(cv->str, "all")) m |= 1u << f;
    dg_prof_fix_off = m;
}

static void cmd_capture(int argc, const char **argv) {
    if (argc < 3) { log_info("usage: prof_capture <frames> <path.json> [label] [warmup] [scene]"); return; }
    ProfCaptureDesc d;
    memset(&d, 0, sizeof(d));
    d.frames = atoi(argv[1]);
    dg_strcpy(d.path, sizeof(d.path), argv[2]);
    dg_strcpy(d.scene, sizeof(d.scene), argc > 5 ? argv[5] : "manual");
    dg_strcpy(d.label, sizeof(d.label), argc > 3 ? argv[3] : "manual");
    Cvar *fid = cvar_find("r_fidelity");
    dg_strcpy(d.fidelity, sizeof(d.fidelity), fid ? fid->str : "?");
    d.warmup = argc > 4 ? atoi(argv[4]) : 30;
    prof_capture_start(&d);
}

void prof_register(void) {
    P.fix_disable = cvar_reg_string("perf_fix_disable", "", 0,
                                    "perf audit A/B: comma separated fix ids switched off (PF-0001..; all), docs/perf/FINDINGS.md; the fixes are fidelity neutral");
    P.fix_disable->on_change = fix_disable_changed;
    fix_disable_changed(P.fix_disable);
    cmd_register("prof_capture", cmd_capture, "prof_capture <frames> <path.json> [label] [warmup=30] [scene=manual]: record a JSON profile (tools/perf_audit)");
}

void prof_shutdown(void) {
    if (!P.init) return;
    dg_prof_recording = 0;
    quiesce();
    reset_threads(true);
    free_capture();
    (free)(P.startup);
    P.startup = NULL;
    P.init = false;
}
