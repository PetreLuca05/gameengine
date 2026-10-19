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
#include "render/shadow_settings.h"
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

static void check_table(const char *level, const char *variant, int res, int casc, float dist, float min_tpm) {
    QualityShadowSettings q;
    shs_quality_default(&q);
    q.cascades = casc;
    q.distance = dist;
    shs_quality_sanitize(&q);
    float splits[4];
    int n = shs_cascade_splits(&q, splits);
    CsmParams p = { n, shs_res_texels(res, SHADOW_LIGHT_DIRECTIONAL), q.distance, 70.0f, 16.0f / 9.0f, splits };
    char err[256];
    bool ok = csm_validate(&p, min_tpm, 6.0f, err, sizeof(err));
    CsmLayout l;
    csm_layout(&p, &l);
    printf("  %-7s %-12s %4u x%d %5.0f m: c0 %.1f m %5.1f tx/m", level, variant, p.tile, n, p.distance, l.split[0], 1.0f / l.texel_m[0]);
    for (int k = 1; k < l.count; k++) printf(" | c%d %.1f m %5.1f tx/m", k, l.split[k], 1.0f / l.texel_m[k]);
    printf("\n");
    CHECK(ok, "%s %s: %s", level, variant, err);
}

static void test_tables(void) {
    printf("shadow_csm: preset tables (Unity knobs, one formula)\n");
    static const char *levels[5] = { "off", "low", "medium", "high", "modern" };
    static const float min_tpm[5] = { 3.5f, 12.0f, 12.0f, 60.0f, 80.0f };   // texels per metre in cascade 0 (docs/SHADOWS_UNITY_MODEL.md)
    for (int i = 0; i < 5; i++) {
        char path[256];
        snprintf(path, sizeof(path), "assets/render/fidelity/%s.json", levels[i]);
        char *txt = read_text(path);
        CHECK(txt != NULL, "cannot read %s (run from the build's bin directory)", path);
        if (!txt) continue;
        cJSON *r = cJSON_Parse(txt);
        free(txt);
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "features");
        int q = (int)feat(f, "shadow_resolution", "default", 2), qmax = (int)feat(f, "shadow_resolution", "max", (float)q);
        int c = (int)feat(f, "shadow_cascades", "default", 4);
        // The distance is locked for every level (docs/SETTINGS_AUDIT.md): the tiers trade resolution / cascades only.
        float d = SHADOW_LOCKED_DISTANCE, dmax = d;
        check_table(levels[i], "default", q, c, d, min_tpm[i]);
        check_table(levels[i], "max", qmax, c, dmax, min_tpm[i] * 0.4f);   // max distance at the max resolution
        // The pure tier table (shadow_settings.c) equals the JSON defaults.
        QualityShadowSettings t;
        shs_quality_for_tier(&t, i);
        CHECK(t.mode == (ShadowsMode)(int)feat(f, "shadows", "default", -1) && (int)t.resolution == q && t.cascades == c && fabsf(t.distance - d) < 1e-3f &&
                  (int)t.soft_quality == (int)feat(f, "soft_shadow_quality", "default", -1) && (int)t.projection == (int)feat(f, "shadow_projection", "default", -1),
              "%s: shs_quality_for_tier differs from the JSON defaults", levels[i]);
        // era overrides of the shadow entries
        const cJSON *eras = cJSON_GetObjectItemCaseSensitive(r, "eras");
        for (const cJSON *e = eras ? eras->child : NULL; e; e = e->next) {
            int eq = (int)feat(e, "shadow_resolution", "default", (float)q), ec = (int)feat(e, "shadow_cascades", "default", (float)c);
            float ed = d;
            char v[32];
            snprintf(v, sizeof(v), "era %s", e->string);
            check_table(levels[i], v, eq, ec, ed, min_tpm[i] * 0.8f);   // an era may trade a little density for reach
        }
        cJSON_Delete(r);
    }
    CHECK(shs_res_texels(0, SHADOW_LIGHT_DIRECTIONAL) == 512 && shs_res_texels(3, SHADOW_LIGHT_DIRECTIONAL) == 4096, "directional tiers 512 .. 4096");
    CHECK(csm_vm_texels(512) == 1024 && csm_vm_texels(2048) == 2048 && csm_vm_texels(4096) == 2048, "viewmodel texels follow the sun within 1024 .. 2048");
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
            CsmParams p = { 3, 2048, 105.0f, fovs[fi], aspects[ai], NULL };
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

// ------------------------------------------------------------------ a turning light (the sun moving every frame)

// The light turns a little every frame (docs/DAYNIGHT.md "Continuity": the direction is exact,
// never held): the sub-texel phase of a fixed point near the camera must drift slowly. About the
// world origin a camera 2 km out sees it jump (|X| x d theta: many texels per frame); about the
// camera-near pivot it moves (|X - pivot| x d theta) a small fraction of a texel.
static float phase_drift(vec3 cam, vec3 X, vec3 pivot, bool use_pivot, float dtheta, int frames, u32 tile, float radius) {
    float worst = 0, fx0 = 0, fy0 = 0;
    for (int fr = 0; fr <= frames; fr++) {
        float az = 2.9f + fr * dtheta, el = 0.6f;
        vec3 L = v3(cosf(el) * sinf(az), sinf(el), cosf(el) * cosf(az));
        mat4 lv = use_pivot ? csm_light_view_at(L, pivot) : csm_light_view(L);
        CsmTile t;
        csm_fit(&lv, cam, radius, tile, radius + 2.0f, &t);
        float fy, fx = frac_texel(&t, X, tile, &fy);
        if (fr > 0) {
            float dx = fabsf(fx - fx0), dy = fabsf(fy - fy0);
            dx = fminf(dx, 1.0f - dx);
            dy = fminf(dy, 1.0f - dy);
            worst = fmaxf(worst, fmaxf(dx, dy));
        }
        fx0 = fx, fy0 = fy;
    }
    return worst;
}

static void test_turning_light(void) {
    printf("shadow_csm: a turning light (camera-near pivot, the far-tile hold rule)\n");
    const u32 tile = 2048;
    const float radius = 6.5f, texel = 2.0f * radius / (float)tile;   // ~ cascade 0 at High
    const float day = 6.6e-5f;   // rad per frame: the sun of the default day (24 min) at 60 fps
    vec3 cams[2] = { v3(80.0f, 1.7f, 40.0f), v3(1800.0f, 30.0f, -900.0f) };
    for (int i = 0; i < 2; i++) {
        vec3 cam = cams[i], X = v3_add(cam, v3(2.0f, -1.7f, -4.0f));
        vec3 piv = csm_pivot(v3(0, 0, 0), false, cam, CSM_PIVOT_CELL);
        float origin = phase_drift(cam, X, piv, false, day, 30, tile, radius);
        float pivot = phase_drift(cam, X, piv, true, day, 30, tile, radius);
        printf("  camera %.0f m from the origin: per-frame phase drift of a point 4.8 m away: origin pivot %.3f texel, camera pivot %.4f texel (texel %.1f mm)\n",
               v3_len(cam), origin, pivot, texel * 1000.0f);
        // 2 km out the float world coordinates themselves carry ~0.1 texel of noise (the shaders' too).
        CHECK(pivot < (i == 0 ? 0.1f : 0.2f) && pivot < 0.5f * origin, "camera pivot: %.4f texel per frame at the default day (origin %.3f)", pivot, origin);
        // The pivot's worst case: the camera a whole cell away from it on every axis (|X - P| ~ 25 m:
        // a smooth 0.25 texel per frame at this texel).
        vec3 far_piv = v3_sub(cam, v3(CSM_PIVOT_CELL, CSM_PIVOT_CELL * 0.5f, CSM_PIVOT_CELL));
        float edge = phase_drift(cam, X, far_piv, true, day, 30, tile, radius);
        CHECK(edge < 0.4f, "camera at the pivot cell's edge: %.4f texel per frame", edge);
    }
    {
        vec3 cam = cams[1], X = v3_add(cam, v3(2.0f, -1.7f, -4.0f));
        float origin = phase_drift(cam, X, v3(0, 0, 0), false, day, 30, tile, radius);
        CHECK(origin > 0.25f, "2 km out the origin pivot re-phases the edges every frame (%.3f texel): the reason for the pivot", origin);
    }
    // The pivot holds while the camera stays within the cell, then jumps onto it (rounded to 1 m).
    vec3 pv = csm_pivot(v3(0, 0, 0), false, v3(10.4f, 2.0f, -3.6f), CSM_PIVOT_CELL);
    CHECK(pv.x == 10.0f && pv.y == 2.0f && pv.z == -4.0f, "pivot placed at (%.1f %.1f %.1f)", pv.x, pv.y, pv.z);
    vec3 pv2 = csm_pivot(pv, true, v3(10.4f + CSM_PIVOT_CELL - 0.5f, 2.0f, -3.6f), CSM_PIVOT_CELL);
    CHECK(pv2.x == pv.x, "pivot kept inside the cell (%.1f)", pv2.x);
    vec3 pv3 = csm_pivot(pv, true, v3(10.4f + CSM_PIVOT_CELL + 0.5f, 2.0f, -3.6f), CSM_PIVOT_CELL);
    CHECK(pv3.x == roundf(10.4f + CSM_PIVOT_CELL + 0.5f), "pivot moved past the cell (%.1f)", pv3.x);
    // The far-tile hold rule (PF-0028): the default day keeps a far tile for a frame above ~8 deg,
    // a time-lapse (time_scale 60: 4e-3 rad per frame) never, and it never allows over 0.02 deg.
    float c2 = 0.062f;   // Modern cascade 2 texel (m)
    float hi = csm_hold_max_turn(c2, 30.0f * DG_DEG2RAD), lo = csm_hold_max_turn(c2, 5.0f * DG_DEG2RAD);
    printf("  hold: cascade 2 (%.1f cm) may lag %.4f deg at 30 deg, %.4f deg at 5 deg; the default day turns %.4f deg per frame, time_scale 60 %.3f\n", c2 * 100.0f,
           hi * DG_RAD2DEG, lo * DG_RAD2DEG, day * DG_RAD2DEG, 4e-3f * DG_RAD2DEG);
    CHECK(hi > day && lo < day, "the default day holds at 30 deg, not at 5 deg");
    CHECK(hi < 4e-3f, "time_scale 60 redraws");
    CHECK(csm_hold_max_turn(10.0f, 1.2f) <= CSM_HOLD_MAX_TURN_DEG * DG_DEG2RAD * 1.0001f, "capped at %.2f deg", CSM_HOLD_MAX_TURN_DEG);
    float prev = 0;
    bool mono = true;
    for (float e = 0.0f; e < 1.5f; e += 0.01f) {
        float m = csm_hold_max_turn(c2, e);
        if (m < prev - 1e-9f) mono = false;
        prev = m;
    }
    CHECK(mono, "the hold allowance grows with the elevation");
}

// ------------------------------------------------------------------ receiver volume (PF-0006)

static bool rcv_inside(const vec4 *pl, int n, vec3 c, float r) {
    for (int i = 0; i < n; i++)
        if (pl[i].x * c.x + pl[i].y * c.y + pl[i].z * c.z + pl[i].w < -r) return false;
    return true;
}

// Every caster that can shadow a receiver of the cascade (anywhere towards the light from it, within
// the filter margin sideways) is inside the receiver planes; boxes far from the view sector are culled.
static void test_receivers(void) {
    printf("shadow_csm: receiver volume (light-extruded view slice) keeps every caster of a receiver\n");
    CsmParams p = { 4, 2048, 150.0f, 70.0f, 16.0f / 9.0f };
    CsmLayout l;
    csm_layout(&p, &l);
    float tv = tanf(35.0f * DG_DEG2RAD), th = tv * 16.0f / 9.0f;
    int miss = 0, n = 0, culled = 0, tested = 0, degenerate = 0;
    for (int it = 0; it < 300; it++) {
        float elev = (2.0f + frand() * 87.0f) * DG_DEG2RAD, az = frand() * 2.0f * DG_PI;
        vec3 L = v3(cosf(elev) * sinf(az), sinf(elev), cosf(elev) * cosf(az));
        mat4 lv = csm_light_view(L);
        vec3 cam = v3((frand() - 0.5f) * 500.0f, frand() * 20.0f, (frand() - 0.5f) * 500.0f), f, r, u;
        cam_basis(frand() * 6.28f, (frand() - 0.5f) * 2.5f, &f, &r, &u);
        for (int k = 0; k < 3; k++) {
            CsmTile t;
            csm_fit(&lv, v3_madd(cam, f, l.ahead[k]), l.radius[k], p.tile, l.radius[k] + 2.0f, &t);
            vec3 pts[16];
            for (int i = 0; i < 8; i++) {
                float d = (i & 4) ? l.split[k] : 0.05f, sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f;
                pts[i] = v3_add(cam, v3_add(v3_scale(f, d), v3_add(v3_scale(r, sx * th * d), v3_scale(u, sy * tv * d))));
                pts[8 + i] = v3_add(cam, v3((i & 1) ? 3.0f : -3.0f, (i & 2) ? 3.0f : -3.0f, (i & 4) ? 3.0f : -3.0f));
            }
            float margin = 0.5f + 52.0f * t.texel_m;
            vec4 pl[CSM_RCV_MAX];
            int np = csm_receiver_planes(&t.vp, pts, 16, margin, pl);
            if (np == 0) { degenerate++; continue; }
            vec3 lx = v3_norm(v3_cross(L, fabsf(L.y) < 0.99f ? v3(0, 1, 0) : v3(1, 0, 0))), ly = v3_cross(L, lx);
            for (int s = 0; s < 40; s++) {
                float sx = (frand() * 2 - 1) * th, sy = (frand() * 2 - 1) * tv;
                vec3 dir = v3_norm(v3_add(f, v3_add(v3_scale(r, sx), v3_scale(u, sy))));
                vec3 rc = v3_madd(cam, dir, l.split[k] * sqrtf(frand()));
                if (s < 4) rc = v3_add(cam, v3((frand() - 0.5f) * 5.0f, (frand() - 0.5f) * 5.0f, (frand() - 0.5f) * 5.0f));   // the rig
                // a caster up to 1 km towards the light, up to 0.95 x the margin sideways
                float a = frand() * 6.2832f, w = 0.95f * margin * sqrtf(frand());
                vec3 c = v3_madd(v3_madd(v3_madd(rc, L, frand() * 1000.0f), lx, w * cosf(a)), ly, w * sinf(a));
                n++;
                if (!rcv_inside(pl, np, c, 0.0f)) miss++;
            }
            for (int s = 0; s < 200; s++) {   // points of the tile's square: how many the volume culls
                vec3 c = v3_madd(v3_madd(t.center, lx, (frand() * 2 - 1) * t.radius), ly, (frand() * 2 - 1) * t.radius);
                tested++;
                if (!rcv_inside(pl, np, c, 0.0f)) culled++;
            }
        }
    }
    printf("  %d receivers' casters, %d outside; %.0f %% of the tile area culled, %d degenerate\n", n, miss, 100.0f * culled / DG_MAX(tested, 1), degenerate);
    CHECK(miss == 0, "%d of %d casters of a receiver culled", miss, n);
    CHECK(culled > tested / 5, "the receiver volume culls only %d of %d tile points", culled, tested);
    CHECK(degenerate < 10, "%d degenerate receiver hulls", degenerate);
    // Too few / too many points: no planes (cull nothing).
    vec4 pl[CSM_RCV_MAX];
    mat4 id = mat4_identity();
    vec3 two[2] = { v3(0, 0, 0), v3(1, 0, 0) };
    CHECK(csm_receiver_planes(&id, two, 2, 0.0f, pl) == 0, "two points gave planes");
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

// Close Fit (Unity): denser than a Stable Fit sphere over the same receivers, but the tile follows the camera rotation, so a
// fixed point's fractional texel position drifts while turning (swimming); Stable Fit does not.
static void test_close_fit(void) {
    printf("shadow_csm: Close Fit vs Stable Fit\n");
    float splits[4] = { 13.4f, 40.0f, 93.4f, 200.0f };
    CsmParams p = { 4, 2048, 200.0f, 70.0f, 16.0f / 9.0f, splits };
    CsmLayout l;
    csm_layout(&p, &l);
    mat4 lv = csm_light_view(v3_norm(v3(0.3f, 0.8f, -0.4f)));
    vec3 cam = v3(5.0f, 1.7f, -3.0f), f, r, u;
    float tv = tanf(35.0f * DG_DEG2RAD), th = tv * 16.0f / 9.0f;
    float dense = 0.0f, drift_close = 0.0f, drift_stable = 0.0f;
    vec3 P = v3(9.0f, 0.0f, -7.0f);
    float fx0c = 0, fy0c = 0, fx0s = 0, fy0s = 0;
    for (int fr = 0; fr < 10; fr++) {
        cam_basis(0.6f + fr * 0.3f * DG_DEG2RAD, -0.2f, &f, &r, &u);
        CsmTile tc, ts;
        csm_fit_close(&lv, cam, f, r, u, tv, th, 0.0f, splits[0], p.tile, CSM_REACH, &tc);
        // Stable Fit holding the same receivers: the sector sphere around the depth slice's corners
        // (Close Fit selects by view depth, Stable Fit by distance)
        float sa, sr, tcn = sqrtf(tv * tv + th * th);
        csm_sector_sphere(splits[0] * sqrtf(1.0f + tcn * tcn), tcn, &sa, &sr);
        csm_fit(&lv, v3_madd(cam, f, sa), sr, p.tile, sr + 2.0f, &ts);
        if (fr == 0) dense = ts.texel_m / tc.texel_m;
        float fyc, fxc = frac_texel(&tc, P, p.tile, &fyc), fys, fxs = frac_texel(&ts, P, p.tile, &fys);
        if (fr == 0) { fx0c = fxc; fy0c = fyc; fx0s = fxs; fy0s = fys; continue; }
        float a = fabsf(fxc - fx0c), b = fabsf(fyc - fy0c);
        drift_close = fmaxf(drift_close, fmaxf(fminf(a, 1 - a), fminf(b, 1 - b)));
        a = fabsf(fxs - fx0s); b = fabsf(fys - fy0s);
        drift_stable = fmaxf(drift_stable, fmaxf(fminf(a, 1 - a), fminf(b, 1 - b)));
    }
    printf("  cascade 0: Close Fit %.2fx the texel density of Stable Fit; drift while turning: close %.3f, stable %.4f texel\n", dense, drift_close, drift_stable);
    CHECK(dense > 1.02f, "Close Fit not denser (%.2fx)", dense);
    CHECK(drift_close > 0.05f, "Close Fit did not swim (%.3f)", drift_close);
    CHECK(drift_stable < 0.02f, "Stable Fit swims (%.4f)", drift_stable);
}

int main(void) {
    test_tables();
    test_fit();
    test_snapping();
    test_turning_light();
    test_casters();
    test_receivers();
    test_selection();
    test_bias_kernel();
    test_viewmodel();
    test_close_fit();
    printf("test_shadow_csm: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
