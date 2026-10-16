#version 450
// HW raster, vertex pulling (no mesh shaders): one instance per HW visible cluster (gl_InstanceIndex =
// visible index: firstInstance is the pass range start), 384 vertices = 128 triangles x 3 corners;
// triangles past the cluster's count collapse outside the frustum.
#extension GL_GOOGLE_include_directive : require
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"

layout(location = 0) flat out uint o_payload;
// The visible list, read-only here (vgeo_common's declaration is writable, which vertex stages may only
// use with vertexPipelineStoresAndAtomics).
layout(set = 0, binding = 13) readonly buffer VisibleRo { uvec2 visible_ro[]; };

void main() {
    uint k = uint(gl_InstanceIndex), t = uint(gl_VertexIndex) / 3u;
    uvec2 e = visible_ro[k];
    VgClusterRef cr = vg_cluster_ref(e.y);
    VgClusterData c = vg_cluster(cr.slot, cr.index);
    o_payload = vg_vis_payload(k, t);
    if (t >= c.tri_count) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    VgXf x = vg_instance_xf(e.x);
    uvec3 tri = vg_triangle(c, t);
    uint corner = uint(gl_VertexIndex) - t * 3u;
    gl_Position = vg_raster_clip(x, c, meshes[x.mesh], corner == 0u ? tri.x : (corner == 1u ? tri.y : tri.z));
}
