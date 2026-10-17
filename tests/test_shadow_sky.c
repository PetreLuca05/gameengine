// Sky-driven shadow tests (docs/SHADOWS.md, src/render/shadow_sky.c):
//   config     assets/render/shadows_sky.json parses and equals the C defaults; bad curves / cutoffs
//              are rejected and leave the config untouched
//   source     clear 0.265 deg; haze, thin cloud, fog and rain widen it, monotonically
//   penumbra   the PCSS kernel (CPU port of shaders/lit_body.glsl pcss_tile, same Vogel disc, the same
//              blocker search / filter radius in metres -> texels) on a rasterised pole: the 10-90 %
//              edge width at 1 / 5 / 20 m from the caster matches the uniform-disc source of radius
//              d x tan(r) within 15 % where that is >= 3 texels, and stays under 2 texels below
//   contrast   the shading (direct x shadow + sky / pi x AO, lit_body.glsl) gives lit / shade =
//              ssky_lit_shade_ratio; the presets order clear >> hazy > light cloud > overcast ~ 1
//   cutoff     a 2 min clear -> overcast transition: the cast shadow turns off once, exactly under
//              the cutoff, never chatters; the shaded reference luminance never moves more than
//              0.5 % per frame; overcast -> clear turns it back on above off x (1 + reenable)
//   moon       the sun owns the cascades while its disc is up; the moon takes them over with a
//              continuous fade, the configured hardness, and leaves them as the dawn sky brightens
//   clouds     the drift moves at the weather wind x wind_aloft
//   tiers      wide mode (cascade 0 weight) with hysteresis, PCF steps, the 2010 fixed hardness
//   TAA        a slowly softening edge, rotated kernel per frame + an exponential history: the
//              resolved frame-to-frame change stays small
#include "render/shadow_sky.h"
#include "core/engine.h"
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

static const float DT = 1.0f / 60.0f;
static float vmaxf3(vec3 v) { return fmaxf(v.x, fmaxf(v.y, v.z)); }

// ------------------------------------------------------------------ a stand-in sky

// Engine units (daynight.json sun_illuminance 3). Not the GPU sky: a grey atmosphere with the same
// trends (the GPU numbers are measured by tools/tests/shadow_ref.cfg).
typedef struct Cond { const char *name; float elev_deg, cover, density, fog, precip, mie; } Cond;

static void make_input(const ShadowSkyConfig *c, const Cond *k, ShadowSkyInput *in) {
    memset(in, 0, sizeof(*in));
    float mu = sinf(k->elev_deg * DG_DEG2RAD);
    float t_atm = mu > 0 ? expf(-0.12f * k->mie / fmaxf(mu, 0.05f)) : 0.0f;
    float E0 = 3.0f;
    in->sun_elev = k->elev_deg * DG_DEG2RAD;
    in->moon_elev = -0.5f;
    in->sun_light = v3_scale(v3(1.0f, 0.97f, 0.9f), E0 * t_atm * (mu > 0 ? 1.0f : 0.0f));
    float tau = ssky_veil_tau(c, k->cover, k->density);
    float tv = expf(-tau / fmaxf(mu, c->min_mu));
    // Sky: Rayleigh + aerosol in-scatter, plus what the cloud sheet scatters down (a thick deck
    // passes less).
    float sky = E0 * fmaxf(mu, 0.0f) * (0.09f * tv + 0.06f * (k->mie - 1.0f) * tv + 0.45f * (1.0f - tv) * expf(-0.12f * tau));
    in->sky_up = v3_scale(v3(0.75f, 0.85f, 1.0f), sky);
    in->cloud_cover = k->cover;
    in->cloud_density = k->density;
    in->fog = k->fog;
    in->precip = k->precip;
    in->mie_scale = k->mie;
    in->wind_speed = 4.0f;
    in->wind_dir_deg = 90.0f;
    in->softness = 2;
    in->moon_shadows = true;
}

static const Cond C_CLEAR = { "clear noon", 55, 0.0f, 0.5f, 0.002f, 0, 1.0f };
static const Cond C_HAZY = { "hazy", 55, 0.2f, 0.5f, 0.004f, 0, 2.0f };
static const Cond C_LIGHT = { "light cloud", 55, 0.72f, 0.5f, 0.004f, 0, 1.4f };
static const Cond C_OVER = { "overcast", 55, 1.0f, 0.9f, 0.004f, 0, 1.5f };

// ------------------------------------------------------------------ config

static void test_config(void) {
    printf("shadow_sky: config\n");
    ShadowSkyConfig d, j;
    ssky_config_default(&d);
    ssky_config_default(&j);
    char err[256] = "";
    bool ok = ssky_config_load(&j, SSKY_PATH, err, sizeof(err));
    CHECK(ok, "load %s: %s", SSKY_PATH, err);
    if (ok) {
        // The file is the source of truth; the defaults mirror it (a missing file behaves the same).
        CHECK(fabsf(d.sun_radius_deg - j.sun_radius_deg) < 1e-6f, "sun radius %g vs %g", d.sun_radius_deg, j.sun_radius_deg);
        CHECK(fabsf(d.cutoff_off - j.cutoff_off) < 1e-6f && fabsf(d.cutoff_on - j.cutoff_on) < 1e-6f, "cutoff");
        CHECK(fabsf(d.wide_deg - j.wide_deg) < 1e-6f, "wide_deg");
        CHECK(d.veil_tau.n == j.veil_tau.n, "veil curve keys %d vs %d", d.veil_tau.n, j.veil_tau.n);
        for (float x = 0; x <= 1.0f; x += 0.05f) CHECK(fabsf(ssky_curve(&d.veil_tau, x) - ssky_curve(&j.veil_tau, x)) < 1e-4f, "veil_tau(%g)", x);
        for (float x = 0; x <= 4.0f; x += 0.2f) CHECK(fabsf(ssky_curve(&d.aureole_haze, x) - ssky_curve(&j.aureole_haze, x)) < 1e-4f, "haze(%g)", x);
        for (float x = 0; x <= 1.0f; x += 0.1f) CHECK(fabsf(ssky_curve(&d.exposure_cover, x) - ssky_curve(&j.exposure_cover, x)) < 1e-4f, "exposure(%g)", x);
        CHECK(fabsf(d.pcss_max_texels[2] - j.pcss_max_texels[2]) < 1e-6f, "pcss max");
    }
    ShadowSkyConfig b = d;
    CHECK(!ssky_config_parse(&b, "{\"clouds\":{\"veil_tau\":[[0,0],[0,1]]}}", err, sizeof(err)), "non-increasing curve accepted");
    CHECK(!ssky_config_parse(&b, "{\"cutoff\":{\"off\":0.2,\"on\":0.1}}", err, sizeof(err)), "off > on accepted");
    CHECK(!ssky_config_parse(&b, "{nope", err, sizeof(err)), "bad JSON accepted");
    CHECK(b.cutoff_off == d.cutoff_off && b.veil_tau.n == d.veil_tau.n, "a rejected file changed the config");
    CHECK(ssky_config_parse(&b, "{\"cutoff\":{\"off\":0.05}}", err, sizeof(err)) && b.cutoff_off == 0.05f && b.cutoff_on == d.cutoff_on, "partial file");
}

static void test_source(const ShadowSkyConfig *c) {
    printf("shadow_sky: source radius\n");
    float clear = ssky_source_radius(c, c->sun_radius_deg, 1.0f, 0.0f, 0.0f, 0.0f);
    CHECK(fabsf(clear - 0.265f) < 1e-4f, "clear %g", clear);
    float hazy = ssky_source_radius(c, c->sun_radius_deg, 2.6f, ssky_veil_tau(c, 0.2f, 0.5f), 0.004f, 0.0f);
    float light = ssky_source_radius(c, c->sun_radius_deg, 1.4f, ssky_veil_tau(c, 0.72f, 0.5f), 0.004f, 0.0f);
    printf("  clear %.3f, hazy %.2f, light cloud %.2f deg\n", clear, hazy, light);
    CHECK(hazy > 0.8f && hazy < 2.0f, "hazy %g (1..1.5 deg expected)", hazy);
    CHECK(light > 2.0f && light < 5.0f, "light cloud %g", light);
    float prev = 0;
    for (float m = 1.0f; m <= 5.0f; m += 0.25f) { float r = ssky_source_radius(c, 0.265f, m, 0, 0, 0); CHECK(r >= prev, "haze not monotonic at %g", m); prev = r; }
    prev = 0;
    for (float t = 0; t <= 6.0f; t += 0.25f) { float r = ssky_source_radius(c, 0.265f, 1, t, 0, 0); CHECK(r >= prev, "veil not monotonic at %g", t); prev = r; }
    prev = 0;
    for (float f = 0; f <= 0.1f; f += 0.005f) { float r = ssky_source_radius(c, 0.265f, 1, 0, f, 0); CHECK(r >= prev, "fog not monotonic at %g", f); prev = r; }
    prev = 0;
    for (float p = 0; p <= 1.0f; p += 0.1f) { float r = ssky_source_radius(c, 0.265f, 1, 0, 0, p); CHECK(r >= prev, "precip not monotonic at %g", p); prev = r; }
    CHECK(ssky_source_radius(c, 0.265f, 9, 20, 1, 1) <= c->max_radius_deg + 1e-4f, "cap");
    CHECK(fabsf(ssky_penumbra(20.0f, 0.265f) - 0.0925f) < 0.001f, "penumbra 20 m %g", ssky_penumbra(20.0f, 0.265f));
}

// ------------------------------------------------------------------ PCSS (CPU port)

// A light-space depth map (metres along the light) of a ground plane with a pole, the light
// coming down at elevation `elev` along +x (the pole's shadow runs along +x on the ground).
// Map space: u = ground x (m), v = ground z (m) projected along the light; depth = metres from
// the light's near plane. One texel = mpt metres on the ground's projection.
#define MAPN 1024
static float g_map[MAPN * MAPN];
static float g_mpt, g_u0;
static float g_ga, g_gb;   // receiver plane depth gradient (m per m along the map axes)

// The receiver ground point (x, z) seen by the light: map coordinate and depth.
// Ortho light looking along L = (cos e, -sin e, 0): screen axes a = (sin e, cos e, 0), b = z.
static void light_proj(float elev, float x, float y, float z, float *a, float *b, float *depth) {
    float ce = cosf(elev), se = sinf(elev);
    *a = x * se + y * ce;
    *b = z;
    *depth = 1000.0f + x * ce - y * se;   // along L from a plane 1000 m back
}

// Caster: a vertical fin of half thickness FIN_T along the light (x) and half width hw across it
// (z in [-hw, hw]), height H. Thin along the light, so its silhouette edge and the depth the map
// stores are the same blocker distance (a thick box's light-facing face would sit farther away).
#define FIN_T 0.02f
static void build_map(float elev, float H, float hw, float mpt) {
    g_mpt = mpt;
    g_u0 = -2.0f;
    // ground plane: n = +Y, towards the light (-cos e, sin e, 0), map axis a = (sin e, cos e, 0):
    // dz/da = (n.a) / (n.l) = cos e / sin e
    g_ga = cosf(elev) / sinf(elev);
    g_gb = 0.0f;
    for (int j = 0; j < MAPN; j++)
        for (int i = 0; i < MAPN; i++) {
            float a = g_u0 + (i + 0.5f) * mpt, b = -(MAPN / 2) * mpt + (j + 0.5f) * mpt;
            // ground point for this texel: a = x sin e  (y = 0)
            float se = sinf(elev), ce = cosf(elev);
            float gx = a / se;
            float d = 1000.0f + gx * ce;
            // fin: the light ray through (a, b) hits it at height y where a = x se + y ce with
            // |x| <= FIN_T: nearest (smallest depth) is the x = -FIN_T face, or the top.
            if (fabsf(b) <= hw) {
                float y = (a + FIN_T * se) / ce;   // x = -FIN_T face
                if (y >= 0 && y <= H) d = fminf(d, 1000.0f - FIN_T * ce - y * se);
                // top: y = H, x = (a - H ce) / se in [-FIN_T, FIN_T]
                float tx = (a - H * ce) / se;
                if (tx >= -FIN_T && tx <= FIN_T) d = fminf(d, 1000.0f + tx * ce - H * se);
            }
            g_map[j * MAPN + i] = d;
        }
}

static float map_fetch(float a, float b) {
    int i = (int)floorf((a - g_u0) / g_mpt), j = (int)floorf(b / g_mpt + MAPN / 2);
    if (i < 0 || j < 0 || i >= MAPN || j >= MAPN) return 1e9f;
    return g_map[j * MAPN + i];
}
// Hardware compare with bilinear PCF (2 x 2).
static float map_compare(float a, float b, float z) {
    float fi = (a - g_u0) / g_mpt - 0.5f, fj = b / g_mpt + MAPN / 2 - 0.5f;
    int i = (int)floorf(fi), j = (int)floorf(fj);
    float tx = fi - i, ty = fj - j;
    float s[4];
    for (int k = 0; k < 4; k++) {
        int ii = i + (k & 1), jj = j + (k >> 1);
        float d = (ii < 0 || jj < 0 || ii >= MAPN || jj >= MAPN) ? 1e9f : g_map[jj * MAPN + ii];
        s[k] = z <= d ? 1.0f : 0.0f;
    }
    return (s[0] * (1 - tx) + s[1] * tx) * (1 - ty) + (s[2] * (1 - tx) + s[3] * tx) * ty;
}

// Mirrors lit_body.glsl: Vogel disc sample i of n, rotated by rot.
static void vogel(int i, int n, float rot, float *x, float *y) {
    float r = sqrtf((i + 0.5f) / (float)n), t = i * 2.39996323f + rot;
    *x = r * cosf(t);
    *y = r * sinf(t);
}

// pcss_tile (lit_body.glsl): blocker search over search_m x tan(r), filter half width =
// blocker distance x tan(r), both in texels: the filter in [0.5, max_tex], the search in
// [1, 1.5 max_tex] (blockers at the edge of the search still count fully). Every tap
// compares against the receiver's plane at the tap (depth gradient ga, gb in m per m of the map):
// a wide kernel on a surface sloped to the light does not shadow itself.
static float pcss(float a, float b, float z, float tan_r, float max_tex, float search_m, int nb, int nf, float rot) {
    float search = clampf(search_m * tan_r / g_mpt, 1.0f, max_tex * 1.5f);
    float sum = 0, cnt = 0;
    for (int i = 0; i < nb; i++) {
        float ox, oy;
        vogel(i, nb, rot, &ox, &oy);
        float da = ox * search * g_mpt, db = oy * search * g_mpt;
        float zt = z + g_ga * da + g_gb * db;
        float d = map_fetch(a + da, b + db);
        if (d < zt) { sum += zt - d; cnt += 1; }
    }
    if (cnt == 0) return 1.0f;
    float dblock = sum / cnt;
    float w = clampf(dblock * tan_r / g_mpt, 0.5f, max_tex);
    float s = 0;
    for (int i = 0; i < nf; i++) {
        float ox, oy;
        vogel(i, nf, rot, &ox, &oy);
        float da = ox * w * g_mpt, db = oy * w * g_mpt;
        s += map_compare(a + da, b + db, z + g_ga * da + g_gb * db);
    }
    return s / nf;
}

// Visibility across the shadow edge of a half-plane occluder for a uniform disc of radius R:
// the fraction of the disc on the lit side of offset u.
static float disc_profile(float u, float R) {
    float t = clampf(u / R, -1.0f, 1.0f);
    float a = acosf(t);
    return 1.0f - (a - t * sqrtf(1 - t * t)) / DG_PI;
}
static float disc_10_90(float R) {
    // solve profile = 0.1 / 0.9
    float lo = 0, hi = 0;
    for (float u = -R; u <= R; u += R * 1e-4f) {
        float p = disc_profile(u, R);
        if (p < 0.1f) lo = u;
        if (p < 0.9f) hi = u;
    }
    return hi - lo;
}

// Edge width (10-90 %) across z at ground distance x from the pole's foot (distance from the
// caster along the shadow ~ the pole's lit edge height x / ... measured in the map).
static float measure_edge(float elev, float x, float tan_r, float max_tex, float search_m, float hw) {
    float a, b0, z;
    light_proj(elev, x, 0, 0, &a, &b0, &z);
    z -= 0.01f + g_mpt * sqrtf(g_ga * g_ga + g_gb * g_gb);   // receiver bias (m): constant + one texel of slope
    float lo = -1, hi = -1;
    // across the shadow (z), averaged over kernel rotations (TAA)
    float step = g_mpt * 0.05f;
    float prev = 0;
    for (float bz = 0; bz < hw + 2.0f; bz += step) {
        float v = 0;
        for (int r = 0; r < 32; r++) v += pcss(a, bz, z, tan_r, max_tex, search_m, 16, 16, r * 0.19634954f);
        v /= 32;
        if (lo < 0 && v >= 0.1f) lo = bz;
        if (hi < 0 && v >= 0.9f) { hi = bz; break; }
        prev = v;
    }
    (void)prev;
    if (lo < 0 || hi < 0) {
        for (float bz = 0; bz < hw + 0.6f; bz += 0.05f) {
            float v = 0;
            for (int r = 0; r < 32; r++) v += pcss(a, bz, z, tan_r, max_tex, search_m, 16, 16, r * 0.19634954f);
            printf("    z %.2f: %.3f\n", bz, v / 32);
        }
    }
    return lo >= 0 && hi >= 0 ? hi - lo : -1;
}

static void test_penumbra(const ShadowSkyConfig *c) {
    printf("shadow_sky: PCSS penumbra width\n");
    float elev = 30.0f * DG_DEG2RAD;
    float mpt = 24.0f / 2048.0f;   // cascade 0 at 4096 (Modern): 1.17 cm texels
    float H = 40.0f, hw = 1.0f;   // 2 m wide: the far penumbrae of both edges never meet
    build_map(elev, H, hw, mpt);
    float rad[2] = { 0.265f, 1.0f };
    const char *nm[2] = { "clear", "hazy" };
    float dist[3] = { 1.0f, 5.0f, 20.0f };
    for (int k = 0; k < 2; k++) {
        float tan_r = tanf(rad[k] * DG_DEG2RAD);
        for (int i = 0; i < 3; i++) {
            // A ground point at x has the pole's edge (the side face, its height y = x tan e) as
            // the blocker: distance from the caster along the light = x / cos e.
            float d = dist[i];
            float x = d * cosf(elev);
            float R = d * tan_r;   // penumbra half width (m) = d x tan(r)
            float expect = disc_10_90(R);
            float got = measure_edge(elev, x, tan_r, c->pcss_max_texels[2], c->pcss_search_m, hw);
            float tex = R / mpt;
            printf("  %-5s d %4.1f m: half width %.4f m (%.1f texels), 10-90 expected %.4f, PCSS %.4f\n", nm[k], d, R, tex, expect, got);
            CHECK(got > 0, "%s %g m: no edge found", nm[k], d);
            if (tex >= 3.0f && tex <= c->pcss_max_texels[2]) CHECK(fabsf(got - expect) <= 0.15f * expect, "%s %g m: %.4f vs %.4f (> 15 %%)", nm[k], d, got, expect);
            else if (tex < 3.0f) CHECK(got <= 2.0f * mpt * 1.8f + expect, "%s %g m: sub-texel penumbra %.4f too wide", nm[k], d, got);
        }
    }
}

// ------------------------------------------------------------------ contrast

// The lit shader's terms (lit_body.glsl): diffuse ambient = E_sky / pi x albedo x AO, direct =
// albedo / pi x n.l x E x shadow. Lit / shaded on the same surface.
static float shade(float albedo, float sky_e, float direct, float ndl, float sh) { return albedo * sky_e / DG_PI + albedo / DG_PI * ndl * direct * sh; }

static void test_contrast(const ShadowSkyConfig *c) {
    printf("shadow_sky: contrast\n");
    const Cond *cs[4] = { &C_CLEAR, &C_HAZY, &C_LIGHT, &C_OVER };
    float ratio[4];
    for (int i = 0; i < 4; i++) {
        ShadowSkyState s;
        ssky_state_reset(&s);
        ShadowSkyInput in;
        ShadowSkyOut o;
        make_input(c, cs[i], &in);
        ssky_update(&s, c, &in, &o);
        float ndl = sinf(cs[i]->elev_deg * DG_DEG2RAD);   // ground
        float fade = o.key.fade;
        float lit = shade(0.18f, o.sky_e, o.key.intensity, ndl, 1.0f);
        float shd = shade(0.18f, o.sky_e, o.key.intensity, ndl, 1.0f - fade);   // a full occluder, faded
        ratio[i] = lit / shd;
        float pred = fade > 0.999f ? ssky_lit_shade_ratio(&o.key, o.sky_e, ndl) : ratio[i];
        printf("  %-11s C %.2f, radius %.2f deg, fade %.2f, lit/shade %.2f (veil tau %.2f)\n", cs[i]->name, o.key.contrast, o.key.radius_deg, fade, ratio[i], o.veil_tau);
        CHECK(fabsf(ratio[i] - pred) < 0.01f * pred, "%s: shading %.3f vs predicted %.3f", cs[i]->name, ratio[i], pred);
    }
    CHECK(ratio[0] > 1.5f * ratio[1], "clear %.2f not >> hazy %.2f", ratio[0], ratio[1]);
    CHECK(ratio[1] > ratio[2], "hazy %.2f <= light cloud %.2f", ratio[1], ratio[2]);
    CHECK(ratio[2] > ratio[3], "light cloud %.2f <= overcast %.2f", ratio[2], ratio[3]);
    CHECK(fabsf(ratio[3] - 1.0f) < 0.05f, "overcast %.3f not ~ 1", ratio[3]);
}

// ------------------------------------------------------------------ cutoff over a transition

static void test_cutoff(const ShadowSkyConfig *c) {
    printf("shadow_sky: cutoff (2 min clear -> overcast and back)\n");
    ShadowSkyState s;
    ssky_state_reset(&s);
    int frames = 120 * 60, toggles = 0, off_frame = -1;
    bool prev_cast = true;
    float max_delta = 0, prev_lum = -1;
    float c_at_off = -1, max_c_after = 0;
    for (int f = 0; f <= frames; f++) {
        float t = (float)f / frames;
        Cond k = C_CLEAR;
        k.cover = t;
        k.density = 0.5f + 0.4f * t;
        k.mie = 1.0f + 0.5f * t;
        ShadowSkyInput in;
        ShadowSkyOut o;
        make_input(c, &k, &in);
        in.dt = f ? DT : 0.0f;
        ssky_update(&s, c, &in, &o);
        // the shaded reference: ground under an occluder (shadow factor 1 - fade); lit: sh = 1
        float ndl = sinf(k.elev_deg * DG_DEG2RAD);
        float lum = shade(0.18f, o.sky_e, o.key.intensity, ndl, 1.0f - o.key.fade);
        if (prev_lum > 0) max_delta = fmaxf(max_delta, fabsf(lum - prev_lum) / prev_lum);
        prev_lum = lum;
        if (o.key.cast != prev_cast) {
            toggles++;
            if (!o.key.cast) { off_frame = f; c_at_off = o.key.contrast; }
        }
        if (off_frame >= 0) max_c_after = fmaxf(max_c_after, o.key.contrast);
        prev_cast = o.key.cast;
    }
    printf("  passes off at %.1f s (C %.3f), %d toggles, max shaded luminance step %.3f %%/frame\n", off_frame / 60.0f, c_at_off, toggles, max_delta * 100.0f);
    CHECK(toggles == 1 && off_frame > 0, "toggles %d", toggles);
    CHECK(c_at_off >= 0 && c_at_off < c->cutoff_off, "turned off at C %.3f (cutoff %.3f)", c_at_off, c->cutoff_off);
    CHECK(max_c_after < c->cutoff_off * (1.0f + c->cutoff_reenable), "contrast came back over the re-enable threshold while off");
    CHECK(max_delta < 0.005f, "luminance step %.4f per frame", max_delta);
    // Back: on again, only above the re-enable threshold.
    bool on = false;
    float c_on = -1;
    for (int f = 0; f <= frames && !on; f++) {
        Cond k = C_CLEAR;
        float t = 1.0f - (float)f / frames;
        k.cover = t;
        k.density = 0.5f + 0.4f * t;
        k.mie = 1.0f + 0.5f * t;
        ShadowSkyInput in;
        ShadowSkyOut o;
        make_input(c, &k, &in);
        in.dt = DT;
        ssky_update(&s, c, &in, &o);
        if (o.key.cast) { on = true; c_on = o.key.contrast; }
    }
    CHECK(on && c_on > c->cutoff_off * (1.0f + c->cutoff_reenable) - 1e-4f, "re-enabled at C %.3f", c_on);
    // Chatter: contrast wobbling around the cutoff never toggles more than once.
    ssky_state_reset(&s);
    int tg = 0;
    bool pc = true;
    for (int f = 0; f < 3600; f++) {
        ShadowSkyInput in;
        ShadowSkyOut o;
        make_input(c, &C_CLEAR, &in);
        float sky = c->cutoff_off * (1.0f + 0.1f * sinf(f * 0.37f));   // C = 1 / that ... around off
        in.sky_up = v3_scale(v3(1, 1, 1), vmaxf3(in.sun_light) / sky);
        in.dt = f ? DT : 0.0f;
        ssky_update(&s, c, &in, &o);
        if (f && o.key.cast != pc) tg++;
        pc = o.key.cast;
    }
    CHECK(tg <= 1, "chatter: %d toggles", tg);
}

// ------------------------------------------------------------------ moon

static void test_moon(const ShadowSkyConfig *c) {
    printf("shadow_sky: moon handover\n");
    ShadowSkyState s;
    ssky_state_reset(&s);
    // Sunset with the moon up (full, 30 deg): the sky darkens with the sun.
    float max_fade_step = 0, prev_fade = -1, prev_int = -1, max_int_step = 0;
    bool handed = false, second_seen = false;
    for (int f = 0; f < 60 * 120; f++) {
        float elev = 4.0f - 12.0f * f / (60.0f * 120.0f);   // +4 -> -8 deg in 2 min
        ShadowSkyInput in;
        Cond k = C_CLEAR;
        k.elev_deg = elev;
        make_input(c, &k, &in);
        // twilight sky: still lit after the sun is down
        float sky = 3.0f * 0.09f * fmaxf(sinf(fmaxf(elev, 0.0f) * DG_DEG2RAD), 0.0f) + 0.02f * expf(elev / 3.0f) + 0.0008f;
        in.sky_up = v3_scale(v3(0.7f, 0.85f, 1.0f), sky);
        in.moon_elev = 30.0f * DG_DEG2RAD;
        in.moon_light = v3_scale(v3(0.82f, 0.9f, 1.0f), 0.02f * 0.9f);
        in.dt = f ? DT : 0.0f;
        ShadowSkyOut o;
        ssky_update(&s, c, &in, &o);
        float mf = o.key_is_moon ? o.key.fade : o.second.fade;
        float mi = o.key_is_moon ? o.key.intensity : o.second.intensity;
        if (!o.key_is_moon && o.second_shadowed) second_seen = true;
        if (prev_fade >= 0) max_fade_step = fmaxf(max_fade_step, fabsf(mf - prev_fade));
        if (prev_int >= 0) max_int_step = fmaxf(max_int_step, fabsf(mi - prev_int));
        prev_fade = mf;
        prev_int = mi;
        if (o.key_is_moon && !handed) {
            handed = true;
            CHECK(elev <= 0.0f, "moon took the cascades with the sun at %.2f deg", elev);
        }
        if (o.key_is_moon) {
            float mr = ssky_source_radius(c, c->moon_radius_deg, in.mie_scale, o.veil_tau, in.fog, in.precip);
            CHECK(fabsf(o.key.radius_deg - mr) < 0.05f, "moon radius %.3f vs %.3f", o.key.radius_deg, mr);
            CHECK(fabsf(o.near_weight - 1.0f) < 1e-3f, "moon: cascade 0 must stay");
        }
    }
    CHECK(handed, "no handover");
    CHECK(second_seen, "the moon tile never ran before the handover");
    CHECK(max_fade_step < 0.02f, "moon fade jumped %.3f in a frame", max_fade_step);
    CHECK(max_int_step < 1e-4f, "moon intensity jumped %.5f", max_int_step);
    // Night: full moon, dark sky -> sharp, fully faded in.
    ShadowSkyOut o;
    {
        ShadowSkyInput in;
        Cond k = C_CLEAR;
        k.elev_deg = -20;
        make_input(c, &k, &in);
        in.sky_up = v3(0.0006f, 0.0007f, 0.0009f);
        in.moon_elev = 40 * DG_DEG2RAD;
        in.moon_light = v3_scale(v3(0.82f, 0.9f, 1.0f), 0.018f);
        in.dt = 0;
        ssky_state_reset(&s);
        ssky_update(&s, c, &in, &o);
        CHECK(o.key_is_moon && o.key.cast && o.key.fade > 0.99f, "night moon: cast %d fade %.2f", o.key.cast, o.key.fade);
        float mr = ssky_source_radius(c, c->moon_radius_deg, in.mie_scale, o.veil_tau, in.fog, in.precip);
        CHECK(fabsf(o.key.radius_deg - mr) < 1e-4f && o.key.radius_deg < 0.35f, "night moon radius %.3f (configured %.3f)", o.key.radius_deg, mr);
        CHECK(o.key.color.z >= o.key.color.x, "moonlight not blue");
        // Dawn: the sky brightens before the sun is up -> the moon's shadow fades and stops.
        bool gone = false;
        for (int f = 0; f < 60 * 60 && !gone; f++) {
            in.sky_up = v3_scale(in.sky_up, 1.004f);
            in.dt = DT;
            ssky_update(&s, c, &in, &o);
            if (!o.key.cast) gone = true;
        }
        CHECK(gone, "moon shadows never left at dawn (C %.3f)", o.key.contrast);
        // New moon: under the absolute floor.
        in.moon_light = v3(0.0001f, 0.0001f, 0.0001f);
        in.sky_up = v3(1e-5f, 1e-5f, 1e-5f);
        ssky_state_reset(&s);
        in.dt = 0;
        ssky_update(&s, c, &in, &o);
        CHECK(!o.key.cast, "new moon casts");
    }
}

// ------------------------------------------------------------------ clouds, tiers, exposure

static void test_clouds_tiers(const ShadowSkyConfig *c) {
    printf("shadow_sky: cloud drift, wide mode, PCF steps, exposure, ground\n");
    ShadowSkyState s;
    ssky_state_reset(&s);
    ShadowSkyInput in;
    ShadowSkyOut o;
    make_input(c, &C_CLEAR, &in);
    in.wind_speed = 4.0f;
    in.wind_dir_deg = 90.0f;   // towards +X
    in.dt = 0;
    ssky_update(&s, c, &in, &o);
    vec2 o0 = o.cloud_offset;
    in.dt = DT;
    for (int f = 0; f < 600; f++) ssky_update(&s, c, &in, &o);
    float moved = o.cloud_offset.x - o0.x, expect = 10.0f * 4.0f * c->cloud_wind_aloft;
    CHECK(fabsf(moved - expect) < 0.01f * expect && fabsf(o.cloud_offset.y - o0.y) < 0.01f, "drift %.2f m in 10 s (expected %.2f)", moved, expect);
    CHECK(fabsf(sqrtf(o.cloud_vel.x * o.cloud_vel.x + o.cloud_vel.y * o.cloud_vel.y) - 4.0f * c->cloud_wind_aloft) < 1e-3f, "cloud speed");
    // Wide mode on the full tier: light cloud (> wide_deg) fades cascade 0 out smoothly.
    ssky_state_reset(&s);
    make_input(c, &C_CLEAR, &in);
    in.dt = 0;
    ssky_update(&s, c, &in, &o);
    CHECK(o.near_weight == 1.0f, "clear: near weight %.2f", o.near_weight);
    Cond k = C_CLEAR;
    k.fog = 0.05f;   // ~4.6 deg
    float maxstep = 0, prev = 1;
    for (int f = 0; f < 600; f++) {
        make_input(c, &k, &in);
        in.dt = DT;
        ssky_update(&s, c, &in, &o);
        maxstep = fmaxf(maxstep, fabsf(o.near_weight - prev));
        prev = o.near_weight;
    }
    CHECK(o.key.radius_deg > c->wide_deg && o.near_weight == 0.0f, "fog: radius %.2f near %.2f", o.key.radius_deg, o.near_weight);
    CHECK(maxstep < 0.05f, "near weight stepped %.3f", maxstep);
    // Limited tier: no wide mode, smaller filter.
    make_input(c, &k, &in);
    in.softness = 1;
    in.dt = 0;
    ssky_state_reset(&s);
    ssky_update(&s, c, &in, &o);
    CHECK(o.near_weight == 1.0f && o.pcss_max_texels == c->pcss_max_texels[1], "limited tier");
    // PCF steps: clear 0, hazy 1, light cloud 2, with hysteresis.
    const Cond *cs[3] = { &C_CLEAR, &C_HAZY, &C_LIGHT };
    for (int i = 0; i < 3; i++) {
        make_input(c, cs[i], &in);
        in.softness = 0;
        in.dt = 0;
        ssky_state_reset(&s);
        ssky_update(&s, c, &in, &o);
        CHECK(fabsf(o.pcf_radius - (float)i) < 1e-3f, "%s: PCF %.2f", cs[i]->name, o.pcf_radius);
    }
    // Hysteresis: radius wobbling around the first step does not flip the step back and forth.
    ssky_state_reset(&s);
    int flips = 0, ps = -1;
    for (int f = 0; f < 2000; f++) {
        make_input(c, &C_CLEAR, &in);
        in.softness = 0;
        in.mie_scale = 1.0f + 0.5f + 0.06f * sinf(f * 0.3f);   // around 0.6 deg (just over the 0.5 step)
        in.dt = f ? DT : 0;
        ssky_update(&s, c, &in, &o);
        if (ps >= 0 && s.pcf_step != ps) flips++;
        ps = s.pcf_step;
    }
    CHECK(flips <= 1, "PCF step flipped %d times", flips);
    // Era 2010: fixed hardness whatever the sky, the contrast unchanged.
    make_input(c, &C_LIGHT, &in);
    in.fixed_hardness = true;
    in.softness = 0;
    in.dt = 0;
    ssky_state_reset(&s);
    ssky_update(&s, c, &in, &o);
    ShadowSkyOut o2;
    ShadowSkyInput in2 = in;
    in2.fixed_hardness = false;
    ShadowSkyState s2;
    ssky_state_reset(&s2);
    ssky_update(&s2, c, &in2, &o2);
    CHECK(o.pcf_radius == c->era_pcf && o.near_weight == 1.0f, "era: PCF %.2f", o.pcf_radius);
    CHECK(fabsf(o.key.fade - o2.key.fade) < 1e-6f && fabsf(o.key.contrast - o2.key.contrast) < 1e-6f, "era changed the contrast");
    // Exposure: clear 0, overcast > hazy, snow adds.
    float ev[3];
    const Cond *ec[3] = { &C_CLEAR, &C_HAZY, &C_OVER };
    for (int i = 0; i < 3; i++) {
        make_input(c, ec[i], &in);
        in.dt = 0;
        ssky_state_reset(&s);
        ssky_update(&s, c, &in, &o);
        ev[i] = o.exposure_comp;
    }
    CHECK(fabsf(ev[0]) < 1e-4f && ev[2] > ev[1] && ev[2] <= 0.5f, "exposure %.2f %.2f %.2f", ev[0], ev[1], ev[2]);
    make_input(c, &C_OVER, &in);
    in.snow = 1.0f;
    ssky_state_reset(&s);
    ssky_update(&s, c, &in, &o);
    CHECK(o.exposure_comp > ev[2] + 0.3f, "snow exposure %.2f", o.exposure_comp);
    CHECK(o.ground_albedo.x > 0.8f, "snow ground %.2f", o.ground_albedo.x);
    in.snow = 0;
    in.wetness = 1;
    ssky_update(&s, c, &in, &o);
    CHECK(o.ground_albedo.x < c->ground_albedo[0], "wet ground %.2f", o.ground_albedo.x);
}

// ------------------------------------------------------------------ TAA on a softening edge

static void test_taa(const ShadowSkyConfig *c) {
    printf("shadow_sky: TAA on a slowly softening edge\n");
    float elev = 30.0f * DG_DEG2RAD;
    float mpt = 24.0f / 1024.0f;   // High (2048 atlas)
    build_map(elev, 40.0f, 0.15f, mpt);
    float d = 6.0f, x = d * cosf(elev);
    float a, b, z;
    light_proj(elev, x, 0, 0, &a, &b, &z);
    z -= 0.01f + g_mpt * sqrtf(g_ga * g_ga + g_gb * g_gb);
    // pixels across the edge
    enum { NP = 24 };
    float hist[NP], prevr[NP];
    float maxd = 0, sum_var = 0;
    int nvar = 0;
    for (int f = 0; f < 600; f++) {
        float rdeg = 0.6f + 1.2f * f / 600.0f;   // 0.6 -> 1.8 deg over 10 s
        float tan_r = tanf(rdeg * DG_DEG2RAD);
        // interleaved gradient noise per pixel and frame (lit_body.glsl)
        for (int p = 0; p < NP; p++) {
            float bz = 0.15f - 0.12f + p * 0.01f;
            float px = (float)p, py = 17.0f, fr = (float)(f % 64);
            float ign = fmodf(52.9829189f * fmodf(0.06711056f * (px + 5.588238f * fr) + 0.00583715f * (py + 5.588238f * fr), 1.0f), 1.0f);
            float v = pcss(a, bz, z, tan_r, c->pcss_max_texels[2], c->pcss_search_m, 16, 16, ign * 6.2831853f);
            hist[p] = f ? hist[p] + (v - hist[p]) * 0.1f : v;
            if (f > 60) {
                float dlt = fabsf(hist[p] - prevr[p]);
                maxd = fmaxf(maxd, dlt);
                sum_var += dlt * dlt;
                nvar++;
            }
            prevr[p] = hist[p];
        }
    }
    float rms = sqrtf(sum_var / (float)nvar);
    printf("  resolved frame-to-frame change: rms %.4f, max %.4f\n", rms, maxd);
    CHECK(rms < 0.01f, "TAA rms %.4f", rms);
    CHECK(maxd < 0.05f, "TAA max %.4f", maxd);
}

int main(void) {
    ShadowSkyConfig c;
    ssky_config_default(&c);
    test_config();
    test_source(&c);
    test_penumbra(&c);
    test_contrast(&c);
    test_cutoff(&c);
    test_moon(&c);
    test_clouds_tiers(&c);
    test_taa(&c);
    printf("test_shadow_sky: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
