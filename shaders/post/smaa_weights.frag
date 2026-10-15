#version 450
// SMAA 1x pass 2: blending weight calculation (area / search tables, third_party/smaa).
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
#define SMAA_GLSL_4 1
#define SMAA_RT_METRICS pc.p0
#define SMAA_PRESET_HIGH 1
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
#include "SMAA.hlsl"
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D edges_tex;
layout(set = 0, binding = 1) uniform sampler2D area_tex;
layout(set = 0, binding = 2) uniform sampler2D search_tex;
layout(location = 0) out vec4 out_weights;
void main() {
    vec2 pixcoord;
    vec4 offset[3];
    SMAABlendingWeightCalculationVS(v_uv, pixcoord, offset);
    out_weights = SMAABlendingWeightCalculationPS(v_uv, pixcoord, offset, edges_tex, area_tex, search_tex, vec4(0.0));
}
