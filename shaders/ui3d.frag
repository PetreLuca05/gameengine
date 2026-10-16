#version 450
// Studio lighting for item views, independent of the world: key (top left front), cool fill,
// warm rim from behind, hemispheric ambient. Output sRGB-encoded (the UI composites encoded values).
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_pos;
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 r0, r1, r2;
} pc;
layout(location = 0) out vec4 out_color;
vec3 srgb_encode(vec3 c) { return mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }
void main() {
    vec3 n = normalize(v_normal);
    vec3 v = vec3(0.0, 0.0, 1.0);                 // view space: the camera looks down -Z
    vec3 key = normalize(vec3(-0.45, 0.75, 0.55));
    vec3 fill = normalize(vec3(0.7, 0.1, 0.5));
    vec3 rim = normalize(vec3(0.3, 0.35, -0.9));
    vec3 albedo = max(pc.color.rgb, vec3(0.02));
    float metal = pc.r1.w;
    float k = max(dot(n, key), 0.0), f = max(dot(n, fill), 0.0);
    float r = pow(1.0 - max(dot(n, v), 0.0), 2.5) * max(dot(n, rim) + 0.4, 0.0);
    float hemi = 0.5 + 0.5 * n.y;
    vec3 c = albedo * (vec3(1.0, 0.97, 0.92) * k * 2.2 + vec3(0.55, 0.62, 0.75) * f * 0.6 + mix(vec3(0.10, 0.09, 0.08), vec3(0.22, 0.24, 0.28), hemi));
    vec3 h = normalize(key + v);
    float spec = pow(max(dot(n, h), 0.0), mix(24.0, 64.0, metal)) * mix(0.18, 0.6, metal);
    c += vec3(spec) + vec3(1.0, 0.85, 0.65) * r * 0.35;
    c += albedo * pc.color.a * 2.0;               // emissive parts (lenses, dots)
    c = mix(c, c + vec3(0.9, 0.55, 0.15) * 0.45, pc.r0.w);   // highlighted socket / part
    c = c / (1.0 + c * 0.35);                     // gentle shoulder
    out_color = vec4(srgb_encode(clamp(c, 0.0, 1.0)), 1.0);
}
