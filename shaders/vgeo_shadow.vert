#version 450
// vgeo clusters in the sun shadow cascades (depth only, vertex pulling): one instance per visible
// cluster of the cascade's view (its HW list), 128 x 3 vertices; triangles past the cluster's count
// collapse. Push constants as the legacy shadow pass (flags.w = cascade, shadow_pass_vp).
#extension GL_GOOGLE_include_directive : require
#define VG_SET 3
#define VG_NO_PUSH
#define VG_VIEW_INDEX 0
#include "vgeo/vgeo_common.glsl"
#include "common.glsl"
#include "vgeo/vg_decode.glsl"
layout(push_constant) uniform Push { mat4 model; vec4 color; vec4 params; uvec4 flags; } pc;
void main() {
    uvec2 e = visible[gl_InstanceIndex];
    VgClusterRef ref = vg_cluster_ref(e.y);
    VgXf x = vg_instance_xf(e.x);
    VgMesh m = meshes[x.mesh];
    VgClusterData c = vg_cluster(ref.slot, ref.index);
    uint tri = uint(gl_VertexIndex) / 3u, corner = uint(gl_VertexIndex) % 3u;
    if (tri >= c.tri_count) { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); return; }
    uvec3 t = vg_triangle(c, tri);
    uint v = corner == 0u ? t.x : (corner == 1u ? t.y : t.z);
    gl_Position = shadow_pass_vp(pc.flags.w) * vec4(vg_xf_point(x, vg_vertex_pos(c, m, v)), 1.0);
}
