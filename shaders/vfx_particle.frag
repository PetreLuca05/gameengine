#version 450
// Particle shading. Atlas textures are premultiplied. Additive: rgb * color * emissive.
// Alpha (premultiplied blend): albedo lit by the per-particle light (simple or 6-way maps) plus an
// emissive term. Distortion: refracts the scene copy by the normal sprite. All paths fade with soft
// particles (scene depth), near-camera fade and flipbook motion-vector blending. Fog: drawn after
// fog_apply (the opaque scene and the sky are already fogged), so each particle fogs its own
// contribution like the blended lit draws: the froxel volume up to its depth, or the analytic fog.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define VFX_SET 1
#include "vfx_common.glsl"

layout(push_constant) uniform Push { uint mode; uint pad0, pad1, pad2; } pc;   // pad0 = 1: scope picture-in-picture
layout(location = 0) in vec3 v_uv_a;
layout(location = 1) in vec3 v_uv_b;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in uvec4 v_info;
layout(location = 4) in vec4 v_misc;
layout(location = 5) in vec3 v_l0;
layout(location = 6) in vec3 v_l1;
layout(location = 7) in vec3 v_l2;
layout(location = 8) in vec3 v_l3;
layout(location = 9) in vec3 v_l4;
layout(location = 10) in vec3 v_l5;
layout(location = 11) in vec3 v_amb;
layout(location = 12) in vec2 v_local;
layout(location = 13) in vec2 v_mv_a;
layout(location = 14) in vec2 v_mv_b;
layout(location = 15) in vec4 v_fog;    // xyz world position, w 1 = fogged
layout(set = 2, binding = 0) uniform sampler3D vfx_froxel;   // integrated volumetric fog (frame.post_fog.x)

layout(location = 0) out vec4 out_color;

float lin_depth(float d) {
    float n = vf.depth_params.x, f = vf.depth_params.y;
    return n * f / (f - d * (f - n));
}

// Fog between the camera and this fragment: rgb in-scattering, a transmittance.
vec4 particle_fog(vec2 suv) {
    if (v_fog.w < 0.5) return vec4(0.0, 0.0, 0.0, 1.0);
    vec3 d = v_fog.xyz - frame.camera_pos.xyz;
    if (frame.post_fog.x > 0.5) {
        float vz = max(dot(d, -vec3(frame.view[0][2], frame.view[1][2], frame.view[2][2])), 0.15);
        float w = clamp(log(vz / 0.15) / log(frame.post_fog.y / 0.15), 0.0, 1.0);
        // The slice stores the integral up to its far edge (as in fog_apply): half a slice back.
        w = clamp(w - 0.5 / float(textureSize(vfx_froxel, 0).z), 0.0, 1.0);
        return textureLod(vfx_froxel, vec3(suv, w), 0.0);
    }
    float f = clamp(1.0 - exp(-length(d) * frame.fog.w), 0.0, 1.0);
    return vec4(frame.fog.rgb * f, 1.0 - f);
}

vec4 sample_flip(Emitter e, out vec4 lm0, out vec4 lm1) {
    float blend = v_misc.x;
    vec2 ua = v_uv_a.xy, ub = v_uv_b.xy;
    bool mv = ((e.modes.x >> 16) & EF_MOTION_VECTORS) != 0u;
    if (mv) {
        // Motion vectors (rg, 0.5 = none) warp frame a forward and frame b backward.
        float mvp = float((e.modes.y >> 8) & 255u);
        vec2 ma = texture(vfx_atlas, vec3(v_mv_a, mvp)).rg * 2.0 - 1.0;
        vec2 mb = texture(vfx_atlas, vec3(v_mv_b, mvp)).rg * 2.0 - 1.0;
        vec2 cell = (e.tex_uv.zw - e.tex_uv.xy) / vec2(max(e.modes.z & 255u, 1u), max((e.modes.z >> 8) & 255u, 1u));
        float strength = 0.15;
        ua -= ma * cell * strength * blend;
        ub += mb * cell * strength * (1.0 - blend);
    }
    float page = v_uv_a.z;
    vec4 a = texture(vfx_atlas, vec3(ua, page));
    vec4 b = texture(vfx_atlas, vec3(ub, page));
    lm0 = vec4(0);
    lm1 = vec4(0);
    if (v_info.z == LT_SIXWAY) {
        float lp = float((e.modes.y >> 16) & 255u);
        vec2 off0 = e.lm0_uv.xy - e.tex_uv.xy, off1 = e.lm1_uv.xy - e.tex_uv.xy;
        lm0 = mix(texture(vfx_atlas, vec3(ua + off0, lp)), texture(vfx_atlas, vec3(ub + off0, lp)), blend);
        lm1 = mix(texture(vfx_atlas, vec3(ua + off1, lp)), texture(vfx_atlas, vec3(ub + off1, lp)), blend);
    }
    bool flipbook = ((e.modes.x >> 16) & (EF_FRAME_OVER_LIFE)) != 0u || e.render1.w > 0.0;
    return flipbook ? mix(a, b, blend) : a;
}

void main() {
    Emitter e = emitters[v_info.x];
    // Soft particles + near fade.
    vec2 suv = gl_FragCoord.xy * vf.screen.zw;
    float fade = 1.0;
    if (v_misc.z > 0.0 && pc.pad0 == 0u) {   // the soft fade reads the main view's depth (none in the scope picture)
        float scene = lin_depth(texture(vfx_depth, suv).r);
        fade *= clamp((scene - v_misc.y) / v_misc.z, 0.0, 1.0);
    }
    if (v_misc.w > 0.0) fade *= clamp((v_misc.y - 0.05) / v_misc.w, 0.0, 1.0);
    vec4 lm0, lm1;
    vec4 tex = e.tex_uv.z > e.tex_uv.x ? sample_flip(e, lm0, lm1) : vec4(vec3(1.0), 1.0);
    if (e.tex_uv.z <= e.tex_uv.x) {
        // No texture: soft round sprite.
        float r = length(v_local);
        float a = clamp(1.0 - r, 0.0, 1.0);
        a = a * a * (3.0 - 2.0 * a);
        tex = vec4(a, a, a, a);
        lm0 = lm1 = vec4(a);
    }
    uint mode = v_info.w;
    float emissive = e.render0.w;
    // Atlas colour textures are sRGB encoded (premultiplied): linearize the colour (approx. 2.2).
    if (mode != RM_DISTORT && e.tex_uv.z > e.tex_uv.x) {
        vec3 straight = tex.a > 1e-4 ? tex.rgb / tex.a : vec3(0);
        tex.rgb = pow(straight, vec3(2.2)) * tex.a;
    }
    if (mode == RM_DISTORT) {
        vec2 n = tex.rg * 2.0 - 1.0;
        float a = tex.a * v_color.a * fade;
        vec2 off = n * e.render1.y * a;
        vec3 scene = texture(vfx_scene, suv + off).rgb;
        out_color = vec4(scene, a);
        return;
    }
    if (mode == RM_ADD) {
        float a = v_color.a * fade;
        // Additive light is attenuated by the fog in front of it (adds no in-scattering).
        out_color = vec4(tex.rgb * v_color.rgb * emissive * a * particle_fog(suv).a, 0.0);
        return;
    }
    // Alpha blended (premultiplied).
    float alpha = tex.a * v_color.a * fade;
    if (alpha < 0.002) discard;
    vec3 albedo = tex.a > 1e-3 ? tex.rgb / tex.a : vec3(0);
    vec3 light;
    if (v_info.z == LT_SIXWAY) {
        // lm0: right, left, top; lm1: bottom, back (away from the camera), front.
        light = v_l0 * lm0.r + v_l1 * lm0.g + v_l2 * lm0.b + v_l3 * lm1.r + v_l4 * lm1.g + v_l5 * lm1.b;
        light += v_amb * (0.6 + 0.4 * lm0.b);
    } else if (v_info.z == LT_SIMPLE) {
        // Volumetric-ish: half of the light from any side, the rest from the front.
        light = (v_l0 + v_l1 + v_l2 + v_l3 + v_l4 + v_l5) * 0.35 + v_l5 * 0.3 + v_amb;
    } else {
        light = vec3(1.0);
    }
    // Thin, backlit parts of the volume glow (forward scattering): blood mist against the sun.
    if (e.render1.y > 0.0) light += v_l4 * e.render1.y * (1.0 - clamp(alpha, 0.0, 1.0) * 0.6) * 1.6;
    vec3 c = albedo * v_color.rgb * light + albedo * v_color.rgb * emissive;
    vec4 fog = particle_fog(suv);
    c = c * fog.a + fog.rgb;
    out_color = vec4(c * alpha, alpha);
}
