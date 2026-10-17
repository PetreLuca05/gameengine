// GPU twin of src/world/water/water_waves.c (docs/WATER.md "Waves"): the same active Gerstner set,
// the same polynomials and argument reduction, every intermediate `precise` (no contraction /
// reassociation), so the swell height here matches water_height_at on the CPU to float rounding.
//
// The includer declares the component data and defines, before the include:
//   WAVE_N          number of active components (uint)
//   WAVE_A(i)       vec4 (dx, dz, k, omega)
//   WAVE_B(i)       vec4 (amp, qa, phase, 0)
// (the C upload is WaveActive[WAVE_ACTIVE]: 2 vec4 each, same order).
#ifndef WATER_WAVES_GLSL
#define WATER_WAVES_GLSL

// Nearest integer, ties to even: the C side's (x + 1.5 * 2^23) - 1.5 * 2^23.
float wv_round(float x) { return roundEven(x); }

void wv_sincos(precise float x, out float s_out, out float c_out) {
    precise float q = wv_round(x * 0.636619772);
    precise float r = ((x - q * 1.5703125) - q * 4.837512969970703125e-4) - q * 7.54978995489188216e-8;
    precise float z = r * r;
    precise float s = r + r * z * (-1.6666654611e-1 + z * (8.3321608736e-3 + z * -1.9515295891e-4));
    precise float c = 1.0 - 0.5 * z + z * z * (4.166664568298827e-2 + z * (-1.388731625493765e-3 + z * 2.443315711809948e-5));
    int qi = int(q) & 3;
    float sv = (qi & 1) != 0 ? c : s;
    float cv = (qi & 1) != 0 ? s : c;
    s_out = (qi & 2) != 0 ? -sv : sv;
    c_out = ((qi + 1) & 2) != 0 ? -cv : cv;
}

// Gerstner displacement of the undisplaced point p0 (xz) at wave time t: (dx, dy, dz).
vec3 wv_displace(vec2 p0, float t) {
    precise vec3 o = vec3(0.0);
    for (uint i = 0u; i < WAVE_N; i++) {
        vec4 a = WAVE_A(i), b = WAVE_B(i);
        float s, c;
        precise float th = a.z * (a.x * p0.x + a.y * p0.y) - a.w * t + b.z;
        wv_sincos(th, s, c);
        o.x -= a.x * b.y * s;
        o.y += b.x * c;
        o.z -= a.y * b.y * s;
    }
    return o;
}

// Undisplaced point whose displaced position is p (2 fixed-point steps, as waves_height).
vec2 wv_invert(vec2 p, float t) {
    precise vec2 p0 = p;
    for (int it = 0; it < 2; it++) {
        vec3 d = wv_displace(p0, t);
        p0 = p - d.xz;
    }
    return p0;
}

// Swell height above the still level at world xz (== waves_height(m, x, z, t, 2)).
float wv_height(vec2 p, float t) { return wv_displace(wv_invert(p, t), t).y; }

// Height, normal and jacobian at world xz (== waves_sample). Returns vec4(normal, height); jac out.
vec4 wv_sample(vec2 p, float t, out float jac) {
    vec2 p0 = wv_invert(p, t);
    precise float h = 0.0, xx = 1.0, xz = 0.0, zz = 1.0, yx = 0.0, yz = 0.0;
    for (uint i = 0u; i < WAVE_N; i++) {
        vec4 a = WAVE_A(i), b = WAVE_B(i);
        float s, c;
        precise float th = a.z * (a.x * p0.x + a.y * p0.y) - a.w * t + b.z;
        wv_sincos(th, s, c);
        precise float qkc = b.y * a.z * c, aks = b.x * a.z * s;
        h += b.x * c;
        xx -= a.x * a.x * qkc;
        xz -= a.x * a.y * qkc;
        zz -= a.y * a.y * qkc;
        yx -= aks * a.x;
        yz -= aks * a.y;
    }
    vec3 n = normalize(vec3(yz * xz - zz * yx, zz * xx - xz * xz, xz * yx - xx * yz));
    jac = xx * zz - xz * xz;
    return vec4(n, h);
}

// Swell fade towards the beach (water.c shore_fade): still depth over SHORE_DEPTH = 3 m.
float wv_shore_fade(float still_depth) { return smoothstep(0.0, 1.0, clamp(still_depth / 3.0, 0.0, 1.0)); }

#endif
