#version 450
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
#define LIT_FROXEL_FOG (pc.flags.z != 0u)   // set by the transparent pass
#define LIT_SURFACES                         // terrain / layered surface textures (shaders/surfaces.glsl)
#define PBR_MAPS                             // glTF material maps (set 3)

#include "lit_body.glsl"
