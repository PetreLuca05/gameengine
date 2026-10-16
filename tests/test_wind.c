// Foliage wind tests (docs/FOLIAGE_WIND.md):
//   wind field   determinism; gust fronts travel at the wind speed; the render tile sampled on the CPU
//                (as the shader does) equals wind_at at texel centres within half precision and is
//                continuous inside the window; the settled-bend spring lags a gust and rebounds; the
//                toroidal window keeps springs across a move; sources (blast ring, wake) push the right way
//   WPO model    main bending keeps every vertex's distance to the pivot and the trunk length within
//                1 % at max bend; each layer's amplitude is linear in its stiffness; neighbouring
//                instances are decorrelated (< 0.2 over 600 frames); the declared bounds contain every
//                displaced vertex at any wind; normals stay unit and follow the bend
//   import       Tree It vertex-colour channel detection over assets/treefbx: the same mapping for
//                every export (R flutter, G phase, B branch), import remap / normalisation
#include "world/wind.h"
#include "render/vgeo/foliage_wind.h"
#include "render/vgeo/foliage_import.h"
#include "render/vgeo/vgeo_gpu.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;   // the core library logs through it
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

static const float DT = 1.0f / 60.0f;

static WindField g_wf;   // large: static

static void field(WindField *f, float dir, float speed, float gust, double t) {
    WindBase b = { dir, speed, gust };
    wind_set(f, &b, t);
}

// ------------------------------------------------------------------ wind field

static void test_determinism(void) {
    printf("wind: determinism\n");
    static WindField a, b;
    wind_init(&a, 77);
    wind_init(&b, 77);
    int diff = 0;
    for (int i = 0; i < 200; i++) {
        double t = i * 0.37;
        field(&a, 30.0f, 9.0f, 0.6f, t);
        field(&b, 30.0f, 9.0f, 0.6f, t);
        float p[3] = { (float)(i * 13 % 300) - 150.0f, 2.0f, (float)(i * 7 % 280) - 140.0f }, wa[3], wb[3];
        wind_at(&a, p, wa);
        wind_at(&b, p, wb);
        diff += memcmp(wa, wb, 12) != 0;
    }
    CHECK(diff == 0, "wind_at differs between two fields with the same input (%d)", diff);
    // direction convention: 90 deg blows towards +X; the mean over a big area follows the base
    field(&a, 90.0f, 10.0f, 0.3f, 12.0);
    double sx = 0, sz = 0, sp = 0;
    for (int j = 0; j < 40; j++)
        for (int i = 0; i < 40; i++) {
            float p[3] = { i * 23.0f, 0, j * 23.0f }, w[3];
            wind_at(&a, p, w);
            sx += w[0], sz += w[2], sp += sqrt(w[0] * w[0] + w[2] * w[2]);
        }
    sx /= 1600, sz /= 1600, sp /= 1600;
    CHECK(sx > 7.0 && fabs(sz) < 1.5, "90 deg wind mean (%.2f, %.2f)", sx, sz);
    CHECK(sp > 8.0 && sp < 13.0, "mean speed %.2f for a 10 m/s base", sp);
    float mx = 0;
    field(&a, 0.0f, 30.0f, 1.0f, 3.0);
    for (int i = 0; i < 2000; i++) {
        float p[3] = { i * 3.1f, 0, i * 1.7f }, w[3];
        wind_at(&a, p, w);
        mx = fmaxf(mx, sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]));
    }
    CHECK(mx <= WIND_MAX_SPEED + 1e-3f, "wind_at exceeds WIND_MAX_SPEED: %.2f", mx);
}

// The gust pattern along the wind at t + dt equals the pattern at t shifted downwind by U dt.
static void test_gust_front(void) {
    printf("wind: gust fronts travel at the wind speed\n");
    WindField *f = &g_wf;
    wind_init(f, 3);
    const float U = 12.0f, dir = 90.0f;   // towards +X
    float best_v = 0;
    double best_err = 1e9;
    enum { N = 600 };
    static float g0[N], g1[N];
    field(f, dir, U, 0.8f, 100.0);
    for (int i = 0; i < N; i++) { float p[3] = { i * 1.0f, 0, 5.0f }; g0[i] = wind_gust_at(f, p); }
    const float dt = 2.0f;
    field(f, dir, U, 0.8f, 100.0 + dt);
    for (int i = 0; i < N; i++) { float p[3] = { i * 1.0f, 0, 5.0f }; g1[i] = wind_gust_at(f, p); }
    for (int sh = 0; sh <= 60; sh++) {   // the shift (m) that best explains g1 from g0
        double e = 0;
        for (int i = 60; i < N; i++) e += (g1[i] - g0[i - sh]) * (g1[i] - g0[i - sh]);
        if (e < best_err) { best_err = e; best_v = sh / dt; }
    }
    CHECK(fabsf(best_v - U) <= 0.5f + 1e-3f, "gust pattern moves at %.2f m/s, wind %.1f m/s", best_v, U);
    CHECK(best_err < 1e-6, "gust pattern shifted exactly (residual %.2e)", best_err);
    // a gust front ahead of a point reaches it after distance / U
    float p[3] = { 0, 0, 5.0f }, fac = 0;
    field(f, dir, U, 0.8f, 200.0);
    float dist = wind_gust_front(f, p, &fac);
    if (dist > 4.0f && dist < 200.0f) {
        field(f, dir, U, 0.8f, 200.0 + dist / U);
        float g = wind_gust_at(f, p);
        CHECK(fabsf(g - fac) < 0.05f, "the front %.1f m upwind (factor %.2f) arrives after %.2f s with %.2f", dist, fac, dist / U, g);
    } else CHECK(dist >= 0, "gust front distance %.1f", dist);
    // gusts and lulls both happen at gust 1
    float lo = 9, hi = 0;
    for (int i = 0; i < N; i++) { lo = fminf(lo, g0[i]); hi = fmaxf(hi, g0[i]); }
    CHECK(lo < 0.75f && hi > 1.4f, "gust factor range %.2f .. %.2f (lulls and gusts)", lo, hi);
}

static void test_tile(void) {
    printf("wind: render tile vs wind_at\n");
    WindField *f = &g_wf;
    wind_init(f, 11);
    field(f, 45.0f, 14.0f, 0.7f, 50.0);
    float cam[3] = { 37.0f, 1.7f, -91.0f };
    wind_tile_update(f, cam, DT);
    // texel centres: the tile's instant channels equal wind_at within half precision
    double worst = 0;
    for (int j = 2; j < WIND_TEX - 2; j += 3)
        for (int i = 2; i < WIND_TEX - 2; i += 3) {
            float x = ((float)(f->ox + i) + 0.5f) * WIND_TEXEL, z = ((float)(f->oz + j) + 0.5f) * WIND_TEXEL, s[4], w[3];
            float p[3] = { x, cam[1], z };
            wind_tile_sample(f, x, z, s);
            wind_at(f, p, w);
            worst = fmax(worst, fmax(fabs(s[0] - w[0]), fabs(s[1] - w[2])) / fmax(1.0, sqrt(w[0] * w[0] + w[2] * w[2])));
        }
    CHECK(worst < 2e-3, "tile instant wind vs wind_at at texel centres: %.2e relative", worst);
    // between texels: bilinear, so within the wind's own variation over a texel
    double worst_mid = 0;
    for (int k = 0; k < 400; k++) {
        float x = cam[0] - 100.0f + (k % 20) * 10.3f, z = cam[2] - 100.0f + (k / 20) * 10.3f, s[4], w[3];
        float p[3] = { x, cam[1], z };
        wind_tile_sample(f, x, z, s);
        wind_at(f, p, w);
        worst_mid = fmax(worst_mid, sqrt((s[0] - w[0]) * (s[0] - w[0]) + (s[1] - w[2]) * (s[1] - w[2])));
    }
    CHECK(worst_mid < 0.15 * 14.0, "tile between texels vs wind_at: %.2f m/s", worst_mid);
    float s[4];
    CHECK(!wind_tile_sample(f, cam[0] + 400.0f, cam[2], s), "outside the window: global fallback");
    // first update: settled = instant
    float x = cam[0], z = cam[2];
    wind_tile_sample(f, x, z, s);
    CHECK(fabsf(s[0] - s[2]) < 1e-2f && fabsf(s[1] - s[3]) < 1e-2f, "a fresh tile starts settled");
    // moving the camera by a few texels keeps the springs of the texels still in the window
    for (int i = 0; i < 30; i++) {
        field(f, 45.0f, 14.0f, 0.7f, 50.0 + i * DT);
        wind_tile_update(f, cam, DT);
    }
    float before[4], after[4];
    wind_tile_sample(f, x + 2.0f, z + 2.0f, before);
    float cam2[3] = { cam[0] + 3 * WIND_TEXEL, cam[1], cam[2] - 2 * WIND_TEXEL };
    i32 ox = f->ox;
    field(f, 45.0f, 14.0f, 0.7f, 50.0 + 30 * DT);
    wind_tile_update(f, cam2, 0.0f);   // dt 0: springs do not move
    wind_tile_sample(f, x + 2.0f, z + 2.0f, after);
    CHECK(f->ox == ox + 3, "window moved 3 texels (%d -> %d)", ox, f->ox);
    CHECK(fabsf(before[2] - after[2]) < 1e-3f && fabsf(before[3] - after[3]) < 1e-3f, "springs kept across a window move (%.3f vs %.3f)", before[2], after[2]);
}

// A step in the wind: the settled bend lags, overshoots once and settles.
static void test_spring(void) {
    printf("wind: settled bend spring\n");
    WindField *f = &g_wf;
    wind_init(f, 5);
    float cam[3] = { 0, 0, 0 };
    field(f, 0.0f, 2.0f, 0.0f, 0.0);
    wind_tile_update(f, cam, DT);
    float s[4], mx = 0, at_half = 0;
    int frames = (int)(4.0f * WIND_SETTLE_PERIOD / DT);
    for (int i = 1; i <= frames; i++) {
        field(f, 0.0f, 14.0f, 0.0f, i * DT);   // gust 0 still carries light gusts: compare to the instant value
        wind_tile_update(f, cam, DT);
        wind_tile_sample(f, 0, 0, s);
        float inst = sqrtf(s[0] * s[0] + s[1] * s[1]), bend = sqrtf(s[2] * s[2] + s[3] * s[3]);
        if (i == (int)(0.15f * WIND_SETTLE_PERIOD / DT)) at_half = bend / inst;
        mx = fmaxf(mx, bend / inst);
    }
    float inst = sqrtf(s[0] * s[0] + s[1] * s[1]), bend = sqrtf(s[2] * s[2] + s[3] * s[3]);
    CHECK(at_half < 0.6f, "the bend lags the instant wind (%.2f of it after 0.15 periods)", at_half);
    CHECK(mx > 1.05f && mx < 1.6f, "one rebound: max %.2f of the instant wind", mx);
    CHECK(fabsf(bend / inst - 1.0f) < 0.1f, "settles to the wind (%.2f)", bend / inst);
}

static void test_sources(void) {
    printf("wind: local sources\n");
    WindField *f = &g_wf;
    wind_init(f, 9);
    field(f, 0.0f, 0.0f, 0.0f, 0.0);
    float c[3] = { 10, 0, 10 };
    wind_add_blast(f, c, 30.0f, 25.0f);
    wind_sources_step(f, 0.2f);   // ring at 12 m
    float p[3] = { 22, 0, 10 }, w[3];
    wind_at(f, p, w);
    CHECK(w[0] > 8.0f && fabsf(w[2]) < 1.0f, "blast ring pushes outwards (%.1f, %.1f)", w[0], w[2]);
    for (int i = 0; i < 200; i++) wind_sources_step(f, 0.05f);
    wind_at(f, p, w);
    CHECK(fabsf(w[0]) < 1e-3f, "the blast has died (%.3f)", w[0]);
    float pos[3] = { 0, 0, 0 }, vel[3] = { 0, 0, 20 };
    wind_add_wake(f, pos, vel, 3.0f);
    float behind[3] = { 0, 0, -4 }, side[3] = { 9, 0, -4 };
    wind_at(f, behind, w);
    CHECK(w[2] > 3.0f, "wake pushes along the travel behind the vehicle (%.1f)", w[2]);
    wind_at(f, side, w);
    CHECK(fabsf(w[2]) < 0.5f, "wake is narrow (%.2f at 9 m to the side)", w[2]);
}

// ------------------------------------------------------------------ WPO model

static void params(int preset, float height, float wpo[4], float wp[8]) {
    VgWindParams p;
    vgeo_wind_defaults(&p, preset, height);
    vgeo_wind_pack(&p, wpo, wp);
}

static VgWindVertex vert(float x, float y, float z, const float col[4]) {
    VgWindVertex v;
    memset(&v, 0, sizeof(v));
    v.p[0] = x, v.p[1] = y, v.p[2] = z;
    v.n[0] = 0.6f, v.n[1] = 0.0f, v.n[2] = 0.8f;
    v.pscale = 1.0f;
    memcpy(v.col, col, 16);
    v.phase = 0.3f;
    return v;
}

static void test_trunk_length(void) {
    printf("wpo: main bending keeps the trunk length\n");
    const char *names[3] = { "grass", "bush", "tree" };
    float heights[3] = { 0.6f, 1.8f, 12.0f };
    for (int pr = 0; pr < 3; pr++) {
        float wpo[4], wp[8];
        params(pr, heights[pr], wpo, wp);
        float wind[4] = { 0, 60.0f, 0, 60.0f };   // far beyond full wind: max bend
        float col[4] = { 0, 0, 0, 0 };
        double worst_r = 0, arc0 = 0, arc1 = 0;
        float prev[3] = { 0, 0, 0 };
        float bent_tip = 0;
        for (int i = 1; i <= 64; i++) {
            float y = heights[pr] * (float)i / 64.0f, o[3];
            VgWindVertex v = vert(0, y, 0, col);
            vgeo_wind_eval(wpo, wp, &v, wind, 0.0f, VGW_MAIN, NULL, o, NULL);
            float q[3] = { o[0], y + o[1], o[2] };
            worst_r = fmax(worst_r, fabs(sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) - y) / y);
            arc0 += heights[pr] / 64.0f;
            arc1 += sqrt((q[0] - prev[0]) * (q[0] - prev[0]) + (q[1] - prev[1]) * (q[1] - prev[1]) + (q[2] - prev[2]) * (q[2] - prev[2]));
            memcpy(prev, q, 12);
            if (i == 64) bent_tip = atan2f(sqrtf(q[0] * q[0] + q[2] * q[2]), q[1]) * 57.29578f;
        }
        VgWindParams p;
        vgeo_wind_unpack(wpo, wp, &p);
        // the chapter's normalisation keeps every vertex's distance to the pivot (the trunk length);
        // the arc of a strongly bent grass blade still stretches a little (the (h / H)^2 profile)
        CHECK(worst_r < 1e-5, "%s: distance to the pivot changes by %.2e", names[pr], worst_r);
        CHECK(fabs(arc1 / arc0 - 1.0) < (pr == VGWP_GRASS ? 0.03 : 0.01), "%s: trunk arc length %.4f of the straight one at max bend (%.1f deg)", names[pr], arc1 / arc0,
              p.max_bend_deg);
        CHECK(bent_tip > 0.25f * p.max_bend_deg && bent_tip <= p.max_bend_deg + 0.01f, "%s: tip leans %.2f deg (max %.1f)", names[pr], bent_tip, p.max_bend_deg);
        printf("  %s: max bend %.1f deg, tip %.2f deg, arc %.4f, pivot distance %.1e\n", names[pr], p.max_bend_deg, bent_tip, arc1 / arc0, worst_r);
    }
}

static void test_linearity(void) {
    printf("wpo: layer amplitudes are linear in the stiffness\n");
    float wpo[4], wp[8];
    params(VGWP_TREE, 10.0f, wpo, wp);
    wp[4] = 10.0f;   // no bend cap (pure linearity of the main layer before the projection)
    float wind[4] = { 6.0f, 8.0f, 6.0f, 8.0f }, col[4] = { 0.7f, 0.4f, 0.8f, 1 };
    u32 layers[3] = { VGW_MAIN, VGW_DETAIL, VGW_FLUTTER };
    const char *names[3] = { "main", "detail", "flutter" };
    for (int L = 0; L < 3; L++) {
        double worst = 0;
        for (int k = 0; k < 50; k++) {
            VgWindVertex v = vert(1.0f + k * 0.03f, 2.0f + k * 0.15f, -0.5f, col);
            float t = k * 0.173f, o1[3], o2[3], a[4], b[4];
            memcpy(a, wpo, 16);
            memcpy(b, wpo, 16);
            a[L] = L == 0 ? 0.002f : wpo[L];
            b[L] = 2.0f * a[L];
            vgeo_wind_eval(a, wp, &v, wind, t, layers[L], NULL, o1, NULL);
            vgeo_wind_eval(b, wp, &v, wind, t, layers[L], NULL, o2, NULL);
            for (int i = 0; i < 3; i++)   // main: the projection's vertical part is second order (horizontal only)
                if (fabsf(o1[i]) > 1e-5f && !(L == 0 && i == 1)) worst = fmax(worst, fabs(o2[i] / o1[i] - 2.0));
        }
        // the main layer projects back onto the pivot sphere: linear to first order at small bends
        CHECK(worst < (L == 0 ? 0.02 : 1e-4), "%s: doubling the stiffness scales the offset by 2 +- %.2e", names[L], worst);
    }
}

// Neighbouring instances (2.5 m apart, the instance phase hashed from the position as
// vgeo_foliage_instance does), the same wind: the correlation of their offsets over 600 frames, per pair.
static float inst_phase(float x, float z) {
    u32 h = (u32)(i32)floorf(x * 16.0f) * 0x9E3779B1u ^ (u32)(i32)floorf(z * 16.0f) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (float)(h & 0xFFu) / 256.0f;
}
static double series_corr(const float (*a)[3], const float (*b)[3], int n) {
    double worst = 0;
    for (int c = 0; c < 3; c++) {
        double ma = 0, mb = 0, sab = 0, saa = 0, sbb = 0;
        for (int f = 0; f < n; f++) ma += a[f][c], mb += b[f][c];
        ma /= n, mb /= n;
        for (int f = 0; f < n; f++) sab += (a[f][c] - ma) * (b[f][c] - mb), saa += (a[f][c] - ma) * (a[f][c] - ma), sbb += (b[f][c] - mb) * (b[f][c] - mb);
        if (saa > 1e-14 && sbb > 1e-14) worst = fmax(worst, fabs(sab / sqrt(saa * sbb)));
    }
    return worst;
}
static void test_decorrelation(void) {
    printf("wpo: neighbouring instances are decorrelated\n");
    float wpo[4], wp[8];
    params(VGWP_TREE, 10.0f, wpo, wp);
    float wind[4] = { 8.0f, 2.0f, 8.0f, 2.0f }, col[4] = { 0.8f, 0.5f, 0.7f, 1 };
    enum { N = 600, PAIRS = 32 };
    static float a[N][3], b[N][3];
    u32 layers[4] = { VGW_MAIN, VGW_DETAIL, VGW_FLUTTER, VGW_ALL };
    const char *names[4] = { "main", "detail", "flutter", "all" };
    for (int L = 0; L < 4; L++) {
        double worst = 0, mean = 0, pab[3] = { 0 }, paa[3] = { 0 }, pbb[3] = { 0 };
        for (int pr = 0; pr < PAIRS; pr++) {
            float x0 = 7.3f + (pr % 8) * 10.0f, z0 = -3.1f + (pr / 8) * 10.0f;
            VgWindVertex va = vert(x0 + 1.0f, 6.0f, z0 + 0.5f, col), vb;
            va.ip[0] = x0, va.ip[2] = z0, va.phase = inst_phase(x0, z0);
            vb = va;
            vb.p[0] += 2.5f, vb.ip[0] += 2.5f, vb.phase = inst_phase(x0 + 2.5f, z0);
            for (int f = 0; f < N; f++) {
                vgeo_wind_eval(wpo, wp, &va, wind, f * DT, layers[L], NULL, a[f], NULL);
                vgeo_wind_eval(wpo, wp, &vb, wind, f * DT, layers[L], NULL, b[f], NULL);
            }
            double r = series_corr((const float (*)[3])a, (const float (*)[3])b, N);
            worst = fmax(worst, r);
            mean += r / PAIRS;
            for (int c = 0; c < 3; c++) {   // pooled over the pairs, each instance's own mean (its lean) removed
                double ma = 0, mb = 0;
                for (int f = 0; f < N; f++) ma += a[f][c] / N, mb += b[f][c] / N;
                for (int f = 0; f < N; f++) pab[c] += (a[f][c] - ma) * (b[f][c] - mb), paa[c] += (a[f][c] - ma) * (a[f][c] - ma), pbb[c] += (b[f][c] - mb) * (b[f][c] - mb);
            }
        }
        // in sync -> 1; random phases -> 0. A single pair of slow waves over 10 s correlates by
        // |cos(phase difference)| whatever the model, so the per-pair values are reported only.
        double pooled = 0;
        for (int c = 0; c < 3; c++)
            if (paa[c] > 1e-14 && pbb[c] > 1e-14) pooled = fmax(pooled, fabs(pab[c] / sqrt(paa[c] * pbb[c])));
        printf("  %-7s neighbours over %d pairs x %d frames: pooled correlation %.3f (per pair |r| mean %.3f, max %.3f)\n", names[L], PAIRS, N, pooled, mean, worst);
        CHECK(pooled < 0.2, "%s: neighbours move in sync (pooled correlation %.3f)", names[L], pooled);
    }
}

static double frand(u32 *s) {
    *s = *s * 1664525u + 1013904223u;
    return (double)(*s >> 8) / 16777216.0;
}

static void test_bounds(void) {
    printf("wpo: declared bounds contain every displaced vertex\n");
    u32 seed = 1;
    for (int pr = 0; pr < 3; pr++) {
        float H = pr == 0 ? 0.7f : pr == 1 ? 2.0f : 14.0f, wpo[4], wp[8];
        params(pr, H, wpo, wp);
        float lm[3];
        vgeo_wind_layer_max(wpo, wp, lm);
        float bound = lm[0] + lm[1] + lm[2] + 1.1180340f * VG_DISPLACE_MAX;
        double worst = 0;
        for (int k = 0; k < 20000; k++) {
            float sc = 0.6f + (float)frand(&seed) * 1.2f;
            float col[4] = { (float)frand(&seed), (float)frand(&seed), (float)frand(&seed), 1 };
            VgWindVertex v = vert((float)(frand(&seed) * 2 - 1) * H * 0.6f * sc, (float)(frand(&seed) * 1.1 - 0.05) * H * sc, (float)(frand(&seed) * 2 - 1) * H * 0.6f * sc, col);
            v.ip[0] = 3.0f, v.ip[2] = -2.0f;
            v.p[0] += 3.0f, v.p[2] -= 2.0f;
            v.pscale = sc;
            float a = (float)(frand(&seed) * 6.283), sp = (float)(frand(&seed) * 45.0);
            float wind[4] = { cosf(a) * sp, sinf(a) * sp, cosf(a + 0.5f) * sp * 1.3f, sinf(a + 0.5f) * sp * 1.3f };
            float disp[2] = { (float)(frand(&seed) * 2 - 1), (float)(frand(&seed) * 2 - 1) }, o[3];
            vgeo_wind_eval(wpo, wp, &v, wind, (float)frand(&seed) * 1000.0f, VGW_ALL, disp, o, NULL);
            double l = sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            worst = fmax(worst, l / (bound * (sc > 1 ? sc : 1)));
        }
        CHECK(worst <= 1.0, "preset %d: displacement %.3f of the declared bound", pr, worst);
        printf("  preset %d: bound main %.3f + detail %.3f + flutter %.3f m, worst use %.2f\n", pr, lm[0], lm[1], lm[2], worst);
    }
}

static void test_normals(void) {
    printf("wpo: normals\n");
    float wpo[4], wp[8];
    params(VGWP_GRASS, 0.6f, wpo, wp);
    float wind[4] = { 0, 20.0f, 0, 20.0f }, col[4] = { 1, 0.5f, 0, 1 }, o[3], n[3];
    VgWindVertex v = vert(0, 0.6f, 0, col);
    v.n[0] = 0, v.n[1] = 1, v.n[2] = 0;
    vgeo_wind_eval(wpo, wp, &v, wind, 0.0f, VGW_MAIN | VGW_NORMALS, NULL, o, n);
    CHECK(fabsf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] - 1.0f) < 1e-5f, "unit normal");
    CHECK(n[2] > 0.2f && n[1] > 0, "an up normal at the tip tilts downwind (%.2f, %.2f, %.2f)", n[0], n[1], n[2]);
    double worst = 0;
    for (int k = 0; k < 200; k++) {
        vgeo_wind_eval(wpo, wp, &v, wind, k * 0.05f, VGW_ALL, NULL, o, n);
        worst = fmax(worst, fabs(sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0));
    }
    CHECK(worst < 1e-5, "unit normals with flutter (%.1e)", worst);
    // SmoothCurve(TriangleWave(x)) * 2 - 1: -1 at integers, +1 at halves, 0 at quarters, C1 everywhere
    float s0 = vgeo_smooth_triangle(0.0f), s1 = vgeo_smooth_triangle(0.5f), s2 = vgeo_smooth_triangle(0.25f), s3 = vgeo_smooth_triangle(7.0f);
    CHECK(fabsf(s0 + 1.0f) < 1e-6f && fabsf(s1 - 1.0f) < 1e-6f && fabsf(s2) < 1e-6f && fabsf(s3 + 1.0f) < 1e-5f, "smooth triangle wave %.3f %.3f %.3f %.3f", s0, s1, s2, s3);
}

// ------------------------------------------------------------------ Tree It channels

static void test_channels(void) {
    printf("import: Tree It vertex-colour channels (assets/treefbx)\n");
    static const char *files[] = { "beech.fbx", "birch.fbx", "daisy.fbx", "elm.fbx", "fir.fbx", "juniper.fbx", "maple.fbx", "oak.fbx", "old.fbx",
                                   "pine.fbx", "spruce.fbx", "bamboo.obj", "tree1.obj" };
    int ok = 0, n = (int)(sizeof(files) / sizeof(files[0]));
    for (int i = 0; i < n; i++) {
        char path[256], err[256] = "";
        snprintf(path, sizeof(path), "assets/treefbx/%s", files[i]);
        VgFoliageChannelReport r;
        if (!vgeo_foliage_detect_channels(path, &r, err, sizeof(err))) { CHECK(0, "%s: %s", files[i], err); continue; }
        bool m = r.flutter == 0 && r.phase == 1 && r.branch == 2;
        CHECK(m, "%s: detected flutter %c, phase %c, branch %c (%s)", files[i], "RGBA"[r.flutter & 3], "RGBA"[r.phase & 3], "RGBA"[r.branch & 3], r.note);
        CHECK(r.has_color, "%s: has vertex colours", files[i]);
        ok += m;
        printf("  %-12s flutter %c (bark max %.3f, leaf tip corr %+.2f) phase %c (per-piece const %.2f) branch %c (radial corr %+.2f, max %.3f)\n", files[i],
               "RGBA"[r.flutter & 3], r.flutter_bark_max, r.flutter_uv_corr, "RGBA"[r.phase & 3], r.phase_const, "RGBA"[r.branch & 3], r.branch_radial_corr, r.branch_max);
    }
    CHECK(ok == n, "%d / %d exports map to R flutter, G phase, B branch", ok, n);
}

static void test_import(void) {
    printf("import: remap, normalisation, inference\n");
    VgFoliageImportOptions o;
    vgeo_foliage_import_defaults(&o);
    VgFoliageAsset m;
    char err[256] = "";
    bool ok = vgeo_foliage_import("assets/treefbx/pine.fbx", &o, &m, err, sizeof(err));
    CHECK(ok, "pine import: %s", err);
    if (!ok) return;
    CHECK(m.height > 9.0f && m.height < 14.0f, "pine height %.2f m (cm -> m)", m.height);
    u32 bmax = 0, rbark = 0;
    for (u32 i = 0; i < m.im.vertex_count; i++) {
        u32 c = m.color[i];
        bmax = DG_MAX(bmax, (c >> 16) & 0xFFu);
    }
    for (u32 t = 0; t < m.im.index_count / 3; t++)
        if (m.material_kind[m.im.tri_material[t]] == VGFM_BARK) rbark = DG_MAX(rbark, m.color[m.im.indices[t * 3]] & 0xFFu);
    CHECK(bmax >= 250, "branch weight normalised to 1 (max %u / 255)", bmax);
    CHECK(rbark <= 1, "no flutter on the bark (%u)", rbark);
    CHECK(m.im.material_count >= 2, "bark + needles materials (%u)", m.im.material_count);
    vgeo_foliage_asset_free(&m);
    // a remap: the procedural layout (R trunk, G branch, B flutter) read as Tree It -> flutter from B
    o.channels[VGFC_FLUTTER] = 2, o.channels[VGFC_PHASE] = 3, o.channels[VGFC_BRANCH] = 1;
    ok = vgeo_foliage_import("assets/treefbx/oak.fbx", &o, &m, err, sizeof(err));
    CHECK(ok, "oak import with a remap: %s", err);
    if (ok) vgeo_foliage_asset_free(&m);
    // no colours: inference
    vgeo_foliage_import_defaults(&o);
    o.ignore_colors = true;
    ok = vgeo_foliage_import("assets/treefbx/birch.fbx", &o, &m, err, sizeof(err));
    CHECK(ok, "birch import without colours: %s", err);
    if (ok) {
        u32 leaf_tip = 0, trunk_b = 0;
        for (u32 t = 0; t < m.im.index_count / 3; t++) {
            u32 c = m.color[m.im.indices[t * 3]];
            if (m.material_kind[m.im.tri_material[t]] == VGFM_LEAF) leaf_tip = DG_MAX(leaf_tip, c & 0xFFu);
        }
        trunk_b = m.inferred ? 1 : 0;
        CHECK(leaf_tip > 128, "inferred leaf flutter reaches the tips (%u)", leaf_tip);
        CHECK(trunk_b, "marked as inferred");
        vgeo_foliage_asset_free(&m);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_determinism();
    test_gust_front();
    test_tile();
    test_spring();
    test_sources();
    test_trunk_length();
    test_linearity();
    test_decorrelation();
    test_bounds();
    test_normals();
    test_channels();
    test_import();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
