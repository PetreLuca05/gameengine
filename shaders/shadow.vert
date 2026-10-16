#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 in_pos;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
} pc;

void main() {
    gl_Position = (pc.flags.w == 3u ? frame.occl_vp : frame.shadow_vps[min(pc.flags.w, 2u)]) * pc.model * vec4(in_pos, 1.0);
}
