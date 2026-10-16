#version 450
// Precipitation fragments: rain streaks (soft across, faded along), snowflakes (soft dots),
// splashes (an expanding ring with a bright edge).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_kind;
layout(location = 0) out vec4 out_color;
void main() {
    float a;
    if (v_kind == 0u) {
        float x = abs(v_uv.x * 2.0 - 1.0);
        a = (1.0 - x * x) * smoothstep(0.0, 0.35, v_uv.y) * smoothstep(1.0, 0.65, v_uv.y);
    } else if (v_kind == 1u) {
        float r = length(v_uv * 2.0 - 1.0);
        a = 1.0 - smoothstep(0.35, 1.0, r);
    } else {
        float r = length(v_uv);
        a = smoothstep(0.55, 0.85, r) * (1.0 - smoothstep(0.85, 1.0, r));
    }
    if (a <= 0.003) discard;
    out_color = vec4(v_color.rgb, v_color.a * a);
}
