// Engine Fidelity Off (docs/ENGINE_FIDELITY.md): no post-processing, only the output transform of
// the lit HDR scene: a fixed exposure (pc.p0.x, metered on the CPU from the sun / sky light, no
// histogram, no adaptation), a hue-preserving Reinhard on luminance, the colour-blind filter and the
// user's brightness / gamma (accessibility and display calibration stay at every level). Used by
// final_direct.frag (swapchain) and ui_blur_down.comp (menu backdrop).
#include "colorblind.glsl"

vec3 direct_srgb_encode(vec3 c) { return mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
vec3 direct_tonemap(vec3 c) { return c / (1.0 + luma(c)); }

// Display-linear (0..1) -> display-encoded sRGB with the colour-blind filter and the user's
// brightness / gamma.
vec3 direct_display_srgb(vec3 d) {
    vec3 e = direct_srgb_encode(clamp(d, 0.0, 1.0));
    int cb = int(U.s_gain.w + 0.5);
    if (cb > 0) e = colorblind_correct(e, cb);
    return pow(clamp(e * U.s_out.w, 0.0, 1.0), vec3(1.0 / max(U.s_fb_tint.w, 0.2)));
}

// Linear HDR -> display-encoded sRGB (0..1).
vec3 direct_output_srgb(vec3 c, float exposure) { return direct_display_srgb(direct_tonemap(sanitize(c) * exposure)); }
