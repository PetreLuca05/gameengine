#version 450
// Particle quads, vertex-pulled. pc.mode: 0 additive list, 1 sorted alpha list, 2 distortion list,
// 3 CPU sprites (host buffer). Orientation: billboard, velocity stretch, cross (billboard + quad
// containing the axis), axis (quad around the axis), normal (facing the spawn normal), segment
// (ribbon piece pos -> pos + vel). Lighting (sun + shadow, point lights, ambient) is evaluated per
// particle here; the fragment shader only combines it with the 6-way / albedo maps.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define VFX_SET 1
#include "vfx_common.glsl"

layout(push_constant) uniform Push { uint mode; uint pad0, pad1, pad2; } pc;

layout(location = 0) out vec3 v_uv_a;       // xy uv, z page
layout(location = 1) out vec3 v_uv_b;
layout(location = 2) out vec4 v_color;      // linear rgb, alpha
layout(location = 3) flat out uvec4 v_info; // x emitter, y flags, z lighting, w render mode
layout(location = 4) out vec4 v_misc;       // x frame blend, y view depth, z soft, w near fade
layout(location = 5) out vec3 v_l0;         // 6-way coefficients: +right, -right, +up, -up, away, toward
layout(location = 6) out vec3 v_l1;
layout(location = 7) out vec3 v_l2;
layout(location = 8) out vec3 v_l3;
layout(location = 9) out vec3 v_l4;
layout(location = 10) out vec3 v_l5;
layout(location = 11) out vec3 v_amb;
layout(location = 12) out vec2 v_local;     // quad coordinates (-1..1)
layout(location = 13) out vec2 v_mv_a;      // motion vector uv (frame a) / page in z of uv_b
layout(location = 14) out vec2 v_mv_b;
layout(location = 15) out vec4 v_fog;         // xyz world position, w 1 = fogged (not viewmodel space)

float shadow_at(vec3 wp) {
    if (frame.sun_color.w < 0.5) return 1.0;
    vec4 sc = frame.shadow_vp * vec4(wp, 1.0);
    vec3 s = sc.xyz / sc.w;
    vec2 uv = s.xy * 0.5 + 0.5;
    // shadow_vp is the last cascade: its own atlas tile only (no samples of the neighbouring tiles)
    int c = clamp(int(frame.shadow_cfg3.x) - 1, 0, 3);
    vec2 lo = vec2(c & 1, c >> 1) * 0.5;
    if (any(lessThan(uv, lo)) || any(greaterThan(uv, lo + 0.5))) return 1.0;
    return mix(1.0, texture(shadow_map, vec3(uv, s.z - 4.0 * frame.shadow_bias.w)), frame.ssky0.y);   // the sky cutoff fade (docs/SHADOWS.md)
}

void accumulate(vec3 L, vec3 col, vec3 R, vec3 U, vec3 F) {
    // F points from the particle towards the camera.
    v_l0 += col * max(dot(L, R), 0.0);
    v_l1 += col * max(dot(L, -R), 0.0);
    v_l2 += col * max(dot(L, U), 0.0);
    v_l3 += col * max(dot(L, -U), 0.0);
    v_l4 += col * max(dot(L, -F), 0.0);
    v_l5 += col * max(dot(L, F), 0.0);
}

void main() {
    uint vid = gl_VertexIndex;
    uint inst = gl_InstanceIndex;
    vec3 pos, vel;
    float size, rot, frame_f, age = 0.0, life = 1.0;
    vec4 col;
    uint emitter, flags = 0u, second = 0u;
    if (pc.mode == 3u) {
        CpuSprite s = cpu_sprites[inst];
        pos = s.pos_size.xyz;
        size = s.pos_size.w;
        vel = s.vel_rot.xyz;
        rot = s.vel_rot.w;
        col = s.color;
        emitter = s.info.x;
        frame_f = uintBitsToFloat(s.info.y);
        flags = s.info.z;
        age = 0.5;
        life = 1.0;
        if ((flags & 0x100u) != 0u) second = 1u;
    } else {
        uint idx;
        if (pc.mode == 0u) idx = list_add[inst];
        else if (pc.mode == 1u) idx = sort_keys[inst].y;
        else idx = list_distort[inst];
        second = idx >> 31;
        idx &= 0x7FFFFFFFu;
        vec4 pa = p_pos_age[idx];
        vec4 vl = p_vel_life[idx];
        vec4 misc = p_misc[idx];
        uvec4 info = p_info[idx];
        emitter = info.x & 0xFFFFu;
        flags = info.x >> 16;
        uint ins = info.w & 0xFFFFu;
        pos = pa.xyz;
        vel = vl.xyz;
        if (ins != 0xFFFFu) {
            vec3 ip = inst_xf[ins * 2u].xyz;
            vec4 iq = inst_xf[ins * 2u + 1u];
            pos = ip + quat_rot(iq, pos);
            vel = quat_rot(iq, vel);
        }
        age = pa.w;
        life = vl.w;
        size = misc.x;
        rot = misc.y;
        frame_f = misc.w;
        col = unpack_rgba8(info.z);
        col.rgb = col.rgb * col.rgb;   // stored in gamma-ish space for precision
    }
    Emitter e = emitters[emitter];
    float tl = clamp(age / max(life, 1e-4), 0.0, 1.0);
    uint row = e.modes.w;
    if (pc.mode != 3u) {
        size *= lut(row, LUT_SIZE, tl).x;
        col *= lut(row, LUT_COLOR, tl);
    }
    uint orient = (e.modes.x >> 4) & 15u;
    float aspect = e.render1.z > 0.0 ? e.render1.z : 1.0;
    vec3 R = vf.cam_right.xyz, U = vf.cam_up.xyz;
    vec3 to_cam = normalize(vf.cam_pos.xyz - pos);
    float speed = length(vel);
    vec3 axis = speed > 1e-5 ? vel / speed : U;
    vec2 corner = vec2(vid == 1u || vid == 2u || vid == 4u ? 1.0 : -1.0, vid == 2u || vid == 4u || vid == 5u ? 1.0 : -1.0);
    vec3 right, up;
    float half_w = size * 0.5 * aspect, half_h = size * 0.5;
    if (orient == OR_STRETCH) {
        vec3 side = cross(axis, to_cam);
        float sl = length(side);
        right = sl > 1e-4 ? side / sl : R;
        up = axis;
        float len = clamp(1.0 + speed * e.render0.x / max(size, 1e-4), 1.0, e.render0.y);
        half_h *= len;
        // Anchor the tail at the particle so sparks streak backwards from where they are.
        pos -= axis * (half_h - size * 0.5);
    } else if (orient == OR_SEGMENT) {
        vec3 a = pos, b = pos + vel;
        axis = normalize(b - a + vec3(1e-6));
        vec3 side = cross(axis, to_cam);
        right = normalize(side + vec3(1e-6, 0, 0));
        up = axis;
        pos = (a + b) * 0.5;
        half_h = length(b - a) * 0.5;
    } else if (orient == OR_AXIS || (orient == OR_CROSS && second == 1u)) {
        // Quad containing the axis, turned towards the camera around it, rotated by the particle rotation.
        vec3 side = cross(axis, to_cam);
        float sl = length(side);
        side = sl > 1e-4 ? side / sl : R;
        vec3 n2 = cross(side, axis);
        float c = cos(rot), s = sin(rot);
        right = side * c + n2 * s * 0.0;   // keep facing (rotation as roll is meaningless here)
        up = axis;
        // Cross flashes: the axis quad is longer than wide (forward flame).
        half_h = size * 0.5 * (orient == OR_CROSS ? 1.6 : 1.0);
        pos += axis * half_h * (orient == OR_CROSS ? 0.8 : 0.0);
    } else if (orient == OR_NORMAL) {
        vec3 n = axis;
        vec3 t, b;
        basis(n, t, b);
        float c = cos(rot), s = sin(rot);
        right = t * c + b * s;
        up = -t * s + b * c;
    } else {
        float c = cos(rot), s = sin(rot);
        right = R * c + U * s;
        up = -R * s + U * c;
    }
    vec3 wp = pos + right * corner.x * half_w + up * corner.y * half_h;
    bool vm = (flags & SF_VIEWMODEL) != 0u;
    gl_Position = frame.view_proj * vec4(wp, 1.0);
    // Scope picture-in-picture (pc.pad0 = 1, the scope camera's frame set): no viewmodel-space
    // particles (the shooter's own muzzle effects live in the eye's viewmodel space).
    if (pc.pad0 == 1u && vm) gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    // Flipbook.
    vec2 uv = corner * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    uint frames = max((e.modes.z >> 16) & 0xFFFFu, 1u);
    float fa = floor(frame_f);
    float fb = mod(fa + 1.0, float(frames));
    v_uv_a = vec3(cell_uv(e.tex_uv, e.modes.z, fa, uv), float(e.modes.y & 255u));
    v_uv_b = vec3(cell_uv(e.tex_uv, e.modes.z, fb, uv), float(e.modes.y & 255u));
    v_mv_a = cell_uv(e.mv_uv, e.modes.z, fa, uv);
    v_mv_b = cell_uv(e.mv_uv, e.modes.z, fb, uv);
    v_local = corner;
    float exposure = ((e.modes.x >> 16) & EF_EXPOSURE) != 0u ? vf.wind.w : 1.0;
    col.a *= exposure;
    v_color = col;
    uint lighting = (e.modes.x >> 8) & 15u;
    if ((flags & SF_NO_LIGHTING) != 0u || (vf.caps.w & 1u) == 0u) lighting = min(lighting, LT_SIMPLE);
    v_info = uvec4(emitter, flags, lighting, pc.mode == 3u ? (e.modes.x & 15u) : pc.mode);
    float view_depth = dot(wp - vf.cam_pos.xyz, vf.cam_fwd.xyz);
    v_misc = vec4(fract(frame_f), view_depth, e.render0.z, vm ? 0.0 : e.render1.x);
    v_fog = vec4(wp, vm ? 0.0 : 1.0);
    // Lighting (per particle).
    v_l0 = v_l1 = v_l2 = v_l3 = v_l4 = v_l5 = vec3(0);
    v_amb = vec3(0);
    if (lighting != LT_NONE) {
        vec3 F = to_cam;
        // Same units as lit_body.glsl: Lambert 1/pi on direct light, identical point light falloff.
        const float INV_PI = 0.31830989;
        vec3 sun = frame.sun_color.rgb * frame.sun_dir.w * shadow_at(pos) * INV_PI;
        accumulate(normalize(frame.sun_dir.xyz), sun, right, up, F);
        uint nl = uint(frame.ground_ambient.w);
        for (uint i = 0u; i < nl; i++) {
            vec3 lp = frame.lights[i].pos_radius.xyz;
            float r = frame.lights[i].pos_radius.w;
            vec3 d = lp - pos;
            float dist = length(d);
            if (dist > r) continue;
            float att = clamp(1.0 - pow(dist / r, 4.0), 0.0, 1.0);
            att = att * att / (dist * dist + 1.0);
            accumulate(d / max(dist, 1e-3), frame.lights[i].color_intensity.rgb * frame.lights[i].color_intensity.w * att * INV_PI, right, up, F);
        }
        v_amb = frame.sky_ambient.rgb * frame.sky_ambient.w * 0.7 + frame.ground_ambient.rgb * frame.sky_ambient.w * 0.3;
    }
}
