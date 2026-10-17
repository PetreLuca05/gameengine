#version 450
// gbuffer.frag for glTF alphaMode MASK materials (PIPE_PREPASS_MASK, docs/PBR.md): the albedo alpha
// (material x vertex x texture) against the cutoff, as lit_mask.frag tests it.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
    uvec4 pbr;     // glTF maps (shaders/pbr_maps.glsl)
} pc;

layout(set = 1, binding = 0) uniform sampler2D tex;

#define GB_FLAGS (pc.flags.x | (pc.flags.y != 0u ? GB_DYNAMIC_FLAG : 0u))
#define GB_PARAMS pc.params
#define PBR_MAPS
#define GB_ALPHA_MASK (pc.color.a * v_color.a * ((pc.flags.x & MAT_TEXTURED) != 0u ? texture(tex, v_uv * max(pc.params.w, 1e-3)).a : 1.0))
#include "gbuffer_body.glsl"
