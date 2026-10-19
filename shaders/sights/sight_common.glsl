// Shared by the weapon sight shaders (src/render/post/post_sights.c, struct SightGpu).
// Bindings: 0 viewmodel depth, 1 scope picture-in-picture, 2 noise, 13 sight params, 14 exposure.
#include "post_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D vm_depth;
layout(set = 0, binding = 1) uniform sampler2D pip_tex;
layout(set = 0, binding = 2) uniform sampler2D noise_tex;
layout(std430, set = 0, binding = 13) readonly buffer SightParams {
    vec4 lens;          // view-space lens centre, radius
    vec4 fwd;           // optic forward, kind (1 red dot, 2 scope PiP, 3 scope full screen)
    vec4 up;            // w ads_t
    vec4 right;         // w complete
    vec4 dotp;          // dot point (main view space), dot radius (rad)
    vec4 color;         // reticle rgb, brightness
    vec4 tint;          // glass tint, dust
    vec4 glass;         // fresnel, chromatic px, reticle shape, outer radius (rad)
    vec4 scope;         // k, k nominal (SFP), eye box, eye box depth
    vec4 eye;           // lens-space eye offset x, y, relief error, illuminated
    vec4 ray_main0, ray_main_du, ray_main_dv;
    vec4 ray_vm0, ray_vm_du, ray_vm_dv;
    vec4 vmdepth;       // vm proj m10, m14, debug view, ffp
    vec4 misc;          // tan(sight fov / 2), aspect, time, output height
    vec4 render;        // render w, h, output w, h
    mat4 pip_from_view;
} S;
layout(std430, set = 0, binding = 14) readonly buffer Exposure { float e[16]; } X;

const float RET_DOT = 0.0, RET_CIRCLE_DOT = 1.0, RET_DUPLEX = 2.0, RET_MILDOT = 3.0;

vec3 main_ray(vec2 uv) { return S.ray_main0.xyz + uv.x * S.ray_main_du.xyz + uv.y * S.ray_main_dv.xyz; }
vec3 vm_ray(vec2 uv) { return S.ray_vm0.xyz + uv.x * S.ray_vm_du.xyz + uv.y * S.ray_vm_dv.xyz; }

// Ray from the eye (view origin) against the lens disc (viewmodel projection). Returns the
// lens-local position (units of the radius) and the hit depth (view z); coverage 0 outside.
struct LensHit { vec2 q; float r; float z; float cos_t; bool ok; };
LensHit lens_hit(vec2 uv) {
    LensHit h;
    vec3 d = normalize(vm_ray(uv));
    float den = dot(d, S.fwd.xyz);
    h.ok = abs(den) > 1e-5;
    float t = dot(S.lens.xyz, S.fwd.xyz) / (h.ok ? den : 1.0);
    vec3 p = d * t - S.lens.xyz;
    h.q = vec2(dot(p, S.right.xyz), dot(p, S.up.xyz)) / max(S.lens.w, 1e-5);
    h.r = length(h.q);
    h.z = (d * t).z;
    h.cos_t = abs(den);
    h.ok = h.ok && t > 0.0;
    return h;
}

// Viewmodel parts in front of the lens hide it (hands, a canted rail, the scope's own rim).
bool lens_occluded(vec2 uv, float z) {
    float d_lens = (S.vmdepth.x * z + S.vmdepth.y) / -z;
    float d = textureLod(vm_depth, uv, 0.0).r;
    return d < d_lens - 2e-6;
}

// Anti-aliased coverage of the disc edge (r in radius units), using screen derivatives.
float disc_cov(float r) {
    float w = max(fwidth(r), 1e-4);
    return 1.0 - smoothstep(1.0 - w, 1.0 + w, r);
}

float exposure_inv() { return 1.0 / max(X.e[0], 1e-4); }

// Reticle mask at tangent position t (radians-ish, small angles) for the shape; px = one pixel
// in the same units. 1 = reticle ink.
float reticle_mask(vec2 t, float px, float shape, float thick, float outer) {
    float a = 0.0;
    float r = length(t);
    if (shape == RET_DOT) {
        a = 1.0 - smoothstep(thick - px, thick + px, r);
    } else if (shape == RET_CIRCLE_DOT) {
        a = 1.0 - smoothstep(thick - px, thick + px, r);
        float ring = abs(r - outer) - thick * 0.6;
        a = max(a, 1.0 - smoothstep(-px, px, ring));
    } else {
        // Fine crosshair, heavy posts beyond the outer radius (duplex); mil dots on the fine part. The
        // fine lines are at least 2.5 px wide: thinner, they broke up into a grey dashed line.
        float lw = max(thick, px * 1.25);
        float fine = min(abs(t.x), abs(t.y));
        a = 1.0 - smoothstep(lw - px, lw + px, fine);
        float post_w = lw * 3.5;
        float postm = (r > outer) ? (1.0 - smoothstep(post_w - px, post_w + px, fine)) : 0.0;
        a = max(a, postm);
        if (shape == RET_MILDOT) {
            float mil = 0.001;
            vec2 g = t / mil;
            vec2 c = vec2(abs(t.x) > abs(t.y) ? round(g.x) : 0.0, abs(t.x) > abs(t.y) ? 0.0 : round(g.y)) * mil;
            float dd = length(t - c);
            float dotm = (length(c) > mil * 0.5 && length(c) < outer) ? (1.0 - smoothstep(lw * 2.2 - px, lw * 2.2 + px, dd)) : 0.0;
            a = max(a, dotm);
        }
    }
    return clamp(a, 0.0, 1.0);
}
