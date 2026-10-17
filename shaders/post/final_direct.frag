#version 450
// Final output of Engine Fidelity Off: the render-resolution lit HDR straight to the swapchain
// (bilinear upscale, post_direct.glsl output transform, blue-noise dither). No AA, bloom, grading,
// sharpening or grain. HDR10 output is not offered at Off; for the one frame the swapchain may still
// be HDR10 after switching, the result is PQ-encoded at paper white.
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
#include "post_direct.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D hdr_tex;
layout(set = 0, binding = 1) uniform sampler2D noise_tex;
// pc.p0.x exposure (linear multiplier)

vec3 pq_encode(vec3 y) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 p = pow(clamp(y, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main() {
    vec3 c = textureLod(hdr_tex, v_uv, 0.0).rgb;
    vec4 n = blue_noise(noise_tex, ivec2(gl_FragCoord.xy));
    float dither = n.r + n.a - 1.0;
    vec3 o;
    if (U.s_out.x > 0.5) {
        vec3 d = direct_tonemap(sanitize(c) * pc.p0.x);
        o = pq_encode(d * (U.s_out.y / 10000.0)) + dither / 1023.0;
    } else {
        o = direct_output_srgb(c, pc.p0.x) + dither / 255.0;
    }
    // The death sequence's desaturation and black cut work even without a post stack (docs/REVIVE.md).
    o = mix(o, vec3(dot(o, vec3(0.2126, 0.7152, 0.0722))), clamp(U.s_fb2.w, 0.0, 1.0));
    o_color = vec4(clamp(o, 0.0, 1.0) * (1.0 - clamp(U.s_fb3.x, 0.0, 1.0)), 1.0);
}
