#version 450
// shadow.vert for alpha-masked casters (glTF alphaMode MASK, PIPE_SHADOW_MASK): passes the uv on.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 2) in vec2 in_uv;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
    uvec4 pbr;
} pc;

layout(location = 2) out vec2 v_uv;

void main() {
    gl_Position = shadow_pass_vp(pc.flags.w) * pc.model * vec4(in_pos, 1.0);
    v_uv = in_uv * max(pc.params.w, 1e-3);
}
