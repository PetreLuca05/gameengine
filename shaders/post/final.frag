#version 450
// Final output (swapchain, before the UI): AMD FidelityFX CAS-style contrast adaptive sharpening,
// luminance-weighted film grain, then the output encode with blue-noise triangular dither:
//   SDR:   sRGB OETF -> 8-bit UNORM swapchain
//   HDR10: SMPTE ST.2084 (PQ) -> 10-bit swapchain
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D ldr_tex;
layout(set = 0, binding = 1) uniform sampler2D noise_tex;
// pc.u0.x debug view active (no sharpen / grain)

vec3 srgb_encode(vec3 c) { return mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
vec3 pq_encode(vec3 y) {   // y = nits / 10000
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 p = pow(clamp(y, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 sz = textureSize(ldr_tex, 0);
    bool hdr = U.s_out.x > 0.5;
    float norm = hdr ? 10000.0 / max(U.s_out.y, 1.0) : 1.0;   // CAS works on paper-white-relative values
    vec3 e = texelFetch(ldr_tex, clamp(p, ivec2(0), sz - 1), 0).rgb * norm;
    float sharp = U.s_lens2.z;
    if (sharp > 0.0 && pc.u0.x == 0u) {
        #define T(x, y) min(texelFetch(ldr_tex, clamp(p + ivec2(x, y), ivec2(0), sz - 1), 0).rgb * norm, vec3(1.0))
        vec3 a = T(-1, -1), b = T(0, -1), c = T(1, -1), d = T(-1, 0), f = T(1, 0), g = T(-1, 1), h = T(0, 1), i = T(1, 1);
        vec3 ec = min(e, vec3(1.0));
        vec3 mn = min(min(min(d, ec), min(f, b)), h);
        vec3 mn2 = min(mn, min(min(a, c), min(g, i)));
        mn += mn2;
        vec3 mx = max(max(max(d, ec), max(f, b)), h);
        vec3 mx2 = max(mx, max(max(a, c), max(g, i)));
        mx += mx2;
        vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0));
        float peak = -1.0 / mix(8.0, 5.0, clamp(sharp, 0.0, 1.0));
        vec3 w = amp * peak;
        vec3 s = ((b + d + f + h) * w + ec) / (1.0 + 4.0 * w);
        e = mix(e, max(s, vec3(0.0)) + (e - ec), 1.0);   // keep the HDR excess above paper white
        #undef T
    }
    vec4 n = blue_noise(noise_tex, p);
    float grain = U.s_lens2.y;
    if (grain > 0.0 && pc.u0.x == 0u) {
        float l = luma(min(e, vec3(1.0)));
        float g = (n.g + n.b - 1.0) * grain * 0.35 * (1.0 - l) * (0.3 + 0.7 * sqrt(l));   // strongest in the mid-shadows
        e = max(e + vec3(g) * max(l, 0.02), vec3(0.0));
    }
    vec3 o;
    float dither = n.r + n.a - 1.0;   // triangular -1..1
    if (hdr) {
        o = pq_encode(e / norm) + dither / 1023.0;
    } else {
        o = srgb_encode(clamp(e, 0.0, 1.0)) + dither / 255.0;
    }
    o_color = vec4(clamp(o, 0.0, 1.0) * (1.0 - clamp(U.s_fb3.x, 0.0, 1.0)), 1.0);   // death black: no grain / dither left
}
