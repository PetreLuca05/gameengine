// SW raster (vg_raster_sw*.comp): one workgroup per SW visible cluster of the pass range. The threads
// transform the cluster's vertices once into shared memory (screen space, 16.8 fixed point), then one
// thread per triangle: back-face cull (CCW front, as the HW pipelines; two-sided materials keep both
// faces), bounding box scan over the pixel centres (x + 0.5, y + 0.5) with integer edge functions and
// the top-left fill rule (y down), depth from the plane equation at the pixel centre, atomicMax into
// the visibility buffer (same packing as HW).
//
// Three variants, each dispatched over the whole SW range and taking only its kind of entry (so the
// plain kernel keeps its registers / occupancy): VG_SW_KIND 0 plain clusters, 1 masked / dithered
// clusters (the per-pixel test of vg_mask.glsl on the triangle's UV plane, as the HW fragment stages),
// 2 impostor quads (entries VG_IMPOSTOR_ENTRY: 4 vertices, 2 triangles, both faces, 64 threads per
// triangle sharing its rows).
//
// SW clusters are small (< sw_px across) and never cross the near plane (vg_clusters.comp sends those
// to HW), so there is no clipping: a vertex with w <= 0 or outside the +-16k px guard band drops its
// triangles (never hit with the classification). Triangles are bbox-scanned whatever their size.
#extension GL_GOOGLE_include_directive : require
#define VG_COMPUTE
#define VG_VIS
#include "vgeo_common.glsl"
#include "vg_decode.glsl"
#include "vg_raster.glsl"
#if VG_SW_KIND != 0
#include "vg_mask.glsl"
#endif
layout(local_size_x = 128) in;

shared ivec2 s_xy[128];   // 16.8 fixed point, framebuffer pixels (y down)
shared float s_z[128];    // NDC depth, < -1 when the vertex is unusable
#if VG_SW_KIND == 1
shared vec4 s_clip[128];  // masked clusters: clip positions and UVs (the triangles' UV planes)
shared vec2 s_uv[128];
VgMaskInfo g_mi;          // this cluster's per-pixel test inputs
VgUvPlane g_pl;           // this thread's triangle's UV plane and mask LOD
float g_lod;
#elif VG_SW_KIND == 2
VgImpostorTri g_it;       // this thread's triangle of the quad
#endif

int64_t edge(ivec2 a, ivec2 b, ivec2 p) { return int64_t(b.x - a.x) * int64_t(p.y - a.y) - int64_t(b.y - a.y) * int64_t(p.x - a.x); }
// Top-left rule for the positive (screen-clockwise, y down) orientation: a top edge runs exactly
// horizontal to the right, a left edge runs up. Pixel centres on other edges are excluded.
int64_t bias(ivec2 a, ivec2 b) { ivec2 d = b - a; return (d.y < 0 || (d.y == 0 && d.x > 0)) ? int64_t(0) : int64_t(-1); }

// Writes a covered pixel: the per-pixel test first (masked / impostor variants).
void sw_write(int x, int y, float z, uint payload) {
#if VG_SW_KIND == 1
    if (!vg_mask_pass(g_mi.bits, g_mi.tex, g_mi.cutoff, g_mi.fade, g_lod, g_pl, ivec2(x, y))) return;
#elif VG_SW_KIND == 2
    if (!vg_impostor_pass(g_it, ivec2(x, y))) return;
#endif
    vg_vis_write(uint(x), uint(y), z, payload);
}

// Bounding box scan in integer type T: ra / rb / rd are the biased edge functions of the first pixel
// centre (weights of a, b, d), stepped per pixel; z from the plane through the unbiased weights.
// Rows yoff, yoff + ystep, ... (impostor quads: 64 threads per triangle share its rows).
#define SCAN(T, RA, RB, RD, ZERO)                                                                       \
    {                                                                                                   \
        T xa = T(b.y - d.y) * 256, xb = T(d.y - a.y) * 256, xd = T(a.y - b.y) * 256;                     \
        T ya = T(d.x - b.x) * 256, yb = T(a.x - d.x) * 256, yd = T(b.x - a.x) * 256;                     \
        T ra = RA + ya * T(yoff), rb = RB + yb * T(yoff), rd = RD + yd * T(yoff);                       \
        for (int y = y0 + yoff; y <= y1; y += ystep) {                                                  \
            T wa = ra, wb = rb, wd = rd;                                                                \
            for (int x = x0; x <= x1; x++) {                                                            \
                if ((wa | wb | wd) >= ZERO) {                                                           \
                    float z = za + (float(wb - bd) * dzb + float(wd - ba) * dzd) * inv;                 \
                    if (z >= 0.0 && z <= 1.0) sw_write(x, y, z, payload);                              \
                }                                                                                       \
                wa += xa;                                                                               \
                wb += xb;                                                                               \
                wd += xd;                                                                               \
            }                                                                                           \
            ra += ya * T(ystep);                                                                        \
            rb += yb * T(ystep);                                                                        \
            rd += yd * T(ystep);                                                                        \
        }                                                                                               \
    }

void main() {
    uvec2 r = vg_raster_range(true);
    uint k = r.x + vg_flat_group();
    if (k >= r.y) return;
    uint vi = V.caps0.z + k;   // SW entries follow the HW list
    uvec2 e = vg_visible_entry(vi);
    bool imp = e.y == VG_IMPOSTOR_ENTRY;
    if (imp != (VG_SW_KIND == 2)) return;   // the impostor variant takes impostors only
    VgXf x = vg_instance_xf(e.x);
    VgMesh m = meshes[x.mesh];
    VgClusterData c;
    uint nv = 4u, nt = 2u;
    bool two = true;
    if (!imp) {
        VgClusterRef cr = vg_cluster_ref(e.y);
        c = vg_cluster(cr.slot, cr.index);
        VgMaterial mat = materials[m.material_base + c.material];
        uint bits = vg_test_bits(m, mat);
        if ((bits != 0u) != (VG_SW_KIND == 1)) return;   // plain vs masked variant
        nv = c.vert_count;
        nt = c.tri_count;
        two = (mat.flags & VGM_TWO_SIDED) != 0u;
#if VG_SW_KIND == 1
        g_mi = vg_mask_info(x, m, mat);
#endif
    }
    uint t = gl_LocalInvocationID.x;
    if (t < nv) {
        vec4 p;
#if VG_SW_KIND == 2
        p = V.raster_view_proj * vec4(vg_impostor_corner(vg_impostor_quad(x, m), t), 1.0);
#else
        p = vg_raster_clip(x, c, m, t);
#endif
#if VG_SW_KIND == 1
        if ((g_mi.bits & 1u) != 0u) {
            s_clip[t] = p;
            s_uv[t] = vg_vertex_uv(c, m, t) * g_mi.tex_scale;
        }
#endif
        vec2 s = (p.xy / p.w * 0.5 + 0.5) * V.viewport.xy;
        bool ok = p.w > 1e-6 && all(lessThan(abs(s), vec2(16384.0)));
        s_xy[t] = ok ? ivec2(floor(s * 256.0 + 0.5)) : ivec2(0);
        s_z[t] = ok ? p.z / p.w : -2.0;
    }
    barrier();
    int yoff = 0, ystep = 1;
#if VG_SW_KIND == 2
    yoff = int(t & 63u);
    ystep = 64;
    t >>= 6;
#endif
    if (t >= nt) return;
#if VG_SW_KIND == 2
    uvec3 tri = vg_impostor_tri(t);
    g_it = vg_impostor_tri_setup(x, m, t);
#else
    uvec3 tri = vg_triangle(c, t);
#endif
#if VG_SW_KIND == 1
    if ((g_mi.bits & 1u) != 0u) {
        g_pl = vg_uv_plane(s_clip[tri.x], s_clip[tri.y], s_clip[tri.z], s_uv[tri.x], s_uv[tri.y], s_uv[tri.z]);
        g_lod = vg_uv_plane_lod(g_pl, s_clip[tri.x], s_clip[tri.y], s_clip[tri.z], g_mi.tsz);
    }
#endif
    float za = s_z[tri.x], zb = s_z[tri.y], zd = s_z[tri.z];
    if (min(za, min(zb, zd)) < -1.5) return;
    ivec2 a = s_xy[tri.x], b = s_xy[tri.y], d = s_xy[tri.z];
    // Vulkan's signed area is -cross (y down): CCW front faces have cross < 0. Swap to the positive
    // order; back faces (cross > 0) are already in it and only kept for two-sided materials.
    int64_t area = edge(a, b, d);
    if (area == 0 || (area > 0 && !two)) return;
    if (area < 0) {
        ivec2 tb = b; b = d; d = tb;
        float tz = zb; zb = zd; zd = tz;
        area = -area;
    }
    ivec2 lo = min(min(a, b), d), hi = max(max(a, b), d);
    int x0 = max((lo.x + 127) >> 8, 0), x1 = min((hi.x - 128) >> 8, int(V.viewport.x) - 1);
    int y0 = max((lo.y + 127) >> 8, 0), y1 = min((hi.y - 128) >> 8, int(V.viewport.y) - 1);
    if (x0 > x1 || y0 > y1) return;
    // Edge functions at the first pixel centre (biased: inside <=> all >= 0), steps per pixel. 32-bit
    // when the triangle spans < 128 px (|edge| < 2^31; every SW triangle in practice), else 64-bit.
    ivec2 p0 = ivec2(x0, y0) * 256 + 128;
    float inv = 1.0 / float(area), dzb = zb - za, dzd = zd - za;
    uint payload = vg_vis_payload(vi, t);
    if (max(hi.x - lo.x, hi.y - lo.y) < 32768) {
        int bb = int(bias(b, d)), bd = int(bias(d, a)), ba = int(bias(a, b));
        SCAN(int, int(edge(b, d, p0)) + bb, int(edge(d, a, p0)) + bd, int(edge(a, b, p0)) + ba, 0)
    } else {
        int64_t bb = bias(b, d), bd = bias(d, a), ba = bias(a, b);
        SCAN(int64_t, edge(b, d, p0) + bb, edge(d, a, p0) + bd, edge(a, b, p0) + ba, int64_t(0))
    }
}
