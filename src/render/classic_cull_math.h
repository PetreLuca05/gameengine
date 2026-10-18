// Classic instance cull arithmetic (docs/CLASSIC_RENDERER.md "Culling", docs/LOD.md): what the
// classic renderer decides for one instance against one view. The culling sphere and its tests (light
// sphere, frustum, receiver volume: PF-0006), the caster class's cascade limit (PF-0013), the shadow
// distance and foliage fades, the impostor share and the distance LOD, and the parts their
// cross-fades split the instance into.
//
// Pure static inline functions over plain values. vk_classic.c cull_instance calls them (with the CRT
// log2f / sqrtf), tests/test_classic_cull.c checks them bit for bit against the code they replaced,
// and the GPU cull's twin keeps their expression order operation for operation (every float
// `precise`). The pure helpers they call are part of that contract: lod_band_weight, lod_pick_band,
// lod_foliage_alpha, lod_iv_pack / lod_iv_empty (lod_fade.c) and scc_alpha (shadow_casters.c).
#ifndef DG_CLASSIC_CULL_MATH_H
#define DG_CLASSIC_CULL_MATH_H

#include "core/base.h"
#include "core/dgmath.h"
#include "lod_fade.h"
#include "shadow_casters.h"
#include "shadow_csm.h"
#include "vgeo/vgeo_gpu.h"

#define CLC_IMPOSTOR 7u              // part LOD of the impostor (vk_classic.c key slot CL_IMPOSTOR)
#define CLC_PARTS 3                  // parts of one instance at most (impostor, coarser, finer LOD)
#define CLC_IMPOSTOR_TRIS 256        // meshes this heavy (coarsest LOD) use the view's impostor_px

// What a view culls with: planes, the eye (fade / density / shadow distance are from the camera),
// LOD scale (pixels per metre of error at distance 1, or per metre flat for ortho views).
typedef struct ClCull {
    vec4 planes[6];
    vec3 eye, lod_eye;              // lod_eye: LOD distance origin (the light for local shadows)
    float px_scale, lod_px, impostor_px, small_impostor_px, density, znear;
    bool ortho, shadow, era2010;
    vec4 sphere;                    // local light shadows: casters must touch it (w > 0)
    // docs/LOD.md (legacy: the cuts from before): bands (octaves), foliage fade range / band scales,
    // instance fade width, shrink, shadow distance scale and fade fraction, whole-cell limit scales
    bool legacy;
    float band, imp_band, fol_k, fol_band_k, fol_instance, fol_shrink, shadow_k, shadow_band, fade_lim_k, shadow_lim_k;
    u32 layers;                     // shadow views: the light's culling mask (0 = every layer)
    vec4 rcv[CSM_RCV_MAX];          // sun cascades: the receiver volume (render.c, PF-0006), nrcv planes (0: none)
    int nrcv;
    bool scc_on;                    // sun cascades: the caster classes' cascade limits (PF-0013)
    SccView scc;
} ClCull;

// A mesh's discrete LOD chain as the cull sees it (vk_classic.c ClMesh): the LOD errors, the
// coarsest LOD's triangles (impostor threshold class), an impostor atlas present.
typedef struct ClcChain {
    const float *lod_error;
    u32 lod_count, coarse_tris;
    bool impostor;
} ClcChain;

// An instance against a view, after the sphere tests: culling sphere, camera distance, the caster
// class's weight in the view (PF-0013; 1 outside sun cascades).
typedef struct ClcFront {
    vec3 c;
    float r, dist, casc_a;
} ClcFront;

// The kept parts of one instance: per part the LOD (CLC_IMPOSTOR: the impostor) and the kept dither
// interval (lod_iv_pack, 0: the whole coverage); alpha (fade), scale (instance scale x foliage shrink)
// and wind phase are shared.
typedef struct ClcParts {
    u32 n;
    u32 lod[CLC_PARTS];
    float iv[CLC_PARTS];
    float alpha, scale, phase;
} ClcParts;

DG_INLINE float clc_clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// Unity-style Cast Shadows Off / Shadows Only, the light's culling mask (docs/SHADOWS_UNITY_MODEL.md):
// true when the view never draws an instance with these flags (VgInstance.flags_phase >> 16).
DG_INLINE bool clc_flags_out(const ClCull *v, u32 iflags) {
    if (iflags & VGI_SKINNED) return true;
    if (v->shadow ? (iflags & VGI_NO_CAST) != 0 : (iflags & VGI_SHADOWS_ONLY) != 0) return true;
    if (v->shadow && v->layers && !((v->layers >> VGI_LAYER(iflags)) & 1u)) return true;
    return false;
}

DG_INLINE bool clc_sphere_in(const vec4 *pl, int n, vec3 c, float r) {
    for (int k = 0; k < n; k++)
        if (pl[k].x * c.x + pl[k].y * c.y + pl[k].z * c.z + pl[k].w < -r) return false;
    return true;
}

// Foliage density / distance fade (docs/LOD.md): the material's fade range and band scaled, a gentler
// (smoothstep) falloff, each instance fading over fol_instance of it (lod_foliage_alpha: opaque before
// the fade starts).
DG_INLINE float clc_foliage_alpha(const ClCull *v, const VgMesh *m, u32 fp, float dist) {
    if (!(m->fflags & VGMF_FOLIAGE)) return 1.0f;
    float u = 1.0f;
    if (m->fade_end > m->fade_start) {
        float s0 = m->fade_start * v->fol_k, s1 = s0 + (m->fade_end - m->fade_start) * v->fol_band_k;
        u = clc_clamp01((s1 - dist) / fmaxf(s1 - s0, 1e-3f));
        u = u * u * (3.0f - 2.0f * u);
    }
    return lod_foliage_alpha(v->density, u, ((float)VG_INST_HASH(fp) + 0.5f) / 256.0f, v->fol_instance);
}

// The instance's culling sphere (mesh sphere at the decoded position p, rotation q, scale sc, grown by
// the WPO margin: vg_wpo_margin) against the view: false when culled. Otherwise f holds the sphere,
// the camera distance of the pivot and the caster class's weight.
DG_INLINE bool clc_front(const ClCull *v, const VgMesh *m, const float p[3], const float q[4], float sc, ClcFront *f) {
    quat qq = { q[0], q[1], q[2], q[3] };
    vec3 c = v3_add(v3(p[0], p[1], p[2]), quat_rotate(qq, v3(m->center[0] * sc, m->center[1] * sc, m->center[2] * sc)));
    float r = m->radius * sc + m->max_wpo * fmaxf(sc, 1.0f);   // vg_wpo_margin
    if (v->sphere.w > 0) {
        float rr = r + v->sphere.w;
        if (v3_len2(v3_sub(c, v3(v->sphere.x, v->sphere.y, v->sphere.z))) > rr * rr) return false;
    }
    if (!clc_sphere_in(v->planes, 6, c, r)) return false;
    if (v->nrcv && !clc_sphere_in(v->rcv, v->nrcv, c, r)) return false;
    float dx = p[0] - v->eye.x, dy = p[1] - v->eye.y, dz = p[2] - v->eye.z, dist = sqrtf(dx * dx + dy * dy + dz * dz);
    float casc_a = 1.0f;   // PF-0013: the caster class's cascade limit (0: not in this cascade / beyond its end)
    if (v->scc_on) {
        u32 scl = VGMF_SHADOW_CLASS(m->fflags);
        casc_a = scc_alpha(&v->scc, scl < SCC_COUNT ? (int)scl : SCC_NONE, dist);
        if (casc_a <= 0.0f) return false;
    }
    f->c = c, f->r = r, f->dist = dist, f->casc_a = casc_a;
    return true;
}

// One part of the cross-fade split: the whole coverage, or the kept interval [lo, hi). An interval
// that quantizes empty keeps no threshold and is not emitted: packed it would be 0 ([0, 0): the
// sentinel for the whole coverage, a full extra draw) or an empty [a, a) drawn for nothing.
DG_INLINE void clc_part(ClcParts *o, u32 lod, bool whole, float lo, float hi) {
    if (!whole && lod_iv_empty(lo, hi)) return;
    o->lod[o->n] = lod;
    o->iv[o->n] = whole ? 0.0f : lod_iv_pack(lo, hi);
    o->n++;
}

// The parts of a LOD pick (docs/LOD.md): impostor [0, iw), coarser LOD (lod + 1) [iw, mid), finer LOD
// (lod) [mid, 1) with mid = iw + (1 - iw) x cw; their kept intervals partition [0, 1) at the 1/256
// steps of the dither (test_lod).
DG_INLINE void clc_split(ClcParts *o, u32 lod, float iw, float cw) {
    float mid = iw + (1.0f - iw) * cw;
    if (iw > 0) clc_part(o, CLC_IMPOSTOR, !(iw < 1.0f), 0.0f, iw);
    if (iw < 1.0f) {
        if (cw > 0) clc_part(o, lod + 1, !(cw < 1.0f || iw > 0), iw, mid);
        if (mid < 1.0f) clc_part(o, lod, !(mid > 0), mid, 1.0f);
    }
}

// docs/LOD.md: the fades (shadow distance, foliage density / distance; casc_a from clc_front), the
// impostor share and the LOD of a kept instance (sc: its scale, fp: VgInstance.flags_phase). Returns
// the part count (0: faded out).
DG_INLINE u32 clc_lod_parts(const ClCull *v, const VgMesh *m, const ClcChain *ch, const ClcFront *f, float sc, u32 fp, ClcParts *o) {
    o->n = 0;
    float dist = f->dist, r = f->r;
    float alpha = f->casc_a, shrink = 1.0f;
    if (v->shadow && m->shadow_max_dist > 0) {   // the shadow fades out over the end of its (scaled) distance
        float sd = m->shadow_max_dist * v->shadow_k;
        if (dist > sd) return 0;
        alpha *= v->shadow_band > 0 ? clc_clamp01((sd - dist) / (sd * v->shadow_band)) : 1.0f;
    }
    if (m->fflags & VGMF_FOLIAGE) {   // density / distance fade: dither + the blades shrink
        float fa = clc_foliage_alpha(v, m, fp, dist);
        if (fa <= 0) return 0;
        alpha *= fa;
        shrink = v->fol_shrink + (1.0f - v->fol_shrink) * fa;
    }
    if (alpha <= 0) return 0;
    float dl = fmaxf(v3_dist(f->c, v->lod_eye), v->znear), dcl = fmaxf(dl - r, v->znear);
    float imp_px = ch->coarse_tris >= CLC_IMPOSTOR_TRIS ? v->impostor_px : v->small_impostor_px;
    u32 ks = m->impostor_grid >> 16;
    if (ks) imp_px *= (float)ks / 16.0f;
    float iw = 0;   // impostor share: fades in over imp_band octaves past the switch size
    if (!v->shadow && ch->impostor && imp_px > 0) {
        float size = 2.0f * m->radius * sc * v->px_scale / dl;
        if (size < imp_px) iw = lod_band_weight(log2f(imp_px / fmaxf(size, 1e-6f)), v->imp_band);
    }
    float k = v->ortho ? sc * v->px_scale : sc * v->px_scale / dcl, cw = 0;
    int first = v->era2010 && ch->lod_count > 1 ? 1 : 0;
    u32 lod = iw >= 1.0f ? 0u : (u32)lod_pick_band(ch->lod_error, (int)ch->lod_count, first, k, v->lod_px, v->band, &cw);
    o->alpha = alpha;
    o->scale = sc * shrink;
    o->phase = (float)VG_INST_PHASE(fp) / 256.0f;
    clc_split(o, lod, iw, cw);
    return o->n;
}

// The whole docs/LOD.md decision for an instance that passed clc_flags_out: clc_front, then
// clc_lod_parts. Returns the part count (0: culled).
DG_INLINE u32 clc_cull(const ClCull *v, const VgMesh *m, const ClcChain *ch, const float p[3], const float q[4], float sc, u32 fp, ClcParts *o) {
    ClcFront f;
    if (!clc_front(v, m, p, q, sc, &f)) { o->n = 0; return 0; }
    return clc_lod_parts(v, m, ch, &f, sc, fp, o);
}

// r_lod_legacy / local light tiles: the cut LOD from before docs/LOD.md (CLC_IMPOSTOR: the impostor).
// Impostors: trees (coarsest LOD >= CLC_IMPOSTOR_TRIS) below impostor_px, small cover (grass tufts: a
// few cards) below small_impostor_px.
DG_INLINE u32 clc_legacy_lod(const ClCull *v, const VgMesh *m, const ClcChain *ch, const ClcFront *f, float sc) {
    float dc = fmaxf(v3_dist(f->c, v->lod_eye) - f->r, v->znear);
    float imp_px = ch->coarse_tris >= CLC_IMPOSTOR_TRIS ? v->impostor_px : v->small_impostor_px;
    u32 ks = m->impostor_grid >> 16;   // the mesh's switch scale (1/16 units, 0 = 1x), as vgeo_use_impostor
    if (ks) imp_px *= (float)ks / 16.0f;
    if (!v->shadow && ch->impostor && imp_px > 0 && 2.0f * m->radius * sc * v->px_scale / fmaxf(v3_dist(f->c, v->lod_eye), v->znear) < imp_px) return CLC_IMPOSTOR;
    u32 lod = v->era2010 && ch->lod_count > 1 ? 1 : 0;
    float k = v->ortho ? sc * v->px_scale : sc * v->px_scale / dc;
    while (lod + 1 < ch->lod_count && ch->lod_error[lod + 1] * k <= v->lod_px) lod++;
    return lod;
}

#endif
