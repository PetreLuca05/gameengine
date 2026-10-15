#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;
layout(location = 4) in uvec4 in_joints;
layout(location = 5) in vec4 in_weights;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_color;
layout(location = 4) flat out vec4 v_mat_color;
layout(location = 5) flat out vec4 v_mat_params;
layout(location = 6) flat out uint v_mat_flags;

void main() {
    SkinInstance si = skin_inst.inst[gl_InstanceIndex];
    // Palettes are bone_world * inverse_bind: the skinned vertex is already in world space.
    mat4 m = skin_matrix(si.info.x, in_joints, in_weights);
    vec4 world = m * vec4(in_pos, 1.0);
    v_world = world.xyz;
    v_normal = normalize(mat3(m) * in_normal);
    v_uv = in_uv;
    v_color = in_color;
    v_mat_color = si.color;
    v_mat_params = si.params;
    v_mat_flags = si.info.y;
    gl_Position = frame.view_proj * world;
}
