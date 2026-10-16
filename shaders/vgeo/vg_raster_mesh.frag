#version 450
// HW raster fragment (mesh shader path): the payload and the per-pixel test bits are per primitive.
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_mesh_shader : require
#define VG_VIS
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"
#include "vg_mask.glsl"

layout(location = 0) perprimitiveEXT flat in uint i_payload;
layout(location = 1) perprimitiveEXT flat in uint i_bits;

void main() {
    uint x = uint(gl_FragCoord.x), y = uint(gl_FragCoord.y);
    if (i_bits != 0u && (vg_vis_loses(x, y, gl_FragCoord.z, i_payload) || !vg_pixel_test(i_payload >> VG_VIS_TRI_BITS, i_payload & ((1u << VG_VIS_TRI_BITS) - 1u), ivec2(x, y))))
        return;
    vg_vis_write(x, y, gl_FragCoord.z, i_payload);
}
