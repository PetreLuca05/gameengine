#version 450
// Bloom upsample (3x3 tent), additively blended into the next larger mip.
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push { vec4 texel; vec4 params; } pc;   // params.x radius
layout(location = 0) out vec4 out_color;
vec3 tap(vec2 o) { return texture(src, v_uv + o * pc.texel.xy * pc.params.x).rgb; }
void main() {
    vec3 c = tap(vec2(0)) * 4.0;
    c += (tap(vec2(-1, 0)) + tap(vec2(1, 0)) + tap(vec2(0, -1)) + tap(vec2(0, 1))) * 2.0;
    c += tap(vec2(-1, -1)) + tap(vec2(1, -1)) + tap(vec2(-1, 1)) + tap(vec2(1, 1));
    out_color = vec4(c / 16.0, 1.0);
}
