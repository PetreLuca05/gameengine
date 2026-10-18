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

// Which HW draw takes a cluster (VgRaster: pc.a[0] of the draw): 0 plain one-sided (back faces culled,
// no per-pixel test), 1 two-sided material (no culling), 2 one-sided masked / dithered.
uint vg_hw_kind(VgMesh m, VgMaterial mat) {
    if ((mat.flags & VGM_TWO_SIDED) != 0u) return 1u;
    return vg_test_bits(m, mat) != 0u ? 2u : 0u;
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
// UV footprint of a pixel at the triangle's screen centroid, in texels of a tsz texture. The centroid
// of the vertices in front of the camera only, clamped to the screen: a large card crossing the camera
// plane (Tree It conifer cards next to the camera) would otherwise put it where a vertex behind the
// camera projects (anywhere), and the footprint there picks the coarsest mip, whose averaged alpha
// passes the cutoff over the whole card. Capped at the 4 x 4 mip for the same reason (CPU mirror:
// tests/test_vgeo_foliage.c cpu_alpha).
float vg_uv_plane_lod(VgUvPlane pl, vec4 c0, vec4 c1, vec4 c2, vec2 tsz) {
    vec2 s = vec2(0.0);
    float k = 0.0;
    if (c0.w > 1e-6) { s += c0.xy / c0.w; k += 1.0; }
    if (c1.w > 1e-6) { s += c1.xy / c1.w; k += 1.0; }
    if (c2.w > 1e-6) { s += c2.xy / c2.w; k += 1.0; }
    vec3 p = vec3(k > 0.0 ? clamp(s / k, vec2(-1.0), vec2(1.0)) : vec2(0.0), 1.0);
    vec2 d = V.viewport.zw * 2.0;
    vec2 n = vec2(dot(pl.u, p), dot(pl.v, p));
    float w = dot(pl.d, p), wx = w + pl.d.x * d.x, wy = w + pl.d.y * d.y;
    if (w <= 0.0 || wx <= 0.0 || wy <= 0.0) return 0.0;   // the plane's horizon: no footprint there
    vec2 uv = n / w;
    vec2 ux = (n + vec2(pl.u.x, pl.v.x) * d.x) / wx, uy = (n + vec2(pl.u.y, pl.v.y) * d.y) / wy;
    float rho = max(length((ux - uv) * tsz), length((uy - uv) * tsz));
    return clamp(log2(max(rho, 1e-6)), 0.0, max(log2(max(tsz.x, tsz.y)) - 2.0, 0.0));
}

// Clip position of cluster vertex v with the raster view (skinned instances: skinned first).
vec4 vg_raster_clip(VgXf x, VgClusterData c, VgMesh m, uint v) {
    return V.raster_view_proj * vec4(vg_vertex_world(x, c, m, v), 1.0);
}

#if defined(VG_VIS32)
// Without 64-bit atomics (MoltenVK; docs/PLATFORM_MACOS.md "vgeo on Metal"): the same 8 bytes per pixel
// as two u32 words, written by two runs of the same rasterisation (pc.a[7], VgRaster vis32): 1 the depth
// key, 2 the payload where the key is this fragment's. Nearest depth, then the larger payload: the u64
// atomicMax value bit for bit (an empty pixel stays 0), so every reader is unchanged. A fragment that
// raises the key clears the payload: an earlier pass's winner (two-pass occlusion) must not survive
// in the max of phase 2 (only phase 1 stores there, and only 0: no ordering between raisers matters).
void vg_vis_write(uint x, uint y, float z, uint payload) {
    uint i = (y * uint(V.viewport.x) + x) * 2u, key = floatBitsToUint(1.0 - z);
    if (pc.a[7] == 1u) {
        if (atomicMax(vis32[i + 1u], key) < key) vis32[i] = 0u;
    } else if (vis32[i + 1u] == key) {
        atomicMax(vis32[i], payload);
    }
}
#elif defined(VG_VIS)
// Visibility buffer write (vgeo_gpu.h): the nearest depth wins, ties go to the larger payload. (A plain
// load skipping the atomic when it already loses measured no faster on the RTX 3060 Ti.)
void vg_vis_write(uint x, uint y, float z, uint payload) {
    uint64_t v = uint64_t(floatBitsToUint(1.0 - z)) << 32 | uint64_t(payload);
    atomicMax(vis[y * uint(V.viewport.x) + x], v);
}
#endif
