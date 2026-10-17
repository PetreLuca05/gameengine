#version 450
// Classic impostors, prepass: blended atlas coverage >= 0.5, the fade dither, baked normals.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "classic_common.glsl"
#include "classic_impostor.glsl"
#define GB_CUSTOM_INPUTS
#define GB_FRONT_FACING true
#define GB_FLAGS pc.flags.x
#define GB_PARAMS pc.params
#define GB_PRE_MAIN if (!cl_impostor_pixel()) discard;
#include "gbuffer_body.glsl"
