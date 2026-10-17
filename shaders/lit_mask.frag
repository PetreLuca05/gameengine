#version 450
// lit.frag for glTF alphaMode MASK materials (PIPE_LIT_MASK, docs/PBR.md): discards below the cutoff.
// Its own pipeline, so the common lit pipeline keeps early depth.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
    uvec4 pbr;     // glTF maps (shaders/pbr_maps.glsl)
} pc;

#define MAT_COLOR pc.color
#define MAT_PARAMS pc.params
#define MAT_FLAGS pc.flags.x
#define LIT_FROXEL_FOG (pc.flags.z != 0u)
#define LIT_SURFACES
#define PBR_MAPS
#define LIT_ALPHA_MASK

#include "lit_body.glsl"
