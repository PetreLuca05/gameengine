#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "lod_fade.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;   // roughness, metallic, emissive, tex_scale
    uvec4 flags;   // x = material flags, y = previous model index + 1 (0 = static)
} pc;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_color;
layout(location = 7) out vec4 v_cur_clip;    // unjittered (velocity)
layout(location = 8) out vec4 v_prev_clip;

// The prepass (gbuffer.frag) and the lit pass must produce bit-identical depth.
invariant gl_Position;

void main() {
    vec4 world = pc.model * vec4(in_pos, 1.0);
    mat3 nmat = transpose(inverse(mat3(pc.model)));
    v_world = world.xyz;
    v_normal = normalize(nmat * in_normal);
    v_uv = in_uv;
    if ((pc.flags.x & MAT_TERRAIN_MORPH) != 0u) {   // terrain geomorph (docs/LOD.md): uv carries the morph target
        lod_terrain_vertex(world.xyz, v_normal, in_uv, pc.params.w);
        v_world = world.xyz;
        v_uv = in_pos.xz;
    }
    v_color = in_color;
    vec4 prev_world = pc.flags.y != 0u ? draw_prev.prev_models[pc.flags.y - 1u] * vec4(in_pos, 1.0) : world;
    if ((pc.flags.x & MAT_VIEWMODEL) != 0u) {
        gl_Position = frame.vm_proj * frame.view * world;
        v_cur_clip = frame.vm_view_proj_nj * world;
        v_prev_clip = frame.prev_vm_view_proj_nj * prev_world;
    } else {
        gl_Position = frame.view_proj * world;
        v_cur_clip = frame.view_proj_nj * world;
        v_prev_clip = frame.prev_view_proj_nj * prev_world;
    }
}
