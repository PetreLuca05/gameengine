// Post-processing Looks (docs/LOOKS.md, src/render/post/look{,s}.c):
//   registry   exactly five looks, unique ids / names, one-line descriptions, look_find by id / name /
//              index, nothing else resolves (no "custom")
//   release    built twice (ctest looks / looks_release): DG_LOOK_TUNE=0 compiles look_tune out and
//              look_get is the constant table
//   identity   Realism (modulators off) on a grey ramp = the plain AgX tonemapper, neutral stays neutral
//   pubg       on a reference frame: lower saturation and lifted (milky) blacks vs Realism
//   cinematic  teal shadows / orange highlights (chromaticity vs Realism)
//   vibrant    more saturated foliage / sky; gritty keeps reds / oranges, drains greens / blues
//   lut        the baked 32^3 LUT (CPU trilinear, the GPU sampler's math) = the analytic stack:
//              grey ramp (< 5 / 255), natural colours (p50 < 0.005, p99 < 0.03 display-encoded)
//   fields     look_apply_post / look_apply_mod touch only the look's PostSettings fields
//   fidelity   Off drops every finish effect, Low / Medium caps, the user's strength 0 = off
//   modulate   Realism is faithful to the sky (no modulation); Cinematic golden hour, PUBG overcast,
//              Gritty storm
//   format     (dev) look_format prints a C initializer that names every field
#include "render/post/look.h"
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

static float enc1(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(fmaxf(c, 0.0f), 1.0f / 2.4f) - 0.055f; }
static vec3 enc(vec3 c) { return v3(enc1(c.x), enc1(c.y), enc1(c.z)); }
static float diff(vec3 a, vec3 b) { return fmaxf(fabsf(a.x - b.x), fmaxf(fabsf(a.y - b.y), fabsf(a.z - b.z))); }
static float lum(vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
static vec3 grey(float ev) { float g = 0.18f * exp2f(ev); return v3(g, g, g); }
// Absolute chroma of a display-encoded colour (HSV saturation reads ~1 whenever a channel nears 0).
static float chroma(vec3 o) { return fmaxf(o.x, fmaxf(o.y, o.z)) - fminf(o.x, fminf(o.y, o.z)); }
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }

// ---------------------------------------------------------------- registry

static void test_registry(void) {
    CHECK(LOOK_COUNT == 5, "LOOK_COUNT %d", LOOK_COUNT);
    for (int i = 0; i < LOOK_COUNT; i++) {
        const LookParams *p = look_preset(i);
        CHECK(p && p->id && p->name && p->desc, "look %d incomplete", i);
        CHECK(look_get(i) != NULL, "look_get %d", i);
        CHECK(strlen(p->desc) > 20 && strlen(p->desc) < 120 && !strchr(p->desc, '\n'), "look %s: description not one line (%zu)", p->id, strlen(p->desc));
        CHECK(look_find(p->id) == i && look_find(p->name) == i, "look_find %s", p->id);
        char idx[4];
        snprintf(idx, sizeof(idx), "%d", i);
        CHECK(look_find(idx) == i, "look_find %s", idx);
        for (int k = 0; k < i; k++) CHECK(strcmp(p->id, look_preset(k)->id) && strcmp(p->name, look_preset(k)->name), "duplicate look %s", p->id);
    }
    static const char *const names[] = { "realism", "pubg", "cinematic", "vibrant", "gritty" };
    for (int i = 0; i < 5; i++) CHECK(look_find(names[i]) == i, "look %s at %d", names[i], i);
    CHECK(look_find("PUBG-style") == LOOK_PUBG, "display name");
    CHECK(look_find("REALISM") == LOOK_REALISM, "case-insensitive");
    CHECK(look_find("custom") < 0 && look_find("5") < 0 && look_find("") < 0 && look_find(NULL) < 0 && look_preset(5) == NULL, "only five looks");
}

static void test_release(void) {
#if DG_LOOK_TUNE
    CHECK(look_tune_compiled(), "dev build: look_tune compiled in");
    LookParams *e = look_tune_edit(LOOK_GRITTY);
    e->contrast = 2.0f;
    CHECK(look_get(LOOK_GRITTY)->contrast == 2.0f && look_preset(LOOK_GRITTY)->contrast != 2.0f, "the override renders, the constants stay");
    CHECK(look_tune_dirty(LOOK_GRITTY), "dirty");
    look_tune_reset(-1);
    CHECK(look_get(LOOK_GRITTY) == look_preset(LOOK_GRITTY), "reset");
#else
    CHECK(!look_tune_compiled(), "release build: look_tune compiled out");
    for (int i = 0; i < LOOK_COUNT; i++) CHECK(look_get(i) == look_preset(i), "release: look_get is the constant table");
#endif
}

// ---------------------------------------------------------------- grading properties

static void test_identity(void) {
    const LookParams *r = look_preset(LOOK_REALISM);
    float worst = 0, worst_neutral = 0;
    for (float ev = -8; ev <= 6.01f; ev += 0.25f) {
        vec3 a = enc(look_eval(r, grey(ev))), b = enc(look_tonemap(LOOK_TM_AGX, 0, 0, grey(ev)));
        worst = fmaxf(worst, diff(a, b));
        worst_neutral = fmaxf(worst_neutral, fmaxf(fabsf(a.x - a.y), fabsf(a.y - a.z)));
    }
    printf("identity: Realism vs AgX on the grey ramp max %.4f, neutral max %.5f\n", worst, worst_neutral);
    CHECK(worst < 0.01f, "Realism grey ramp off the AgX tonemapper by %.4f", worst);
    CHECK(worst_neutral < 0.002f, "Realism tints grey by %.4f", worst_neutral);
}

// A small reference frame: sky, foliage, terrain, skin, deep shadow, highlights (scene-linear, exposed).
#define REF_N 512
static vec3 g_ref[REF_N];
static void make_reference(void) {
    srand(1234);
    for (int i = 0; i < REF_N; i++) {
        float u = rand() / (float)RAND_MAX, v = rand() / (float)RAND_MAX;
        vec3 base;
        float ev;
        switch (i % 8) {
        case 0: base = v3(0.35f, 0.55f, 0.95f); ev = 1.0f + u; break;          // sky
        case 1: base = v3(0.18f, 0.32f, 0.08f); ev = -1.5f + 2.0f * u; break;   // grass
        case 2: base = v3(0.10f, 0.22f, 0.06f); ev = -3.0f + 2.5f * u; break;   // tree canopy
        case 3: base = v3(0.30f, 0.22f, 0.14f); ev = -1.0f + 1.5f * u; break;   // dirt / road
        case 4: base = v3(0.60f, 0.42f, 0.32f); ev = -0.5f + u; break;         // skin
        case 5: base = v3(0.5f, 0.5f, 0.5f); ev = -7.0f + 2.0f * u; break;      // deep shadow
        case 6: base = v3(0.5f, 0.5f, 0.52f); ev = 2.0f + 2.5f * u; break;      // highlights / clouds
        default: base = v3(0.4f + 0.2f * v, 0.4f, 0.38f); ev = -2.0f + 3.0f * u; break;   // concrete
        }
        float k = 0.18f * exp2f(ev) / fmaxf(lum(base), 1e-4f);
        g_ref[i] = v3_scale(base, k);
    }
}

typedef struct FrameStats { float sat, black, black_g, mean_l; } FrameStats;
static FrameStats frame_stats(const LookParams *p) {
    static float l[REF_N];
    FrameStats st = { 0 };
    int n_sat = 0;
    for (int i = 0; i < REF_N; i++) {
        vec3 o = enc(look_eval(p, g_ref[i]));
        l[i] = lum(o);
        st.mean_l += l[i];
        if (i % 8 != 5) { st.sat += look_saturation(o); n_sat++; }
    }
    qsort(l, REF_N, sizeof(float), cmpf);
    st.sat /= (float)n_sat;
    st.mean_l /= REF_N;
    st.black = l[REF_N / 100];
    st.black_g = enc1(look_eval(p, v3(0, 0, 0)).y);
    return st;
}

static void test_presets(void) {
    make_reference();
    FrameStats st[LOOK_COUNT];
    for (int i = 0; i < LOOK_COUNT; i++) {
        st[i] = frame_stats(look_preset(i));
        printf("frame: %-10s mean sat %.3f  1%% black %.3f  black %.3f  mean luma %.3f\n", look_id_name(i), st[i].sat, st[i].black, st[i].black_g, st[i].mean_l);
    }
    const FrameStats *r = &st[LOOK_REALISM];
    CHECK(st[LOOK_PUBG].sat < r->sat * 0.92f, "PUBG-style not desaturated (%.3f vs %.3f)", st[LOOK_PUBG].sat, r->sat);
    CHECK(st[LOOK_PUBG].black > r->black + 0.04f, "PUBG-style blacks not lifted (%.3f vs %.3f)", st[LOOK_PUBG].black, r->black);
    CHECK(st[LOOK_VIBRANT].sat > r->sat * 1.1f, "Vibrant not more saturated (%.3f vs %.3f)", st[LOOK_VIBRANT].sat, r->sat);
    CHECK(st[LOOK_VIBRANT].black_g < 0.01f, "Vibrant blacks not clean (%.3f)", st[LOOK_VIBRANT].black_g);
    CHECK(st[LOOK_GRITTY].sat < r->sat * 0.75f, "Gritty not desaturated (%.3f vs %.3f)", st[LOOK_GRITTY].sat, r->sat);
    CHECK(st[LOOK_GRITTY].black <= r->black + 0.005f, "Gritty shadows not dark (%.3f vs %.3f)", st[LOOK_GRITTY].black, r->black);
    CHECK(st[LOOK_CINEMATIC].black_g > 0.02f && st[LOOK_CINEMATIC].black_g < 0.09f, "Cinematic black not crushed-but-not-black (%.3f)", st[LOOK_CINEMATIC].black_g);
    // Mid grey stays in a sane band for every look (no look is a different exposure).
    for (int i = 0; i < LOOK_COUNT; i++) {
        float m = lum(enc(look_eval(look_preset(i), grey(0))));
        CHECK(m > 0.3f && m < 0.6f, "%s: mid grey at %.3f", look_id_name(i), m);
    }
    // Brightness intent on the frame: PUBG-style washed, not darker than Realism (its shadows lifted
    // too); Vibrant brighter.
    CHECK(fabsf(st[LOOK_PUBG].mean_l - r->mean_l) < 0.03f, "PUBG-style brightness %.3f vs Realism %.3f", st[LOOK_PUBG].mean_l, r->mean_l);
    {
        float pg = lum(enc(look_eval(look_preset(LOOK_PUBG), grey(-2.0f)))), rg = lum(enc(look_eval(look_preset(LOOK_REALISM), grey(-2.0f))));
        CHECK(pg > rg * 0.85f, "PUBG-style shadows (-2 EV) %.3f vs Realism %.3f", pg, rg);
    }
    CHECK(st[LOOK_VIBRANT].mean_l > r->mean_l, "Vibrant not brighter (%.3f vs %.3f)", st[LOOK_VIBRANT].mean_l, r->mean_l);

    // Cinematic split: shadows towards teal (b, g over r), highlights towards orange (r over b),
    // as chromaticity relative to Realism.
    const LookParams *c = look_preset(LOOK_CINEMATIC), *re = look_preset(LOOK_REALISM);
    vec3 sh = look_eval(c, grey(-3.0f)), hi = look_eval(c, grey(1.5f));
    vec3 sh_r = look_eval(re, grey(-3.0f)), hi_r = look_eval(re, grey(1.5f));
    float rs = sh.x / (sh.x + sh.y + sh.z), bs = sh.z / (sh.x + sh.y + sh.z), rh = hi.x / (hi.x + hi.y + hi.z), bh = hi.z / (hi.x + hi.y + hi.z);
    float rs0 = sh_r.x / (sh_r.x + sh_r.y + sh_r.z), rh0 = hi_r.x / (hi_r.x + hi_r.y + hi_r.z);
    printf("cinematic: shadow rgb chromaticity r %.3f b %.3f (realism r %.3f), highlight r %.3f b %.3f (realism r %.3f), shadow hue %.0f highlight hue %.0f\n", rs,
           bs, rs0, rh, bh, rh0, look_hue_deg(sh), look_hue_deg(hi));
    CHECK(sh.z > sh.x && sh.y > sh.x && rs < rs0 - 0.01f, "Cinematic shadows not teal");
    CHECK(hi.x > hi.z && rh > rh0 + 0.01f, "Cinematic highlights not orange");
    float hs = look_hue_deg(sh), hh = look_hue_deg(hi);
    CHECK(hs > 160 && hs < 220, "Cinematic shadow hue %.0f (teal ~190)", hs);
    CHECK(hh > 15 && hh < 60, "Cinematic highlight hue %.0f (orange ~30)", hh);

    // Gritty keeps fire / blood, drains the rest.
    const LookParams *g = look_preset(LOOK_GRITTY);
    vec3 red = v3(0.5f, 0.05f, 0.03f), orange = v3(0.6f, 0.25f, 0.03f), green = v3(0.08f, 0.3f, 0.06f), blue = v3(0.05f, 0.12f, 0.5f);
    float s_red = chroma(enc(look_eval(g, red))) / chroma(enc(look_eval(re, red)));
    float s_or = chroma(enc(look_eval(g, orange))) / chroma(enc(look_eval(re, orange)));
    float s_gr = chroma(enc(look_eval(g, green))) / chroma(enc(look_eval(re, green)));
    float s_bl = chroma(enc(look_eval(g, blue))) / chroma(enc(look_eval(re, blue)));
    printf("gritty: chroma vs realism red %.2f orange %.2f green %.2f blue %.2f\n", s_red, s_or, s_gr, s_bl);
    CHECK(s_red > 0.85f && s_or > 0.8f, "Gritty drains reds / oranges (%.2f %.2f)", s_red, s_or);
    CHECK(s_gr < 0.75f && s_bl < 0.75f, "Gritty keeps greens / blues (%.2f %.2f)", s_gr, s_bl);
    // Vibrant: vivid greens and sky blues.
    const LookParams *v = look_preset(LOOK_VIBRANT);
    float v_gr = chroma(enc(look_eval(v, green))) / chroma(enc(look_eval(re, green)));
    float v_sky = chroma(enc(look_eval(v, v3(0.12f, 0.2f, 0.4f)))) / chroma(enc(look_eval(re, v3(0.12f, 0.2f, 0.4f))));
    CHECK(v_gr > 1.05f && v_sky > 1.05f, "Vibrant greens %.2f sky %.2f", v_gr, v_sky);
}

// ---------------------------------------------------------------- LUT

static void test_lut(void) {
    float *lut = (float *)malloc(sizeof(float) * 3 * LOOK_LUT_N * LOOK_LUT_N * LOOK_LUT_N);
    enum { N = 20000 };
    static float err[N];
    for (int id = 0; id < LOOK_COUNT; id++) {
        const LookParams *p = look_preset(id);
        look_bake_lut(p, lut);
        float ramp = 0;
        for (float ev = -10; ev <= 6.5f; ev += 0.1f) ramp = fmaxf(ramp, diff(enc(look_eval(p, grey(ev))), enc(look_lut_sample(lut, grey(ev)))));
        srand(7 + id);
        for (int k = 0; k < N; k++) {
            float y = 0.18f * exp2f(-8.0f + 13.0f * rand() / (float)RAND_MAX);
            vec3 c = v3(y * (0.2f + rand() / (float)RAND_MAX), y * (0.2f + rand() / (float)RAND_MAX), y * (0.2f + rand() / (float)RAND_MAX));
            err[k] = diff(enc(look_eval(p, c)), enc(look_lut_sample(lut, c)));
        }
        qsort(err, N, sizeof(float), cmpf);
        printf("lut: %-10s grey ramp max %.4f  colours p50 %.4f p99 %.4f max %.4f (display-encoded)\n", p->id, ramp, err[N / 2], err[N * 99 / 100], err[N - 1]);
        // 0.02 = 5 / 255: Gritty's steep mids (contrast, ACES, bleach overlay) are the worst case.
        CHECK(ramp < 0.02f, "%s: LUT grey ramp off by %.4f", p->id, ramp);
        CHECK(err[N / 2] < 0.005f, "%s: LUT median error %.4f", p->id, err[N / 2]);
        CHECK(err[N * 99 / 100] < 0.03f, "%s: LUT p99 error %.4f", p->id, err[N * 99 / 100]);
    }
    // The shaper round trip and the clamps.
    vec3 s = look_lut_coord(grey(0));
    CHECK(fabsf(s.x - (0.0f - LOOK_EV_MIN) / (LOOK_EV_MAX - LOOK_EV_MIN)) < 1e-5f, "shaper mid grey");
    s = look_lut_coord(v3(0, 1e9f, -1));
    CHECK(s.x == 0.0f && s.y == 1.0f && s.z == 0.0f, "shaper clamps");
    free(lut);
}

// ---------------------------------------------------------------- PostSettings fields

static void fill_random(PostSettings *s, unsigned seed) {
    unsigned char *b = (unsigned char *)s;
    srand(seed);
    for (size_t i = 0; i < sizeof(*s); i++) b[i] = (unsigned char)(rand() & 0x3f);
    // keep the floats sane (no NaN from random bytes): the fields the look reads / scales
    s->saturation = 0.9f;
    s->contrast = 1.1f;
    s->wb_temp = 0.1f;
    s->wb_tint = -0.05f;
    s->exposure_comp = 0.25f;
    s->bloom_intensity = 0.04f;
    s->grain = 0.02f;
    s->vignette = 0.2f;
}

static void test_fields(void) {
    LookUser u = { 1, 1, 1, 1, 1 };
    for (int id = 0; id < LOOK_COUNT; id++)
        for (int lvl = 0; lvl < 5; lvl++) {
            PostSettings a, b;
            fill_random(&a, 100u + (unsigned)id * 7u + (unsigned)lvl);
            b = a;
            look_apply_post(&b, id, look_preset(id), &u, lvl);
            // Put the look's own fields back: everything else must be byte-identical.
            PostSettings c = b;
            c.look_lut = a.look_lut;
            c.bloom_intensity = a.bloom_intensity;
            c.bloom_threshold = a.bloom_threshold;
            c.bloom_tint = a.bloom_tint;
            c.vignette = a.vignette;
            c.grain = a.grain;
            c.chromatic = a.chromatic;
            c.sharpen = a.sharpen;
            c.halation = a.halation;
            c.letterbox = a.letterbox;
            c.fb_damage_color = a.fb_damage_color;
            c.fb_lowhealth_color = a.fb_lowhealth_color;
            CHECK(!memcmp(&a, &c, sizeof(a)), "look_apply_post %s / level %d touched a non-look field", look_id_name(id), lvl);
            CHECK(b.look_lut == id, "look_lut");
            // Fidelity / quality fields specifically (what Engine Fidelity and the era own).
            CHECK(b.aa == a.aa && b.ao == a.ao && b.ssr == a.ssr && b.volumetrics == a.volumetrics && b.bloom == a.bloom && b.render_scale == a.render_scale &&
                      b.tonemapper == a.tonemapper && b.direct_output == a.direct_output,
                  "quality field changed");
            // Modulators.
            LookEnv e = { 4.0f, 0.6f, 0.9f, 0.9f, 1.0f, 14.0f };
            LookMod m;
            look_modulate(look_preset(id), &e, &m);
            PostSettings d = a, f;
            look_apply_mod(&d, &m);
            f = d;
            f.wb_temp = a.wb_temp;
            f.wb_tint = a.wb_tint;
            f.exposure_comp = a.exposure_comp;
            f.saturation = a.saturation;
            f.contrast = a.contrast;
            f.bloom_intensity = a.bloom_intensity;
            f.grain = a.grain;
            f.vignette = a.vignette;
            CHECK(!memcmp(&a, &f, sizeof(a)), "look_apply_mod %s touched a non-grade field", look_id_name(id));
        }
}

static void test_fidelity(void) {
    LookUser u = { 1, 1, 1, 1, 1 };
    for (int id = 0; id < LOOK_COUNT; id++) {
        const LookParams *p = look_preset(id);
        LookFinish f;
        look_finish(p, &u, 0, &f);
        CHECK(f.bloom == 0 && f.vignette == 0 && f.grain == 0 && f.chromatic == 0 && f.sharpen == 0 && f.halation == 0 && !f.letterbox, "%s: Off keeps a finish effect", p->id);
        for (int lvl = 1; lvl <= 2; lvl++) {
            look_finish(p, &u, lvl, &f);
            const LookCaps *k = look_caps(lvl);
            CHECK(f.bloom <= k->bloom && f.grain <= k->grain && f.chromatic <= k->chromatic && f.halation <= k->halation, "%s: level %d caps", p->id, lvl);
            CHECK(f.chromatic == 0, "%s: chromatic aberration at level %d", p->id, lvl);
        }
        look_finish(p, &u, 4, &f);
        CHECK(fabsf(f.bloom - p->bloom) < 1e-6f && fabsf(f.grain - p->grain) < 1e-6f && fabsf(f.vignette - p->vignette) < 1e-6f && f.letterbox == p->letterbox,
              "%s: Modern changes the look's finish", p->id);
        LookUser none = { 0, 0, 0, 0, 0 };
        look_finish(p, &none, 4, &f);
        CHECK(f.bloom == 0 && f.vignette == 0 && f.grain == 0 && f.chromatic == 0 && f.sharpen == 0 && f.halation == 0, "%s: the user's 0 strength", p->id);
    }
    const LookParams *r = look_preset(LOOK_REALISM);
    CHECK(r->vignette == 0 && r->grain == 0 && r->chromatic == 0 && !r->letterbox && r->sharpen > 0, "Realism: no vignette / grain / CA, mild sharpening");
    CHECK(look_preset(LOOK_CINEMATIC)->letterbox && look_preset(LOOK_CINEMATIC)->halation > 0, "Cinematic: letterbox + halation");
    CHECK(look_preset(LOOK_GRITTY)->grain > look_preset(LOOK_CINEMATIC)->grain && look_preset(LOOK_GRITTY)->vignette >= 0.4f, "Gritty: heavy grain, strong vignette");
}

static void test_modulate(void) {
    LookEnv noon = { 60.0f, 1.0f, 0.1f, 0.0f, 0.0f, 3.0f }, golden = { 4.0f, 0.7f, 0.1f, 0.0f, 0.0f, 3.0f };
    LookEnv overcast = { 40.0f, 1.0f, 1.0f, 0.0f, 0.0f, 4.0f }, storm = { 30.0f, 0.8f, 1.0f, 1.0f, 1.0f, 15.0f }, night = { -30.0f, 0.0f, 0.1f, 0, 0, 2 };
    LookEnv all[] = { noon, golden, overcast, storm, night };
    LookMod m;
    for (int i = 0; i < 5; i++) {
        look_modulate(look_preset(LOOK_REALISM), &all[i], &m);
        CHECK(m.temp == 0 && m.tint == 0 && m.exposure == 0 && m.sat_mul == 1 && m.contrast_mul == 1 && m.bloom_mul == 1 && m.grain_mul == 1 && m.vignette_add == 0,
              "Realism modulates (env %d)", i);
    }
    for (int id = 0; id < LOOK_COUNT; id++) {
        look_modulate(look_preset(id), &noon, &m);
        CHECK(fabsf(m.temp) < 0.01f && fabsf(m.sat_mul - 1) < 0.02f && fabsf(m.contrast_mul - 1) < 0.02f, "%s: a clear noon is not neutral", look_id_name(id));
    }
    LookMod g, n;
    look_modulate(look_preset(LOOK_CINEMATIC), &golden, &g);
    look_modulate(look_preset(LOOK_CINEMATIC), &noon, &n);
    CHECK(g.temp > 0.08f && g.bloom_mul > 1.3f && g.temp > n.temp, "Cinematic golden hour (temp %.3f bloom %.2f)", g.temp, g.bloom_mul);
    LookMod pg, pr;
    look_modulate(look_preset(LOOK_PUBG), &overcast, &pg);
    look_modulate(look_preset(LOOK_REALISM), &overcast, &pr);
    CHECK(pg.sat_mul < 0.9f && pg.contrast_mul < 0.95f, "PUBG-style overcast not flat / grey (sat %.2f contrast %.2f)", pg.sat_mul, pg.contrast_mul);
    LookMod gs;
    look_modulate(look_preset(LOOK_GRITTY), &storm, &gs);
    CHECK(gs.sat_mul < 0.85f && gs.contrast_mul > 1.08f && gs.grain_mul > 1.4f && gs.vignette_add > 0.05f, "Gritty storm (sat %.2f contrast %.2f grain %.2f)", gs.sat_mul,
          gs.contrast_mul, gs.grain_mul);
}

static void test_format(void) {
#if DG_LOOK_TUNE
    static char buf[16384];
    for (int id = 0; id < LOOK_COUNT; id++) {
        int n = look_format(look_preset(id), buf, sizeof(buf));
        CHECK(n > 0 && n < (int)sizeof(buf), "look_format size %d", n);
        int count;
        const LookField *f = look_fields(&count);
        for (int i = 0; i < count; i++) {
            char key[48];
            const char *dot = strchr(f[i].name, '.');
            snprintf(key, sizeof(key), ".%.*s = ", dot ? (int)(dot - f[i].name) : (int)strlen(f[i].name), f[i].name);
            CHECK(strstr(buf, key) != NULL, "look_format %s misses %s", look_id_name(id), key);
        }
        CHECK(strstr(buf, look_preset(id)->id) != NULL, "id");
    }
    look_format(look_preset(LOOK_CINEMATIC), buf, sizeof(buf));
    CHECK(strstr(buf, ".tonemapper = LOOK_TM_FILMIC,") && strstr(buf, ".lut = LOOK_LUT_PRINT,") && strstr(buf, ".letterbox = 1,"), "enum / int fields");
#endif
}

int main(void) {
    test_registry();
    test_release();
    test_identity();
    test_presets();
    test_lut();
    test_fields();
    test_fidelity();
    test_modulate();
    test_format();
    printf("looks (%s): %d checks, %d failed\n", look_tune_compiled() ? "dev" : "release", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
