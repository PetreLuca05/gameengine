#version 450
// Classic instanced mesh draws into the sun cascades / local light tiles (depth only; masked: uv).
#extension GL_GOOGLE_include_directive : require
#define CL_SHADOW
#include "common.glsl"
#include "classic_common.glsl"
#include "classic_vert.glsl"
