#include "vgeo_impostor.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void vgeo_hemi_oct_decode(float u, float v, float out[3]) {
    float px = u * 2.0f - 1.0f, py = v * 2.0f - 1.0f;
    float d[3] = { (px + py) * 0.5f, 0.0f, (px - py) * 0.5f };
    d[1] = 1.0f - fabsf(d[0]) - fabsf(d[2]);
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; k++) out[k] = d[k] / l;
}

void vgeo_hemi_oct_encode(const float in[3], float *u, float *v) {
    float d[3] = { in[0], fmaxf(in[1], 0.0f), in[2] };
    float s = fmaxf(fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]), 1e-8f);
    *u = (d[0] / s + d[2] / s) * 0.5f + 0.5f;
    *v = (d[0] / s - d[2] / s) * 0.5f + 0.5f;
}

static void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

void vgeo_impostor_basis(const float f[3], float r[3], float u[3]) {
    float up[3] = { 0.0f, 1.0f, 0.0f };
    if (fabsf(f[1]) > 0.999f) { up[1] = 0.0f; up[2] = -1.0f; }
    cross3(up, f, r);
    float l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int k = 0; k < 3; k++) r[k] /= l;
    cross3(f, r, u);
}

static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float lin_to_srgb(float c) {
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

// Bilinear, wrapping: rgb linear, a as stored.
static void sample(const VgImpostorMaterial *m, float u, float v, float out[4]) {
    float x = u * (float)m->w - 0.5f, y = v * (float)m->h - 0.5f;
    float fx = floorf(x), fy = floorf(y), tx = x - fx, ty = y - fy;
    int x0 = (int)fx, y0 = (int)fy;
    for (int k = 0; k < 4; k++) out[k] = 0;
    for (int j = 0; j < 4; j++) {
        int sx = ((x0 + (j & 1)) % (int)m->w + (int)m->w) % (int)m->w, sy = ((y0 + (j >> 1)) % (int)m->h + (int)m->h) % (int)m->h;
        float w = ((j & 1) ? tx : 1 - tx) * ((j >> 1) ? ty : 1 - ty);
        const u8 *p = &m->rgba[((size_t)sy * m->w + (size_t)sx) * 4];
        for (int k = 0; k < 3; k++) out[k] += srgb_to_lin((float)p[k] / 255.0f) * w;
        out[3] += (float)p[3] / 255.0f * w;
    }
}

typedef struct BakeTri { float p[3][3], n[3][3], uv[3][2]; u32 mat; } BakeTri;

const VgImpostorHeader *vgeo_impostor(const VgBuilt *b, const u8 **albedo, const u8 **normal) {
    if (!b->impostor || b->impostor_size < sizeof(VgImpostorHeader)) return NULL;
    const VgImpostorHeader *h = (const VgImpostorHeader *)b->impostor;
    size_t n = (size_t)h->size * h->size * 4;
    if (b->impostor_size < sizeof(VgImpostorHeader) + 2 * n) return NULL;
    if (albedo) *albedo = b->impostor + sizeof(VgImpostorHeader);
    if (normal) *normal = b->impostor + sizeof(VgImpostorHeader) + n;
    return h;
}

bool vgeo_impostor_bake(VgBuilt *b, const VgImpostorOptions *o) {
    u32 size = o && o->size ? o->size : 1024, grid = o && o->grid ? o->grid : 8;
    if (grid < 2 || size % grid) return false;
    u32 tile = size / grid, ss = tile * 2;   // 2x supersampled frame
    // Leaf clusters (full detail) as triangles.
    u32 cap = 1024, nt = 0;
    BakeTri *tris = (BakeTri *)malloc(sizeof(BakeTri) * cap);
    VgDecoded *dec = (VgDecoded *)malloc(sizeof(VgDecoded));
    for (u32 p = 0; p < b->hdr.page_count; p++) {
        const VgPageHeader *ph = vgeo_page(b, p);
        for (u32 c = 0; c < ph->cluster_count; c++) {
            const VgCluster *cl = vgeo_page_cluster(b, p, c);
            if (cl->child_group != ~0u) continue;
            vgeo_decode_cluster(b, p, c, dec);
            for (u32 t = 0; t < dec->tri_count; t++) {
                if (nt == cap) { cap *= 2; tris = (BakeTri *)realloc(tris, sizeof(BakeTri) * cap); }
                BakeTri *bt = &tris[nt++];
                for (int k = 0; k < 3; k++) {
                    u32 v = dec->tri[t][k];
                    memcpy(bt->p[k], dec->pos[v], 12);
                    memcpy(bt->n[k], dec->normal[v], 12);
                    memcpy(bt->uv[k], dec->uv[v], 8);
                }
                bt->mat = dec->material;
            }
        }
    }
    free(dec);
    const float *c = b->hdr.sphere, rad = b->hdr.sphere[3] > 0 ? b->hdr.sphere[3] : 1.0f;
    size_t ntex = (size_t)size * size * 4;
    u8 *out = (u8 *)calloc(1, sizeof(VgImpostorHeader) + 2 * ntex);
    VgImpostorHeader *hd = (VgImpostorHeader *)out;
    hd->size = size;
    hd->grid = grid;
    memcpy(hd->center, c, 12);
    hd->radius = rad;
    u8 *alb = out + sizeof(VgImpostorHeader), *nrm = alb + ntex;
    float *zb = (float *)malloc(sizeof(float) * ss * ss), *cb = (float *)malloc(sizeof(float) * 8 * ss * ss);   // rgb, n xyz, coverage, -
    u8 *filled = (u8 *)malloc((size_t)tile * tile);   // 1 covered, k + 1 dilated in pass k
    for (u32 fj = 0; fj < grid; fj++)
        for (u32 fi = 0; fi < grid; fi++) {
            float f[3], r[3], u[3];
            vgeo_hemi_oct_decode((float)fi / (float)(grid - 1), (float)fj / (float)(grid - 1), f);
            vgeo_impostor_basis(f, r, u);
            for (u32 i = 0; i < ss * ss; i++) zb[i] = -FLT_MAX;
            memset(cb, 0, sizeof(float) * 8 * ss * ss);
            for (u32 t = 0; t < nt; t++) {
                const BakeTri *bt = &tris[t];
                float sx[3], sy[3], sz[3];
                for (int k = 0; k < 3; k++) {
                    float d[3] = { bt->p[k][0] - c[0], bt->p[k][1] - c[1], bt->p[k][2] - c[2] };
                    sx[k] = (0.5f + 0.5f * (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / rad) * (float)ss;
                    sy[k] = (0.5f - 0.5f * (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / rad) * (float)ss;
                    sz[k] = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
                }
                float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
                if (fabsf(area) < 1e-12f) continue;
                int x0 = (int)fmaxf(floorf(fminf(sx[0], fminf(sx[1], sx[2]))), 0), x1 = (int)fminf(ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2]))), (float)ss - 1);
                int y0 = (int)fmaxf(floorf(fminf(sy[0], fminf(sy[1], sy[2]))), 0), y1 = (int)fminf(ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2]))), (float)ss - 1);
                const VgImpostorMaterial *mt = o && bt->mat < o->material_count && o->materials ? &o->materials[bt->mat] : NULL;
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) {
                        float px = (float)x + 0.5f, py = (float)y + 0.5f;
                        float w1 = ((px - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (py - sy[0])) / area;
                        float w2 = ((sx[1] - sx[0]) * (py - sy[0]) - (px - sx[0]) * (sy[1] - sy[0])) / area;
                        float w0 = 1.0f - w1 - w2;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                        size_t i = (size_t)y * ss + (size_t)x;
                        if (z <= zb[i]) continue;
                        float tu = w0 * bt->uv[0][0] + w1 * bt->uv[1][0] + w2 * bt->uv[2][0], tv = w0 * bt->uv[0][1] + w1 * bt->uv[1][1] + w2 * bt->uv[2][1];
                        float col[4] = { 1, 1, 1, 1 };
                        if (mt) {
                            memcpy(col, mt->color, 16);
                            if (mt->rgba && mt->w && mt->h) {
                                float ts = mt->tex_scale > 0 ? mt->tex_scale : 1.0f, s4[4];
                                sample(mt, tu * ts, tv * ts, s4);
                                for (int k = 0; k < 3; k++) col[k] *= s4[k];
                                if (mt->masked && s4[3] < mt->cutoff) continue;
                            }
                        }
                        float n[3];
                        for (int k = 0; k < 3; k++) n[k] = w0 * bt->n[0][k] + w1 * bt->n[1][k] + w2 * bt->n[2][k];
                        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        if (nl < 1e-8f) { n[0] = f[0]; n[1] = f[1]; n[2] = f[2]; nl = 1; }
                        float s = (n[0] * f[0] + n[1] * f[1] + n[2] * f[2]) < 0 ? -1.0f / nl : 1.0f / nl;   // facing the viewer (cards)
                        zb[i] = z;
                        float *o8 = &cb[i * 8];
                        for (int k = 0; k < 3; k++) { o8[k] = col[k]; o8[3 + k] = n[k] * s; }
                        o8[6] = 1.0f;
                    }
            }
            // 2x2 box filter into the tile (colour / normal weighted by coverage)
            memset(filled, 0, (size_t)tile * tile);
            for (u32 y = 0; y < tile; y++)
                for (u32 x = 0; x < tile; x++) {
                    float acc[7] = { 0 };
                    for (u32 k = 0; k < 4; k++) {
                        const float *s8 = &cb[((size_t)(y * 2 + (k >> 1)) * ss + x * 2 + (k & 1)) * 8];
                        for (int q = 0; q < 6; q++) acc[q] += s8[q] * s8[6];
                        acc[6] += s8[6];
                    }
                    size_t o = (((size_t)fj * tile + y) * size + (size_t)fi * tile + x) * 4;
                    float a = acc[6] * 0.25f;
                    if (acc[6] > 0) {
                        filled[y * tile + x] = 1;
                        for (int k = 0; k < 3; k++) alb[o + k] = (u8)lrintf(lin_to_srgb(acc[k] / acc[6]) * 255.0f);
                        float nn[3] = { acc[3], acc[4], acc[5] }, l = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                        for (int k = 0; k < 3; k++) nrm[o + k] = (u8)lrintf((l > 1e-8f ? nn[k] / l : f[k]) * 127.5f + 127.5f);
                    }
                    alb[o + 3] = nrm[o + 3] = (u8)lrintf(a * 255.0f);
                }
            // dilate colour / normal into empty texels of the tile (bilinear and mips do not bleed dark)
            for (u8 it = 1; it <= 4; it++) {
                for (u32 y = 0; y < tile; y++)
                    for (u32 x = 0; x < tile; x++) {
                        if (filled[y * tile + x]) continue;
                        size_t o = (((size_t)fj * tile + y) * size + (size_t)fi * tile + x) * 4;
                        u32 sum[6] = { 0 }, n = 0;
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                int nx = (int)x + dx, ny = (int)y + dy;
                                if (nx < 0 || ny < 0 || nx >= (int)tile || ny >= (int)tile) continue;
                                u8 fl = filled[(u32)ny * tile + (u32)nx];
                                if (!fl || fl > it) continue;   // texels filled before this pass
                                size_t q = (((size_t)fj * tile + (u32)ny) * size + (size_t)fi * tile + (u32)nx) * 4;
                                for (int k = 0; k < 3; k++) { sum[k] += alb[q + k]; sum[3 + k] += nrm[q + k]; }
                                n++;
                            }
                        if (!n) continue;
                        for (int k = 0; k < 3; k++) { alb[o + k] = (u8)(sum[k] / n); nrm[o + k] = (u8)(sum[3 + k] / n); }
                        filled[y * tile + x] = (u8)(it + 1);
                    }
            }
        }
    free(zb);
    free(cb);
    free(filled);
    free(tris);
    free(b->impostor);
    b->impostor = out;
    b->impostor_size = (u32)(sizeof(VgImpostorHeader) + 2 * ntex);
    b->hdr.flags |= VGF_HAS_IMPOSTOR;
    return true;
}
