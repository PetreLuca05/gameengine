// LOD ranges and cross-fades (src/render/lod_fade.c, docs/LOD.md), on the shipped tables
// (assets/render/lod.json, assets/render/fidelity):
//   * lod.json parses and matches the built-in defaults; malformed tables are rejected;
//   * thresholds = lod.json, the same at every Engine Fidelity level and era (docs/SETTINGS_AUDIT.md);
//   * character px -> LOD at the new targets (140 / 60 / 25 px) and the line-up distances;
//   * every LOD switch of a slow dolly (characters: time fades; static chains, impostors: distance
//     bands) is a fade: the silhouette coverage of each LOD changes by less than 5 % per frame;
//   * fades complete on camera reversal (no ping-pong half-fades);
//   * complementary masks: coverage sums to 1 for every dither threshold (no holes, no double
//     shadows), also for the three-way impostor / coarse / fine split, and the classic renderer's
//     parts emit no empty interval (a fine iw / cw sweep near 0 and 1, random values);
//   * foliage density / distance fade: every instance (all 256 hashes) fully opaque before its fade
//     starts, at the shipped instance_fade; density thins by dropping instances, never by a partial
//     alpha (a tree near the camera is never dithered);
//   * streaming: never a lower LOD after a higher one is resident;
//   * terrain geomorph: neighbouring chunks agree on every shared edge vertex, a chunk LOD switch
//     does not move a vertex.
#include "render/lod_fade.h"
#include "render/classic_cull_math.h"
#include "render/fidelity/fidelity.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/platform.h"
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

#define MAX_COVER_STEP 0.05f   // per-frame silhouette coverage change bound of a fade (5 %)
static const float k_tier[FID_LEVELS] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };   // Off .. Modern: the same ranges (docs/SETTINGS_AUDIT.md)

// Character projected height at 1080p, 75 deg vertical FOV, 1.72 m tall (characters.c).
static float char_px(float d) { return 1080.0f * 1.72f / (2.0f * d * tanf(37.5f * 3.14159265f / 180.0f)); }

static void test_table(void) {
    LodTable def, t;
    lod_table_default(&def);
    char *text = platform_read_file("assets/render/lod.json", NULL);
    CHECK(text != NULL, "assets/render/lod.json missing");
    if (!text) return;
    char err[160] = "";
    CHECK(lod_table_parse(&t, text, err, sizeof(err)), "lod.json: %s", err);
    free(text);
    CHECK(fabsf(t.fade_time - def.fade_time) < 1e-6f && fabsf(t.band - def.band) < 1e-6f, "fade time / band differ from the built-ins");
    for (int c = 0; c < LODC_COUNT; c++) CHECK(fabsf(t.cat[c] - def.cat[c]) < 1e-6f, "distance multiplier %d: json %.3f built-in %.3f", c, (double)t.cat[c], (double)def.cat[c]);
    for (int l = 0; l < 5; l++) CHECK(fabsf(t.tier[l] - def.tier[l]) < 1e-6f, "tier %d differs", l);
    for (int e = 0; e < 4; e++)
        for (int c = 0; c < LODC_COUNT; c++) CHECK(fabsf(t.era[e][c] - def.era[e][c]) < 1e-6f, "era %d category %d differs", e, c);
    for (int i = 0; i < 3; i++) CHECK(fabsf(t.char_px[i] - def.char_px[i]) < 1e-6f, "char px %d differs", i);
    CHECK(t.char_px[0] == 140 && t.char_px[1] == 60 && t.char_px[2] == 25, "character targets 140 / 60 / 25 px");
    CHECK(t.cat[LODC_STATIC] == 2 && t.cat[LODC_IMPOSTOR] == 2 && t.cat[LODC_FOLIAGE] == 1.5f && t.cat[LODC_TERRAIN] == 1.5f, "spec multipliers");
    CHECK(fabsf(t.fade_time - 0.35f) < 1e-6f, "lod_fade_time default 0.35 s");
    CHECK(fabsf(t.imp_gain - def.imp_gain) < 1e-6f && fabsf(t.imp_band - def.imp_band) < 1e-6f, "impostor gain / band differ from the built-ins");
    // malformed
    CHECK(!lod_table_parse(&t, "{", err, sizeof(err)), "a parse error is rejected");
    CHECK(!lod_table_parse(&t, "{\"characters\": {\"px\": [10, 60, 25]}}", err, sizeof(err)), "increasing thresholds are rejected");
    CHECK(!lod_table_parse(&t, "{\"characters\": {\"px\": [140, 60]}}", err, sizeof(err)), "a short px array is rejected");
    CHECK(!lod_table_parse(&t, "{\"distance\": {\"static\": 0}}", err, sizeof(err)), "a zero multiplier is rejected");
}

static void test_tiers(void) {
    char err[256] = "";
    CHECK(fid_load_dir("assets/render/fidelity", err, sizeof(err)), "fidelity tables: %s", err);
    LodTable t;
    lod_table_default(&t);
    for (int l = 0; l < FID_LEVELS; l++) {
        CHECK(fabsf(t.tier[l] - k_tier[l]) < 1e-4f, "level %s: tier %.3f, want %.2f", fid_level_id(l), (double)t.tier[l], (double)k_tier[l]);
        // thresholds = lod.json x tier for every category
        for (int c = 0; c < LODC_COUNT; c++) {
            float s = lod_scale_of(&t, (LodCat)c, l, 1.0f, 0);
            CHECK(fabsf(s - t.cat[c] * k_tier[l]) < 1e-4f, "level %d category %d: scale %.3f", l, c, (double)s);
        }
        // the character switch points move with the tier: LOD0 holds above 140 / tier px
        float s = lod_scale_of(&t, LODC_CHARACTER, l, 1.0f, 0);
        float edge = t.char_px[0] / s;
        CHECK(lod_char_select(&t, s, -1, edge * 1.01f) == 0 && lod_char_select(&t, s, -1, edge * 0.99f) == 1, "level %d: LOD0 edge at %.1f px", l, (double)edge);
        // discrete LOD error: 2x the distance at Modern -> half the pixel error
        float lod_px = 2.0f / lod_scale_of(&t, LODC_STATIC, l, 1.0f, 0);
        CHECK(fabsf(lod_px - 2.0f / (2.0f * k_tier[l])) < 1e-4f, "level %d: static LOD px %.3f", l, (double)lod_px);
    }
    // eras keep their own table on top
    CHECK(fabsf(lod_scale_of(&t, LODC_STATIC, FID_MODERN, 1.0f, 3) - 2.0f) < 1e-5f, "2010 era: the same ranges");
    CHECK(fabsf(lod_scale_of(&t, LODC_FOLIAGE, FID_MODERN, 1.0f, 2) - 1.5f) < 1e-5f, "2015 era: the same ranges");
    // the user's r_lod_bias against the level default multiplies in
    CHECK(fabsf(lod_scale_of(&t, LODC_STATIC, FID_HIGH, 2.0f, 0) - 2.0f * 2.0f) < 1e-5f, "bias x2 (dev builds) at High");
    // the tier / era multipliers must be 1: shorter ranges on a low level would hide what others see
    CHECK(!lod_table_parse(&t, "{\"tiers\": {\"high\": 1.2}}", err, sizeof(err)), "a tier multiplier other than 1 is rejected");
    CHECK(!lod_table_parse(&t, "{\"tiers\": {\"off\": 0.3}}", err, sizeof(err)), "a shorter Off range is rejected");
    CHECK(!lod_table_parse(&t, "{\"eras\": {\"2010\": {\"all\": 0.7}}}", err, sizeof(err)), "an era multiplier other than 1 is rejected");
}

static void test_char_mapping(void) {
    LodTable t;
    lod_table_default(&t);
    // new targets: LOD0 while >= 140 px, LOD1 to 60, LOD2 to 25, LOD3 below
    CHECK(lod_char_select(&t, 1, -1, 141) == 0 && lod_char_select(&t, 1, -1, 139) == 1, "140 px edge");
    CHECK(lod_char_select(&t, 1, -1, 61) == 1 && lod_char_select(&t, 1, -1, 59) == 2, "60 px edge");
    CHECK(lod_char_select(&t, 1, -1, 26) == 2 && lod_char_select(&t, 1, -1, 24) == 3, "25 px edge");
    // line-up distances (anim_lod_lineup): 5 / 15 / 30 / 60 / 120 / 240 m
    const float d[6] = { 5, 15, 30, 60, 120, 240 };
    const int want[6] = { 0, 1, 2, 3, 3, 3 };
    for (int i = 0; i < 6; i++) CHECK(lod_char_select(&t, 1, -1, char_px(d[i])) == want[i], "%.0f m (%.0f px): LOD %d", (double)d[i], (double)char_px(d[i]), want[i]);
    // roughly 2x the distance per switch against the old 260 / 120 / 50 px
    const float old_px[3] = { 260, 120, 50 };
    for (int i = 0; i < 3; i++) {
        float ratio = old_px[i] / t.char_px[i];
        CHECK(ratio > 1.8f && ratio < 2.1f, "switch %d: x%.2f distance", i, (double)ratio);
    }
    // hysteresis: a size jittering around a threshold never oscillates
    int cur = 0, switches = 0;
    for (int k = 0; k < 1000; k++) {
        int n = lod_char_select(&t, 1, cur, 140.0f + 10.0f * sinf((float)k));
        switches += n != cur;
        cur = n;
    }
    CHECK(switches <= 1, "jitter +-10 px around 140 px: %d switches", switches);
}

// Slow dolly of a character: per tick (60 Hz) the selector, the time fade, the coverage of every LOD.
static void test_char_dolly(float speed, float fade_time) {
    LodTable t;
    lod_table_default(&t);
    LodFade f = { -1, -1, 1 };
    float prev[LOD_CHAR_COUNT] = { 0 }, worst = 0;
    int fades = 0, cuts = 0;
    const float dt = 1.0f / 60.0f;
    for (float d = 2.0f; d < 300.0f; d += speed * dt) {
        int want = lod_char_select(&t, 1, f.cur, char_px(d));
        bool was = lod_fade_active(&f);
        lod_fade_step(&f, want, dt, fade_time);
        if (!was && lod_fade_active(&f)) fades++;
        int lods[2];
        float fd[2], cov[LOD_CHAR_COUNT] = { 0 };
        int n = lod_fade_draws(&f, lods, fd);
        for (int i = 0; i < n; i++) cov[lods[i]] += lod_coverage(fd[i]);
        float sum = 0;
        for (int l = 0; l < LOD_CHAR_COUNT; l++) sum += cov[l];
        CHECK(fabsf(sum - 1.0f) < 1e-4f, "coverage sums to %.4f", (double)sum);
        if (d > 2.0f)
            for (int l = 0; l < LOD_CHAR_COUNT; l++) {
                float step = fabsf(cov[l] - prev[l]);
                worst = fmaxf(worst, step);
                if (step > MAX_COVER_STEP) cuts++;
            }
        memcpy(prev, cov, sizeof(prev));
    }
    CHECK(fades == 3, "dolly %.1f m/s: %d fades (want 3)", (double)speed, fades);
    CHECK(cuts == 0, "dolly %.1f m/s: %d steps above %.0f %% (worst %.1f %%)", (double)speed, cuts, (double)(MAX_COVER_STEP * 100), (double)(worst * 100));
}

static void test_reversal(void) {
    LodFade f = { -1, -1, 1 };
    lod_fade_step(&f, 0, 0.016f, 0.35f);
    lod_fade_step(&f, 1, 0.1f, 0.35f);   // starts 0 -> 1
    CHECK(lod_fade_active(&f) && f.cur == 1 && f.from == 0, "fade 0 -> 1 started");
    // the camera reverses mid-fade: the selector wants 0 again, the fade still completes
    float t_prev = f.t;
    int frames = 0;
    while (lod_fade_active(&f) && frames < 100) {
        lod_fade_step(&f, 0, 0.016f, 0.35f);
        CHECK(!lod_fade_active(&f) || f.t > t_prev, "fade progress is monotonic");
        t_prev = f.t;
        frames++;
    }
    CHECK(!lod_fade_active(&f) && f.cur == 1, "the 0 -> 1 fade completed (at LOD %d)", f.cur);
    CHECK(frames <= 18, "it took %d frames (0.35 s at 60 fps = 21, 0.1 s were done)", frames);
    lod_fade_step(&f, 0, 0.016f, 0.35f);
    CHECK(lod_fade_active(&f) && f.from == 1 && f.cur == 0, "then the fade back 1 -> 0 starts");
    // zero fade time: cuts
    LodFade g = { -1, -1, 1 };
    lod_fade_step(&g, 0, 0.016f, 0);
    lod_fade_step(&g, 2, 0.016f, 0);
    CHECK(!lod_fade_active(&g) && g.cur == 2, "fade time 0 cuts");
}

static void test_masks(void) {
    // pair: exactly one of in / out keeps each threshold
    int bad = 0;
    for (int ti = 1; ti < 100; ti++) {
        float t = (float)ti / 100.0f, in = lod_fade_in(t), out = lod_fade_out(t);
        for (int k = 0; k < 256; k++) {
            float n = ((float)k + 0.5f) / 256.0f;
            bad += lod_kept(in, n) == lod_kept(out, n);
        }
    }
    CHECK(bad == 0, "complementary pair: %d thresholds kept by both or neither", bad);
    CHECK(lod_kept(0, 0.999f) && lod_kept(0, 0.0f), "fade 0 keeps everything");
    // three-way split (impostor [0, iw), coarse [iw, iw + (1 - iw) cw), fine [.., 1))
    bad = 0;
    for (int a = 0; a <= 20; a++)
        for (int b = 0; b <= 20; b++) {
            float iw = (float)a / 20.0f, cw = (float)b / 20.0f, m = iw + (1 - iw) * cw;
            float p0 = lod_iv_pack(0, iw), p1 = lod_iv_pack(iw, m), p2 = lod_iv_pack(m, 1);
            for (int k = 0; k < 256; k++) {
                float n = ((float)k + 0.5f) / 256.0f;
                int c = (iw > 0 && lod_iv_kept(p0, n)) + (m > iw && lod_iv_kept(p1, n)) + (m < 1 && lod_iv_kept(p2, n));
                bad += c != 1;
            }
        }
    CHECK(bad == 0, "three-way split: %d thresholds not covered exactly once", bad);
    CHECK(lod_iv_pack(0, 1) == LOD_IV_FULL && lod_iv_kept(LOD_IV_FULL, 0.99f) && lod_iv_kept(0, 0.5f), "full interval / no interval");
    CHECK(lod_iv_empty(0, 0.001f) && lod_iv_empty(0.999f, 1) && lod_iv_empty(0.5f, 0.5f) && !lod_iv_empty(0, 0.002f) && !lod_iv_empty(0.3f, 0.7f), "empty intervals");
}

// The classic renderer's parts (classic_cull_math.h clc_split) at every iw / cw: every threshold is
// kept by exactly one part, and no part keeps none. Before lod_iv_empty, an impostor share iw (or a
// coarser share cw at iw = 0) in (0, 1/512) packed [0, 0) as 0, the "whole coverage" sentinel: two
// whole draws of one instance (an impostor over the mesh, or two LODs).
static int split_bad, split_empty;
static void split_check(float iw, float cw) {
    ClcParts o;
    o.n = 0;
    clc_split(&o, 1, iw, cw);
    for (int k = 0; k < 256; k++) {
        float n = ((float)k + 0.5f) / 256.0f;
        int c = 0;
        for (u32 i = 0; i < o.n; i++) c += lod_iv_kept(o.iv[i], n);
        split_bad += c != 1;
    }
    for (u32 i = 0; i < o.n; i++) {
        if (o.iv[i] == 0.0f) { split_empty += o.n > 1; continue; }   // 0: the whole coverage, the only part
        int p = (int)o.iv[i], a = p % 512, b = p / 512;
        split_empty += a >= b;
    }
}

static void test_split_sweep(void) {
    split_bad = split_empty = 0;
    const float others[] = { 0.0f, 1e-6f, 0.5f / 256.0f, 1.0f / 256.0f, 0.25f, 0.5f, 1.0f - 1.0f / 256.0f, 1.0f - 1e-6f, 1.0f };
    const int no = (int)(sizeof(others) / sizeof(others[0]));
    for (int s = 1; s <= 3907; s++) {   // (0, 1/256] in 1e-6 steps, at both ends of [0, 1]
        float t = (float)s * 1e-6f;
        for (int j = 0; j < no; j++) {
            split_check(t, others[j]);
            split_check(others[j], t);
            split_check(1.0f - t, others[j]);
            split_check(others[j], 1.0f - t);
        }
        split_check(t, t);
        split_check(1.0f - t, 1.0f - t);
    }
    u32 seed = 12345;
    for (int i = 0; i < 200000; i++) {   // random, a third of them pushed towards the band edges
        seed = seed * 1664525u + 1013904223u;
        float a = (float)(seed >> 8) / 16777216.0f;
        seed = seed * 1664525u + 1013904223u;
        float b = (float)(seed >> 8) / 16777216.0f;
        if (i % 3 == 1) a *= 0.01f, b *= 0.01f;
        if (i % 3 == 2) a = 1.0f - a * 0.01f, b = b * b;
        split_check(a, b);
    }
    CHECK(split_bad == 0, "classic parts: %d thresholds not covered exactly once", split_bad);
    CHECK(split_empty == 0, "classic parts: %d empty parts emitted", split_empty);
}

// Foliage instance alpha (vk_classic.c cl_foliage_alpha): with instance_fade 0.3 the old mapping
// (alpha = (density - hash x 0.9375) / width) left a quarter of the instances (hash > 0.7) at alpha
// 0.21 .. 1 at every distance: trees and grass dithered see-through up close.
static void test_foliage_alpha(void) {
    LodTable t;
    lod_table_default(&t);
    char err[256];
    char *text = platform_read_file("assets/render/lod.json", NULL);
    if (text) {
        CHECK(lod_table_parse(&t, text, err, sizeof(err)), "lod.json: %s", err);
        free(text);
    }
    const float widths[] = { t.fol_instance, 1.0f / 16.0f, 0.3f, 1.0f };
    for (int wi = 0; wi < 4; wi++) {
        float w = widths[wi];
        int partial_near = 0, visible_far = 0, rising = 0, partial_density = 0, kept = 0;
        for (int h = 0; h < 256; h++) {
            float hash = ((float)h + 0.5f) / 256.0f;
            if (lod_foliage_alpha(1.0f, 1.0f, hash, w) != 1.0f) partial_near++;
            if (lod_foliage_alpha(1.0f, 0.0f, hash, w) != 0.0f) visible_far++;
            float prev = 1.0f;
            for (int k = 0; k <= 64; k++) {   // the camera backs away through the fade: alpha only falls
                float a = lod_foliage_alpha(1.0f, 1.0f - (float)k / 64.0f, hash, w);
                if (a > prev + 1e-6f) rising++;
                prev = a;
            }
            for (int d = 1; d < 10; d++) {   // density thinning: kept (opaque) or dropped
                float a = lod_foliage_alpha((float)d / 10.0f, 1.0f, hash, w);
                if (a != 0.0f && a != 1.0f) partial_density++;
                kept += a > 0;
            }
        }
        CHECK(partial_near == 0, "width %.3f: %d of 256 instances not opaque before the fade starts", (double)w, partial_near);
        CHECK(visible_far == 0, "width %.3f: %d instances left at the fade end", (double)w, visible_far);
        CHECK(rising == 0, "width %.3f: alpha rises %d times while the camera backs away", (double)w, rising);
        CHECK(partial_density == 0, "width %.3f: %d partial-alpha instances from density thinning", (double)w, partial_density);
        CHECK(abs(kept - 256 * 45 / 10) <= 9, "width %.3f: density 0.1 .. 0.9 keeps %d instance-steps (want ~%d)", (double)w, kept, 256 * 45 / 10);
    }
    // the per-instance fade is a short front: half way through the band about half are gone
    int half = 0;
    for (int h = 0; h < 256; h++) half += lod_foliage_alpha(1.0f, 0.5f, ((float)h + 0.5f) / 256.0f, t.fol_instance) > 0.5f;
    CHECK(half > 96 && half < 160, "fade 0.5: %d of 256 instances above alpha 0.5", half);
}

// Static discrete chain under a slow dolly: per-LOD coverage over distance from lod_pick_band.
static void test_static_dolly(void) {
    const float err[4] = { 0.0f, 0.004f, 0.02f, 0.1f };
    float k_at1 = 704.0f;   // px per metre at 1 m (1080p, 75 deg)
    float px = 2.0f / 2.0f, band = 0.25f, worst = 0, prev[4] = { 1, 0, 0, 0 };
    int switches = 0, last = 0;
    for (float d = 1.0f; d < 2000.0f; d += fmaxf(d, 1.0f) / 600.0f) {   // 1 m/s-ish at 10 m, a slow dolly in log space
        float cw;
        int l = lod_pick_band(err, 4, 0, k_at1 / d, px, band, &cw);
        float cov[4] = { 0 };
        cov[l] += 1 - cw;
        if (cw > 0) cov[l + 1] += cw;
        int top = cw >= 0.5f ? l + 1 : l;
        switches += top != last;
        last = top;
        for (int i = 0; i < 4; i++) worst = fmaxf(worst, fabsf(cov[i] - prev[i]));
        memcpy(prev, cov, sizeof(prev));
    }
    CHECK(switches == 3, "static chain: %d switches (want 3)", switches);
    CHECK(worst < MAX_COVER_STEP, "static chain dolly: worst coverage step %.2f %%", (double)(worst * 100));
    // band 0: the old cut
    float cw;
    lod_pick_band(err, 4, 0, k_at1 / 50.0f, px, 0, &cw);
    CHECK(cw == 0, "band 0 never fades");
}

static void test_streaming(void) {
    CHECK(lod_resident_pick(1, 0x2, 4) == 1, "resident wanted LOD drawn");
    CHECK(lod_resident_pick(0, 0x2, 4) == 1, "finer not resident: the coarser is held");
    CHECK(lod_resident_pick(1, 0x1, 4) == 0, "nothing coarser resident: the finer");
    CHECK(lod_resident_pick(2, 0, 4) == -1, "nothing resident");
    // never lower after higher is resident: once LOD0 is in, wanting 0 draws 0
    u32 ready = 0x8;
    int drawn_prev = 3;
    for (int frame = 0; frame < 10; frame++) {
        if (frame == 3) ready |= 0x4;
        if (frame == 6) ready |= 0x1;
        int d = lod_resident_pick(0, ready, 4);
        CHECK(d <= drawn_prev, "frame %d: drew LOD %d after LOD %d", frame, d, drawn_prev);
        drawn_prev = d;
    }
    CHECK(drawn_prev == 0, "the wanted LOD once resident");
}

// Terrain: a 32 m chunk grid around random cameras; chunk levels by nearest point, per-vertex morph.
static float chunk_near(float cx, float cz, float x0, float z0) {
    float dx = fmaxf(fmaxf(x0 - cx, 0), cx - (x0 + 32)), dz = fmaxf(fmaxf(z0 - cz, 0), cz - (z0 + 32));
    return sqrtf(dx * dx + dz * dz);
}

static void test_terrain(void) {
    const float diag = 32.0f * 1.4142136f;
    int bad_edge = 0, bad_jump = 0, bad_level = 0, edges = 0;
    u32 seed = 7;
    const float lods[4] = { 32, 42, 105, 300 };   // chunks.c clamps the LOD distance to >= 32 m
    for (int li = 0; li < 4; li++) {
        float lod_d = lods[li];
        for (int trial = 0; trial < 200; trial++) {
            seed = seed * 1664525u + 1013904223u;
            float cx = (float)(seed >> 8 & 1023) * 0.37f, cz = (float)(seed >> 18 & 1023) * 0.41f;
            for (int gz = -20; gz < 20; gz++)
                for (int gx = -20; gx < 19; gx++) {
                    float ax = (float)gx * 32, az = (float)gz * 32, bx = ax + 32;
                    int la = lod_terrain_level(chunk_near(cx, cz, ax, az), lod_d), lb = lod_terrain_level(chunk_near(cx, cz, bx, az), lod_d);
                    if (abs(la - lb) > 1) bad_level++;
                    if (la == lb) continue;
                    edges++;
                    int fine = la < lb ? la : lb, coarse = fine + 1;
                    // shared edge x = bx, z in [az, az + 32]
                    for (int k = 0; k <= 32; k++) {
                        float x = bx, z = az + (float)k, d = sqrtf((x - cx) * (x - cx) + (z - cz) * (z - cz));
                        float mf = lod_terrain_morph(d, fine, lod_d, 0.5f, diag), mc = lod_terrain_morph(d, coarse, lod_d, 0.5f, diag);
                        if (mf < 0.9999f || mc > 1e-4f) bad_edge++;
                    }
                }
            // switch pop: a chunk whose level changes between two nearby camera positions keeps its vertices
            for (int gz = -8; gz < 8; gz++)
                for (int gx = -8; gx < 8; gx++) {
                    float x0 = (float)gx * 32, z0 = (float)gz * 32;
                    float n1 = chunk_near(cx, cz, x0, z0), n2 = chunk_near(cx + 0.25f, cz, x0, z0);
                    int l1 = lod_terrain_level(n1, lod_d), l2 = lod_terrain_level(n2, lod_d);
                    if (l1 == l2) continue;
                    int fine = l1 < l2 ? l1 : l2;
                    float ccx = l1 < l2 ? cx + 0.25f : cx;   // the camera where the chunk is coarse
                    float ncoarse = chunk_near(ccx, cz, x0, z0);
                    (void)ncoarse;
                    for (int k = 0; k <= 32; k += 4)
                        for (int j = 0; j <= 32; j += 4) {
                            float vx = x0 + (float)k, vz = z0 + (float)j;
                            float d = sqrtf((vx - ccx) * (vx - ccx) + (vz - cz) * (vz - cz));
                            float mf = lod_terrain_morph(d, fine, lod_d, 0.5f, diag), mc = lod_terrain_morph(d, fine + 1, lod_d, 0.5f, diag);
                            if (mf < 0.999f || mc > 0.02f) { if (!bad_jump) printf("  jump: lod_d %.0f fine %d d %.2f mf %.4f mc %.4f\n", (double)lod_d, fine, (double)d, (double)mf, (double)mc); bad_jump++; }   // 0.25 m of camera motion
                        }
                }
        }
    }
    CHECK(bad_level == 0, "terrain: %d neighbours two levels apart", bad_level);
    CHECK(edges > 100 && bad_edge == 0, "terrain: %d shared-edge vertices disagree (of %d LOD edges)", bad_edge, edges);
    CHECK(bad_jump == 0, "terrain: %d vertices move at a chunk LOD switch", bad_jump);
}

int main(void) {
    cvar_system_init();
    test_table();
    test_tiers();
    test_char_mapping();
    test_char_dolly(1.0f, 0.35f);
    test_char_dolly(4.0f, 0.35f);
    test_reversal();
    test_masks();
    test_split_sweep();
    test_static_dolly();
    test_foliage_alpha();
    test_streaming();
    test_terrain();
    printf("test_lod: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
