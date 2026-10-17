// Tiling breakers (docs/TERRAIN_SHADING.md "Breaking the tiling"; CPU mirror ter_hex in
// src/world/terrain/terrain_blend.c):
//   cells  per-cell 90 degree rotation + mirror on two grids offset by half a cell, blended by the
//          distance to the cell edges (2 taps; Off / Low)
//   hex    hex-tile stochastic bombing: the 3 hex cells around the point, each with a random offset,
//          rotation and scale, weights sharpened, histogram-preserving blend (Mikkelsen 2022; 3 taps)
// Both blend around the texture's mean with mean + sum w_i (t_i - mean) / sqrt(sum w_i^2), which keeps the
// texture's mean and variance (a plain average would wash the contrast out where cells overlap).
// Tangent normals are rotated / mirrored with their cell. Gradients follow the rotation (textureGrad).
#ifndef TERRAIN_TILE_GLSL
#define TERRAIN_TILE_GLSL
#include "terrain/terrain_noise.glsl"
#include "terrain/terrain_params.glsl"

struct TerTap { vec4 alb; vec4 nrh; };   // alb: linear albedo, a AO; nrh: normal xy (0..1), roughness, height

int g_ter_cell = 0;                      // debug view: the dominant bombing cell's hash

vec2 ter_rot(vec2 v, float a) {
    float c = cos(a), s = sin(a);
    return vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

TerTap ter_tap(float l, vec2 uv, vec2 gx, vec2 gy) {
    TerTap t;
    t.alb = textureGrad(terrain_alb, vec3(uv, l), gx, gy);
    t.nrh = textureGrad(terrain_nrh, vec3(uv, l), gx, gy);
    return t;
}

// The layer's mean (its last mip): the centre of the histogram-preserving blend.
TerTap ter_mean(float l) {
    TerTap t;
    t.alb = textureLod(terrain_alb, vec3(0.5, 0.5, l), 16.0);
    t.nrh = textureLod(terrain_nrh, vec3(0.5, 0.5, l), 16.0);
    return t;
}

// A tap through a rotation a, mirror m (+-1) and scale s about centre c, plus offset o (uv in tiles); the
// tangent normal follows.
TerTap ter_tap_xf(float l, vec2 uv, vec2 gx, vec2 gy, vec2 c, float a, float m, float s, vec2 o) {
    vec2 u = ter_rot((uv - c) * vec2(m, 1.0), a) * s + c + o;
    TerTap t = ter_tap(l, u, ter_rot(gx * vec2(m, 1.0), a) * s, ter_rot(gy * vec2(m, 1.0), a) * s);
    vec2 nt = t.nrh.xy * 2.0 - 1.0;
    nt = ter_rot(nt, -a) * vec2(m, 1.0);   // back into the surface's frame (undo the rotation, then the mirror)
    t.nrh.xy = nt * 0.5 + 0.5;
    return t;
}

void ter_acc(inout TerTap acc, TerTap t, TerTap mean, float w) {
    acc.alb += (t.alb - mean.alb) * w;
    acc.nrh += (t.nrh - mean.nrh) * w;
}
TerTap ter_resolve(TerTap acc, TerTap mean, float w2) {
    float k = inversesqrt(max(w2, 1e-6));
    TerTap t;
    t.alb = clamp(mean.alb + acc.alb * k, 0.0, 1.0);
    t.nrh = clamp(mean.nrh + acc.nrh * k, 0.0, 1.0);
    return t;
}

TerTap ter_tap_cells(float l, vec2 uv, vec2 gx, vec2 gy, float cell, TerTap mean) {
    TerTap acc = TerTap(vec4(0.0), vec4(0.0));
    float wsum = 0.0, w2 = 0.0, wt[2];
    vec2 f0 = fract(uv / cell), f1 = fract(uv / cell + 0.5);
    wt[0] = (0.5 - abs(f0.x - 0.5)) * (0.5 - abs(f0.y - 0.5));
    wt[1] = (0.5 - abs(f1.x - 0.5)) * (0.5 - abs(f1.y - 0.5));
    wsum = wt[0] + wt[1];
    for (int k = 0; k < 2; k++) {
        vec2 g = uv / cell + (k == 1 ? vec2(0.5) : vec2(0.0));
        vec2 cid = floor(g) + (k == 1 ? vec2(1000.0) : vec2(0.0));
        vec2 h = tn_hash2(cid);
        vec2 c = (floor(g) + 0.5 - (k == 1 ? vec2(0.5) : vec2(0.0))) * cell;
        float w = wt[k] / max(wsum, 1e-5);
        w = w * w * (3.0 - 2.0 * w);
        ter_acc(acc, ter_tap_xf(l, uv, gx, gy, c, floor(h.x * 4.0) * 1.5707963, h.y > 0.5 ? -1.0 : 1.0, 1.0, h * 7.31), mean, w);
        w2 += w * w;
        if (k == 0) g_ter_cell = int(h.x * 255.0);
    }
    return ter_resolve(acc, mean, w2);
}

TerTap ter_tap_hex(float l, vec2 uv, vec2 gx, vec2 gy, float cell, float rot_amount, float scale_jitter, TerTap mean) {
    vec2 st = uv / cell;
    vec2 sk = vec2(st.x - st.y * 0.57735027, st.y * 1.15470054);
    vec2 b = floor(sk), f = sk - b;
    vec2 v[3];
    vec3 w;
    if (f.x + f.y < 1.0) {
        v[0] = b; v[1] = b + vec2(1, 0); v[2] = b + vec2(0, 1);
        w = vec3(1.0 - f.x - f.y, f.x, f.y);
    } else {
        v[0] = b + vec2(1, 1); v[1] = b + vec2(1, 0); v[2] = b + vec2(0, 1);
        w = vec3(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x);
    }
    w = pow(max(w, vec3(0.0)), vec3(5.0));
    w /= w.x + w.y + w.z;
    TerTap acc = TerTap(vec4(0.0), vec4(0.0));
    float best = 0.0;
    for (int i = 0; i < 3; i++) {
        vec2 c = vec2(v[i].x + v[i].y * 0.5, v[i].y * 0.8660254) * cell;
        vec2 o = vec2(tn_hash(v[i]), tn_hash(v[i] + vec2(57.0, 113.0)));
        float a = (tn_hash(v[i] + vec2(-31.0, 7.0)) * 2.0 - 1.0) * 3.14159265 * rot_amount;
        float s = 1.0 + (tn_hash(v[i] + vec2(3.0, -71.0)) * 2.0 - 1.0) * scale_jitter;
        ter_acc(acc, ter_tap_xf(l, uv, gx, gy, c, a, 1.0, s, o), mean, w[i]);
        if (w[i] > best) { best = w[i]; g_ter_cell = int(o.x * 255.0); }
    }
    return ter_resolve(acc, mean, dot(w, w));
}

// A layer's tap at uv (tiles): its tiling mode capped by the level (Off / Low: cells, Medium+: hex within
// hex_far, cells beyond), the uv scale halving past the layer's distance (crossfade), multi-scale albedo
// detail (Medium+) from two far taps in noise patches.
TerTap ter_sample(TerLayer L, float l, vec2 uv, vec2 gx, vec2 gy, int level, float dist, vec2 xz) {
    TerTap mean = ter_mean(l);
    int mode = int(L.tiling.x);
    if (level < 2 && mode == 2) mode = 1;
    if (mode == 2 && dist > TB.g.dist.x) mode = 1;
    float halve = smoothstep(L.multi.w, L.multi.w * 1.5, dist);
    vec2 u = uv * (1.0 - 0.5 * halve);
    vec2 ugx = gx * (1.0 - 0.5 * halve), ugy = gy * (1.0 - 0.5 * halve);
    TerTap t;
    if (mode == 2) t = ter_tap_hex(l, u, ugx, ugy, max(L.tiling.y, 0.1), L.tiling.w, L.tiling.z, mean);
    else if (mode == 1) t = ter_tap_cells(l, u, ugx, ugy, max(L.tiling.y, 0.1) * 2.0, mean);
    else t = ter_tap(l, u, ugx, ugy);
    if (level >= 2 && L.multi.z > 0.0) {
        // multi-scale: the texture at 1 / 0.23 and 1 / 0.07 of its scale, rotated, as luminance ratios in
        // world-noise patches (low-frequency variation the single scale cannot have)
        float n1 = smoothstep(0.35, 0.75, tn_noise(xz * 0.031)), n2 = smoothstep(0.4, 0.8, tn_noise(xz * 0.011 + 7.7));
        vec3 m = max(mean.alb.rgb, vec3(0.004));
        vec3 f1 = textureGrad(terrain_alb, vec3(ter_rot(uv * L.multi.x, 1.1) + 0.37, l), gx * L.multi.x, gy * L.multi.x).rgb / m;
        vec3 f2 = textureGrad(terrain_alb, vec3(ter_rot(uv * L.multi.y, -2.3) + 0.71, l), gx * L.multi.y, gy * L.multi.y).rgb / m;
        t.alb.rgb *= mix(vec3(1.0), f1, n1 * L.multi.z) * mix(vec3(1.0), f2, n2 * L.multi.z * 0.7);
    }
    return t;
}
#endif
