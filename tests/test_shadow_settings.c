// Unity-style shadow controls (docs/SHADOWS_UNITY_MODEL.md, src/render/shadow_settings.c):
//   knobs      defaults, names, describe -> shs_light_set round trip, sanitize ranges, bad input
//   tiers      shs_quality_for_tier agrees with assets/render/fidelity/*.json defaults
//   filters    Hard = one bilinear compare (edge <= 1 texel); Soft tent 3 / 5 / 7 = the exact tent
//              convolution of the texels, widths increasing; Ultra = PCSS on the sun, tent 7 locals
//   strength   0.5 halves the darkening, 0 = lit
//   bias       the same number of texels at every resolution
//   cascades   split percentages of the distance, Hard Only caps Soft, Disable = none
//   renderers  Cast Shadows Off / On / Two Sided / Shadows Only, culling masks
//   atlas      buddy packing in priority order, no overlaps, groups all or nothing + downgrade
//   viewmodel  its tile follows the directional resolution and filter
#include "render/shadow_settings.h"
#include "render/shadow_csm.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

static bool near_(float a, float b, float eps) { return fabsf(a - b) <= eps; }

// ------------------------------------------------------------------ knobs

static void test_knobs(void) {
    LightShadowSettings s;
    shs_light_default(&s, SHADOW_LIGHT_POINT);
    CHECK(s.type == SHADOW_SOFT && s.resolution == SHADOW_RES_QUALITY && s.strength == 1.0f, "point default");
    CHECK(s.bias == 1.0f && s.normal_bias == 1.0f && s.near_plane > 0.0f && s.culling_mask == SHADOW_LAYERS_ALL, "point default bias / near / mask");
    shs_light_default(&s, SHADOW_LIGHT_DIRECTIONAL);
    CHECK(s.near_plane == 0.0f, "directional has no near plane");

    for (int t = 0; t < SHADOW_TYPE_COUNT; t++) CHECK(shs_type_from_name(shs_type_name((ShadowType)t)) == t, "type name %d", t);
    for (int c = 0; c < CAST_SHADOWS_COUNT; c++) CHECK(shs_cast_from_name(shs_cast_name((CastShadows)c)) == c, "cast name %d", c);
    for (int r = SHADOW_RES_QUALITY; r < SHADOW_RES_COUNT; r++) {
        int out = -99;
        CHECK(shs_res_from_name(shs_res_name(r), &out) && out == r, "res name %d", r);
    }
    CHECK(shs_type_from_name("medium") < 0 && shs_cast_from_name("maybe") < 0, "unknown names");

    // by hand, the way a level / the console edits a light
    char err[160];
    shs_light_default(&s, SHADOW_LIGHT_SPOT);
    CHECK(shs_light_set(&s, "type", "hard", err, sizeof(err)) && s.type == SHADOW_HARD, "set type hard");
    CHECK(shs_light_set(&s, "resolution", "very_high", err, sizeof(err)) && s.resolution == SHADOW_RES_VERY_HIGH, "set resolution");
    CHECK(shs_light_set(&s, "strength", "0.4", err, sizeof(err)) && near_(s.strength, 0.4f, 1e-6f), "set strength");
    CHECK(shs_light_set(&s, "bias", "0.5", err, sizeof(err)) && shs_light_set(&s, "normal_bias", "2", err, sizeof(err)), "set biases");
    CHECK(shs_light_set(&s, "near_plane", "0.5", err, sizeof(err)) && shs_light_set(&s, "distance", "40", err, sizeof(err)), "set near / distance");
    CHECK(shs_light_set(&s, "culling_mask", "0x05", err, sizeof(err)) && s.culling_mask == 5u, "set mask");
    CHECK(shs_light_set(&s, "strength", "7", err, sizeof(err)) && s.strength == 1.0f, "strength clamps");
    CHECK(!shs_light_set(&s, "type", "fuzzy", err, sizeof(err)) && s.type == SHADOW_HARD, "bad value rejected, light unchanged");
    CHECK(!shs_light_set(&s, "colour", "1", err, sizeof(err)), "unknown key rejected");
    CHECK(!shs_light_set(&s, "bias", "1x", err, sizeof(err)) && s.bias == 0.5f, "bad number rejected");

    // describe -> parse back
    char buf[256];
    shs_light_describe(&s, buf, sizeof(buf));
    LightShadowSettings r;
    shs_light_default(&r, SHADOW_LIGHT_POINT);
    char *tok = strtok(buf, " ");
    while (tok) {
        char *v = strtok(NULL, " ");
        CHECK(v && shs_light_set(&r, tok, v, err, sizeof(err)), "round trip %s: %s", tok, err);
        tok = strtok(NULL, " ");
    }
    CHECK(r.type == s.type && r.resolution == s.resolution && near_(r.strength, s.strength, 0.005f) && near_(r.bias, s.bias, 0.005f) && near_(r.normal_bias, s.normal_bias, 0.005f)
              && near_(r.near_plane, s.near_plane, 0.005f) && r.culling_mask == s.culling_mask && near_(r.distance, s.distance, 0.05f),
          "describe round trip");

    QualityShadowSettings q;
    shs_quality_default(&q);
    q.cascades = 3;
    q.split4[0] = 0.5f; q.split4[1] = 0.2f; q.split4[2] = 2.0f;
    q.border = 0.9f;
    shs_quality_sanitize(&q);
    CHECK(q.cascades == 4 && q.split4[0] < q.split4[1] && q.split4[1] < q.split4[2] && q.split4[2] < 1.0f && q.border == 0.5f, "quality sanitize");
}

// ------------------------------------------------------------------ tiers vs the fidelity JSON

static cJSON *load_json(const char *name) {
    static const char *dirs[] = { "assets/render/fidelity/", "../assets/render/fidelity/", "../../assets/render/fidelity/" };
    for (int i = 0; i < 3; i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s%s", dirs[i], name);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *s = malloc((size_t)n + 1);
        size_t got = fread(s, 1, (size_t)n, f);
        s[got] = 0;
        fclose(f);
        cJSON *j = cJSON_Parse(s);
        free(s);
        return j;
    }
    return NULL;
}

static double feat_default(const cJSON *j, const char *key) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "features"), key);
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(f, "default");
    return cJSON_IsNumber(d) ? d->valuedouble : -1.0;
}

static void test_tiers(void) {
    static const char *files[5] = { "off.json", "low.json", "medium.json", "high.json", "modern.json" };
    // the spec's table: mode, resolution, cascades, distance, soft quality
    static const int want[5][5] = { { 0, 0, 1, 50, 0 }, { 1, 0, 2, 50, 0 }, { 2, 1, 2, 100, 0 }, { 2, 2, 4, 200, 1 }, { 2, 3, 4, 300, 3 } };
    for (int l = 0; l < 5; l++) {
        QualityShadowSettings q;
        shs_quality_for_tier(&q, l);
        CHECK((int)q.mode == want[l][0] && (int)q.resolution == want[l][1] && q.cascades == want[l][2] && near_(q.distance, (float)want[l][3], 0.01f) && (int)q.soft_quality == want[l][4],
              "tier %d table", l);
        cJSON *j = load_json(files[l]);
        CHECK(j != NULL, "load %s", files[l]);
        if (!j) continue;
        CHECK((int)feat_default(j, "shadows") == (int)q.mode, "%s shadows", files[l]);
        CHECK((int)feat_default(j, "shadow_resolution") == (int)q.resolution, "%s shadow_resolution", files[l]);
        CHECK((int)feat_default(j, "shadow_cascades") == q.cascades, "%s shadow_cascades", files[l]);
        CHECK(near_((float)feat_default(j, "shadow_distance"), q.distance, 0.01f), "%s shadow_distance", files[l]);
        CHECK((int)feat_default(j, "soft_shadow_quality") == (int)q.soft_quality, "%s soft_shadow_quality", files[l]);
        cJSON_Delete(j);
    }
    QualityShadowSettings d, m;
    shs_quality_default(&d);
    shs_quality_for_tier(&m, 4);
    CHECK(!memcmp(&d, &m, sizeof(d)), "default = Modern");
}

// ------------------------------------------------------------------ filters (CPU model of lighting.glsl)

// A step edge in the map: texels < EDGE are occluded (compare 0), the rest lit (1).
#define EDGE 16
static float texel_cmp(int i) { return i < EDGE ? 0.0f : 1.0f; }
// Bilinear compare fetch at texel-space x (texel i centred at i + 0.5).
static float fetch_cmp(float x) {
    float t = x - 0.5f;
    int i = (int)floorf(t);
    float f = t - (float)i;
    return texel_cmp(i) * (1.0f - f) + texel_cmp(i + 1) * f;
}
static float hard_at(float x) { return fetch_cmp(x); }
static float tent_at(float x, float n) {
    float pos[4], w[4];
    int k = shs_tent_axis(x, n, pos, w);
    float s = 0.0f;
    for (int i = 0; i < k; i++) s += w[i] * fetch_cmp(pos[i]);
    return s;
}
// Reference: the tent of base n convolved with the box texels (numerical integration).
static float tent_ref(float x, float n) {
    double s = 0.0, ws = 0.0;
    int N = 4000;
    for (int i = 0; i < N; i++) {
        float u = -n * 0.5f + n * ((float)i + 0.5f) / (float)N;
        float w = 1.0f - fabsf(u) / (n * 0.5f);
        s += w * texel_cmp((int)floorf(x + u));
        ws += w;
    }
    return (float)(s / ws);
}
// Width (texels) over which the filtered term goes from 0.02 to 0.98.
static float edge_width(float (*fn)(float, float), float n) {
    float lo = 1e9f, hi = -1e9f;
    for (float x = EDGE - 8.0f; x <= EDGE + 8.0f; x += 0.01f) {
        float v = fn(x, n);
        if (v > 0.02f && x < lo) lo = x;
        if (v < 0.98f && x > hi) hi = x;
    }
    return hi - lo;
}
static float hard2(float x, float n) { (void)n; return hard_at(x); }

static void test_filters(void) {
    CHECK(shs_filter(SHADOW_NONE, SOFT_HIGH, SHADOW_LIGHT_DIRECTIONAL).mode == SHF_NONE, "none");
    CHECK(shs_filter(SHADOW_HARD, SOFT_ULTRA, SHADOW_LIGHT_DIRECTIONAL).mode == SHF_HARD, "hard ignores soft quality");
    CHECK(shs_filter(SHADOW_SOFT, SOFT_ULTRA, SHADOW_LIGHT_DIRECTIONAL).mode == SHF_PCSS, "ultra sun = PCSS");
    ShadowFilter lf = shs_filter(SHADOW_SOFT, SOFT_ULTRA, SHADOW_LIGHT_POINT);
    CHECK(lf.mode == SHF_TENT && lf.tent == 7.0f, "ultra local = tent 7");

    float hw = edge_width(hard2, 0.0f);
    CHECK(hw <= 1.0f + 0.02f, "hard edge %.3f texels (<= 1)", hw);

    float prev = hw;
    for (int q = SOFT_LOW; q <= SOFT_HIGH; q++) {
        ShadowFilter f = shs_filter(SHADOW_SOFT, (SoftQuality)q, SHADOW_LIGHT_SPOT);
        CHECK(f.mode == SHF_TENT && f.tent == 3.0f + 2.0f * (float)q, "soft %d tent %.0f", q, f.tent);
        CHECK(shs_tent_fetches(f.tent) == 2 + q, "tent %.0f: %d fetches per axis", f.tent, 2 + q);
        float maxerr = 0.0f;
        for (float x = EDGE - 6.0f; x <= EDGE + 6.0f; x += 0.037f) {
            float pos[4], w[4];
            int k = shs_tent_axis(x, f.tent, pos, w);
            float ws = 0.0f;
            for (int i = 0; i < k; i++) ws += w[i];
            CHECK(near_(ws, 1.0f, 1e-4f), "tent weights sum %.5f", ws);
            float e = fabsf(tent_at(x, f.tent) - tent_ref(x, f.tent));
            if (e > maxerr) maxerr = e;
        }
        CHECK(maxerr < 0.003f, "tent %.0f = exact convolution (err %.4f)", f.tent, maxerr);
        float w = edge_width(tent_at, f.tent);
        CHECK(w > prev + 1.0f, "tent %.0f wider (%.2f vs %.2f)", f.tent, w, prev);
        CHECK(w < f.tent + 1.0f, "tent %.0f width %.2f bounded", f.tent, w);
        prev = w;
    }
    // Ultra on the sun: the penumbra in texels follows the penumbra in metres (blocker distance)
    float k1 = csm_kernel_texels(0.02f, 0.01f, 0.5f, 16.0f), k2 = csm_kernel_texels(0.08f, 0.01f, 0.5f, 16.0f);
    CHECK(k2 > k1, "PCSS kernel grows with the penumbra (%.2f -> %.2f)", k1, k2);
}

// ------------------------------------------------------------------ strength / bias / cascades

static void test_strength_bias(void) {
    CHECK(near_(shs_apply_strength(0.0f, 0.5f), 0.5f, 1e-6f), "strength 0.5 halves");
    CHECK(near_(shs_apply_strength(0.4f, 0.5f), 0.7f, 1e-6f), "strength 0.5 on a partial shadow");
    CHECK(shs_apply_strength(0.0f, 0.0f) == 1.0f && shs_apply_strength(0.0f, 1.0f) == 0.0f, "strength ends");
    CHECK(shs_apply_strength(1.0f, 0.7f) == 1.0f, "lit stays lit");

    // the same texel count at 1024 and 4096 (texel = tile / texels)
    float tile = 40.0f;
    for (float b = 0.0f; b <= 2.0f; b += 0.5f) {
        float c1, n1, c4, n4;
        float t1 = tile / 1024.0f, t4 = tile / 4096.0f;
        shs_bias_m(t1, b, b, 1.0f, &c1, &n1);
        shs_bias_m(t4, b, b, 1.0f, &c4, &n4);
        CHECK(near_(c1 / t1, c4 / t4, 1e-5f) && near_(n1 / t1, n4 / t4, 1e-5f), "bias %.1f same texels", b);
    }
    float c, n;
    shs_bias_m(0.01f, 1.0f, 1.0f, 1.0f, &c, &n);
    CsmBias e = csm_bias(0.01f, 1.0f);
    CHECK(near_(c, e.const_m, 1e-6f) && near_(n, e.normal_m, 1e-6f), "bias 1 / 1 = the engine default (%.5f %.5f vs %.5f %.5f)", c, n, e.const_m, e.normal_m);
    shs_bias_m(0.01f, 1.0f, 1.0f, 3.0f, &c, &n);
    CHECK(near_(c, 3.0f * 0.0075f, 1e-6f), "low sun scales the bias");
}

static void test_cascades(void) {
    QualityShadowSettings q;
    shs_quality_default(&q);
    float s[4];
    CHECK(shs_cascade_splits(&q, s) == 4, "4 cascades");
    CHECK(near_(s[0], 0.067f * 300.0f, 0.01f) && near_(s[1], 60.0f, 0.01f) && near_(s[2], 0.467f * 300.0f, 0.01f) && s[3] == 300.0f, "4-split percentages");
    q.cascades = 2;
    q.distance = 100.0f;
    CHECK(shs_cascade_splits(&q, s) == 2 && near_(s[0], 33.333f, 0.01f) && s[1] == 100.0f, "2-split percentage");
    q.cascades = 1;
    CHECK(shs_cascade_splits(&q, s) == 1 && s[0] == 100.0f, "no cascades");

    CHECK(shs_effective_type(SHADOW_SOFT, SHADOWS_HARD_ONLY) == SHADOW_HARD, "Hard Only caps Soft");
    CHECK(shs_effective_type(SHADOW_HARD, SHADOWS_HARD_ONLY) == SHADOW_HARD, "Hard Only keeps Hard");
    CHECK(shs_effective_type(SHADOW_NONE, SHADOWS_HARD_AND_SOFT) == SHADOW_NONE, "None stays None");
    CHECK(shs_effective_type(SHADOW_SOFT, SHADOWS_DISABLE) == SHADOW_NONE, "Disable");
    CHECK(shs_effective_type(SHADOW_SOFT, SHADOWS_HARD_AND_SOFT) == SHADOW_SOFT, "Hard and Soft");

    LightShadowSettings l;
    shs_light_default(&l, SHADOW_LIGHT_SPOT);
    shs_quality_for_tier(&q, 2);
    CHECK(shs_light_texels(&l, &q, SHADOW_LIGHT_SPOT) == 512u && shs_light_texels(&l, &q, SHADOW_LIGHT_DIRECTIONAL) == 1024u, "use quality resolution");
    l.resolution = SHADOW_RES_VERY_HIGH;
    CHECK(shs_light_texels(&l, &q, SHADOW_LIGHT_SPOT) == 2048u && shs_res_texels(SHADOW_RES_VERY_HIGH, SHADOW_LIGHT_DIRECTIONAL) == 4096u, "own resolution");
}

static void test_renderers(void) {
    CHECK(!shs_casts(CAST_SHADOWS_OFF) && shs_visible(CAST_SHADOWS_OFF), "Off: visible, no shadow");
    CHECK(shs_casts(CAST_SHADOWS_ON) && shs_visible(CAST_SHADOWS_ON) && !shs_two_sided_caster(CAST_SHADOWS_ON, false), "On: back faces culled");
    CHECK(shs_two_sided_caster(CAST_SHADOWS_ON, true), "On + two-sided material");
    CHECK(shs_two_sided_caster(CAST_SHADOWS_TWO_SIDED, false), "Two Sided");
    CHECK(shs_casts(CAST_SHADOWS_ONLY) && !shs_visible(CAST_SHADOWS_ONLY), "Shadows Only");
    CHECK(!shs_two_sided_caster(CAST_SHADOWS_OFF, true), "Off never casts");
    // culling mask: layer bit
    u32 mask = 0x05u;
    CHECK(((mask >> 0) & 1u) && !((mask >> 1) & 1u) && ((mask >> 2) & 1u), "culling mask bits");
}

// ------------------------------------------------------------------ atlas

static bool overlap(const ShsAtlasRect *a, const ShsAtlasRect *b) {
    return a->x < b->x + b->size && b->x < a->x + a->size && a->y < b->y + b->size && b->y < a->y + a->size;
}

static void test_atlas(void) {
    ShsAtlasReq req[SHS_ATLAS_MAX];
    ShsAtlasRect out[SHS_ATLAS_MAX];
    // 4 spots at 2048 fill a 4096 atlas exactly
    for (int i = 0; i < 4; i++) req[i] = (ShsAtlasReq){ 2048, i };
    CHECK(shs_atlas_pack(4096, 256, req, 4, out) == 4, "4 x 2048");
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) CHECK(!overlap(&out[i], &out[j]), "no overlap %d %d", i, j);
    // a fifth does not fit at 2048 or smaller: dropped (the budget)
    req[4] = (ShsAtlasReq){ 2048, 4 };
    CHECK(shs_atlas_pack(4096, 256, req, 5, out) == 4 && out[4].size == 0, "budget: the lowest priority drops");

    // point lights: 6 faces as a group; the second point downgrades instead of half-placing
    int n = 0;
    req[n++] = (ShsAtlasReq){ 2048, 0 };
    req[n++] = (ShsAtlasReq){ 2048, 1 };
    req[n++] = (ShsAtlasReq){ 2048, 2 };
    for (int f = 0; f < 6; f++) req[n++] = (ShsAtlasReq){ 1024, 3 };
    int placed = shs_atlas_pack(4096, 256, req, n, out);
    CHECK(placed == n, "group placed after a downgrade (%d / %d)", placed, n);
    for (int f = 3; f < 9; f++) CHECK(out[f].size == 512u, "face %d downgraded to 512 (got %u)", f, out[f].size);
    for (int i = 0; i < n; i++) {
        CHECK(out[i].x + out[i].size <= 4096u && out[i].y + out[i].size <= 4096u, "inside");
        for (int j = i + 1; j < n; j++) CHECK(!out[i].size || !out[j].size || !overlap(&out[i], &out[j]), "no overlap %d %d", i, j);
    }
    // a group that cannot fit even at the minimum is dropped whole
    n = 0;
    for (int i = 0; i < 4; i++) req[n++] = (ShsAtlasReq){ 2048, i };
    for (int f = 0; f < 6; f++) req[n++] = (ShsAtlasReq){ 256, 9 };
    CHECK(shs_atlas_pack(4096, 256, req, n, out) == 4, "full atlas drops the cube");
    for (int f = 4; f < n; f++) CHECK(out[f].size == 0, "whole group dropped");
    // many small tiles in priority order: the first ones get placed
    n = 0;
    for (int i = 0; i < 100; i++) req[n++] = (ShsAtlasReq){ 512, i };
    placed = shs_atlas_pack(4096, 512, req, n, out);
    CHECK(placed == 64 && out[63].size == 512u && out[64].size == 0u, "64 x 512 in 4096 (%d)", placed);
}

// ------------------------------------------------------------------ viewmodel

static void test_viewmodel(void) {
    u32 prev = 0;
    for (int r = 0; r < SHADOW_RES_COUNT; r++) {
        u32 t = csm_vm_texels(shs_res_texels(r, SHADOW_LIGHT_DIRECTIONAL));
        CHECK(t >= 1024u && t <= 2048u && t >= prev, "viewmodel tile %u at sun res %d", t, r);
        prev = t;
    }
    // the rig uses the sun's type and filter
    for (int q = SOFT_LOW; q <= SOFT_ULTRA; q++) {
        ShadowFilter f = shs_filter(shs_effective_type(SHADOW_SOFT, SHADOWS_HARD_ONLY), (SoftQuality)q, SHADOW_LIGHT_DIRECTIONAL);
        CHECK(f.mode == SHF_HARD, "Hard Only rig hard");
    }
}

int main(void) {
    test_knobs();
    test_tiers();
    test_filters();
    test_strength_bias();
    test_cascades();
    test_renderers();
    test_atlas();
    test_viewmodel();
    printf("test_shadow_settings: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
