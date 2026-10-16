#version 450
// vgeo lit resolve (inside the opaque pass): the shared lit body on the visibility buffer's
// surfaces, depth test EQUAL against the exported depth, so pixels the legacy prepass covered keep
// their forward shading.
#extension GL_GOOGLE_include_directive : require
#define VG_SET 3
#define VG_NO_PUSH
#define VG_VIEW_INDEX 0
#define VG_VIS
#include "vgeo/vgeo_common.glsl"
#include "common.glsl"
#include "vgeo/vg_decode.glsl"
#include "vgeo/vg_resolve.glsl"
#define LIT_CUSTOM_INPUTS
#define LIT_FRONT_FACING true
#define MAT_COLOR g_mat_color
#define MAT_PARAMS g_mat_params
#define MAT_FLAGS g_mat_flags
#define LIT_PRE_MAIN if (!vg_resolve_pixel()) discard; gl_FragDepth = g_depth;
#include "lit_body.glsl"
