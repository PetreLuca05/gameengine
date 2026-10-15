#version 450
// SMAA 1x pass 1: luma edge detection (on the tonemapped copy, third_party/smaa, MIT).
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
#define SMAA_GLSL_4 1
#define SMAA_RT_METRICS pc.p0
#define SMAA_PRESET_HIGH 1
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
#include "SMAA.hlsl"
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D color_tex;
layout(location = 0) out vec4 out_edges;
void main() {
    vec4 offset[3];
    SMAAEdgeDetectionVS(v_uv, offset);
    out_edges = vec4(SMAALumaEdgeDetectionPS(v_uv, offset, color_tex), 0.0, 0.0);
}
