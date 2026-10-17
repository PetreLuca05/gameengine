#version 450
// Classic lit pass (depth EQUAL against the prepass): the shared lit body.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define PBR_MAPS   // glTF material maps (set 5)
#include "classic_common.glsl"
#define MAT_COLOR CL_MAT_COLOR
#define MAT_PARAMS pc.params
#define MAT_FLAGS CL_MAT_FLAGS
#include "lit_body.glsl"
