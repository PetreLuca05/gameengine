// Virtualized geometry rasteriser tests (src/render/vgeo/vgeo_raster.c, shaders/vgeo/vg_raster_*,
// docs/VGEO.md "Rasterisation"), on the window-less harness at 1920x1080:
//   - winding: the procedural meshes are CCW outward (positive signed volume)
//   - all-HW (sw_px 0) vs all-SW (sw_px huge) visibility: decoded (instance, cluster, triangle) per
//     pixel, <= 0.5 % of covered pixels differ and every differing pixel is on a triangle edge
//   - mesh shader vs vertex pulling HW paths: same tolerance (expected identical)
//   - depth: the visibility buffer depth vs a CPU recomputation of the triangle's plane at the pixel
//     centre (decoded vertices, raster_view_proj, 16.8 snapping), within 1e-5 relative
//   - mixed (sw_px 32) vs all-HW: same tolerance (no holes between the two rasterisers)
//   - two-pass: pass 1 (occluded by a synthetic wall) + pass 2 rasterise the same image as one pass
//   - 32-bit two-phase visibility writes (MoltenVK, docs/PLATFORM_MACOS.md) vs 64-bit atomics: every
//     pixel's depth bit-identical, the decoded (instance, cluster, triangle) identical but for depth
//     ties (the payload holds the visible-list index: append order, different on every cull)
//   - --validation: zero validation messages
#include "vgeo_gpu_harness.h"
#include "render/vgeo/vgeo_raster.h"
#include "render/vgeo/vgeo_procgen.h"
#include "core/engine.h"
#include "core/dgmath.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

#define W 1920
#define H 1080
#define ZNEAR 0.1f
#define FOV (60.0f * DG_DEG2RAD)
#define SW_ALL 1e9f

static VgHarness g_h;
static VgScene g_scene;
static VgCuller g_cull;
static VgRaster g_rast;
static VgRaster g_rast32;           // 32-bit two-phase writes on the same culler (VgContext.vis32)
static VgRaster *g_r = &g_rast;     // the rasteriser render() uses

typedef struct Cam { vec3 eye, target; } Cam;

static void view_for(const Cam *c, u32 flags, float sw_px, VgViewGpu *v) {
    mat4 view = mat4_look_at(c->eye, c->target, v3(0, 1, 0));
    mat4 proj = mat4_perspective(FOV, (float)W / (float)H, ZNEAR, 4000.0f);
    mat4 vp = mat4_mul(proj, view);
    float ps = (float)H / (2.0f * tanf(FOV * 0.5f));
    float eye[3] = { c->eye.x, c->eye.y, c->eye.z };
    vgeo_view_setup(v, vp.m, vp.m, eye, ps, ZNEAR, W, H, 1.0f, sw_px, flags);
}

// ------------------------------------------------------------------ scene

static double signed_volume(const VgGenMesh *m) {
    double v = 0;
    for (u32 i = 0; i < m->index_count; i += 3) {
        const float *a = &m->pos[m->indices[i] * 3], *b = &m->pos[m->indices[i + 1] * 3], *c = &m->pos[m->indices[i + 2] * 3];
        v += ((double)a[0] * ((double)b[1] * c[2] - (double)b[2] * c[1]) - (double)a[1] * ((double)b[0] * c[2] - (double)b[2] * c[0]) +
              (double)a[2] * ((double)b[0] * c[1] - (double)b[1] * c[0])) / 6.0;
    }
    return v;
}

static void add_mesh(VgGenMesh *m, const char *name) {
    double vol = signed_volume(m);
    CHECK(vol > 0, "%s: signed volume %.4f (not CCW outward)", name, vol);
    printf("winding: %s signed volume %.3f m3 (%u tris)\n", name, vol, m->index_count / 3);
    VgSource src;
    memset(&src, 0, sizeof(src));
    src.vertex_count = m->vertex_count;
    src.pos = m->pos;
    src.index_count = m->index_count;
    src.indices = m->indices;
    VgBuildOptions opt;
    memset(&opt, 0, sizeof(opt));
    VgBuilt b;
    char err[256];
    if (!vgeo_build(&src, &opt, &b, err, sizeof(err))) { printf("build failed: %s\n", err); exit(1); }
    vgeo_scene_add_mesh(&g_scene, &b);
    vgeo_free(&b);
    vgeo_gen_free(m);
}

static u32 g_rng = 12345;
static float rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(g_rng >> 8) / 16777216.0f;
}

static void place_instances(u32 side, float spacing) {
    vgeo_scene_clear_instances(&g_scene);
    g_rng = 12345;
    for (u32 z = 0; z < side; z++)
        for (u32 x = 0; x < side; x++) {
            float p[3] = { ((float)x - (float)side * 0.5f) * spacing + rnd() * 2.0f, rnd() * 1.5f, ((float)z - (float)side * 0.5f) * spacing + rnd() * 2.0f };
            quat q = quat_mul(quat_yaw(rnd() * 6.2831853f), quat_axis_angle(v3(1, 0, 0), (rnd() - 0.5f) * 0.8f));
            float r[4] = { q.x, q.y, q.z, q.w };
            vgeo_scene_add_instance(&g_scene, (x * 7 + z * 3) % g_scene.mesh_count, p, r, 0.5f + rnd() * 2.5f);
        }
    vgeo_scene_commit(&g_scene);
}

// ------------------------------------------------------------------ rendering + decode

typedef struct Px { u32 inst, packed, tri; float depth; } Px;   // inst ~0: empty
typedef struct Img { Px *px; u64 *vis; u32 hw, sw, hw_p1, sw_p1, covered; float ms[2]; } Img;

static void img_free(Img *m) { free(m->px); free(m->vis); memset(m, 0, sizeof(*m)); }

// Culls (both passes when the view is two-pass), clears and rasterises passes 1 and 2, reads back.
static void render(const VgViewGpu *v, const VgHizRef *prev, const VgHizRef *cur, bool mesh, Img *out) {
    memset(out, 0, sizeof(*out));
    g_r->use_mesh = mesh && g_r->p_mesh;
    vg_cull_set_vis(&g_cull, g_r->vis.buffer);
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
    vg_cull_record(&g_cull, cmd, 0, 0, v, prev, cur);
    vg_raster_clear(g_r, cmd);
    vg_raster_record(g_r, cmd, 0, 0, 1);
    vg_raster_record(g_r, cmd, 0, 0, 2);
    vgc_submit_wait(&g_h.ctx, pool, cmd);
    float ms1[2] = { 0, 0 }, ms2[2] = { 0, 0 };
    vg_raster_timings(g_r, 0, 0, 1, true, ms1);
    vg_raster_timings(g_r, 0, 0, 2, true, ms2);
    out->ms[0] = ms1[0] + ms2[0];
    out->ms[1] = ms1[1] + ms2[1];
    VgCounters ctr;
    VgViewRes *r = &g_cull.v[0];
    vgh_read(&g_h, &r->counters, 0, sizeof(VgCounters), &ctr);
    u32 cap = g_cull.caps.visible;
    out->hw = DG_MIN(ctr.n[VGN_HW], cap);
    out->sw = DG_MIN(ctr.n[VGN_SW], cap);
    out->hw_p1 = ctr.n[VGN_HW_P1];
    out->sw_p1 = ctr.n[VGN_SW_P1];
    CHECK(ctr.n[VGN_OVERFLOW] == 0, "%u overflows", ctr.n[VGN_OVERFLOW]);
    u32 *hw = (u32 *)malloc(8ull * (out->hw + 1)), *sw = (u32 *)malloc(8ull * (out->sw + 1));
    vgh_read(&g_h, &r->visible, 0, 8ull * out->hw, hw);
    vgh_read(&g_h, &r->visible, 8ull * cap, 8ull * out->sw, sw);
    u64 *vis = (u64 *)malloc(8ull * W * H);
    vgh_read(&g_h, &g_r->vis, 0, 8ull * W * H, vis);
    out->px = (Px *)malloc(sizeof(Px) * W * H);
    u32 bad = 0;
    for (u32 i = 0; i < W * H; i++) {
        Px *p = &out->px[i];
        p->inst = ~0u;
        if (!vis[i]) continue;
        u32 lo = (u32)vis[i], hi = (u32)(vis[i] >> 32), idx = lo >> VG_VIS_TRI_BITS;
        const u32 *e = idx < cap ? (idx < out->hw ? &hw[2 * idx] : NULL) : (idx - cap < out->sw ? &sw[2 * (idx - cap)] : NULL);
        if (!e) { bad++; continue; }
        float f;
        memcpy(&f, &hi, 4);
        p->inst = e[0];
        p->packed = e[1];
        p->tri = lo & ((1u << VG_VIS_TRI_BITS) - 1);
        p->depth = 1.0f - f;
        out->covered++;
    }
    CHECK(bad == 0, "%u pixels reference a visible index outside the lists", bad);
    out->vis = vis;
    free(hw);
    free(sw);
}

static bool same(const Px *a, const Px *b) { return a->inst == b->inst && (a->inst == ~0u || (a->packed == b->packed && a->tri == b->tri)); }

// Differing pixels between two images; each must be on a triangle edge (a 3x3 neighbour of a different
// triangle in either image). Returns the count.
static u32 compare(const char *what, const Img *a, const Img *b, double max_frac) {
    u32 covered = 0, diff = 0, inner = 0;
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) {
            u32 i = y * W + x;
            const Px *pa = &a->px[i], *pb = &b->px[i];
            if (pa->inst != ~0u || pb->inst != ~0u) covered++;
            if (same(pa, pb)) continue;
            diff++;
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; dy++)
                for (int dx = -1; dx <= 1 && !edge; dx++) {
                    int nx = (int)x + dx, ny = (int)y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    u32 n = (u32)ny * W + (u32)nx;
                    edge = !same(&a->px[n], pa) || !same(&b->px[n], pb);
                }
            if (!edge) {
                if (inner < 3) printf("  interior difference at %u,%u: %u/%x/%u (%.7f) vs %u/%x/%u (%.7f)\n", x, y, pa->inst, pa->packed, pa->tri, pa->depth, pb->inst,
                                      pb->packed, pb->tri, pb->depth);
                inner++;
            }
        }
    double frac = covered ? (double)diff / covered : 0.0;
    printf("  %s: %u of %u covered pixels differ (%.3f %%), %u not on an edge\n", what, diff, covered, frac * 100.0, inner);
    CHECK(covered > 10000, "%s: only %u pixels covered", what, covered);
    CHECK(frac <= max_frac, "%s: %.3f %% of covered pixels differ", what, frac * 100.0);
    CHECK(inner == 0, "%s: %u differing pixels not on a triangle edge", what, inner);
    return diff;
}

// ------------------------------------------------------------------ CPU depth reference

static void qrot(const float q[4], const float v[3], float o[3]) {
    float t[3] = { 2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]), 2.0f * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

// The page in a pool slot (the scene keeps page sources, not a CPU pool copy).
static const u32 *slot_words(u32 slot) {
    static u32 buf[VGEO_PAGE_SIZE / 4], cached = ~0u;
    if (cached != slot) {
        vgeo_scene_read_page(&g_scene, g_scene.slot_page[slot], buf);
        cached = slot;
    }
    return buf;
}

// Clip position of a cluster vertex (the shaders' decode: (base + offset) * q_step, instance transform).
static void vertex_clip(u32 inst, u32 slot, const VgCluster *cl, u32 v, const float *vp, double clip[4]) {
    const VgInstance *in = &g_scene.instances[inst];
    const VgMesh *me = &g_scene.meshes[in->mesh];
    const u32 *page = slot_words(slot);
    u32 vw = (me->flags & VGF_HAS_COLOR) ? 4 : 3;
    const u32 *w = page + (cl->offsets & 0x7FFF) + v * vw;
    i32 g[3] = { cl->base[0] + (i32)(w[0] & 0xFFFF), cl->base[1] + (i32)(w[0] >> 16), cl->base[2] + (i32)(w[1] & 0xFFFF) };
    float p[3] = { (float)g[0] * me->q_step, (float)g[1] * me->q_step, (float)g[2] * me->q_step }, pos[3], q[4], s, r[3];
    vgeo_instance_decode(in, pos, q, &s);
    for (int k = 0; k < 3; k++) p[k] *= s;
    qrot(q, p, r);
    double wp[3] = { (double)(pos[0] + r[0]), (double)(pos[1] + r[1]), (double)(pos[2] + r[2]) };
    for (int k = 0; k < 4; k++) clip[k] = (double)vp[k] * wp[0] + (double)vp[4 + k] * wp[1] + (double)vp[8 + k] * wp[2] + (double)vp[12 + k];
}

static void check_depth(const char *what, const Img *img, const VgViewGpu *v) {
    u32 stride = img->covered / 3000 + 1, n = 0, seen = 0, bad = 0, strict = 0, skipped = 0, outside = 0;
    double worst = 0;
    for (u32 i = 0; i < W * H; i++) {
        const Px *p = &img->px[i];
        if (p->inst == ~0u || seen++ % stride) continue;
        u32 slot = p->packed >> VG_CLUSTER_BITS, ci = p->packed & ((1u << VG_CLUSTER_BITS) - 1);
        const VgCluster *cl = vgeo_scene_cluster(&g_scene, slot, ci);
        const u32 *page = slot_words(slot);
        u32 t = page[((cl->offsets >> 15) & 0x7FFF) + p->tri];
        double sx[3], sy[3], sz[3];
        bool ok = true;
        for (int k = 0; k < 3; k++) {
            double c[4];
            vertex_clip(p->inst, slot, cl, (t >> (8 * k)) & 0xFF, v->raster_view_proj, c);
            if (c[3] <= 1e-6) { ok = false; break; }   // clipped by the HW rasteriser: no plane in screen space
            sx[k] = floor((c[0] / c[3] * 0.5 + 0.5) * W * 256.0 + 0.5) / 256.0;
            sy[k] = floor((c[1] / c[3] * 0.5 + 0.5) * H * 256.0 + 0.5) / 256.0;
            sz[k] = c[2] / c[3];
        }
        if (!ok) { skipped++; continue; }
        double px = (double)(i % W) + 0.5, py = (double)(i / W) + 0.5;
        double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
        double l1 = ((sx[2] - sx[0]) * (py - sy[0]) - (sy[2] - sy[0]) * (px - sx[0])) / -area;   // weight of vertex 1
        double l2 = ((sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (px - sx[0])) / area;    // weight of vertex 2
        if (l1 < -1e-6 || l2 < -1e-6 || l1 + l2 > 1 + 1e-6) outside++;
        double z = sz[0] + l1 * (sz[1] - sz[0]) + l2 * (sz[2] - sz[0]);
        double rel = fabs((double)p->depth - z) / fmax(z, 1e-6);
        if (rel > worst) worst = rel;
        // The f32 vertex transform can land a vertex on the other side of a 1/256 px snapping step than
        // the f64 reference: steep (grazing, very near) triangles get one step of slope as slack.
        double dzdx = ((sz[1] - sz[0]) * (sy[2] - sy[0]) - (sz[2] - sz[0]) * (sy[1] - sy[0])) / area;
        double dzdy = ((sz[2] - sz[0]) * (sx[1] - sx[0]) - (sz[1] - sz[0]) * (sx[2] - sx[0])) / area;
        if (rel > 1e-5) strict++;
        if (fabs((double)p->depth - z) > 1e-5 * z + (fabs(dzdx) + fabs(dzdy)) / 256.0) {
            if (bad < 3) printf("  depth at %u,%u: gpu %.8f cpu %.8f\n", i % W, i / W, p->depth, z);
            bad++;
        }
        n++;
    }
    printf("  %s depth: %u pixels, worst relative error %.2e, %u over 1e-5 (%u over 1e-5 + one snapping step), %u centres outside the CPU triangle, "
           "%u skipped (w <= 0)\n", what, n, worst, strict, bad, outside, skipped);
    CHECK(n > 1000, "%s depth: only %u samples", what, n);
    CHECK(bad == 0, "%s depth: %u of %u samples off by more than 1e-5 relative", what, bad, n);
    CHECK(outside <= n / 1000, "%s depth: %u pixel centres outside their triangle", what, outside);
}

// ------------------------------------------------------------------ tests

static Cam k_cams[4] = {
    { { 30, 4, 80 }, { 0, 0, 0 } },              // looking in from the edge
    { { 0, 1.7f, 0 }, { 0, 1.7f, -50 } },        // inside the field, eye height
    { { 5, 1.0f, 5 }, { 10, 1, 9 } },            // close to instances
    { { 0 } },                                   // grazing a rock's surface (near plane cuts clusters)
};

static void test_rasterisers(void) {
    // Camera 3: 0.15 m outside a leaf cluster's sphere of the rock instance nearest the origin, looking
    // at it. (Near the origin: the shaders transform world-space positions in f32, which at 100 m from
    // the origin and 0.3 m from the camera alone is ~5e-5 relative depth, beyond the depth check.)
    u32 best = 0;
    float best_d = 1e30f;
    for (u32 i = 0; i < g_scene.instance_count; i++) {
        const VgInstance *in = &g_scene.instances[i];
        float d = in->pos[0] * in->pos[0] + in->pos[2] * in->pos[2];
        if (in->mesh == 0 && d < best_d) { best_d = d; best = i; }
    }
    for (u32 i = best; i < g_scene.instance_count; i++) {
        const VgInstance *in = &g_scene.instances[i];
        const VgMesh *me = &g_scene.meshes[0];
        const VgCluster *cl = vgeo_scene_cluster(&g_scene, g_scene.page_table[me->page_base + me->page_count - 1], 0);
        float p[3], q[4], s, c[3], d[3];
        vgeo_instance_decode(in, p, q, &s);
        float lc[3] = { cl->center[0] * s, cl->center[1] * s, cl->center[2] * s }, lo[3] = { (cl->center[0] - me->center[0]) * s, (cl->center[1] - me->center[1]) * s, (cl->center[2] - me->center[2]) * s };
        qrot(q, lc, c);
        qrot(q, lo, d);
        vec3 wc = v3(p[0] + c[0], p[1] + c[1], p[2] + c[2]), out = v3_norm(v3(d[0], d[1], d[2]));
        k_cams[3].eye = v3_add(wc, v3_scale(out, cl->radius * s + 0.15f));
        k_cams[3].target = wc;
        break;
    }
    for (int ci = 0; ci < 4; ci++) {
        printf("cam %d\n", ci);
        VgViewGpu v;
        Img hw, sw, mix, vp;
        view_for(&k_cams[ci], VGV_NO_OCCLUSION, 0.0f, &v);
        render(&v, NULL, NULL, true, &hw);
        CHECK(hw.sw == 0, "cam %d: %u SW clusters with sw_px 0", ci, hw.sw);
        view_for(&k_cams[ci], VGV_NO_OCCLUSION, SW_ALL, &v);
        render(&v, NULL, NULL, true, &sw);
        printf("  all-HW: %u clusters, %.3f ms; all-SW: %u SW + %u HW (near plane), %.3f + %.3f ms\n", hw.hw, hw.ms[0], sw.sw, sw.hw, sw.ms[1], sw.ms[0]);
        CHECK(sw.sw > hw.hw / 2, "cam %d: only %u of %u clusters went SW", ci, sw.sw, hw.hw);
        if (ci == 3) CHECK(sw.hw > 0, "cam 3: no cluster near the camera went to HW");
        compare("HW vs SW", &hw, &sw, 0.005);
        check_depth("HW", &hw, &v);
        check_depth("SW", &sw, &v);
        img_free(&sw);
        view_for(&k_cams[ci], VGV_NO_OCCLUSION, 32.0f, &v);
        render(&v, NULL, NULL, true, &mix);
        printf("  mixed: %u HW + %u SW clusters\n", mix.hw, mix.sw);
        CHECK(ci == 3 || (mix.hw > 0 && mix.sw > 0), "cam %d: mixed has %u HW, %u SW", ci, mix.hw, mix.sw);
        compare("mixed vs HW", &mix, &hw, 0.005);
        img_free(&mix);
        if (g_rast.p_mesh) {
            view_for(&k_cams[ci], VGV_NO_OCCLUSION, 0.0f, &v);
            render(&v, NULL, NULL, false, &vp);
            printf("  vertex pulling %.3f ms vs mesh shader %.3f ms\n", vp.ms[0], hw.ms[0]);
            compare("mesh shader vs vertex pulling", &hw, &vp, 0.005);
            img_free(&vp);
        }
        img_free(&hw);
    }
    if (!g_rast.p_mesh) printf("no VK_EXT_mesh_shader: mesh path not tested\n");
}

// Pixels whose depth key (the high word) differs.
static u32 depth_diff(const Img *a, const Img *b) {
    u32 d = 0;
    for (u32 i = 0; i < W * H; i++) d += (a->vis[i] >> 32) != (b->vis[i] >> 32);
    return d;
}

// The same views through the 32-bit two-phase writes: the raw visibility buffer must match bit for bit.
static void test_vis32(void) {
    if (!g_rast32.ok) { CHECK(false, "vis32 rasteriser unavailable"); return; }
    for (int ci = 0; ci < 4; ci++) {
        const float sws[3] = { 0.0f, SW_ALL, 32.0f };
        for (int k = 0; k < 3; k++) {
            VgViewGpu v;
            Img a, b;
            view_for(&k_cams[ci], VGV_NO_OCCLUSION, sws[k], &v);
            g_r = &g_rast;
            render(&v, NULL, NULL, false, &a);
            g_r = &g_rast32;
            render(&v, NULL, NULL, false, &b);
            g_r = &g_rast;
            u32 dz = depth_diff(&a, &b);
            printf("  vis32 cam %d sw_px %g: %u covered, %u depths differ (64-bit HW %.3f + SW %.3f ms, 32-bit depth %.3f + payload %.3f ms)\n", ci, sws[k], a.covered, dz,
                   a.ms[0], a.ms[1], b.ms[0], b.ms[1]);
            CHECK(a.covered > 0 && a.covered == b.covered, "vis32 cam %d: covered %u vs %u", ci, a.covered, b.covered);
            CHECK(dz == 0, "vis32 cam %d sw_px %g: %u pixel depths differ from the 64-bit writes", ci, sws[k], dz);
            compare("vis32 vs 64-bit", &a, &b, 0.0001);
            img_free(&a);
            img_free(&b);
        }
    }
}

static void test_two_pass(void) {
    // Camera at z = 60 looking down -z; a wall at 30 m over the left half of the previous Hi-Z.
    Cam cam = { { 0, 2, 60 }, { 0, 2, 0 } };
    VgHizImage wall, empty;
    vgh_hiz_create(&g_h, &wall, W, H);
    vgh_hiz_create(&g_h, &empty, W, H);
    mat4 proj = mat4_perspective(FOV, (float)W / (float)H, ZNEAR, 4000.0f);
    float wall_z = (proj.m[10] * -30.0f + proj.m[14]) / 30.0f;
    float *depth = (float *)malloc(sizeof(float) * W * H);
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) depth[y * W + x] = x < W / 2 ? wall_z : 1.0f;
    vgh_hiz_build(&g_h, &g_cull, &wall, depth);
    for (u32 i = 0; i < W * H; i++) depth[i] = 1.0f;
    vgh_hiz_build(&g_h, &g_cull, &empty, depth);
    free(depth);
    VgHizRef wr = vgh_hiz_ref(&wall), er = vgh_hiz_ref(&empty);
    VgViewGpu v;
    Img one, two;
    view_for(&cam, VGV_NO_OCCLUSION, 32.0f, &v);
    render(&v, NULL, NULL, true, &one);
    view_for(&cam, VGV_TWO_PASS, 32.0f, &v);
    render(&v, &wr, &er, true, &two);
    printf("two-pass: pass 1 %u HW + %u SW, pass 2 %u HW + %u SW (one pass: %u + %u)\n", two.hw_p1, two.sw_p1, two.hw - two.hw_p1, two.sw - two.sw_p1, one.hw, one.sw);
    CHECK(two.hw > two.hw_p1 && two.sw > two.sw_p1, "two-pass: pass 2 rasterised nothing new");
    u32 d = compare("two-pass vs one pass", &one, &two, 0.0001);
    CHECK(d == 0, "two-pass: %u pixels differ from one pass", d);
    if (g_rast32.ok && g_r != &g_rast32) {
        Img t64, t32;   // both vertex pulling: the mesh shader path's depths differ in the last bits
        render(&v, &wr, &er, false, &t64);
        g_r = &g_rast32;
        render(&v, &wr, &er, false, &t32);
        g_r = &g_rast;
        u32 d32 = depth_diff(&t64, &t32);
        CHECK(d32 == 0, "two-pass: %u pixel depths differ between the 64-bit and 32-bit writes", d32);
        compare("two-pass vis32 vs 64-bit", &t64, &t32, 0.0001);
        img_free(&t64);
        img_free(&t32);
    }
    img_free(&one);
    img_free(&two);
    vgh_hiz_destroy(&g_h, &wall);
    vgh_hiz_destroy(&g_h, &empty);
}

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++) validation |= !strcmp(argv[i], "--validation");
    if (!vgh_init(&g_h, validation)) { printf("vgeo_raster: no device, skipped\n"); return 0; }
    u64 t0 = SDL_GetPerformanceCounter();
    vgeo_scene_init(&g_scene, &g_h.ctx);
    VgGenMesh m;
    float c0[3] = { 0, 0, 0 };
    memset(&m, 0, sizeof(m));
    vgeo_gen_rock(&m, 3, 30, 1.0f, c0, 0.3f);
    add_mesh(&m, "rock");
    memset(&m, 0, sizeof(m));
    vgeo_gen_torus(&m, 4, 1.2f, 0.35f, 160, 50, c0, 0.2f);
    add_mesh(&m, "torus");
    memset(&m, 0, sizeof(m));
    vgeo_gen_statue(&m, 5, 60000);
    add_mesh(&m, "statue");
    VgCullCaps caps = { 1u << 20, 1u << 19, 1u << 21, 1u << 20, 4096 };
    if (!vg_cull_init(&g_cull, &g_h.ctx, &g_scene, &caps)) { printf("FAIL: cull init\n"); return 1; }
    if (!vg_raster_init(&g_rast, &g_h.ctx, &g_cull)) { printf("FAIL: raster init\n"); return 1; }
    g_rast.serialize = true;
    vg_raster_resize(&g_rast, W, H);
    // The second rasteriser on the 32-bit writes (when the device has 64-bit atomics the first one
    // takes them, so the two are compared; DG_VGEO_VIS32=1 puts both on 32-bit writes).
    VgContext c32 = g_h.ctx;
    c32.vis32 = true;
    if (vg_raster_init(&g_rast32, &c32, &g_cull)) {
        g_rast32.serialize = true;
        vg_raster_resize(&g_rast32, W, H);
    }
    place_instances(20, 12.0f);
    printf("scene: %u meshes, %u instances, mesh shader path %s\n", g_scene.mesh_count, g_scene.instance_count, g_rast.p_mesh ? "yes" : "no");
    test_rasterisers();
    test_two_pass();
    test_vis32();
    vg_raster_shutdown(&g_rast32);
    vg_raster_shutdown(&g_rast);
    vg_cull_shutdown(&g_cull);
    vgeo_scene_free(&g_scene);
    vgh_shutdown(&g_h);
    if (validation) CHECK(g_vgh_vk_messages == 0, "%d validation messages", g_vgh_vk_messages);
    printf("vgeo_raster: %d/%d checks passed (%.1f s)\n", g_checks - g_fail, g_checks,
           (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency());
    return g_fail ? 1 : 0;
}
