#version 450
// Depth / G-buffer prepass (static draws, push-constant material): octahedral normal, material
// (roughness, metallic, flags), screen-space velocity. The lit pass shades at depth-equal later.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
} pc;

#define GB_FLAGS (pc.flags.x | (pc.flags.y != 0u ? GB_DYNAMIC_FLAG : 0u))   // moving draws: no decals
#define GB_PARAMS pc.params
#include "gbuffer_body.glsl"
