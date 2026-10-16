#version 450
// Classic lit pass (depth EQUAL against the prepass): the shared lit body.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "classic_common.glsl"
#define MAT_COLOR pc.color
#define MAT_PARAMS pc.params
#define MAT_FLAGS pc.flags.x
#include "lit_body.glsl"
