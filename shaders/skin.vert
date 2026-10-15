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
layout(location = 7) out vec4 v_cur_clip;
layout(location = 8) out vec4 v_prev_clip;

invariant gl_Position;

void main() {
    SkinInstance si = skin_inst.inst[gl_InstanceIndex];
    // Palettes are bone_world * inverse_bind: the skinned vertex is already in world space.
    mat4 m = skin_matrix(si.info.x, in_joints, in_weights);
    vec4 world = m * vec4(in_pos, 1.0);
    // Previous frame's palette (velocity of skinned characters): info.z = its first bone + 1.
    vec4 prev_world = si.info.z != 0u ? skin_matrix(si.info.z - 1u, in_joints, in_weights) * vec4(in_pos, 1.0) : world;
    v_world = world.xyz;
    v_normal = normalize(mat3(m) * in_normal);
    v_uv = in_uv;
    v_color = in_color;
    v_mat_color = si.color;
    v_mat_params = si.params;
    v_mat_flags = si.info.y;
    // First-person arms from the body model are drawn in the viewmodel pass (own projection).
    if ((si.info.y & MAT_VIEWMODEL) != 0u) {
        gl_Position = frame.vm_proj * frame.view * world;
        v_cur_clip = frame.vm_view_proj_nj * world;
        v_prev_clip = frame.prev_vm_view_proj_nj * prev_world;
    } else {
        gl_Position = frame.view_proj * world;
        v_cur_clip = frame.view_proj_nj * world;
        v_prev_clip = frame.prev_view_proj_nj * prev_world;
    }
}
