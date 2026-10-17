// Shared G-buffer output (gbuffer.frag, gbuffer_skin.frag).
//   normal   RG16_SNORM  octahedral world normal
//   material RGBA8       r roughness, g metallic, b flags / 255 (1 viewmodel, 2 dynamic, 4 unlit), a emissive (0..1 of 8)
//   velocity RG16F       current - previous position in UV units (unjittered)
#define GB_VIEWMODEL 1u
#define GB_DYNAMIC 2u
#define GB_UNLIT 4u
#define GB_DYNAMIC_FLAG 0x80000000u

#include "weather.glsl"

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
    vec3 n = normalize(v_normal);
    if (!GB_FRONT_FACING) n = -n;
    // Weather: the same wet / puddle / snow terms as the lit pass (SSR sees wet, rippled puddles).
    vec3 wx_alb = vec3(0.5);
    float wx_rough = clamp(GB_PARAMS.x, 0.04, 1.0);
    weather_surface(v_world, n, wx_alb, wx_rough, clamp(GB_PARAMS.y, 0.0, 1.0), (flags & MAT_VIEWMODEL) != 0u);
    o_normal = oct_encode(n);
    uint gf = 0u;
    if ((flags & MAT_VIEWMODEL) != 0u) gf |= GB_VIEWMODEL;
    if ((flags & GB_DYNAMIC_FLAG) != 0u || (flags & MAT_HIGHLIGHT) != 0u) gf |= GB_DYNAMIC;
    if ((flags & MAT_UNLIT) != 0u) gf |= GB_UNLIT;
    o_material = vec4(wx_rough, clamp(GB_PARAMS.y, 0.0, 1.0), float(gf) / 255.0, clamp(GB_PARAMS.z / 8.0, 0.0, 1.0));
    vec2 cur = v_cur_clip.xy / v_cur_clip.w, prev = v_prev_clip.xy / v_prev_clip.w;
    o_velocity = (cur - prev) * 0.5;   // NDC -> UV
}
