// HW raster, VK_EXT_mesh_shader (vg_raster_hw*.mesh): one workgroup per HW visible cluster of the pass
// range, one thread per vertex and per triangle (<= 128 / 128). Each draw takes one kind of cluster
// (pc.a[0], vg_hw_kind: 0 plain one-sided: the plain variant; 1 two-sided, 2 one-sided masked: the
// masked variant, VG_HW_MASKED 1); the others emit nothing. Masked: the vertices' clip positions and
// UVs go through shared memory, each triangle's UV plane is a per-primitive output.
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_mesh_shader : require
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"

layout(local_size_x = 128) in;
layout(triangles, max_vertices = 128, max_primitives = 128) out;
layout(location = 0) perprimitiveEXT flat out uint o_payload[];
#if VG_HW_MASKED
layout(location = 1) perprimitiveEXT flat out uint o_info[];
layout(location = 2) perprimitiveEXT flat out vec3 o_cf[];
layout(location = 3) perprimitiveEXT flat out vec3 o_pu[];
layout(location = 4) perprimitiveEXT flat out vec3 o_pv[];
layout(location = 5) perprimitiveEXT flat out vec3 o_pd[];
shared vec4 s_clip[128];
shared vec2 s_uv[128];
#endif

void main() {
    uvec2 r = vg_raster_range(false);
    uint k = r.x + gl_WorkGroupID.y * 65535u + gl_WorkGroupID.x;
    if (k >= r.y) {
        SetMeshOutputsEXT(0u, 0u);
        return;
    }
    uvec2 e = vg_visible_entry(k);
    VgClusterRef cr = vg_cluster_ref(e.y);
    VgClusterData c = vg_cluster(cr.slot, cr.index);
    VgXf x = vg_instance_xf(e.x);
    VgMesh m = meshes[x.mesh];
    VgMaterial mat = materials[m.material_base + c.material];
    if (pc.a[1] == 0u && vg_hw_kind(m, mat) != pc.a[0]) {   // a[1]: every cluster is plain
        SetMeshOutputsEXT(0u, 0u);
        return;
    }
    SetMeshOutputsEXT(c.vert_count, c.tri_count);
    uint t = gl_LocalInvocationID.x;
#if VG_HW_MASKED
    VgMaskInfo mi = vg_mask_info(x, m, mat);
    if (t < c.vert_count) {
        vec4 p = vg_raster_clip(x, c, m, t);
        gl_MeshVerticesEXT[t].gl_Position = p;
        s_clip[t] = p;
        s_uv[t] = vg_vertex_uv(c, m, t) * mi.tex_scale;
    }
    barrier();
    if (t < c.tri_count) {
        uvec3 tri = vg_triangle(c, t);
        gl_PrimitiveTriangleIndicesEXT[t] = tri;
        o_payload[t] = vg_vis_payload(k, t);
        o_info[t] = mi.bits | mi.tex << 4;
        VgUvPlane pl;
        pl.u = pl.v = pl.d = vec3(0.0);
        float lod = 0.0;
        if ((mi.bits & 1u) != 0u) {
            pl = vg_uv_plane(s_clip[tri.x], s_clip[tri.y], s_clip[tri.z], s_uv[tri.x], s_uv[tri.y], s_uv[tri.z]);
            lod = vg_uv_plane_lod(pl, s_clip[tri.x], s_clip[tri.y], s_clip[tri.z], mi.tsz);
        }
        o_cf[t] = vec3(mi.cutoff, mi.fade, lod);
        o_pu[t] = pl.u;
        o_pv[t] = pl.v;
        o_pd[t] = pl.d;
    }
#else
    if (t < c.vert_count) gl_MeshVerticesEXT[t].gl_Position = vg_raster_clip(x, c, m, t);
    if (t < c.tri_count) {
        gl_PrimitiveTriangleIndicesEXT[t] = vg_triangle(c, t);
        o_payload[t] = vg_vis_payload(k, t);
    }
#endif
}
