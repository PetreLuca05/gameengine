// Virtualized geometry rasterisers (docs/VGEO.md "Rasterisation"): helpers shared by the HW (mesh /
// vertex pulling + fragment) and SW (compute) paths, so both see bit-identical clip positions.
// Include after vgeo_common.glsl and vg_decode.glsl.
//
// pc.pass selects the visible list range: 1 = pass 1 entries [0, *_P1), 2 = pass 2 [*_P1, total).

// [start, end) of the HW (sw false) or SW list for pc.pass (indices within the list).
uvec2 vg_raster_range(bool sw) {
    uint cap = V.caps0.z;
    uint p1 = min(ctr.n[sw ? VGN_SW_P1 : VGN_HW_P1], cap), all = min(ctr.n[sw ? VGN_SW : VGN_HW], cap);
    return pc.pass == 1u ? uvec2(0u, p1) : uvec2(p1, max(all, p1));
}

// Per-pixel test bits of a cluster (vg_mask.glsl; the vertex / mesh stages pass them flat to the
// fragment stage): 1 alpha mask, 2 distance-fade dither. 0: every covered pixel is written.
uint vg_test_bits(VgMesh m, VgMaterial mat) {
    uint b = 0u;
    if ((mat.flags & VGM_MASKED) != 0u) b |= 1u;
    if ((mat.flags & VGM_DITHER_FADE) != 0u && (m.fflags & VGMF_FOLIAGE) != 0u) b |= 2u;
    return b;
}

// The per-cluster inputs of the pixel test: bits, mask texture, cutoff, the instance's fade alpha
// (vg_foliage_alpha), the UV scale of the material (as the lit body's texture lookup).
struct VgMaskInfo { uint bits, tex; float cutoff, fade, tex_scale; vec2 tsz; };
VgMaskInfo vg_mask_info(VgXf x, VgMesh m, VgMaterial mat) {
    VgMaskInfo i;
    i.bits = vg_test_bits(m, mat);
    i.tex = mat.mask_tex != 0u ? mat.mask_tex : mat.albedo_tex;
    i.tsz = (i.bits & 1u) != 0u ? vec2(textureSize(vg_tex[nonuniformEXT(i.tex)], 0)) : vec2(1.0);
    i.cutoff = mat.alpha_cutoff;
    i.fade = (i.bits & 2u) != 0u ? vg_foliage_alpha(m, x.fp, distance(x.pos, V.cam_pos.xyz)) : 1.0;
    i.tex_scale = max(mat.tex_scale, 1e-3);
    return i;
}

// A triangle's UV plane: the perspective-correct UV at NDC point p is (dot(u, P), dot(v, P)) /
// dot(d, P) with P = (p, 1), from its clip positions and UVs. Both rasterisers compute it with this
// function from the same clip positions (bit-identical inputs: the same pixels pass the mask).
struct VgUvPlane { vec3 u, v, d; };
VgUvPlane vg_uv_plane(vec4 c0, vec4 c1, vec4 c2, vec2 uv0, vec2 uv1, vec2 uv2) {
    vec2 s0 = c0.xy / c0.w, s1 = c1.xy / c1.w, s2 = c2.xy / c2.w;
    float area = (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);
    VgUvPlane pl;
    if (abs(area) < 1e-20) {
        pl.u = vec3(0.0, 0.0, uv0.x);
        pl.v = vec3(0.0, 0.0, uv0.y);
        pl.d = vec3(0.0, 0.0, 1.0);
        return pl;
    }
    // screen barycentrics of vertices 1 and 2 as planes in p (vertex 0: 1 - both), then / w
    vec3 e1 = vec3(s2.y - s0.y, -(s2.x - s0.x), (s2.x - s0.x) * s0.y - s0.x * (s2.y - s0.y)) / area;
    vec3 e2 = vec3(-(s1.y - s0.y), s1.x - s0.x, s0.x * (s1.y - s0.y) - (s1.x - s0.x) * s0.y) / area;
    vec3 q0 = (vec3(0.0, 0.0, 1.0) - e1 - e2) / c0.w, q1 = e1 / c1.w, q2 = e2 / c2.w;
    pl.d = q0 + q1 + q2;
    pl.u = q0 * uv0.x + q1 * uv1.x + q2 * uv2.x;
    pl.v = q0 * uv0.y + q1 * uv1.y + q2 * uv2.y;
    return pl;
}
// The mask's mip LOD of a triangle (one per triangle, so both rasterisers sample the same mip): the
// UV footprint of a pixel at the triangle's screen centroid, in texels of a tsz texture.
float vg_uv_plane_lod(VgUvPlane pl, vec4 c0, vec4 c1, vec4 c2, vec2 tsz) {
    vec3 p = vec3((c0.xy / c0.w + c1.xy / c1.w + c2.xy / c2.w) / 3.0, 1.0);
    vec2 d = V.viewport.zw * 2.0;
    vec2 n = vec2(dot(pl.u, p), dot(pl.v, p));
    float w = dot(pl.d, p);
    vec2 uv = n / w;
    vec2 ux = (n + vec2(pl.u.x, pl.v.x) * d.x) / (w + pl.d.x * d.x), uy = (n + vec2(pl.u.y, pl.v.y) * d.y) / (w + pl.d.y * d.y);
    float rho = max(length((ux - uv) * tsz), length((uy - uv) * tsz));
    return log2(max(rho, 1e-6));
}

// Clip position of cluster vertex v with the raster view (skinned instances: skinned first).
vec4 vg_raster_clip(VgXf x, VgClusterData c, VgMesh m, uint v) {
    return V.raster_view_proj * vec4(vg_vertex_world(x, c, m, v), 1.0);
}

#ifdef VG_VIS
// Visibility buffer write (vgeo_gpu.h): the nearest depth wins, ties go to the larger payload. (A plain
// load skipping the atomic when it already loses measured no faster on the RTX 3060 Ti.)
void vg_vis_write(uint x, uint y, float z, uint payload) {
    uint64_t v = uint64_t(floatBitsToUint(1.0 - z)) << 32 | uint64_t(payload);
    atomicMax(vis[y * uint(V.viewport.x) + x], v);
}
#endif
