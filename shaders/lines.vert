#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 v_color;

void main() {
    v_color = vec4(srgb_to_linear(in_color.rgb) * 1.5, in_color.a);
    gl_Position = frame.view_proj * vec4(in_pos, 1.0);
}
