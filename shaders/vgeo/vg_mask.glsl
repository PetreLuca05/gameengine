// Per-pixel raster test of foliage clusters (docs/VGEO.md "Foliage support"): the alpha mask
// (VGM_MASKED), the distance-fade dither (VGM_DITHER_FADE) and impostor coverage. Both rasterisers
// keep exactly the same pixels of every masked triangle: per triangle, its UV plane (vg_uv_plane in
// vg_raster.glsl) is computed by the same function from the same clip positions (vg_raster_clip: the
// mesh shader / vertex pulling pass it flat to the fragment stage, the SW rasteriser computes it per
// triangle thread), and per pixel vg_mask_pass evaluates it at the pixel centre and samples the
// mask's textureLod against the cutoff, at the triangle's mip LOD (vg_uv_plane_lod: the pixel
// footprint at its screen centroid, computed with the plane). Include after vg_raster.glsl (and VG_VIS).
#include "vgeo/vg_impostor.glsl"

bool vg_mask_pass(uint bits, uint tex, float cutoff, float fade, float lod, VgUvPlane pl, ivec2 px) {
    if ((bits & 2u) != 0u && fade < 1.0 && fade <= vg_dither(px)) return false;
    if ((bits & 1u) == 0u) return true;
    vec3 p = vec3((vec2(px) + 0.5) * V.viewport.zw * 2.0 - 1.0, 1.0);
    vec2 uv = vec2(dot(pl.u, p), dot(pl.v, p)) / dot(pl.d, p);
    return textureLod(vg_tex[nonuniformEXT(tex)], uv, lod).a >= cutoff;
}

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

// Impostor quads (SW only): per triangle, the tile coordinates of its 3 frames as UV planes (they are
// linear in the point on the quad); per pixel the blended atlas coverage >= 0.5, and the fade dither.
struct VgImpostorTri { VgImpostorFrames F; VgUvPlane pl[3]; float fade; bool dither; };
VgImpostorTri vg_impostor_tri_setup(VgXf x, VgMesh m, uint tri) {
    VgImpostorTri it;
    VgImpostorQuad q = vg_impostor_quad(x, m);
    it.F = vg_impostor_frames(x, m, q);
    uvec3 t = vg_impostor_tri(tri);
    vec3 w0 = vg_impostor_corner(q, t.x), w1 = vg_impostor_corner(q, t.y), w2 = vg_impostor_corner(q, t.z);
    vec4 c0 = V.raster_view_proj * vec4(w0, 1.0), c1 = V.raster_view_proj * vec4(w1, 1.0), c2 = V.raster_view_proj * vec4(w2, 1.0);
    vec3 l0 = vg_impostor_local(x, q, w0), l1 = vg_impostor_local(x, q, w1), l2 = vg_impostor_local(x, q, w2);
    for (int k = 0; k < 3; k++) it.pl[k] = vg_uv_plane(c0, c1, c2, vg_impostor_tile_uv(it.F, k, l0), vg_impostor_tile_uv(it.F, k, l1), vg_impostor_tile_uv(it.F, k, l2));
    it.dither = (m.fflags & VGMF_DITHER) != 0u;
    it.fade = it.dither ? vg_foliage_alpha(m, x.fp, distance(x.pos, V.cam_pos.xyz)) : 1.0;
    return it;
}
bool vg_impostor_pass(VgImpostorTri it, ivec2 px) {
    if (it.dither && it.fade < 1.0 && it.fade <= vg_dither(px)) return false;
    vec3 p = vec3((vec2(px) + 0.5) * V.viewport.zw * 2.0 - 1.0, 1.0);
    float a = 0.0;
    for (int k = 0; k < 3; k++) {
        vec2 t = vec2(dot(it.pl[k].u, p), dot(it.pl[k].v, p)) / dot(it.pl[k].d, p);
        if (any(lessThan(t, vec2(0.0))) || any(greaterThan(t, vec2(1.0)))) continue;
        vec2 uv = (it.F.fr[k] + clamp(t, vec2(it.F.hm), vec2(1.0 - it.F.hm))) / it.F.n;
        a += textureLod(vg_tex[nonuniformEXT(it.F.ta)], uv, it.F.lod).a * it.F.w[k];
    }
    return a >= 0.5;
}

#ifdef VG_VIS
// The pixel already holds a nearer (or equal, larger payload) surface: the atomic would lose, the
// test can be skipped. A stale read only means the test runs.
bool vg_vis_loses(uint px, uint py, float z, uint payload) {
    uint64_t v = uint64_t(floatBitsToUint(1.0 - z)) << 32 | uint64_t(payload);
    return vis[py * uint(V.viewport.x) + px] >= v;
}
#endif
