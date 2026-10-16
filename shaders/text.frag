#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(set = 1, binding = 0) uniform sampler2D atlas;
layout(location = 0) out vec4 out_color;

void main() {
    float a = texture(atlas, v_uv).a * v_color.a;
    if (a < 0.02) discard;
    out_color = vec4(v_color.rgb, a);
}
