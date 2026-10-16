// Virtualized geometry GPU culling tests (src/render/vgeo/vgeo_cull.c, shaders/vgeo, docs/VGEO.md
// "Culling"), on the window-less harness:
//   - LOD cut: for 8 cameras the GPU selected set equals the CPU reference (vgeo_scene_cpu_select);
//     only clusters within 1e-4 of the threshold may differ
//   - no cluster is drawn together with one of its DAG ancestors (walking child_group chains)
//   - frustum: no cluster inside the frustum is lost, none outside is kept; instance count vs brute force
//   - occlusion: Hi-Z culling is conservative (nothing in front of / beside a wall is culled) and
//     effective (most clusters behind it are), and pass 2 against an empty Hi-Z restores the full set
//   - Hi-Z MAX pyramid: every texel bounds its whole footprint (odd sizes)
//   - CPU cost: the dispatch count does not depend on the instance count (100x more instances)
//   - --validation: zero validation messages
#include "vgeo_gpu_harness.h"
#include "render/vgeo/vgeo_procgen.h"
#include "core/engine.h"
#include "core/dgmath.h"
#include <SDL3/SDL.h>
#include <float.h>
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

static VgHarness g_h;
static VgScene g_scene;
static VgCuller g_cull;

typedef struct Cam { vec3 eye, target; } Cam;

static void view_for(const Cam *c, u32 flags, VgViewGpu *v) {
    mat4 view = mat4_look_at(c->eye, c->target, v3(0, 1, 0));
    mat4 proj = mat4_perspective(FOV, (float)W / (float)H, ZNEAR, 4000.0f);
    mat4 vp = mat4_mul(proj, view);
    float ps = (float)H / (2.0f * tanf(FOV * 0.5f));
    float eye[3] = { c->eye.x, c->eye.y, c->eye.z };
    vgeo_view_setup(v, vp.m, vp.m, eye, ps, ZNEAR, W, H, 1.0f, 32.0f, flags);
}

static u64 key(u32 inst, u32 packed) { return (u64)inst << 32 | packed; }
static int u64_cmp(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

// Runs one view; returns the sorted visible keys (pass 1 + pass 2, HW + SW).
static u64 *run_view(const VgViewGpu *v, const VgHizRef *prev, const VgHizRef *cur, VgCounters *ctr, u32 *count) {
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
    vg_cull_record(&g_cull, cmd, 0, 0, v, prev, cur);
    vgc_submit_wait(&g_h.ctx, pool, cmd);
    VgViewRes *r = &g_cull.v[0];
    vgh_read(&g_h, &r->counters, 0, sizeof(VgCounters), ctr);
    u32 hw = DG_MIN(ctr->n[VGN_HW], g_cull.caps.visible), sw = DG_MIN(ctr->n[VGN_SW], g_cull.caps.visible);
    u32 *raw = (u32 *)malloc(sizeof(u32) * 2 * (hw + sw + 1));
    vgh_read(&g_h, &r->visible, 0, 8ull * hw, raw);
    vgh_read(&g_h, &r->visible, 8ull * g_cull.caps.visible, 8ull * sw, raw + 2 * hw);
    u64 *keys = (u64 *)malloc(sizeof(u64) * (hw + sw + 1));
    for (u32 i = 0; i < hw + sw; i++) keys[i] = key(raw[2 * i], raw[2 * i + 1]);
    free(raw);
    qsort(keys, hw + sw, sizeof(u64), u64_cmp);
    *count = hw + sw;
    return keys;
}

static bool has(const u64 *keys, u32 n, u64 k) { return bsearch(&k, keys, n, sizeof(u64), u64_cmp) != NULL; }

// ------------------------------------------------------------------ scene

static void add_mesh(VgGenMesh *m) {
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

static const Cam k_cams[8] = {
    { { 0, 1.7f, 0 }, { 0, 1.7f, -50 } },          // inside the field, eye height
    { { 30, 4, 80 }, { 0, 0, 0 } },                // looking in from the edge
    { { -150, 40, -150 }, { 0, 0, 0 } },           // far, above
    { { 0, 300, 1 }, { 0, 0, 0 } },                // straight down, high
    { { 5, 1.0f, 5 }, { 10, 1, 9 } },              // close to instances
    { { 0, 2, 200 }, { 0, 2, 0 } },                // outside the field
    { { 60, 0.3f, -40 }, { -60, 3, 40 } },         // grazing
    { { 0.5f, 1.2f, 0.4f }, { 0.5f, 1.2f, 1.4f } },// possibly inside a mesh sphere (znear clamp)
};

// ------------------------------------------------------------------ tests

// Global group of every (page slot, cluster) for the ancestor walk.
static u32 *g_group_of;
static void build_group_map(void) {
    free(g_group_of);
    g_group_of = (u32 *)malloc(sizeof(u32) * (size_t)g_scene.page_count << VG_CLUSTER_BITS);
    memset(g_group_of, 0xFF, sizeof(u32) * ((size_t)g_scene.page_count << VG_CLUSTER_BITS));
    for (u32 m = 0; m < g_scene.mesh_count; m++) {
        const VgMesh *me = &g_scene.meshes[m];
        u32 end = m + 1 < g_scene.mesh_count ? g_scene.meshes[m + 1].group_base : g_scene.group_count;
        for (u32 g = me->group_base; g < end; g++) {
            const VgGroup *gr = &g_scene.groups[g];
            u32 slot = g_scene.page_table[me->page_base + gr->page];
            for (u32 k = 0; k < gr->cluster_count; k++) g_group_of[((size_t)slot << VG_CLUSTER_BITS) + gr->first_cluster + k] = g;
        }
    }
}

static u32 ancestor_violations(const u64 *keys, u32 n) {
    u32 bad = 0;
    u32 *stack = (u32 *)malloc(sizeof(u32) * (g_scene.group_count + 1));
    u8 *seen = (u8 *)calloc(g_scene.group_count, 1);
    for (u32 i = 0; i < n && bad < 10; i++) {
        u32 inst = (u32)(keys[i] >> 32), packed = (u32)keys[i];
        const VgMesh *me = &g_scene.meshes[g_scene.instances[inst].mesh];
        const VgCluster *c = vgeo_scene_cluster(&g_scene, packed >> VG_CLUSTER_BITS, packed & ((1u << VG_CLUSTER_BITS) - 1));
        if (c->child_group == ~0u) continue;
        u32 sp = 0, touched = 0;
        stack[sp++] = me->group_base + c->child_group;
        u32 *visited = (u32 *)malloc(sizeof(u32) * (g_scene.group_count + 1));
        while (sp) {
            u32 g = stack[--sp];
            if (seen[g]) continue;
            seen[g] = 1;
            visited[touched++] = g;
            const VgGroup *gr = &g_scene.groups[g];
            u32 slot = g_scene.page_table[me->page_base + gr->page];
            for (u32 k = 0; k < gr->cluster_count; k++) {
                u32 p = slot << VG_CLUSTER_BITS | (gr->first_cluster + k);
                if (has(keys, n, key(inst, p))) bad++;
                const VgCluster *cc = vgeo_scene_cluster(&g_scene, slot, gr->first_cluster + k);
                if (cc->child_group != ~0u) stack[sp++] = me->group_base + cc->child_group;
            }
        }
        for (u32 k = 0; k < touched; k++) seen[visited[k]] = 0;
        free(visited);
    }
    free(stack);
    free(seen);
    return bad;
}

static void test_cut_and_frustum(void) {
    u32 cap = 4u << 20;
    VgRefCluster *ref = (VgRefCluster *)malloc(sizeof(VgRefCluster) * cap);
    for (int ci = 0; ci < 8; ci++) {
        // LOD only
        VgViewGpu v;
        view_for(&k_cams[ci], VGV_NO_FRUSTUM | VGV_NO_CONE | VGV_NO_OCCLUSION, &v);
        VgCounters ctr;
        u32 n = 0;
        u64 *keys = run_view(&v, NULL, NULL, &ctr, &n);
        u32 rn = vgeo_scene_cpu_select(&g_scene, &v, ref, cap);
        CHECK(ctr.n[VGN_OVERFLOW] == 0, "cam %d: %u overflows", ci, ctr.n[VGN_OVERFLOW]);
        u32 missing = 0, border = 0;
        u64 *rk = (u64 *)malloc(sizeof(u64) * (rn + 1));
        for (u32 i = 0; i < rn; i++) {
            rk[i] = key(ref[i].instance, ref[i].packed);
            if (!has(keys, n, rk[i])) {
                if (ref[i].lod_margin < 1e-4f) border++;
                else missing++;
            }
        }
        qsort(rk, rn, sizeof(u64), u64_cmp);
        u32 extra = 0;
        for (u32 i = 0; i < n; i++) if (!has(rk, rn, keys[i])) extra++;
        CHECK(missing == 0, "cam %d: %u clusters of the CPU cut missing on the GPU (%u selected, %u reference)", ci, missing, n, rn);
        CHECK(extra <= border + rn / 10000, "cam %d: %u GPU clusters not in the CPU cut", ci, extra);
        CHECK(ancestor_violations(keys, n) == 0, "cam %d: a cluster is drawn with its ancestor", ci);
        printf("cam %d: cut %u clusters (cpu %u, %u borderline, %u extra), %u tris, nodes %u\n", ci, n, rn, border, extra, ctr.n[VGN_TRIS], ctr.n[VGN_NODES_VISITED]);
        free(keys);
        // + frustum
        view_for(&k_cams[ci], VGV_NO_CONE | VGV_NO_OCCLUSION, &v);
        keys = run_view(&v, NULL, NULL, &ctr, &n);
        u32 lost = 0, kept = 0;
        for (u32 i = 0; i < rn; i++) {
            if (ref[i].lod_margin < 1e-4f || ref[i].frustum < 0) continue;
            bool in = has(keys, n, key(ref[i].instance, ref[i].packed));
            if (ref[i].frustum == 1 && !in) lost++;
            if (ref[i].frustum == 0 && in) kept++;
        }
        CHECK(lost == 0, "cam %d: %u clusters inside the frustum lost", ci, lost);
        CHECK(kept == 0, "cam %d: %u clusters outside the frustum kept", ci, kept);
        // instances vs brute force (mesh sphere vs planes, borderline excluded)
        u32 inside = 0, band = 0;
        for (u32 i = 0; i < g_scene.instance_count; i++) {
            const VgInstance *in = &g_scene.instances[i];
            const VgMesh *me = &g_scene.meshes[in->mesh];
            float p[3], q[4], s;
            vgeo_instance_decode(in, p, q, &s);
            quat qq = { q[0], q[1], q[2], q[3] };
            vec3 c = v3_add(v3(p[0], p[1], p[2]), quat_rotate(qq, v3(me->center[0] * s, me->center[1] * s, me->center[2] * s)));
            float r = me->radius * s;
            int st = 1;
            for (int k = 0; k < 6; k++) {
                float d = v.planes[k][0] * c.x + v.planes[k][1] * c.y + v.planes[k][2] * c.z + v.planes[k][3];
                if (d < -r - 1e-3f) { st = 0; break; }
                if (d < -r + 1e-3f) st = -1;
            }
            if (st == 1) inside++;
            if (st == -1) band++;
        }
        CHECK(ctr.n[VGN_INST_VIS] >= inside && ctr.n[VGN_INST_VIS] <= inside + band, "cam %d: %u instances visible, brute force %u (+%u borderline)", ci,
              ctr.n[VGN_INST_VIS], inside, band);
        free(keys);
        free(rk);
    }
    free(ref);
}

// Screen rect / nearest depth of a sphere's box corners (the shader's projection).
static bool sphere_rect(const VgViewGpu *v, vec3 c, float r, float *x1, float *zmin) {
    mat4 vp;
    memcpy(vp.m, v->view_proj, 64);
    float mx = -1e30f, mn = 1e30f;
    for (int i = 0; i < 8; i++) {
        vec3 p = v3(c.x + ((i & 1) ? r : -r), c.y + ((i & 2) ? r : -r), c.z + ((i & 4) ? r : -r));
        vec4 h = mat4_mul_v4(vp, v4(p.x, p.y, p.z, 1));
        if (h.w <= 1e-4f) return false;
        mx = fmaxf(mx, h.x / h.w);
        mn = fminf(mn, h.z / h.w);
    }
    *x1 = (mx * 0.5f + 0.5f) * (float)W;
    *zmin = mn;
    return true;
}

static void test_occlusion(VgHizImage *wall, VgHizImage *empty) {
    // Camera at z = 60 looking down -z; a wall at distance 30 over the left half of the screen.
    Cam cam = { { 0, 2, 60 }, { 0, 2, 0 } };
    VgViewGpu v;
    view_for(&cam, VGV_NO_CONE, &v);
    mat4 proj = mat4_perspective(FOV, (float)W / (float)H, ZNEAR, 4000.0f);
    float D = 30.0f;
    float wall_z = (proj.m[10] * -D + proj.m[14]) / D;
    float *depth = (float *)malloc(sizeof(float) * W * H);
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) depth[y * W + x] = x < W / 2 ? wall_z : 1.0f;
    vgh_hiz_build(&g_h, &g_cull, wall, depth);
    for (u32 i = 0; i < W * H; i++) depth[i] = 1.0f;
    vgh_hiz_build(&g_h, &g_cull, empty, depth);
    free(depth);
    VgHizRef wr = vgh_hiz_ref(wall), er = vgh_hiz_ref(empty);

    VgCounters c0, c1, c2;
    u32 n0, n1, n2;
    u64 *all = run_view(&v, NULL, NULL, &c0, &n0);
    u64 *p1 = run_view(&v, &wr, NULL, &c1, &n1);
    u32 must = 0, must_lost = 0, behind = 0, behind_kept = 0;
    for (u32 i = 0; i < n0; i++) {
        u32 inst = (u32)(all[i] >> 32), packed = (u32)all[i];
        const VgInstance *in = &g_scene.instances[inst];
        const VgCluster *cl = vgeo_scene_cluster(&g_scene, packed >> VG_CLUSTER_BITS, packed & ((1u << VG_CLUSTER_BITS) - 1));
        float p[3], q[4], s;
        vgeo_instance_decode(in, p, q, &s);
        quat qq = { q[0], q[1], q[2], q[3] };
        vec3 c = v3_add(v3(p[0], p[1], p[2]), quat_rotate(qq, v3(cl->center[0] * s, cl->center[1] * s, cl->center[2] * s)));
        float r = cl->radius * s, x1, zmin;
        bool kept = has(p1, n1, all[i]);
        if (!sphere_rect(&v, c, r, &x1, &zmin) || x1 + 2.0f >= (float)(W / 2) || zmin <= wall_z * 0.9999f) {
            must++;
            if (!kept) must_lost++;
        } else if (zmin > wall_z * 1.0001f && x1 < (float)(W / 2) - 64.0f) {
            behind++;
            if (kept) behind_kept++;
        }
    }
    CHECK(must_lost == 0, "occlusion: %u of %u visible clusters culled", must_lost, must);
    CHECK(behind > 100 && behind_kept * 2 < behind, "occlusion: %u of %u clusters behind the wall kept", behind_kept, behind);
    printf("occlusion: %u clusters without, %u with the wall (%u must stay, %u behind, %u of those kept); occluded: %u cells, %u instances, %u nodes, %u clusters\n",
           n0, n1, must, behind, behind_kept, c1.n[VGN_CELLS_OCC], c1.n[VGN_INST_OCC], c1.n[VGN_NODES_OCC], c1.n[VGN_CL_OCC]);
    // Pass 2 against an empty Hi-Z brings every occluded item back: the union equals the full set.
    view_for(&cam, VGV_NO_CONE | VGV_TWO_PASS, &v);
    u64 *p2 = run_view(&v, &wr, &er, &c2, &n2);
    u32 diff = n2 > n0 ? n2 - n0 : n0 - n2, absent = 0;
    for (u32 i = 0; i < n0; i++) if (!has(p2, n2, all[i])) absent++;
    CHECK(diff == 0 && absent == 0, "two-pass: %u clusters vs %u without occlusion (%u absent)", n2, n0, absent);
    CHECK(c2.n[VGN_HW_P1] + c2.n[VGN_SW_P1] == n1, "two-pass: pass 1 end %u vs %u", c2.n[VGN_HW_P1] + c2.n[VGN_SW_P1], n1);
    printf("two-pass: pass 1 %u + pass 2 %u (instances %u, re-tested clusters %u)\n", c2.n[VGN_HW_P1] + c2.n[VGN_SW_P1], n2 - c2.n[VGN_HW_P1] - c2.n[VGN_SW_P1],
           c2.n[VGN_INST_P2], c2.n[VGN_CL_P2]);
    free(all);
    free(p1);
    free(p2);
}

static void test_hiz(void) {
    u32 w = 333, h = 217;
    VgHizImage z;
    CHECK(vgh_hiz_create(&g_h, &z, w, h), "hiz create");
    float *d = (float *)malloc(sizeof(float) * w * h);
    g_rng = 7;
    for (u32 i = 0; i < w * h; i++) d[i] = rnd();
    vgh_hiz_build(&g_h, &g_cull, &z, d);
    u32 bad = 0;
    float *m = (float *)malloc(sizeof(float) * w * h);
    u32 pw, ph, pm;
    vg_hiz_size(w, h, &pw, &ph, &pm);
    CHECK(pm == z.mips, "hiz mips %u vs %u", pm, z.mips);
    float *m2 = (float *)malloc(sizeof(float) * pw * ph);
    for (u32 mip = 0; mip < z.mips; mip++) {
        vgh_hiz_read(&g_h, &z, mip, m2);
        u32 mw = DG_MAX(1u, pw >> mip), mh = DG_MAX(1u, ph >> mip), sh = mip + 1;   // texel covers 2^(mip+1) pixels
        for (u32 y = 0; y < mh; y++)
            for (u32 x = 0; x < mw; x++) {
                // exactly the max of its footprint (pixels off the screen count as 0)
                u32 x0 = x << sh, y0 = y << sh, x1 = DG_MIN(w, (x + 1) << sh), y1 = DG_MIN(h, (y + 1) << sh);
                float mx = 0;
                for (u32 yy = y0; yy < y1; yy++)
                    for (u32 xx = x0; xx < x1; xx++) mx = fmaxf(mx, d[yy * w + xx]);
                if (m2[y * mw + x] != mx) bad++;
            }
    }
    free(m2);
    CHECK(bad == 0, "hiz: %u texels are not the max of their footprint", bad);
    free(m);
    free(d);
    vgh_hiz_destroy(&g_h, &z);
}

static void test_cpu_cost(void) {
    Cam cam = k_cams[1];
    VgViewGpu v;
    double t[2];
    u32 disp[2];
    u32 sides[2] = { 10, 100 };
    for (int k = 0; k < 2; k++) {
        place_instances(sides[k], 12.0f);
        view_for(&cam, 0, &v);
        VgCounters ctr;
        u32 n;
        free(run_view(&v, NULL, NULL, &ctr, &n));   // warm (descriptor sets, buffers)
        VkCommandPool pool;
        VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
        u64 a = SDL_GetPerformanceCounter();
        for (int r = 0; r < 20; r++) vg_cull_record(&g_cull, cmd, 0, 0, &v, NULL, NULL);
        u64 b = SDL_GetPerformanceCounter();
        vgc_submit_wait(&g_h.ctx, pool, cmd);
        t[k] = (double)(b - a) / (double)SDL_GetPerformanceFrequency() * 1000.0 / 20.0;
        disp[k] = g_cull.dispatches;
        printf("cpu: %u instances: %u dispatches, record %.3f ms\n", g_scene.instance_count, disp[k], t[k]);
    }
    CHECK(disp[0] == disp[1], "dispatch count depends on the instance count (%u vs %u)", disp[0], disp[1]);
    CHECK(t[1] < t[0] * 3.0 + 0.2, "record time grows with the instance count (%.3f vs %.3f ms)", t[0], t[1]);
}

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++) validation |= !strcmp(argv[i], "--validation");
    if (!vgh_init(&g_h, validation)) { printf("vgeo_gpu: no device, skipped\n"); return 0; }
    vgeo_scene_init(&g_scene, &g_h.ctx);
    VgGenMesh m;
    float c0[3] = { 0, 0, 0 };
    memset(&m, 0, sizeof(m));
    vgeo_gen_rock(&m, 3, 30, 1.0f, c0, 0.3f);
    add_mesh(&m);
    memset(&m, 0, sizeof(m));
    vgeo_gen_torus(&m, 4, 1.2f, 0.35f, 160, 50, c0, 0.2f);
    add_mesh(&m);
    memset(&m, 0, sizeof(m));
    vgeo_gen_statue(&m, 5, 60000);
    add_mesh(&m);
    VgCullCaps caps = { 1u << 20, 1u << 19, 1u << 21, 1u << 20, 4096 };
    if (!vg_cull_init(&g_cull, &g_h.ctx, &g_scene, &caps)) { printf("FAIL: cull init\n"); return 1; }
    place_instances(20, 12.0f);
    build_group_map();
    printf("scene: %u meshes, %u instances, %u cells, %u pages, max BVH depth %u\n", g_scene.mesh_count, g_scene.instance_count, g_scene.cell_count,
           g_scene.page_count, g_scene.max_depth);
    test_cut_and_frustum();
    VgHizImage wall, empty;
    vgh_hiz_create(&g_h, &wall, W, H);
    vgh_hiz_create(&g_h, &empty, W, H);
    test_occlusion(&wall, &empty);
    vgh_hiz_destroy(&g_h, &wall);
    vgh_hiz_destroy(&g_h, &empty);
    test_hiz();
    test_cpu_cost();
    vg_cull_shutdown(&g_cull);
    vgeo_scene_free(&g_scene);
    free(g_group_of);
    vgh_shutdown(&g_h);
    if (validation) CHECK(g_vgh_vk_messages == 0, "%d validation messages", g_vgh_vk_messages);
    printf("vgeo_gpu: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
