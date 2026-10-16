#version 450
// Precipitation (docs/WEATHER.md): procedural, no simulation state. Each instance owns a hashed
// spot in a box that follows the camera; it falls (and drifts with the wind) through the box and
// wraps, so the field is continuous and world-anchored. Instances under a roof (the top-down rain
// occlusion map) collapse. pc.flags.x: 0 rain / snow streaks, 1 splashes on the ground.
//   pc.params: x snow fraction (0 rain .. 1 snow), y intensity, z box half size (m), w box height (m)
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "weather.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_kind;   // 0 rain, 1 snow, 2 splash

vec4 hash4(uint n) {
    uvec4 v = uvec4(n) * uvec4(1597334677u, 3812015801u, 2798796415u, 1979697957u);
    v = (v.x ^ v.y ^ v.z ^ v.w) * uvec4(1597334677u, 3812015801u, 2798796415u, 1979697957u);
    return vec4(v) / 4294967295.0;
}

const vec2 k_corner[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

vec3 lighting() {
    // Ambient sky + a little key light + lightning flashes (rain catches the light softly).
    vec3 amb = frame.sky_ambient.rgb * frame.sky_ambient.w * 1.6 + frame.ground_ambient.rgb * frame.sky_ambient.w * 0.4;
    vec3 key = frame.sun_color.rgb * frame.sun_dir.w * 0.12;
    return amb + key + vec3(frame.wx0.w * 3.0);
}

void main() {
    uint inst = uint(gl_InstanceIndex);
    vec2 corner = k_corner[gl_VertexIndex % 6];
    vec3 cam = frame.camera_pos.xyz;
    float t = frame.wx1.z;
    vec2 wind = frame.wx2.xy;
    vec4 h = hash4(inst * 7919u + (pc.flags.x == 1u ? 104729u : 0u));
    gl_Position = vec4(0.0, 0.0, -2.0, 1.0);   // collapsed unless placed below
    v_uv = corner * 0.5 + 0.5;
    if (pc.flags.x == 1u) {
        // Splash: a random spot near the camera per time slot, on the top-most surface.
        float rate = 2.2 + h.w * 1.5;
        float slot = floor(t * rate + h.z * 7.0);
        vec4 h2 = hash4(inst * 2654435761u + uint(slot) * 97u);
        float age = fract(t * rate + h.z * 7.0);
        float r = sqrt(h2.x) * 14.0, a = h2.y * 6.2831853;
        vec3 p = vec3(cam.x + cos(a) * r, 0.0, cam.z + sin(a) * r);
        if (frame.wx1.w < 0.5) return;
        // Height from the occlusion map: its top surface (centre top y - depth x range).
        vec4 c = frame.occl_vp * vec4(p.x, frame.wx3.w, p.z, 1.0);
        vec2 uv = c.xy * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return;
        float d = texture(wx_occl, uv).r;
        if (d >= 0.999) return;   // no surface under it
        p.y = frame.wx3.w - d * frame.wx4.x + 0.01;
        float size = mix(0.03, 0.11, age) * (0.6 + h2.z * 0.8);
        vec3 wp = p + vec3(corner.x, 0.0, corner.y) * size;
        gl_Position = frame.view_proj * vec4(wp, 1.0);
        float fade = (1.0 - age) * smoothstep(14.0, 8.0, r);
        v_color = vec4(lighting() * 0.7, 0.28 * fade * pc.params.y);
        v_kind = 2u;
        v_uv = corner;   // the fragment draws a ring from the local coordinate
        return;
    }
    bool snow = h.w < pc.params.x;
    float half_size = pc.params.z, height = pc.params.w;
    float fall = snow ? (0.9 + h.x * 0.6) : (8.5 + h.y * 2.5);
    vec3 vel = vec3(wind.x * (snow ? 0.8 : 0.35), -fall, wind.y * (snow ? 0.8 : 0.35));
    vec3 box = vec3(half_size * 2.0, height, half_size * 2.0);
    vec3 base = h.xyz * box + vel * t;
    if (snow) base.xz += vec2(sin(t * 0.9 + h.x * 30.0), cos(t * 0.7 + h.z * 30.0)) * 0.35;   // turbulence
    vec3 lo = cam - vec3(half_size, height * 0.45, half_size);
    vec3 p = lo + mod(base - lo, box);
    // Under a roof: hidden.
    if (wx_exposed(p) < 0.5) return;
    vec3 to_cam = cam - p;
    float dist = length(to_cam);
    vec3 axis = normalize(vel);
    vec3 side = normalize(cross(axis, to_cam / max(dist, 1e-3)));
    float len = snow ? 0.022 : clamp(length(vel) * 0.022, 0.15, 0.4);
    float wid = snow ? 0.022 : 0.006 + dist * 0.0006;
    vec3 up = snow ? normalize(cross(to_cam, side)) : axis;
    vec3 wp = p + side * corner.x * wid + up * corner.y * (snow ? wid : len * 0.5);
    gl_Position = frame.view_proj * vec4(wp, 1.0);
    float fade = smoothstep(0.3, 1.2, dist) * (1.0 - smoothstep(half_size * 0.7, half_size, dist));
    v_color = vec4(lighting() * (snow ? 1.2 : 0.8), (snow ? 0.85 : 0.22) * fade);
    v_kind = snow ? 1u : 0u;
}
