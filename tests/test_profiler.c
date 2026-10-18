// Profiler (src/core/profiler.c, docs/PERF_AUDIT.md): a capture over frames with nested zones, jobs,
// parallel_for, waits, counters, series and wrapped allocations writes a valid JSON profile whose
// aggregates match what the test did. Run from the build dir: writes test_profiler.json.
#include "core/engine.h"
#include "core/profiler.h"
#include "core/jobs.h"
#include "core/cvar.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void spin_ms(double ms) {
    u64 t0 = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    while ((double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)f < ms) {}
}

static void job_work(void *u) { (void)u; spin_ms(0.2); }
static void range_work(void *ctx, int b, int e) { (void)ctx; (void)b; (void)e; spin_ms(0.05); }

static void section(FILE *f) { fprintf(f, "{\"hello\":1}"); }

// parallel_for back to back (late helpers of one loop meet the next loops on their slot): every index
// runs exactly once and all of them before the call returns (docs/perf/FINDINGS.md PF-0021).
static SDL_AtomicInt g_pf_hits[512];
static void count_range(void *ctx, int b, int e) {
    (void)ctx;
    for (int i = b; i < e; i++) SDL_AddAtomicInt(&g_pf_hits[i], 1);
    if ((b & 7) == 0) SDL_Delay(0);   // yield: helpers fall behind
}
static int pf_stress(int loops) {
    int bad = 0;
    for (int l = 0; l < loops; l++) {
        int n = 17 + (l * 37) % 480;
        for (int i = 0; i < n; i++) SDL_SetAtomicInt(&g_pf_hits[i], 0);
        jobs_parallel_for(n, 1 + l % 5, count_range, NULL);
        for (int i = 0; i < n; i++) bad += SDL_GetAtomicInt(&g_pf_hits[i]) != 1;
    }
    return bad;
}

int main(void) {
    log_init("test_profiler.log");
    prof_init();
    cvar_system_init();
    prof_register();
    jobs_init(3);
    prof_add_section("custom", section);
    prof_frame_mark();   // ends the startup recording

    ProfCaptureDesc d;
    memset(&d, 0, sizeof(d));
    snprintf(d.path, sizeof(d.path), "test_profiler.json");
    snprintf(d.scene, sizeof(d.scene), "unit");
    snprintf(d.label, sizeof(d.label), "test");
    snprintf(d.fidelity, sizeof(d.fidelity), "none");
    d.frames = 12;
    d.warmup = 2;
    CHECK(prof_capture_start(&d), "capture start");
    CHECK(!prof_capture_start(&d), "a second capture is refused");
    int frames = 0;
    while (prof_capture_active() || frames < 3) {
        prof_frame_mark();
        if (!prof_capture_active()) break;
        PROF_BEGIN("frame_work");
        PROF_BEGIN("inner");
        spin_ms(0.5);
        void *p = malloc(100);   // a hot-path allocation (site counted)
        free(p);
        char buf[64];
        memcpy(buf, "0123456789", 10);
        (void)buf;
        PROF_END();
        JobCounter c = { 0 };
        for (int i = 0; i < 4; i++) jobs_submit(job_work, NULL, NULL, &c);
        jobs_wait(&c);
        jobs_parallel_for(64, 4, range_work, NULL);
        prof_count(PC_DRAW_ITEMS, 7);
        prof_series("custom_ms", 1.5);
        PROF_END();
        frames++;
        if (frames > 100) break;
    }
    CHECK(prof_capture_done() || !prof_capture_active(), "capture finished");

    // The file: parsed by python in the ctest (tools/perf_audit/tests); here a few substrings.
    FILE *f = fopen("test_profiler.json", "rb");
    CHECK(f != NULL, "profile written");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *s = (char *)calloc((size_t)n + 1, 1);
        fread(s, 1, (size_t)n, f);
        fclose(f);
        CHECK(strstr(s, "\"format\":\"dg-perf-profile\"") != NULL, "format tag");
        CHECK(strstr(s, "\"name\":\"frame_work\"") != NULL, "zone frame_work");
        CHECK(strstr(s, "\"name\":\"inner\"") != NULL, "zone inner");
        CHECK(strstr(s, "\"name\":\"job:") != NULL, "job zones");
        CHECK(strstr(s, "parfor:") != NULL, "parallel_for zones");
        CHECK(strstr(s, "test_profiler.c:") != NULL, "allocation site");
        CHECK(strstr(s, "\"custom\":{\"hello\":1}") != NULL, "section");
        CHECK(strstr(s, "\"custom_ms\":[1.5") != NULL, "series");
        CHECK(strstr(s, "\"draw_items\":[7,7") != NULL, "counter per frame");
        free(s);
    }

    // perf_fix_disable -> prof_fix_enabled
    int pf_bad = pf_stress(4000);
    CHECK(pf_bad == 0, "parallel_for: %d indices not run exactly once by the time it returned", pf_bad);
    CHECK(prof_fix_enabled(PFX_CVAR_HASH), "fixes on by default");
    cvar_set("perf_fix_disable", "PF-0002,PF-0005");
    CHECK(!prof_fix_enabled(PFX_CVAR_HASH) && !prof_fix_enabled(PFX_FG_REPORT_LAZY) && prof_fix_enabled(PFX_SHADOW_CLEAR_SKIP), "fix ids parsed");
    // "all" = every fidelity-neutral fix; the visual trade-offs only by their own id
    cvar_set("perf_fix_disable", "all");
    CHECK(!prof_fix_enabled(PFX_SHADOW_LIGHT_ORDER) && prof_fix_enabled(PFX_SHADOW_CASTER_LIMIT), "all: neutral fixes off, trade-offs on");
    cvar_set("perf_fix_disable", "PF-0013");
    CHECK(!prof_fix_enabled(PFX_SHADOW_CASTER_LIMIT) && prof_fix_enabled(PFX_SHADOW_LIGHT_ORDER) && !strcmp(prof_fix_id(PFX_SHADOW_CASTER_LIMIT), "PF-0013"), "a trade-off by its id");
    cvar_set("perf_fix_disable", "PF-0002,PF-0005");
    // cvar_find: the hash index and the scan agree (PF-0002)
    Cvar *a = cvar_find("PERF_FIX_DISABLE");
    cvar_set("perf_fix_disable", "");
    Cvar *b = cvar_find("perf_fix_disable");
    CHECK(a && a == b, "cvar_find hash == scan, case-insensitive");
    CHECK(cvar_find("no_such_cvar_xyz") == NULL, "missing cvar");
    // PF-0002 cost per lookup with as many cvars as the game registers (~700): scan vs index.
    for (int i = 0; i < 700; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "bench_cvar_%03d", i);
        cvar_reg_int(nm, i, 0, 1000, 0, "microbenchmark cvar");
    }
    double us[2];
    for (int pass = 0; pass < 2; pass++) {
        cvar_set("perf_fix_disable", pass == 0 ? "PF-0002" : "");
        u64 t0 = SDL_GetPerformanceCounter();
        int hits = 0;
        for (int i = 0; i < 20000; i++) {
            char nm[32];
            snprintf(nm, sizeof(nm), "BENCH_CVAR_%03d", (i * 37) % 700);
            hits += cvar_find(nm) != NULL;
        }
        us[pass] = (double)(SDL_GetPerformanceCounter() - t0) * 1e6 / (double)SDL_GetPerformanceFrequency() / 20000.0;
        CHECK(hits == 20000, "every bench cvar found (pass %d)", pass);
    }
    printf("cvar_find with %d cvars: scan %.3f us, hash index %.3f us per lookup\n", cvar_count(), us[0], us[1]);
    CHECK(us[1] < us[0], "the index is faster than the scan");

    jobs_shutdown();
    prof_shutdown();
    printf("test_profiler: %d frame(s), %s\n", frames, fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
