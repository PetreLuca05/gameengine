// Look LUT fetch (docs/LOOKS.md, src/render/post/look.h): exposed scene-linear HDR -> display-linear
// through the active look's baked grade. Shaper: log2(c / 0.18) over LOOK_EV_MIN .. LOOK_EV_MAX,
// texel centres of a LOOK_LUT_N^3 volume (r = x, g = y, b = z). Must match look_lut_coord.
const float LOOK_EV_MIN = -11.0;
const float LOOK_EV_MAX = 7.0;
const float LOOK_LUT_N = 32.0;

vec3 look_lut(sampler3D lut, vec3 c) {
    vec3 s = clamp((log2(max(c, vec3(1e-10)) / 0.18) - LOOK_EV_MIN) / (LOOK_EV_MAX - LOOK_EV_MIN), 0.0, 1.0);
    return textureLod(lut, s * ((LOOK_LUT_N - 1.0) / LOOK_LUT_N) + 0.5 / LOOK_LUT_N, 0.0).rgb;
}
