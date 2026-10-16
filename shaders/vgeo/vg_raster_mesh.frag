#version 450
// HW raster fragment (mesh shader path): the payload is per primitive.
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_mesh_shader : require
#define VG_VIS
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"

layout(location = 0) perprimitiveEXT flat in uint i_payload;

void main() { vg_vis_write(uint(gl_FragCoord.x), uint(gl_FragCoord.y), gl_FragCoord.z, i_payload); }
