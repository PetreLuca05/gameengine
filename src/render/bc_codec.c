// Block compression codecs (docs/TEXTURE_MIPS.md "Block compression"): see bc_codec.h.
#include "bc_codec.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

u32 bc_block_bytes(BcFormat f) { return f == BC_1 || f == BC_4 ? 8u : 16u; }

size_t bc_image_bytes(BcFormat f, u32 w, u32 h) { return (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) * bc_block_bytes(f); }

const char *bc_format_name(BcFormat f) {
    static const char *const names[BC_FORMAT_COUNT] = { "none", "BC1", "BC3", "BC4", "BC5", "BC7" };
    return (unsigned)f < BC_FORMAT_COUNT ? names[f] : "?";
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ------------------------------------------------------------------ shared: principal axis

// Mean and principal axis of n points (nch channels): power iteration on the scatter matrix, started
// from its largest column. False when the points are (nearly) one point.
static bool principal_axis(const float (*p)[4], int n, int nch, float mean[4], float axis[4]) {
    float m[4] = { 0, 0, 0, 0 }, s[4][4];
    memset(s, 0, sizeof(s));
    for (int i = 0; i < n; i++)
        for (int c = 0; c < nch; c++) m[c] += p[i][c];
    for (int c = 0; c < nch; c++) m[c] /= (float)n;
    for (int i = 0; i < n; i++) {
        float d[4];
        for (int c = 0; c < nch; c++) d[c] = p[i][c] - m[c];
        for (int a = 0; a < nch; a++)
            for (int b = a; b < nch; b++) s[a][b] += d[a] * d[b];
    }
    for (int a = 0; a < nch; a++)
        for (int b = 0; b < a; b++) s[a][b] = s[b][a];
    memcpy(mean, m, sizeof(m));
    int k = 0;
    for (int c = 1; c < nch; c++)
        if (s[c][c] > s[k][k]) k = c;
    axis[0] = axis[1] = axis[2] = axis[3] = 0;
    if (s[k][k] < 1e-3f) return false;
    float v[4] = { 0, 0, 0, 0 };
    for (int c = 0; c < nch; c++) v[c] = s[k][c];
    for (int it = 0; it < 5; it++) {
        float w[4] = { 0, 0, 0, 0 }, len = 0;
        for (int a = 0; a < nch; a++) {
            for (int b = 0; b < nch; b++) w[a] += s[a][b] * v[b];
            len += w[a] * w[a];
        }
        if (len < 1e-20f) break;
        len = 1.0f / sqrtf(len);
        for (int a = 0; a < nch; a++) v[a] = w[a] * len;
    }
    float len = 0;
    for (int c = 0; c < nch; c++) len += v[c] * v[c];
    if (len < 1e-20f) return false;
    len = 1.0f / sqrtf(len);
    for (int c = 0; c < nch; c++) axis[c] = v[c] * len;
    return true;
}

// ------------------------------------------------------------------ BC7

typedef struct Bc7Mode { u8 ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; } Bc7Mode;
static const Bc7Mode k_bc7[8] = {
    { 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 }, { 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 }, { 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 }, { 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
    { 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 }, { 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 }, { 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 }, { 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 },
};

static const u8 k_w2[4] = { 0, 21, 43, 64 };
static const u8 k_w3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const u8 k_w4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
static const u8 *bc7_weights(int ib) { return ib == 2 ? k_w2 : ib == 3 ? k_w3 : k_w4; }

// Two-subset partitions (subset of each texel) and the anchor texel of subset 1.
static const u8 k_part2[64][16] = {
    { 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1 }, { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1 }, { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1 }, { 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1 }, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1 }, { 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1 }, { 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0 }, { 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0 }, { 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0 }, { 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1 },
    { 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0 }, { 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0 }, { 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0 },
    { 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0 }, { 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0 }, { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0 }, { 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0 }, { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1 }, { 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0 }, { 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0 },
    { 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0 }, { 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0 }, { 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1 },
    { 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1 }, { 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0 }, { 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0 },
    { 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0 }, { 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0 }, { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 },
    { 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1 }, { 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1 }, { 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 }, { 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0 },
    { 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0 }, { 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1 }, { 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1 },
    { 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0 }, { 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0 }, { 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1 },
    { 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1 }, { 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1 }, { 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1 }, { 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 }, { 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0 },
    { 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1 },
};
static const u8 k_anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};

typedef struct BitW { u8 *out; u32 pos; } BitW;   // LSB first
static void bw_put(BitW *b, u32 v, u32 n) {
    for (u32 i = 0; i < n; i++, b->pos++)
        if (v >> i & 1u) b->out[b->pos >> 3] |= (u8)(1u << (b->pos & 7));
}
typedef struct BitR { const u8 *in; u32 pos; } BitR;
static u32 br_get(BitR *b, u32 n) {
    u32 v = 0;
    for (u32 i = 0; i < n; i++, b->pos++) v |= (u32)(b->in[b->pos >> 3] >> (b->pos & 7) & 1u) << i;
    return v;
}

// A stored endpoint channel (cb bits, + the p-bit when pmode) expanded to 8 bits.
static inline int bc7_unq(int q, int p, int cb, int pmode) {
    int v = pmode ? (q << 1 | p) : q, n = cb + (pmode ? 1 : 0);
    if (n >= 8) return v;
    v <<= 8 - n;
    return v | v >> n;
}

static inline int bc7_interp(int e0, int e1, int w) { return ((64 - w) * e0 + w * e1 + 32) >> 6; }

// The stored value whose expansion (with p-bit p) is nearest x (0 .. 255): the rounded guess or a
// neighbour (the expansion is not exactly linear).
static int bc7_quant(float x, int p, int cb, int pmode) {
    int qmax = (1 << cb) - 1, n = cb + (pmode ? 1 : 0);
    float s = (float)((1 << n) - 1) / 255.0f;
    float v = pmode ? (x * s - (float)p) * 0.5f + 0.5f : x * s + 0.5f;
    int q = clampi(v > 0 ? (int)v : 0, 0, qmax);
    int d0 = bc7_unq(q, p, cb, pmode) - (int)(x + 0.5f);
    int k = d0 > 0 ? q - 1 : d0 < 0 ? q + 1 : q;
    if (k != q && k >= 0 && k <= qmax) {
        float a = fabsf((float)bc7_unq(q, p, cb, pmode) - x), b = fabsf((float)bc7_unq(k, p, cb, pmode) - x);
        if (b < a) q = k;
    }
    return q;
}

typedef struct Bc7Fit {
    int q[2][4];   // stored endpoints
    int p[2];      // p-bits (pmode 1: equal)
    u8 idx[16];
    u32 err;
} Bc7Fit;

// Nearest palette entry of every texel; returns the summed squared error. Wide palettes (8 / 16) are
// searched around the texel's projection on the endpoint segment unless `full`.
static u32 bc7_assign(const int (*px)[4], int n, int nch, const int ev[2][4], int ib, u8 *idx, bool full) {
    const u8 *w = bc7_weights(ib);
    int levels = 1 << ib, pal[16][4];
    for (int k = 0; k < levels; k++)
        for (int c = 0; c < nch; c++) pal[k][c] = bc7_interp(ev[0][c], ev[1][c], w[k]);
    u32 err = 0;
    if (levels <= 4 || full) {
        for (int i = 0; i < n; i++) {
            u32 bd = 0xFFFFFFFFu;
            int bk = 0;
            for (int k = 0; k < levels; k++) {
                u32 d = 0;
                for (int c = 0; c < nch; c++) {
                    int e = pal[k][c] - px[i][c];
                    d += (u32)(e * e);
                }
                if (d < bd) bd = d, bk = k;
            }
            idx[i] = (u8)bk;
            err += bd;
        }
        return err;
    }
    float dir[4] = { 0, 0, 0, 0 }, dd = 0;
    for (int c = 0; c < nch; c++) {
        dir[c] = (float)(ev[1][c] - ev[0][c]);
        dd += dir[c] * dir[c];
    }
    float inv = dd > 0 ? (float)(levels - 1) / dd : 0.0f;
    for (int i = 0; i < n; i++) {
        float t = 0;
        for (int c = 0; c < nch; c++) t += ((float)px[i][c] - (float)ev[0][c]) * dir[c];
        t *= inv;   // in index units (the weights are within half a step of uniform)
        int k0 = t <= 0 ? 0 : t >= (float)(levels - 1) ? levels - 1 : (int)(t + 0.5f);
        u32 bd = 0xFFFFFFFFu;
        int bk = k0;
        for (int k = k0 - 1; k <= k0 + 1; k++) {
            if (k < 0 || k >= levels) continue;
            u32 d = 0;
            for (int c = 0; c < nch; c++) {
                int e = pal[k][c] - px[i][c];
                d += (u32)(e * e);
            }
            if (d < bd) bd = d, bk = k;
        }
        idx[i] = (u8)bk;
        err += bd;
    }
    return err;
}

// Quantizes the float endpoints f with each p-bit choice worth trying, keeps the best in `best`.
static void bc7_try(const int (*px)[4], int n, int nch, int cb, int pmode, int ib, const float f[2][4], BcQuality q, Bc7Fit *best) {
    int pc[4][2], np = 0;
    if (pmode == 0) {
        pc[np][0] = pc[np][1] = 0, np++;
    } else if (pmode == 1) {
        pc[0][0] = pc[0][1] = 0;
        pc[1][0] = pc[1][1] = 1;
        np = 2;
    } else if (q == BC_Q_HIGH) {
        for (int k = 0; k < 4; k++) pc[k][0] = k & 1, pc[k][1] = k >> 1;
        np = 4;
    } else {
        np = -1;   // per endpoint: the p-bit with the smaller quantization error
    }
    // quantized endpoints for both p-bits
    int qv[2][2][4], uv[2][2][4];
    float qe[2][2] = { { 0, 0 }, { 0, 0 } };
    for (int e = 0; e < 2; e++)
        for (int p = 0; p < (pmode ? 2 : 1); p++)
            for (int c = 0; c < 4; c++) {
                qv[e][p][c] = c < nch ? bc7_quant(f[e][c], p, cb, pmode) : 0;
                uv[e][p][c] = bc7_unq(qv[e][p][c], p, cb, pmode);
                if (c < nch) qe[e][p] += ((float)uv[e][p][c] - f[e][c]) * ((float)uv[e][p][c] - f[e][c]);
            }
    if (np < 0) {
        pc[0][0] = qe[0][1] < qe[0][0];
        pc[0][1] = qe[1][1] < qe[1][0];
        np = 1;
    }
    for (int k = 0; k < np; k++) {
        Bc7Fit t;
        int ev[2][4];
        t.p[0] = pc[k][0];
        t.p[1] = pc[k][1];
        for (int e = 0; e < 2; e++)
            for (int c = 0; c < 4; c++) {
                t.q[e][c] = qv[e][t.p[e]][c];
                ev[e][c] = uv[e][t.p[e]][c];
            }
        t.err = bc7_assign(px, n, nch, (const int (*)[4])ev, ib, t.idx, q == BC_Q_HIGH);
        if (t.err < best->err) *best = t;
    }
}

// Least-squares endpoints for fixed indices; false when every texel has the same weight.
static bool bc7_refit(const int (*px)[4], int n, int nch, int ib, const u8 *idx, float f[2][4]) {
    const u8 *w = bc7_weights(ib);
    float a = 0, b = 0, d = 0, r0[4] = { 0, 0, 0, 0 }, r1[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        float t = (float)w[idx[i]] / 64.0f, s = 1.0f - t;
        a += s * s;
        b += s * t;
        d += t * t;
        for (int c = 0; c < nch; c++) {
            r0[c] += s * (float)px[i][c];
            r1[c] += t * (float)px[i][c];
        }
    }
    float det = a * d - b * b;
    if (fabsf(det) < 1e-6f) return false;
    det = 1.0f / det;
    for (int c = 0; c < nch; c++) {
        f[0][c] = clampf((d * r0[c] - b * r1[c]) * det, 0.0f, 255.0f);
        f[1][c] = clampf((a * r1[c] - b * r0[c]) * det, 0.0f, 255.0f);
    }
    return true;
}

// One subset: cb-bit endpoints (p-bits per pmode), ib-bit indices.
static void bc7_fit(const int (*px)[4], int n, int nch, int cb, int pmode, int ib, BcQuality q, Bc7Fit *best) {
    float pts[16][4], mean[4], axis[4], f[2][4];
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) pts[i][c] = (float)px[i][c];
    if (principal_axis((const float (*)[4])pts, n, nch, mean, axis)) {
        float t0 = 1e30f, t1 = -1e30f;
        for (int i = 0; i < n; i++) {
            float t = 0;
            for (int c = 0; c < nch; c++) t += (pts[i][c] - mean[c]) * axis[c];
            if (t < t0) t0 = t;
            if (t > t1) t1 = t;
        }
        for (int c = 0; c < 4; c++) {
            f[0][c] = clampf(mean[c] + t0 * axis[c], 0.0f, 255.0f);
            f[1][c] = clampf(mean[c] + t1 * axis[c], 0.0f, 255.0f);
        }
    } else {
        for (int c = 0; c < 4; c++) f[0][c] = f[1][c] = mean[c];
    }
    memset(best, 0, sizeof(*best));
    best->err = 0xFFFFFFFFu;
    int iters = q == BC_Q_HIGH ? 4 : 2;
    for (int it = 0; it < iters; it++) {
        bc7_try(px, n, nch, cb, pmode, ib, (const float (*)[4])f, q, best);
        if (best->err == 0 || it + 1 == iters) break;
        if (!bc7_refit(px, n, nch, ib, best->idx, f)) break;
    }
}

// The anchor texel of a subset must have its index MSB clear: else the endpoints swap (and p-bits)
// and the indices mirror (the weight tables are symmetric: the decoded texels do not change).
static void bc7_anchor(Bc7Fit *f, int ib, int anchor_pos) {
    int levels = 1 << ib;
    if (f->idx[anchor_pos] < levels / 2) return;
    for (int c = 0; c < 4; c++) {
        int t = f->q[0][c];
        f->q[0][c] = f->q[1][c];
        f->q[1][c] = t;
    }
    int t = f->p[0];
    f->p[0] = f->p[1];
    f->p[1] = t;
    for (int i = 0; i < 16; i++) f->idx[i] = (u8)(levels - 1 - f->idx[i]);
}

static inline bool bc7_is_anchor(int i, int ns, int part) { return i == 0 || (ns == 2 && i == k_anchor2[part]); }

// eq[subset][endpoint][channel] stored values, ep p-bits; i1 the first index set, i2 the second.
static void bc7_write(u8 out[16], int mode, int part, int rot, int isb, const int eq[2][2][4], const int ep[2][2], const u8 *i1, const u8 *i2) {
    const Bc7Mode *m = &k_bc7[mode];
    memset(out, 0, 16);
    BitW b = { out, 0 };
    bw_put(&b, 1u << mode, (u32)mode + 1);
    bw_put(&b, (u32)part, m->pb);
    bw_put(&b, (u32)rot, m->rb);
    bw_put(&b, (u32)isb, m->isb);
    for (int c = 0; c < 3; c++)
        for (int s = 0; s < m->ns; s++)
            for (int e = 0; e < 2; e++) bw_put(&b, (u32)eq[s][e][c], m->cb);
    if (m->ab)
        for (int s = 0; s < m->ns; s++)
            for (int e = 0; e < 2; e++) bw_put(&b, (u32)eq[s][e][3], m->ab);
    if (m->epb)
        for (int s = 0; s < m->ns; s++)
            for (int e = 0; e < 2; e++) bw_put(&b, (u32)ep[s][e], 1);
    if (m->spb)
        for (int s = 0; s < m->ns; s++) bw_put(&b, (u32)ep[s][0], 1);
    for (int i = 0; i < 16; i++) bw_put(&b, i1[i], (u32)m->ib - (bc7_is_anchor(i, m->ns, part) ? 1u : 0u));
    if (m->ib2)
        for (int i = 0; i < 16; i++) bw_put(&b, i2[i], (u32)m->ib2 - (i == 0 ? 1u : 0u));
}

int bc7_partition_subset(int part, int texel) { return k_part2[part & 63][texel & 15]; }

int bc7_block_mode(const u8 in[16]) {
    int mode = 0;
    while (mode < 8 && !(in[0] >> mode & 1)) mode++;
    return mode;
}

static void bc7_decode(const u8 in[16], u8 rgba[64]) {
    int mode = bc7_block_mode(in);
    if (mode >= 8) { memset(rgba, 0, 64); return; }   // reserved: transparent black (the spec)
    if (mode == 0 || mode == 2) {                     // three subsets: never encoded here
        for (int i = 0; i < 16; i++) rgba[i * 4] = 255, rgba[i * 4 + 1] = 0, rgba[i * 4 + 2] = 255, rgba[i * 4 + 3] = 255;
        return;
    }
    const Bc7Mode *m = &k_bc7[mode];
    BitR r = { in, (u32)mode + 1 };
    int part = (int)br_get(&r, m->pb), rot = (int)br_get(&r, m->rb), isb = (int)br_get(&r, m->isb);
    int eq[2][2][4], ep[2][2] = { { 0, 0 }, { 0, 0 } }, ev[2][2][4];
    for (int c = 0; c < 3; c++)
        for (int s = 0; s < m->ns; s++)
            for (int e = 0; e < 2; e++) eq[s][e][c] = (int)br_get(&r, m->cb);
    for (int s = 0; s < m->ns; s++)
        for (int e = 0; e < 2; e++) eq[s][e][3] = m->ab ? (int)br_get(&r, m->ab) : 255;
    if (m->epb)
        for (int s = 0; s < m->ns; s++)
            for (int e = 0; e < 2; e++) ep[s][e] = (int)br_get(&r, 1);
    if (m->spb)
        for (int s = 0; s < m->ns; s++) ep[s][0] = ep[s][1] = (int)br_get(&r, 1);
    int pm = m->epb || m->spb;
    for (int s = 0; s < m->ns; s++)
        for (int e = 0; e < 2; e++) {
            for (int c = 0; c < 3; c++) ev[s][e][c] = bc7_unq(eq[s][e][c], ep[s][e], m->cb, pm);
            ev[s][e][3] = m->ab ? bc7_unq(eq[s][e][3], ep[s][e], m->ab, pm) : 255;
        }
    u8 i1[16], i2[16];
    for (int i = 0; i < 16; i++) i1[i] = (u8)br_get(&r, (u32)m->ib - (bc7_is_anchor(i, m->ns, part) ? 1u : 0u));
    for (int i = 0; i < 16; i++) i2[i] = m->ib2 ? (u8)br_get(&r, (u32)m->ib2 - (i == 0 ? 1u : 0u)) : 0;
    const u8 *w1 = bc7_weights(m->ib), *w2 = m->ib2 ? bc7_weights(m->ib2) : w1;
    for (int i = 0; i < 16; i++) {
        int s = m->ns == 2 ? k_part2[part][i] : 0, wc, wa;
        if (!m->ib2) wc = wa = w1[i1[i]];
        else if (isb) wc = w2[i2[i]], wa = w1[i1[i]];
        else wc = w1[i1[i]], wa = w2[i2[i]];
        u8 *o = &rgba[i * 4];
        for (int c = 0; c < 3; c++) o[c] = (u8)bc7_interp(ev[s][0][c], ev[s][1][c], wc);
        o[3] = (u8)(m->ab ? bc7_interp(ev[s][0][3], ev[s][1][3], wa) : 255);
        if (rot) {
            u8 t = o[3];
            o[3] = o[rot - 1];
            o[rot - 1] = t;
        }
    }
}

// Mode 6: one RGBA subset, 7-bit endpoints + a p-bit each, 4-bit indices.
static u32 bc7_mode6(const int (*px)[4], u8 out[16], BcQuality q) {
    Bc7Fit f;
    bc7_fit(px, 16, 4, 7, 2, 4, q, &f);
    bc7_anchor(&f, 4, 0);
    int eq[2][2][4], ep[2][2] = { { f.p[0], f.p[1] }, { 0, 0 } };
    memcpy(eq[0], f.q, sizeof(f.q));
    memset(eq[1], 0, sizeof(eq[1]));
    bc7_write(out, 6, 0, 0, 0, (const int (*)[2][4])eq, (const int (*)[2])ep, f.idx, NULL);
    return f.err;
}

// Modes 4 / 5: the rotated channel (rot 1..3 swaps r / g / b with alpha; 0 keeps alpha) gets its own
// endpoints and indices. Mode 4: isb picks which channel set gets the 3-bit indices.
static u32 bc7_mode45(const int (*px)[4], u8 out[16], int mode, int rot, int isb, BcQuality q) {
    int col[16][4], al[16][4];
    for (int i = 0; i < 16; i++) {
        int v[4] = { px[i][0], px[i][1], px[i][2], px[i][3] };
        if (rot) {
            int t = v[3];
            v[3] = v[rot - 1];
            v[rot - 1] = t;
        }
        col[i][0] = v[0], col[i][1] = v[1], col[i][2] = v[2], col[i][3] = 0;
        al[i][0] = v[3], al[i][1] = al[i][2] = al[i][3] = 0;
    }
    Bc7Fit fc, fa;
    int cib = mode == 5 ? 2 : (isb ? 3 : 2), aib = mode == 5 ? 2 : (isb ? 2 : 3);
    bc7_fit((const int (*)[4])col, 16, 3, mode == 5 ? 7 : 5, 0, cib, q, &fc);
    bc7_fit((const int (*)[4])al, 16, 1, mode == 5 ? 8 : 6, 0, aib, q, &fa);
    bc7_anchor(&fc, cib, 0);
    bc7_anchor(&fa, aib, 0);
    int eq[2][2][4], ep[2][2] = { { 0, 0 }, { 0, 0 } };
    memset(eq, 0, sizeof(eq));
    for (int e = 0; e < 2; e++) {
        for (int c = 0; c < 3; c++) eq[0][e][c] = fc.q[e][c];
        eq[0][e][3] = fa.q[e][0];
    }
    const u8 *i1 = fc.idx, *i2 = fa.idx;
    if (mode == 4 && isb) i1 = fa.idx, i2 = fc.idx;
    bc7_write(out, mode, 0, rot, mode == 4 ? isb : 0, (const int (*)[2][4])eq, (const int (*)[2])ep, i1, i2);
    return fc.err + fa.err;
}

// Modes 1 / 3 (RGB) and 7 (RGBA): two subsets of partition `part`.
static u32 bc7_mode137(const int (*px)[4], u8 out[16], int mode, int part, BcQuality q) {
    const Bc7Mode *m = &k_bc7[mode];
    int nch = mode == 7 ? 4 : 3, pmode = m->spb ? 1 : 2;
    int sub[2][16][4], cnt[2] = { 0, 0 }, pos[16];
    for (int i = 0; i < 16; i++) {
        int s = k_part2[part][i];
        pos[i] = cnt[s];
        memcpy(sub[s][cnt[s]++], px[i], sizeof(int) * 4);
    }
    Bc7Fit f[2];
    u32 err = 0;
    for (int s = 0; s < 2; s++) {
        bc7_fit((const int (*)[4])sub[s], cnt[s], nch, m->cb, pmode, m->ib, q, &f[s]);
        err += f[s].err;
    }
    if (nch == 3)   // these modes decode alpha 255
        for (int i = 0; i < 16; i++) err += (u32)((255 - px[i][3]) * (255 - px[i][3]));
    // indices back in texel order, then the anchors (subset 0 at texel 0, subset 1 at its anchor)
    Bc7Fit g[2];
    for (int s = 0; s < 2; s++) {
        g[s] = f[s];
        for (int i = 0; i < 16; i++) g[s].idx[i] = k_part2[part][i] == s ? f[s].idx[pos[i]] : 0;
    }
    int levels = 1 << m->ib;
    for (int s = 0; s < 2; s++) {
        int a = s ? k_anchor2[part] : 0;
        if (g[s].idx[a] < levels / 2) continue;
        bc7_anchor(&g[s], m->ib, a);   // mirrors every index; only this subset's are used below
    }
    u8 idx[16];
    for (int i = 0; i < 16; i++) idx[i] = g[k_part2[part][i]].idx[i];
    int eq[2][2][4], ep[2][2];
    for (int s = 0; s < 2; s++) {
        memcpy(eq[s], g[s].q, sizeof(g[s].q));
        ep[s][0] = g[s].p[0];
        ep[s][1] = g[s].p[1];
    }
    bc7_write(out, mode, part, 0, 0, (const int (*)[2][4])eq, (const int (*)[2])ep, idx, NULL);
    return err;
}

// Squared distance of a subset to its best-fit line from its moments (n texels, sums s, products ss
// in upper-triangle order): scatter trace - largest eigenvalue (3 power iterations).
static float moment_line_error(int n, const float *sm, const float *ss, int nch) {
    if (n <= 1) return 0.0f;
    float c[4][4], inv = 1.0f / (float)n, tr = 0;
    int k = 0;
    for (int a = 0; a < nch; a++)
        for (int b = a; b < nch; b++, k++) c[a][b] = c[b][a] = ss[k] - sm[a] * sm[b] * inv;
    int m = 0;
    for (int a = 0; a < nch; a++) {
        tr += c[a][a];
        if (c[a][a] > c[m][m]) m = a;
    }
    if (tr < 1e-3f) return 0.0f;
    float v[4] = { c[m][0], c[m][1], c[m][2], nch > 3 ? c[m][3] : 0.0f }, lambda = 0;
    for (int it = 0; it < 3; it++) {
        float w[4] = { 0, 0, 0, 0 }, ww = 0, vw = 0, vv = 0;
        for (int a = 0; a < nch; a++) {
            for (int b = 0; b < nch; b++) w[a] += c[a][b] * v[b];
            ww += w[a] * w[a];
            vw += v[a] * w[a];
            vv += v[a] * v[a];
        }
        if (ww < 1e-20f || vv < 1e-20f) break;
        lambda = vw / vv;   // Rayleigh quotient: a lower bound of the largest eigenvalue
        float l = 1.0f / sqrtf(ww);
        for (int a = 0; a < nch; a++) v[a] = w[a] * l;
    }
    float e = tr - lambda;
    return e > 0 ? e : 0.0f;
}

// The k partitions whose subsets lie closest to a line each (the two-subset candidates).
static int bc7_rank_partitions(const int (*px)[4], int nch, int *best, int k) {
    float mo[16][14], tot[14];
    int nm = nch + nch * (nch + 1) / 2;
    memset(tot, 0, sizeof(tot));
    for (int i = 0; i < 16; i++) {
        int j = 0;
        for (int c = 0; c < nch; c++) mo[i][j++] = (float)px[i][c];
        for (int a = 0; a < nch; a++)
            for (int b = a; b < nch; b++) mo[i][j++] = (float)(px[i][a] * px[i][b]);
        for (j = 0; j < nm; j++) tot[j] += mo[i][j];
    }
    float be[8];
    int n = 0;
    for (int p = 0; p < 64; p++) {
        float s1[14];
        int n1 = 0;
        memset(s1, 0, sizeof(s1));
        for (int i = 0; i < 16; i++) {
            if (!k_part2[p][i]) continue;
            n1++;
            for (int j = 0; j < nm; j++) s1[j] += mo[i][j];
        }
        float s0[14];
        for (int j = 0; j < nm; j++) s0[j] = tot[j] - s1[j];
        float e = moment_line_error(16 - n1, s0, s0 + nch, nch) + moment_line_error(n1, s1, s1 + nch, nch);
        int at = n < k ? n : k;
        while (at > 0 && be[at - 1] > e) at--;
        if (at >= k) continue;
        for (int j = (n < k ? n : k - 1); j > at; j--) be[j] = be[j - 1], best[j] = best[j - 1];
        be[at] = e;
        best[at] = p;
        if (n < k) n++;
    }
    return n;
}

// Mode search per effort: mode 6 always; the separate-channel modes 5 / 4 where alpha (or, for data
// maps, any channel) is uncorrelated; the two-subset modes on the best-ranked partitions. A block
// already within ~1 level per channel (2 at FAST) stops early.
static void bc7_encode(const u8 rgba[64], u8 out[16], int qf) {
    BcQuality q = (BcQuality)(qf & BC_Q_MASK);
    bool data = (qf & BC_DATA) != 0;
    int px[16][4];
    bool opaque = true;
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 4; c++) px[i][c] = rgba[i * 4 + c];
        opaque &= rgba[i * 4 + 3] == 255;
    }
    u8 cand[16];
    u32 best = bc7_mode6((const int (*)[4])px, out, q);
    u32 good = q == BC_Q_HIGH ? 0u : q == BC_Q_NORMAL ? 64u : 256u;
    if (best <= good) return;
    // mode 5: rotation 0 (alpha apart) for blocks with alpha, every rotation for data maps
    int r0 = opaque ? 1 : 0, r1 = q == BC_Q_HIGH || data ? 4 : opaque ? 0 : 1;
    for (int r = r0; r < r1; r++) {
        u32 e = bc7_mode45((const int (*)[4])px, cand, 5, r, 0, q);
        if (e < best) best = e, memcpy(out, cand, 16);
    }
    if (q == BC_Q_HIGH || (data && q == BC_Q_NORMAL))
        for (int r = r0; r < 4; r++)
            for (int isb = 0; isb < 2; isb++) {
                u32 e = bc7_mode45((const int (*)[4])px, cand, 4, r, isb, q);
                if (e < best) best = e, memcpy(out, cand, 16);
            }
    if (q == BC_Q_FAST || best <= good) return;
    int parts[8], np = bc7_rank_partitions((const int (*)[4])px, opaque ? 3 : 4, parts, q == BC_Q_HIGH ? 4 : 2);
    for (int k = 0; k < np; k++) {
        if (opaque) {
            u32 e = bc7_mode137((const int (*)[4])px, cand, 1, parts[k], q);
            if (e < best) best = e, memcpy(out, cand, 16);
            if (k && q != BC_Q_HIGH && !data) continue;   // mode 3 on the best partition only
            e = bc7_mode137((const int (*)[4])px, cand, 3, parts[k], q);
            if (e < best) best = e, memcpy(out, cand, 16);
        } else {
            u32 e = bc7_mode137((const int (*)[4])px, cand, 7, parts[k], q);
            if (e < best) best = e, memcpy(out, cand, 16);
        }
    }
}

u32 bc7_encode_block_mode(const u8 rgba[64], u8 out[16], int mode, int part, int qf) {
    BcQuality q = (BcQuality)(qf & BC_Q_MASK);
    int px[16][4];
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++) px[i][c] = rgba[i * 4 + c];
    switch (mode) {
    case 1: case 3: case 7: return bc7_mode137((const int (*)[4])px, out, mode, part & 63, q);
    case 4: return bc7_mode45((const int (*)[4])px, out, 4, part & 3, (part >> 2) & 1, q);
    case 5: return bc7_mode45((const int (*)[4])px, out, 5, part & 3, 0, q);
    default: return bc7_mode6((const int (*)[4])px, out, q);
    }
}

// ------------------------------------------------------------------ BC4 (and BC5 / the BC3 alpha)

static void bc4_palette(int r0, int r1, int pal[8]) {
    pal[0] = r0;
    pal[1] = r1;
    if (r0 > r1) {
        for (int k = 2; k < 8; k++) pal[k] = ((8 - k) * r0 + (k - 1) * r1 + 3) / 7;
    } else {
        for (int k = 2; k < 6; k++) pal[k] = ((6 - k) * r0 + (k - 1) * r1 + 2) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
}

static u32 bc4_assign(const int v[16], const int pal[8], u8 idx[16]) {
    u32 err = 0;
    for (int i = 0; i < 16; i++) {
        int bk = 0, bd = 1 << 30;
        for (int k = 0; k < 8; k++) {
            int d = (pal[k] - v[i]) * (pal[k] - v[i]);
            if (d < bd) bd = d, bk = k;
        }
        idx[i] = (u8)bk;
        err += (u32)bd;
    }
    return err;
}

// Weight of endpoint 1 per index in the 8-value (r0 > r1) and 6-value palettes; -1: the fixed 0 / 255.
static const float k_bc4_w8[8] = { 0.0f, 1.0f, 1.0f / 7, 2.0f / 7, 3.0f / 7, 4.0f / 7, 5.0f / 7, 6.0f / 7 };
static const float k_bc4_w6[8] = { 0.0f, 1.0f, 0.2f, 0.4f, 0.6f, 0.8f, -1.0f, -1.0f };

typedef struct Bc4Best { u32 err; int r0, r1; u8 idx[16]; } Bc4Best;

static void bc4_try(const int v[16], int r0, int r1, Bc4Best *b) {
    int pal[8];
    u8 idx[16];
    bc4_palette(r0, r1, pal);
    u32 e = bc4_assign(v, pal, idx);
    if (e < b->err) {
        b->err = e, b->r0 = r0, b->r1 = r1;
        memcpy(b->idx, idx, 16);
    }
}

// Least-squares endpoints for fixed indices (texels on the fixed 0 / 255 entries left out).
static bool bc4_refit(const int v[16], const u8 idx[16], const float *w, float *f0, float *f1) {
    float a = 0, b = 0, d = 0, x0 = 0, x1 = 0;
    for (int i = 0; i < 16; i++) {
        float t = w[idx[i]];
        if (t < 0) continue;
        float s = 1.0f - t;
        a += s * s, b += s * t, d += t * t;
        x0 += s * (float)v[i], x1 += t * (float)v[i];
    }
    float det = a * d - b * b;
    if (fabsf(det) < 1e-6f) return false;
    *f0 = clampf((d * x0 - b * x1) / det, 0.0f, 255.0f);
    *f1 = clampf((a * x1 - b * x0) / det, 0.0f, 255.0f);
    return true;
}

// From endpoints (f0, f1): rounded (in the mode's order), assigned, refitted `iters` times.
static void bc4_descend(const int v[16], bool eight, float f0, float f1, int iters, Bc4Best *b) {
    for (int it = 0; it < iters; it++) {
        int r0 = clampi((int)(f0 + 0.5f), 0, 255), r1 = clampi((int)(f1 + 0.5f), 0, 255);
        if (eight) {
            if (r0 < r1) { int t = r0; r0 = r1; r1 = t; }
            if (r0 == r1) { if (r0 < 255) r0++; else r1--; }
        } else if (r0 > r1) {
            int t = r0;
            r0 = r1, r1 = t;
        }
        Bc4Best c = *b;
        c.err = 0xFFFFFFFFu;
        bc4_try(v, r0, r1, &c);
        if (c.err < b->err) *b = c;
        if (it + 1 == iters || !bc4_refit(v, c.idx, eight ? k_bc4_w8 : k_bc4_w6, &f0, &f1)) break;
    }
}

// One palette mode. The palette spacing that lands its levels on the texels' clusters can reach past
// the texels' range: every pair of levels (i, j) the lowest / highest texel could sit on gives a
// start (just the outer pair at FAST), each refined by least squares; then a +-d search around the
// best pair. eight: r0 > r1 (8 values, levels run from r0 down), else r0 <= r1 (6 values + 0, 255)
// spanning [lo, hi] (the texels off the extremes).
static void bc4_mode(const int v[16], bool eight, int lo, int hi, int d, BcQuality q, Bc4Best *out) {
    Bc4Best b;
    b.err = 0xFFFFFFFFu;
    b.r0 = b.r1 = 0;
    int np = eight ? 8 : 6;   // level positions, uniform in the endpoint-1 weight
    for (int i = 0; i < np; i++)
        for (int j = i + 1; j < np; j++) {
            if (q == BC_Q_FAST && (i || j != np - 1)) continue;
            float ti = (float)i / (float)(np - 1), tj = (float)j / (float)(np - 1);
            float a = eight ? (float)hi : (float)lo, z = eight ? (float)lo : (float)hi;   // texel values at ti / tj
            float step = (z - a) / (tj - ti), e0 = a - ti * step;
            bc4_descend(v, eight, clampf(e0, 0.0f, 255.0f), clampf(e0 + step, 0.0f, 255.0f), 2, &b);
        }
    int c0 = b.r0, c1 = b.r1;
    for (int r0 = clampi(c0 - d, 0, 255); r0 <= clampi(c0 + d, 0, 255); r0++)
        for (int r1 = clampi(c1 - d, 0, 255); r1 <= clampi(c1 + d, 0, 255); r1++)
            if (eight ? r0 > r1 : r0 <= r1) bc4_try(v, r0, r1, &b);
    if (b.err < out->err) *out = b;
}

static void bc4_encode(const int v[16], u8 out[8], BcQuality q) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    Bc4Best b;
    b.err = 0xFFFFFFFFu;
    b.r0 = b.r1 = hi;   // solid: r0 == r1 (6-value mode), every index 0
    memset(b.idx, 0, sizeof(b.idx));
    if (lo != hi) {
        int d = q == BC_Q_FAST ? 1 : q == BC_Q_NORMAL ? 2 : 4;
        bc4_mode(v, true, lo, hi, d, q, &b);
        // 6 values + the fixed 0 / 255: the texels within `near` of an extreme go to it, the rest span
        // the interpolated levels
        static const int k_near[3] = { 1, 12, 32 };
        for (int k = 0; k < (q == BC_Q_FAST ? 1 : 3) && b.err; k++) {
            int lo6 = 255, hi6 = 0;
            for (int i = 0; i < 16; i++)
                if (v[i] >= k_near[k] && v[i] <= 255 - k_near[k]) {
                    if (v[i] < lo6) lo6 = v[i];
                    if (v[i] > hi6) hi6 = v[i];
                }
            if (lo6 <= hi6 && (k == 0 || lo6 != lo || hi6 != hi)) bc4_mode(v, false, lo6, hi6, d, q, &b);
        }
    }
    out[0] = (u8)b.r0;
    out[1] = (u8)b.r1;
    u64 bits = 0;
    for (int i = 0; i < 16; i++) bits |= (u64)(b.idx[i] & 7u) << (3 * i);
    for (int k = 0; k < 6; k++) out[2 + k] = (u8)(bits >> (8 * k));
}

static void bc4_decode(const u8 in[8], u8 *dst, int stride) {
    int pal[8];
    bc4_palette(in[0], in[1], pal);
    u64 bits = 0;
    for (int k = 0; k < 6; k++) bits |= (u64)in[2 + k] << (8 * k);
    for (int i = 0; i < 16; i++) dst[i * stride] = (u8)pal[(bits >> (3 * i)) & 7u];
}

// ------------------------------------------------------------------ BC1 (and the BC3 colour)

static inline int e5(int v) { return (v << 3) | (v >> 2); }
static inline int e6(int v) { return (v << 2) | (v >> 4); }
static inline u16 pack565(int r, int g, int b) { return (u16)(r << 11 | g << 5 | b); }

static void bc1_palette(u16 c0, u16 c1, int pal[4][3]) {
    int a[3] = { e5(c0 >> 11), e6(c0 >> 5 & 63), e5(c0 & 31) }, b[3] = { e5(c1 >> 11), e6(c1 >> 5 & 63), e5(c1 & 31) };
    for (int c = 0; c < 3; c++) {
        pal[0][c] = a[c];
        pal[1][c] = b[c];
        if (c0 > c1) {
            pal[2][c] = (2 * a[c] + b[c] + 1) / 3;
            pal[3][c] = (a[c] + 2 * b[c] + 1) / 3;
        } else {
            pal[2][c] = (a[c] + b[c] + 1) / 2;
            pal[3][c] = 0;
        }
    }
}

static u32 bc1_assign(const int (*px)[4], u16 c0, u16 c1, u8 idx[16]) {
    int pal[4][3];
    bc1_palette(c0, c1, pal);
    int levels = c0 > c1 ? 4 : 3;   // 3-colour mode: index 3 is black, never chosen
    u32 err = 0;
    for (int i = 0; i < 16; i++) {
        int bk = 0, bd = 1 << 30;
        for (int k = 0; k < levels; k++) {
            int d = 0;
            for (int c = 0; c < 3; c++) d += (pal[k][c] - px[i][c]) * (pal[k][c] - px[i][c]);
            if (d < bd) bd = d, bk = k;
        }
        idx[i] = (u8)bk;
        err += (u32)bd;
    }
    return err;
}

static u16 bc1_quant(const float f[3]) {
    return pack565(clampi((int)floorf(f[0] * 31.0f / 255.0f + 0.5f), 0, 31), clampi((int)floorf(f[1] * 63.0f / 255.0f + 0.5f), 0, 63),
                   clampi((int)floorf(f[2] * 31.0f / 255.0f + 0.5f), 0, 31));
}

// Index weight of endpoint 1 in 4-colour mode.
static const float k_bc1_t[4] = { 0.0f, 1.0f, 1.0f / 3.0f, 2.0f / 3.0f };

static void bc1_write(u8 out[8], u16 c0, u16 c1, const u8 idx[16]) {
    out[0] = (u8)c0, out[1] = (u8)(c0 >> 8), out[2] = (u8)c1, out[3] = (u8)(c1 >> 8);
    u32 bits = 0;
    for (int i = 0; i < 16; i++) bits |= (u32)(idx[i] & 3u) << (2 * i);
    for (int k = 0; k < 4; k++) out[4 + k] = (u8)(bits >> (8 * k));
}

// Best (lo, hi) of one channel with (2 lo' + hi') / 3 nearest v (lo', hi' the expansions).
static void bc1_solid_channel(int v, int bits, int *a, int *b) {
    int mx = (1 << bits) - 1, q = clampi((v * mx + 127) / 255, 0, mx), bd = 1 << 30;
    *a = *b = q;
    for (int x = clampi(q - 2, 0, mx); x <= clampi(q + 2, 0, mx); x++)
        for (int y = clampi(q - 2, 0, mx); y <= clampi(q + 2, 0, mx); y++) {
            int ex = bits == 5 ? e5(x) : e6(x), ey = bits == 5 ? e5(y) : e6(y);
            int d = abs((2 * ex + ey + 1) / 3 - v);
            if (d < bd) bd = d, *a = x, *b = y;
        }
}

// 4-colour mode only (BC1 RGB / the BC3 colour block): c0 > c1, or c0 == c1 with every index 0.
static void bc1_encode(const int (*px)[4], u8 out[8], BcQuality q) {
    bool solid = true;
    for (int i = 1; i < 16 && solid; i++) solid = px[i][0] == px[0][0] && px[i][1] == px[0][1] && px[i][2] == px[0][2];
    u8 idx[16];
    if (solid) {
        int a[3], b[3];
        bc1_solid_channel(px[0][0], 5, &a[0], &b[0]);
        bc1_solid_channel(px[0][1], 6, &a[1], &b[1]);
        bc1_solid_channel(px[0][2], 5, &a[2], &b[2]);
        u16 c0 = pack565(a[0], a[1], a[2]), c1 = pack565(b[0], b[1], b[2]);
        u8 k = 2;
        if (c0 < c1) {
            u16 t = c0;
            c0 = c1, c1 = t, k = 3;
        } else if (c0 == c1) k = 0;
        memset(idx, k, sizeof(idx));
        bc1_write(out, c0, c1, idx);
        return;
    }
    float pts[16][4], mean[4], axis[4], f[2][3];
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++) pts[i][c] = (float)px[i][c];
    principal_axis((const float (*)[4])pts, 16, 3, mean, axis);
    float t0 = 1e30f, t1 = -1e30f;
    for (int i = 0; i < 16; i++) {
        float t = 0;
        for (int c = 0; c < 3; c++) t += (pts[i][c] - mean[c]) * axis[c];
        if (t < t0) t0 = t;
        if (t > t1) t1 = t;
    }
    for (int c = 0; c < 3; c++) {   // endpoint 0 the bright end: c0 > c1 mostly holds as quantized
        f[0][c] = clampf(mean[c] + t1 * axis[c], 0, 255);
        f[1][c] = clampf(mean[c] + t0 * axis[c], 0, 255);
    }
    u16 bc0 = 0, bc1 = 0;
    u8 bidx[16];
    u32 best = 0xFFFFFFFFu;
    int iters = q == BC_Q_FAST ? 2 : q == BC_Q_NORMAL ? 3 : 5;
    for (int it = 0; it < iters; it++) {
        u16 c0 = bc1_quant(f[0]), c1 = bc1_quant(f[1]);
        if (c0 < c1) {
            u16 t = c0;
            c0 = c1, c1 = t;
        }
        u32 e = bc1_assign(px, c0, c1, idx);
        if (c0 == c1) memset(idx, 0, sizeof(idx));
        if (e < best) best = e, bc0 = c0, bc1 = c1, memcpy(bidx, idx, 16);
        if (it + 1 == iters || c0 == c1) break;
        // least squares on the 4-colour weights
        float a = 0, b = 0, d = 0, r0[3] = { 0, 0, 0 }, r1[3] = { 0, 0, 0 };
        for (int i = 0; i < 16; i++) {
            float t = k_bc1_t[idx[i]], s = 1.0f - t;
            a += s * s, b += s * t, d += t * t;
            for (int c = 0; c < 3; c++) r0[c] += s * pts[i][c], r1[c] += t * pts[i][c];
        }
        float det = a * d - b * b;
        if (fabsf(det) < 1e-6f) break;
        for (int c = 0; c < 3; c++) {
            f[0][c] = clampf((d * r0[c] - b * r1[c]) / det, 0, 255);
            f[1][c] = clampf((a * r1[c] - b * r0[c]) / det, 0, 255);
        }
    }
    // greedy +-1 steps of each endpoint channel
    if (q != BC_Q_FAST && bc0 != bc1) {
        for (int pass = 0; pass < (q == BC_Q_HIGH ? 3 : 1); pass++) {
            bool better = false;
            for (int e = 0; e < 2; e++)
                for (int c = 0; c < 3; c++)
                    for (int s = -1; s <= 1; s += 2) {
                        u16 cc[2] = { bc0, bc1 };
                        int sh = c == 0 ? 11 : c == 1 ? 5 : 0, mx = c == 1 ? 63 : 31;
                        int v = (cc[e] >> sh & mx) + s;
                        if (v < 0 || v > mx) continue;
                        cc[e] = (u16)((cc[e] & ~(mx << sh)) | v << sh);
                        if (cc[0] <= cc[1]) continue;
                        u32 err = bc1_assign(px, cc[0], cc[1], idx);
                        if (err < best) best = err, bc0 = cc[0], bc1 = cc[1], memcpy(bidx, idx, 16), better = true;
                    }
            if (!better) break;
        }
    }
    bc1_write(out, bc0, bc1, bidx);
}

static void bc1_decode(const u8 in[8], u8 rgba[64]) {
    u16 c0 = (u16)(in[0] | in[1] << 8), c1 = (u16)(in[2] | in[3] << 8);
    int pal[4][3];
    bc1_palette(c0, c1, pal);
    u32 bits = (u32)in[4] | (u32)in[5] << 8 | (u32)in[6] << 16 | (u32)in[7] << 24;
    for (int i = 0; i < 16; i++) {
        int k = (int)(bits >> (2 * i) & 3u);
        for (int c = 0; c < 3; c++) rgba[i * 4 + c] = (u8)pal[k][c];
        rgba[i * 4 + 3] = 255;   // BC1 RGB: opaque (index 3 of the 3-colour mode is black)
    }
}

// ------------------------------------------------------------------ blocks / images

void bc_encode_block(BcFormat f, const u8 rgba[64], u8 *out, int qf) {
    BcQuality q = (BcQuality)(qf & BC_Q_MASK);
    int px[16][4], v[16];
    if (f != BC_7)
        for (int i = 0; i < 16; i++)
            for (int c = 0; c < 4; c++) px[i][c] = rgba[i * 4 + c];
    switch (f) {
    case BC_1: bc1_encode((const int (*)[4])px, out, q); break;
    case BC_3:
        for (int i = 0; i < 16; i++) v[i] = px[i][3];
        bc4_encode(v, out, q);
        bc1_encode((const int (*)[4])px, out + 8, q);
        break;
    case BC_4:
        for (int i = 0; i < 16; i++) v[i] = px[i][0];
        bc4_encode(v, out, q);
        break;
    case BC_5:
        for (int i = 0; i < 16; i++) v[i] = px[i][0];
        bc4_encode(v, out, q);
        for (int i = 0; i < 16; i++) v[i] = px[i][1];
        bc4_encode(v, out + 8, q);
        break;
    case BC_7: bc7_encode(rgba, out, qf); break;
    default: break;
    }
}

void bc_decode_block(BcFormat f, const u8 *in, u8 rgba[64]) {
    switch (f) {
    case BC_1: bc1_decode(in, rgba); break;
    case BC_3:
        bc1_decode(in + 8, rgba);
        bc4_decode(in, rgba + 3, 4);
        break;
    case BC_4:
        bc4_decode(in, rgba, 4);
        for (int i = 0; i < 16; i++) rgba[i * 4 + 1] = rgba[i * 4 + 2] = 0, rgba[i * 4 + 3] = 255;
        break;
    case BC_5:
        bc4_decode(in, rgba, 4);
        bc4_decode(in + 8, rgba + 1, 4);
        for (int i = 0; i < 16; i++) rgba[i * 4 + 2] = 0, rgba[i * 4 + 3] = 255;
        break;
    case BC_7: bc7_decode(in, rgba); break;
    default: memset(rgba, 0, 64); break;
    }
}

void bc_encode_rows(BcFormat f, const u8 *rgba, u32 w, u32 h, u32 by0, u32 by1, u8 *blocks, int q) {
    u32 bw = (w + 3) / 4, bs = bc_block_bytes(f);
    u8 blk[64];
    for (u32 by = by0; by < by1; by++)
        for (u32 bx = 0; bx < bw; bx++) {
            for (u32 y = 0; y < 4; y++) {
                u32 sy = DG_MIN(by * 4 + y, h - 1);
                for (u32 x = 0; x < 4; x++) {
                    u32 sx = DG_MIN(bx * 4 + x, w - 1);
                    memcpy(&blk[(y * 4 + x) * 4], &rgba[((size_t)sy * w + sx) * 4], 4);
                }
            }
            bc_encode_block(f, blk, blocks + ((size_t)by * bw + bx) * bs, q);
        }
}

void bc_decode_image(BcFormat f, const u8 *blocks, u32 w, u32 h, u8 *rgba) {
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4, bs = bc_block_bytes(f);
    u8 blk[64];
    for (u32 by = 0; by < bh; by++)
        for (u32 bx = 0; bx < bw; bx++) {
            bc_decode_block(f, blocks + ((size_t)by * bw + bx) * bs, blk);
            for (u32 y = 0; y < 4 && by * 4 + y < h; y++)
                for (u32 x = 0; x < 4 && bx * 4 + x < w; x++) memcpy(&rgba[((size_t)(by * 4 + y) * w + bx * 4 + x) * 4], &blk[(y * 4 + x) * 4], 4);
        }
}
