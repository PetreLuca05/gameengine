#version 450
// HW raster fragment (vertex pulling), plain clusters, 32-bit visibility writes (vg_raster_hw_frag.glsl, vg_raster.glsl VG_VIS32).
#define VG_HW_MASKED 0
#define VG_VIS32
#extension GL_GOOGLE_include_directive : require
#include "vg_raster_hw_frag.glsl"
