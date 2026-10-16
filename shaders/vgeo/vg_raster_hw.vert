#version 450
// HW raster, vertex pulling (no mesh shaders): one instance per HW visible cluster (gl_InstanceIndex =
// visible index: firstInstance is the pass range start), 384 vertices = 128 triangles x 3 corners;
// triangles past the cluster's count collapse outside the frustum. pc.a[0]: this draw's sidedness (0
// one-sided clusters, back faces culled; 1 two-sided materials, no culling): clusters of the other
// kind collapse too. Masked clusters: every corner computes its triangle's UV plane (flat).
#extension GL_GOOGLE_include_directive : require
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"

layout(location = 0) flat out uint o_payload;
layout(location = 1) flat out uint o_info;     // per-pixel test bits | mask texture << 4 (vg_mask.glsl)
layout(location = 2) flat out vec3 o_cf;       // cutoff, fade alpha, mask LOD
layout(location = 3) flat out vec3 o_pu;
layout(location = 4) flat out vec3 o_pv;
layout(location = 5) flat out vec3 o_pd;
// The visible list, read-only here (vgeo_common's declaration is writable, which vertex stages may only
// use with vertexPipelineStoresAndAtomics).
layout(set = 0, binding = 13) readonly buffer VisibleRo { uvec2 visible_ro[]; };

void main() {
    uint k = uint(gl_InstanceIndex), t = uint(gl_VertexIndex) / 3u;
    uvec2 e = visible_ro[k];
    VgClusterRef cr = vg_cluster_ref(e.y);
    VgClusterData c = vg_cluster(cr.slot, cr.index);
    o_payload = vg_vis_payload(k, t);
    o_info = 0u;
    o_cf = vec3(0.0);
    o_pu = o_pv = o_pd = vec3(0.0);
    VgXf x = vg_instance_xf(e.x);
    VgMesh m = meshes[x.mesh];
    VgMaterial mat = materials[m.material_base + c.material];
    if (t >= c.tri_count || ((mat.flags & VGM_TWO_SIDED) != 0u) != (pc.a[0] == 1u)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    uvec3 tri = vg_triangle(c, t);
    uint corner = uint(gl_VertexIndex) - t * 3u;
    VgMaskInfo mi = vg_mask_info(x, m, mat);
    o_info = mi.bits | mi.tex << 4;
    o_cf = vec3(mi.cutoff, mi.fade, 0.0);
    if ((mi.bits & 1u) != 0u) {
        vec4 c0 = vg_raster_clip(x, c, m, tri.x), c1 = vg_raster_clip(x, c, m, tri.y), c2 = vg_raster_clip(x, c, m, tri.z);
        VgUvPlane pl = vg_uv_plane(c0, c1, c2, vg_vertex_uv(c, m, tri.x) * mi.tex_scale, vg_vertex_uv(c, m, tri.y) * mi.tex_scale, vg_vertex_uv(c, m, tri.z) * mi.tex_scale);
        o_cf.z = vg_uv_plane_lod(pl, c0, c1, c2, mi.tsz);
        o_pu = pl.u;
        o_pv = pl.v;
        o_pd = pl.d;
        gl_Position = corner == 0u ? c0 : (corner == 1u ? c1 : c2);
        return;
    }
    gl_Position = vg_raster_clip(x, c, m, corner == 0u ? tri.x : (corner == 1u ? tri.y : tri.z));
}
