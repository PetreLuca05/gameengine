// Per-pixel raster test of foliage clusters (docs/VGEO.md "Foliage support"): the alpha mask
// (VGM_MASKED), the distance-fade dither (VGM_DITHER_FADE) and impostor coverage. The HW fragment
// stages and the SW rasteriser call the same function with the same inputs (visible index, triangle,
// pixel), and it recomputes everything from them: the triangle's clip positions exactly as rasterised
// (vg_raster_clip), the perspective-correct barycentrics of the pixel centre, the UV there and at the
// next pixel right / below (the explicit mip LOD, as the hardware's quad derivatives would give), then
// textureLod of the mask. So SW and HW keep the same pixels of every masked triangle. Include after
// vg_raster.glsl (and VG_VIS).
#include "vgeo/vg_impostor.glsl"


// Perspective-correct barycentrics of NDC point pn on the triangle with clip positions c0 c1 c2.
vec3 vg_tri_bary(vec4 c0, vec4 c1, vec4 c2, vec2 pn) {
    vec2 s0 = c0.xy / c0.w, s1 = c1.xy / c1.w, s2 = c2.xy / c2.w;
    float area = (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);
    if (abs(area) < 1e-20) return vec3(1.0, 0.0, 0.0);
    vec3 b;
    b.y = ((pn.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (pn.y - s0.y)) / area;
    b.z = ((s1.x - s0.x) * (pn.y - s0.y) - (pn.x - s0.x) * (s1.y - s0.y)) / area;
    b.x = 1.0 - b.y - b.z;
    b /= vec3(c0.w, c1.w, c2.w);
    return b / (b.x + b.y + b.z);
}
vec2 vg_tri_uv(vec4 c0, vec4 c1, vec4 c2, vec2 uv0, vec2 uv1, vec2 uv2, vec2 pn) {
    vec3 b = vg_tri_bary(c0, c1, c2, pn);
    return uv0 * b.x + uv1 * b.y + uv2 * b.z;
}

// Whether pixel px keeps triangle `tri` of visible entry `vi` (HW list index, or cap + SW index).
bool vg_pixel_test(uint vi, uint tri, ivec2 px) {
    uvec2 e = visible[vi];
    VgXf x = vg_instance_xf(e.x);
    VgMesh m = meshes[x.mesh];
    vec2 pn = (vec2(px) + 0.5) * V.viewport.zw * 2.0 - 1.0;
    if (e.y == VG_IMPOSTOR_ENTRY) {
        VgImpostorQuad q = vg_impostor_quad(x, m);
        uvec3 t = vg_impostor_tri(tri);
        vec3 w0 = vg_impostor_corner(q, t.x), w1 = vg_impostor_corner(q, t.y), w2 = vg_impostor_corner(q, t.z);
        vec4 c0 = V.raster_view_proj * vec4(w0, 1.0), c1 = V.raster_view_proj * vec4(w1, 1.0), c2 = V.raster_view_proj * vec4(w2, 1.0);
        vec3 b = vg_tri_bary(c0, c1, c2, pn), nrm;
        if ((m.fflags & VGMF_DITHER) != 0u) {
            float fa = vg_foliage_alpha(m, x.fp, distance(x.pos, V.cam_pos.xyz));
            if (fa < 1.0 && fa <= vg_dither(px)) return false;
        }
        return vg_impostor_sample(x, m, q, w0 * b.x + w1 * b.y + w2 * b.z, false, nrm).a >= 0.5;
    }
    VgClusterRef r = vg_cluster_ref(e.y);
    VgClusterData c = vg_cluster(r.slot, r.index);
    VgMaterial mat = materials[m.material_base + c.material];
    uint bits = vg_test_bits(m, mat);
    if ((bits & 2u) != 0u) {
        float fa = vg_foliage_alpha(m, x.fp, distance(x.pos, V.cam_pos.xyz));
        if (fa < 1.0 && fa <= vg_dither(px)) return false;
    }
    if ((bits & 1u) == 0u) return true;
    uvec3 t = vg_triangle(c, tri);
    vec4 c0 = vg_raster_clip(x, c, m, t.x), c1 = vg_raster_clip(x, c, m, t.y), c2 = vg_raster_clip(x, c, m, t.z);
    float ts = max(mat.tex_scale, 1e-3);   // as the lit body's texture lookup
    vec2 uv0 = vg_vertex_uv(c, m, t.x) * ts, uv1 = vg_vertex_uv(c, m, t.y) * ts, uv2 = vg_vertex_uv(c, m, t.z) * ts;
    vec2 d = V.viewport.zw * 2.0;
    vec2 uv = vg_tri_uv(c0, c1, c2, uv0, uv1, uv2, pn);
    vec2 ux = vg_tri_uv(c0, c1, c2, uv0, uv1, uv2, pn + vec2(d.x, 0.0)), uy = vg_tri_uv(c0, c1, c2, uv0, uv1, uv2, pn + vec2(0.0, d.y));
    uint ti = mat.mask_tex != 0u ? mat.mask_tex : mat.albedo_tex;
    vec2 tsz = vec2(textureSize(vg_tex[nonuniformEXT(ti)], 0));
    float rho = max(length((ux - uv) * tsz), length((uy - uv) * tsz));
    float lod = log2(max(rho, 1e-6));
    return textureLod(vg_tex[nonuniformEXT(ti)], uv, lod).a >= mat.alpha_cutoff;
}

#ifdef VG_VIS
// The pixel already holds a nearer (or equal, larger payload) surface: the atomic would lose, the
// test can be skipped. A stale read only means the test runs.
bool vg_vis_loses(uint px, uint py, float z, uint payload) {
    uint64_t v = uint64_t(floatBitsToUint(1.0 - z)) << 32 | uint64_t(payload);
    return vis[py * uint(V.viewport.x) + px] >= v;
}
#endif
