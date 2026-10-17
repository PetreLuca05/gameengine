// Material weights and transitions (docs/TERRAIN_SHADING.md "Transitions"; CPU mirror
// src/world/terrain/terrain_blend.c): the blend map gives each material its box fraction; the top two
// fractions give the signed distance to their border in metres; the pair's techniques shift that border
// (noise-broken edge, crumbs, wear, moisture, slope, wind / downhill drift) and its band width shapes the
// blend; the layers' heights then decide who wins inside the band (height blending, order independent).
#ifndef TERRAIN_WEIGHTS_GLSL
#define TERRAIN_WEIGHTS_GLSL
#include "terrain/terrain_noise.glsl"
#include "terrain/terrain_params.glsl"

#define TER_GATHER 8
struct TerW { int n; int id[TER_GATHER]; float w[TER_GATHER]; };

// The blend map's 4 samples around xz (bilinear), each giving its fraction to its dominant and second
// material; merged, normalised, sorted by weight (ties: lower id first).
TerW ter_gather(vec2 xz) {
    TerW o;
    o.n = 0;
    for (int i = 0; i < TER_GATHER; i++) { o.id[i] = 0; o.w[i] = 0.0; }
    vec2 g = (xz - frame.surf_map.xy) * frame.surf_map.z;
    vec2 gf = floor(g), fr = g - gf;
    int n = int(frame.surf_map.w);
    for (int k = 0; k < 4; k++) {
        ivec2 c = clamp(ivec2(gf) + ivec2(k & 1, k >> 1), ivec2(0), ivec2(n - 1));
        vec4 t = texelFetch(terrain_blend, c, 0);
        float bw = ((k & 1) != 0 ? fr.x : 1.0 - fr.x) * ((k >> 1) != 0 ? fr.y : 1.0 - fr.y);
        for (int e = 0; e < 2; e++) {
            int id = min(int(t[e] * 255.0 + 0.5), TER_SLOTS - 1);
            float w = bw * t[2 + e];
            if (w <= 0.0) continue;
            int j = 0;
            while (j < o.n && o.id[j] != id) j++;
            if (j == o.n) {
                if (o.n == TER_GATHER) continue;
                o.id[j] = id;
                o.w[j] = 0.0;
                o.n++;
            }
            o.w[j] += w;
        }
    }
    float s = 0.0;
    for (int i = 0; i < o.n; i++) s += o.w[i];
    for (int i = 0; i < o.n; i++) o.w[i] /= max(s, 1e-6);
    for (int i = 1; i < o.n; i++)
        for (int j = i; j > 0; j--) {
            if (!(o.w[j] > o.w[j - 1] || (o.w[j] == o.w[j - 1] && o.id[j] < o.id[j - 1]))) break;
            float tw = o.w[j];
            int ti = o.id[j];
            o.w[j] = o.w[j - 1];
            o.id[j] = o.id[j - 1];
            o.w[j - 1] = tw;
            o.id[j - 1] = ti;
        }
    return o;
}

// The pair's border shift (m) at xz: terrain_blend.c ter_border_shift.
float ter_border_shift(TerPair q, vec2 xz, vec3 attr, vec2 slope_dir, vec2 border_dir, vec2 wind, int level) {
    uint m = uint(q.b.w);
    float s = 0.0;
    if ((m & TPM_NOISE) != 0u) {
        float sc = 1.0 / max(q.a.y, 0.05);
        s += (tn_fbm(xz * sc, level >= 2 ? 3 : 1) * 2.0 - 1.0) * q.a.z;
    }
    if ((m & TPM_CRUMBS) != 0u && level >= 2 && q.a.w > 0.0) {
        float c = tn_noise(xz / TB.g.macro.y + vec2(31.7, -5.3));
        s += (smoothstep(1.0 - q.a.w, 1.0 - q.a.w * 0.6, c) - smoothstep(1.0 - q.a.w, 1.0 - q.a.w * 0.6, 1.0 - c)) * q.a.x * 1.5;
    }
    if ((m & TPM_WEAR) != 0u) s += q.c.w * (attr.z - 0.5) * 2.0 + (level >= 4 ? q.c.w * 0.5 * (attr.x - 0.5) * 2.0 : 0.0);
    if ((m & TPM_MOISTURE) != 0u) s += q.d.x * (attr.y - 0.5) * 2.0;
    if ((m & TPM_SLOPE) != 0u && level >= 4) s += q.c.z * max(0.0, -dot(border_dir, slope_dir));
    if ((m & TPM_DIRECTIONAL) != 0u && level >= 4 && q.c.x > 0.0) {
        vec2 dir = q.c.x == 1.0 ? wind : slope_dir;
        s += q.c.y * max(0.0, -dot(border_dir, dir)) * (0.5 + 0.5 * tn_noise(xz * 0.4));
    }
    return s;
}

float ter_band(TerPair q, float d) {
    float W = max(q.a.x, 0.05);
    return smoothstep(-0.5 * W, 0.5 * W, d);
}

// Height blend (terrain_blend.c ter_height_blend): a layer outside its band cannot win on height.
void ter_height_blend(int n, float t[4], float H[4], float sharp, out float o[4]) {
    float mx = -1e9, h[4];
    for (int i = 0; i < 4; i++) {
        h[i] = i < n ? t[i] + H[i] * min(1.0, 4.0 * t[i]) : -1e9;
        mx = max(mx, h[i]);
    }
    float s = 0.0;
    for (int i = 0; i < 4; i++) {
        o[i] = i < n ? max(h[i] - mx + sharp, 0.0) : 0.0;
        s += o[i];
    }
    for (int i = 0; i < 4; i++) o[i] /= max(s, 1e-6);
}
#endif
