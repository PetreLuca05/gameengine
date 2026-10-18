// Shared G-buffer output (gbuffer.frag, gbuffer_skin.frag). PBR_MAPS: the glTF material maps
// (shaders/pbr_maps.glsl: normal, metallic-roughness, emissive) as the lit pass applies them;
// GB_ALPHA_MASK (an expression: the albedo alpha) discards below the alphaMode MASK cutoff.
//   normal   RG16_SNORM  octahedral world normal
//   material RGBA8       r roughness, g metallic, b flags / 255 (1 viewmodel, 2 dynamic, 4 unlit, 8 thin occluder,
//                        bits 4-7 (1 - ao_strength) x 15), a emissive (0..1 of 8)
//   velocity RG16F       current - previous position in UV units (unjittered)
// GB_AO_BYTE: the material's AO byte (src/render/material_ao.h; default the pbr emissive word's alpha).
#define GB_VIEWMODEL 1u
#define GB_DYNAMIC 2u
#define GB_UNLIT 4u
#define GB_THIN 8u
#define GB_AO_SHIFT 4u
#define GB_DYNAMIC_FLAG 0x80000000u

#include "weather.glsl"
#ifdef PBR_MAPS
#include "pbr_maps.glsl"
#endif
#ifndef GB_AO_BYTE
#ifdef PBR_MAPS
#define GB_AO_BYTE (PBR_PARAMS.z >> 24u)
#else
#define GB_AO_BYTE 0u
#endif
#endif

// GB_CUSTOM_INPUTS: the includer provides v_world ... v_prev_clip and GB_FRONT_FACING itself
// (the vgeo resolve decodes them per pixel, shaders/vg_gbuffer.frag).
#ifndef GB_CUSTOM_INPUTS
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_color;
layout(location = 7) in vec4 v_cur_clip;
layout(location = 8) in vec4 v_prev_clip;
#define GB_FRONT_FACING gl_FrontFacing
#endif

layout(location = 0) out vec2 o_normal;
layout(location = 1) out vec4 o_material;
layout(location = 2) out vec2 o_velocity;

vec2 oct_wrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 oct_encode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : oct_wrap(n.xy);
}

void main() {
#ifdef GB_PRE_MAIN
    GB_PRE_MAIN
#endif
    uint flags = GB_FLAGS;
#ifdef GB_ALPHA_MASK
    if (GB_ALPHA_MASK < pbr_alpha_cutoff()) discard;
#endif
    vec3 n = normalize(v_normal);
    if (!GB_FRONT_FACING) n = -n;
    float rough = clamp(GB_PARAMS.x, 0.04, 1.0), metal = clamp(GB_PARAMS.y, 0.0, 1.0), emissive = GB_PARAMS.z;
    if ((flags & MAT_VERTEX_PBR) != 0u) { rough = clamp(v_uv.x, 0.04, 1.0); metal = clamp(v_uv.y, 0.0, 1.0); }
#ifdef PBR_MAPS
    vec2 map_uv = v_uv * max(GB_PARAMS.w, 1e-3);
    n = pbr_normal(flags, map_uv, v_world, n);
    pbr_metal_rough(flags, map_uv, rough, metal);
    rough = max(rough, 0.04);
    if ((flags & MAT_EMISSIVE_TEX) != 0u) {
        vec3 e = pbr_emissive(map_uv);
        emissive *= max(e.r, max(e.g, e.b));
    }
#endif
    // Weather: the same wet / puddle / snow terms as the lit pass (SSR sees wet, rippled puddles).
    vec3 wx_alb = vec3(0.5);
    float wx_rough = rough;
    weather_surface(v_world, n, wx_alb, wx_rough, metal, (flags & MAT_VIEWMODEL) != 0u);
    o_normal = oct_encode(n);
    uint gf = 0u;
    if ((flags & MAT_VIEWMODEL) != 0u) gf |= GB_VIEWMODEL;
    if ((flags & GB_DYNAMIC_FLAG) != 0u || (flags & MAT_HIGHLIGHT) != 0u) gf |= GB_DYNAMIC;
    if ((flags & MAT_UNLIT) != 0u) gf |= GB_UNLIT;
    // Per-material AO (docs/RENDERING_POST.md "Foliage-aware AO"): thin occluders for the GTAO search, the
    // weight for the debug view (the lit pass applies the exact value from the material itself).
    uint ao_b = GB_AO_BYTE;
    if ((ao_b & MAT_AO_BYTE_THIN) != 0u) gf |= GB_THIN;
    gf |= uint((1.0 - mat_ao_strength(ao_b)) * 15.0 + 0.5) << GB_AO_SHIFT;
    o_material = vec4(wx_rough, metal, float(gf) / 255.0, clamp(emissive / 8.0, 0.0, 1.0));
    vec2 cur = v_cur_clip.xy / v_cur_clip.w, prev = v_prev_clip.xy / v_prev_clip.w;
    o_velocity = (cur - prev) * 0.5;   // NDC -> UV
}
