// Virtualized geometry page streaming tests (src/render/vgeo/vgeo_stream.c, docs/VGEO.md
// "Streaming"), on the window-less harness. Scene: a rock, a torus, a statue and a rock read back
// from a .vgeo file (file source, worker-thread reads), 400 instances. Frames are recorded as the
// renderer does (update, then the cull, frame slot = frame % frames in flight), one submit each.
//   - cold start: only root pages resident; frame 1 selects only resident clusters, the GPU set
//     equals the residency-aware CPU cut and every instance draws (all in LOD-only, every instance
//     inside the frustum with culling)
//   - convergence: with a pool that fits, a fixed camera converges to the all-resident CPU cut
//   - small pool: detail degrades only (every instance draws, no cluster with an ancestor, the
//     selection of closed meshes stays watertight, residency closed under page dependencies)
//   - eviction audit over a 200 frame flythrough with a tiny pool: every visible cluster's slot is
//     mapped, pages evicted in a frame are not in its visible list, no slot is overwritten while a
//     frame that may still be in flight references it, and the GPU pool holds the CPU page data
//   - --validation: zero validation messages
#include "vgeo_gpu_harness.h"
#include "render/vgeo/vgeo_procgen.h"
#include "render/vgeo/vgeo_stream.h"
#include "core/engine.h"
#include "core/dgmath.h"
#include "core/platform.h"
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
#define NMESH 4
#define LOD_ONLY (VGV_NO_FRUSTUM | VGV_NO_CONE | VGV_NO_OCCLUSION)

static VgHarness g_h;
static VgScene g_scene;
static VgCuller g_cull;
static VgStream g_st;
static VgBuilt g_built[NMESH];
static bool g_closed[NMESH] = { true, true, false, true };
static u32 g_frame;

typedef struct Cam { vec3 eye, target; } Cam;

static void view_for(const Cam *c, u32 flags, VgViewGpu *v) {
    mat4 view = mat4_look_at(c->eye, c->target, v3(0, 1, 0));
    mat4 proj = mat4_perspective(FOV, (float)W / (float)H, ZNEAR, 4000.0f);
    mat4 vp = mat4_mul(proj, view);
    float ps = (float)H / (2.0f * tanf(FOV * 0.5f));
    float eye[3] = { c->eye.x, c->eye.y, c->eye.z };
    vgeo_view_setup(v, vp.m, vp.m, eye, ps, ZNEAR, W, H, 1.0f, 32.0f, flags);
}

// Selection keys independent of the slot: instance, global page, cluster.
static u64 key(u32 inst, u32 page, u32 cl) { return (u64)inst << 32 | (u64)page << VG_CLUSTER_BITS | cl; }
static int u64_cmp(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}
static bool has(const u64 *keys, u32 n, u64 k) { return bsearch(&k, keys, n, sizeof(u64), u64_cmp) != NULL; }

typedef struct Frame {
    u64 *keys;                   // sorted
    u32 n, unmapped;             // entries whose slot was free at the cull
    u32 *slots;                  // raw slot of every entry (unsorted)
    VgCounters ctr;
} Frame;

static void frame_free(Frame *f) { free(f->keys); free(f->slots); memset(f, 0, sizeof(*f)); }

// One frame: stream update + cull in one submit; the visible list mapped through the slot table
// the cull ran with.
static Frame run_frame(const VgViewGpu *v) {
    Frame f;
    memset(&f, 0, sizeof(f));
    u32 slot = g_frame % g_h.ctx.frames;
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
    vg_stream_update(&g_st, cmd, slot);
    vg_cull_record(&g_cull, cmd, slot, 0, v, NULL, NULL);
    vgc_submit_wait(&g_h.ctx, pool, cmd);
    g_frame++;
    VgViewRes *r = &g_cull.v[0];
    vgh_read(&g_h, &r->counters, 0, sizeof(VgCounters), &f.ctr);
    u32 hw = DG_MIN(f.ctr.n[VGN_HW], g_cull.caps.visible), sw = DG_MIN(f.ctr.n[VGN_SW], g_cull.caps.visible);
    u32 *raw = (u32 *)malloc(sizeof(u32) * 2 * (hw + sw + 1));
    vgh_read(&g_h, &r->visible, 0, 8ull * hw, raw);
    vgh_read(&g_h, &r->visible, 8ull * g_cull.caps.visible, 8ull * sw, raw + 2 * hw);
    f.keys = (u64 *)malloc(sizeof(u64) * (hw + sw + 1));
    f.slots = (u32 *)malloc(sizeof(u32) * (hw + sw + 1));
    for (u32 i = 0; i < hw + sw; i++) {
        u32 s = raw[2 * i + 1] >> VG_CLUSTER_BITS, cl = raw[2 * i + 1] & ((1u << VG_CLUSTER_BITS) - 1);
        u32 page = s < g_scene.pool_slots ? g_scene.slot_page[s] : ~0u;
        if (page == ~0u) f.unmapped++;
        f.slots[i] = s;
        f.keys[i] = key(raw[2 * i], page == ~0u ? 0xFFFFFu : page, cl);
    }
    free(raw);
    f.n = hw + sw;
    qsort(f.keys, f.n, sizeof(u64), u64_cmp);
    return f;
}

// The CPU cut as keys (residency aware, or as if every page were resident).
static u64 *cpu_keys(const VgViewGpu *v, bool all, u32 *n, u32 *border, VgRefCluster **ref_out) {
    u32 cap = 4u << 20;
    VgRefCluster *ref = (VgRefCluster *)malloc(sizeof(VgRefCluster) * cap);
    u32 rn = vgeo_scene_cpu_select_ex(&g_scene, v, all, ref, cap);
    u64 *k = (u64 *)malloc(sizeof(u64) * (rn + 1));
    *border = 0;
    for (u32 i = 0; i < rn; i++) {
        k[i] = key(ref[i].instance, ref[i].page, ref[i].packed & ((1u << VG_CLUSTER_BITS) - 1));
        if (ref[i].lod_margin < 1e-4f) (*border)++;
    }
    qsort(k, rn, sizeof(u64), u64_cmp);
    *n = rn;
    if (ref_out) *ref_out = ref;
    else free(ref);
    return k;
}

// GPU set vs a CPU cut: CPU entries missing (not borderline) and GPU extras.
static void compare(const Frame *f, const VgViewGpu *v, bool all, u32 *missing, u32 *extra, u32 *border) {
    VgRefCluster *ref;
    u32 rn;
    u64 *rk = cpu_keys(v, all, &rn, border, &ref);
    *missing = *extra = 0;
    for (u32 i = 0; i < rn; i++) {
        u64 k = key(ref[i].instance, ref[i].page, ref[i].packed & ((1u << VG_CLUSTER_BITS) - 1));
        if (!has(f->keys, f->n, k) && ref[i].lod_margin >= 1e-4f) (*missing)++;
    }
    for (u32 i = 0; i < f->n; i++) if (!has(rk, rn, f->keys[i])) (*extra)++;
    free(rk);
    free(ref);
}

// ------------------------------------------------------------------ scene

static void build_mesh(VgGenMesh *m, VgBuilt *b) {
    VgSource src;
    memset(&src, 0, sizeof(src));
    src.vertex_count = m->vertex_count;
    src.pos = m->pos;
    src.index_count = m->index_count;
    src.indices = m->indices;
    VgBuildOptions opt;
    memset(&opt, 0, sizeof(opt));
    char err[256];
    if (!vgeo_build(&src, &opt, b, err, sizeof(err))) { printf("build failed: %s\n", err); exit(1); }
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
            // 5 x 5 patches of one mesh: a moving camera changes which pages are needed
            vgeo_scene_add_instance(&g_scene, (x / 5 + (z / 5) * 3) % g_scene.mesh_count, p, r, 0.5f + rnd() * 2.5f);
        }
}

// Fresh residency (root pages only) with a pool of `slots`, and a fresh streamer.
static void restart(u32 slots, u32 ppf) {
    if (g_st.ok) vg_stream_shutdown(&g_st);
    VgStreamConfig cfg = { 1, ppf };
    vg_stream_init(&g_st, &g_scene, &g_cull, &cfg);
    vgeo_scene_set_pool(&g_scene, slots);   // exact slot counts (the config is in MB)
    vgeo_scene_commit(&g_scene);
}

// ------------------------------------------------------------------ checks

static u32 *g_group_of;   // (global page << 12 | cluster) -> global group
static void build_group_map(void) {
    g_group_of = (u32 *)malloc(sizeof(u32) * ((size_t)g_scene.page_count << VG_CLUSTER_BITS));
    memset(g_group_of, 0xFF, sizeof(u32) * ((size_t)g_scene.page_count << VG_CLUSTER_BITS));
    for (u32 m = 0; m < g_scene.mesh_count; m++) {
        const VgMesh *me = &g_scene.meshes[m];
        u32 end = m + 1 < g_scene.mesh_count ? g_scene.meshes[m + 1].group_base : g_scene.group_count;
        for (u32 g = me->group_base; g < end; g++) {
            const VgGroup *gr = &g_scene.groups[g];
            for (u32 k = 0; k < gr->cluster_count; k++) g_group_of[((size_t)(me->page_base + gr->page) << VG_CLUSTER_BITS) + gr->first_cluster + k] = g;
        }
    }
}

// Selected clusters drawn together with one of their DAG descendants (page-independent walk).
static u32 ancestor_violations(const Frame *f) {
    u32 bad = 0;
    u32 *stack = (u32 *)malloc(sizeof(u32) * (g_scene.group_count + 1));
    u8 *seen = (u8 *)calloc(g_scene.group_count, 1);
    u32 *visited = (u32 *)malloc(sizeof(u32) * (g_scene.group_count + 1));
    for (u32 i = 0; i < f->n && bad < 10; i++) {
        u32 inst = (u32)(f->keys[i] >> 32), page = (u32)(f->keys[i] >> VG_CLUSTER_BITS) & 0xFFFFFu, cl = (u32)f->keys[i] & ((1u << VG_CLUSTER_BITS) - 1);
        if (page >= g_scene.page_count) continue;
        const VgMesh *me = &g_scene.meshes[g_scene.instances[inst].mesh];
        const VgCluster *c = vgeo_scene_page_cluster(&g_scene, page, cl);
        if (c->child_group == ~0u) continue;
        u32 sp = 0, touched = 0;
        stack[sp++] = me->group_base + c->child_group;
        while (sp) {
            u32 g = stack[--sp];
            if (seen[g]) continue;
            seen[g] = 1;
            visited[touched++] = g;
            const VgGroup *gr = &g_scene.groups[g];
            u32 gp = me->page_base + gr->page;
            for (u32 k = 0; k < gr->cluster_count; k++) {
                if (has(f->keys, f->n, key(inst, gp, gr->first_cluster + k))) bad++;
                const VgCluster *cc = vgeo_scene_page_cluster(&g_scene, gp, gr->first_cluster + k);
                if (cc->child_group != ~0u) stack[sp++] = me->group_base + cc->child_group;
            }
        }
        for (u32 k = 0; k < touched; k++) seen[visited[k]] = 0;
    }
    free(visited);
    free(stack);
    free(seen);
    return bad;
}

// Instances with at least one selected cluster.
static u32 instances_drawn(const Frame *f, u8 *drawn) {
    memset(drawn, 0, g_scene.instance_count);
    u32 n = 0;
    for (u32 i = 0; i < f->n; i++) {
        u32 inst = (u32)(f->keys[i] >> 32);
        if (!drawn[inst]) n++;
        drawn[inst] = 1;
    }
    return n;
}

// Instance sphere completely inside the frustum (all its clusters are).
static bool fully_inside(const VgViewGpu *v, u32 i) {
    const VgInstance *in = &g_scene.instances[i];
    const VgMesh *me = &g_scene.meshes[in->mesh];
    float p[3], q[4], s;
    vgeo_instance_decode(in, p, q, &s);
    quat qq = { q[0], q[1], q[2], q[3] };
    vec3 c = v3_add(v3(p[0], p[1], p[2]), quat_rotate(qq, v3(me->center[0] * s, me->center[1] * s, me->center[2] * s)));
    float r = me->radius * s;
    for (int k = 0; k < 6; k++)
        if (v->planes[k][0] * c.x + v->planes[k][1] * c.y + v->planes[k][2] * c.z + v->planes[k][3] < r + 1e-3f) return false;
    return true;
}

typedef struct Key3 { i32 v[3]; } Key3;
typedef struct EdgeKey { Key3 a, b; int dir; } EdgeKey;
static int key3_cmp(const Key3 *a, const Key3 *b) {
    for (int i = 0; i < 3; i++)
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return 0;
}
static int edge_qcmp(const void *a, const void *b) {
    const EdgeKey *x = (const EdgeKey *)a, *y = (const EdgeKey *)b;
    int c = key3_cmp(&x->a, &y->a);
    return c ? c : key3_cmp(&x->b, &y->b);
}
static EdgeKey edge_of(const i32 *p, const i32 *q) {
    EdgeKey e;
    Key3 a = { { p[0], p[1], p[2] } }, b = { { q[0], q[1], q[2] } };
    if (key3_cmp(&a, &b) <= 0) { e.a = a; e.b = b; e.dir = 0; } else { e.a = b; e.b = a; e.dir = 1; }
    return e;
}

// Watertightness of one instance's selected clusters (decoded on the mesh grid): every directed
// edge is matched by its reverse; balanced 4-use (pinched) edges are allowed at <= 0.1 %.
static bool instance_closed(const Frame *f, u32 inst, size_t *open_out, size_t *pinch_out, size_t *tris_out) {
    static VgDecoded dc;
    u32 mesh = g_scene.instances[inst].mesh;
    const VgMesh *me = &g_scene.meshes[mesh];
    EdgeKey *edges = NULL;
    size_t ne = 0, ecap = 0, tris = 0;
    u64 lo = key(inst, 0, 0), hi = key(inst + 1, 0, 0);
    for (u32 i = 0; i < f->n; i++) {
        if (f->keys[i] < lo || f->keys[i] >= hi) continue;
        u32 page = (u32)(f->keys[i] >> VG_CLUSTER_BITS) & 0xFFFFFu, cl = (u32)f->keys[i] & ((1u << VG_CLUSTER_BITS) - 1);
        vgeo_decode_cluster(&g_built[mesh], page - me->page_base, cl, &dc);
        tris += dc.tri_count;
        if (ne + dc.tri_count * 3 > ecap) {
            while (ne + dc.tri_count * 3 > ecap) ecap = ecap ? ecap * 2 : 65536;
            edges = (EdgeKey *)realloc(edges, sizeof(EdgeKey) * ecap);
        }
        for (u32 k = 0; k < dc.tri_count; k++)
            for (int e = 0; e < 3; e++) edges[ne++] = edge_of(dc.grid[dc.tri[k][e]], dc.grid[dc.tri[k][(e + 1) % 3]]);
    }
    qsort(edges, ne, sizeof(EdgeKey), edge_qcmp);
    size_t open = 0, pinch = 0, ue = 0;
    for (size_t i = 0; i < ne;) {
        size_t j = i, fwd = 0;
        while (j < ne && !edge_qcmp(&edges[i], &edges[j])) fwd += edges[j++].dir == 0;
        if (fwd * 2 != j - i) open++;
        else if (j - i != 2) pinch++;
        ue++;
        i = j;
    }
    free(edges);
    *open_out = open;
    *pinch_out = pinch;
    *tris_out = tris;
    return open == 0 && pinch * 1000 <= ue && tris > 0;
}

// Residency closed under the page dependencies (a resident page's parent pages are resident).
static u32 closure_violations(void) {
    u32 bad = 0;
    for (u32 p = 0; p < g_scene.page_count; p++) {
        if (g_scene.page_table[p] == ~0u) continue;
        for (u32 k = g_st.parent_first[p]; k < g_st.parent_first[p + 1]; k++) bad += g_scene.page_table[g_st.parents[k]] == ~0u;
    }
    return bad;
}

static const Cam k_cams[4] = {
    { { 0, 1.7f, 0 }, { 0, 1.7f, -50 } },
    { { 30, 4, 80 }, { 0, 0, 0 } },
    { { 5, 1.0f, 5 }, { 10, 1, 9 } },
    { { -150, 40, -150 }, { 0, 0, 0 } },
};

// ------------------------------------------------------------------ tests

static void test_cold_start(void) {
    restart(g_scene.page_count, 64);
    u8 *drawn = (u8 *)malloc(g_scene.instance_count);
    for (int ci = 0; ci < 2; ci++) {
        if (ci) restart(g_scene.page_count, 64);
        VgViewGpu v;
        view_for(&k_cams[ci], LOD_ONLY, &v);
        Frame f = run_frame(&v);
        u32 nonroot = 0;
        for (u32 i = 0; i < f.n; i++) {
            u32 page = (u32)(f.keys[i] >> VG_CLUSTER_BITS) & 0xFFFFFu;
            if (page >= g_scene.page_count || !g_scene.page_root[page]) nonroot++;
        }
        CHECK(f.unmapped == 0 && nonroot == 0, "cold start cam %d: %u unmapped / %u non-root clusters", ci, f.unmapped, nonroot);
        u32 missing, extra, border;
        compare(&f, &v, false, &missing, &extra, &border);
        CHECK(missing == 0 && extra <= border, "cold start cam %d: vs the resident CPU cut %u missing, %u extra (%u borderline)", ci, missing, extra, border);
        u32 nd = instances_drawn(&f, drawn);
        CHECK(nd == g_scene.instance_count, "cold start cam %d: %u of %u instances drawn (LOD only)", ci, nd, g_scene.instance_count);
        CHECK(ancestor_violations(&f) == 0, "cold start cam %d: a cluster drawn with its ancestor", ci);
        printf("cold start cam %d: %u clusters from %u root pages, %u page requests\n", ci, f.n, g_scene.root_count, f.ctr.n[VGN_PAGE_REQ]);
        frame_free(&f);
        // With culling: every instance inside the frustum draws.
        restart(g_scene.page_count, 64);
        view_for(&k_cams[ci], VGV_NO_OCCLUSION, &v);
        f = run_frame(&v);
        instances_drawn(&f, drawn);
        u32 inside = 0, lost = 0;
        for (u32 i = 0; i < g_scene.instance_count; i++)
            if (fully_inside(&v, i)) { inside++; lost += !drawn[i]; }
        CHECK(lost == 0 && f.unmapped == 0, "cold start cam %d: %u of %u instances inside the frustum not drawn", ci, lost, inside);
        frame_free(&f);
    }
    free(drawn);
}

static void test_convergence(void) {
    for (int ci = 0; ci < 3; ci++) {
        restart(g_scene.page_count, 16);
        VgViewGpu v;
        view_for(&k_cams[ci], LOD_ONLY, &v);
        u32 quiet = 0, frames = 0, loads = 0;
        Frame f;
        memset(&f, 0, sizeof(f));
        for (; frames < 400 && quiet < 2 * g_h.ctx.frames + 3; frames++) {
            frame_free(&f);
            f = run_frame(&v);
            const VgStreamStats *st = vg_stream_stats(&g_st);
            loads += st->loaded;
            quiet = st->requested == 0 && st->loaded == 0 && st->pending == 0 && f.ctr.n[VGN_PAGE_REQ] == 0 ? quiet + 1 : 0;
        }
        u32 missing, extra, border;
        compare(&f, &v, true, &missing, &extra, &border);
        CHECK(quiet > 0, "convergence cam %d: still streaming after %u frames", ci, frames);
        CHECK(missing == 0 && extra <= border, "convergence cam %d: vs the all-resident CPU cut %u missing, %u extra (%u borderline)", ci, missing, extra, border);
        CHECK(f.unmapped == 0 && ancestor_violations(&f) == 0, "convergence cam %d: unmapped / ancestor", ci);
        CHECK(closure_violations() == 0, "convergence cam %d: residency not closed", ci);
        printf("convergence cam %d: %u clusters after %u frames, %u pages loaded (%u resident of %u)\n", ci, f.n, frames, loads, vg_stream_stats(&g_st)->resident,
               g_scene.page_count);
        frame_free(&f);
    }
}

static void test_small_pool(void) {
    u8 *drawn = (u8 *)malloc(g_scene.instance_count);
    u32 extra_slots = (g_scene.page_count - g_scene.root_count) / 4;
    restart(g_scene.root_count + extra_slots, 8);
    VgViewGpu v;
    u32 bad_draw = 0, bad_anc = 0, bad_closed = 0, bad_closure = 0, unmapped = 0, checked = 0;
    size_t worst_open = 0;
    Frame f;
    memset(&f, 0, sizeof(f));
    for (u32 fr = 0; fr < 60; fr++) {
        // a new camera every 15 frames: the old one's pages age out and are evicted
        view_for(&k_cams[(fr / 15) % 4], LOD_ONLY, &v);
        frame_free(&f);
        f = run_frame(&v);
        unmapped += f.unmapped;
        bad_draw += instances_drawn(&f, drawn) != g_scene.instance_count;
        bad_closure += closure_violations();
        if (fr % 6 == 5 || fr == 59) {
            bad_anc += ancestor_violations(&f) != 0;
            for (u32 i = 0; i < g_scene.instance_count; i += 7) {
                if (!g_closed[g_scene.instances[i].mesh]) continue;
                size_t open, pinch, tris;
                if (!instance_closed(&f, i, &open, &pinch, &tris)) bad_closed++;
                if (open > worst_open) worst_open = open;
                checked++;
            }
        }
    }
    const VgStreamStats *st = vg_stream_stats(&g_st);
    u32 missing, extra, border;
    compare(&f, &v, false, &missing, &extra, &border);
    CHECK(unmapped == 0, "small pool: %u visible clusters in free slots", unmapped);
    CHECK(bad_draw == 0, "small pool: %u frames lost an instance", bad_draw);
    CHECK(bad_anc == 0, "small pool: %u frames drew a cluster with its ancestor", bad_anc);
    CHECK(bad_closed == 0, "small pool: %u of %u instance selections not watertight (worst %zu open edges)", bad_closed, checked, worst_open);
    CHECK(bad_closure == 0, "small pool: residency not closed (%u)", bad_closure);
    CHECK(missing == 0 && extra <= border, "small pool: vs the resident CPU cut %u missing, %u extra", missing, extra);
    CHECK(st->evicted_total > 0, "small pool: nothing evicted (pool %u slots)", g_scene.pool_slots);
    u32 an, ab;
    u64 *all = cpu_keys(&v, true, &an, &ab, NULL);
    printf("small pool: %u slots (%u root) of %u pages: %u clusters vs %u all-resident, %llu loaded / %llu evicted, %u instance cuts watertight\n",
           g_scene.pool_slots, g_scene.root_count, g_scene.page_count, f.n, an, (unsigned long long)st->loaded_total, (unsigned long long)st->evicted_total,
           checked - bad_closed);
    free(all);
    frame_free(&f);
    free(drawn);
}

static void test_flythrough(void) {
    u32 extra_slots = DG_MAX(4u, (g_scene.page_count - g_scene.root_count) / 6);
    restart(g_scene.root_count + extra_slots, 4);
    u32 slots = g_scene.pool_slots, F = g_h.ctx.frames;
    u32 *prev_map = (u32 *)malloc(sizeof(u32) * slots);
    u32 *last_ref = (u32 *)malloc(sizeof(u32) * slots);   // last frame a visible entry used the slot (+1)
    memcpy(prev_map, g_scene.slot_page, sizeof(u32) * slots);
    memset(last_ref, 0, sizeof(u32) * slots);
    u32 unmapped = 0, evicted_visible = 0, unsafe_reuse = 0, evicted_prev_visible = 0, bad_data = 0, data_checked = 0, evictions = 0, reloads = 0;
    u64 *prev_keys = NULL;
    u32 prev_n = 0;
    u8 *pool = (u8 *)malloc((size_t)slots * VGEO_PAGE_SIZE);
    for (u32 fr = 0; fr < 200; fr++) {
        float t = (float)fr / 199.0f;
        Cam c = { v3(-110.0f + 220.0f * t, 1.8f + 4.0f * sinf(t * 9.0f), -100.0f + 200.0f * t + 30.0f * sinf(t * 5.0f)), v3(0, 0, 0) };
        c.target = v3(c.eye.x + 30.0f * cosf(t * 11.0f), 1.0f, c.eye.z + 30.0f * sinf(t * 11.0f));
        VgViewGpu v;
        view_for(&c, VGV_NO_OCCLUSION, &v);
        Frame f = run_frame(&v);
        u32 now = g_frame;   // this frame's number (1-based)
        unmapped += f.unmapped;
        // Slots whose page changed in this update: an overwrite must not race a frame in flight.
        for (u32 s = 0; s < slots; s++) {
            u32 a = prev_map[s], b = g_scene.slot_page[s];
            if (a == b) continue;
            if (a != ~0u) {   // page a evicted this frame: not in this frame's list
                evictions++;
                for (u32 cl = 0; cl < (1u << VG_CLUSTER_BITS) && cl < vgeo_scene_page_header(&g_scene, a)->cluster_count; cl++) {
                    bool vis = false;
                    for (u32 i = 0; i < f.n && !vis; i++) vis = (u32)(f.keys[i] >> VG_CLUSTER_BITS & 0xFFFFFu) == a && (u32)(f.keys[i] & 0xFFFu) == cl;
                    if (vis) { evicted_visible++; break; }
                }
                if (prev_keys)
                    for (u32 i = 0; i < prev_n; i++)
                        if ((u32)(prev_keys[i] >> VG_CLUSTER_BITS & 0xFFFFFu) == a) { evicted_prev_visible++; break; }
            }
            if (b != ~0u) {   // page b written into slot s this frame
                reloads += a != ~0u || last_ref[s] != 0;
                if (last_ref[s] && now - last_ref[s] <= F) unsafe_reuse++;
            }
        }
        for (u32 i = 0; i < f.n; i++) if (f.slots[i] < slots) last_ref[f.slots[i]] = now;
        memcpy(prev_map, g_scene.slot_page, sizeof(u32) * slots);
        // Every 10th frame: the GPU pool holds each referenced page's CPU data.
        if (fr % 10 == 9) {
            vgh_read(&g_h, &g_scene.b_pool, 0, (VkDeviceSize)slots * VGEO_PAGE_SIZE, pool);
            u8 *ref = (u8 *)malloc(VGEO_PAGE_SIZE);
            u8 *done = (u8 *)calloc(slots, 1);
            for (u32 i = 0; i < f.n; i++) {
                u32 s = f.slots[i];
                if (s >= slots || done[s] || g_scene.slot_page[s] == ~0u) continue;
                done[s] = 1;
                u32 page = g_scene.slot_page[s];
                vgeo_scene_read_page(&g_scene, page, ref);
                bad_data += memcmp(pool + (size_t)s * VGEO_PAGE_SIZE, ref, g_scene.page_desc[page].size) != 0;
                data_checked++;
            }
            free(done);
            free(ref);
        }
        free(prev_keys);
        prev_keys = f.keys;
        prev_n = f.n;
        f.keys = NULL;
        frame_free(&f);
    }
    const VgStreamStats *st = vg_stream_stats(&g_st);
    CHECK(unmapped == 0, "flythrough: %u visible clusters in free slots", unmapped);
    CHECK(evicted_visible == 0, "flythrough: %u pages evicted in a frame that drew them", evicted_visible);
    CHECK(unsafe_reuse == 0, "flythrough: %u slots overwritten while a frame in flight referenced them", unsafe_reuse);
    CHECK(bad_data == 0, "flythrough: %u of %u resident pages differ from the CPU data on the GPU", bad_data, data_checked);
    CHECK(evictions > 0 && reloads > 0, "flythrough: the pool was not under pressure (%u evictions, %u slot reuses)", evictions, reloads);
    CHECK(closure_violations() == 0, "flythrough: residency not closed");
    printf("flythrough: 200 frames, pool %u slots of %u pages: %llu loaded, %llu evicted (%u slot reuses, %u evicted pages drawn the frame before), %u pages compared\n",
           slots, g_scene.page_count, (unsigned long long)st->loaded_total, (unsigned long long)st->evicted_total, reloads, evicted_prev_visible, data_checked);
    free(prev_keys);
    free(pool);
    free(prev_map);
    free(last_ref);
}

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++) validation |= !strcmp(argv[i], "--validation");
    if (!vgh_init(&g_h, validation)) { printf("vgeo_stream: no device, skipped\n"); return 0; }
    vgeo_scene_init(&g_scene, &g_h.ctx);
    VgGenMesh m;
    float c0[3] = { 0, 0, 0 };
    memset(&m, 0, sizeof(m));
    vgeo_gen_rock(&m, 3, 40, 1.0f, c0, 0.3f);
    build_mesh(&m, &g_built[0]);
    memset(&m, 0, sizeof(m));
    vgeo_gen_torus(&m, 4, 1.2f, 0.35f, 240, 80, c0, 0.2f);
    build_mesh(&m, &g_built[1]);
    memset(&m, 0, sizeof(m));
    vgeo_gen_statue(&m, 5, 120000);
    build_mesh(&m, &g_built[2]);
    // The fourth mesh streams from a .vgeo file (range reads on the worker thread).
    memset(&m, 0, sizeof(m));
    vgeo_gen_rock(&m, 9, 48, 1.0f, c0, 0.35f);
    VgBuilt tmp;
    build_mesh(&m, &tmp);
    u8 *file = NULL;
    size_t fsize = 0;
    const char *path = "vgeo_stream_test.vgeo";
    if (!vgeo_serialize(&tmp, 1, &file, &fsize) || !platform_write_file(path, file, fsize)) { printf("FAIL: cannot write %s\n", path); return 1; }
    vgeo_free(&tmp);
    CHECK(vgeo_deserialize(file, fsize, &g_built[3]), "file mesh reload");
    free(file);
    for (int i = 0; i < 3; i++) vgeo_scene_add_mesh(&g_scene, &g_built[i]);
    vgeo_scene_add_mesh_file(&g_scene, &g_built[3], path);
    VgCullCaps caps = { 1u << 20, 1u << 19, 1u << 21, 1u << 20, 4096 };
    if (!vg_cull_init(&g_cull, &g_h.ctx, &g_scene, &caps)) { printf("FAIL: cull init\n"); return 1; }
    place_instances(20, 12.0f);
    vgeo_scene_commit(&g_scene);
    build_group_map();
    printf("scene: %u meshes, %u instances, %u pages (%u root), max BVH depth %u\n", g_scene.mesh_count, g_scene.instance_count, g_scene.page_count,
           g_scene.root_count, g_scene.max_depth);
    for (u32 i = 0; i < NMESH; i++) printf("  mesh %u: %u pages, %u root, %u levels\n", i, g_built[i].hdr.page_count, g_built[i].hdr.root_page_count, g_built[i].hdr.level_count);
    CHECK(g_scene.page_count > g_scene.root_count + 8, "too few streamable pages (%u of %u)", g_scene.page_count - g_scene.root_count, g_scene.page_count);
    test_cold_start();
    test_convergence();
    test_small_pool();
    test_flythrough();
    vg_stream_shutdown(&g_st);
    vg_cull_shutdown(&g_cull);
    vgeo_scene_free(&g_scene);
    for (int i = 0; i < NMESH; i++) vgeo_free(&g_built[i]);
    free(g_group_of);
    remove(path);
    vgh_shutdown(&g_h);
    if (validation) CHECK(g_vgh_vk_messages == 0, "%d validation messages", g_vgh_vk_messages);
    printf("vgeo_stream: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
