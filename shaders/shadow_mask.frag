#version 450
// Alpha test of masked casters in the shadow passes (the albedo's alpha x the material colour's,
// against the glTF alphaCutoff in pbr.w): foliage cards and fences cast their cut-out shape.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
    vec4 params;
    uvec4 flags;
    uvec4 pbr;
} pc;

layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 2) in vec2 v_uv;

void main() {
    float a = pc.color.a * ((pc.flags.x & MAT_TEXTURED) != 0u ? texture(tex, v_uv).a : 1.0);
    if (a < uintBitsToFloat(pc.pbr.w)) discard;
}
