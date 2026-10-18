// Unit tests for the procedural synth (src/audio/synth, src/audio/mixer.c, docs/AUDIO_PROCEDURAL.md
// "Tests"): the patch compiler (execution order, cycles), node responses (RBJ biquads against
// reference coefficients, SVF, envelope timing and gate, modal decay, noise colours), expressions,
// recipes (deterministic per seed; the gunshot monotonic in energy and shifted per muzzle device),
// every shipped patch / recipe loads, the mixer (start delay = d / c within a block, no allocation
// in mixer_render, stealing, no NaN / denormals), smoothing without zipper noise, the death ring
// (migrated death_synth), and the 64-voice budget.
#include "audio/synth/syn_graph.h"
#include "audio/synth/syn_patch.h"
#include "audio/synth/syn_recipe.h"
#include "audio/synth/syn_material.h"
#include "audio/mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#endif

static int g_fail, g_checks;
extern int g_audio_render_allocs;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

#define SR 48000.0f

static double now_s(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

// JSON files of a directory (sorted: patches before the recipes that use them is the caller's job).
static int list_json(const char *dir, char names[][64], int max) {
    int n = 0;
#if defined(_WIN32)
    char pat[512];
    snprintf(pat, sizeof(pat), "%s/*.json", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return 0;
    do { if (n < max) snprintf(names[n++], 64, "%s", fd.name); } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l > 5 && !strcmp(e->d_name + l - 5, ".json")) snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(d);
#endif
    return n;
}

// ------------------------------------------------------------------ fake sample bank

#define NSAMPLES 10
static const char *k_sample_names[NSAMPLES] = { "shot_rifle", "bullet_crack_1", "bullet_whizz", "engine_loop",
                                                "tk_1", "tk_2", "tk_3", "solo", "mat_a_1", "mat_b" };
static float *g_sample_data[NSAMPLES];
static int g_sample_frames[NSAMPLES];
static SynSampleBank g_bank = { (const float *const *)g_sample_data, g_sample_frames, NSAMPLES };

static int resolve(const char *name, void *user) {
    (void)user;
    for (int i = 0; i < NSAMPLES; i++) if (!strcmp(k_sample_names[i], name)) return i;
    return -1;
}

static void make_samples(void) {
    for (int i = 0; i < NSAMPLES; i++) {
        int n = (int)(0.3f * SR);
        g_sample_data[i] = malloc(sizeof(float) * (size_t)n);
        SynRng r = syn_rng((u32)i + 1);
        for (int k = 0; k < n; k++) g_sample_data[i][k] = 0.5f * syn_rngbi(&r) * expf(-(float)k / (0.05f * SR));
        g_sample_frames[i] = n;
    }
    syn_set_sample_bank(&g_bank);
}

static SynLibrary g_lib;
static int g_patch_files, g_recipe_files, g_load_errors;

static void load_library(void) {
    syn_lib_init(&g_lib, SR, resolve, NULL);
    char names[256][64], path[512], err[256];
    const char *dirs[2] = { DG_SOURCE_DIR "/assets/audio/patches", DG_SOURCE_DIR "/assets/audio/recipes" };
    for (int d = 0; d < 2; d++) {
        int n = list_json(dirs[d], names, 256);
        for (int i = 0; i < n; i++) {
            snprintf(path, sizeof(path), "%s/%s", dirs[d], names[i]);
            char *txt = read_file(path);
            if (!txt) continue;
            int r = d == 0 ? syn_lib_add_patch(&g_lib, txt, err, sizeof(err)) : syn_lib_add_recipe(&g_lib, txt, err, sizeof(err));
            if (r < 0) { printf("  load %s: %s\n", names[i], err); g_load_errors++; }
            if (d == 0) g_patch_files++; else g_recipe_files++;
            free(txt);
        }
    }
}

// ------------------------------------------------------------------ helpers

static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * 3.14159265358979 / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                float wr = (float)cos(ang * k), wi = (float)sin(ang * k);
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                float vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
            }
    }
}

// Power spectrum of x[0..n) (n power of two), Hann window. out: n/2 bins.
static void power_spectrum(const float *x, int n, float *out) {
    float *re = calloc((size_t)n, sizeof(float)), *im = calloc((size_t)n, sizeof(float));
    for (int i = 0; i < n; i++) re[i] = x[i] * (0.5f - 0.5f * cosf(6.2831853f * (float)i / (float)(n - 1)));
    fft(re, im, n);
    for (int i = 0; i < n / 2; i++) out[i] = re[i] * re[i] + im[i] * im[i];
    free(re);
    free(im);
}

static float band_energy(const float *ps, int n, float lo, float hi) {
    float e = 0;
    for (int i = 0; i < n / 2; i++) {
        float hz = (float)i * SR / (float)n;
        if (hz >= lo && hz < hi) e += ps[i];
    }
    return e;
}

static float centroid(const float *x, int n) {
    int N = 1;
    while (N < n && N < 65536) N <<= 1;
    float *buf = calloc((size_t)N, sizeof(float)), *ps = malloc(sizeof(float) * (size_t)N / 2);
    memcpy(buf, x, sizeof(float) * (size_t)(n < N ? n : N));
    power_spectrum(buf, N, ps);
    double num = 0, den = 0;
    for (int i = 1; i < N / 2; i++) { double hz = i * SR / N; num += hz * ps[i]; den += ps[i]; }
    free(buf);
    free(ps);
    return den > 0 ? (float)(num / den) : 0.0f;
}

static float rms(const float *x, int n) {
    double e = 0;
    for (int i = 0; i < n; i++) e += (double)x[i] * x[i];
    return (float)sqrt(e / (n > 0 ? n : 1));
}

static bool parse_compile(const char *json, SynPatch *p, char *err, int en) {
    if (!syn_patch_parse(json, p, resolve, NULL, err, en)) return false;
    return syn_patch_compile(p, SR, err, en);
}

// Renders a compiled patch (one voice) for n frames into out.
static double g_arena_d[SYN_VOICE_ARENA / sizeof(double)];   // 8-byte aligned
#define g_arena ((void *)g_arena_d)
static float g_scratch[SYN_MAX_NODES * SYN_BLOCK];
static int render_patch(const SynPatch *p, float *out, int n, u32 seed) {
    SynVoice v;
    syn_voice_init(&v, p, g_arena, NULL, seed);
    int done_at = -1;
    for (int off = 0; off < n; off += SYN_BLOCK) {
        int k = n - off < SYN_BLOCK ? n - off : SYN_BLOCK;
        if (!syn_voice_render(&v, out + off, k, g_scratch) && done_at < 0) done_at = off;
    }
    return done_at;
}

// Renders a recipe through a mixer (2D, no environment) and returns the left channel.
static Mixer *g_mix;
static int render_recipe(const char *name, const char *const *keys, const float *vals, int nk, u32 seed, float *out, int frames, const char *sample) {
    int ri = syn_lib_find_recipe(&g_lib, name);
    if (ri < 0) return -1;
    const SynRecipe *r = g_lib.recipe[ri];
    float in[RCP_MAX_INPUTS];
    for (int i = 0; i < r->ninputs; i++) in[i] = r->input_def[i];
    for (int k = 0; k < nk; k++) {
        int idx = syn_recipe_input(r, keys[k]);
        if (idx >= 0) in[idx] = vals[k];
    }
    SynRecipeOut o;
    syn_recipe_eval(&g_lib, r, in, seed, sample, &o);
    MixStart s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < o.nlayers; i++) {
        if (!o.layer[i].on || !o.layer[i].patch) continue;
        MixLayerStart *l = &s.layer[s.nlayers++];
        l->patch = o.layer[i].patch;
        l->gain = o.layer[i].gain;
        l->optional = o.layer[i].optional;
        memcpy(l->params, o.layer[i].params, sizeof(l->params));
    }
    s.seed = seed;
    s.volume = 1.0f;
    s.score = 1.0f;
    env_params_neutral(&s.env_params);
    MixHandle h = mixer_start(g_mix, &s);
    if (!h) return -2;
    static float st[2 * 65536];
    int done = 0;
    while (done < frames) {
        int k = frames - done < 480 ? frames - done : 480;
        mixer_render(g_mix, st, k);
        for (int i = 0; i < k; i++) out[done + i] = st[i * 2];
        done += k;
    }
    mixer_stop(g_mix, h, 0.005f);
    for (int i = 0; i < 4; i++) mixer_render(g_mix, st, 480);
    mixer_update(g_mix);
    return 0;
}

// ------------------------------------------------------------------ compiler

static void test_compiler(void) {
    char err[256];
    static SynPatch p;
    // Listed out of order: the output first, its inputs after. The order must put dependencies first.
    const char *j = "{\"name\":\"t\",\"nodes\":["
                    "{\"id\":\"out\",\"type\":\"mix\",\"in\":[\"f\",\"e\"]},"
                    "{\"id\":\"f\",\"type\":\"biquad\",\"mode\":\"lp\",\"in\":\"n\",\"hz\":{\"mod\":\"e\",\"scale\":2000,\"offset\":100}},"
                    "{\"id\":\"e\",\"type\":\"env\",\"stages\":[[5,1],[50,0]]},"
                    "{\"id\":\"n\",\"type\":\"noise\"}],\"out\":\"out\"}";
    bool ok = parse_compile(j, &p, err, sizeof(err));
    CHECK(ok, "out-of-order patch compiles (%s)", err);
    int pos[4];
    for (int i = 0; i < p.nnodes; i++) pos[p.order[i]] = i;
    CHECK(pos[3] < pos[1] && pos[2] < pos[1], "noise and the modulating env before the filter");
    CHECK(pos[1] < pos[0] && pos[2] < pos[0], "the mix last");
    CHECK(p.out == 0, "out node");
    // Cycles: a -> b -> a, and a node feeding itself.
    const char *cyc = "{\"name\":\"c\",\"nodes\":[{\"id\":\"a\",\"type\":\"gain\",\"in\":\"b\"},{\"id\":\"b\",\"type\":\"gain\",\"in\":\"a\"}]}";
    ok = parse_compile(cyc, &p, err, sizeof(err));
    CHECK(!ok && strstr(err, "cycle"), "cycle rejected (%s)", err);
    const char *self = "{\"name\":\"s\",\"nodes\":[{\"id\":\"a\",\"type\":\"gain\",\"in\":\"a\"}]}";
    ok = parse_compile(self, &p, err, sizeof(err));
    CHECK(!ok, "self feed rejected (%s)", err);
    const char *modcyc = "{\"name\":\"m\",\"nodes\":[{\"id\":\"n\",\"type\":\"noise\"},{\"id\":\"a\",\"type\":\"svf\",\"in\":\"n\",\"hz\":{\"mod\":\"b\"}},{\"id\":\"b\",\"type\":\"gain\",\"in\":\"a\"}]}";
    ok = parse_compile(modcyc, &p, err, sizeof(err));
    CHECK(!ok && strstr(err, "cycle"), "modulation cycle rejected (%s)", err);
    ok = parse_compile("{\"name\":\"u\",\"nodes\":[{\"type\":\"warp\"}]}", &p, err, sizeof(err));
    CHECK(!ok, "unknown node type rejected");
    ok = parse_compile("{\"name\":\"u\",\"nodes\":[{\"type\":\"osc\",\"cutoff\":3}]}", &p, err, sizeof(err));
    CHECK(!ok, "unknown parameter rejected");
    ok = parse_compile("{\"name\":\"u\",\"nodes\":[{\"type\":\"biquad\"}]}", &p, err, sizeof(err));
    CHECK(!ok, "missing input rejected");
    ok = parse_compile("{\"name\":\"u\",\"nodes\":[{\"type\":\"delay\",\"in\":\"x\"}]}", &p, err, sizeof(err));
    CHECK(!ok, "dangling input rejected");
    ok = parse_compile("{\"name\":\"big\",\"nodes\":[{\"id\":\"n\",\"type\":\"noise\"},{\"type\":\"delay\",\"in\":\"n\",\"max_ms\":2000}]}", &p, err, sizeof(err));
    CHECK(!ok && strstr(err, "arena"), "oversized state rejected (%s)", err);
    // Round trip through the writer.
    ok = parse_compile(j, &p, err, sizeof(err));
    char *w = syn_patch_write(&p, k_sample_names, NSAMPLES);
    static SynPatch q;
    CHECK(w && parse_compile(w, &q, err, sizeof(err)) && q.nnodes == p.nnodes && memcmp(q.order, p.order, sizeof(p.order)) == 0, "write -> parse round trip (%s)", err);
    free(w);
}

// ------------------------------------------------------------------ nodes

// Reference RBJ coefficients, double precision, written out independently of syn_biquad_design.
static void rbj_ref(int mode, double f, double q, double gdb, double sr, double c[5]) {
    double w = 2 * 3.14159265358979323846 * f / sr, cs = cos(w), sn = sin(w), al = sn / (2 * q), A = pow(10, gdb / 40);
    double b0, b1, b2, a0, a1, a2;
    if (mode == SYN_LP) { b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }
    else if (mode == SYN_HP) { b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }
    else if (mode == SYN_BP) { b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }
    else if (mode == SYN_NOTCH) { b0 = 1; b1 = -2 * cs; b2 = 1; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }
    else if (mode == SYN_PEAK) { b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; }
    else if (mode == SYN_LSHELF) {
        double k = 2 * sqrt(A) * al;
        b0 = A * ((A + 1) - (A - 1) * cs + k); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - k);
        a0 = (A + 1) + (A - 1) * cs + k; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - k;
    } else {
        double k = 2 * sqrt(A) * al;
        b0 = A * ((A + 1) + (A - 1) * cs + k); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - k);
        a0 = (A + 1) - (A - 1) * cs + k; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - k;
    }
    c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
}

static void test_filters(void) {
    const int modes[7] = { SYN_LP, SYN_HP, SYN_BP, SYN_NOTCH, SYN_PEAK, SYN_LSHELF, SYN_HSHELF };
    const float freqs[3] = { 100.0f, 1000.0f, 9000.0f };
    double worst = 0;
    for (int m = 0; m < 7; m++)
        for (int k = 0; k < 3; k++) {
            SynBiquad f;
            syn_biquad_design(&f, (SynFilterMode)modes[m], freqs[k], 0.9f, 6.0f, SR);
            double c[5];
            rbj_ref(modes[m], freqs[k], 0.9, 6.0, SR, c);
            const float got[5] = { f.b0, f.b1, f.b2, f.a1, f.a2 };
            for (int i = 0; i < 5; i++) worst = fmax(worst, fabs(got[i] - c[i]));
        }
    CHECK(worst < 1e-6, "biquad coefficients match the RBJ reference (max error %.2e)", worst);
    // Butterworth low-pass: -3.01 dB at the cutoff, ~-40 dB a decade above (12 dB / octave).
    SynBiquad lp;
    syn_biquad_design(&lp, SYN_LP, 1000.0f, 0.70710678f, 0, SR);
    float at = 20.0f * log10f(syn_biquad_mag(&lp, 1000.0f, SR)), dec = 20.0f * log10f(syn_biquad_mag(&lp, 10000.0f, SR));
    CHECK(fabsf(at + 3.01f) < 0.05f, "LP -3 dB at fc (%.2f dB)", at);
    CHECK(dec < -38.0f && dec > -46.0f, "LP 2nd order slope (%.1f dB a decade up; bilinear warping steepens it near Nyquist)", dec);
    // The filter applied to a sine at fc: amplitude 0.707 after settling.
    static float x[48000], y[48000];
    for (int i = 0; i < 48000; i++) x[i] = sinf(6.2831853f * 1000.0f * (float)i / SR);
    syn_biquad_run(&lp, x, y, 48000);
    float pk = 0;
    for (int i = 24000; i < 48000; i++) pk = fmaxf(pk, fabsf(y[i]));
    CHECK(fabsf(pk - 0.7071f) < 0.01f, "LP sine at fc peak %.4f", pk);
    // SVF: the same cutoff behaviour.
    SynSvf sv;
    memset(&sv, 0, sizeof(sv));
    syn_svf_design(&sv, 1000.0f, 0.70710678f, SR);
    syn_svf_run(&sv, SYN_LP, x, y, 48000);
    pk = 0;
    for (int i = 24000; i < 48000; i++) pk = fmaxf(pk, fabsf(y[i]));
    CHECK(fabsf(pk - 0.7071f) < 0.015f, "SVF LP sine at fc peak %.4f", pk);
    // One-pole: -3 dB at its cutoff (approximately, the matched-z design).
    float k = syn_onepole_coef(500.0f, SR), z = 0;
    for (int i = 0; i < 48000; i++) { float s = sinf(6.2831853f * 500.0f * (float)i / SR); z += k * (s - z); y[i] = z; }
    pk = 0;
    for (int i = 24000; i < 48000; i++) pk = fmaxf(pk, fabsf(y[i]));
    CHECK(fabsf(20.0f * log10f(pk) + 3.0f) < 0.4f, "one-pole -3 dB at fc (%.2f dB)", 20.0f * log10f(pk));
}

static void test_envelope(void) {
    char err[256];
    static SynPatch p;
    static float out[24000];
    bool ok = parse_compile("{\"name\":\"e\",\"nodes\":[{\"type\":\"env\",\"stages\":[[10,1],[100,0]]}]}", &p, err, sizeof(err));
    CHECK(ok, "env patch (%s)", err);
    int done = render_patch(&p, out, 24000, 1);
    int peak = 0;
    for (int i = 0; i < 24000; i++) if (out[i] > out[peak]) peak = i;
    int end = 0;
    for (int i = 0; i < 24000; i++) if (out[i] > 1e-6f) end = i;
    CHECK(abs(peak - 480) <= 2, "attack peak at 10 ms (sample %d)", peak);
    CHECK(fabsf(out[peak] - 1.0f) < 1e-3f, "attack reaches 1 (%.4f)", out[peak]);
    CHECK(abs(end - 5280) <= 2, "release ends at 110 ms (sample %d)", end);
    CHECK(done >= 5280 && done <= 5280 + 9 * SYN_BLOCK, "voice ends once silent (block at %d)", done);
    // Gate: adsr holds the sustain while gate = 1 and releases when it drops.
    ok = parse_compile("{\"name\":\"g\",\"params\":{\"gate\":1},\"nodes\":[{\"type\":\"env\",\"adsr\":[5,20,0.5,50],\"gate\":\"$gate\"}]}", &p, err, sizeof(err));
    CHECK(ok, "adsr patch (%s)", err);
    SynVoice v;
    syn_voice_init(&v, &p, g_arena, NULL, 1);
    int n = 0;
    for (; n < 14400; n += SYN_BLOCK) syn_voice_render(&v, out + n, SYN_BLOCK, g_scratch);   // 300 ms held
    CHECK(fabsf(out[14000] - 0.5f) < 1e-3f, "sustain level held (%.4f)", out[14000]);
    syn_voice_snap_param(&v, 0, 0.0f);
    int rel_end = -1;
    for (int i = 0; i < 9600 / SYN_BLOCK; i++) {
        bool alive = syn_voice_render(&v, out + n, SYN_BLOCK, g_scratch);
        for (int s = 0; s < SYN_BLOCK && rel_end < 0; s++) if (out[n + s] < 1e-6f) rel_end = n + s - 14400;
        n += SYN_BLOCK;
        if (!alive) break;
    }
    CHECK(rel_end >= 2300 && rel_end <= 2500, "release ~50 ms after the gate (%d samples)", rel_end);
}

static void test_modal_noise(void) {
    char err[256];
    static SynPatch p;
    static float out[48000];
    bool ok = parse_compile("{\"name\":\"m\",\"nodes\":[{\"id\":\"i\",\"type\":\"impulse\"},{\"type\":\"modal\",\"in\":\"i\",\"modes\":[[1000,0.5,1]]}]}", &p, err, sizeof(err));
    CHECK(ok, "modal patch (%s)", err);
    render_patch(&p, out, 48000, 1);
    float a0 = 0, a1 = 0;
    for (int i = 0; i < 480; i++) a0 = fmaxf(a0, fabsf(out[i]));
    for (int i = 24000; i < 24480; i++) a1 = fmaxf(a1, fabsf(out[i]));
    float db = 20.0f * log10f(a1 / a0);
    CHECK(fabsf(db + 60.0f) < 2.0f, "modal T60: -60 dB after 0.5 s (%.1f dB)", db);
    CHECK(fabsf(a0 - 1.0f) < 0.05f, "modal peak ~ mode gain (%.3f)", a0);
    int zc = 0;
    for (int i = 1; i < 24000; i++) if ((out[i - 1] < 0) != (out[i] < 0)) zc++;
    CHECK(fabsf(zc / 2.0f / 0.5f - 1000.0f) < 5.0f, "modal frequency %.1f Hz", zc / 2.0f / 0.5f);
    // Material presets: metal rings longer than wood.
    ok = parse_compile("{\"name\":\"mm\",\"params\":{\"mat\":4},\"nodes\":[{\"id\":\"i\",\"type\":\"impulse\"},{\"type\":\"modal\",\"in\":\"i\",\"material\":\"$mat\"}]}", &p, err, sizeof(err));
    CHECK(ok, "material modal (%s)", err);
    float e_metal = 0, e_wood = 0;
    render_patch(&p, out, 48000, 1);
    for (int i = 9600; i < 48000; i++) e_metal += out[i] * out[i];
    p.param_def[0] = SYN_MAT_WOOD;
    render_patch(&p, out, 48000, 1);
    for (int i = 9600; i < 48000; i++) e_wood += out[i] * out[i];
    CHECK(e_metal > 100.0f * e_wood, "thin metal rings longer than wood (%.3g vs %.3g)", e_metal, e_wood);
    // Noise colours: pink -3 dB / octave, brown -6 dB / octave (1 kHz octave vs the 4 kHz octave).
    const int N = 65536;
    float *buf = malloc(sizeof(float) * N), *ps = malloc(sizeof(float) * N / 2);
    const SynNoiseColor col[3] = { SYN_WHITE, SYN_PINK, SYN_BROWN };
    const float want[3] = { 6.0f, 0.0f, -6.0f };   // dB difference 4k octave - 1k octave (2 octaves up)
    for (int c = 0; c < 3; c++) {
        SynNoise nz;
        memset(&nz, 0, sizeof(nz));
        nz.rng = syn_rng(7);
        syn_noise_run(&nz, col[c], buf, N, 1.0f, 0, SR);
        power_spectrum(buf, N, ps);
        float lo = band_energy(ps, N, 707, 1414), hi = band_energy(ps, N, 2828, 5657);
        float d = 10.0f * log10f(hi / lo);
        CHECK(fabsf(d - want[c]) < 1.5f, "noise colour %d: %+.1f dB over two octaves (want %+.0f)", c, d, want[c]);
    }
    free(buf);
    free(ps);
}

static void test_expressions(void) {
    const char *names[3] = { "x", "device", "energy" };
    float vars[3] = { 0.4f, 2.0f, 1500.0f };
    struct { const char *src; float want; } t[] = {
        { "1 + 2 * 3", 7 }, { "(1 + 2) * 3", 9 }, { "2 ^ 3 * 2", 16 }, { "-x * 10", -4 }, { "lerp(x, 0.25, 22, 0.6, 14)", 22 - 8 * (0.15f / 0.35f) },
        { "lerp(5, 0, 1, 1, 2)", 2 }, { "sel(device, 10, 20, 30, 40)", 30 }, { "sel(9, 1, 2)", 2 }, { "device == 2 ? 5 : 6", 5 },
        { "device != 2 ? 5 : 6", 6 }, { "x < 0.5 && energy > 1000", 1 }, { "!(x < 0.5) || 0", 0 }, { "sqrt(energy / 15) ", 10 },
        { "db(-6)", 0.501187f }, { "clamp(energy, 0, 1000)", 1000 }, { "min(1, 2) + max(1, 2)", 3 }, { "7 % 4", 3 },
    };
    char err[128];
    for (int i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
        SynExpr e;
        bool ok = syn_expr_compile(&e, t[i].src, names, 3, err, sizeof(err));
        float v = ok ? syn_expr_eval(&e, vars) : NAN;
        CHECK(ok && fabsf(v - t[i].want) < 1e-3f * fmaxf(1.0f, fabsf(t[i].want)), "expr '%s' = %g (want %g) %s", t[i].src, v, t[i].want, ok ? "" : err);
    }
    SynExpr e;
    CHECK(!syn_expr_compile(&e, "nope + 1", names, 3, err, sizeof(err)), "unknown name rejected");
    CHECK(!syn_expr_compile(&e, "lerp(1, 2)", names, 3, err, sizeof(err)), "wrong arity rejected");
    CHECK(!syn_expr_compile(&e, "1 +", names, 3, err, sizeof(err)), "syntax error rejected");
}

// ------------------------------------------------------------------ recipes

static void test_library(void) {
    CHECK(g_patch_files >= 4 && g_recipe_files >= 3, "found %d patches, %d recipes", g_patch_files, g_recipe_files);
    CHECK(g_load_errors == 0, "every shipped patch and recipe loads (%d errors)", g_load_errors);
    // Every shipped patch renders finite audio with its defaults.
    static float out[24000];
    int bad = 0;
    for (int i = 0; i < g_lib.npatches; i++) {
        render_patch(g_lib.patch[i], out, 24000, 3);
        for (int k = 0; k < 24000; k++) if (!isfinite(out[k]) || fabsf(out[k]) > 50.0f) { bad++; printf("  patch %s: bad sample %g at %d\n", g_lib.patch[i]->name, out[k], k); break; }
    }
    CHECK(bad == 0, "every patch renders finite, bounded audio (%d bad)", bad);
}

#define SHOT_FRAMES 24000
static void shot(float energy, float device, u32 seed, float *out) {
    // From 20 m, not the shooter: the blast alone (the mechanism layer is for the shooter / close by).
    const char *k[4] = { "energy", "device", "local", "distance" };
    float v[4] = { energy, device, 0, 20 };
    int r = render_recipe("gunshot", k, v, 4, seed, out, SHOT_FRAMES, NULL);
    if (r < 0) memset(out, 0, sizeof(float) * SHOT_FRAMES);
}

static void test_recipes(void) {
    static float a[SHOT_FRAMES], b[SHOT_FRAMES];
    shot(2000, 0, 1, a);
    shot(2000, 0, 1, b);
    CHECK(rms(a, SHOT_FRAMES) > 1e-3f, "gunshot renders (rms %.4f)", rms(a, SHOT_FRAMES));
    CHECK(memcmp(a, b, sizeof(a)) == 0, "same seed renders bit-identical");
    shot(2000, 0, 2, b);
    float diff = 0;
    for (int i = 0; i < SHOT_FRAMES; i++) diff = fmaxf(diff, fabsf(a[i] - b[i]));
    CHECK(diff > 1e-3f, "another seed varies (max diff %.4f)", diff);
    float rb = rms(a, SHOT_FRAMES), rb2 = rms(b, SHOT_FRAMES);
    CHECK(fabsf(20.0f * log10f(rb2 / rb)) < 3.0f, "variation stays within a few dB (%.2f dB)", 20.0f * log10f(rb2 / rb));
    // Monotonic in energy: level and low end grow.
    const float energies[5] = { 300, 1000, 2000, 4000, 8000 };
    float last_rms = 0, last_low = 0;
    bool mono_rms = true, mono_low = true;
    const int N = 16384;
    float *ps = malloc(sizeof(float) * N / 2);
    for (int i = 0; i < 5; i++) {
        shot(energies[i], 0, 5, a);
        float r = rms(a, SHOT_FRAMES);
        power_spectrum(a, N, ps);
        float low = band_energy(ps, N, 20, 300);
        if (i && r <= last_rms) mono_rms = false;
        if (i && low <= last_low) mono_low = false;
        last_rms = r;
        last_low = low;
    }
    CHECK(mono_rms, "gunshot level grows with energy");
    CHECK(mono_low, "gunshot low end grows with energy");
    // Muzzle devices: the suppressor is far quieter and darker; a brake brighter, a flash hider darker.
    // Its level is the device's report_db (-28 dB) on the voice as the game plays it: measured
    // suppressors take 20-35 dB off the peak.
    shot(2000, 0, 9, a);
    float bare_c = centroid(a, SHOT_FRAMES), bare_pk = 0, sup_pk = 0;
    for (int i = 0; i < SHOT_FRAMES; i++) bare_pk = fmaxf(bare_pk, fabsf(a[i]));
    shot(2000, 3, 9, b);
    float sup_c = centroid(b, SHOT_FRAMES);
    for (int i = 0; i < SHOT_FRAMES; i++) sup_pk = fmaxf(sup_pk, fabsf(b[i]) * syn_db(-28.0f));
    float sup_db = 20.0f * log10f(sup_pk / bare_pk);
    CHECK(sup_db < -20.0f && sup_db > -40.0f, "suppressed peak %.1f dB vs bare (measured: -20 .. -35)", sup_db);
    CHECK(sup_c < 0.7f * bare_c, "suppressor darker (centroid %.0f vs %.0f Hz)", sup_c, bare_c);
    shot(2000, 2, 9, b);
    float comp_c = centroid(b, SHOT_FRAMES);
    CHECK(comp_c > bare_c, "brake / compensator brighter (%.0f vs %.0f Hz)", comp_c, bare_c);
    shot(2000, 1, 9, b);
    float hid_c = centroid(b, SHOT_FRAMES);
    CHECK(hid_c < bare_c, "flash hider a little darker (%.0f vs %.0f Hz)", hid_c, bare_c);
    free(ps);
    // A sample layer ("@" = the weapon's own sound_fire) comes in when the event names one.
    shot(2000, 0, 1, a);
    const char *k[2] = { "energy", "local" };
    float v[2] = { 2000, 1 };
    render_recipe("gunshot", k, v, 2, 1, b, SHOT_FRAMES, "shot_rifle");
    diff = 0;
    for (int i = 0; i < SHOT_FRAMES; i++) diff = fmaxf(diff, fabsf(a[i] - b[i]));
    CHECK(diff > 1e-3f, "the hybrid sample layer plays (max diff %.4f)", diff);
    // Bad recipes are refused with a reason.
    char err[256];
    int r = syn_lib_add_recipe(&g_lib, "{\"name\":\"bad\",\"layers\":[{\"patch\":\"gun_blast\",\"bind\":{\"nope\":\"1\"}}]}", err, sizeof(err));
    CHECK(r < 0 && strstr(err, "nope"), "binding an unknown patch parameter fails (%s)", err);
    r = syn_lib_add_recipe(&g_lib, "{\"name\":\"bad2\",\"inputs\":{\"e\":1},\"layers\":[{\"patch\":\"gun_blast\",\"bind\":{\"amp\":\"e * zz\"}}]}", err, sizeof(err));
    CHECK(r < 0 && strstr(err, "zz"), "an expression with an unknown input fails (%s)", err);
}

// Recorded takes (<name>_1 .. _N, one per seed) and sample lists picked by an expression.
static const char *layer_patch(const SynRecipe *r, const char *in, float v, u32 seed) {
    float inputs[RCP_MAX_INPUTS];
    for (int i = 0; i < r->ninputs; i++) inputs[i] = r->input_def[i];
    inputs[syn_recipe_input(r, in)] = v;
    SynRecipeOut o;
    syn_recipe_eval(&g_lib, r, inputs, seed, NULL, &o);
    return o.layer[0].patch ? o.layer[0].patch->name : "";
}

static void test_takes(void) {
    CHECK(syn_lib_sample_variants(&g_lib, "tk") == 3 && syn_lib_sample_variants(&g_lib, "solo") == 0, "takes counted (tk: %d, solo: %d)",
          syn_lib_sample_variants(&g_lib, "tk"), syn_lib_sample_variants(&g_lib, "solo"));
    int seen[3] = { 0, 0, 0 };
    bool stable = true;
    for (u32 s = 0; s < 64; s++) {
        int a = syn_lib_sample_pick(&g_lib, "tk", s, false), b = syn_lib_sample_pick(&g_lib, "tk", s, false);
        stable = stable && a >= 0 && a == b;
        if (a < 0) continue;
        const char *n = g_lib.patch[a]->name;
        int k = n[strlen(n) - 1] - '1';
        if (!strncmp(n, "sample:tk_", 10) && k >= 0 && k < 3) seen[k]++;
    }
    CHECK(stable && seen[0] && seen[1] && seen[2], "a seed keeps its take, every take plays (%d / %d / %d of 64)", seen[0], seen[1], seen[2]);
    int solo = syn_lib_sample_pick(&g_lib, "solo", 5, false);
    CHECK(solo >= 0 && !strcmp(g_lib.patch[solo]->name, "sample:solo") && syn_lib_sample_pick(&g_lib, "nothing", 5, false) < 0,
          "a name without takes plays itself; a missing one nothing");
    char err[256];
    int ri = syn_lib_add_recipe(&g_lib, "{\"name\":\"takes_t\",\"inputs\":{\"m\":0},\"layers\":[{\"sample\":[\"mat_a\",\"mat_b\",\"\"],\"sample_sel\":\"m\"}]}", err, sizeof(err));
    CHECK(ri >= 0, "a sample list recipe loads (%s)", ri >= 0 ? "ok" : err);
    if (ri >= 0) {
        const SynRecipe *r = g_lib.recipe[ri];
        CHECK(!strcmp(layer_patch(r, "m", 0, 1), "sample:mat_a_1") && !strcmp(layer_patch(r, "m", 1, 1), "sample:mat_b") &&
                  !layer_patch(r, "m", 2, 1)[0] && !layer_patch(r, "m", 9, 1)[0],
              "sample_sel picks the list entry (takes or the file; \"\" and past the end: nothing)");
    }
    ri = syn_lib_add_recipe(&g_lib, "{\"name\":\"takes_none\",\"layers\":[{\"sample\":[\"zz\",\"yy\"]}]}", err, sizeof(err));
    CHECK(ri < 0, "a list with no sample at all is dropped (%s)", err);
}

// ------------------------------------------------------------------ mixer

static void test_mixer(void) {
    char err[256];
    static SynPatch click;
    bool ok = parse_compile("{\"name\":\"click\",\"nodes\":[{\"type\":\"impulse\",\"amp\":1}]}", &click, err, sizeof(err));
    CHECK(ok, "click (%s)", err);
    // Start delay: d / c lands within one block of the exact sample.
    MixStart s;
    memset(&s, 0, sizeof(s));
    s.nlayers = 1;
    s.layer[0].patch = &click;
    s.layer[0].gain = 1.0f;
    s.volume = 1.0f;
    s.score = 0.5f;
    env_params_neutral(&s.env_params);
    float d = 100.0f;
    s.start_delay = d / 343.0f;
    g_audio_render_allocs = 0;
    MixHandle h = mixer_start(g_mix, &s);
    CHECK(h != 0, "voice started");
    static float st[2 * 48000];
    mixer_render(g_mix, st, 24000);
    int first = -1;
    for (int i = 0; i < 24000 && first < 0; i++) if (fabsf(st[i * 2]) > 1e-3f) first = i;
    int want = (int)(d / 343.0f * SR);
    CHECK(first >= 0 && abs(first - want) <= SYN_BLOCK, "propagation delay: first sample %d, d/c = %d", first, want);
    CHECK(abs(first - want) <= 1, "propagation delay sample-exact (%d vs %d)", first, want);
    mixer_render(g_mix, st, 4800);
    mixer_update(g_mix);
    CHECK(!mixer_alive(g_mix, h), "a finished voice frees its slot");
    // Fill every slot: a more important voice steals the least important.
    static SynPatch tone;
    ok = parse_compile("{\"name\":\"tone\",\"loop\":true,\"nodes\":[{\"type\":\"osc\",\"hz\":220,\"amp\":0.01}]}", &tone, err, sizeof(err));
    s.layer[0].patch = &tone;
    s.start_delay = 0;
    MixHandle hs[MIX_VOICES];
    for (int i = 0; i < MIX_VOICES; i++) { s.score = 0.1f + 0.001f * (float)i; hs[i] = mixer_start(g_mix, &s); }
    mixer_render(g_mix, st, 480);
    s.score = 0.05f;
    CHECK(mixer_start(g_mix, &s) == 0, "a less important voice does not steal");
    s.score = 0.9f;
    MixHandle hv = mixer_start(g_mix, &s);
    CHECK(hv != 0 && !mixer_alive(g_mix, hs[0]) && mixer_alive(g_mix, hs[1]), "the least important voice is stolen");
    mixer_render(g_mix, st, 480);
    for (int i = 0; i < MIX_VOICES; i++) mixer_stop(g_mix, hs[i], 0.01f);
    mixer_stop(g_mix, hv, 0.01f);
    for (int i = 0; i < 10; i++) mixer_render(g_mix, st, 480);
    mixer_update(g_mix);
    int alive = 0;
    for (int i = 0; i < MIX_VOICES; i++) alive += mixer_alive(g_mix, hs[i]);
    CHECK(alive == 0 && !mixer_alive(g_mix, hv), "stop fades out and frees (%d still alive)", alive);
    CHECK(g_audio_render_allocs == 0, "mixer_render never allocates (%d allocations)", g_audio_render_allocs);
}

// NaN / denormal safety: hostile parameters, and long decays under flush-to-zero.
static void test_numeric(void) {
    char err[256];
    static SynPatch p;
    const char *hostile = "{\"name\":\"h\",\"params\":{\"x\":0},\"nodes\":["
                          "{\"id\":\"n\",\"type\":\"noise\"},{\"id\":\"i\",\"type\":\"impulse\",\"amp\":1e6},"
                          "{\"id\":\"b\",\"type\":\"biquad\",\"in\":\"n\",\"hz\":0,\"q\":0},{\"id\":\"s\",\"type\":\"svf\",\"in\":\"b\",\"hz\":1e9,\"q\":1e-9},"
                          "{\"id\":\"m\",\"type\":\"modal\",\"in\":\"i\",\"modes\":[[0,1e9,1],[30000,0,1]]},"
                          "{\"id\":\"k\",\"type\":\"karplus\",\"hz\":0,\"t60\":1e9},{\"id\":\"w\",\"type\":\"waveguide\",\"in\":\"s\",\"reflect\":5,\"length\":0},"
                          "{\"id\":\"c\",\"type\":\"comb\",\"in\":\"w\",\"fb\":3,\"ms\":-5},{\"id\":\"o\",\"type\":\"osc\",\"hz\":-100,\"pw\":7},"
                          "{\"id\":\"mx\",\"type\":\"mix\",\"in\":[\"s\",\"m\",\"k\",\"c\",\"o\"]}],\"out\":\"mx\"}";
    bool ok = parse_compile(hostile, &p, err, sizeof(err));
    CHECK(ok, "hostile patch compiles (%s)", err);
    static float out[48000];
    render_patch(&p, out, 48000, 1);
    bool finite = true;
    for (int i = 0; i < 48000; i++) finite = finite && isfinite(out[i]) && fabsf(out[i]) <= 1e4f;
    CHECK(finite, "hostile parameters stay finite and bounded");
    // A long resonator decay through the mixer: flush-to-zero leaves no denormals.
    ok = parse_compile("{\"name\":\"d\",\"loop\":true,\"nodes\":[{\"id\":\"i\",\"type\":\"impulse\"},{\"type\":\"modal\",\"in\":\"i\",\"modes\":[[300,0.05,1]]}]}", &p, err, sizeof(err));
    MixStart s;
    memset(&s, 0, sizeof(s));
    s.nlayers = 1;
    s.layer[0].patch = &p;
    s.layer[0].gain = 1.0f;
    s.volume = 1.0f;
    s.score = 1.0f;
    env_params_neutral(&s.env_params);
    MixHandle h = mixer_start(g_mix, &s);
    static float st[2 * 48000];
    int denorm = 0;
    for (int b = 0; b < 20; b++) {
        mixer_render(g_mix, st, 48000);
        for (int i = 0; i < 96000; i++) {
            float a = fabsf(st[i]);
            if (a != 0.0f && a < 1.17549435e-38f) denorm++;
        }
    }
    CHECK(denorm == 0, "no denormal output after 20 s of decay (%d)", denorm);
    mixer_stop(g_mix, h, 0.01f);
    mixer_render(g_mix, st, 4800);
    mixer_update(g_mix);
}

// Smoothing: a gain parameter jumping 0.2 -> 1.0 must not splatter energy above the tone (a zipper /
// click). Compared with the same jump unsmoothed.
static void test_zipper(void) {
    char err[256];
    static SynPatch p;
    bool ok = parse_compile("{\"name\":\"z\",\"params\":{\"g\":0.2},\"nodes\":[{\"id\":\"o\",\"type\":\"osc\",\"hz\":1000},{\"type\":\"gain\",\"amp\":\"$g\",\"in\":\"o\"}]}", &p, err, sizeof(err));
    CHECK(ok, "zipper patch (%s)", err);
    const int N = 8192;
    static float out[2][8192];
    for (int smooth = 0; smooth < 2; smooth++) {
        SynVoice v;
        syn_voice_init(&v, &p, g_arena, NULL, 1);
        for (int off = 0; off < N; off += SYN_BLOCK) {
            if (off == N / 2) {
                if (smooth) syn_voice_set_param(&v, 0, 1.0f);
                else syn_voice_snap_param(&v, 0, 1.0f);
            }
            syn_voice_render(&v, out[smooth] + off, SYN_BLOCK, g_scratch);
        }
    }
    float *ps = malloc(sizeof(float) * N / 2);
    float hf[2], tone[2];
    for (int k = 0; k < 2; k++) {
        power_spectrum(out[k], N, ps);
        hf[k] = band_energy(ps, N, 3000, 20000);
        tone[k] = band_energy(ps, N, 900, 1100);
    }
    free(ps);
    float sm = 10.0f * log10f(hf[1] / tone[1]), raw = 10.0f * log10f(hf[0] / tone[0]);
    CHECK(sm < -70.0f, "smoothed jump: energy above 3 kHz %.1f dB below the tone", sm);
    CHECK(sm < raw - 20.0f, "smoothing removes the zipper (%.1f dB smoothed vs %.1f dB stepped)", sm, raw);
}

// The death ring (formerly audio/death_synth.c, test_revive): pitch, pop-free gain steps, heartbeat.
static void test_death_ring(void) {
    int pi = syn_lib_find_patch(&g_lib, "death_ring");
    CHECK(pi >= 0, "death_ring patch");
    if (pi < 0) return;
    const SynPatch *p = g_lib.patch[pi];
    int ig = syn_patch_param(p, "ring_gain"), iw = syn_patch_param(p, "warble_dev"), ih = syn_patch_param(p, "heart_gain"), ib = syn_patch_param(p, "heart_bpm");
    static float buf[48000];
    SynVoice v;
    syn_voice_init(&v, p, g_arena, NULL, 1);
    syn_voice_snap_param(&v, iw, 0.0f);
    for (int off = 0; off < 48000; off += SYN_BLOCK) syn_voice_render(&v, buf + off, SYN_BLOCK, g_scratch);
    float mx = 0;
    for (int i = 0; i < 48000; i++) mx = fmaxf(mx, fabsf(buf[i]));
    CHECK(mx == 0.0f, "silent until targeted (%.2g)", mx);
    syn_voice_set_param(&v, ig, 0.3f);
    for (int off = 0; off < 48000; off += SYN_BLOCK) syn_voice_render(&v, buf + off, SYN_BLOCK, g_scratch);
    int zc = 0;
    for (int i = 24000; i < 48000 - 1; i++) if ((buf[i] < 0) != (buf[i + 1] < 0)) zc++;
    float hz = (float)zc / 2.0f / 0.5f;
    CHECK(fabsf(hz - 3600.0f) < 40.0f, "ring pitch %.0f Hz", hz);
    float slope = (float)(6.2831853 * 3600.0 / SR) * 0.3f * 0.5f * 1.16f;
    float maxd = 0;
    for (int i = 1; i < 48000; i++) maxd = fmaxf(maxd, fabsf(buf[i] - buf[i - 1]));
    CHECK(maxd <= slope * 1.05f, "no pop on the attack (max delta %.4f, tone slope %.4f)", maxd, slope);
    syn_voice_set_param(&v, ig, 0.0f);
    for (int off = 0; off < 24000; off += SYN_BLOCK) syn_voice_render(&v, buf + off, SYN_BLOCK, g_scratch);
    maxd = 0;
    for (int i = 1; i < 24000; i++) maxd = fmaxf(maxd, fabsf(buf[i] - buf[i - 1]));
    CHECK(maxd <= slope * 1.05f, "no pop on the stop (max delta %.4f)", maxd);
    CHECK(fabsf(buf[23999]) < 1e-4f, "silent after the release (%.2g)", buf[23999]);
    // Heartbeat: thumps at the rate.
    syn_voice_init(&v, p, g_arena, NULL, 1);
    syn_voice_snap_param(&v, ih, 0.6f);
    syn_voice_snap_param(&v, ib, 60.0f);
    for (int off = 0; off < 48000; off += SYN_BLOCK) syn_voice_render(&v, buf + off, SYN_BLOCK, g_scratch);
    float e_beat = 0, e_gap = 0;
    for (int i = 0; i < 4800; i++) e_beat += buf[i] * buf[i];
    for (int i = 33600; i < 38400; i++) e_gap += buf[i] * buf[i];
    CHECK(e_beat > 50.0f * fmaxf(e_gap, 1e-9f), "heartbeat pulses (%.3f vs %.6f)", e_beat, e_gap);
}

// This machine's speed: a serial one-pole recursion (the inner loop of most nodes and the chain),
// median of five runs, against ~2 ns on a current desktop. Budgets scale by it on slower machines
// (shared CI VMs), never below 1.
static float machine_factor(void) {
    float t[5];
    for (int r = 0; r < 5; r++) {
        volatile float seed = 0.999f;
        float z = 0, k = seed;
        double t0 = now_s();
        for (int i = 0; i < 20000000; i++) z += k * (1.0f - z);
        t[r] = (float)((now_s() - t0) * 1e9 / 2e7);
        if (z < 0) t[r] = 0;
    }
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++) if (t[j] < t[i]) { float x = t[i]; t[i] = t[j]; t[j] = x; }
    float f = t[2] / 2.0f;
    printf("  machine: %.2f ns per serial one-pole step (factor %.2f)\n", t[2], f < 1 ? 1.0f : f);
    return f < 1 ? 1.0f : f;
}

// The gameplay patches (not the UI-only death ring) round robin.
static const SynPatch *budget_patch(int i) {
    for (int k = 0; k < g_lib.npatches; k++) {
        const SynPatch *p = g_lib.patch[(i + k) % g_lib.npatches];
        if (strncmp(p->name, "sample:", 7) && strcmp(p->name, "death_ring")) return p;
    }
    return g_lib.patch[0];
}

// Budget: 64 synthesized voices (the shipped patches round robin, as loops so all 64 render the whole
// time, with no degradation) + their chains.
static SynPatch g_loops[16];
static void test_budget(void) {
    mixer_force_degrade(g_mix, 0);
    int nl = 0;
    for (int i = 0; i < g_lib.npatches && nl < 16; i++) {
        const SynPatch *p = budget_patch(i);
        bool dup = false;
        for (int k = 0; k < nl; k++) dup |= !strcmp(g_loops[k].name, p->name);
        if (dup) continue;
        g_loops[nl] = *p;
        g_loops[nl++].loop = true;
    }
    MixStart s;
    MixHandle h[64];
    int n = 0;
    float gs = 0;
    for (int i = 0; i < 64; i++) {
        const SynPatch *p = &g_loops[i % nl];
        memset(&s, 0, sizeof(s));
        s.nlayers = 1;
        s.layer[0].patch = p;
        s.layer[0].gain = 0.05f;
        s.volume = 1.0f;
        s.score = 0.5f;
        s.seed = (u32)i;
        s.env = true;
        env_params_neutral(&s.env_params);
        s.env_params.spread = 0.0f;
        s.env_params.pan = -0.8f + 1.6f * (float)i / 63.0f;
        s.env_params.lp_hz = 6000.0f;
        s.env_params.band_db[2] = -6.0f;
        s.env_params.send[0] = 0.2f;
        s.env_params.ntaps = 6;
        for (int t = 0; t < 6; t++) s.env_params.tap[t] = (EnvTap){ 0.005f + 0.007f * (float)t, 0.3f, 8000.0f, t & 1 ? 0.5f : -0.5f };
        h[n++] = mixer_start(g_mix, &s);
        gs += 1;
    }
    (void)gs;
    static float st[2 * 480];
    // The voices end at their own pace; restart the ended ones so 64 run for the whole measurement.
    double total = 0;
    int bufs = 0;
    for (int it = 0; it < 400; it++) {
        mixer_update(g_mix);
        for (int i = 0; i < n; i++) {
            if (!mixer_alive(g_mix, h[i])) {
                memset(&s, 0, sizeof(s));
                s.nlayers = 1;
                s.layer[0].patch = &g_loops[(i + it) % nl];
                s.layer[0].gain = 0.05f;
                s.volume = 1.0f;
                s.score = 0.5f;
                s.env = true;
                env_params_neutral(&s.env_params);
                s.env_params.spread = 0;
                s.env_params.ntaps = 6;
                for (int t = 0; t < 6; t++) s.env_params.tap[t] = (EnvTap){ 0.005f + 0.007f * (float)t, 0.3f, 8000.0f, 0.3f };
                h[i] = mixer_start(g_mix, &s);
            }
        }
        double t0 = now_s();
        mixer_render(g_mix, st, 480);
        double dt = now_s() - t0;
        if (it >= 50) { total += dt; bufs++; }
    }
    MixStats ms = mixer_stats(g_mix);
    float us = (float)(total / bufs * 1e6), f = machine_factor();
    printf("  budget: 64 voices + chains: %.0f us per 10 ms buffer (%.1f us / voice), %d active, degrade %d\n", us, us / 64.0f, ms.active, ms.degrade);
    // Synth <= 1 ms and chains <= 15 us / voice: together 1.96 ms per 10 ms buffer (on a desktop).
    CHECK(us < 1960.0f * f, "64 synth voices + chains within budget (%.0f us per buffer, budget %.0f)", us, 1960.0f * f);
    // The chain alone: 15 us per voice per buffer on average (here with 6 taps: the worst case).
    static EnvChain ch;
    EnvParams ep;
    env_params_neutral(&ep);
    ep.spread = 0, ep.pan = 0.4f, ep.lp_hz = 7000, ep.band_db[2] = -6, ep.send[0] = 0.3f, ep.ntaps = 3;
    for (int t = 0; t < 3; t++) ep.tap[t] = (EnvTap){ 0.004f + 0.006f * (float)t, 0.3f, 9000.0f, 0.3f };
    env_chain_init(&ch, SR, NULL, 0, &ep);
    float in[SYN_BLOCK], l[SYN_BLOCK], r[SYN_BLOCK], sd[SYN_BLOCK];
    float *sends[ENV_ZONES] = { sd, sd, sd };
    for (int i = 0; i < SYN_BLOCK; i++) in[i] = 0.1f * sinf((float)i);
    double t0 = now_s();
    for (int b = 0; b < 7500; b++) { memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); env_chain_process(&ch, in, SYN_BLOCK, l, r, sends); }
    float cus = (float)((now_s() - t0) * 1e6 / 1000.0);
    printf("  chain: %.1f us per voice per 10 ms buffer (3 taps, sends, ITD)\n", cus);
    CHECK(cus < 15.0f * f, "environment chain within budget (%.1f us, budget %.1f)", cus, 15.0f * f);
    CHECK(ms.active >= 64, "64 voices active during the measurement (%d)", ms.active);
    for (int i = 0; i < n; i++) mixer_stop(g_mix, h[i], 0.005f);
    for (int i = 0; i < 4; i++) mixer_render(g_mix, st, 480);
    mixer_update(g_mix);
    mixer_force_degrade(g_mix, -1);
}

int main(void) {
    make_samples();
    g_mix = mixer_create(SR, now_s);
    load_library();
    test_compiler();
    test_filters();
    test_envelope();
    test_modal_noise();
    test_expressions();
    test_library();
    test_recipes();
    test_takes();
    test_mixer();
    test_numeric();
    test_zipper();
    test_death_ring();
    test_budget();
    mixer_destroy(g_mix);
    syn_lib_free(&g_lib);
    printf("test_synth: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
