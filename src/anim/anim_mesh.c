#include "anim_mesh.h"
#include "meshoptimizer.h"
#include <math.h>
#include <stdlib.h>

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

void skin_fit_bone_radii(const SkinMeshData *m, int lod, const Skeleton *s, float *radius) {
    for (int b = 0; b < s->count; b++) radius[b] = 0;
    if (lod < 0 || lod >= m->lod_count) return;
    const SkinLod *L = &m->lods[lod];
    int *counts = (int *)calloc((size_t)s->count, sizeof(int));
    for (u32 i = 0; i < L->vcount; i++) counts[L->verts[i].joints[0]]++;
    float **d = (float **)calloc((size_t)s->count, sizeof(float *));
    int *fill = (int *)calloc((size_t)s->count, sizeof(int));
    for (int b = 0; b < s->count; b++) if (counts[b]) d[b] = (float *)malloc(sizeof(float) * (size_t)counts[b]);
    // Segment per bone: bone origin -> first child origin (or along the bone for leaves).
    vec3 seg_a[ANIM_MAX_BONES], seg_b[ANIM_MAX_BONES];
    for (int b = 0; b < s->count; b++) {
        seg_a[b] = s->bind_model[b].t;
        seg_b[b] = seg_a[b];
        for (int c = b + 1; c < s->count; c++)
            if (s->parent[c] == b) { seg_b[b] = s->bind_model[c].t; break; }
    }
    for (u32 i = 0; i < L->vcount; i++) {
        int j = L->verts[i].joints[0];
        vec3 p = L->verts[i].pos;
        vec3 q = closest_point_segment(p, seg_a[j], seg_b[j]);
        d[j][fill[j]++] = v3_dist(p, q);
    }
    for (int b = 0; b < s->count; b++) {
        if (!counts[b]) continue;
        qsort(d[b], (size_t)counts[b], sizeof(float), cmp_float);
        radius[b] = d[b][(counts[b] * 3) / 4];
        free(d[b]);
    }
    free(d);
    free(fill);
    free(counts);
}

static void gen_lod(const SkinLod *src, float ratio, SkinLod *dst) {
    size_t target = (size_t)((float)src->icount * ratio) / 3 * 3;
    if (target < 3) target = 3;
    u32 *idx = (u32 *)malloc(sizeof(u32) * src->icount);
    float err = 0;
    size_t n = meshopt_simplify(idx, src->indices, src->icount, &src->verts[0].pos.x, src->vcount, sizeof(SkinVertex), target, 0.05f, 0, &err);
    if (n < target / 2 || n == 0) {
        // Topology-limited: fall back to the sloppy simplifier.
        n = meshopt_simplifySloppy(idx, src->indices, src->icount, &src->verts[0].pos.x, src->vcount, sizeof(SkinVertex), target, 0.2f, &err);
    }
    // Compact the vertex set.
    u32 *remap = (u32 *)malloc(sizeof(u32) * src->vcount);
    for (u32 i = 0; i < src->vcount; i++) remap[i] = 0xFFFFFFFFu;
    dst->verts = (SkinVertex *)malloc(sizeof(SkinVertex) * src->vcount);
    dst->vmat = (u8 *)malloc(src->vcount);
    dst->vcount = 0;
    for (size_t i = 0; i < n; i++) {
        u32 v = idx[i];
        if (remap[v] == 0xFFFFFFFFu) {
            remap[v] = dst->vcount;
            dst->verts[dst->vcount] = src->verts[v];
            dst->vmat[dst->vcount] = src->vmat[v];
            dst->vcount++;
        }
        idx[i] = remap[v];
    }
    dst->indices = idx;
    dst->icount = (u32)n;
    dst->generated = true;
    free(remap);
}

void skin_generate_lods(SkinMeshData *m) {
    if (m->lod_count <= 0) return;
    static const float ratios[SKIN_MAX_LODS] = { 1.0f, 0.5f, 0.25f, 0.1f };
    u32 base_tris = m->lods[0].icount / 3;
    while (m->lod_count < SKIN_MAX_LODS) {
        int l = m->lod_count;
        // Simplify from the most detailed authored LOD; ratio relative to LOD0.
        const SkinLod *src = &m->lods[0];
        float r = ratios[l];
        u32 prev_tris = m->lods[l - 1].icount / 3;
        float target_tris = fminf((float)base_tris * r, (float)prev_tris * 0.6f);
        float rel = target_tris / (float)DG_MAX(1u, src->icount / 3);
        gen_lod(src, fmaxf(rel, 0.02f), &m->lods[l]);
        m->lod_count++;
    }
}
