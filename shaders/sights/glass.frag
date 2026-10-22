#version 450
// Red dot window, opaque full-screen pass into a copy of the scene (outside the window: the scene
// unchanged). Through the window, what is behind it is refracted by the window, a weak meniscus:
//   - Prentice's rule: a ray at height q crosses a lens of power P deviated by q P (slight
//     magnification), plus spherical aberration growing as r^5: the edge distortion;
//   - the ground bevel at the rim: a strong prism band (the image folds and squeezes there);
//   - dispersion: blue bends more than red (colour fringes growing towards the rim).
// Shading: two-surface fresnel transmission, the coating tint (the dichroic film reflects the LED's
// red, so the light that gets through goes cyan), longer path / absorption at grazing angles and at
// the thick rim, the dark ground edge; reflections: the sky through a thin-film coating whose hue
// slides with the angle (gold / magenta face on, blue at grazing), a sun glint on the curved face and
// along the bevel; dust specks dim the glass and catch the sun. The dot itself is reflected by the
// coating, not refracted: reddot.frag (additive, after this).
#extension GL_GOOGLE_include_directive : require
#include "sight_common.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// Screen uv of a main-view direction (inverse of main_ray: the projection has no skew).
vec2 main_uv(vec3 d) {
    vec3 p = d / max(-d.z, 1e-5);
    return vec2((p.x - S.ray_main0.x) / S.ray_main_du.x, (p.y - S.ray_main0.y) / S.ray_main_dv.y);
}

// Thin-film reflectance per channel (R, G, B at 650 / 540 / 450 nm): film n 1.38, thickness nm,
// ci = cosine of the angle inside the film.
vec3 film(float thick, float ci) {
    vec3 ph = 2.0 * 1.38 * thick * ci / vec3(650.0, 540.0, 450.0);
    return 0.5 - 0.5 * cos(6.2831853 * ph);
}

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec2 uv = gl_FragCoord.xy / S.render.zw;
    vec4 src = texelFetch(scene_tex, pix, 0);
    LensHit h = lens_hit(uv);
    float cov = h.ok ? disc_cov(h.r) : 0.0;
    if (cov <= 0.0 || lens_occluded(uv, h.z)) { out_color = src; return; }
    float dbg = S.vmdepth.z;
    float rc = min(h.r, 1.0);
    vec2 dir = h.r > 1e-5 ? h.q / h.r : vec2(0.0);
    vec3 dv = dir.x * S.right.xyz + dir.y * S.up.xyz;      // radial direction on the window (view space)
    float bevel = max(S.refr.z, 1e-3);
    float band = smoothstep(1.0 - bevel, 1.0, rc);

    // Refraction: angular deviation (rad) towards the axis (positive power magnifies).
    float r2 = rc * rc;
    float dev = (S.refr.x * rc + S.refr.y * r2 * r2 * rc + 3.0 * S.refr.y * band * band) * 1e-3;
    vec3 dm = normalize(main_ray(uv));
    float disp = S.refr.w;
    vec3 kd = dev * vec3(1.0 - disp, 1.0, 1.0 + disp);
    vec3 bg = vec3(textureLod(scene_tex, main_uv(dm - kd.r * dv), 0.0).r,
                   textureLod(scene_tex, main_uv(dm - kd.g * dv), 0.0).g,
                   textureLod(scene_tex, main_uv(dm - kd.b * dv), 0.0).b);

    // Transmission: fresnel at both faces, the coating tint over the glass path (longer at grazing
    // angles and through the thick rim), the dark ground edge, dust.
    float ct = clamp(h.cos_t, 0.0, 1.0);
    float f0 = S.glass.x;
    float fr = f0 + (1.0 - f0) * pow(1.0 - ct, 5.0);
    float path = (1.0 + 1.5 * band) / max(ct, 0.25);
    vec3 trans = pow(max(S.tint.rgb, vec3(1e-3)), vec3(path)) * (1.0 - fr) * (1.0 - fr);
    trans *= 1.0 - 0.65 * smoothstep(1.0 - 0.35 * bevel, 1.0, rc);
    float n = textureLod(noise_tex, h.q * 0.35 + 0.5, 0.0).r;
    float speck = smoothstep(0.93, 0.99, n) * S.tint.w;
    trans *= 1.0 - 0.35 * speck;

    // Reflection: the curved face (normal tilts with the height, steeply on the bevel).
    vec3 nrm = normalize(-S.fwd.xyz + dv * (0.12 * rc + 0.9 * band));
    vec3 vd = normalize(vm_ray(uv));
    float cn = clamp(dot(-vd, nrm), 0.0, 1.0);
    float frn = f0 + (1.0 - f0) * pow(1.0 - cn, 5.0);
    float ci = sqrt(1.0 - (1.0 - cn * cn) / (1.38 * 1.38));
    vec3 sheen = mix(vec3(1.0), film(250.0 + 40.0 * rc, ci) * 2.0, clamp(S.coat.x, 0.0, 1.0));
    vec3 sky = U.sky.rgb * max(U.sky.w, 0.0);
    vec3 refl = sky * frn * 0.6 * sheen;
    vec3 sun_v = normalize((U.view * vec4(U.sun_dir.xyz, 0.0)).xyz);
    float sd = max(dot(reflect(vd, nrm), sun_v), 0.0);
    vec3 sun = U.sun_color.rgb * max(U.sun_dir.w, 0.0);
    refl += sun * frn * sheen * (pow(sd, 900.0) * 3.0 + pow(sd, 60.0) * 0.04 * band);
    refl += sun * speck * 0.02;

    vec3 col = bg * trans + refl;
    if (dbg == 1.0) col = bg * vec3(0.3, 1.0, 0.3);                      // lens mask
    if (dbg == 7.0) col = vec3(dev * 40.0, band, 0.0) * exposure_inv();   // refraction (red) / bevel (green)
    out_color = vec4(mix(src.rgb, sanitize(col), cov), src.a);
}
