// Virtualized geometry builder (docs/VGEO.md "Builder"). Nanite's offline pipeline in a smaller form:
// grid-snapped weld, meshoptimizer meshlets as leaf clusters, a DAG of locked-boundary group
// simplifications, a 4-wide BVH over groups per level, 128 KiB pages. Deterministic: stable sorts only
// (merge / LSD radix), parallel group jobs whose results are appended in group order.
#include "vgeo_build.h"
#include "meshoptimizer.h"
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VG_GROUP_TARGET 8
#define VG_GROUP_MIN 4
#define VG_NEAR_WINDOW 128
#define VG_MULTI 0xFFFFFFFFu

// ------------------------------------------------------------------ small helpers

typedef struct Sph { float c[3], r; } Sph;

static Sph sph_merge(Sph a, Sph b) {
    if (a.r < 0) return b;
    if (b.r < 0) return a;
    float d[3] = { b.c[0] - a.c[0], b.c[1] - a.c[1], b.c[2] - a.c[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist + b.r <= a.r) return a;
    if (dist + a.r <= b.r) return b;
    Sph s;
    s.r = (dist + a.r + b.r) * 0.5f;
    float k = dist > 1e-12f ? (s.r - a.r) / dist : 0.0f;
    for (int i = 0; i < 3; i++) s.c[i] = a.c[i] + d[i] * k;
    s.r *= 1.0f + 1e-6f;   // float slack: containment holds after rounding
    s.r += 1e-7f;
    return s;
}

static double now_ms(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

#define VEC(T) struct { T *p; size_t n, cap; }
#define VEC_PUSH(v, x) do { if ((v).n == (v).cap) { (v).cap = (v).cap ? (v).cap * 2 : 64; (v).p = realloc((v).p, sizeof(*(v).p) * (v).cap); } (v).p[(v).n++] = (x); } while (0)
#define VEC_RESERVE(v, k) do { if ((v).n + (k) > (v).cap) { while ((v).n + (k) > (v).cap) (v).cap = (v).cap ? (v).cap * 2 : 64; (v).p = realloc((v).p, sizeof(*(v).p) * (v).cap); } } while (0)
#define VEC_FREE(v) do { free((v).p); (v).p = NULL; (v).n = (v).cap = 0; } while (0)
typedef VEC(u32) VecU32;
typedef VEC(u64) VecU64;
typedef VEC(u8) VecU8;

u32 vgeo_fnv1a(const void *data, size_t size, u32 h) {
    const u8 *p = (const u8 *)data;
    if (!h) h = 2166136261u;
    for (size_t i = 0; i < size; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

u16 vgeo_half(float f) {
    union { float f; u32 u; } v = { f };
    u32 s = (v.u >> 16) & 0x8000u, e = (v.u >> 23) & 0xFFu, m = v.u & 0x7FFFFFu;
    if (e == 0xFF) return (u16)(s | 0x7C00u | (m ? 0x200u : 0));
    int ex = (int)e - 127 + 15;
    if (ex >= 31) return (u16)(s | 0x7C00u);
    if (ex <= 0) {
        if (ex < -10) return (u16)s;
        m |= 0x800000u;
        u32 t = 14u - (u32)ex;
        u32 hm = m >> t, rem = m & ((1u << t) - 1u), half = 1u << (t - 1u);
        if (rem > half || (rem == half && (hm & 1u))) hm++;
        return (u16)(s | hm);
    }
    u32 hm = m >> 13, rem = m & 0x1FFFu;
    u32 h = s | ((u32)ex << 10) | hm;
    if (rem > 0x1000u || (rem == 0x1000u && (hm & 1u))) h++;
    return (u16)h;
}

float vgeo_half_to_float(u16 h) {
    u32 s = (u32)(h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    union { u32 u; float f; } v;
    if (e == 0) {
        if (!m) { v.u = s; return v.f; }
        while (!(m & 0x400u)) { m <<= 1; e--; }
        e++;
        m &= 0x3FFu;
    } else if (e == 31) { v.u = s | 0x7F800000u | (m << 13); return v.f; }
    v.u = s | ((e + 112u) << 23) | (m << 13);
    return v.f;
}

static u32 oct_encode8(const float n[3]) {
    float l = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    float x = l > 0 ? n[0] / l : 0, y = l > 0 ? n[1] / l : 0;
    if (n[2] < 0) {
        float ox = (1.0f - fabsf(y)) * (x >= 0 ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0 ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    int qx = (int)lrintf((x * 0.5f + 0.5f) * 255.0f), qy = (int)lrintf((y * 0.5f + 0.5f) * 255.0f);
    return (u32)DG_CLAMP(qx, 0, 255) | ((u32)DG_CLAMP(qy, 0, 255) << 8);
}

static void oct_decode8(u32 v, float n[3]) {
    float x = (float)(v & 0xFF) / 255.0f * 2.0f - 1.0f, y = (float)((v >> 8) & 0xFF) / 255.0f * 2.0f - 1.0f;
    float z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0) {
        float ox = (1.0f - fabsf(y)) * (x >= 0 ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0 ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    float l = sqrtf(x * x + y * y + z * z);
    n[0] = x / l;
    n[1] = y / l;
    n[2] = z / l;
}

// Stable merge sort of u32 items with a context comparator (deterministic, no qsort).
typedef int (*CmpFn)(const void *ctx, u32 a, u32 b);
static void msort_rec(u32 *a, u32 *t, size_t n, CmpFn cmp, const void *ctx) {
    if (n < 2) return;
    if (n <= 16) {
        for (size_t i = 1; i < n; i++) {
            u32 x = a[i];
            size_t j = i;
            while (j > 0 && cmp(ctx, a[j - 1], x) > 0) { a[j] = a[j - 1]; j--; }
            a[j] = x;
        }
        return;
    }
    size_t h = n / 2;
    msort_rec(a, t, h, cmp, ctx);
    msort_rec(a + h, t, n - h, cmp, ctx);
    if (cmp(ctx, a[h - 1], a[h]) <= 0) return;
    memcpy(t, a, sizeof(u32) * h);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) a[k++] = cmp(ctx, a[j], t[i]) < 0 ? a[j++] : t[i++];
    while (i < h) a[k++] = t[i++];
}
static void msort(u32 *a, size_t n, CmpFn cmp, const void *ctx) {
    u32 *t = (u32 *)malloc(sizeof(u32) * (n / 2 + 1));
    msort_rec(a, t, n, cmp, ctx);
    free(t);
}

// LSD radix sort of (u64 key, u32 value) pairs, stable (16-bit digits).
static void radix_sort64(u64 *keys, u32 *vals, size_t n) {
    if (n < 2) return;
    u64 *k2 = (u64 *)malloc(sizeof(u64) * n);
    u32 *v2 = (u32 *)malloc(sizeof(u32) * n);
    u32 *count = (u32 *)malloc(sizeof(u32) * 65536);
    u64 all_or = 0;
    for (size_t i = 0; i < n; i++) all_or |= keys[i];
    for (int pass = 0; pass < 4; pass++) {
        int sh = pass * 16;
        if (!((all_or >> sh) & 0xFFFFu)) continue;   // digit is zero everywhere
        memset(count, 0, sizeof(u32) * 65536);
        for (size_t i = 0; i < n; i++) count[(keys[i] >> sh) & 0xFFFFu]++;
        u32 sum = 0;
        for (int d = 0; d < 65536; d++) { u32 c = count[d]; count[d] = sum; sum += c; }
        for (size_t i = 0; i < n; i++) {
            u32 d = (u32)((keys[i] >> sh) & 0xFFFFu);
            u32 o = count[d]++;
            k2[o] = keys[i];
            v2[o] = vals[i];
        }
        memcpy(keys, k2, sizeof(u64) * n);
        memcpy(vals, v2, sizeof(u32) * n);
    }
    free(k2);
    free(v2);
    free(count);
}

// ------------------------------------------------------------------ thread pool

typedef void (*JobFn)(void *ctx, u32 index, int worker);
typedef struct Par { JobFn fn; void *ctx; u32 n; SDL_AtomicInt next; int worker_id; SDL_AtomicInt wid; } Par;

static int SDLCALL par_main(void *p) {
    Par *par = (Par *)p;
    int w = SDL_AddAtomicInt(&par->wid, 1);
    for (;;) {
        int i = SDL_AddAtomicInt(&par->next, 1);
        if (i >= (int)par->n) break;
        par->fn(par->ctx, (u32)i, w);
    }
    return 0;
}

static int resolve_threads(int t) {
    if (t <= 0) t = SDL_GetNumLogicalCPUCores();
    return DG_CLAMP(t, 1, 64);
}

static void par_for(int threads, u32 n, JobFn fn, void *ctx) {
    Par par;
    memset(&par, 0, sizeof(par));
    par.fn = fn;
    par.ctx = ctx;
    par.n = n;
    if (threads <= 1 || n < 2) {
        for (u32 i = 0; i < n; i++) fn(ctx, i, 0);
        return;
    }
    SDL_Thread *th[64];
    int k = DG_MIN(threads, (int)n);
    for (int i = 0; i < k; i++) th[i] = SDL_CreateThread(par_main, "vgeo_build", &par);
    for (int i = 0; i < k; i++) SDL_WaitThread(th[i], NULL);
}

// ------------------------------------------------------------------ build state

typedef struct BCluster {
    u32 first, tris;             // into B.idx (global welded vertex ids)
    u32 material, level, group, child_group;
    float error;
    Sph lod, bounds;
    u32 cone;                    // encoded (VgCluster.cone)
} BCluster;

typedef VEC(BCluster) VecCluster;

typedef struct BGroup {
    u32 first_member, member_count;   // into B.members
    u32 level;
    float parent_error;
    Sph lod, bounds;
    bool unreducible;
} BGroup;
typedef VEC(BGroup) VecGroup;

typedef struct B {
    const VgBuildOptions *opt;
    int threads;
    // welded vertices
    u32 vcount;
    float *pos, *nrm, *uv;
    u32 *col;
    u32 *pid;                    // position-only weld id
    u32 pcount;
    float q_step;
    float aabb_min[3], aabb_max[3];
    i32 *grid;                   // exact grid coordinates (pos = grid * q_step)
    VecCluster clusters;
    VecU32 idx;
    VecGroup groups;
    VecU32 members;
    u32 material_count;
    float radius;
    bool has_color;
    bool skinned;                // VGF_SKINNED
    u8 *skin;                    // per welded vertex: joints[4], weights[4] (sorted by weight, sum 255)
    u32 bone_count;              // max joint with a non-zero weight + 1
} B;

// ------------------------------------------------------------------ skin

// One source vertex's skin: duplicate joints merged, <= 4 influences sorted by weight (ties: lower
// joint), quantised to u8 summing to 255 (largest remainder). Unused slots repeat the dominant joint
// with weight 0, so out[0] is the dominant bone.
static void skin_normalise(const u8 j[4], const float w[4], u8 out[8]) {
    u8 jj[4];
    float ww[4];
    int n = 0;
    for (int k = 0; k < 4; k++) {
        float x = isfinite(w[k]) && w[k] > 0 ? w[k] : 0.0f;
        int f = 0;
        while (f < n && jj[f] != j[k]) f++;
        if (f == n) { jj[n] = j[k]; ww[n] = 0; n++; }
        ww[f] += x;
    }
    for (int a = 1; a < n; a++)
        for (int c = a; c > 0 && (ww[c] > ww[c - 1] || (ww[c] == ww[c - 1] && jj[c] < jj[c - 1])); c--) {
            float t = ww[c]; ww[c] = ww[c - 1]; ww[c - 1] = t;
            u8 u = jj[c]; jj[c] = jj[c - 1]; jj[c - 1] = u;
        }
    float sum = 0;
    for (int k = 0; k < n; k++) sum += ww[k];
    if (!(sum > 0)) { ww[0] = 1.0f; sum = 1.0f; for (int k = 1; k < n; k++) ww[k] = 0; }
    int q[4] = { 0, 0, 0, 0 }, total = 0;
    float rem[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < n; k++) {
        float x = ww[k] / sum * 255.0f;
        q[k] = (int)floorf(x);
        rem[k] = x - (float)q[k];
        total += q[k];
    }
    while (total < 255) {   // largest remainder first (ties: the heavier slot)
        int best = 0;
        for (int k = 1; k < n; k++) if (rem[k] > rem[best]) best = k;
        q[best]++;
        rem[best] = -1.0f;
        total++;
    }
    for (int k = 0; k < 4; k++) {
        bool used = k < n && q[k] > 0;
        out[k] = used ? jj[k] : jj[0];
        out[4 + k] = used ? (u8)q[k] : 0;
    }
}

static int popcount64(u64 x) {
    int c = 0;
    while (x) { x &= x - 1; c++; }
    return c;
}

// Bones with a non-zero weight on welded vertex v, as a 128-bit mask.
static void skin_mask(const B *b, u32 v, u64 m[2]) {
    const u8 *s = &b->skin[v * 8];
    for (int k = 0; k < 4; k++)
        if (s[4 + k]) m[s[k] >> 6] |= 1ull << (s[k] & 63);
}

// Conservative half rounding (bone boxes): the half at or below / above f.
static u16 half_step(u16 h, bool up) {
    bool neg = (h & 0x8000u) != 0;
    if ((h & 0x7FFFu) == 0) return up ? 0x0001u : 0x8001u;
    return (u16)(neg == up ? h - 1 : h + 1);
}
static u16 half_down(float f) {
    u16 h = vgeo_half(f);
    return vgeo_half_to_float(h) > f ? half_step(h, false) : h;
}
static u16 half_up(float f) {
    u16 h = vgeo_half(f);
    return vgeo_half_to_float(h) < f ? half_step(h, true) : h;
}

static Sph cluster_bounds_and_cone(const B *b, const u32 *tris, u32 tri_count, u32 *cone_out) {
    // meshopt bounds on a compact copy (meshopt_computeClusterBounds takes global indices).
    struct meshopt_Bounds mb = meshopt_computeClusterBounds(tris, (size_t)tri_count * 3, b->pos, b->vcount, sizeof(float) * 3);
    Sph s = { { mb.center[0], mb.center[1], mb.center[2] }, mb.radius };
    // Exact containment: meshopt's sphere is approximate, grow it to every vertex.
    for (u32 i = 0; i < tri_count * 3; i++) {
        const float *p = &b->pos[tris[i] * 3];
        float d = sqrtf((p[0] - s.c[0]) * (p[0] - s.c[0]) + (p[1] - s.c[1]) * (p[1] - s.c[1]) + (p[2] - s.c[2]) * (p[2] - s.c[2]));
        if (d > s.r) s.r = d;
    }
    s.r = s.r * (1.0f + 1e-6f) + 1e-7f;
    u32 flags = 0;
    if (mb.cone_cutoff >= 1.0f - 1e-4f || b->skinned) flags |= VGC_NO_CONE;   // skinned: the bind-pose cone says nothing
    float ax[3] = { mb.cone_axis[0], mb.cone_axis[1], mb.cone_axis[2] };
    *cone_out = oct_encode8(ax) | ((u32)(u8)mb.cone_cutoff_s8 << 16) | (flags << 24);
    return s;
}

// Appends meshopt meshlets of a triangle list (local vertex ids into lpos, mapped to global ids) as
// new clusters of the given level / material / error / lod / child_group.
static void meshletize(B *b, const u32 *lidx, size_t lcount, const float *lpos, size_t lv, const u32 *l2g, u32 material, u32 level, float error, Sph lod,
                       u32 child_group, VecCluster *out_clusters, VecU32 *out_idx) {
    size_t max_m = meshopt_buildMeshletsBound(lcount, VGEO_MAX_CLUSTER_VERTS, VGEO_MAX_CLUSTER_TRIS);
    struct meshopt_Meshlet *ml = (struct meshopt_Meshlet *)malloc(sizeof(*ml) * (max_m ? max_m : 1));
    u32 *mv = (u32 *)malloc(sizeof(u32) * (max_m * VGEO_MAX_CLUSTER_VERTS + 1));
    u8 *mt = (u8 *)malloc(max_m * VGEO_MAX_CLUSTER_TRIS * 3 + 4);
    size_t mc = meshopt_buildMeshlets(ml, mv, mt, lidx, lcount, lpos, lv, sizeof(float) * 3, VGEO_MAX_CLUSTER_VERTS, VGEO_MAX_CLUSTER_TRIS, 0.25f);
    for (size_t m = 0; m < mc; m++) {
        meshopt_optimizeMeshlet(&mv[ml[m].vertex_offset], &mt[ml[m].triangle_offset], ml[m].triangle_count, ml[m].vertex_count);
        // Skinned: split into runs of triangles (meshlet order) touching <= VGEO_MAX_CLUSTER_BONES bones.
        for (u32 t0 = 0, t1; t0 < ml[m].triangle_count; t0 = t1) {
            t1 = ml[m].triangle_count;
            if (b->skinned) {
                u64 mask[2] = { 0, 0 };
                for (t1 = t0; t1 < ml[m].triangle_count; t1++) {
                    u64 tm[2] = { mask[0], mask[1] };
                    for (int k = 0; k < 3; k++) {
                        u32 local = mv[ml[m].vertex_offset + mt[ml[m].triangle_offset + t1 * 3 + k]];
                        skin_mask(b, l2g ? l2g[local] : local, tm);
                    }
                    if (t1 > t0 && popcount64(tm[0]) + popcount64(tm[1]) > VGEO_MAX_CLUSTER_BONES) break;
                    mask[0] = tm[0];
                    mask[1] = tm[1];
                }
            }
            BCluster c;
            memset(&c, 0, sizeof(c));
            c.first = (u32)out_idx->n;
            c.tris = t1 - t0;
            c.material = material;
            c.level = level;
            c.group = ~0u;
            c.child_group = child_group;
            c.error = error;
            c.lod = lod;
            VEC_RESERVE(*out_idx, (size_t)c.tris * 3);
            for (u32 t = t0 * 3; t < t1 * 3; t++) {
                u32 local = mv[ml[m].vertex_offset + mt[ml[m].triangle_offset + t]];
                out_idx->p[out_idx->n++] = l2g ? l2g[local] : local;
            }
            c.bounds = cluster_bounds_and_cone(b, &out_idx->p[c.first], c.tris, &c.cone);
            if (lod.r < 0) c.lod = c.bounds;   // leaves: their own sphere, error 0
            VEC_PUSH(*out_clusters, c);
        }
    }
    free(ml);
    free(mv);
    free(mt);
}

// ------------------------------------------------------------------ weld

typedef struct WeldCtx { const float *key; int stride; } WeldCtx;
static int cmp_key(const void *ctx, u32 a, u32 b) {
    const WeldCtx *w = (const WeldCtx *)ctx;
    int c = memcmp(&w->key[(size_t)a * w->stride], &w->key[(size_t)b * w->stride], sizeof(float) * (size_t)w->stride);
    if (c) return c;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static bool weld(B *b, const VgSource *src, u32 **out_idx, u32 *out_count, u16 **out_mat, char *err, int err_size) {
    u32 n = src->vertex_count;
    // Grid: one step per mesh such that every cluster (<= the whole mesh) fits u16 offsets.
    float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (u32 i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            float v = src->pos[i * 3 + k];
            if (!isfinite(v)) { snprintf(err, (size_t)err_size, "vertex %u is not finite", i); return false; }
            if (src->joints && (src->joints[(size_t)i * 4 + k] >= VGEO_MAX_BONES || (k == 2 && src->joints[(size_t)i * 4 + 3] >= VGEO_MAX_BONES))) {
                snprintf(err, (size_t)err_size, "vertex %u: joint >= %d", i, VGEO_MAX_BONES);
                return false;
            }
            mn[k] = fminf(mn[k], v);
            mx[k] = fmaxf(mx[k], v);
        }
    float ext = fmaxf(mx[0] - mn[0], fmaxf(mx[1] - mn[1], mx[2] - mn[2]));
    b->q_step = fmaxf(ext, 1e-6f) / 65000.0f;
    // Keys: grid position (exact integers, stored as their bits), normal, uv, colour (stride 9 floats).
    const int S = 9;
    float *key = (float *)malloc(sizeof(float) * (size_t)S * n);
    for (u32 i = 0; i < n; i++) {
        float *k = &key[(size_t)i * S];
        for (int j = 0; j < 3; j++) { i32 gv = (i32)llrint((double)src->pos[i * 3 + j] / (double)b->q_step); memcpy(&k[j], &gv, 4); }
        for (int j = 0; j < 3; j++) k[3 + j] = src->normal ? src->normal[i * 3 + j] : 0.0f;
        k[6] = src->uv ? src->uv[i * 2] : 0.0f;
        k[7] = src->uv ? src->uv[i * 2 + 1] : 0.0f;
        u32 c = src->color ? src->color[i] : 0;
        memcpy(&k[8], &c, 4);
        for (int j = 3; j < 8; j++) if (k[j] == 0.0f) k[j] = 0.0f;   // -0 == +0
    }
    u32 *order = (u32 *)malloc(sizeof(u32) * n);
    for (u32 i = 0; i < n; i++) order[i] = i;
    WeldCtx wc = { key, S };
    msort(order, n, cmp_key, &wc);
    u32 *remap = (u32 *)malloc(sizeof(u32) * n);
    u32 vc = 0;
    for (u32 i = 0; i < n; i++) {
        if (i == 0 || memcmp(&key[(size_t)order[i] * S], &key[(size_t)order[i - 1] * S], sizeof(float) * S)) vc++;
        remap[order[i]] = vc - 1;
    }
    b->vcount = vc;
    b->pos = (float *)malloc(sizeof(float) * 3 * vc);
    b->grid = (i32 *)malloc(sizeof(i32) * 3 * vc);
    b->nrm = (float *)malloc(sizeof(float) * 3 * vc);
    b->uv = (float *)malloc(sizeof(float) * 2 * vc);
    b->col = (u32 *)malloc(sizeof(u32) * vc);
    for (u32 i = 0; i < n; i++) {
        u32 v = remap[i];
        const float *k = &key[(size_t)i * S];
        memcpy(&b->grid[v * 3], k, 12);
        for (int j = 0; j < 3; j++) b->pos[v * 3 + j] = (float)((double)b->grid[v * 3 + j] * (double)b->q_step);
        memcpy(&b->nrm[v * 3], k + 3, 12);
        memcpy(&b->uv[v * 2], k + 6, 8);
        memcpy(&b->col[v], k + 8, 4);
    }
    free(order);
    free(key);
    // Triangles: remapped, degenerate (in position space) removed.
    u32 tc = src->index_count / 3;
    u32 *idx = (u32 *)malloc(sizeof(u32) * (size_t)tc * 3);
    u16 *tm = (u16 *)malloc(sizeof(u16) * (tc ? tc : 1));
    u32 kept = 0;
    // Position weld (pid): sort welded vertices by position bits.
    u32 *po = (u32 *)malloc(sizeof(u32) * vc);
    for (u32 i = 0; i < vc; i++) po[i] = i;
    WeldCtx pc = { (const float *)b->grid, 3 };
    msort(po, vc, cmp_key, &pc);
    b->pid = (u32 *)malloc(sizeof(u32) * vc);
    u32 pcount = 0;
    for (u32 i = 0; i < vc; i++) {
        if (i == 0 || memcmp(&b->grid[po[i] * 3], &b->grid[po[i - 1] * 3], 12)) pcount++;
        b->pid[po[i]] = pcount - 1;
    }
    free(po);
    b->pcount = pcount;
    for (u32 t = 0; t < tc; t++) {
        u32 a = src->indices[t * 3], c1 = src->indices[t * 3 + 1], c2 = src->indices[t * 3 + 2];
        if (a >= n || c1 >= n || c2 >= n) { snprintf(err, (size_t)err_size, "index out of range at triangle %u", t); free(idx); free(tm); free(remap); return false; }
        u32 v0 = remap[a], v1 = remap[c1], v2 = remap[c2];
        if (b->pid[v0] == b->pid[v1] || b->pid[v1] == b->pid[v2] || b->pid[v0] == b->pid[v2]) continue;
        idx[kept * 3] = v0;
        idx[kept * 3 + 1] = v1;
        idx[kept * 3 + 2] = v2;
        tm[kept] = src->tri_material ? src->tri_material[t] : 0;
        kept++;
    }
    // Skin per position: every welded vertex at a grid position takes the normalised skin of the first
    // source vertex there (crack-free animated cuts: shared vertices skin bit-identically).
    if (b->skinned) {
        u8 *pskin = (u8 *)malloc((size_t)pcount * 8);
        u8 *pset = (u8 *)calloc(pcount ? pcount : 1, 1);
        for (u32 i = 0; i < n; i++) {
            u32 p = b->pid[remap[i]];
            if (pset[p]) continue;
            pset[p] = 1;
            float w[4] = { 1, 0, 0, 0 };
            for (int k = 0; k < 4 && (src->weights || src->weights_u8); k++)
                w[k] = src->weights ? src->weights[(size_t)i * 4 + k] : (float)src->weights_u8[(size_t)i * 4 + k];
            skin_normalise(&src->joints[(size_t)i * 4], w, &pskin[(size_t)p * 8]);
        }
        b->skin = (u8 *)malloc((size_t)vc * 8);
        for (u32 v = 0; v < vc; v++) {
            memcpy(&b->skin[(size_t)v * 8], &pskin[(size_t)b->pid[v] * 8], 8);
            for (int k = 0; k < 4; k++)
                if (b->skin[v * 8 + 4 + k]) b->bone_count = DG_MAX(b->bone_count, (u32)b->skin[v * 8 + k] + 1);
        }
        free(pskin);
        free(pset);
    }
    free(remap);
    // Missing normals: smooth, area weighted, per position.
    if (!src->normal) {
        float *pn = (float *)calloc((size_t)pcount * 3, sizeof(float));
        for (u32 t = 0; t < kept; t++) {
            const float *p0 = &b->pos[idx[t * 3] * 3], *p1 = &b->pos[idx[t * 3 + 1] * 3], *p2 = &b->pos[idx[t * 3 + 2] * 3];
            float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
            float fn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < 3; j++) pn[b->pid[idx[t * 3 + k]] * 3 + j] += fn[j];
        }
        for (u32 v = 0; v < vc; v++) {
            float *q = &pn[b->pid[v] * 3];
            float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            for (int j = 0; j < 3; j++) b->nrm[v * 3 + j] = l > 0 ? q[j] / l : (j == 1 ? 1.0f : 0.0f);
        }
        free(pn);
    }
    for (int k = 0; k < 3; k++) { b->aabb_min[k] = mn[k]; b->aabb_max[k] = mx[k]; }
    *out_idx = idx;
    *out_count = kept * 3;
    *out_mat = tm;
    return true;
}

// ------------------------------------------------------------------ partition (greedy groups)

static u32 morton3(u32 x, u32 y, u32 z) {
    u32 r = 0;
    for (int i = 0; i < 10; i++) r |= ((x >> i) & 1u) << (3 * i) | ((y >> i) & 1u) << (3 * i + 1) | ((z >> i) & 1u) << (3 * i + 2);
    return r;
}

static float cdist2(const BCluster *a, const float c[3]) {
    float d[3] = { a->bounds.c[0] - c[0], a->bounds.c[1] - c[1], a->bounds.c[2] - c[2] };
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

// Partitions the level's clusters into groups (appended to b->groups) of ~VG_GROUP_TARGET by shared
// edges. Returns the number of groups created.
static u32 partition(B *b, const u32 *level, u32 n, u32 level_index, float parent_error_override) {
    u32 first_group = (u32)b->groups.n;
    // Spatial order (Morton over the level's bounds, ties by id): seeds and the fallback window.
    float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (u32 i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            float v = b->clusters.p[level[i]].bounds.c[k];
            mn[k] = fminf(mn[k], v);
            mx[k] = fmaxf(mx[k], v);
        }
    u64 *mk = (u64 *)malloc(sizeof(u64) * (n ? n : 1));
    u32 *ord = (u32 *)malloc(sizeof(u32) * (n ? n : 1));
    for (u32 i = 0; i < n; i++) {
        const float *c = b->clusters.p[level[i]].bounds.c;
        u32 q[3];
        for (int k = 0; k < 3; k++) q[k] = (u32)DG_CLAMP((int)((c[k] - mn[k]) / fmaxf(mx[k] - mn[k], 1e-9f) * 1023.0f), 0, 1023);
        mk[i] = ((u64)morton3(q[0], q[1], q[2]) << 32) | i;
        ord[i] = i;
    }
    radix_sort64(mk, ord, n);
    free(mk);
    u32 *rank = (u32 *)malloc(sizeof(u32) * (n ? n : 1));
    for (u32 k = 0; k < n; k++) rank[ord[k]] = k;
    // Adjacency: shared position-space edges between clusters (k = Morton rank).
    size_t ecount = 0;
    for (u32 i = 0; i < n; i++) ecount += (size_t)b->clusters.p[level[i]].tris * 3;
    u64 *ek = (u64 *)malloc(sizeof(u64) * (ecount ? ecount : 1));
    u32 *ev = (u32 *)malloc(sizeof(u32) * (ecount ? ecount : 1));
    size_t e = 0;
    for (u32 k = 0; k < n; k++) {
        const BCluster *c = &b->clusters.p[level[ord[k]]];
        const u32 *t = &b->idx.p[c->first];
        for (u32 j = 0; j < c->tris; j++)
            for (int s = 0; s < 3; s++) {
                u32 pa = b->pid[t[j * 3 + s]], pb = b->pid[t[j * 3 + (s + 1) % 3]];
                if (pa > pb) { u32 x = pa; pa = pb; pb = x; }
                ek[e] = ((u64)pa << 32) | pb;
                ev[e] = k;
                e++;
            }
    }
    radix_sort64(ek, ev, e);
    VecU64 pairs = { 0 };
    for (size_t i = 0; i < e;) {
        size_t j = i + 1;
        while (j < e && ek[j] == ek[i]) j++;
        for (size_t a = i; a < j; a++)
            for (size_t c = a + 1; c < j; c++)
                if (ev[a] != ev[c]) {
                    u32 x = ev[a], y = ev[c];
                    VEC_PUSH(pairs, ((u64)x << 32) | y);
                    VEC_PUSH(pairs, ((u64)y << 32) | x);
                }
        i = j;
    }
    free(ek);
    free(ev);
    u32 *pv = (u32 *)malloc(sizeof(u32) * (pairs.n ? pairs.n : 1));
    for (size_t i = 0; i < pairs.n; i++) pv[i] = 0;
    radix_sort64(pairs.p, pv, pairs.n);
    free(pv);
    // CSR: per k, neighbours with their shared-edge counts.
    u32 *adj_first = (u32 *)calloc((size_t)n + 1, sizeof(u32));
    VecU32 adj = { 0 };
    VecU32 adjw = { 0 };
    for (size_t i = 0; i < pairs.n;) {
        size_t j = i + 1;
        while (j < pairs.n && pairs.p[j] == pairs.p[i]) j++;
        u32 a = (u32)(pairs.p[i] >> 32), c = (u32)pairs.p[i];
        VEC_PUSH(adj, c);
        VEC_PUSH(adjw, (u32)(j - i));
        adj_first[a + 1]++;
        i = j;
    }
    for (u32 k = 0; k < n; k++) adj_first[k + 1] += adj_first[k];
    VEC_FREE(pairs);
    // Greedy growth.
    u32 *assigned = (u32 *)malloc(sizeof(u32) * (n ? n : 1));
    for (u32 k = 0; k < n; k++) assigned[k] = ~0u;
    VecU32 cand = { 0 };
    VecU32 candw = { 0 };
    u32 mem[16 + 1];
    for (u32 seed = 0; seed < n; seed++) {
        if (assigned[seed] != ~0u) continue;
        u32 g = (u32)b->groups.n;
        u32 cnt = 0;
        mem[cnt++] = seed;
        assigned[seed] = g;
        float gc[3];
        memcpy(gc, b->clusters.p[level[ord[seed]]].bounds.c, sizeof(gc));
        cand.n = candw.n = 0;
        while (cnt < VG_GROUP_TARGET) {
            // Candidates: unassigned neighbours of the members, weighted by shared edges.
            cand.n = candw.n = 0;
            for (u32 m = 0; m < cnt; m++)
                for (u32 a = adj_first[mem[m]]; a < adj_first[mem[m] + 1]; a++) {
                    u32 nb = adj.p[a];
                    if (assigned[nb] != ~0u) continue;
                    size_t f = 0;
                    while (f < cand.n && cand.p[f] != nb) f++;
                    if (f == cand.n) { VEC_PUSH(cand, nb); VEC_PUSH(candw, 0); }
                    candw.p[f] += adjw.p[a];
                }
            u32 best = ~0u, bw = 0;
            float bd = FLT_MAX;
            for (size_t f = 0; f < cand.n; f++) {
                float d = cdist2(&b->clusters.p[level[ord[cand.p[f]]]], gc);
                if (candw.p[f] > bw || (candw.p[f] == bw && (d < bd || (d == bd && cand.p[f] < best)))) { best = cand.p[f]; bw = candw.p[f]; bd = d; }
            }
            if (best == ~0u && cnt < VG_GROUP_MIN) {
                // Disconnected: the spatially nearest unassigned cluster (window in Morton order).
                u32 lo = seed > VG_NEAR_WINDOW ? seed - VG_NEAR_WINDOW : 0, hi = DG_MIN(n, seed + VG_NEAR_WINDOW + 1);
                for (u32 k = lo; k < hi; k++) {
                    if (assigned[k] != ~0u) continue;
                    float d = cdist2(&b->clusters.p[level[ord[k]]], gc);
                    if (d < bd || (d == bd && k < best)) { best = k; bd = d; }
                }
            }
            if (best == ~0u) break;
            mem[cnt++] = best;
            assigned[best] = g;
            for (int k = 0; k < 3; k++) gc[k] = (gc[k] * (float)(cnt - 1) + b->clusters.p[level[ord[best]]].bounds.c[k]) / (float)cnt;
        }
        BGroup gr;
        memset(&gr, 0, sizeof(gr));
        gr.first_member = (u32)b->members.n;
        gr.member_count = cnt;
        gr.level = level_index;
        gr.lod.r = gr.bounds.r = -1.0f;
        float maxerr = 0;
        // Members in a fixed order (ascending cluster id): deterministic merged triangle order.
        u32 ids[16 + 1];
        for (u32 m = 0; m < cnt; m++) ids[m] = level[ord[mem[m]]];
        for (u32 x = 1; x < cnt; x++)
            for (u32 y = x; y > 0 && ids[y] < ids[y - 1]; y--) { u32 t = ids[y]; ids[y] = ids[y - 1]; ids[y - 1] = t; }
        for (u32 m = 0; m < cnt; m++) {
            BCluster *c = &b->clusters.p[ids[m]];
            c->group = g;
            VEC_PUSH(b->members, ids[m]);
            gr.lod = sph_merge(gr.lod, c->lod);
            gr.bounds = sph_merge(gr.bounds, c->bounds);
            maxerr = fmaxf(maxerr, c->error);
        }
        gr.parent_error = parent_error_override >= 0 ? parent_error_override : maxerr;
        VEC_PUSH(b->groups, gr);
    }
    VEC_FREE(cand);
    VEC_FREE(candw);
    VEC_FREE(adj);
    VEC_FREE(adjw);
    free(adj_first);
    free(assigned);
    free(rank);
    free(ord);
    return (u32)b->groups.n - first_group;
}

// ------------------------------------------------------------------ group simplification (jobs)

typedef struct GroupJob {
    B *b;
    u32 first_group, level;
    const u32 *owner;            // per pid: owner key (group * mats + material) or VG_MULTI
    struct GroupOut {
        VecCluster clusters;
        VecU32 idx;
        float error;
        u32 tris_in, tris_out;
    } *out;
} GroupJob;

static int cmp_u32v(const void *ctx, u32 a, u32 b) {
    (void)ctx;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static void group_job(void *ctx, u32 gi, int worker) {
    (void)worker;
    GroupJob *j = (GroupJob *)ctx;
    B *b = j->b;
    u32 g = j->first_group + gi;
    const BGroup *gr = &b->groups.p[g];
    struct GroupOut *o = &j->out[gi];
    memset(o, 0, sizeof(*o));
    // Members' triangles, split by material (each subset simplified on its own; material borders lock).
    u32 mats[16];
    u32 nm = 0;
    for (u32 m = 0; m < gr->member_count; m++) {
        u32 mat = b->clusters.p[b->members.p[gr->first_member + m]].material;
        u32 k = 0;
        while (k < nm && mats[k] != mat) k++;
        if (k == nm && nm < 16) mats[nm++] = mat;
    }
    float maxerr = 0;
    for (u32 m = 0; m < gr->member_count; m++) maxerr = fmaxf(maxerr, b->clusters.p[b->members.p[gr->first_member + m]].error);
    float serr = 0;
    for (u32 mi = 0; mi < nm; mi++) {
        u32 mat = mats[mi];
        VecU32 tri = { 0 };
        for (u32 m = 0; m < gr->member_count; m++) {
            const BCluster *c = &b->clusters.p[b->members.p[gr->first_member + m]];
            if (c->material != mat) continue;
            VEC_RESERVE(tri, (size_t)c->tris * 3);
            memcpy(&tri.p[tri.n], &b->idx.p[c->first], sizeof(u32) * c->tris * 3);
            tri.n += (size_t)c->tris * 3;
        }
        o->tris_in += (u32)(tri.n / 3);
        // Compact local vertex set (sorted global ids -> local).
        u32 *uv = (u32 *)malloc(sizeof(u32) * tri.n);
        memcpy(uv, tri.p, sizeof(u32) * tri.n);
        msort(uv, tri.n, cmp_u32v, NULL);
        size_t lv = 0;
        for (size_t i = 0; i < tri.n; i++) if (i == 0 || uv[i] != uv[i - 1]) uv[lv++] = uv[i];
        u32 *lidx = (u32 *)malloc(sizeof(u32) * tri.n);
        for (size_t i = 0; i < tri.n; i++) {
            size_t lo = 0, hi = lv;
            while (lo < hi) { size_t mid = (lo + hi) / 2; if (uv[mid] < tri.p[i]) lo = mid + 1; else hi = mid; }
            lidx[i] = (u32)lo;
        }
        // Attributes: normal, uv [, skinned: the 4 weights (sorted slots, at low weight)].
        const size_t NA = b->skinned ? 9 : 5;
        float *lpos = (float *)malloc(sizeof(float) * 3 * lv);
        float *lattr = (float *)malloc(sizeof(float) * NA * lv);
        u8 *lock = (u8 *)malloc(lv);
        u32 key = g * b->material_count + mat;
        for (size_t v = 0; v < lv; v++) {
            memcpy(&lpos[v * 3], &b->pos[uv[v] * 3], 12);
            memcpy(&lattr[v * NA], &b->nrm[uv[v] * 3], 12);
            memcpy(&lattr[v * NA + 3], &b->uv[uv[v] * 2], 8);
            for (size_t k = 5; k < NA; k++) lattr[v * NA + k] = (float)b->skin[uv[v] * 8 + 4 + (k - 5)] / 255.0f;
            lock[v] = j->owner[b->pid[uv[v]]] != key;   // shared with another group or material: locked
        }
        if (b->skinned && j->level == 0) {
            // Bone-set penalty, kept simple: a vertex whose dominant bone differs from the dominant bone
            // of every one of its neighbours (a lone vertex of one bone in another's region, e.g. a tip)
            // is locked, so the simplifier never drags it across the bone boundary. Only on the source
            // mesh (the first simplification): at coarser levels earlier collapses isolate vertices,
            // and locking those pinched thin parts (the test creature: 90 pinched cut edges vs 0).
            // Beyond that the 4 weights are simplification attributes.
            u8 *nb = (u8 *)calloc(lv, 1);   // bit 0: has a neighbour, bit 1: one shares the dominant bone
            for (size_t t = 0; t < tri.n; t += 3)
                for (int e = 0; e < 3; e++) {
                    u32 x = lidx[t + e], y = lidx[t + (e + 1) % 3];
                    u8 same = b->skin[uv[x] * 8] == b->skin[uv[y] * 8] ? 2 : 0;
                    nb[x] |= 1 | same;
                    nb[y] |= 1 | same;
                }
            for (size_t v = 0; v < lv; v++) if (nb[v] == 1) lock[v] = 1;
            free(nb);
        }
        float wn = b->radius * 0.01f, wt = b->radius * 0.005f;
        float weights[9] = { wn, wn, wn, wt, wt, wt, wt, wt, wt };
        size_t target = (tri.n / 3 / 2) * 3;
        if (target < 3) target = 3;
        u32 *dst = (u32 *)malloc(sizeof(u32) * tri.n);
        float e = 0;
        size_t r = meshopt_simplifyWithAttributes(dst, lidx, tri.n, lpos, lv, sizeof(float) * 3, lattr, sizeof(float) * NA, weights, NA, lock, target,
                                                  b->radius * 4.0f, meshopt_SimplifyErrorAbsolute, &e);
        if (r > tri.n * 3 / 4) {
            // Retry harder; if the lock still holds it, carry the subset through unchanged.
            float e2 = 0;
            size_t r2 = meshopt_simplifyWithAttributes(dst, lidx, tri.n, lpos, lv, sizeof(float) * 3, lattr, sizeof(float) * NA, weights, NA, lock,
                                                       (tri.n / 3 / 4) * 3, b->radius * 16.0f, meshopt_SimplifyErrorAbsolute, &e2);
            if (r2 <= tri.n * 3 / 4) { r = r2; e = e2; }
            else { memcpy(dst, lidx, sizeof(u32) * tri.n); r = tri.n; e = 0; }
        }
        if (r == 0) {   // the subset collapsed entirely (tiny disconnected cards, foliage): carry it unchanged
            memcpy(dst, lidx, sizeof(u32) * tri.n);
            r = tri.n;
            e = 0;
        }
        serr = fmaxf(serr, e);
        o->tris_out += (u32)(r / 3);
        // Re-cluster (lod / error filled in after the group error is final).
        Sph none = { { 0, 0, 0 }, -1.0f };
        meshletize(b, dst, r, lpos, lv, uv, mat, j->level + 1, 0.0f, none, g, &o->clusters, &o->idx);
        free(dst);
        free(lock);
        free(lattr);
        free(lpos);
        free(lidx);
        free(uv);
        VEC_FREE(tri);
    }
    o->error = fmaxf(serr, maxerr);
}

// ------------------------------------------------------------------ BVH

typedef struct BvhItem { Sph s; float err; u32 id; } BvhItem;
typedef VEC(VgNode) VecNode;
typedef struct Bvh { VecNode nodes; BvhItem *items; int axis; } Bvh;

static int cmp_item_axis(const void *ctx, u32 a, u32 c) {
    const Bvh *h = (const Bvh *)ctx;
    float x = h->items[a].s.c[h->axis], y = h->items[c].s.c[h->axis];
    if (x < y) return -1;
    if (x > y) return 1;
    return h->items[a].id < h->items[c].id ? -1 : (h->items[a].id > h->items[c].id ? 1 : 0);
}

static void node_fill(VgNode *n, Sph s, float err) {
    n->center[0] = s.c[0];
    n->center[1] = s.c[1];
    n->center[2] = s.c[2];
    n->radius = s.r;
    n->max_parent_error = err;
}

// Builds into node slot `slot` over items[order[0..n)], leaves referencing group indices
// leaf_base + position in `order` (order is reordered in place: the leaves' order).
static void bvh_build(Bvh *h, u32 slot, u32 *order, u32 n, u32 *order_base, u32 leaf_base) {
    Sph s = { { 0, 0, 0 }, -1.0f };
    float err = 0;
    for (u32 i = 0; i < n; i++) {
        s = sph_merge(s, h->items[order[i]].s);
        err = fmaxf(err, h->items[order[i]].err);
    }
    if (n <= 4) {
        VgNode *nd = &h->nodes.p[slot];
        memset(nd, 0, sizeof(*nd));
        node_fill(nd, s, err);
        nd->first = leaf_base + (u32)(order - order_base);
        nd->count = (u16)n;
        nd->flags = VGN_LEAF;
        return;
    }
    // Split in 4 by count along the widest axis of the item centres.
    float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (u32 i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) { mn[k] = fminf(mn[k], h->items[order[i]].s.c[k]); mx[k] = fmaxf(mx[k], h->items[order[i]].s.c[k]); }
    int ax = 0;
    for (int k = 1; k < 3; k++) if (mx[k] - mn[k] > mx[ax] - mn[ax]) ax = k;
    h->axis = ax;
    msort(order, n, cmp_item_axis, h);
    u32 child = (u32)h->nodes.n;
    VgNode z;
    memset(&z, 0, sizeof(z));
    for (int c = 0; c < 4; c++) VEC_PUSH(h->nodes, z);
    VgNode *nd = &h->nodes.p[slot];
    memset(nd, 0, sizeof(*nd));
    node_fill(nd, s, err);
    nd->first = child;
    nd->count = 4;
    u32 cuts[5] = { 0, n / 4, n / 2, n - n / 4, n };
    for (int c = 0; c < 4; c++) bvh_build(h, child + (u32)c, order + cuts[c], cuts[c + 1] - cuts[c], order_base, leaf_base);
}

// A root over existing nodes (the per-level roots): copies them into contiguous child slots.
static u32 bvh_top(Bvh *h, const u32 *roots, u32 n) {
    if (n == 1) return roots[0];
    u32 cur[VGEO_MAX_LEVELS];
    memcpy(cur, roots, sizeof(u32) * n);
    while (n > 1) {
        u32 next[VGEO_MAX_LEVELS], nn = 0;
        for (u32 i = 0; i < n; i += 4) {
            u32 k = DG_MIN(4u, n - i);
            u32 child = (u32)h->nodes.n;
            for (u32 c = 0; c < k; c++) { VgNode copy = h->nodes.p[cur[i + c]]; VEC_PUSH(h->nodes, copy); }
            Sph s = { { 0, 0, 0 }, -1.0f };
            float err = 0;
            for (u32 c = 0; c < k; c++) {
                const VgNode *cn = &h->nodes.p[child + c];
                Sph cs = { { cn->center[0], cn->center[1], cn->center[2] }, cn->radius };
                s = sph_merge(s, cs);
                err = fmaxf(err, cn->max_parent_error);
            }
            VgNode p;
            memset(&p, 0, sizeof(p));
            node_fill(&p, s, err);
            p.first = child;
            p.count = (u16)k;
            next[nn++] = (u32)h->nodes.n;
            VEC_PUSH(h->nodes, p);
        }
        memcpy(cur, next, sizeof(u32) * nn);
        n = nn;
    }
    return cur[0];
}

// ------------------------------------------------------------------ pack

typedef struct PackCluster { u32 build; u32 vc; u32 verts[VGEO_MAX_CLUSTER_VERTS]; u8 tri[VGEO_MAX_CLUSTER_TRIS][3]; } PackCluster;

static void pack_local(const B *b, const BCluster *c, PackCluster *pc) {
    pc->vc = 0;
    for (u32 t = 0; t < c->tris; t++)
        for (int k = 0; k < 3; k++) {
            u32 v = b->idx.p[c->first + t * 3 + k], f = 0;
            while (f < pc->vc && pc->verts[f] != v) f++;
            if (f == pc->vc) pc->verts[pc->vc++] = v;
            pc->tri[t][k] = (u8)f;
        }
}

static u32 vertex_bytes(const B *b) { return vgeo_vertex_bytes((b->has_color ? VGF_HAS_COLOR : 0) | (b->skinned ? VGF_SKINNED : 0)); }

// Skinned: the cluster's bone set, ascending bone ids, with the bind-space box of the vertices that
// have a non-zero weight on each bone. Returns the count (0 when static).
static u32 cluster_bone_set(const B *b, const PackCluster *pc, VgBoneBox *out) {
    if (!b->skinned) return 0;
    u64 mask[2] = { 0, 0 };
    for (u32 v = 0; v < pc->vc; v++) skin_mask(b, pc->verts[v], mask);
    u32 n = 0;
    for (u32 bone = 0; bone < VGEO_MAX_BONES && n < VGEO_MAX_CLUSTER_BONES; bone++) {
        if (!(mask[bone >> 6] >> (bone & 63) & 1)) continue;
        VgBoneBox *bx = &out[n++];
        bx->bone = bone;
        for (int k = 0; k < 3; k++) { bx->min[k] = FLT_MAX; bx->max[k] = -FLT_MAX; }
        for (u32 v = 0; v < pc->vc; v++) {
            const u8 *s = &b->skin[pc->verts[v] * 8];
            bool on = false;
            for (int k = 0; k < 4; k++) on |= s[k] == bone && s[4 + k];
            if (!on) continue;
            for (int k = 0; k < 3; k++) {
                bx->min[k] = fminf(bx->min[k], b->pos[pc->verts[v] * 3 + k]);
                bx->max[k] = fmaxf(bx->max[k], b->pos[pc->verts[v] * 3 + k]);
            }
        }
    }
    return n;
}

static u32 bone_set_bytes(const B *b, u32 bones) { return b->skinned ? 4u + bones * VGEO_BONE_BOX_BYTES : 0u; }

static u32 cluster_bytes(const B *b, const PackCluster *pc, u32 tris) {
    VgBoneBox bx[VGEO_MAX_CLUSTER_BONES];
    u32 nb = cluster_bone_set(b, pc, bx);
    return (u32)sizeof(VgCluster) + ((pc->vc * vertex_bytes(b) + 15u) & ~15u) + ((tris * VGEO_TRI_BYTES + bone_set_bytes(b, nb) + 15u) & ~15u);
}


// Writes the open page: header, clusters, vertex block, triangle block (16-byte aligned blocks).
static void flush_page(const B *b, VgPageDesc *pd, VecU32 *page_cl, VecU8 *data, const u32 *gfinal) {
    u32 ncl = (u32)page_cl->n;
    if (!ncl) return;
    u32 vb = vertex_bytes(b);
    PackCluster *pcs = (PackCluster *)malloc(sizeof(PackCluster) * ncl);
    u32 vbytes = 0, tbytes = 0, sbytes = 0;
    VgBoneBox bx[VGEO_MAX_CLUSTER_BONES];
    for (u32 q = 0; q < ncl; q++) {
        const BCluster *bc = &b->clusters.p[page_cl->p[q]];
        pack_local(b, bc, &pcs[q]);
        u32 nb = bone_set_bytes(b, cluster_bone_set(b, &pcs[q], bx));
        vbytes += (pcs[q].vc * vb + 15u) & ~15u;
        tbytes += (bc->tris * VGEO_TRI_BYTES + nb + 15u) & ~15u;   // skinned: the bone set follows the triangles
        sbytes += nb;
    }
    u32 size = (u32)sizeof(VgPageHeader) + ncl * (u32)sizeof(VgCluster) + vbytes + tbytes;
    size_t at = data->n;
    VEC_RESERVE(*data, size);
    memset(&data->p[at], 0, size);
    data->n += size;
    u8 *page = &data->p[at];
    VgPageHeader *ph = (VgPageHeader *)page;
    ph->cluster_count = ncl;
    ph->vertex_bytes = vbytes;
    ph->index_bytes = tbytes;
    ph->skin_bytes = sbytes;   // the bone sets inside index_bytes
    u32 voff = (u32)sizeof(VgPageHeader) + ncl * (u32)sizeof(VgCluster), toff = voff + vbytes;
    for (u32 q = 0; q < ncl; q++) {
        const BCluster *bc = &b->clusters.p[page_cl->p[q]];
        const PackCluster *pc = &pcs[q];
        VgCluster *vc = (VgCluster *)(page + sizeof(VgPageHeader) + q * sizeof(VgCluster));
        memcpy(vc->center, bc->bounds.c, 12);
        vc->radius = bc->bounds.r;
        memcpy(vc->lod_center, bc->lod.c, 12);
        vc->lod_radius = bc->lod.r;
        vc->error = bc->error;
        vc->cone = bc->cone;
        vc->counts = pc->vc | (bc->tris << 8) | (bc->material << 16);
        vc->offsets = (voff / 4u) | ((toff / 4u) << 15);
        vc->child_group = bc->child_group == ~0u ? ~0u : gfinal[bc->child_group];
        i32 gmin[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
        for (u32 v = 0; v < pc->vc; v++)
            for (int k = 0; k < 3; k++) gmin[k] = DG_MIN(gmin[k], b->grid[pc->verts[v] * 3 + k]);
        memcpy(vc->base, gmin, 12);
        for (u32 v = 0; v < pc->vc; v++) {
            u8 *vp = page + voff + v * vb;
            for (int k = 0; k < 3; k++) {
                u16 o16 = (u16)DG_CLAMP(b->grid[pc->verts[v] * 3 + k] - gmin[k], 0, 65535);
                memcpy(vp + k * 2, &o16, 2);
            }
            u16 on = (u16)oct_encode8(&b->nrm[pc->verts[v] * 3]);
            u16 hu = vgeo_half(b->uv[pc->verts[v] * 2]), hv = vgeo_half(b->uv[pc->verts[v] * 2 + 1]);
            memcpy(vp + 6, &on, 2);
            memcpy(vp + 8, &hu, 2);
            memcpy(vp + 10, &hv, 2);
            if (b->has_color) memcpy(vp + 12, &b->col[pc->verts[v]], 4);
            if (b->skinned) memcpy(vp + (b->has_color ? 16 : 12), &b->skin[pc->verts[v] * 8], 8);
        }
        for (u32 t = 0; t < bc->tris; t++) {
            u8 *tp = page + toff + t * VGEO_TRI_BYTES;
            tp[0] = pc->tri[t][0];
            tp[1] = pc->tri[t][1];
            tp[2] = pc->tri[t][2];
        }
        // Bone set: count, then { bone | half min.x << 16, min.y | min.z << 16, max.x | max.y << 16, max.z }
        // with the decoded-position box rounded outwards (the GPU's f32 decode may differ by an ulp).
        u32 nb = cluster_bone_set(b, pc, bx);
        if (b->skinned) {
            u32 *w = (u32 *)(page + toff + bc->tris * VGEO_TRI_BYTES);
            w[0] = nb;
            for (u32 i = 0; i < nb; i++) {
                u16 lo[3], hi[3];
                for (int k = 0; k < 3; k++) {
                    float a = bx[i].min[k], c = bx[i].max[k];
                    lo[k] = half_down(a - (fabsf(a) * 1e-6f + 1e-7f));
                    hi[k] = half_up(c + (fabsf(c) * 1e-6f + 1e-7f));
                }
                u32 *e = &w[1 + i * 4];
                e[0] = bx[i].bone | (u32)lo[0] << 16;
                e[1] = lo[1] | (u32)lo[2] << 16;
                e[2] = hi[0] | (u32)hi[1] << 16;
                e[3] = hi[2];
            }
        }
        voff += (pc->vc * vb + 15u) & ~15u;
        toff += (bc->tris * VGEO_TRI_BYTES + bone_set_bytes(b, nb) + 15u) & ~15u;
    }
    pd->size = size;
    page_cl->n = 0;
    free(pcs);
}

// ------------------------------------------------------------------ build

static void stats_level(B *b, VgBuildStats *st, u32 level, const u32 *ids, u32 n) {
    if (level >= VGEO_MAX_LEVELS) return;
    VgLevelStats *ls = &st->level[level];
    ls->clusters = n;
    ls->err_min = FLT_MAX;
    ls->err_max = 0;
    for (u32 i = 0; i < n; i++) {
        const BCluster *c = &b->clusters.p[ids[i]];
        ls->tris += c->tris;
        ls->err_min = fminf(ls->err_min, c->error);
        ls->err_max = fmaxf(ls->err_max, c->error);
    }
    if (st->levels < level + 1) st->levels = level + 1;
}

static void free_b(B *b) {
    free(b->pos);
    free(b->nrm);
    free(b->uv);
    free(b->col);
    free(b->pid);
    free(b->grid);
    free(b->skin);
    VEC_FREE(b->clusters);
    VEC_FREE(b->idx);
    VEC_FREE(b->groups);
    VEC_FREE(b->members);
}

bool vgeo_build(const VgSource *src, const VgBuildOptions *optp, VgBuilt *out, char *err, int err_size) {
    static const VgBuildOptions def_opt = { 0 };
    const VgBuildOptions *opt = optp ? optp : &def_opt;
    memset(out, 0, sizeof(*out));
    if (err_size > 0) err[0] = 0;
    if (!src || !src->pos || !src->indices || src->index_count < 3 || src->vertex_count < 3) { snprintf(err, (size_t)err_size, "empty mesh"); return false; }
    double t0 = now_ms(), t;
    B b;
    memset(&b, 0, sizeof(b));
    b.opt = opt;
    b.threads = resolve_threads(opt->threads);
    b.material_count = DG_MAX(src->material_count, 1u);
    b.has_color = src->color != NULL;
    b.skinned = src->joints != NULL;
    VgBuildStats *st = &out->stats;
    st->threads = b.threads;
    st->input_tris = src->index_count / 3;
    st->input_verts = src->vertex_count;
    // 1. weld
    u32 *tri_idx = NULL, tri_icount = 0;
    u16 *tri_mat = NULL;
    t = now_ms();
    if (!weld(&b, src, &tri_idx, &tri_icount, &tri_mat, err, err_size)) { free_b(&b); return false; }
    st->welded_verts = b.vcount;
    st->ms_weld = now_ms() - t;
    if (tri_icount < 3) { snprintf(err, (size_t)err_size, "no triangles left after welding"); free(tri_idx); free(tri_mat); free_b(&b); return false; }
    {
        float d[3] = { b.aabb_max[0] - b.aabb_min[0], b.aabb_max[1] - b.aabb_min[1], b.aabb_max[2] - b.aabb_min[2] };
        b.radius = 0.5f * sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    // 2. leaf clusters, per material
    t = now_ms();
    for (u32 m = 0; m < b.material_count; m++) {
        VecU32 mi = { 0 };
        for (u32 tt = 0; tt < tri_icount / 3; tt++)
            if ((tri_mat[tt] % b.material_count) == m) {
                VEC_RESERVE(mi, 3);
                for (int k = 0; k < 3; k++) mi.p[mi.n++] = tri_idx[tt * 3 + k];
            }
        if (mi.n) {
            Sph none = { { 0, 0, 0 }, -1.0f };
            meshletize(&b, mi.p, mi.n, b.pos, b.vcount, NULL, m, 0, 0.0f, none, ~0u, &b.clusters, &b.idx);
        }
        VEC_FREE(mi);
    }
    free(tri_idx);
    free(tri_mat);
    st->ms_leaf = now_ms() - t;
    // 3. DAG
    t = now_ms();
    VecU32 lvl = { 0 };
    for (u32 i = 0; i < (u32)b.clusters.n; i++) VEC_PUSH(lvl, i);
    u32 *owner = (u32 *)malloc(sizeof(u32) * b.pcount);
    u32 level = 0;
    for (;; level++) {
        stats_level(&b, st, level, lvl.p, (u32)lvl.n);
        if (lvl.n <= 1 || level + 1 >= VGEO_MAX_LEVELS) break;
        u32 g0 = (u32)b.groups.n;
        u32 ng = partition(&b, lvl.p, (u32)lvl.n, level, -1.0f);
        if (level < VGEO_MAX_LEVELS) st->level[level].groups = ng;
        // Ownership of positions by (group, material): anything shared is locked.
        for (u32 p = 0; p < b.pcount; p++) owner[p] = ~0u - 1u;
        for (u32 i = 0; i < (u32)lvl.n; i++) {
            const BCluster *c = &b.clusters.p[lvl.p[i]];
            u32 key = c->group * b.material_count + c->material;
            for (u32 k = 0; k < c->tris * 3; k++) {
                u32 p = b.pid[b.idx.p[c->first + k]];
                if (owner[p] == ~0u - 1u) owner[p] = key;
                else if (owner[p] != key) owner[p] = VG_MULTI;
            }
        }
        GroupJob job;
        memset(&job, 0, sizeof(job));
        job.b = &b;
        job.first_group = g0;
        job.level = level;
        job.owner = owner;
        job.out = calloc(ng, sizeof(*job.out));
        par_for(b.threads, ng, group_job, &job);
        // Append the new clusters in group order (deterministic), with their final error / lod.
        VecU32 next = { 0 };
        u32 tin = 0, tout = 0;
        for (u32 gi = 0; gi < ng; gi++) {
            struct GroupOut *o = &job.out[gi];
            BGroup *gr = &b.groups.p[g0 + gi];
            gr->parent_error = o->error;
            gr->unreducible = o->tris_out * 4 > o->tris_in * 3;
            if (gr->unreducible && level < VGEO_MAX_LEVELS) st->level[level].unreducible++;
            tin += o->tris_in;
            tout += o->tris_out;
            u32 base = (u32)b.idx.n;
            VEC_RESERVE(b.idx, o->idx.n);
            memcpy(&b.idx.p[b.idx.n], o->idx.p, sizeof(u32) * o->idx.n);
            b.idx.n += o->idx.n;
            for (size_t c = 0; c < o->clusters.n; c++) {
                BCluster nc = o->clusters.p[c];
                nc.first += base;
                nc.error = o->error;
                nc.lod = gr->lod;
                VEC_PUSH(next, (u32)b.clusters.n);
                VEC_PUSH(b.clusters, nc);
            }
            VEC_FREE(o->clusters);
            VEC_FREE(o->idx);
        }
        free(job.out);
        VEC_FREE(lvl);
        lvl = next;
        // No progress at all (every group locked solid): stop, the rest becomes the root.
        if (tout * 20 > tin * 19) {
            level++;
            stats_level(&b, st, level, lvl.p, (u32)lvl.n);
            break;
        }
    }
    free(owner);
    // Root group(s): the last level, parent error infinite.
    {
        u32 g0 = (u32)b.groups.n;
        u32 ng = partition(&b, lvl.p, (u32)lvl.n, level, FLT_MAX);
        if (level < VGEO_MAX_LEVELS) st->level[level].groups = ng;
        (void)g0;
    }
    VEC_FREE(lvl);
    st->ms_dag = now_ms() - t;
    // 4. pack: group order coarse -> fine, BVH order within a level.
    t = now_ms();
    u32 levels = 0;
    for (size_t g = 0; g < b.groups.n; g++) levels = DG_MAX(levels, b.groups.p[g].level + 1);
    Bvh h;
    memset(&h, 0, sizeof(h));
    h.items = (BvhItem *)malloc(sizeof(BvhItem) * b.groups.n);
    for (size_t g = 0; g < b.groups.n; g++) {
        h.items[g].s = sph_merge(b.groups.p[g].bounds, b.groups.p[g].lod);
        h.items[g].err = b.groups.p[g].parent_error;
        h.items[g].id = (u32)g;
    }
    u32 *gorder = (u32 *)malloc(sizeof(u32) * b.groups.n);   // final position -> build group
    u32 roots[VGEO_MAX_LEVELS], nroots = 0, pos = 0;
    VgNode z;
    memset(&z, 0, sizeof(z));
    for (int L = (int)levels - 1; L >= 0; L--) {
        u32 start = pos;
        for (size_t g = 0; g < b.groups.n; g++) if (b.groups.p[g].level == (u32)L) gorder[pos++] = (u32)g;
        if (pos == start) continue;
        u32 slot = (u32)h.nodes.n;
        VEC_PUSH(h.nodes, z);
        bvh_build(&h, slot, gorder + start, pos - start, gorder + start, start);
        roots[nroots++] = slot;
    }
    u32 root = bvh_top(&h, roots, nroots);
    u32 *gfinal = (u32 *)malloc(sizeof(u32) * b.groups.n);   // build group -> final index
    for (u32 i = 0; i < (u32)b.groups.n; i++) gfinal[gorder[i]] = i;
    // Pages: groups in order, a group never spans pages.
    struct { VgPageDesc *p; size_t n, cap; } pages = { 0 };
    VecU8 data = { 0 };
    out->groups = (VgGroup *)calloc(b.groups.n, sizeof(VgGroup));
    PackCluster *pc = (PackCluster *)malloc(sizeof(PackCluster) * 16);
    u32 page_used = VGEO_PAGE_SIZE, page_clusters = 0;
    VecU32 page_cl = { 0 };   // build clusters of the open page
    u32 cluster_total = 0;
    u32 top_level = levels ? levels - 1 : 0, root_pages = 0;
    VgDbgCluster *dbg = NULL;
    if (opt->keep_build_data) dbg = (VgDbgCluster *)calloc(b.clusters.n, sizeof(VgDbgCluster));
    for (u32 gi = 0; gi < (u32)b.groups.n; gi++) {
        const BGroup *bg = &b.groups.p[gorder[gi]];
        u32 gbytes = 0;
        for (u32 m = 0; m < bg->member_count; m++) {
            const BCluster *c = &b.clusters.p[b.members.p[bg->first_member + m]];
            pack_local(&b, c, &pc[0]);
            gbytes += cluster_bytes(&b, &pc[0], c->tris);
        }
        if (page_used + gbytes > VGEO_PAGE_SIZE || page_clusters + bg->member_count > 4096) {
            if (pages.n) flush_page(&b, &pages.p[pages.n - 1], &page_cl, &data, gfinal);
            VgPageDesc pd;
            memset(&pd, 0, sizeof(pd));
            pd.file_offset = (u32)data.n;
            pd.first_group = gi;
            VEC_PUSH(pages, pd);
            page_used = (u32)sizeof(VgPageHeader);
            page_clusters = 0;
        }
        VgGroup *og = &out->groups[gi];
        memcpy(og->lod_center, bg->lod.c, 12);
        og->lod_radius = bg->lod.r;
        og->parent_error = bg->parent_error;
        og->page = (u32)pages.n - 1;
        og->first_cluster = (u16)page_cl.n;
        og->cluster_count = (u16)bg->member_count;
        og->level = bg->level;
        pages.p[pages.n - 1].group_count++;
        if (bg->level + 2 >= top_level) root_pages = (u32)pages.n;
        for (u32 m = 0; m < bg->member_count; m++) {
            u32 cid = b.members.p[bg->first_member + m];
            if (dbg) {
                VgDbgCluster *d = &dbg[cid];
                const BCluster *c = &b.clusters.p[cid];
                d->first_index = c->first;
                d->tri_count = c->tris;
                d->group = gi;
                d->child_group = c->child_group == ~0u ? ~0u : gfinal[c->child_group];
                d->level = c->level;
                d->error = c->error;
                memcpy(d->lod_center, c->lod.c, 12);
                d->lod_radius = c->lod.r;
                memcpy(d->center, c->bounds.c, 12);
                d->radius = c->bounds.r;
            }
            VEC_PUSH(page_cl, cid);
            cluster_total++;
        }
        page_used += gbytes;
        page_clusters += bg->member_count;
    }
    if (pages.n) flush_page(&b, &pages.p[pages.n - 1], &page_cl, &data, gfinal);
    free(pc);
    VEC_FREE(page_cl);
    st->ms_pack = now_ms() - t;
    // 5. discrete LOD cuts (object-space errors)
    t = now_ms();
    int lodn = opt->lod_count > 0 ? DG_MIN(opt->lod_count, VGEO_MAX_LODS) : VGEO_MAX_LODS;
    static const float def_rel[VGEO_MAX_LODS] = { 0.0f, 0.0025f, 0.01f, 0.04f };
    VecU32 lidx = { 0 };
    struct { VgLodVertex *p; size_t n, cap; } lvert = { 0 };
    u32 *vmap = (u32 *)malloc(sizeof(u32) * b.vcount);
    for (int l = 0; l < lodn; l++) {
        float rel = opt->lod_error_rel[l] > 0 ? opt->lod_error_rel[l] : def_rel[l];
        float te = rel * b.radius;
        VgDiscreteLod *L = &out->lods[l];
        L->error = te;
        L->first_index = (u32)lidx.n;
        L->first_vertex = (u32)lvert.n;
        for (u32 v = 0; v < b.vcount; v++) vmap[v] = ~0u;
        for (size_t g = 0; g < b.groups.n; g++) {
            const BGroup *bg = &b.groups.p[g];
            if (!(bg->parent_error > te)) continue;
            for (u32 m = 0; m < bg->member_count; m++) {
                const BCluster *c = &b.clusters.p[b.members.p[bg->first_member + m]];
                if (!(c->error <= te)) continue;
                for (u32 k = 0; k < c->tris * 3; k++) {
                    u32 v = b.idx.p[c->first + k];
                    if (vmap[v] == ~0u) {
                        vmap[v] = (u32)(lvert.n - L->first_vertex);
                        VgLodVertex lv;
                        memcpy(lv.pos, &b.pos[v * 3], 12);
                        memcpy(lv.normal, &b.nrm[v * 3], 12);
                        memcpy(lv.uv, &b.uv[v * 2], 8);
                        VEC_PUSH(lvert, lv);
                    }
                    VEC_PUSH(lidx, vmap[v]);
                }
                L->tris += c->tris;
            }
        }
        L->index_count = (u32)lidx.n - L->first_index;
        L->vertex_count = (u32)lvert.n - L->first_vertex;
    }
    free(vmap);
    st->ms_lods = now_ms() - t;
    // Output
    out->nodes = h.nodes.p;
    out->pages = pages.p;
    out->page_data = data.p;
    out->page_data_size = (u32)data.n;
    out->lod_indices = lidx.p;
    out->lod_vertices = lvert.p;
    out->materials = (VgMaterialRef *)calloc(b.material_count, sizeof(VgMaterialRef));
    for (u32 m = 0; m < b.material_count; m++) {
        if (src->material_names && m < src->material_count) dg_strcpy(out->materials[m].name, sizeof(out->materials[m].name), src->material_names[m]);
        out->materials[m].slot = m;
    }
    VgFileHeader *hd = &out->hdr;
    hd->magic = VGEO_MAGIC;
    hd->version = VGEO_FILE_VERSION;
    hd->flags = (b.has_color ? VGF_HAS_COLOR : 0) | (b.skinned ? VGF_SKINNED : 0);
    hd->bone_count = b.bone_count;
    hd->cluster_count = cluster_total;
    hd->group_count = (u32)b.groups.n;
    hd->node_count = (u32)h.nodes.n;
    hd->page_count = (u32)pages.n;
    hd->root_page_count = DG_MAX(1u, DG_MIN(root_pages, (u32)pages.n));
    hd->lod_count = (u32)lodn;
    hd->material_count = b.material_count;
    hd->level_count = levels;
    for (int k = 0; k < 3; k++) { hd->aabb_min[k] = b.aabb_min[k]; hd->aabb_max[k] = b.aabb_max[k]; }
    hd->q_step = b.q_step;
    {
        Sph s = { { 0, 0, 0 }, -1.0f };
        for (size_t g = 0; g < b.groups.n; g++) if (b.groups.p[g].level == 0) s = sph_merge(s, b.groups.p[g].bounds);
        memcpy(hd->sphere, s.c, 12);
        hd->sphere[3] = s.r;
    }
    hd->root_node = root;
    st->clusters = cluster_total;
    st->groups = (u32)b.groups.n;
    st->nodes = (u32)h.nodes.n;
    st->pages = (u32)pages.n;
    st->root_pages = hd->root_page_count;
    st->page_bytes = data.n;
    if (opt->keep_build_data) {
        out->dbg.vertex_count = b.vcount;
        out->dbg.pos = b.pos;
        out->dbg.pos_id = b.pid;
        b.pos = NULL;
        b.pid = NULL;
        out->dbg.cluster_count = (u32)b.clusters.n;
        out->dbg.clusters = dbg;
        out->dbg.indices = b.idx.p;
        b.idx.p = NULL;
    }
    free(h.items);
    free(gorder);
    free(gfinal);
    free_b(&b);
    st->ms_total = now_ms() - t0;
    return true;
}

void vgeo_free(VgBuilt *b) {
    if (!b) return;
    free(b->nodes);
    free(b->groups);
    free(b->pages);
    free(b->materials);
    free(b->lod_indices);
    free(b->lod_vertices);
    free(b->page_data);
    free(b->impostor);
    free(b->dbg.pos);
    free(b->dbg.pos_id);
    free(b->dbg.clusters);
    free(b->dbg.indices);
    memset(b, 0, sizeof(*b));
}

// ------------------------------------------------------------------ file

bool vgeo_serialize(const VgBuilt *b, u32 key, u8 **data, size_t *size) {
    VgFileHeader h = b->hdr;
    h.key = key;
    // Stats without the run-dependent fields: the file is a pure function of the input and options.
    VgBuildStats st = b->stats;
    st.ms_weld = st.ms_leaf = st.ms_dag = st.ms_pack = st.ms_lods = st.ms_total = 0;
    st.threads = 0;
    struct { const void *p; u32 size; } sec[VGS_COUNT] = {
        { b->nodes, h.node_count * (u32)sizeof(VgNode) },
        { b->groups, h.group_count * (u32)sizeof(VgGroup) },
        { b->pages, h.page_count * (u32)sizeof(VgPageDesc) },
        { b->materials, h.material_count * (u32)sizeof(VgMaterialRef) },
        { b->lods, h.lod_count * (u32)sizeof(VgDiscreteLod) },
        { b->lod_indices, 0 },
        { b->lod_vertices, 0 },
        { b->impostor, b->impostor_size },   // vgeo_impostor.h (VGF_HAS_IMPOSTOR)
        { b->page_data, b->page_data_size },
        { &st, (u32)sizeof(VgBuildStats) },
    };
    u32 li = 0, lv = 0;
    for (u32 l = 0; l < h.lod_count; l++) {
        li = DG_MAX(li, b->lods[l].first_index + b->lods[l].index_count);
        lv = DG_MAX(lv, b->lods[l].first_vertex + b->lods[l].vertex_count);
    }
    sec[VGS_LOD_INDICES].size = li * 4;
    sec[VGS_LOD_VERTICES].size = lv * (u32)sizeof(VgLodVertex);
    u32 at = (u32)sizeof(VgFileHeader);
    for (int s = 0; s < VGS_COUNT; s++) {
        at = (at + 15u) & ~15u;
        h.sections[s].offset = at;
        h.sections[s].size = sec[s].size;
        at += sec[s].size;
    }
    u8 *buf = (u8 *)calloc(1, at);
    memcpy(buf, &h, sizeof(h));
    for (int s = 0; s < VGS_COUNT; s++) if (sec[s].size) memcpy(buf + h.sections[s].offset, sec[s].p, sec[s].size);
    *data = buf;
    *size = at;
    return true;
}

bool vgeo_deserialize(const u8 *data, size_t size, VgBuilt *out) {
    memset(out, 0, sizeof(*out));
    if (size < sizeof(VgFileHeader)) return false;
    VgFileHeader h;
    memcpy(&h, data, sizeof(h));
    if (h.magic != VGEO_MAGIC || h.version != VGEO_FILE_VERSION) return false;
    for (int s = 0; s < VGS_COUNT; s++) if ((size_t)h.sections[s].offset + h.sections[s].size > size) return false;
    #define TAKE(dst, s) do { (dst) = malloc(h.sections[s].size ? h.sections[s].size : 1); memcpy((dst), data + h.sections[s].offset, h.sections[s].size); } while (0)
    TAKE(out->nodes, VGS_NODES);
    TAKE(out->groups, VGS_GROUPS);
    TAKE(out->pages, VGS_PAGES);
    TAKE(out->materials, VGS_MATERIALS);
    TAKE(out->lod_indices, VGS_LOD_INDICES);
    TAKE(out->lod_vertices, VGS_LOD_VERTICES);
    TAKE(out->page_data, VGS_PAGE_DATA);
    if (h.sections[VGS_IMPOSTOR].size) {
        TAKE(out->impostor, VGS_IMPOSTOR);
        out->impostor_size = h.sections[VGS_IMPOSTOR].size;
    }
    #undef TAKE
    memcpy(out->lods, data + h.sections[VGS_LODS].offset, DG_MIN(h.sections[VGS_LODS].size, (u32)sizeof(out->lods)));
    if (h.sections[VGS_STATS].size == sizeof(VgBuildStats)) memcpy(&out->stats, data + h.sections[VGS_STATS].offset, sizeof(VgBuildStats));
    out->page_data_size = h.sections[VGS_PAGE_DATA].size;
    out->hdr = h;
    return true;
}

// ------------------------------------------------------------------ decode / cut

const VgPageHeader *vgeo_page(const VgBuilt *b, u32 page) {
    if (page >= b->hdr.page_count) return NULL;
    return (const VgPageHeader *)(b->page_data + b->pages[page].file_offset);
}

const VgCluster *vgeo_page_cluster(const VgBuilt *b, u32 page, u32 index) {
    const VgPageHeader *ph = vgeo_page(b, page);
    if (!ph || index >= ph->cluster_count) return NULL;
    return (const VgCluster *)((const u8 *)ph + sizeof(VgPageHeader) + index * sizeof(VgCluster));
}

void vgeo_decode_cluster(const VgBuilt *b, u32 page, u32 index, VgDecoded *o) {
    memset(o, 0, sizeof(*o));
    const VgCluster *c = vgeo_page_cluster(b, page, index);
    if (!c) return;
    const u8 *base = (const u8 *)vgeo_page(b, page);
    o->vert_count = c->counts & 0xFFu;
    o->tri_count = (c->counts >> 8) & 0xFFu;
    o->material = c->counts >> 16;
    u32 voff = (c->offsets & 0x7FFFu) * 4u, toff = ((c->offsets >> 15) & 0x7FFFu) * 4u;
    u32 vb = vgeo_vertex_bytes(b->hdr.flags), skin_at = (b->hdr.flags & VGF_HAS_COLOR) ? 16 : 12;
    for (u32 v = 0; v < o->vert_count; v++) {
        if (b->hdr.flags & VGF_SKINNED) {
            memcpy(o->joints[v], base + voff + v * vb + skin_at, 4);
            memcpy(o->weights[v], base + voff + v * vb + skin_at + 4, 4);
        }
        const u8 *vp = base + voff + v * vb;
        o->color[v] = 0xFFFFFFFFu;
        if (b->hdr.flags & VGF_HAS_COLOR) memcpy(&o->color[v], vp + 12, 4);
        for (int k = 0; k < 3; k++) {
            u16 q;
            memcpy(&q, vp + k * 2, 2);
            o->grid[v][k] = c->base[k] + (i32)q;
            o->pos[v][k] = (float)((double)o->grid[v][k] * b->hdr.q_step);
        }
        u16 on, hu, hv;
        memcpy(&on, vp + 6, 2);
        memcpy(&hu, vp + 8, 2);
        memcpy(&hv, vp + 10, 2);
        oct_decode8(on, o->normal[v]);
        o->uv[v][0] = vgeo_half_to_float(hu);
        o->uv[v][1] = vgeo_half_to_float(hv);
    }
    for (u32 t = 0; t < o->tri_count; t++) {
        const u8 *tp = base + toff + t * 4;
        o->tri[t][0] = tp[0];
        o->tri[t][1] = tp[1];
        o->tri[t][2] = tp[2];
    }
}

u32 vgeo_cluster_bones(const u8 *page, const VgCluster *c, VgBoneBox out[VGEO_MAX_CLUSTER_BONES]) {
    const u32 *w = (const u32 *)(page + ((c->offsets >> 15) & 0x7FFFu) * 4u) + ((c->counts >> 8) & 0xFFu);
    u32 n = DG_MIN(w[0], (u32)VGEO_MAX_CLUSTER_BONES);
    for (u32 i = 0; i < n; i++) {
        const u32 *e = &w[1 + i * 4];
        out[i].bone = e[0] & 0xFFu;
        out[i].min[0] = vgeo_half_to_float((u16)(e[0] >> 16));
        out[i].min[1] = vgeo_half_to_float((u16)e[1]);
        out[i].min[2] = vgeo_half_to_float((u16)(e[1] >> 16));
        out[i].max[0] = vgeo_half_to_float((u16)e[2]);
        out[i].max[1] = vgeo_half_to_float((u16)(e[2] >> 16));
        out[i].max[2] = vgeo_half_to_float((u16)e[3]);
    }
    return n;
}

void vgeo_bone_bounds(const VgBuilt *b, float out[VGEO_MAX_BONES][4]) {
    float mn[VGEO_MAX_BONES][3], mx[VGEO_MAX_BONES][3];
    for (u32 i = 0; i < VGEO_MAX_BONES; i++)
        for (int k = 0; k < 3; k++) { mn[i][k] = FLT_MAX; mx[i][k] = -FLT_MAX; }
    if (b->hdr.flags & VGF_SKINNED)
        for (u32 p = 0; p < b->hdr.page_count; p++) {
            const u8 *page = (const u8 *)vgeo_page(b, p);
            for (u32 i = 0; i < ((const VgPageHeader *)page)->cluster_count; i++) {
                const VgCluster *c = vgeo_page_cluster(b, p, i);
                if (c->child_group != ~0u) continue;   // coarser clusters keep a subset of the leaves' vertices
                VgBoneBox bx[VGEO_MAX_CLUSTER_BONES];
                u32 n = vgeo_cluster_bones(page, c, bx);
                for (u32 k = 0; k < n; k++)
                    for (int a = 0; a < 3; a++) {
                        mn[bx[k].bone][a] = fminf(mn[bx[k].bone][a], bx[k].min[a]);
                        mx[bx[k].bone][a] = fmaxf(mx[bx[k].bone][a], bx[k].max[a]);
                    }
            }
        }
    for (u32 i = 0; i < VGEO_MAX_BONES; i++) {
        if (mn[i][0] > mx[i][0]) { out[i][0] = out[i][1] = out[i][2] = 0; out[i][3] = -1.0f; continue; }
        float h2 = 0;
        for (int k = 0; k < 3; k++) {
            out[i][k] = (mn[i][k] + mx[i][k]) * 0.5f;
            h2 += (mx[i][k] - mn[i][k]) * (mx[i][k] - mn[i][k]) * 0.25f;
        }
        out[i][3] = sqrtf(h2) * (1.0f + 1e-5f) + 1e-6f;
    }
}

u32 vgeo_cut(const VgBuilt *b, float t, VgCutRef *out, u32 cap) {
    u32 n = 0;
    for (u32 g = 0; g < b->hdr.group_count; g++) {
        const VgGroup *gr = &b->groups[g];
        if (!(gr->parent_error > t)) continue;
        for (u32 i = 0; i < gr->cluster_count; i++) {
            const VgCluster *c = vgeo_page_cluster(b, gr->page, gr->first_cluster + i);
            if (!c || !(c->error <= t)) continue;
            if (out && n < cap) { out[n].page = gr->page; out[n].index = gr->first_cluster + i; out[n].group = g; }
            n++;
        }
    }
    return n;
}

void vgeo_print_stats(const VgBuildStats *s, void (*print)(const char *line)) {
    char l[256];
    snprintf(l, sizeof(l), "vgeo build: %u tris (%u verts, %u welded) -> %u levels, %u clusters, %u groups, %u nodes, %u pages (%u root), %.2f MB, %d threads",
             s->input_tris, s->input_verts, s->welded_verts, s->levels, s->clusters, s->groups, s->nodes, s->pages, s->root_pages, (double)s->page_bytes / (1024.0 * 1024.0), s->threads);
    print(l);
    snprintf(l, sizeof(l), "vgeo build: weld %.0f ms, leaf %.0f ms, dag %.0f ms, pack %.0f ms, lods %.0f ms, total %.0f ms", s->ms_weld, s->ms_leaf, s->ms_dag, s->ms_pack,
             s->ms_lods, s->ms_total);
    print(l);
    for (u32 i = 0; i < s->levels && i < VGEO_MAX_LEVELS; i++) {
        const VgLevelStats *ls = &s->level[i];
        snprintf(l, sizeof(l), "  level %2u: %7u clusters %9u tris %6u groups (%u unreducible)  error %.6g .. %.6g", i, ls->clusters, ls->tris, ls->groups, ls->unreducible,
                 ls->err_min == FLT_MAX ? 0.0 : (double)ls->err_min, (double)ls->err_max);
        print(l);
    }
}
