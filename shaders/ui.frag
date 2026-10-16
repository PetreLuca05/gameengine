#version 450
// UI (src/ui/gui/gui_draw.c). The uv range selects the mode, so shapes, text and shadows batch
// into one draw per texture / clip:
//   uv.x in [0, 1]       textured (white pixel for flat shapes, 9-slice panels, icons, item views)
//   uv.x in [2, 3]       SDF text / shapes (distance in alpha, 0.5 = edge), anti-aliased per pixel
//   uv.x in [4, 5]       soft SDF (text shadow / glow): a wide edge
//   uv.x < -0.5          the blurred scene behind the UI, sampled in screen space (render_ui_backdrop)
// Derivatives are taken in uniform control flow, from the uv (smooth), never from the sampled
// distance: fwidth of the distance across a thin stem is meaningless and dropped strokes.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform Push { vec4 screen; } pc;
layout(location = 0) out vec4 out_color;

const float SDF_SLOPE = 128.0 / 6.0 / 255.0;   // distance units per atlas texel (FONT_PAD 6)

void main() {
    float mode = v_uv.x < -0.5 ? -1.0 : (v_uv.x > 3.5 ? 2.0 : (v_uv.x > 1.5 ? 1.0 : 0.0));
    vec2 uv = v_uv - vec2(mode > 1.5 ? 4.0 : (mode > 0.5 ? 2.0 : 0.0), 0.0);
    vec2 texels = fwidth(uv) * vec2(textureSize(tex, 0));        // screen pixel footprint in texels
    float px = max(max(texels.x, texels.y), 1e-3) * SDF_SLOPE;    // distance change per screen pixel
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    if (mode < -0.5) {
        vec2 suv = gl_FragCoord.xy * pc.screen.zw;
        out_color = vec4(texture(tex, suv).rgb * v_color.rgb, v_color.a);
        return;
    }
    if (mode > 0.5) {
        bool soft = mode > 1.5;
        float w = soft ? 0.22 : clamp(px * 0.7, 0.004, 0.25);
        // Small text a touch heavier (the blend is in display space, thin stems wash out).
        float edge = soft ? 0.5 : 0.5 - clamp(px * 0.3, 0.0, 0.05);
        float a;
        if (soft || px < 0.05) {
            a = smoothstep(edge - w, edge + w, texture(tex, uv).a);
        } else {
            // Minified (small text, no mips): 4 rotated-grid samples over the pixel footprint.
            a = smoothstep(edge - w, edge + w, texture(tex, uv + dx * 0.125 + dy * 0.375).a);
            a += smoothstep(edge - w, edge + w, texture(tex, uv - dx * 0.375 + dy * 0.125).a);
            a += smoothstep(edge - w, edge + w, texture(tex, uv + dx * 0.375 - dy * 0.125).a);
            a += smoothstep(edge - w, edge + w, texture(tex, uv - dx * 0.125 - dy * 0.375).a);
            a *= 0.25;
        }
        out_color = vec4(v_color.rgb, v_color.a * a);
        return;
    }
    out_color = v_color * texture(tex, uv);
}
