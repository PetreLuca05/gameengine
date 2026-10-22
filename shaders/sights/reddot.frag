#version 450
// Red dot, additive pass (after glass.frag, which refracts and shades the window): a collimated dot
// (parallax-free at the sight's parallax distance: its direction is the eye -> convergence point, so
// an eye off the axis still sees it on the same target point), MOA-sized, emissive (blooms). The LED
// is reflected by the coating, so the window's refraction does not move it. It glows: a soft halo
// (scatter in the coating and the eye), a faint starburst, and the LED light picked up by the glass
// (a red veil around the dot, dust specks lit red), so it reads in the dark as well as by day. Only
// through the window, and not where viewmodel parts are in front of it.
#extension GL_GOOGLE_include_directive : require
#include "sight_common.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 uv = gl_FragCoord.xy / S.render.zw;
    LensHit h = lens_hit(uv);
    float cov = h.ok ? disc_cov(h.r) : 0.0;
    if (cov <= 0.0 || lens_occluded(uv, h.z)) discard;
    float dbg = S.vmdepth.z;
    // Dot: angular distance between the pixel's main-projection ray and the convergence point.
    vec3 dm = normalize(main_ray(uv));
    vec3 a = normalize(S.dotp.xyz);
    float c = dot(dm, a);
    float px = 2.0 * U.depthp.z / max(S.misc.w, 1.0);         // one output pixel (tangent units)
    float shape = S.glass.z;
    // A 2 MOA dot is often smaller than a pixel: drawn at least ~2.3 px wide, the energy spread
    // (dimmer per pixel) so it reads like the real thing (a small, very bright point that blooms).
    float r = S.dotp.w, r_eff = max(r, 1.15 * px);
    float energy = clamp((r * r) / (r_eff * r_eff) * 3.0, 0.6, 1.0);
    vec3 side = normalize(cross(a, vec3(0.0, 1.0, 0.0)));
    vec3 upv = cross(side, a);
    vec2 t = vec2(dot(dm, side), dot(dm, upv)) / max(c, 1e-4);
    float m = reticle_mask(t, px, shape == RET_CIRCLE_DOT ? RET_CIRCLE_DOT : RET_DOT, r_eff, S.glass.w);
    // Emissive in exposed units: constant on screen whatever the scene exposure, bright enough to bloom.
    float lum = S.color.w * exposure_inv();
    vec3 led = S.color.rgb;
    // Hot core: the centre of a bright LED saturates towards white.
    float core = 1.0 - smoothstep(0.0, r_eff * 0.6, length(t));
    vec3 emit = (led * m + vec3(1.0, 0.75, 0.6) * core * 0.35) * 8.0 * energy * lum;
    // Glow: a halo falling off with the angle (in dot radii), a faint six-point starburst.
    float d = length(t) / max(r_eff, 1e-6);
    float glow = S.coat.y;
    float halo = 1.0 / (1.0 + d * d * 0.35);
    halo = halo * halo * 0.6 + exp(-d * 0.18) * 0.12;
    float ang = atan(t.y, t.x);
    float star = pow(abs(cos(ang * 3.0 + 0.4)), 40.0) * exp(-d * 0.25) * 0.5;
    vec3 glow_c = led * (halo + star) * glow * 2.0 * lum;
    // LED light picked up by the glass: a broad red veil over the window around the dot, dust specks.
    float n = textureLod(noise_tex, h.q * 0.35 + 0.5, 0.0).r;
    float speck = smoothstep(0.93, 0.99, n) * S.tint.w;
    float veil = exp(-d * 0.04) * S.coat.z;
    vec3 scatter = led * (veil * 0.05 + speck * veil * 0.6) * lum;
    vec3 col = (emit + glow_c + scatter) * cov;
    if (dbg == 4.0) col = emit * cov;                 // reticle only
    if (dbg == 5.0) col = vec3(0.0);                  // glass only
    out_color = vec4(sanitize(col), 0.0);
}
