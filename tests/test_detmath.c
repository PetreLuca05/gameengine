// Deterministic float math (src/core/detmath.c, docs/PLATFORM_MACOS.md "Determinism"):
//   - accuracy: every function within 1 ulp of the correctly rounded float (reference: the platform
//     libm in double, rounded once) over dense sweeps of its working range + special values
//   - determinism: a hash of the bit patterns over a fixed set of inputs equals the golden value
//     recorded on Linux x86_64 (gcc); CI runs this on macOS arm64 / x86_64 and Windows, so the same
//     hash there is the cross-platform proof (a mismatch prints the new value)
#include "core/detmath.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;
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

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// Distance in ulps between two finite floats of the same sign (ordered integer mapping).
static int64_t ulps(float a, float b) {
    int32_t ia, ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    if (ia < 0) ia = (int32_t)0x80000000 - ia;
    if (ib < 0) ib = (int32_t)0x80000000 - ib;
    int64_t d = (int64_t)ia - (int64_t)ib;
    return d < 0 ? -d : d;
}

typedef float (*F1)(float);
typedef double (*R1)(double);

static double ref_exp2(double x) { return exp2(x); }
static double ref_log2(double x) { return log2(x); }

// Sweeps n points over [lo, hi] (and their negatives when sym), worst ulp error vs the reference.
static void sweep(const char *name, F1 f, R1 ref, double lo, double hi, int n, int64_t max_ulps) {
    int64_t worst = 0;
    float worst_x = 0;
    for (int i = 0; i <= n; i++) {
        float x = (float)(lo + (hi - lo) * (double)i / (double)n);
        float got = f(x), want = (float)ref((double)x);
        if (isnan(want)) { CHECK(isnan(got), "%s(%.9g): %.9g, want NaN", name, x, got); continue; }
        if (isinf(want)) { CHECK(got == want, "%s(%.9g): %.9g, want %.9g", name, x, got, want); continue; }
        int64_t u = ulps(got, want);
        if (u > worst) { worst = u; worst_x = x; }
    }
    printf("  %-6s [%g, %g]: worst %lld ulp (at %.9g)\n", name, lo, hi, (long long)worst, worst_x);
    CHECK(worst <= max_ulps, "%s: %lld ulp at %.9g", name, (long long)worst, worst_x);
}

static double ref_pow_fixed_y;
static float pow_x(float x) { return dm_powf(x, (float)ref_pow_fixed_y); }
static double ref_pow_x(double x) { return pow(x, (double)(float)ref_pow_fixed_y); }
static double ref_atan2_x;
static float atan2_y(float y) { return dm_atan2f(y, (float)ref_atan2_x); }
static double ref_atan2_y(double y) { return atan2(y, (double)(float)ref_atan2_x); }

static uint64_t fnv(uint64_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 0x100000001B3ull; }
    return h;
}

int main(void) {
    CHECK(ulps(1.0f, nextafterf(1.0f, 2.0f)) == 1 && ulps(-1.0f, 1.0f) > 1000000, "ulp metric");
    printf("accuracy:\n");
    sweep("sin", dm_sinf, sin, -100.0, 100.0, 400000, 1);
    sweep("sin", dm_sinf, sin, -1e5, 1e5, 200000, 1);
    sweep("cos", dm_cosf, cos, -100.0, 100.0, 400000, 1);
    sweep("tan", dm_tanf, tan, -1.5, 1.5, 300000, 1);
    sweep("tan", dm_tanf, tan, -50.0, 50.0, 300000, 1);
    sweep("atan", dm_atanf, atan, -50.0, 50.0, 400000, 1);
    sweep("asin", dm_asinf, asin, -1.0, 1.0, 400000, 1);
    sweep("acos", dm_acosf, acos, -1.0, 1.0, 400000, 1);
    sweep("exp", dm_expf, exp, -100.0, 88.0, 400000, 1);
    sweep("exp2", dm_exp2f, ref_exp2, -140.0, 127.0, 400000, 1);
    sweep("log", dm_logf, log, 1e-30, 10.0, 400000, 1);
    sweep("log", dm_logf, log, 0.5, 2.0, 400000, 1);
    sweep("log2", dm_log2f, ref_log2, 1e-6, 1e6, 400000, 1);
    sweep("cbrt", dm_cbrtf, cbrt, -1e4, 1e4, 400000, 1);
    const float ys[] = { 0.5f, 1.5f, -0.75f, 2.2f, 3.0f, -2.0f, 0.333f };
    for (int i = 0; i < 7; i++) {
        ref_pow_fixed_y = ys[i];
        char name[32];
        snprintf(name, sizeof(name), "pow^%g", ys[i]);
        sweep(name, pow_x, ref_pow_x, 1e-3, 100.0, 100000, 1);
    }
    const float xs[] = { 1.0f, -1.0f, 0.25f, -3.5f };
    for (int i = 0; i < 4; i++) {
        ref_atan2_x = xs[i];
        sweep("atan2", atan2_y, ref_atan2_y, -10.0, 10.0, 100000, 1);
    }
    // special values
    CHECK(isnan(dm_sinf(NAN)) && isnan(dm_sinf(INFINITY)), "sin special");
    CHECK(dm_expf(-INFINITY) == 0.0f && dm_expf(INFINITY) == INFINITY && dm_expf(200.0f) == INFINITY, "exp special");
    CHECK(dm_logf(0.0f) == -INFINITY && isnan(dm_logf(-1.0f)) && dm_logf(1.0f) == 0.0f, "log special");
    CHECK(dm_log2f(8.0f) == 3.0f && dm_exp2f(-3.0f) == 0.125f, "log2 / exp2 exact powers");
    CHECK(dm_powf(-2.0f, 3.0f) == -8.0f && isnan(dm_powf(-2.0f, 0.5f)) && dm_powf(0.0f, -1.0f) == INFINITY && dm_powf(5.0f, 0.0f) == 1.0f, "pow special");
    CHECK(dm_cbrtf(-27.0f) == -3.0f && dm_cbrtf(0.0f) == 0.0f, "cbrt special");
    CHECK(dm_atan2f(0.0f, -1.0f) == (float)3.14159265358979323846 && dm_atan2f(-0.0f, -1.0f) == -(float)3.14159265358979323846 && dm_atan2f(1.0f, 0.0f) == (float)1.57079632679489661923, "atan2 special");
    CHECK(dm_atanf(INFINITY) == (float)1.57079632679489661923, "atan inf");

    // golden hash over fixed inputs (bit patterns only: no libm involved)
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t s = 0x9E3779B9u;
    for (int i = 0; i < 20000; i++) {
        s = s * 1664525u + 1013904223u;
        float u = (float)(s >> 8) / 16777216.0f;   // [0, 1)
        float a = (u - 0.5f) * 200.0f, p = u * 50.0f + 1e-3f;
        h = fnv(h, bits(dm_sinf(a)));
        h = fnv(h, bits(dm_cosf(a)));
        h = fnv(h, bits(dm_tanf(a * 0.01f)));
        h = fnv(h, bits(dm_atanf(a)));
        h = fnv(h, bits(dm_atan2f(a, u - 0.3f)));
        h = fnv(h, bits(dm_asinf(u * 2.0f - 1.0f)));
        h = fnv(h, bits(dm_expf(a * 0.4f)));
        h = fnv(h, bits(dm_exp2f(a * 0.5f)));
        h = fnv(h, bits(dm_logf(p)));
        h = fnv(h, bits(dm_log2f(p)));
        h = fnv(h, bits(dm_powf(p, u * 4.0f - 2.0f)));
        h = fnv(h, bits(dm_cbrtf(a)));
        double e = dm_exp((double)a * 0.1);
        uint64_t eb;
        memcpy(&eb, &e, 8);
        h = fnv(h, (uint32_t)eb);
        h = fnv(h, (uint32_t)(eb >> 32));
    }
    const uint64_t golden = 0x13F54BF6475B830Cull;   // Linux x86_64 gcc; must match on every platform
    printf("determinism hash %016llx (golden %016llx)\n", (unsigned long long)h, (unsigned long long)golden);
    CHECK(h == golden, "determinism hash %016llx differs from the golden %016llx", (unsigned long long)h, (unsigned long long)golden);
    printf("detmath: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
