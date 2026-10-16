// vgeo clusters in the sun shadow cascades (vgeo_shadow.vert: opaque, depth only; vgeo_shadow_masked.vert
// + .frag: alpha-tested): vertex pulling, one instance per visible cluster of the cascade's view,
// 128 x 3 vertices, triangles past the cluster's count collapse. gl_InstanceIndex is the visible
// index (the masked draw's firstInstance is the SW half). Positions as every other stage
// (vg_vertex_world: skinned, WPO). Push constants as the legacy shadow pass (flags.w = cascade).
layout(push_constant) uniform Push { mat4 model; vec4 color; vec4 params; uvec4 flags; } pc;

vec4 vg_shadow_vertex(out vec2 uv, out uint tex, out float cutoff) {
    uvec2 e = visible[gl_InstanceIndex];
    VgClusterRef ref = vg_cluster_ref(e.y);
    VgXf x = vg_instance_xf(e.x);
    VgMesh m = meshes[x.mesh];
    VgClusterData c = vg_cluster(ref.slot, ref.index);
    uint tri = uint(gl_VertexIndex) / 3u, corner = uint(gl_VertexIndex) % 3u;
    uv = vec2(0.0);
    tex = 0u;
    cutoff = 0.0;
    if (tri >= c.tri_count) return vec4(0.0, 0.0, 0.0, 1.0);
    uvec3 t = vg_triangle(c, tri);
    uint v = corner == 0u ? t.x : (corner == 1u ? t.y : t.z);
    VgMaterial mat = materials[m.material_base + c.material];
    uv = vg_vertex_uv(c, m, v) * max(mat.tex_scale, 1e-3);
    tex = mat.mask_tex != 0u ? mat.mask_tex : mat.albedo_tex;
    cutoff = mat.alpha_cutoff;
    return shadow_pass_vp(pc.flags.w) * vec4(vg_vertex_world(x, c, m, v), 1.0);   // skinned: re-skinned
}
