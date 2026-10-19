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
layout(location = 9) flat out vec4 v_recon;
layout(location = 10) flat out uint v_normal_scale;   // float bits (PBR maps, shaders/pbr_maps.glsl)

invariant gl_Position;
out float gl_ClipDistance[1];

// The arms layer's shoulder seam (docs/CAMERA_HOLDPOINT.md "Rule 3"): 0 at an upper-arm joint (drawn
// where the world camera sees it, so the arm meets the body) .. 1 past r1 (the arms camera).
float arms_seam_w(vec3 p, vec4 s0, vec4 s1) {
    if (frame.arms_seam.z < 0.5) return 1.0;
    float d = min(distance(p, s0.xyz), distance(p, s1.xyz));
    return smoothstep(frame.arms_seam.x, frame.arms_seam.y, d);
}
// The screen position blended from the world camera's clip `a` to the arms camera's `b`; the depth
// stays the arms layer's (its own depth buffer). Behind the world camera: the arms camera alone.
vec4 arms_seam_mix(vec4 a, vec4 b, float w) {
    if (w >= 1.0 || a.w < 0.02 || b.w < 0.002) return b;
    vec2 n = mix(a.xy / a.w, b.xy / b.w, w);
    return vec4(n * b.w, b.z, b.w);
}

void main() {
    SkinInstance si = skin_inst.inst[gl_InstanceIndex];
    // Palettes are bone_world * inverse_bind: the skinned vertex is already in world space.
    mat4 m = skin_matrix(si.info.x, in_joints, in_weights);
    vec4 prev_world;
    if ((si.info.z & SKIN_ROOTED) != 0u) {
        // A shared model-space pose (PF-0034) placed by this instance's root; last frame's root for
        // the velocity (the pose's own motion is not in it).
        prev_world = skin_root(si.prev_root[0], si.prev_root[1], si.prev_root[2]) * (m * vec4(in_pos, 1.0));
        m = skin_root(si.root[0], si.root[1], si.root[2]) * m;
    }
    vec4 world = m * vec4(in_pos, 1.0);
    // Previous frame's palette (velocity of skinned characters): info.z = its first bone + 1.
    if ((si.info.z & SKIN_ROOTED) == 0u) prev_world = si.info.z != 0u ? skin_matrix(si.info.z - 1u, in_joints, in_weights) * vec4(in_pos, 1.0) : world;
    v_world = world.xyz;
    v_normal = normalize(mat3(m) * in_normal);
    v_uv = in_uv;
    v_color = in_color;
    v_mat_color = si.color;
    v_mat_params = si.params;
    v_mat_flags = si.info.y;
    v_recon = si.recon;
    v_normal_scale = si.info.w;
    // First-person arms from the body model are drawn in the viewmodel pass (own projection).
    if ((si.info.y & MAT_VIEWMODEL) != 0u) {
        gl_Position = frame.vm_proj * frame.view * world;
        v_cur_clip = frame.vm_view_proj_nj * world;
        v_prev_clip = frame.prev_vm_view_proj_nj * prev_world;
        // The arms are cut closer than arms_seam.w (m, depth along the arms camera) to their camera:
        // the upper arms reach the shoulders, which a camera behind the eye (looking up: under it) would
        // otherwise see from inside. The item and the depth are untouched.
        gl_ClipDistance[0] = frame.arms_seam.w > 0.0 ? gl_Position.w - frame.arms_seam.w : 1.0;
        float w = arms_seam_w(world.xyz, frame.arms_shoulder[0], frame.arms_shoulder[1]);
        if (w < 1.0) {
            gl_Position = arms_seam_mix(frame.view_proj * world, gl_Position, w);
            v_cur_clip = arms_seam_mix(frame.view_proj_nj * world, v_cur_clip, w);
            v_prev_clip = arms_seam_mix(frame.prev_view_proj_nj * prev_world, v_prev_clip,
                                        arms_seam_w(prev_world.xyz, frame.arms_shoulder_prev[0], frame.arms_shoulder_prev[1]));
        }
    } else {
        gl_Position = frame.view_proj * world;
        v_cur_clip = frame.view_proj_nj * world;
        v_prev_clip = frame.prev_view_proj_nj * prev_world;
        gl_ClipDistance[0] = 1.0;
    }
}
