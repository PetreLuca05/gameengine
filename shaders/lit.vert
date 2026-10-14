#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;   // roughness, metallic, emissive, tex_scale
    uvec4 flags;   // x = material flags
} pc;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_color;

void main() {
    vec4 world = pc.model * vec4(in_pos, 1.0);
    mat3 nmat = transpose(inverse(mat3(pc.model)));
    v_world = world.xyz;
    v_normal = normalize(nmat * in_normal);
    v_uv = in_uv;
    v_color = in_color;
    if ((pc.flags.x & MAT_VIEWMODEL) != 0u)
        gl_Position = frame.vm_proj * frame.view * world;
    else
        gl_Position = frame.view_proj * world;
}
