#version 450
// UI item views (src/render/render_ui.c): inventory preview, icon bakes. Unit meshes scaled per part.
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;        // linear albedo, a = emissive
    vec4 r0, r1, r2;   // view-space rotation columns; r0.w highlight, r1.w metallic
} pc;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_pos;
void main() {
    v_normal = mat3(pc.r0.xyz, pc.r1.xyz, pc.r2.xyz) * in_normal;
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    v_pos = gl_Position.xyz / gl_Position.w;
}
