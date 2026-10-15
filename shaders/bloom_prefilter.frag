#version 450
// Bloom prefilter: soft-knee threshold + 13-tap downsample of the HDR scene into bloom mip 0.
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push { vec4 texel_threshold; vec4 params; } pc;   // xy texel, z threshold, w knee
layout(location = 0) out vec4 out_color;
vec3 tap(vec2 o) { return texture(src, v_uv + o * pc.texel_threshold.xy).rgb; }
void main() {
    vec3 c = tap(vec2(0)) * 0.125;
    c += (tap(vec2(-1, -1)) + tap(vec2(1, -1)) + tap(vec2(-1, 1)) + tap(vec2(1, 1))) * 0.125;
    c += (tap(vec2(-2, -2)) + tap(vec2(2, -2)) + tap(vec2(-2, 2)) + tap(vec2(2, 2))) * 0.03125;
    c += (tap(vec2(0, -2)) + tap(vec2(0, 2)) + tap(vec2(-2, 0)) + tap(vec2(2, 0))) * 0.0625;
    c = min(c, vec3(64.0));   // fireflies
    float br = max(c.r, max(c.g, c.b));
    float t = pc.texel_threshold.z, k = max(pc.texel_threshold.w, 1e-4);
    float soft = clamp(br - t + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k);
    float w = max(soft, br - t) / max(br, 1e-4);
    out_color = vec4(c * w, 1.0);
}
