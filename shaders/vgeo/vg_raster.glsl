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
