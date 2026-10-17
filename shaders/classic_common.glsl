// Classic instanced renderer of the vgeo scene content (src/render/vk_classic.c, docs/NO_NANITE.md).
// Include after common.glsl. Push constants (PushLit layout, per material draw):
//   model[0]  wpo_params (trunk, branch, leaf stiffness, full wind)   impostors: mesh-space centre xyz, radius
//   model[1]  wind_params a (height, main / detail / flutter Hz)      impostors: x frames per side
//   model[2]  wind_params b (tan max bend, settle mix, response, 0)
//   model[3]  unused, alpha cutoff (0 = not masked), frame index (dither), dither fade on
//   color, params (roughness, metallic, emissive, tex scale), flags (x material, y WPO snippet | WPO_DISPLACE, w shadow view),
//   pbr (glTF maps: normal_scale, occlusion_strength bits, emissive RGBA8, 0; shaders/pbr_maps.glsl)
// Instance stream (binding 1): i_pos (xyz, uniform scale), i_rot (quaternion), i_misc (x wind phase, y fade alpha).
// Set 4 (per frame slot): the wind field as the vgeo views see it (VgViewGpu wind / wind_tile / foliage /
// disp / wind_misc) and the wind tile ring (docs/FOLIAGE_WIND.md).
#define WPO_DISPLACE 256u
#define VG_DISPLACE_MAX 0.6
#define VGW_MAIN 1u
#define VGW_DETAIL 2u
#define VGW_FLUTTER 4u
#define VGW_NORMALS 8u
#define VGWD_CHANNELS 1u
#define VGWD_BEND 2u
#include "vgeo/wpo.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
    uvec4 pbr;
} pc;

// PBR_MAPS (mesh shaders): set 5 is the material's glTF maps (R->mat_layout, vkr_material_set: every slot
// a valid texture, white / flat normal where the material has none), sampled by pbr_maps.glsl with the
// screen-derivative cotangent frame as on the legacy path (docs/PBR.md).
#ifdef PBR_MAPS
layout(set = 5, binding = 0) uniform sampler2D cl_pbr_tex[4];
#define PBR_TEX(slot, uv) texture(cl_pbr_tex[slot], (uv))
#endif

// Set 3: binding 0 the displacement buffer (vertex) or an impostor's normal atlas, binding 1 its albedo atlas.
layout(set = 3, binding = 0) uniform sampler2D cl_aux;
layout(set = 3, binding = 1) uniform sampler2D cl_aux2;

layout(set = 4, binding = 0) uniform ClWind {
    vec4 wind;          // the field outside its tile: instant x, z, settled bend x, z (m/s)
    vec4 wind_prev;     // last frame's (velocity)
    vec4 tile;          // tile: texel 0 world x, z, metres per texel, valid
    vec4 tile_prev;
    vec4 disp;          // displacement buffer: texel 0 world x, z, metres per texel, valid
    vec4 time;          // x time, y previous time
    uvec4 misc;         // x tile ring slot, y last frame's, z layers (VGW_*), w debug view (VGWD_*)
} cw;
layout(set = 4, binding = 1) uniform sampler2D cl_wind_tex[3];

vec3 cl_qrot(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// The wind field at world xz: as vg_wind_at (vg_decode.glsl), the tile blended into the field outside it.
vec4 cl_wind_at(vec2 xz, bool prev) {
    vec4 tile = prev ? cw.tile_prev : cw.tile, g = prev ? cw.wind_prev : cw.wind;
    if (tile.w < 0.5) return g;
    vec2 lo = tile.xy + 0.5 * tile.z, hi = lo + 63.0 * tile.z;
    float e = min(min(xz.x - lo.x, hi.x - xz.x), min(xz.y - lo.y, hi.y - xz.y));
    float w = clamp(e / (2.0 * tile.z), 0.0, 1.0);
    if (w <= 0.0) return g;
    vec4 s = textureLod(cl_wind_tex[prev ? cw.misc.y : cw.misc.x], xz / (64.0 * tile.z), 0.0);
    return g + (s - g) * w;
}

// The material's vegetation WPO at world position w (normal n): as vg_wpo_n. Instance pivot ip,
// scale ps, wind weights col, phase; prev: last frame's wind / time (velocity); nrm bent with
// VGW_NORMALS in `layers`.
vec3 cl_wpo(vec3 w, vec3 n, vec3 ip, float ps, vec4 col, float phase, bool prev, uint layers, inout vec3 nrm) {
    uint sn = pc.flags.y;
    if (sn == 0u) return vec3(0.0);
    vec2 disp = vec2(0.0);
    bool has_disp = false;
    if ((sn & WPO_DISPLACE) != 0u && cw.disp.w > 0.5) {
        vec2 rel = w.xz - cw.disp.xy, ext = vec2(128.0 * cw.disp.z);
        if (all(greaterThanEqual(rel, vec2(0.0))) && all(lessThan(rel, ext))) {   // toroidal: world texel = texel mod 128
            disp = textureLod(cl_aux, w.xz / ext, 0.0).rg;
            has_disp = true;
        }
    }
    return wpo_veg(pc.model[0], pc.model[1], pc.model[2], w, n, ip, ps, col, phase, cl_wind_at(ip.xz, prev), prev ? cw.time.y : cw.time.x, layers, disp,
                   has_disp, nrm);
}

// wind_view debug views on WPO materials (vg_resolve.glsl): the colour replaces the material's.
bool cl_wind_debug() { return pc.flags.y != 0u && cw.misc.w != 0u; }
vec4 cl_wind_debug_color(vec3 ip, vec4 col) {
    if (cw.misc.w == VGWD_CHANNELS) return vec4(col.rgb, 1.0);
    vec4 wf = cl_wind_at(ip.xz, false);
    vec2 W = wf.xy + (wf.zw - wf.xy) * pc.model[2].y;
    float bend = min(pc.model[0].x * pow(clamp(length(W) / pc.model[0].w, 0.0, 1.0), pc.model[2].z) / max(pc.model[2].x, 1e-4), 1.0);
    return vec4(bend, 0.15 * (1.0 - bend), 0.05, 1.0);
}
#define CL_MAT_COLOR (cl_wind_debug() ? vec4(1.0) : pc.color)
#define CL_MAT_FLAGS (cl_wind_debug() ? (pc.flags.x & ~(MAT_TEXTURED | MAT_TRIPLANAR | MAT_GRID | MAT_CHECKER)) : pc.flags.x)

// Fade dither (vg_dither: interleaved gradient noise rotated per frame for TAA).
float cl_dither(ivec2 px) {
    vec2 p = vec2(px) + 5.588238 * float(uint(pc.model[3].z) & 63u);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
bool cl_faded(float fade, vec2 frag) { return pc.model[3].w > 0.5 && fade < 1.0 && fade <= cl_dither(ivec2(frag)); }
