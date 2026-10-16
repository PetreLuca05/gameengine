#version 450
// HW raster fragment (vertex pulling path): no attachments, no depth test; the 64-bit atomicMax into
// the visibility buffer is the depth test.
#extension GL_GOOGLE_include_directive : require
#define VG_VIS
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"

layout(location = 0) flat in uint i_payload;

void main() { vg_vis_write(uint(gl_FragCoord.x), uint(gl_FragCoord.y), gl_FragCoord.z, i_payload); }
