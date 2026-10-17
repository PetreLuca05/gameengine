#version 450
// vgeo depth export + G-buffer (frame graph pass vgeo_resolve, before the prepass): the shared
// G-buffer body on the visibility buffer's surfaces; empty pixels are discarded. The legacy prepass
// then draws on top with its depth test (docs/VGEO.md "Resolve").
#extension GL_GOOGLE_include_directive : require
#define VG_SET 3
#define VG_NO_PUSH
#define VG_VIEW_INDEX 0
#define VG_VIS
#include "vgeo/vgeo_common.glsl"
#include "common.glsl"
#include "vgeo/vg_decode.glsl"
#include "vgeo/vg_resolve.glsl"
#define GB_CUSTOM_INPUTS
#define GB_FRONT_FACING g_front
#define GB_FLAGS g_mat_flags
#define GB_PARAMS g_mat_params
#define GB_PRE_MAIN if (!vg_resolve_pixel()) discard; gl_FragDepth = g_depth;
#include "gbuffer_body.glsl"
