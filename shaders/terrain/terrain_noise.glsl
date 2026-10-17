// Noise shared by the terrain shading and the ground cover (docs/TERRAIN_SHADING.md): a 2D hash,
// value noise, fBm. tn_hash2 / tn_noise mirror src/world/terrain/terrain_blend.c (CPU tests).
#ifndef TERRAIN_NOISE_GLSL
#define TERRAIN_NOISE_GLSL
float tn_hash(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p)));
    uint h = q.x * 73856093u ^ q.y * 19349663u;
    h ^= h >> 13u;
    h *= 0x5bd1e995u;
    h ^= h >> 15u;
    return float(h & 0xFFFFFFu) / 16777216.0;
}
vec2 tn_hash2(vec2 p) { return vec2(tn_hash(p), tn_hash(p + vec2(57.0, 113.0))); }
float tn_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(tn_hash(i), tn_hash(i + vec2(1, 0)), f.x), mix(tn_hash(i + vec2(0, 1)), tn_hash(i + vec2(1, 1)), f.x), f.y);
}
// fBm in [0, 1] (octaves of tn_noise, lacunarity 2.03, gain 0.5).
float tn_fbm(vec2 p, int octaves) {
    float a = 0.5, s = 0.0, t = 0.0;
    for (int i = 0; i < octaves; i++) {
        s += tn_noise(p) * a;
        t += a;
        p = p * 2.03 + vec2(17.3, -9.1);
        a *= 0.5;
    }
    return s / t;
}
#endif
