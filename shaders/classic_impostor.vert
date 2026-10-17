#version 450
// Classic impostor quads (src/render/vk_classic.c): 6 vertices per instance, facing the camera at
// the impostor sphere centre, half size = impostor radius x scale.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "classic_common.glsl"
layout(location = 4) in vec4 i_pos;
layout(location = 5) in vec4 i_rot;
layout(location = 6) in vec4 i_misc;
layout(location = 0) out vec3 o_world;
layout(location = 1) flat out vec4 o_rot;
layout(location = 2) flat out vec4 o_ctr;
layout(location = 3) flat out float o_fade;
invariant gl_Position;

void main() {
    const uint corner[6] = uint[6](0u, 1u, 2u, 0u, 2u, 3u);
    uint k = corner[gl_VertexIndex % 6];
    vec3 c = i_pos.xyz + cl_qrot(i_rot, pc.model[0].xyz * i_pos.w);
    float h = pc.model[0].w * i_pos.w;
    vec3 f = frame.camera_pos.xyz - c;
    f = dot(f, f) > 1e-12 ? normalize(f) : vec3(0.0, 0.0, 1.0);
    vec3 up = abs(f.y) > 0.999 ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
    vec3 r = normalize(cross(up, f)), u = cross(f, r);
    float sr = (k == 1u || k == 2u) ? 1.0 : -1.0, su = k >= 2u ? 1.0 : -1.0;
    vec3 w = c + (r * sr + u * su) * h;
    o_world = w;
    o_rot = i_rot;
    o_ctr = vec4(c, h);
    o_fade = i_misc.y;
    gl_Position = frame.view_proj * vec4(w, 1.0);
}
