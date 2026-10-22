// Thermal scope sensor: shared by shaders/post/thermal_*.comp (docs/NIGHT_OPTICS.md "Thermal sensor",
// the CPU side render/post/post_thermal.c + render/thermal_sensor.c). The per-frame block (binding 14).
layout(std430, set = 0, binding = 14) readonly buffer Thermal {
    vec4 head;       // x sky zenith depression C, y its exponent, z air C, w display brightness
    vec4 sens;       // x dt, y smear tau (s), z NETD (C), w fixed-pattern drift 0..1
    vec4 agc;        // x lo, y hi (radiance), z digital zoom, w palette
    vec4 flags;      // x frozen (NUC), y dead (battery), z history valid, w OSD chars
    mat4 inv_vp;     // the PiP camera: clip -> world
    vec4 cam;        // its position
    vec4 core;       // x core square (px), y display size, z seed, w quality
    float lut[256];  // band radiance (-60 C, 6.5 C steps)
    uint pal[8 * 256];   // RGBA8 palettes
    uint osd[96];    // OSD characters (ASCII; 10 = the next line)
} T;

float tc_radiance(float t) {
    float x = clamp((t + 60.0) / 6.5, 0.0, 254.999);
    int i = int(x);
    return mix(T.lut[i], T.lut[i + 1], x - float(i));
}

// dL/dT (radiance per C) near a radiance: how much a temperature step moves the signal there.
float tc_dldt(float l) {
    int lo = 0, hi = 255;
    for (int k = 0; k < 8; k++) { int m = (lo + hi) / 2; if (T.lut[m] <= l) lo = m; else hi = m; }
    return max((T.lut[min(lo + 1, 255)] - T.lut[lo]) / 6.5, 1e-4);
}

float tc_temp(float l) {
    int lo = 0, hi = 255;
    for (int k = 0; k < 8; k++) { int m = (lo + hi) / 2; if (T.lut[m] <= l) lo = m; else hi = m; }
    float a = T.lut[lo], b = T.lut[min(lo + 1, 255)];
    return -60.0 + 6.5 * (float(lo) + clamp((l - a) / max(b - a, 1e-6), 0.0, 1.0));
}

uvec3 tc_pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 tc_rand3(uvec3 v) { return vec3(tc_pcg3d(v) >> 8u) * (1.0 / 16777216.0); }

// 5 x 6 bitmap font, ASCII 32 .. 90 (bit r * 5 + x; '*' is the degree sign).
const uint FONT[59] = uint[](
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x31911173u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00001926u, 0x00023880u, 0x00000000u, 0x00007c00u, 0x0c600000u, 0x00111110u,
    0x1d19d72eu, 0x1c4210c4u, 0x3e22222eu, 0x1f083a0fu, 0x108fa988u, 0x1f083c3fu, 0x1d18bc2eu, 0x0422221fu,
    0x1d18ba2eu, 0x1d0f462eu, 0x006000c0u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x231fc62eu, 0x1f18be2fu, 0x1d10862eu, 0x1f18c62fu, 0x3e10bc3fu, 0x0210bc3fu, 0x1d18f42eu,
    0x2318fe31u, 0x1c42108eu, 0x0c94211cu, 0x23149d31u, 0x3e108421u, 0x2318d771u, 0x231cd671u, 0x1d18c62eu,
    0x0210be2fu, 0x2c9ac62eu, 0x2292be2fu, 0x1f08383eu, 0x0842109fu, 0x1d18c631u, 0x08a8c631u, 0x23bac631u,
    0x22a21151u, 0x08421151u, 0x3e11111fu
);
