// Classic instanced renderer of the vgeo scene content (src/render/vk_classic.c, docs/NO_NANITE.md).
// Include after common.glsl. Push constants (PushLit layout, per material draw):
//   model[0]  WPO parameters (shaders/vgeo/wpo.glsl)        impostors: mesh-space centre xyz, radius
//   model[1]  previous frame's wind (dir x, z, strength, gust)  impostors: x frames per side
//   model[2]  displacement buffer: texel 0 world x, z, metres per texel, valid
//   model[3]  previous time, alpha cutoff (0 = not masked), frame index (dither), dither fade on
//   color, params (roughness, metallic, emissive, tex scale), flags (x material, y WPO snippet, w shadow view)
// Instance stream (binding 1): i_pos (xyz, uniform scale), i_rot (quaternion), i_misc (x wind phase, y fade alpha).
#define WPO_GRASS 1u
#define WPO_TREE 2u
#define WPO_DISPLACE 256u
#define VG_DISPLACE_MAX 0.6
#include "vgeo/wpo.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
} pc;

// Set 3: binding 0 the displacement buffer (vertex) or an impostor's normal atlas, binding 1 its albedo atlas.
layout(set = 3, binding = 0) uniform sampler2D cl_aux;
layout(set = 3, binding = 1) uniform sampler2D cl_aux2;

vec3 cl_qrot(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// This frame's wind as the vgeo views see it (RenderWeather: frame.wx2 wind, frame.wx5 gust / time).
vec4 cl_wind() {
    float ws = frame.wx2.z;
    return vec4(ws > 1e-4 ? frame.wx2.xy / ws : vec2(1.0, 0.0), ws, frame.wx5.x);
}

// The material's WPO at world position w (instance pivot ip, wind weights col): as vg_wpo.
vec3 cl_wpo(vec3 w, vec3 ip, vec4 col, float phase, bool prev) {
    uint sn = pc.flags.y;
    if (sn == 0u) return vec3(0.0);
    vec3 d = wpo_offset(sn & 0xFFu, pc.model[0], w, ip, col, phase, prev ? pc.model[1] : cl_wind(), prev ? pc.model[3].x : frame.wx5.z);
    if ((sn & WPO_DISPLACE) != 0u && pc.model[2].w > 0.5) {
        vec2 rel = w.xz - pc.model[2].xy, ext = vec2(128.0 * pc.model[2].z);
        if (all(greaterThanEqual(rel, vec2(0.0))) && all(lessThan(rel, ext)))   // toroidal: world texel = texel mod 128
            d += wpo_displace(textureLod(cl_aux, w.xz / ext, 0.0).rg, col);
    }
    return d;
}

// Fade dither (vg_dither: interleaved gradient noise rotated per frame for TAA).
float cl_dither(ivec2 px) {
    vec2 p = vec2(px) + 5.588238 * float(uint(pc.model[3].z) & 63u);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
bool cl_faded(float fade, vec2 frag) { return pc.model[3].w > 0.5 && fade < 1.0 && fade <= cl_dither(ivec2(frag)); }
