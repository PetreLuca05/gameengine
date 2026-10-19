#version 450
// Studio lighting for item views, independent of the world: key (top left front), cool fill,
// warm rim from behind, hemispheric ambient. Output sRGB-encoded (the UI composites encoded values).
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_pos;
layout(location = 2) in vec4 v_color;   // item meshes: layered albedo, a = baked AO
layout(location = 3) in vec2 v_uv;      // item meshes: (roughness, metallic)
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
    bool vpbr = pc.r2.w > 0.5;   // item mesh (docs/ITEM_PIPELINE.md): per-vertex material
    vec3 albedo = max(pc.color.rgb * (vpbr ? v_color.rgb * v_color.rgb : vec3(1.0)), vec3(0.004));   // vertex rgb = sqrt(albedo)
    float metal = vpbr ? v_uv.y : pc.r1.w;
    float rough = vpbr ? v_uv.x : 0.5;
    float ao = vpbr ? v_color.a : 1.0;
    float k = max(dot(n, key), 0.0), f = max(dot(n, fill), 0.0);
    float r = pow(1.0 - max(dot(n, v), 0.0), 2.5) * max(dot(n, rim) + 0.4, 0.0);
    float hemi = 0.5 + 0.5 * n.y;
    // Metals: the albedo is the specular colour, little diffuse.
    vec3 diff = albedo * (1.0 - metal * 0.85);
    vec3 c = diff * (vec3(1.0, 0.97, 0.92) * k * 2.2 + vec3(0.55, 0.62, 0.75) * f * 0.6 + mix(vec3(0.10, 0.09, 0.08), vec3(0.22, 0.24, 0.28), hemi) * ao);
    vec3 h = normalize(key + v);
    float gloss = vpbr ? mix(160.0, 8.0, sqrt(rough)) : mix(24.0, 64.0, metal);
    vec3 spec_col = vpbr ? mix(vec3(0.04), albedo, metal) * 4.0 * (1.0 - rough * 0.7) : vec3(mix(0.18, 0.6, metal));
    vec3 spec = pow(max(dot(n, h), 0.0), gloss) * spec_col * mix(0.6, 1.0, ao);
    c += spec + vec3(1.0, 0.85, 0.65) * r * 0.35 * ao;
    c += albedo * pc.color.a * 2.0;               // emissive parts (lenses, dots)
    c = mix(c, c + vec3(0.9, 0.55, 0.15) * 0.45, pc.r0.w);   // highlighted socket / part
    c = c / (1.0 + c * 0.35);                     // gentle shoulder
    out_color = vec4(srgb_encode(clamp(c, 0.0, 1.0)), 1.0);
}
