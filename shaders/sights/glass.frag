#version 450
// Sight glass, multiplicative pass: the red dot window's coated glass tints what is behind it
// (blend: dst * src). Dust dims the glass slightly in specks. Outside the window: 1 (no change).
#extension GL_GOOGLE_include_directive : require
#include "sight_common.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 uv = gl_FragCoord.xy / S.render.zw;
    LensHit h = lens_hit(uv);
    float cov = h.ok ? disc_cov(h.r) : 0.0;
    if (cov <= 0.0 || lens_occluded(uv, h.z)) { out_color = vec4(1.0); return; }
    vec3 tint = S.tint.rgb;
    // Dust: a few dark specks (noise in lens space, stable with the optic).
    float n = textureLod(noise_tex, h.q * 0.35 + 0.5, 0.0).r;
    float speck = smoothstep(0.93, 0.99, n) * S.tint.w;
    vec3 m = tint * (1.0 - 0.35 * speck);
    float dbg = S.vmdepth.z;
    if (dbg == 1.0) m = vec3(0.3, 1.0, 0.3);          // lens mask
    out_color = vec4(mix(vec3(1.0), m, cov), 1.0);
}
