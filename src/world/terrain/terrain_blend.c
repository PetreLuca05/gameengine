#include "terrain_blend.h"
#include <math.h>
#include <string.h>

static float smoothstepf(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3 - 2 * t);
}
static float fractf(float x) { return x - floorf(x); }

float ter_hash(float x, float y) {
    u32 qx = (u32)(i32)floorf(x), qy = (u32)(i32)floorf(y);
    u32 h = qx * 73856093u ^ qy * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFFFu) / 16777216.0f;
}

float ter_noise(float x, float y) {
    float ix = floorf(x), iy = floorf(y), fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = ter_hash(ix, iy), b = ter_hash(ix + 1, iy), c = ter_hash(ix, iy + 1), d = ter_hash(ix + 1, iy + 1);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

float ter_fbm(float x, float y, int octaves) {
    float a = 0.5f, s = 0, t = 0;
    for (int i = 0; i < octaves; i++) {
        s += ter_noise(x, y) * a;
        t += a;
        x = x * 2.03f + 17.3f;
        y = y * 2.03f - 9.1f;
        a *= 0.5f;
    }
    return s / t;
}

void ter_gather(const u8 *blend, const float maps[4], float x, float z, TerWeights *out) {
    memset(out, 0, sizeof(*out));
    int n = (int)maps[3];
    float gx = (x - maps[0]) * maps[2], gz = (z - maps[1]) * maps[2];
    float fx0 = floorf(gx), fz0 = floorf(gz), fx = gx - fx0, fz = gz - fz0;
    for (int k = 0; k < 4; k++) {
        int cx = (int)fx0 + (k & 1), cz = (int)fz0 + (k >> 1);
        cx = cx < 0 ? 0 : (cx >= n ? n - 1 : cx);
        cz = cz < 0 ? 0 : (cz >= n ? n - 1 : cz);
        const u8 *t = &blend[((size_t)cz * n + cx) * 4];
        float bw = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fz : 1 - fz);
        for (int e = 0; e < 2; e++) {
            int id = t[e];
            float w = bw * (float)t[2 + e] / 255.0f;
            if (w <= 0) continue;
            int j = 0;
            while (j < out->n && out->id[j] != id) j++;
            if (j == out->n) {
                if (out->n == TER_GATHER_MAX) continue;
                out->id[out->n] = id;
                out->w[out->n++] = 0;
            }
            out->w[j] += w;
        }
    }
    float s = 0;
    for (int i = 0; i < out->n; i++) s += out->w[i];
    for (int i = 0; i < out->n; i++) out->w[i] /= s > 0 ? s : 1;
    // sort by weight (insertion: at most 8), ties by id (deterministic, order independent)
    for (int i = 1; i < out->n; i++)
        for (int j = i; j > 0 && (out->w[j] > out->w[j - 1] || (out->w[j] == out->w[j - 1] && out->id[j] < out->id[j - 1])); j--) {
            float tw = out->w[j];
            int ti = out->id[j];
            out->w[j] = out->w[j - 1], out->id[j] = out->id[j - 1];
            out->w[j - 1] = tw, out->id[j - 1] = ti;
        }
}

float ter_border_distance(const TerWeights *w, float radius_m, float spacing) {
    if (w->n < 2) return radius_m + 0.5f * spacing;
    float wa = w->w[0], wb = w->w[1];
    return (wa - wb) / (wa + wb) * (radius_m + 0.5f * spacing);
}

float ter_border_shift(const TerPairGpu *q, float x, float z, float crumb_m, const float attr[3], const float slope_dir[2], const float border_dir[2],
                       const float wind[2], int level) {
    u32 m = (u32)q->b[3];
    float s = 0;
    if (m & TPM_NOISE) {
        float sc = 1.0f / fmaxf(q->a[1], 0.05f);
        s += (ter_fbm(x * sc, z * sc, level >= 2 ? 3 : 1) * 2 - 1) * q->a[2];
    }
    if ((m & TPM_CRUMBS) && level >= 2 && q->a[3] > 0) {
        float c = ter_noise(x / crumb_m + 31.7f, z / crumb_m - 5.3f);
        // islands: the top of the noise pushes toward a, the bottom toward b, scaled to cross the band
        s += (smoothstepf(1.0f - q->a[3], 1.0f - q->a[3] * 0.6f, c) - smoothstepf(1.0f - q->a[3], 1.0f - q->a[3] * 0.6f, 1.0f - c)) * q->a[0] * 1.5f;
    }
    if (m & TPM_WEAR) s += q->c[3] * (attr[2] - 0.5f) * 2.0f + (level >= 4 ? q->c[3] * 0.5f * (attr[0] - 0.5f) * 2.0f : 0.0f);
    if (m & TPM_MOISTURE) s += q->d[0] * (attr[1] - 0.5f) * 2.0f;
    if ((m & TPM_SLOPE) && level >= 4) s += q->c[2] * fmaxf(0.0f, -(border_dir[0] * slope_dir[0] + border_dir[1] * slope_dir[1]));
    if ((m & TPM_DIRECTIONAL) && level >= 4 && q->c[0] > 0) {
        float dx = q->c[0] == 1.0f ? wind[0] : slope_dir[0], dz = q->c[0] == 1.0f ? wind[1] : slope_dir[1];
        float k = fmaxf(0.0f, -(border_dir[0] * dx + border_dir[1] * dz));
        s += q->c[1] * k * (0.5f + 0.5f * ter_noise(x * 0.4f, z * 0.4f));
    }
    return s;
}

float ter_band(const TerPairGpu *q, float d) {
    float W = fmaxf(q->a[0], 0.05f);
    return smoothstepf(-0.5f * W, 0.5f * W, d);
}

void ter_height_blend(int n, const float *t, const float *H, float sharpness, float *out) {
    float mx = -1e9f, h[TER_GATHER_MAX];
    for (int i = 0; i < n; i++) {
        h[i] = t[i] + H[i] * fminf(1.0f, 4.0f * t[i]);   // a layer outside its band cannot win on height
        mx = fmaxf(mx, h[i]);
    }
    float s = 0;
    for (int i = 0; i < n; i++) {
        out[i] = fmaxf(h[i] - mx + sharpness, 0.0f);
        s += out[i];
    }
    for (int i = 0; i < n; i++) out[i] /= s > 0 ? s : 1;
}

float ter_set_weight(const TerPairGpu *q, float ta) { return 4.0f * ta * (1.0f - ta) * q->b[1]; }

void ter_hex(float u, float v, float cell, float rot_amount, float scale_jitter, TerHex *o) {
    float x = u / cell, y = v / cell;
    float sx = x - y * 0.57735027f, sy = y * 1.15470054f;   // skew to the triangle grid
    float bx = floorf(sx), by = floorf(sy), fx = sx - bx, fy = sy - by;
    float vx[3], vy[3], w[3];
    if (fx + fy < 1.0f) {
        vx[0] = bx, vy[0] = by, w[0] = 1 - fx - fy;
        vx[1] = bx + 1, vy[1] = by, w[1] = fx;
        vx[2] = bx, vy[2] = by + 1, w[2] = fy;
    } else {
        vx[0] = bx + 1, vy[0] = by + 1, w[0] = fx + fy - 1;
        vx[1] = bx + 1, vy[1] = by, w[1] = 1 - fy;
        vx[2] = bx, vy[2] = by + 1, w[2] = 1 - fx;
    }
    float s = 0;
    for (int i = 0; i < 3; i++) {
        w[i] = powf(fmaxf(w[i], 0.0f), 5.0f);
        s += w[i];
    }
    for (int i = 0; i < 3; i++) {
        o->w[i] = w[i] / s;
        o->centre[i][0] = (vx[i] + vy[i] * 0.5f) * cell;
        o->centre[i][1] = vy[i] * 0.8660254f * cell;
        o->off[i][0] = ter_hash(vx[i], vy[i]);
        o->off[i][1] = ter_hash(vx[i] + 57.0f, vy[i] + 113.0f);
        o->rot[i] = (ter_hash(vx[i] - 31.0f, vy[i] + 7.0f) * 2 - 1) * 3.14159265f * rot_amount;
        o->scale[i] = 1.0f + (ter_hash(vx[i] + 3.0f, vy[i] - 71.0f) * 2 - 1) * scale_jitter;
    }
}

float ter_plain_sample(const float *img, int size, float u, float v) {
    float x = fractf(u) * (float)size - 0.5f, y = fractf(v) * (float)size - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float tx = x - (float)x0, ty = y - (float)y0;
    int xa = (x0 % size + size) % size, xb = (xa + 1) % size, ya = (y0 % size + size) % size, yb = (ya + 1) % size;
    float a = img[ya * size + xa] * (1 - tx) + img[ya * size + xb] * tx;
    float b = img[yb * size + xa] * (1 - tx) + img[yb * size + xb] * tx;
    return a * (1 - ty) + b * ty;
}

float ter_hex_sample(const float *img, int size, float mean, float u, float v, float cell, float rot_amount, float scale_jitter) {
    TerHex h;
    ter_hex(u, v, cell, rot_amount, scale_jitter, &h);
    float acc = 0, w2 = 0;
    for (int i = 0; i < 3; i++) {
        float cs = cosf(h.rot[i]), sn = sinf(h.rot[i]);
        float du = u - h.centre[i][0], dv = v - h.centre[i][1];
        float ru = (cs * du - sn * dv) * h.scale[i] + h.centre[i][0] + h.off[i][0];
        float rv = (sn * du + cs * dv) * h.scale[i] + h.centre[i][1] + h.off[i][1];
        acc += h.w[i] * (ter_plain_sample(img, size, ru, rv) - mean);
        w2 += h.w[i] * h.w[i];
    }
    return mean + acc / sqrtf(fmaxf(w2, 1e-6f));
}
