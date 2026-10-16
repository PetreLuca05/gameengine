#version 450
// HW raster fragment (vertex pulling path): no attachments, no depth test; the 64-bit atomicMax into
// the visibility buffer is the depth test. Masked / dithered clusters run the shared per-pixel test
// first (vg_mask.glsl: the SW rasteriser runs the same one on the same UV plane).
#extension GL_GOOGLE_include_directive : require
#define VG_VIS
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"
#include "vg_mask.glsl"

layout(location = 0) flat in uint i_payload;
layout(location = 1) flat in uint i_info;
layout(location = 2) flat in vec3 i_cf;
layout(location = 3) flat in vec3 i_pu;
layout(location = 4) flat in vec3 i_pv;
layout(location = 5) flat in vec3 i_pd;

void main() {
    uint x = uint(gl_FragCoord.x), y = uint(gl_FragCoord.y);
    if ((i_info & 3u) != 0u) {
        if (vg_vis_loses(x, y, gl_FragCoord.z, i_payload)) return;
        VgUvPlane pl;
        pl.u = i_pu;
        pl.v = i_pv;
        pl.d = i_pd;
        if (!vg_mask_pass(i_info & 3u, i_info >> 4, i_cf.x, i_cf.y, i_cf.z, pl, ivec2(x, y))) return;
    }
    vg_vis_write(x, y, gl_FragCoord.z, i_payload);
}
