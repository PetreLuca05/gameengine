// Macro variation shared by the terrain and the ground cover (frame.ground_macro*, assets/terrain/terrain.json
// "macro"): two fBm fields at 50..500 m scales shift the hue toward straw (dry) or green, the saturation
// and the value. Grass and the ground under it use the same field, so the grass fade line does not show.
#ifndef TERRAIN_MACRO_GLSL
#define TERRAIN_MACRO_GLSL
#include "terrain/terrain_noise.glsl"

// Hue rotation of a linear colour about the grey axis (YIQ), angle in radians.
vec3 tn_hue(vec3 c, float a) {
    const mat3 toYIQ = mat3(0.299, 0.596, 0.211, 0.587, -0.274, -0.523, 0.114, -0.322, 0.312);
    const mat3 toRGB = mat3(1.0, 1.0, 1.0, 0.956, -0.272, -1.106, 0.621, -0.647, 1.703);
    vec3 yiq = toYIQ * c;
    float cs = cos(a), sn = sin(a);
    yiq.yz = vec2(yiq.y * cs - yiq.z * sn, yiq.y * sn + yiq.z * cs);
    return max(toRGB * yiq, vec3(0.0));
}
vec3 tn_sat(vec3 c, float k) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return max(mix(vec3(l), c, k), vec3(0.0));
}
// The macro field at xz: x hue (-1..1), y saturation (-1..1), z value (-1..1).
vec3 ground_macro_field(vec2 xz) {
    float a = tn_fbm(xz * frame.ground_macro.x, 4) * 2.0 - 1.0;
    float b = tn_fbm(xz * frame.ground_macro.y + vec2(31.7, 11.3), 3) * 2.0 - 1.0;
    return vec3(a, b * 0.7 + a * 0.3, b * 0.8 - a * 0.4) * frame.ground_macro2.y;
}
vec3 ground_macro_apply(vec3 c, vec3 f, float strength) {
    c = tn_hue(c, f.x * frame.ground_macro.z * strength);
    c = tn_sat(c, 1.0 + f.y * frame.ground_macro2.x * strength);
    return c * (1.0 + f.z * frame.ground_macro.w * strength);
}
#endif
