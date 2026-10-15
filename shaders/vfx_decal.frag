#version 450
// Decal: reconstruct the scene position from depth, project into the decal box, sample albedo +
// normal (atlas), fade by the surface angle and lifetime, light with the sun (+ shadow) and the
// point lights, and blend over the lit scene.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define VFX_SET 1
#include "vfx_common.glsl"
layout(location = 0) flat in uint v_decal;
layout(location = 0) out vec4 out_color;

float shadow_at(vec3 wp) {
    if (frame.sun_color.w < 0.5) return 1.0;
    vec4 sc = frame.shadow_vp * vec4(wp, 1.0);
    vec3 s = sc.xyz / sc.w;
    vec2 uv = s.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    return texture(shadow_map, vec3(uv, s.z - 0.003));
}

void main() {
    Decal d = decals[v_decal];
    vec2 suv = gl_FragCoord.xy * vf.screen.zw;
    float depth = texture(vfx_depth, suv).r;
    vec4 wh = vf.inv_view_proj * vec4(suv * 2.0 - 1.0, depth, 1.0);
    vec3 wp = wh.xyz / wh.w;
    vec3 n = d.normal_depth.xyz, t = d.tangent_angle.xyz, b = cross(n, t);
    vec3 rel = wp - d.pos_size.xyz;
    float hs = d.pos_size.w * 0.5;
    vec3 lp = vec3(dot(rel, t) / hs, dot(rel, b) / hs, dot(rel, n) / d.normal_depth.w);
    if (any(greaterThan(abs(lp), vec3(1.0)))) discard;
    // Surface normal from depth derivatives: reject surfaces facing away from the decal.
    vec3 sn = normalize(cross(dFdx(wp), dFdy(wp)));
    if (dot(sn, vf.cam_pos.xyz - wp) < 0.0) sn = -sn;
    float facing = dot(sn, n);
    if (facing < 0.3) discard;
    vec2 uv = lp.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    uint grid = d.info.x;
    float cell = float(d.info.y);
    vec4 alb = texture(vfx_atlas, vec3(cell_uv(d.uv, grid, cell, uv), float(d.info.z & 255u)));
    vec3 nn = sn;
    if (d.nuv.z > d.nuv.x) {
        vec3 tn = texture(vfx_atlas, vec3(cell_uv(d.nuv, grid, cell, uv), float((d.info.z >> 8) & 255u))).rgb * 2.0 - 1.0;
        nn = normalize(t * tn.x + b * -tn.y + sn * max(tn.z, 0.2));
    }
    float a = alb.a * d.tint.a * smoothstep(0.3, 0.6, facing) * (1.0 - smoothstep(0.7, 1.0, abs(lp.z)));
    if (a < 0.003) discard;
    vec3 albedo = pow(alb.rgb / max(alb.a, 1e-3), vec3(2.2)) * d.tint.rgb;   // sRGB atlas -> linear
    vec3 L = normalize(frame.sun_dir.xyz);
    const float INV_PI = 0.31830989;   // same units as lit_body.glsl
    vec3 light = frame.sun_color.rgb * frame.sun_dir.w * max(dot(nn, L), 0.0) * shadow_at(wp + sn * 0.01) * INV_PI;
    float hemi = nn.y * 0.5 + 0.5;
    light += mix(frame.ground_ambient.rgb, frame.sky_ambient.rgb, hemi) * frame.sky_ambient.w;
    uint nl = uint(frame.ground_ambient.w);
    for (uint i = 0u; i < nl; i++) {
        vec3 dl = frame.lights[i].pos_radius.xyz - wp;
        float dist = length(dl);
        float r = frame.lights[i].pos_radius.w;
        if (dist > r) continue;
        float att = clamp(1.0 - pow(dist / r, 4.0), 0.0, 1.0);
        att = att * att / (dist * dist + 1.0);
        light += frame.lights[i].color_intensity.rgb * frame.lights[i].color_intensity.w * att * max(dot(nn, dl / dist), 0.0) * INV_PI;
    }
    out_color = vec4(albedo * light * a, a);
}
