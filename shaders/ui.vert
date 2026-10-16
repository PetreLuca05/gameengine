#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(push_constant) uniform Push { vec4 screen; } pc; // xy = 2 / size, zw = 1 / size
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = vec4(in_pos * pc.screen.xy - 1.0, 0.0, 1.0);
}
