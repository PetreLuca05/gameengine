#version 450
// Bloom downsample (13-tap, Jimenez 2014).
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push { vec4 texel; vec4 params; } pc;
layout(location = 0) out vec4 out_color;
vec3 tap(vec2 o) { return texture(src, v_uv + o * pc.texel.xy).rgb; }
void main() {
    vec3 c = tap(vec2(0)) * 0.125;
    c += (tap(vec2(-1, -1)) + tap(vec2(1, -1)) + tap(vec2(-1, 1)) + tap(vec2(1, 1))) * 0.125;
    c += (tap(vec2(-2, -2)) + tap(vec2(2, -2)) + tap(vec2(-2, 2)) + tap(vec2(2, 2))) * 0.03125;
    c += (tap(vec2(0, -2)) + tap(vec2(0, 2)) + tap(vec2(-2, 0)) + tap(vec2(2, 0))) * 0.0625;
    out_color = vec4(c, 1.0);
}
