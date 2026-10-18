// Classic instance cull arithmetic (src/render/classic_cull_math.h, vk_classic.c cull_instance):
//   * the factored functions give the same bits as an in-test copy of the cull_instance they replaced
//     (every ClInst word and key of every part), on 1M random instances against random main /
//     scope / sun cascade / local light views, docs/LOD.md and legacy, with the receiver volume
//     (PF-0006) and the caster class limits (PF-0013);
//   * the one intended difference: a part whose kept interval quantizes empty is no longer emitted
//     (the old code drew it whole when both ends rounded to 0, the lod_iv_pack sentinel, or as an
//     empty [a, a)); every other part is unchanged;
//   * the parts of every kept instance cover each of the 256 dither thresholds exactly once.
#include "render/classic_cull_math.h"
#include "core/engine.h"
#include "core/cvar.h"
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

// vk_classic.c's instance record and key layout
#define CL_IMPOSTOR 7u
#define CL_KEY_FADE 8u
#define CL_KEY_SHIFT 4
#define CL_IMPOSTOR_TRIS 256
typedef struct ClInst { float pos[4], rot[4], misc[4]; } ClInst;

// The cull's view of a ClMesh (vk_classic.c): the fields cull_instance read.
typedef struct OldMesh {
    u32 lod_count;
    float lod_error[VGEO_MAX_LODS];
    u32 coarse_tris;
    bool imp_set;
} OldMesh;

// vgeo_foliage_alpha (vgeo/foliage_support.c, the legacy foliage fade): both sides call this copy.
static float legacy_foliage_alpha(const VgMesh *m, u32 fp, float dist, float density) {
    if (!(m->fflags & VGMF_FOLIAGE)) return 1.0f;
    float t = density;
    if (m->fade_end > m->fade_start) t *= clc_clamp01((m->fade_end - dist) / (m->fade_end - m->fade_start));
    float h = ((float)VG_INST_HASH(fp) + 0.5f) * (0.9375f / 256.0f);
    return clc_clamp01((t - h) * 16.0f);
}

static void emit_part(ClInst *out, u32 *key, u32 *n, u32 mesh, u32 lod, const float p[3], const float q[4], float sc, float phase, float alpha, float iv) {
    ClInst *o = &out[*n];
    o->pos[0] = p[0], o->pos[1] = p[1], o->pos[2] = p[2], o->pos[3] = sc;
    memcpy(o->rot, q, 16);
    o->misc[0] = phase;
    o->misc[1] = alpha;
    o->misc[2] = iv;
    float role = iv != 0.0f ? (fmodf(iv, 512.0f) == 0.0f ? 1.0f : 2.0f) : (alpha < 1.0f ? 3.0f : 0.0f);
    o->misc[3] = (float)(lod == CL_IMPOSTOR ? 4u : lod) + 8.0f * role;
    bool fading = iv != 0.0f || alpha < 1.0f;
    key[(*n)++] = mesh << CL_KEY_SHIFT | (fading ? CL_KEY_FADE : 0u) | lod;
}

// ------------------------------------------------------------------ the code before the refactor
// vk_classic.c (1baaf08) sphere_in, clamp01f, cl_foliage_alpha and cull_instance from the flag tests
// on, verbatim but for the decoded instance passed in (p, q, sc, fp, mesh) and ClMesh -> OldMesh.

static bool old_sphere_in(const vec4 *pl, int n, vec3 c, float r) {
    for (int k = 0; k < n; k++)
        if (pl[k].x * c.x + pl[k].y * c.y + pl[k].z * c.z + pl[k].w < -r) return false;
    return true;
}

static float clamp01f(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

static float cl_foliage_alpha(const ClCull *v, const VgMesh *m, u32 fp, float dist) {
    if (!(m->fflags & VGMF_FOLIAGE)) return 1.0f;
    float u = 1.0f;
    if (m->fade_end > m->fade_start) {
        float s0 = m->fade_start * v->fol_k, s1 = s0 + (m->fade_end - m->fade_start) * v->fol_band_k;
        u = clamp01f((s1 - dist) / fmaxf(s1 - s0, 1e-3f));
        u = u * u * (3.0f - 2.0f * u);
    }
    return lod_foliage_alpha(v->density, u, ((float)VG_INST_HASH(fp) + 0.5f) / 256.0f, v->fol_instance);
}

static void old_cull(const ClCull *v, const VgMesh *m, const OldMesh *cm, u32 mesh, const float p[3], const float q[4], float sc, u32 fp, ClInst *out, u32 *key, u32 *n) {
    u32 iflags = fp >> 16;
    if (iflags & VGI_SKINNED) return;
    if (v->shadow ? (iflags & VGI_NO_CAST) != 0 : (iflags & VGI_SHADOWS_ONLY) != 0) return;
    if (v->shadow && v->layers && !((v->layers >> VGI_LAYER(iflags)) & 1u)) return;
    quat qq = { q[0], q[1], q[2], q[3] };
    vec3 c = v3_add(v3(p[0], p[1], p[2]), quat_rotate(qq, v3(m->center[0] * sc, m->center[1] * sc, m->center[2] * sc)));
    float r = m->radius * sc + m->max_wpo * fmaxf(sc, 1.0f);   // vg_wpo_margin
    if (v->sphere.w > 0) {
        float rr = r + v->sphere.w;
        if (v3_len2(v3_sub(c, v3(v->sphere.x, v->sphere.y, v->sphere.z))) > rr * rr) return;
    }
    if (!old_sphere_in(v->planes, 6, c, r)) return;
    if (v->nrcv && !old_sphere_in(v->rcv, v->nrcv, c, r)) return;
    float dx = p[0] - v->eye.x, dy = p[1] - v->eye.y, dz = p[2] - v->eye.z, dist = sqrtf(dx * dx + dy * dy + dz * dz);
    float casc_a = 1.0f;
    if (v->scc_on) {
        u32 sc = VGMF_SHADOW_CLASS(m->fflags);
        casc_a = scc_alpha(&v->scc, sc < SCC_COUNT ? (int)sc : SCC_NONE, dist);
        if (casc_a <= 0.0f) return;
    }
    if (!v->legacy) {
        float alpha = casc_a, shrink = 1.0f;
        if (v->shadow && m->shadow_max_dist > 0) {
            float sd = m->shadow_max_dist * v->shadow_k;
            if (dist > sd) return;
            alpha *= v->shadow_band > 0 ? clamp01f((sd - dist) / (sd * v->shadow_band)) : 1.0f;
        }
        if (m->fflags & VGMF_FOLIAGE) {
            float fa = cl_foliage_alpha(v, m, fp, dist);
            if (fa <= 0) return;
            alpha *= fa;
            shrink = v->fol_shrink + (1.0f - v->fol_shrink) * fa;
        }
        if (alpha <= 0) return;
        float dl = fmaxf(v3_dist(c, v->lod_eye), v->znear), dcl = fmaxf(dl - r, v->znear);
        float imp_px = cm->coarse_tris >= CL_IMPOSTOR_TRIS ? v->impostor_px : v->small_impostor_px;
        u32 ks = m->impostor_grid >> 16;
        if (ks) imp_px *= (float)ks / 16.0f;
        float iw = 0;
        if (!v->shadow && cm->imp_set && imp_px > 0) {
            float size = 2.0f * m->radius * sc * v->px_scale / dl;
            if (size < imp_px) iw = lod_band_weight(log2f(imp_px / fmaxf(size, 1e-6f)), v->imp_band);
        }
        float k = v->ortho ? sc * v->px_scale : sc * v->px_scale / dcl, cw = 0;
        int first = v->era2010 && cm->lod_count > 1 ? 1 : 0;
        u32 lod = iw >= 1.0f ? 0u : (u32)lod_pick_band(cm->lod_error, (int)cm->lod_count, first, k, v->lod_px, v->band, &cw);
        float phase = (float)VG_INST_PHASE(fp) / 256.0f, sd = sc * shrink;
        float mid = iw + (1.0f - iw) * cw;
        if (iw > 0) emit_part(out, key, n, mesh, CL_IMPOSTOR, p, q, sd, phase, alpha, iw < 1.0f ? lod_iv_pack(0, iw) : 0.0f);
        if (iw < 1.0f) {
            if (cw > 0) emit_part(out, key, n, mesh, lod + 1, p, q, sd, phase, alpha, cw < 1.0f || iw > 0 ? lod_iv_pack(iw, mid) : 0.0f);
            if (mid < 1.0f) emit_part(out, key, n, mesh, lod, p, q, sd, phase, alpha, mid > 0 ? lod_iv_pack(mid, 1.0f) : 0.0f);
        }
        return;
    }
    if (v->shadow && m->shadow_max_dist > 0 && dist > m->shadow_max_dist) return;
    float alpha = (m->fflags & VGMF_FOLIAGE) ? legacy_foliage_alpha(m, fp, dist, v->density) : 1.0f;
    if (alpha <= 0) return;
    u32 lod;
    float dc = fmaxf(v3_dist(c, v->lod_eye) - r, v->znear);
    float imp_px = cm->coarse_tris >= CL_IMPOSTOR_TRIS ? v->impostor_px : v->small_impostor_px;
    u32 ks = m->impostor_grid >> 16;
    if (ks) imp_px *= (float)ks / 16.0f;
    if (!v->shadow && cm->imp_set && imp_px > 0 && 2.0f * m->radius * sc * v->px_scale / fmaxf(v3_dist(c, v->lod_eye), v->znear) < imp_px) {
        lod = CL_IMPOSTOR;
    } else {
        lod = v->era2010 && cm->lod_count > 1 ? 1 : 0;
        float k = v->ortho ? sc * v->px_scale : sc * v->px_scale / dc;
        while (lod + 1 < cm->lod_count && cm->lod_error[lod + 1] * k <= v->lod_px) lod++;
    }
    ClInst *o = &out[*n];
    o->pos[0] = p[0], o->pos[1] = p[1], o->pos[2] = p[2], o->pos[3] = sc;
    memcpy(o->rot, q, 16);
    o->misc[0] = (float)VG_INST_PHASE(fp) / 256.0f;
    o->misc[1] = alpha;
    o->misc[2] = o->misc[3] = 0;
    key[(*n)++] = mesh << CL_KEY_SHIFT | lod;
}

// ------------------------------------------------------------------ the code now (vk_classic.c cull_instance)

static void new_cull(const ClCull *v, const VgMesh *m, const OldMesh *cm, u32 mesh, const float p[3], const float q[4], float sc, u32 fp, ClInst *out, u32 *key, u32 *n) {
    if (clc_flags_out(v, fp >> 16)) return;
    const ClcChain ch = { cm->lod_error, cm->lod_count, cm->coarse_tris, cm->imp_set };
    if (!v->legacy) {
        ClcParts o;
        u32 np = clc_cull(v, m, &ch, p, q, sc, fp, &o);
        for (u32 i = 0; i < np; i++) emit_part(out, key, n, mesh, o.lod[i], p, q, o.scale, o.phase, o.alpha, o.iv[i]);
        return;
    }
    ClcFront f;
    if (!clc_front(v, m, p, q, sc, &f)) return;
    if (v->shadow && m->shadow_max_dist > 0 && f.dist > m->shadow_max_dist) return;
    float alpha = (m->fflags & VGMF_FOLIAGE) ? legacy_foliage_alpha(m, fp, f.dist, v->density) : 1.0f;
    if (alpha <= 0) return;
    u32 lod = clc_legacy_lod(v, m, &ch, &f, sc);
    ClInst *o = &out[*n];
    o->pos[0] = p[0], o->pos[1] = p[1], o->pos[2] = p[2], o->pos[3] = sc;
    memcpy(o->rot, q, 16);
    o->misc[0] = (float)VG_INST_PHASE(fp) / 256.0f;
    o->misc[1] = alpha;
    o->misc[2] = o->misc[3] = 0;
    key[(*n)++] = mesh << CL_KEY_SHIFT | lod;
}

// ------------------------------------------------------------------ random scenes

static u32 g_seed = 0x2545F491u;
static u32 rnd_u32(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
static float rnd01(void) { return (float)(rnd_u32() >> 8) / 16777216.0f; }
static float rnd_range(float a, float b) { return a + (b - a) * rnd01(); }
static bool rnd_chance(float p) { return rnd01() < p; }

// vk_classic.c planes_of
static void planes_of(mat4 m, vec4 out[6]) {
    float *a = m.m;
    vec4 r0 = v4(a[0], a[4], a[8], a[12]), r1 = v4(a[1], a[5], a[9], a[13]), r2 = v4(a[2], a[6], a[10], a[14]), r3 = v4(a[3], a[7], a[11], a[15]);
    out[0] = v4(r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w);
    out[1] = v4(r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w);
    out[2] = v4(r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w);
    out[3] = v4(r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w);
    out[4] = r2;
    out[5] = v4(r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w);
    for (int i = 0; i < 6; i++) {
        float l = sqrtf(out[i].x * out[i].x + out[i].y * out[i].y + out[i].z * out[i].z);
        out[i] = v4(out[i].x / l, out[i].y / l, out[i].z / l, out[i].w / l);
    }
}

#define NMESH 24
static VgMesh g_mesh[NMESH];
static OldMesh g_cm[NMESH];

static void random_meshes(void) {
    for (int i = 0; i < NMESH; i++) {
        VgMesh *m = &g_mesh[i];
        OldMesh *cm = &g_cm[i];
        memset(m, 0, sizeof(*m));
        memset(cm, 0, sizeof(*cm));
        bool tree = rnd_chance(0.4f);
        m->radius = tree ? rnd_range(3.0f, 15.0f) : rnd_range(0.1f, 2.0f);
        for (int a = 0; a < 3; a++) m->center[a] = rnd_range(-0.3f, 0.3f) * m->radius;
        m->center[1] = fabsf(m->center[1]) + 0.4f * m->radius;
        m->max_wpo = rnd_chance(0.6f) ? rnd_range(0.0f, 0.6f) : 0.0f;
        m->fflags = (rnd_chance(0.7f) ? VGMF_FOLIAGE : 0u) | (rnd_u32() & (VGMF_MASKED | VGMF_TWO_SIDED | VGMF_DITHER)) | ((rnd_u32() % 5u) << VGMF_SHADOW_CLASS_SHIFT);
        if (rnd_chance(0.75f)) {
            m->fade_start = rnd_range(10.0f, 120.0f);
            m->fade_end = m->fade_start + rnd_range(0.0f, 80.0f);
        }
        if (rnd_chance(0.1f)) m->fade_end = m->fade_start;   // no fade range
        m->shadow_max_dist = rnd_chance(0.6f) ? rnd_range(10.0f, 120.0f) : 0.0f;
        u32 ks = rnd_chance(0.5f) ? 0u : 8u + rnd_u32() % 25u;
        m->impostor_grid = 8u | ks << 16;
        cm->lod_count = 1u + rnd_u32() % VGEO_MAX_LODS;
        float e = 0.0f;
        for (u32 k = 0; k < cm->lod_count; k++) {
            cm->lod_error[k] = e;
            e = (e > 0 ? e : rnd_range(0.001f, 0.01f)) * rnd_range(2.0f, 6.0f);
        }
        cm->coarse_tris = tree ? 300u + rnd_u32() % 2000u : 4u + rnd_u32() % 200u;
        cm->imp_set = rnd_chance(0.8f);
    }
}

enum { VIEW_MAIN, VIEW_PIP, VIEW_CASCADE, VIEW_LOCAL };
static vec3 g_fwd;                  // the current view's direction: most instances are placed in front

static void random_view(ClCull *v, int kind) {
    memset(v, 0, sizeof(*v));
    vec3 eye = v3(rnd_range(-50, 50), rnd_range(1, 30), rnd_range(-50, 50));
    float yaw = rnd_range(-3.14f, 3.14f), pitch = rnd_range(-0.3f, 0.1f);
    vec3 fwd = dir_from_angles(yaw, pitch);
    g_fwd = fwd;
    v->eye = v->lod_eye = eye;
    v->density = rnd_chance(0.3f) ? 1.0f : rnd_range(0.3f, 1.0f);
    v->era2010 = rnd_chance(0.15f);
    v->legacy = rnd_chance(0.2f);
    v->lod_px = rnd_range(0.5f, 3.0f);
    v->impostor_px = rnd_range(48.0f, 200.0f);
    v->small_impostor_px = rnd_chance(0.2f) ? 0.0f : rnd_range(6.0f, 40.0f);
    v->band = rnd_chance(0.1f) ? 0.0f : rnd_range(0.15f, 0.6f);
    v->imp_band = rnd_chance(0.1f) ? 0.0f : rnd_range(0.15f, 0.6f);
    v->fol_k = rnd_range(1.0f, 2.0f);
    v->fol_band_k = rnd_range(1.0f, 2.0f);
    v->fol_instance = rnd_range(0.05f, 1.0f);
    v->fol_shrink = rnd_range(0.3f, 1.0f);
    v->shadow_k = rnd_range(1.0f, 2.0f);
    v->shadow_band = rnd_chance(0.1f) ? 0.0f : rnd_range(0.05f, 0.3f);
    v->fade_lim_k = fmaxf(v->fol_k, v->fol_band_k);
    v->shadow_lim_k = v->shadow_k;
    if (kind == VIEW_MAIN || kind == VIEW_PIP) {
        float fov = kind == VIEW_PIP ? rnd_range(2.0f, 15.0f) : rnd_range(50.0f, 100.0f);
        mat4 vp = mat4_mul(mat4_perspective(fov * DG_DEG2RAD, 16.0f / 9.0f, 0.1f, 2000.0f), mat4_look_at(eye, v3_add(eye, fwd), v3(0, 1, 0)));
        planes_of(vp, v->planes);
        v->px_scale = 1080.0f / (2.0f * tanf(fov * DG_DEG2RAD * 0.5f));
        v->znear = kind == VIEW_PIP ? 0.3f : 0.1f;
    } else if (kind == VIEW_CASCADE) {
        float rad = rnd_range(10.0f, 160.0f);
        vec3 ld = v3_norm(v3(rnd_range(-1, 1), rnd_range(-1.0f, -0.2f), rnd_range(-1, 1)));
        vec3 c = v3_add(eye, v3_scale(fwd, rad * 0.7f));
        mat4 vp = mat4_mul(mat4_ortho(-rad, rad, -rad, rad, -500.0f, 500.0f), mat4_look_at(v3_sub(c, ld), c, fabsf(ld.y) > 0.99f ? v3(1, 0, 0) : v3(0, 1, 0)));
        planes_of(vp, v->planes);
        v->ortho = v->shadow = true;
        v->px_scale = 2048.0f / (2.0f * rad);
        v->znear = 0.1f;
        v->layers = rnd_chance(0.3f) ? (rnd_u32() & 0xFFu) | 0x100u : 0u;
        if (rnd_chance(0.6f)) {   // a receiver volume: planes around the cascade's centre
            v->nrcv = 3 + (int)(rnd_u32() % 6u);
            for (int i = 0; i < v->nrcv; i++) {
                vec3 nrm = v3_norm(v3(rnd_range(-1, 1), rnd_range(-0.3f, 0.3f), rnd_range(-1, 1)));
                float w = rnd_range(0.4f, 1.2f) * rad - v3_dot(nrm, c);
                v->rcv[i] = v4(nrm.x, nrm.y, nrm.z, w);
            }
        }
        if (rnd_chance(0.7f)) {   // PF-0013 caster class limits
            v->scc_on = true;
            v->scc.allow = rnd_u32() & ((1u << SCC_COUNT) - 1u);
            for (int k = 0; k < SCC_COUNT; k++) {
                v->scc.end_m[k] = rnd_chance(0.3f) ? 0.0f : rnd_range(10.0f, 200.0f);
                v->scc.fade_m[k] = rnd_chance(0.2f) ? 0.0f : v->scc.end_m[k] * rnd_range(0.05f, 0.3f);
            }
        }
    } else {   // point / spot light tile: the light's sphere, legacy LOD from the light
        mat4 vp = mat4_mul(mat4_perspective(90.0f * DG_DEG2RAD, 1.0f, 0.05f, 60.0f), mat4_look_at(eye, v3_add(eye, fwd), fabsf(fwd.y) > 0.99f ? v3(1, 0, 0) : v3(0, 1, 0)));
        planes_of(vp, v->planes);
        float rad = rnd_range(5.0f, 40.0f);
        v->sphere = v4(eye.x, eye.y, eye.z, rad);
        v->eye = v3(eye.x + rnd_range(-30, 30), eye.y, eye.z + rnd_range(-30, 30));   // the camera
        v->px_scale = 512.0f * 0.5f;
        v->lod_px = 2.0f;
        v->znear = 0.05f;
        v->shadow = true;
        v->legacy = true;
        v->layers = rnd_chance(0.3f) ? (rnd_u32() & 0xFFu) | 0x100u : 0u;
    }
}

static void random_instance(const ClCull *v, float p[3], float q[4], float *sc, u32 *fp, u32 *mesh) {
    // distance: uniform over a disc or log-uniform (every LOD / impostor band equally often); azimuth:
    // mostly in front of the view
    float d = rnd_chance(0.5f) ? (rnd_chance(0.5f) ? 60.0f : 320.0f) * sqrtf(rnd01()) : expf(rnd_range(0.7f, 6.0f));
    float a = atan2f(g_fwd.z, g_fwd.x) + (rnd_chance(0.75f) ? rnd_range(-0.5f, 0.5f) : rnd_range(-3.1416f, 3.1416f));
    vec3 e = v->sphere.w > 0 ? v3(v->sphere.x, v->sphere.y, v->sphere.z) : v->eye;
    p[0] = e.x + d * cosf(a), p[1] = rnd_range(-2.0f, 4.0f), p[2] = e.z + d * sinf(a);
    float x = rnd_range(-1, 1), y = rnd_range(-1, 1), z = rnd_range(-1, 1), w = rnd_range(-1, 1), l = sqrtf(x * x + y * y + z * z + w * w);
    if (l < 1e-3f) x = 0, y = 0, z = 0, w = 1, l = 1;
    q[0] = x / l, q[1] = y / l, q[2] = z / l, q[3] = w / l;
    *sc = rnd_chance(0.1f) ? rnd_range(0.05f, 1.0f) : rnd_range(0.5f, 2.5f);
    u32 flags = 0;
    if (rnd_chance(0.02f)) flags |= VGI_SKINNED;
    if (rnd_chance(0.05f)) flags |= VGI_NO_CAST;
    if (rnd_chance(0.05f)) flags |= VGI_SHADOWS_ONLY;
    flags |= (rnd_u32() % 8u) << VGI_LAYER_SHIFT;
    *fp = (rnd_u32() & 0xFFFFu) | flags << 16;
    *mesh = rnd_u32() % NMESH;
}

// An old part the new cull drops: its interval quantized empty ([a, a), or [0, 0) packed as the 0
// sentinel next to other parts: whole coverage alone is the only legitimate 0 of a docs/LOD.md list).
static bool old_part_empty(const ClInst *o, u32 n) {
    float iv = o->misc[2];
    if (iv == 0.0f) return n > 1;
    int p = (int)iv, a = p % 512, b = p / 512;
    return a >= b;
}

// Every threshold k (n = (k + 0.5) / 256) kept by exactly one part (shaders/classic_common.glsl
// cl_faded on the packed interval; 0 = all).
static bool covers_once(const ClInst *o, u32 n) {
    for (int k = 0; k < 256; k++) {
        float t = ((float)k + 0.5f) / 256.0f;
        int c = 0;
        for (u32 i = 0; i < n; i++) c += lod_iv_kept(o[i].misc[2], t);
        if (c != 1) return false;
    }
    return true;
}

static void test_bits(void) {
    random_meshes();
    const u32 instances = 1000000, per_view = 2000;
    u32 kept = 0, parts = 0, dropped = 0, dropped_sentinel = 0, mismatch = 0, cover_bad = 0, three = 0, imp = 0, legacy = 0;
    u32 shown = 0;
    ClCull v;
    int kinds[4] = { 0 };
    for (u32 i = 0; i < instances; i++) {
        if (i % per_view == 0) {
            int kind = (int)(rnd_u32() % 4u);
            kinds[kind]++;
            random_view(&v, kind);
        }
        float p[3], q[4], sc;
        u32 fp, mesh;
        random_instance(&v, p, q, &sc, &fp, &mesh);
        ClInst oo[CLC_PARTS], no[CLC_PARTS];
        u32 ok[CLC_PARTS], nk[CLC_PARTS], on = 0, nn = 0;
        memset(oo, 0, sizeof(oo));
        memset(no, 0, sizeof(no));
        old_cull(&v, &g_mesh[mesh], &g_cm[mesh], mesh, p, q, sc, fp, oo, ok, &on);
        new_cull(&v, &g_mesh[mesh], &g_cm[mesh], mesh, p, q, sc, fp, no, nk, &nn);
        // the old parts minus the empty ones, in order
        ClInst fo[CLC_PARTS];
        u32 fk[CLC_PARTS], fn = 0;
        for (u32 k = 0; k < on; k++) {
            if (!v.legacy && old_part_empty(&oo[k], on)) {
                dropped++;
                dropped_sentinel += oo[k].misc[2] == 0.0f;
                continue;
            }
            fo[fn] = oo[k], fk[fn++] = ok[k];
        }
        bool same = fn == nn;
        for (u32 k = 0; same && k < nn; k++) same = memcmp(&fo[k], &no[k], sizeof(ClInst)) == 0 && fk[k] == nk[k];
        if (!same) {
            mismatch++;
            if (shown++ < 5) printf("  mismatch: instance %u view legacy %d shadow %d: old %u parts (%u kept), new %u\n", i, v.legacy, v.shadow, on, fn, nn);
        }
        kept += nn > 0;
        parts += nn;
        three += nn == 3;
        legacy += v.legacy && nn > 0;
        for (u32 k = 0; k < nn; k++) imp += (nk[k] & 7u) == CL_IMPOSTOR;
        if (nn && !v.legacy && !covers_once(no, nn)) cover_bad++;
    }
    printf("test_classic_cull: %u instances (views: %d main, %d scope, %d cascade, %d local): %u kept, %u parts (%u three-part, %u impostor parts, %u legacy); "
           "%u empty old parts dropped (%u drawn whole by the 0 sentinel)\n",
           instances, kinds[0], kinds[1], kinds[2], kinds[3], kept, parts, three, imp, legacy, dropped, dropped_sentinel);
    CHECK(mismatch == 0, "%u instances differ from the old cull_instance (beyond the dropped empty parts)", mismatch);
    CHECK(cover_bad == 0, "%u kept instances whose parts do not cover every threshold exactly once", cover_bad);
    // the random scenes exercise every branch
    CHECK(kept > instances / 10 && three > 100 && imp > 1000 && legacy > 1000 && dropped > 0, "coverage of the random scenes: kept %u, three-part %u, impostor %u, legacy %u, dropped %u",
          kept, three, imp, legacy, dropped);
}

// The split itself near the sentinel: iw / cw just above 0 (the old code drew a whole extra part).
static void test_split_edges(void) {
    int bad = 0, empty = 0;
    for (int s = 1; s <= 4000; s++) {
        float t = (float)s * 1e-6f;
        const float pairs[4][2] = { { t, 0.0f }, { 0.0f, t }, { t, t }, { t, 0.5f } };
        for (int k = 0; k < 4; k++) {
            ClcParts o = { 0 };
            clc_split(&o, 1, pairs[k][0], pairs[k][1]);
            ClInst pi[CLC_PARTS];
            for (u32 i = 0; i < o.n; i++) {
                pi[i].misc[2] = o.iv[i];
                if (o.iv[i] != 0.0f ? lod_iv_empty(fmodf(o.iv[i], 512.0f) / 256.0f, floorf(o.iv[i] / 512.0f) / 256.0f) : o.n > 1) empty++;
            }
            if (!covers_once(pi, o.n)) bad++;
        }
    }
    CHECK(bad == 0, "split near 0: %d cases not covering every threshold exactly once", bad);
    CHECK(empty == 0, "split near 0: %d empty parts emitted", empty);
}

int main(void) {
    cvar_system_init();
    test_split_edges();
    test_bits();
    printf("test_classic_cull: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
