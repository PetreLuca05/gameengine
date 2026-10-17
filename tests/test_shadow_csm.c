// Cascaded sun shadow layout tests (docs/SHADOWS.md "Cascades", src/render/shadow_csm.c):
//   tables     every Engine Fidelity preset (default and max values, era overrides) follows the one
//              formula: the splits end at the shadow distance, cascade 0 has at least the preset's
//              texel density, neighbouring cascades within 6x, bias from the texel size
//   fit        every visible point up to a cascade's split lies inside its tile (with the shader's
//              border) for any camera rotation, field of view and aspect; the tile size never
//              depends on the rotation
//   snapping   10 frames of pure sub-texel translation (and a slow turn): a fixed world point keeps
//              its fractional texel position, so edges move by whole texels only (no swimming)
//   casters    a caster up to 1 km towards the light from any receiver (towers outside the view at
//              a low sun) lies inside the tile's depth range, at every sun elevation and heading
//   selection  the shadow reaches the distance and fades only over its last 10 %; the cascade
//              index + blend weight is continuous along the distance (no seam steps)
//   bias       normal / constant bias proportional to the texel size, raised smoothly at a low sun;
//              a penumbra in metres is the same on every cascade until a clamp applies
//   viewmodel  the rig's tile: sub-millimetre texels at 2048, snapped, the rig inside
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

static u32 g_rng = 12345u;
static float frand(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(g_rng >> 8) / 16777216.0f;
}

static const float REACH_FULL = 140.0f;   // render.c: max(level extent x 3.5, 110) at the dev room's 40 m

// Camera basis from yaw / pitch (the engine's FPS view: yaw 0 looks along -Z).
static void cam_basis(float yaw, float pitch, vec3 *f, vec3 *r, vec3 *u) {
    *f = v3(-sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch));
    *r = v3_norm(v3_cross(*f, v3(0, 1, 0)));
    *u = v3_cross(*r, *f);
}

// Local tile coordinates (0..1) and NDC depth of a world point.
static void tile_coord(const CsmTile *t, vec3 p, float *lx, float *ly, float *z) {
    vec4 c = mat4_mul_v4(t->vp, v4(p.x, p.y, p.z, 1.0f));
    *lx = c.x / c.w * 0.5f + 0.5f;
    *ly = c.y / c.w * 0.5f + 0.5f;
    *z = c.z / c.w;
}

// ------------------------------------------------------------------ preset tables

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

static float feat(const cJSON *feats, const char *name, const char *key, float fallback) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(feats, name);
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : fallback;
}

static void check_table(const char *level, const char *variant, int q, int casc, float dist_k, float min_tpm) {
    CsmParams p = { casc, csm_tile_texels(q), REACH_FULL * dist_k, 70.0f, 16.0f / 9.0f };
    char err[256];
    bool ok = csm_validate(&p, min_tpm, 6.0f, err, sizeof(err));
    CsmLayout l;
    csm_layout(&p, &l);
    printf("  %-7s %-12s q%d (%4u) x%d %5.1f m: c0 %.1f m %5.1f tx/m", level, variant, q, p.tile, casc, p.distance, l.split[0], 1.0f / l.texel_m[0]);
    for (int k = 1; k < l.count; k++) printf(" | c%d %.1f m %5.1f tx/m", k, l.split[k], 1.0f / l.texel_m[k]);
    printf("\n");
    CHECK(ok, "%s %s: %s", level, variant, err);
}

static void test_tables(void) {
    printf("shadow_csm: preset tables (one formula)\n");
    static const char *levels[5] = { "off", "low", "medium", "high", "modern" };
    static const float min_tpm[5] = { 6.0f, 30.0f, 40.0f, 80.0f, 80.0f };   // texels per metre in cascade 0 (docs/SHADOWS.md)
    for (int i = 0; i < 5; i++) {
        char path[256];
        snprintf(path, sizeof(path), "assets/render/fidelity/%s.json", levels[i]);
        char *txt = read_text(path);
        CHECK(txt != NULL, "cannot read %s (run from the build's bin directory)", path);
        if (!txt) continue;
        cJSON *r = cJSON_Parse(txt);
        free(txt);
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "features");
        int q = (int)feat(f, "shadow_quality", "default", 2), qmax = (int)feat(f, "shadow_quality", "max", (float)q);
        int c = (int)feat(f, "shadow_cascades", "default", 3), cmax = (int)feat(f, "shadow_cascades", "max", (float)c);
        float d = feat(f, "shadow_distance", "default", 1.0f), dmax = feat(f, "shadow_distance", "max", d);
        check_table(levels[i], "default", q, c, d, min_tpm[i]);
        check_table(levels[i], "max", qmax, cmax, dmax, min_tpm[i]);
        // era overrides of the shadow entries
        const cJSON *eras = cJSON_GetObjectItemCaseSensitive(r, "eras");
        for (const cJSON *e = eras ? eras->child : NULL; e; e = e->next) {
            int eq = (int)feat(e, "shadow_quality", "default", (float)q), ec = (int)feat(e, "shadow_cascades", "default", (float)c);
            float ed = feat(e, "shadow_distance", "default", d);
            char v[32];
            snprintf(v, sizeof(v), "era %s", e->string);
            check_table(levels[i], v, eq, ec, ed, min_tpm[i] * 0.8f);   // an era may trade a little density for reach
        }
        cJSON_Delete(r);
    }
    // The tile sizes the quality steps name (Settings > Graphics labels).
    CHECK(csm_tile_texels(1) == 1024 && csm_tile_texels(2) == 2048 && csm_tile_texels(3) == 4096, "tile texels");
    CHECK(csm_vm_texels(1) == 1024 && csm_vm_texels(2) == 2048 && csm_vm_texels(3) == 2048, "viewmodel texels");
}

// ------------------------------------------------------------------ fit

static void test_fit(void) {
    printf("shadow_csm: sector sphere fit, any rotation / FOV / aspect\n");
    const float fovs[3] = { 50.0f, 70.0f, 100.0f }, aspects[3] = { 4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f };
    int outside = 0, n = 0;
    float r_first = -1;
    bool r_same = true;
    for (int fi = 0; fi < 3; fi++)
        for (int ai = 0; ai < 3; ai++) {
            CsmParams p = { 3, 2048, 105.0f, fovs[fi], aspects[ai] };
            CsmLayout l;
            csm_layout(&p, &l);
            float tv = tanf(fovs[fi] * 0.5f * DG_DEG2RAD), th = tv * aspects[ai];
            for (int it = 0; it < 200; it++) {
                float yaw = frand() * 2.0f * DG_PI, pitch = (frand() - 0.5f) * 3.0f;
                float elev = (2.0f + frand() * 87.0f) * DG_DEG2RAD, az = frand() * 2.0f * DG_PI;
                vec3 L = v3(cosf(elev) * sinf(az), sinf(elev), cosf(elev) * cosf(az));
                mat4 lv = csm_light_view(L);
                vec3 cam = v3((frand() - 0.5f) * 2000.0f, frand() * 50.0f, (frand() - 0.5f) * 2000.0f), f, r, u;
                cam_basis(yaw, pitch, &f, &r, &u);
                for (int k = 0; k < 3; k++) {
                    CsmTile t;
                    csm_fit(&lv, v3_madd(cam, f, l.ahead[k]), l.radius[k], p.tile, l.radius[k] + 2.0f, &t);
                    if (fi == 1 && ai == 1 && k == 0) {
                        if (r_first < 0) r_first = t.texel_m;
                        else if (fabsf(t.texel_m - r_first) > 1e-6f) r_same = false;
                    }
                    float edge = 4.0f / (float)p.tile;
                    for (int s = 0; s < 40; s++) {
                        // a point in the view frustum at distance <= split k
                        float sx = (frand() * 2 - 1) * th, sy = (frand() * 2 - 1) * tv;
                        vec3 dir = v3_norm(v3_add(f, v3_add(v3_scale(r, sx), v3_scale(u, sy))));
                        float d = l.split[k] * sqrtf(frand());
                        if (s == 0) d = l.split[k];
                        vec3 pt = v3_madd(cam, dir, d);
                        float lx, ly, z;
                        tile_coord(&t, pt, &lx, &ly, &z);
                        n++;
                        if (lx < edge || lx > 1 - edge || ly < edge || ly > 1 - edge || z < 0 || z > 1) {
                            if (outside++ < 4) printf("    outside: fov %.0f aspect %.2f cascade %d d %.1f local %.4f %.4f z %.4f\n", fovs[fi], aspects[ai], k, d, lx, ly, z);
                        }
                    }
                }
            }
        }
    CHECK(outside == 0, "%d of %d visible points outside their cascade tile", outside, n);
    CHECK(r_same, "the texel size depends on the camera rotation");
    // Narrow FOVs (ADS zoom) fit with the floor: the same layout as the hip FOV.
    CsmParams a = { 3, 2048, 105.0f, 70.0f, 16.0f / 9.0f }, b = a;
    b.fov_y_deg = 25.0f;
    CsmLayout la, lb;
    csm_layout(&a, &la);
    csm_layout(&b, &lb);
    CHECK(fabsf(la.radius[0] - lb.radius[0]) < 1e-5f && fabsf(la.radius[2] - lb.radius[2]) < 1e-5f, "zooming resized the cascades");
}

// ------------------------------------------------------------------ snapping

static float frac_texel(const CsmTile *t, vec3 p, u32 tile, float *fy) {
    float lx, ly, z;
    tile_coord(t, p, &lx, &ly, &z);
    float x = lx * (float)tile, y = ly * (float)tile;
    *fy = y - floorf(y);
    return x - floorf(x);
}

static void test_snapping(void) {
    printf("shadow_csm: texel snapping (pure translation, slow turn)\n");
    CsmParams p = { 3, 2048, 105.0f, 70.0f, 16.0f / 9.0f };
    CsmLayout l;
    csm_layout(&p, &l);
    float worst = 0;
    for (int it = 0; it < 50; it++) {
        float elev = (5.0f + frand() * 80.0f) * DG_DEG2RAD, az = frand() * 2.0f * DG_PI;
        mat4 lv = csm_light_view(v3(cosf(elev) * sinf(az), sinf(elev), cosf(elev) * cosf(az)));
        vec3 cam = v3((frand() - 0.5f) * 400.0f, 2.0f, (frand() - 0.5f) * 400.0f), f, r, u;
        float yaw = frand() * 6.28f;
        cam_basis(yaw, 0.0f, &f, &r, &u);
        vec3 step = v3_scale(v3_norm(v3(frand() - 0.5f, 0.1f * (frand() - 0.5f), frand() - 0.5f)), 0.37f * l.texel_m[0]);
        vec3 P = v3_madd(cam, f, 4.0f);   // a fixed shadow edge 4 m ahead
        for (int k = 0; k < 3; k++) {
            float fx0 = 0, fy0 = 0;
            for (int fr = 0; fr < 10; fr++) {
                vec3 c = v3_madd(cam, step, (float)fr);
                vec3 ff = f;
                if (it & 1) {   // half the runs also turn 0.3 deg per frame
                    vec3 rr, uu;
                    cam_basis(yaw + fr * 0.3f * DG_DEG2RAD, 0.0f, &ff, &rr, &uu);
                }
                CsmTile t;
                csm_fit(&lv, v3_madd(c, ff, l.ahead[k]), l.radius[k], p.tile, l.radius[k] + 2.0f, &t);
                float fy, fx = frac_texel(&t, P, p.tile, &fy);
                if (fr == 0) { fx0 = fx; fy0 = fy; continue; }
                float dx = fabsf(fx - fx0), dy = fabsf(fy - fy0);
                dx = fminf(dx, 1.0f - dx);
                dy = fminf(dy, 1.0f - dy);
                worst = fmaxf(worst, fmaxf(dx, dy));
            }
        }
    }
    printf("  worst fractional texel drift over 10 frames: %.4f texel\n", worst);
    CHECK(worst < 0.02f, "edges swim: %.4f texel", worst);
}

// ------------------------------------------------------------------ casters outside the view

static void test_casters(void) {
    printf("shadow_csm: casters towards the light (off-screen / tall) at every elevation and heading\n");
    CsmParams p = { 3, 2048, 140.0f, 70.0f, 16.0f / 9.0f };
    CsmLayout l;
    csm_layout(&p, &l);
    const float elevs[6] = { 1.0f, 2.0f, 5.0f, 15.0f, 45.0f, 89.5f }, heights[4] = { 5.0f, 60.0f, 300.0f, 1000.0f };
    int miss = 0, n = 0;
    for (int e = 0; e < 6; e++)
        for (int a = 0; a < 24; a++) {
            float el = elevs[e] * DG_DEG2RAD, az = a * 15.0f * DG_DEG2RAD;
            vec3 L = v3(cosf(el) * sinf(az), sinf(el), cosf(el) * cosf(az));
            mat4 lv = csm_light_view(L);
            for (int y = 0; y < 8; y++) {
                vec3 f, r, u;
                cam_basis(y * 0.785f, 0.0f, &f, &r, &u);
                vec3 cam = v3(10.0f, 1.7f, -30.0f);
                for (int k = 0; k < 3; k++) {
                    CsmTile t;
                    csm_fit(&lv, v3_madd(cam, f, l.ahead[k]), l.radius[k], p.tile, l.radius[k] + 2.0f, &t);
                    vec3 recv = v3_madd(cam, f, l.split[k] * 0.9f);
                    for (int h = 0; h < 4; h++) {
                        float lx, ly, z;
                        tile_coord(&t, v3_madd(recv, L, heights[h]), &lx, &ly, &z);
                        n++;
                        if (z < 0.0f || z > 1.0f) miss++;
                    }
                }
            }
        }
    CHECK(miss == 0, "%d of %d casters outside the depth range", miss, n);
}

// ------------------------------------------------------------------ selection, fade, seams

static void test_selection(void) {
    printf("shadow_csm: reach, distance fade, continuous cascade blend\n");
    for (int n = 1; n <= 3; n++) {
        CsmParams p = { n, 2048, 105.0f, 70.0f, 16.0f / 9.0f };
        CsmLayout l;
        csm_layout(&p, &l);
        float D = l.split[n - 1], prev_eff = -1, prev_fade = 1, worst = 0;
        bool fade_ok = true;
        for (float d = 0.0f; d <= D * 1.05f; d += 0.01f) {
            float blend, fade;
            int c = csm_select(&l, d, 0, &blend, &fade);
            float eff = (float)c + blend;
            if (prev_eff >= 0) worst = fmaxf(worst, fabsf(eff - prev_eff));
            if (d <= 0.9f * D - 0.01f && fade < 1.0f) fade_ok = false;
            if (d >= D && fade > 0.0f) fade_ok = false;
            if (fade > prev_fade + 1e-6f) fade_ok = false;
            prev_eff = eff;
            prev_fade = fade;
        }
        CHECK(fade_ok, "%d cascades: the shadow must be full to 90 %% of %.0f m and gone at it", n, D);
        // smooth: a 12 % band over >= 1.3 m moves <= ~1.2 % per cm; a seam would step by ~1
        CHECK(worst < 0.02f, "%d cascades: the cascade blend steps by %.3f per cm (seam)", n, worst);
        CHECK(fabsf(D - 105.0f) < 1e-3f, "%d cascades: reach %.2f m", n, D);
    }
    // Wide mode (cascade 0 skipped): cascade 1's sphere holds the near field too.
    CsmParams p = { 3, 2048, 105.0f, 70.0f, 16.0f / 9.0f };
    CsmLayout l;
    csm_layout(&p, &l);
    float blend, fade;
    CHECK(csm_select(&l, 0.5f, 1, &blend, &fade) == 1, "wide mode: near points use cascade 1");
}

static void test_bias_kernel(void) {
    printf("shadow_csm: bias and kernel from the texel size\n");
    float prev = 10.0f;
    bool mono = true;
    for (float e = 0.0f; e <= 90.0f; e += 0.5f) {
        float k = csm_low_sun(e * DG_DEG2RAD);
        if (k > prev + 1e-6f || k < 1.0f - 1e-6f || k > 3.0f + 1e-6f) mono = false;
        prev = k;
    }
    CHECK(mono, "low-sun factor not monotonic in 1 .. 3");
    CHECK(fabsf(csm_low_sun(DG_PI * 0.5f) - 1.0f) < 1e-6f && csm_low_sun(1.0f * DG_DEG2RAD) > 2.9f, "low-sun factor ends");
    CsmBias a = csm_bias(0.01f, 1.0f), b = csm_bias(0.03f, 1.0f);
    CHECK(fabsf(b.normal_m / a.normal_m - 3.0f) < 1e-4f && fabsf(b.const_m / a.const_m - 3.0f) < 1e-4f, "bias not proportional to the texel");
    CHECK(a.const_m < 0.01f && a.normal_m < 0.02f, "noon bias at a 1 cm texel: %.4f / %.4f m (peter-panning)", a.const_m, a.normal_m);
    // A 5 cm penumbra: 4.2 texels on a 1.2 cm cascade, 1.4 on a 3.5 cm one: the same metres.
    float w0 = csm_kernel_texels(0.05f, 0.012f, 1.0f, 32.0f) * 0.012f, w1 = csm_kernel_texels(0.05f, 0.035f, 1.0f, 32.0f) * 0.035f;
    CHECK(fabsf(w0 - w1) < 1e-5f, "penumbra differs across cascades: %.4f vs %.4f m", w0, w1);
    CHECK(fabsf(csm_kernel_texels(0.001f, 0.01f, 1.0f, 32.0f) - 1.0f) < 1e-6f, "min clamp");
}

// ------------------------------------------------------------------ viewmodel

static void test_viewmodel(void) {
    printf("shadow_csm: viewmodel tile\n");
    mat4 lv = csm_light_view(v3_norm(v3(0.3f, 0.8f, -0.2f)));
    CsmTile t;
    vec3 rig = v3(12.0f, 1.4f, -7.0f);
    csm_fit_vm(&lv, rig, 2048, &t);
    printf("  2048: %.3f mm texels, half extent %.2f m\n", t.texel_m * 1000.0f, t.radius);
    CHECK(t.texel_m < 0.001f, "viewmodel texel %.4f m", t.texel_m);
    int out = 0;
    for (int i = 0; i < 500; i++) {
        vec3 p = v3_add(rig, v3((frand() - 0.5f) * 1.3f, (frand() - 0.5f) * 1.3f, (frand() - 0.5f) * 1.3f));
        if (v3_len(v3_sub(p, rig)) > 0.8f) continue;
        float lx, ly, z;
        tile_coord(&t, p, &lx, &ly, &z);
        float e = 2.0f / 2048.0f;
        if (lx < e || lx > 1 - e || ly < e || ly > 1 - e || z < 0 || z > 1) out++;
    }
    CHECK(out == 0, "%d rig points outside the viewmodel tile", out);
    // Snapped: the rig moving by sub-texels shifts the mapping by whole texels.
    float fy0, fx0 = frac_texel(&t, v3(12.1f, 1.3f, -7.2f), 2048, &fy0), worst = 0;
    for (int fr = 1; fr < 10; fr++) {
        CsmTile t2;
        csm_fit_vm(&lv, v3_add(rig, v3(0.00031f * fr, 0.0002f * fr, -0.00027f * fr)), 2048, &t2);
        float fy, fx = frac_texel(&t2, v3(12.1f, 1.3f, -7.2f), 2048, &fy);
        float dx = fabsf(fx - fx0), dy = fabsf(fy - fy0);
        worst = fmaxf(worst, fmaxf(fminf(dx, 1 - dx), fminf(dy, 1 - dy)));
    }
    CHECK(worst < 0.02f, "viewmodel tile swims %.4f texel", worst);
}

int main(void) {
    test_tables();
    test_fit();
    test_snapping();
    test_casters();
    test_selection();
    test_bias_kernel();
    test_viewmodel();
    printf("test_shadow_csm: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
