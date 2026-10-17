#include "terrain_maps.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void terrain_map_params_default(TerrainMapParams *p) {
    memset(p, 0, sizeof(*p));
    p->spacing = 1.0f;
    p->blend_radius = 3;
    p->curvature_scale = 2.5f;
    p->flow_ref = 400.0f;
    p->low_radius_m = 8.0f;
    p->low_depth_m = 0.6f;
    p->wear_m = 2.5f;
    p->wet_mask = 1u << 13;          // MGM_MUD
    p->path_mask = 1u << 8;          // MGM_DIRT_ROAD
}

static float smooth01(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3 - 2 * t);
}
static u8 to_u8(float v) {
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    return (u8)lrintf(v * 255.0f);
}

// Box mean of h over radius r (clamped), separable.
static void box_blur(const float *src, float *dst, int n, int r) {
    float *tmp = (float *)malloc(sizeof(float) * (size_t)n * n);
    for (int y = 0; y < n; y++) {
        double acc = 0;
        int cnt = 0;
        for (int x = -r; x <= r; x++) if (x >= 0 && x < n) acc += src[(size_t)y * n + x], cnt++;
        for (int x = 0; x < n; x++) {
            tmp[(size_t)y * n + x] = (float)(acc / cnt);
            int out = x - r, in = x + r + 1;
            if (out >= 0) acc -= src[(size_t)y * n + out], cnt--;
            if (in < n) acc += src[(size_t)y * n + in], cnt++;
        }
    }
    for (int x = 0; x < n; x++) {
        double acc = 0;
        int cnt = 0;
        for (int y = -r; y <= r; y++) if (y >= 0 && y < n) acc += tmp[(size_t)y * n + x], cnt++;
        for (int y = 0; y < n; y++) {
            dst[(size_t)y * n + x] = (float)(acc / cnt);
            int out = y - r, in = y + r + 1;
            if (out >= 0) acc -= tmp[(size_t)out * n + x], cnt--;
            if (in < n) acc += tmp[(size_t)in * n + x], cnt++;
        }
    }
    free(tmp);
}

// Chamfer (3-4) distance (in samples) from the seeds (dist[i] = seed value, others FLT_MAX), two passes.
static void chamfer(float *d, int n) {
    const float a = 1.0f, b = 1.41421356f;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float *c = &d[(size_t)y * n + x];
            if (x > 0) *c = fminf(*c, c[-1] + a);
            if (y > 0) {
                *c = fminf(*c, c[-n] + a);
                if (x > 0) *c = fminf(*c, c[-n - 1] + b);
                if (x < n - 1) *c = fminf(*c, c[-n + 1] + b);
            }
        }
    for (int y = n - 1; y >= 0; y--)
        for (int x = n - 1; x >= 0; x--) {
            float *c = &d[(size_t)y * n + x];
            if (x < n - 1) *c = fminf(*c, c[1] + a);
            if (y < n - 1) {
                *c = fminf(*c, c[n] + a);
                if (x < n - 1) *c = fminf(*c, c[n + 1] + b);
                if (x > 0) *c = fminf(*c, c[n - 1] + b);
            }
        }
}

static const float *g_sort_h;
static int cmp_desc(const void *x, const void *y) {
    float a = g_sort_h[*(const int *)x], b = g_sort_h[*(const int *)y];
    return a < b ? 1 : (a > b ? -1 : 0);
}

void terrain_ref_flow(const float *h, int n, float *acc) {
    size_t N = (size_t)n * n;
    int *order = (int *)malloc(sizeof(int) * N);
    for (size_t i = 0; i < N; i++) order[i] = (int)i, acc[i] = 1.0f;
    g_sort_h = h;
    qsort(order, N, sizeof(int), cmp_desc);
    for (size_t k = 0; k < N; k++) {
        int i = order[k], x = i % n, y = i / n, best = -1;
        float drop = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int xx = x + dx, yy = y + dy;
                if ((!dx && !dy) || xx < 0 || yy < 0 || xx >= n || yy >= n) continue;
                float s = (h[i] - h[(size_t)yy * n + xx]) / ((dx && dy) ? 1.41421356f : 1.0f);
                if (s > drop) drop = s, best = yy * n + xx;
            }
        if (best >= 0) acc[best] += acc[i];
    }
    free(order);
}

float terrain_ref_curvature(const float *h, int n, int x, int y, float spacing) {
    int r = (int)lrintf(2.0f / spacing);
    if (r < 1) r = 1;
    // box means (clamped) at the point and its 4 neighbours
    float m[5];
    const int ox[5] = { 0, 1, -1, 0, 0 }, oy[5] = { 0, 0, 0, 1, -1 };
    for (int k = 0; k < 5; k++) {
        int cx = x + ox[k], cy = y + oy[k];
        cx = cx < 0 ? 0 : (cx >= n ? n - 1 : cx);
        cy = cy < 0 ? 0 : (cy >= n ? n - 1 : cy);
        double acc = 0;
        int cnt = 0;
        // separable box as the bake: rows clamped first, then columns clamped
        for (int yy = cy - r; yy <= cy + r; yy++) {
            if (yy < 0 || yy >= n) continue;
            double row = 0;
            int rc = 0;
            for (int xx = cx - r; xx <= cx + r; xx++) if (xx >= 0 && xx < n) row += h[(size_t)yy * n + xx], rc++;
            acc += row / rc;
            cnt++;
        }
        m[k] = (float)(acc / cnt);
    }
    return (m[1] + m[2] + m[3] + m[4] - 4.0f * m[0]) / (spacing * spacing);
}

void terrain_ref_fractions(const u8 *mat, int n, int x, int y, int r, int *a, int *b, float *fa, float *fb) {
    int cnt[256] = { 0 }, tot = 0;
    for (int yy = y - r; yy <= y + r; yy++)
        for (int xx = x - r; xx <= x + r; xx++) {
            if (xx < 0 || yy < 0 || xx >= n || yy >= n) continue;
            cnt[mat[(size_t)yy * n + xx]]++;
            tot++;
        }
    // dominant: most counted (ties: the sample's own material, then the lowest id)
    int own = mat[(size_t)y * n + x], ia = own, ib;
    for (int i = 0; i < 256; i++) if (cnt[i] > cnt[ia]) ia = i;
    ib = -1;
    for (int i = 0; i < 256; i++) if (i != ia && cnt[i] > 0 && (ib < 0 || cnt[i] > cnt[ib])) ib = i;
    *a = ia;
    *b = ib < 0 ? ia : ib;
    *fa = (float)cnt[ia] / (float)tot;
    *fb = ib < 0 ? 0.0f : (float)cnt[ib] / (float)tot;
}

float terrain_ref_border_distance(const u8 *mat, int n, int x, int y, float spacing) {
    u8 own = mat[(size_t)y * n + x];
    int r = (int)ceilf(8.0f / spacing) + 1;
    float best = FLT_MAX;
    for (int yy = y - r; yy <= y + r; yy++)
        for (int xx = x - r; xx <= x + r; xx++) {
            if (xx < 0 || yy < 0 || xx >= n || yy >= n || mat[(size_t)yy * n + xx] == own) continue;
            float d = sqrtf((float)((xx - x) * (xx - x) + (yy - y) * (yy - y)));
            if (d < best) best = d;
        }
    if (best == FLT_MAX) return 8.0f;
    return fminf((best - 0.5f) * spacing, 8.0f);
}

void terrain_maps_build(const float *h, const u8 *mat, int n, const TerrainMapParams *p, u8 *blend, u8 *attr) {
    size_t N = (size_t)n * n;
    int r = p->blend_radius < 1 ? 1 : p->blend_radius;
    // --- blend: per material a summed-area table of its indicator, keep the top two counts per sample
    u32 present = 0;
    for (size_t i = 0; i < N; i++) present |= 1u << (mat[i] & 31u);
    u16 *c1 = (u16 *)calloc(N, sizeof(u16)), *c2 = (u16 *)calloc(N, sizeof(u16));
    u8 *i1 = (u8 *)malloc(N), *i2 = (u8 *)malloc(N);
    u16 *area = (u16 *)malloc(sizeof(u16) * N);
    for (size_t i = 0; i < N; i++) i1[i] = i2[i] = mat[i];
    int *sat = (int *)malloc(sizeof(int) * (size_t)(n + 1) * (n + 1));
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int x0 = x - r < 0 ? 0 : x - r, x1 = x + r >= n ? n - 1 : x + r, y0 = y - r < 0 ? 0 : y - r, y1 = y + r >= n ? n - 1 : y + r;
            area[(size_t)y * n + x] = (u16)((x1 - x0 + 1) * (y1 - y0 + 1));
        }
    for (u32 m = 0; m < 32; m++) {
        if (!(present >> m & 1u)) continue;
        memset(sat, 0, sizeof(int) * (size_t)(n + 1));
        for (int y = 0; y < n; y++) {
            int row = 0;
            sat[(size_t)(y + 1) * (n + 1)] = 0;
            for (int x = 0; x < n; x++) {
                row += (mat[(size_t)y * n + x] & 31u) == m;
                sat[(size_t)(y + 1) * (n + 1) + x + 1] = sat[(size_t)y * (n + 1) + x + 1] + row;
            }
        }
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int x0 = x - r < 0 ? 0 : x - r, x1 = x + r >= n ? n - 1 : x + r, y0 = y - r < 0 ? 0 : y - r, y1 = y + r >= n ? n - 1 : y + r;
                int c = sat[(size_t)(y1 + 1) * (n + 1) + x1 + 1] - sat[(size_t)y0 * (n + 1) + x1 + 1] - sat[(size_t)(y1 + 1) * (n + 1) + x0] + sat[(size_t)y0 * (n + 1) + x0];
                if (!c) continue;
                size_t i = (size_t)y * n + x;
                // ties keep the sample's own material first (the reference's rule)
                bool own = (mat[i] & 31u) == m;
                if (c > c1[i] || (c == c1[i] && own)) {
                    if (i1[i] != m) { c2[i] = c1[i]; i2[i] = i1[i]; }
                    c1[i] = (u16)c;
                    i1[i] = (u8)m;
                } else if (c > c2[i] || (c2[i] == 0)) {
                    c2[i] = (u16)c;
                    i2[i] = (u8)m;
                }
            }
    }
    for (size_t i = 0; i < N; i++) {
        if (!c2[i]) i2[i] = i1[i];
        blend[i * 4 + 0] = i1[i];
        blend[i * 4 + 1] = i2[i];
        blend[i * 4 + 2] = to_u8((float)c1[i] / (float)area[i]);
        blend[i * 4 + 3] = to_u8((float)c2[i] / (float)area[i]);
    }
    free(sat);
    free(c1);
    free(c2);
    free(i1);
    free(i2);
    free(area);
    // --- curvature: Laplacian of the height blurred over 2 m
    float *hb = (float *)malloc(sizeof(float) * N);
    int cr = (int)lrintf(2.0f / p->spacing);
    box_blur(h, hb, n, cr < 1 ? 1 : cr);
    // --- moisture: flow accumulation + low ground
    float *acc = (float *)malloc(sizeof(float) * N), *mean = (float *)malloc(sizeof(float) * N);
    terrain_ref_flow(h, n, acc);
    int lr = (int)lrintf(p->low_radius_m / p->spacing);
    box_blur(h, mean, n, lr < 1 ? 1 : lr);
    // --- distances: to another material (border) and to a path
    float *db = (float *)malloc(sizeof(float) * N), *dp = (float *)malloc(sizeof(float) * N);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            size_t i = (size_t)y * n + x;
            u8 m = mat[i];
            bool edge = (x > 0 && mat[i - 1] != m) || (x < n - 1 && mat[i + 1] != m) || (y > 0 && mat[i - n] != m) || (y < n - 1 && mat[i + n] != m) ||
                        (x > 0 && y > 0 && mat[i - n - 1] != m) || (x < n - 1 && y > 0 && mat[i - n + 1] != m) || (x > 0 && y < n - 1 && mat[i + n - 1] != m) ||
                        (x < n - 1 && y < n - 1 && mat[i + n + 1] != m);
            db[i] = edge ? 0.5f : FLT_MAX;
            dp[i] = (p->path_mask >> (m & 31u) & 1u) ? 0.0f : FLT_MAX;
        }
    chamfer(db, n);
    chamfer(dp, n);
    const float lref = logf(1.0f + p->flow_ref);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            size_t i = (size_t)y * n + x;
            float c = 0.0f;
            if (x > 0 && y > 0 && x < n - 1 && y < n - 1) c = (hb[i - 1] + hb[i + 1] + hb[i - n] + hb[i + n] - 4.0f * hb[i]) / (p->spacing * p->spacing);
            else {
                int xx = x < 1 ? 1 : (x > n - 2 ? n - 2 : x), yy = y < 1 ? 1 : (y > n - 2 ? n - 2 : y);
                size_t j = (size_t)yy * n + xx;
                c = (hb[j - 1] + hb[j + 1] + hb[j - n] + hb[j + n] - 4.0f * hb[j]) / (p->spacing * p->spacing);
            }
            float flow = logf(1.0f + acc[i]) / lref;
            float low = smooth01(0.0f, p->low_depth_m, mean[i] - h[i]);
            float moist = fminf(flow, 1.0f) * 0.75f + low * 0.5f;
            if (p->wet_mask >> (mat[i] & 31u) & 1u) moist = 1.0f;
            float wear = 1.0f - smooth01(0.0f, p->wear_m, dp[i] * p->spacing);
            attr[i * 4 + 0] = to_u8(0.5f + c * p->curvature_scale);
            attr[i * 4 + 1] = to_u8(moist);
            attr[i * 4 + 2] = to_u8(wear);
            attr[i * 4 + 3] = to_u8(fminf(db[i] * p->spacing, 8.0f) / 8.0f);
        }
    free(hb);
    free(acc);
    free(mean);
    free(db);
    free(dp);
}
