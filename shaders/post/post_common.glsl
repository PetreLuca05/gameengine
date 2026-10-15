// Shared declarations of the post-processing shaders (must match src/render/post/post_internal.h).
// Universal push-descriptor set: 0..7 sampled textures, 8..11 storage images, 12 PostUBO,
// 13 / 14 storage buffers. Sampled bindings are declared per shader with the dimension it needs.
#define MAX_POINT_LIGHTS 64

layout(std140, set = 0, binding = 12) uniform PostUBO {
    mat4 view, proj, view_proj, inv_view_proj, inv_proj, view_proj_nj, prev_view_proj_nj, inv_view_proj_nj, shadow_vp;
    vec4 cam_pos;
    vec4 screen;
    vec4 outsz;
    vec4 jitter;
    vec4 depthp;      // near, far, tan(fov/2), frame
    vec4 sun_dir;
    vec4 sun_color;
    vec4 sky;
    vec4 ground;
    vec4 misc;        // dt, camera cut, ads, aspect
    vec4 s_ao, s_ssr, s_gi, s_vol, s_vol2, s_vol3, s_mb, s_dof, s_dof2, s_bloom, s_exp, s_exp2;
    vec4 s_lens, s_lens2, s_tm, s_lift, s_gamma, s_gain, s_out, s_fb, s_fb2, s_fb_tint, s_dbg;
    vec4 lights[MAX_POINT_LIGHTS * 2];
} U;

layout(push_constant) uniform PostPush { vec4 p0, p1, p2, p3; uvec4 u0, u1, u2, u3; } pc;

const float PI = 3.14159265;

float linear_depth(float d) {
    float n = U.depthp.x, f = U.depthp.y;
    return n * f / (f - d * (f - n));
}
// Unjittered view-space position from a depth sample at uv (0..1).
vec3 view_pos(vec2 uv, float d) {
    vec4 c = vec4(uv * 2.0 - 1.0, d, 1.0);
    c.xy -= U.jitter.xy;
    vec4 v = U.inv_proj * c;
    return v.xyz / v.w;
}
vec3 world_pos(vec2 uv, float d) {
    vec4 c = vec4(uv * 2.0 - 1.0, d, 1.0);
    vec4 w = U.inv_view_proj * c;
    return w.xyz / w.w;
}
vec2 oct_wrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec3 oct_decode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = oct_wrap(n.xy);
    return normalize(n);
}
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
// NaN / Inf never enter a temporal history, an average or a blur (one bad pixel would otherwise
// spread over the whole screen and stay there): replaced by black, clamped to a sane HDR range.
vec3 sanitize(vec3 c) { return (any(isnan(c)) || any(isinf(c))) ? vec3(0.0) : clamp(c, vec3(0.0), vec3(65000.0)); }
vec4 sanitize4(vec4 c) { return (any(isnan(c)) || any(isinf(c))) ? vec4(0.0) : clamp(c, vec4(-65000.0), vec4(65000.0)); }
vec3 rgb_to_ycocg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 ycocg_to_rgb(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
// G-buffer material flags (gbuffer_body.glsl)
#define GB_VIEWMODEL 1u
#define GB_DYNAMIC 2u
#define GB_UNLIT 4u
uint gb_flags(vec4 m) { return uint(m.b * 255.0 + 0.5); }

// Blue noise (64x64 RGBA, 4 independent channels) with a per-frame offset (R2 sequence) so the
// stochastic passes converge under temporal filtering.
vec4 blue_noise(sampler2D tex, ivec2 px) {
    float f = U.depthp.w;
    ivec2 off = ivec2(fract(vec2(0.7548777, 0.5698403) * f) * 64.0);
    vec4 n = texelFetch(tex, (px + off) & 63, 0);
    return fract(n + 0.618034 * floor(f / 64.0));
}
float interleaved_gradient(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
