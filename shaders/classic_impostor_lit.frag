#version 450
// Classic impostors, lit pass (depth EQUAL): the atlas albedo through the shared lit body. Far quads
// of different trees can share a depth value, so the coverage test runs again here.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "classic_common.glsl"
#include "classic_impostor.glsl"
#define LIT_CUSTOM_INPUTS
#define LIT_FRONT_FACING true
#define MAT_COLOR pc.color
#define MAT_PARAMS pc.params
#define MAT_FLAGS pc.flags.x
#define LIT_PRE_MAIN if (!cl_impostor_pixel()) discard;
#define LIT_BASE_HOOK base.rgb = cl_lod_debug(base.rgb, i_fade);
#include "lit_body.glsl"
