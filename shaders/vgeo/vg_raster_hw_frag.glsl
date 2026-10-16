// HW raster fragment (vertex pulling path; vg_raster_hw*.frag): no attachments, no depth test; the
// 64-bit atomicMax into the visibility buffer is the depth test. The masked variant (VG_HW_MASKED 1)
// runs the shared per-pixel test first (vg_mask.glsl: the SW rasteriser runs the same one on the same
// UV plane).
#extension GL_GOOGLE_include_directive : require
#define VG_VIS
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"
#if VG_HW_MASKED
#include "vg_mask.glsl"
#endif

layout(location = 0) flat in uint i_payload;
#if VG_HW_MASKED
layout(location = 1) flat in uint i_info;
layout(location = 2) flat in vec3 i_cf;
layout(location = 3) flat in vec3 i_pu;
layout(location = 4) flat in vec3 i_pv;
layout(location = 5) flat in vec3 i_pd;
#endif

void main() {
    uint x = uint(gl_FragCoord.x), y = uint(gl_FragCoord.y);
#if VG_HW_MASKED
    if ((i_info & 3u) != 0u) {
        VgUvPlane pl;
        pl.u = i_pu;
        pl.v = i_pv;
        pl.d = i_pd;
        if (!vg_mask_pass(i_info & 3u, i_info >> 4, i_cf.x, i_cf.y, i_cf.z, pl, ivec2(x, y))) return;
    }
#endif
    vg_vis_write(x, y, gl_FragCoord.z, i_payload);
}
