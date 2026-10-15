#version 450
// Red dot, additive pass: a collimated dot (parallax-free at the sight's parallax distance: its
// direction is the eye -> convergence point, so an eye off the axis still sees it on the same
// target point), MOA-sized, emissive (blooms); the window's fresnel reflection of the sky and
// lit dust specks. Only through the window, and not where viewmodel parts are in front of it.
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
    vec3 emit = S.color.rgb * S.color.w * 8.0 * energy * exposure_inv() * m;
    // Fresnel (Schlick) reflection of the sky ambient off the window.
    float f0 = S.glass.x;
    float fr = f0 + (1.0 - f0) * pow(1.0 - clamp(h.cos_t, 0.0, 1.0), 5.0);
    vec3 refl = U.sky.rgb * max(U.sky.w, 0.0) * fr * 0.6;
    // Lit dust specks: catch the sun.
    float n = textureLod(noise_tex, h.q * 0.35 + 0.5, 0.0).r;
    float speck = smoothstep(0.95, 1.0, n) * S.tint.w;
    vec3 dust = U.sun_color.rgb * U.sun_dir.w * speck * 0.02;
    vec3 col = (emit + refl + dust) * cov;
    if (dbg == 4.0) col = emit * cov;                 // reticle only
    if (dbg == 5.0) col = (refl + dust) * cov;        // glass only
    out_color = vec4(sanitize(col), 0.0);
}
