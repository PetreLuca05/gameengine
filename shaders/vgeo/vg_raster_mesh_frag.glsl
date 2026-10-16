// HW raster fragment (mesh shader path; vg_raster_mesh*.frag): the payload (and in the masked variant
// the per-pixel test inputs and the UV plane) are per primitive.
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_mesh_shader : require
#define VG_VIS
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"
#if VG_HW_MASKED
#include "vg_mask.glsl"
#endif

layout(location = 0) perprimitiveEXT flat in uint i_payload;
#if VG_HW_MASKED
layout(location = 1) perprimitiveEXT flat in uint i_info;
layout(location = 2) perprimitiveEXT flat in vec3 i_cf;
layout(location = 3) perprimitiveEXT flat in vec3 i_pu;
layout(location = 4) perprimitiveEXT flat in vec3 i_pv;
layout(location = 5) perprimitiveEXT flat in vec3 i_pd;
#endif

void main() {
    uint x = uint(gl_FragCoord.x), y = uint(gl_FragCoord.y);
#if VG_HW_MASKED
    if ((i_info & 3u) != 0u) {
        if (vg_vis_loses(x, y, gl_FragCoord.z, i_payload)) return;
        VgUvPlane pl;
        pl.u = i_pu;
        pl.v = i_pv;
        pl.d = i_pd;
        if (!vg_mask_pass(i_info & 3u, i_info >> 4, i_cf.x, i_cf.y, i_cf.z, pl, ivec2(x, y))) return;
    }
#endif
    vg_vis_write(x, y, gl_FragCoord.z, i_payload);
}
