// Virtualized geometry skinned meshes (docs/VGEO.md "Skinned meshes", Phase 6): a procedural
// creature (body + 5 tentacles, 31 bones, src/render/vgeo/vgeo_procgen_skinned.c) with an analytic
// animation.
//   - builder: VGF_SKINNED, <= 16 bones per cluster, bone boxes contain their vertices, weights sum 255
//   - animated bounds: over 600 ticks, every skinned vertex of every cluster of every DAG level lies in
//     its cluster's bone-box sphere, in the transformed LOD / culling spheres and the instance sphere
//   - crack-free animated cuts: CPU cut + CPU skin over an error sweep at several poses, every directed
//     edge (keyed by the skinned float bits) matched by its reverse
//   - GPU cut (LOD only, then + frustum) equals the CPU reference (vgeo_scene_cpu_select) for skinned
//     and static instances at several poses / cameras; empty slots draw nothing
//   - raster: all-HW vs all-SW and mixed vs all-HW visibility of the animated instances (as
//     test_vgeo_raster); the shared GLSL skinning helper (vg_vertex_world & co, used by both rasterisers
//     and the resolve) against the CPU skin within 1e-4, for the current and the previous palette
//   - --validation: zero validation messages
#include "vgeo_gpu_harness.h"
#include "render/vgeo/vgeo_raster.h"
#include "render/vgeo/vgeo_skin.h"
#include "render/vgeo/vgeo_procgen_skinned.h"
#include "core/engine.h"
#include "core/dgmath.h"
#include <SDL3/SDL.h>
#include <float.h>
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
#define SLOTS 32
#define EMPTY_SLOT 30            // an instance on a slot that never gets a palette

static VgGenSkinned g_cr;
static VgBuilt g_b;              // the creature
static VgHarness g_h;
static VgScene g_scene;
static VgCuller g_cull;
static VgRaster g_rast;
static VgPalettes g_pal;

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

static void build_creature(void) {
    vgeo_gen_creature(&g_cr, 3, 5, 6, 30000);
    VgSource src;
    memset(&src, 0, sizeof(src));
    src.vertex_count = g_cr.mesh.vertex_count;
    src.pos = g_cr.mesh.pos;
    src.index_count = g_cr.mesh.index_count;
    src.indices = g_cr.mesh.indices;
    src.joints = g_cr.joints;
    src.weights = g_cr.weights;
    VgBuildOptions opt;
    memset(&opt, 0, sizeof(opt));
    char err[256];
    if (!vgeo_build(&src, &opt, &g_b, err, sizeof(err))) { printf("creature build failed: %s\n", err); exit(1); }
    printf("creature: %u tris, %u bones, %u shells -> %u clusters, %u groups, %u pages, %u levels\n", g_cr.mesh.index_count / 3, g_cr.bone_count, g_cr.shells,
           g_b.hdr.cluster_count, g_b.hdr.group_count, g_b.hdr.page_count, g_b.hdr.level_count);
}

// ------------------------------------------------------------------ CPU skinning of decoded clusters

typedef struct DecCluster {
    u32 page, index, vc;
    float pos[VGEO_MAX_CLUSTER_VERTS][3], nrm[VGEO_MAX_CLUSTER_VERTS][3];
    u8 j[VGEO_MAX_CLUSTER_VERTS][4], w[VGEO_MAX_CLUSTER_VERTS][4];
    VgBoneBox bones[VGEO_MAX_CLUSTER_BONES];
    u32 nb;
    const VgCluster *c;
} DecCluster;
static DecCluster *g_dec;
static u32 g_ndec;

static void decode_all(void) {
    g_dec = (DecCluster *)calloc(g_b.hdr.cluster_count, sizeof(DecCluster));
    static VgDecoded d;
    for (u32 p = 0; p < g_b.hdr.page_count; p++)
        for (u32 i = 0; i < vgeo_page(&g_b, p)->cluster_count; i++) {
            DecCluster *o = &g_dec[g_ndec++];
            vgeo_decode_cluster(&g_b, p, i, &d);
            o->page = p;
            o->index = i;
            o->vc = d.vert_count;
            o->c = vgeo_page_cluster(&g_b, p, i);
            // positions as the shaders decode them: float(grid) * q_step in f32
            for (u32 v = 0; v < d.vert_count; v++) {
                for (int k = 0; k < 3; k++) o->pos[v][k] = (float)d.grid[v][k] * g_b.hdr.q_step;
                memcpy(o->nrm[v], d.normal[v], 12);
                memcpy(o->j[v], d.joints[v], 4);
                memcpy(o->w[v], d.weights[v], 4);
            }
            o->nb = vgeo_cluster_bones((const u8 *)vgeo_page(&g_b, p), o->c, o->bones);
        }
}

static void skin_vertex(const float *pal, const DecCluster *c, u32 v, float out[3]) {
    float m[16];
    vgeo_skin_matrix(pal, c->j[v], c->w[v], m);
    vgeo_mat4_point(m, c->pos[v], out);
}

static float dist3(const float a[3], const float b[3]) {
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// ------------------------------------------------------------------ builder

static void test_builder(void) {
    CHECK(g_b.hdr.flags & VGF_SKINNED, "creature: VGF_SKINNED missing");
    CHECK(g_b.hdr.bone_count == g_cr.bone_count, "creature: %u bones in the header, %u in the rig", g_b.hdr.bone_count, g_cr.bone_count);
    u32 bad_bones = 0, bad_box = 0, bad_sum = 0, bad_set = 0, max_bones = 0, hist[17] = { 0 };
    for (u32 i = 0; i < g_ndec; i++) {
        const DecCluster *c = &g_dec[i];
        if (c->nb == 0 || c->nb > VGEO_MAX_CLUSTER_BONES) bad_bones++;
        max_bones = DG_MAX(max_bones, c->nb);
        hist[DG_MIN(c->nb, 16u)]++;
        for (u32 b = 1; b < c->nb; b++) if (c->bones[b].bone <= c->bones[b - 1].bone) bad_set++;
        for (u32 v = 0; v < c->vc; v++) {
            u32 sum = 0;
            for (int k = 0; k < 4; k++) {
                sum += c->w[v][k];
                if (!c->w[v][k]) continue;
                if (c->j[v][k] >= g_cr.bone_count) bad_set++;
                u32 f = 0;
                while (f < c->nb && c->bones[f].bone != c->j[v][k]) f++;
                if (f == c->nb) { bad_set++; continue; }
                for (int a = 0; a < 3; a++)
                    if (c->pos[v][a] < c->bones[f].min[a] || c->pos[v][a] > c->bones[f].max[a]) { bad_box++; break; }
            }
            if (sum != 255) bad_sum++;
        }
        CHECK(c->c->cone >> 24 & VGC_NO_CONE, "cluster %u: skinned without NO_CONE", i);
        if (!(c->c->cone >> 24 & VGC_NO_CONE)) break;
    }
    printf("builder: max %u bones per cluster; clusters by bone count:", max_bones);
    for (int k = 1; k <= 16; k++) if (hist[k]) printf(" %d:%u", k, hist[k]);
    printf("\n");
    CHECK(bad_bones == 0, "%u clusters with 0 or > 16 bones", bad_bones);
    CHECK(bad_set == 0, "%u weighted joints missing from their cluster's bone set (or unsorted sets)", bad_set);
    CHECK(bad_box == 0, "%u vertices outside the bind box of one of their bones", bad_box);
    CHECK(bad_sum == 0, "%u vertices whose weights do not sum to 255", bad_sum);
    CHECK(max_bones >= 12, "only %u bones in the widest cluster: the > 16 split is not exercised", max_bones);
    u32 top = g_b.stats.level[g_b.stats.levels - 1].clusters;
    printf("builder: %u levels, top level %u clusters, level 0 %u clusters\n", g_b.stats.levels, top, g_b.stats.level[0].clusters);
    CHECK(g_b.stats.levels >= 5, "creature DAG: %u levels, %u top clusters", g_b.stats.levels, top);
}

// ------------------------------------------------------------------ (a) animated bounds

// A CPU-only palette set (one slot, the shaders' slot math) for the CPU tests.
static VgPalettes g_cpu_pal;
static VgPaletteSlot g_cpu_slot;
static float *g_cpu_bones;
static void cpu_slot(const float *pal) {
    memcpy(g_cpu_bones, pal, sizeof(float) * 16 * g_cr.bone_count);
    g_cpu_slot.first = g_cpu_slot.prev_first = 0;
    g_cpu_slot.bone_count = g_cr.bone_count;
    vgeo_palette_bounds(pal, g_cr.bone_count, &g_cpu_slot.max_scale, &g_cpu_slot.k0, &g_cpu_slot.k1);
}

static void cpu_xf(VgXfCpu *x, const float pos[3], const float rot[4], float scale) {
    VgInstance in;
    memset(&in, 0, sizeof(in));
    memcpy(in.pos, pos, 12);
    in.rot = vg_quat_encode(rot);
    in.scale_h = vgeo_half(scale);
    in.flags_phase = (u32)VGI_SKINNED << 16;
    VgMesh m;
    memset(&m, 0, sizeof(m));
    const VgNode *root = &g_b.nodes[g_b.hdr.root_node];
    memcpy(m.center, root->center, 12);
    m.radius = root->radius;
    m.flags = g_b.hdr.flags;
    vgeo_xf_decode(x, &in, &m, &g_cpu_pal);
}

static void test_bounds(void) {
    double t0 = now_s();
    float *pal = (float *)malloc(sizeof(float) * 16 * VGEO_MAX_BONES);
    const float pos[3] = { 0, 0, 0 }, rot[4] = { 0, 0, 0, 1 };
    u64 tested = 0;
    u32 out_bone = 0, out_lod = 0, out_cull = 0, out_inst = 0;
    double worst_bone = 0, sum_ratio = 0, worst_lod = 0;
    u64 nratio = 0;
    const VgNode *root = &g_b.nodes[g_b.hdr.root_node];
    for (int tick = 0; tick < 600; tick++) {
        vgeo_gen_creature_pose(&g_cr, (float)tick / 60.0f, 0.0f, pal);
        cpu_slot(pal);
        VgXfCpu x;
        cpu_xf(&x, pos, rot, 1.0f);
        float inst[4];
        vgeo_xf_sphere(&x, root->center, root->radius, inst);
        for (u32 i = 0; i < g_ndec; i++) {
            const DecCluster *c = &g_dec[i];
            float bs[4], ls[4], cs[4];
            vgeo_skin_cluster_sphere(pal, c->bones, c->nb, bs);
            vgeo_xf_sphere(&x, c->c->lod_center, c->c->lod_radius, ls);
            vgeo_xf_sphere(&x, c->c->center, c->c->radius, cs);
            sum_ratio += bs[3] / c->c->radius;
            nratio++;
            for (u32 v = 0; v < c->vc; v++) {
                float p[3];
                skin_vertex(pal, c, v, p);
                tested++;
                float db = dist3(p, bs), dl = dist3(p, ls);
                worst_bone = fmax(worst_bone, db / bs[3]);
                worst_lod = fmax(worst_lod, dl / ls[3]);
                if (db > bs[3]) out_bone++;
                if (dl > ls[3]) out_lod++;
                if (dist3(p, cs) > cs[3]) out_cull++;
                if (dist3(p, inst) > inst[3]) out_inst++;
            }
        }
    }
    printf("bounds: 600 ticks x %u clusters, %llu skinned vertices in %.1f s: worst |v - c| / r = %.4f (bone boxes), %.4f (LOD sphere); "
           "bone-box sphere / bind sphere = %.2f on average\n", g_ndec, (unsigned long long)tested, now_s() - t0, worst_bone, worst_lod, sum_ratio / (double)nratio);
    CHECK(out_bone == 0, "bounds: %u skinned vertices outside their cluster's bone-box sphere", out_bone);
    CHECK(out_lod == 0, "bounds: %u skinned vertices outside their cluster's transformed LOD sphere", out_lod);
    CHECK(out_cull == 0, "bounds: %u skinned vertices outside their cluster's transformed bind sphere", out_cull);
    CHECK(out_inst == 0, "bounds: %u skinned vertices outside the instance sphere", out_inst);
    free(pal);
}

// ------------------------------------------------------------------ (b) crack-free animated cuts

typedef struct Key3 { u32 v[3]; } Key3;
typedef struct EdgeKey { Key3 a, b; int dir; } EdgeKey;
static int key3_cmp(const Key3 *a, const Key3 *b) {
    for (int i = 0; i < 3; i++)
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return 0;
}
static int key3_qcmp(const void *a, const void *b) { return key3_cmp((const Key3 *)a, (const Key3 *)b); }
static int edge_qcmp(const void *a, const void *b) {
    const EdgeKey *x = (const EdgeKey *)a, *y = (const EdgeKey *)b;
    int c = key3_cmp(&x->a, &y->a);
    return c ? c : key3_cmp(&x->b, &y->b);
}
static Key3 key_of(const float p[3]) {
    Key3 k;
    memcpy(k.v, p, 12);
    for (int i = 0; i < 3; i++) if (p[i] == 0.0f) k.v[i] = 0;   // -0 == +0
    return k;
}

// One decoded cut keyed by the skinned float bits (pal) or the bind grid (pal NULL): open edges (a
// use without its reverse: a crack), pinched (balanced, > 2 uses), Euler characteristic.
typedef struct CutStats { size_t open, pinched, edges, tris; long long chi; } CutStats;
static CutStats cut_stats(const VgCutRef *cut, u32 n, const u32 *dec_of, const float *pal) {
    static VgDecoded d;
    size_t ne = 0, nv = 0, ecap = 0, vcap = 0;
    EdgeKey *edges = NULL;
    Key3 *verts = NULL;
    CutStats st;
    memset(&st, 0, sizeof(st));
    for (u32 i = 0; i < n; i++) {
        const DecCluster *c = &g_dec[dec_of[cut[i].page * 4096 + cut[i].index]];
        vgeo_decode_cluster(&g_b, cut[i].page, cut[i].index, &d);
        if (ne + d.tri_count * 3 > ecap) { while (ne + d.tri_count * 3 > ecap) ecap = ecap ? ecap * 2 : 65536; edges = (EdgeKey *)realloc(edges, sizeof(EdgeKey) * ecap); }
        if (nv + d.vert_count > vcap) { while (nv + d.vert_count > vcap) vcap = vcap ? vcap * 2 : 65536; verts = (Key3 *)realloc(verts, sizeof(Key3) * vcap); }
        Key3 sk[VGEO_MAX_CLUSTER_VERTS];
        for (u32 v = 0; v < c->vc; v++) {
            if (pal) {
                float p[3];
                skin_vertex(pal, c, v, p);
                sk[v] = key_of(p);
            } else memcpy(sk[v].v, d.grid[v], 12);
            verts[nv++] = sk[v];
        }
        st.tris += d.tri_count;
        for (u32 k = 0; k < d.tri_count; k++)
            for (int e = 0; e < 3; e++) {
                Key3 a = sk[d.tri[k][e]], b = sk[d.tri[k][(e + 1) % 3]];
                EdgeKey ek;
                if (key3_cmp(&a, &b) <= 0) { ek.a = a; ek.b = b; ek.dir = 0; } else { ek.a = b; ek.b = a; ek.dir = 1; }
                edges[ne++] = ek;
            }
    }
    qsort(edges, ne, sizeof(EdgeKey), edge_qcmp);
    qsort(verts, nv, sizeof(Key3), key3_qcmp);
    size_t uv = 0;
    for (size_t i = 0; i < ne;) {
        size_t j = i, fwd = 0;
        while (j < ne && !edge_qcmp(&edges[i], &edges[j])) j++;
        for (size_t k = i; k < j; k++) fwd += edges[k].dir == 0;
        if (fwd * 2 != j - i) st.open++;
        else if (j - i != 2) st.pinched++;
        st.edges++;
        i = j;
    }
    for (size_t i = 0; i < nv; i++) if (!i || key3_cmp(&verts[i], &verts[i - 1])) uv++;
    st.chi = (long long)uv - (long long)st.edges + (long long)st.tris;
    free(edges);
    free(verts);
    return st;
}

// Crack-free animated cuts: the skinned cut has no open edge, and exactly the topology of the bind
// pose cut (same edges, pinched edges and Euler characteristic: skinning shared vertices
// bit-identically adds nothing; the coarse-level pinches of the thin tentacles are the simplifier's,
// see docs/VGEO.md "Pinched edges").
static void test_cuts(void) {
    VgCutRef *cut = (VgCutRef *)malloc(sizeof(VgCutRef) * g_b.hdr.cluster_count);
    u32 *dec_of = (u32 *)malloc(sizeof(u32) * g_b.hdr.page_count * 4096);
    for (u32 i = 0; i < g_ndec; i++) dec_of[g_dec[i].page * 4096 + g_dec[i].index] = i;
    float *pal = (float *)malloc(sizeof(float) * 16 * VGEO_MAX_BONES);
    float r = g_b.hdr.sphere[3];
    const float poses[3] = { 0.7f, 3.9f, 8.3f };
    u32 total_open = 0, cuts = 0, pinched = 0, bind_pinched = 0, fine_cuts = 0;
    for (int s = 0; s <= 24; s++) {
        float t = s == 0 ? 0.0f : r * 1e-5f * powf(2.0f, (float)s);
        u32 n = vgeo_cut(&g_b, t, cut, g_b.hdr.cluster_count);
        CutStats bind = cut_stats(cut, n, dec_of, NULL);
        CHECK(bind.open == 0, "bind t=%g: %zu open edges", (double)t, bind.open);
        bind_pinched += (u32)bind.pinched;
        if (!bind.pinched) {
            fine_cuts++;
            // every shell closed while the error is below the tentacles' size; coarser cuts may drop whole tentacles
            if (s <= 10) CHECK(bind.chi == 2 * (long long)g_cr.shells, "bind t=%g: Euler %lld, want %u", (double)t, bind.chi, 2 * g_cr.shells);
            else CHECK(bind.chi % 2 == 0 && bind.chi <= 2 * (long long)g_cr.shells && bind.chi > 0, "bind t=%g: Euler %lld", (double)t, bind.chi);
        }
        for (int pi = 0; pi < 3; pi++) {
            vgeo_gen_creature_pose(&g_cr, poses[pi], 0.0f, pal);
            CutStats st = cut_stats(cut, n, dec_of, pal);
            CHECK(st.open == 0, "pose %.1f t=%g: %zu open edges on the skinned cut (%u clusters, %zu tris)", poses[pi], (double)t, st.open, n, st.tris);
            CHECK(st.pinched == bind.pinched && st.chi == bind.chi && st.edges == bind.edges,
                  "pose %.1f t=%g: skinned topology (%zu edges, %zu pinched, Euler %lld) differs from the bind pose (%zu, %zu, %lld)", poses[pi], (double)t,
                  st.edges, st.pinched, st.chi, bind.edges, bind.pinched, bind.chi);
            total_open += (u32)st.open;
            pinched += (u32)st.pinched;
            cuts++;
        }
    }
    CHECK(fine_cuts >= 8, "only %u cuts without pinched edges", fine_cuts);
    printf("animated cuts: %u cuts (25 errors x 3 poses), %u open edges, %u pinched edges (bind pose: %u over the 25 cuts; %u cuts without pinched edges, the fine ones closed of "
           "Euler %u)\n", cuts, total_open, pinched, bind_pinched, fine_cuts, 2 * g_cr.shells);
    free(pal);
    free(dec_of);
    free(cut);
}

// ------------------------------------------------------------------ GPU scene

typedef struct Cam { vec3 eye, target; } Cam;

static void view_for(const Cam *c, u32 flags, float sw_px, VgViewGpu *v) {
    mat4 view = mat4_look_at(c->eye, c->target, v3(0, 1, 0));
    mat4 proj = mat4_perspective(FOV, (float)W / (float)H, ZNEAR, 4000.0f);
    mat4 vp = mat4_mul(proj, view);
    float ps = (float)H / (2.0f * tanf(FOV * 0.5f));
    float eye[3] = { c->eye.x, c->eye.y, c->eye.z };
    vgeo_view_setup(v, vp.m, vp.m, eye, ps, ZNEAR, W, H, 1.0f, sw_px, flags);
}

static u32 g_creature_mesh, g_skinned_instances;

static void build_scene(void) {
    vgeo_scene_init(&g_scene, &g_h.ctx);
    g_creature_mesh = vgeo_scene_add_mesh(&g_scene, &g_b);
    VgGenMesh m;
    memset(&m, 0, sizeof(m));
    float c0[3] = { 0, 0, 0 };
    vgeo_gen_rock(&m, 3, 30, 1.0f, c0, 0.3f);
    VgSource src;
    memset(&src, 0, sizeof(src));
    src.vertex_count = m.vertex_count;
    src.pos = m.pos;
    src.index_count = m.index_count;
    src.indices = m.indices;
    VgBuilt rb;
    char err[256];
    if (!vgeo_build(&src, NULL, &rb, err, sizeof(err))) { printf("rock build failed: %s\n", err); exit(1); }
    u32 rock = vgeo_scene_add_mesh(&g_scene, &rb);
    vgeo_free(&rb);
    vgeo_gen_free(&m);
    // 5 x 5 creatures (slots 0..24), one rotated + scaled; one on the empty slot; rocks around
    u32 slot = 0;
    for (int z = 0; z < 5; z++)
        for (int x = 0; x < 5; x++) {
            float p[3] = { (float)(x - 2) * 7.0f, 0.0f, (float)(z - 2) * 7.0f };
            quat q = slot == 7 ? quat_mul(quat_yaw(0.8f), quat_axis_angle(v3(1, 0, 0), 0.3f)) : quat_identity();
            float r[4] = { q.x, q.y, q.z, q.w };
            vgeo_scene_add_skinned_instance(&g_scene, g_creature_mesh, p, r, slot == 7 ? 1.4f : 1.0f, slot);
            slot++;
        }
    float pe[3] = { 3.5f, 0.0f, 3.5f }, ri[4] = { 0, 0, 0, 1 };
    vgeo_scene_add_skinned_instance(&g_scene, g_creature_mesh, pe, ri, 1.0f, EMPTY_SLOT);
    g_skinned_instances = slot + 1;
    u32 rng = 99;
    for (int i = 0; i < 40; i++) {
        rng = rng * 1664525u + 1013904223u;
        float a = (float)(rng >> 8) / 16777216.0f * 6.2831853f;
        float p[3] = { cosf(a) * (24.0f + (float)(i % 7)), 0.3f, sinf(a) * (24.0f + (float)(i % 5)) };
        vgeo_scene_add_instance(&g_scene, rock, p, ri, 0.8f + (float)(i % 4) * 0.4f);
    }
    vgeo_scene_commit(&g_scene);
    printf("scene: %u meshes, %u instances (%u skinned), %u cells\n", g_scene.mesh_count, g_scene.instance_count, g_skinned_instances, g_scene.cell_count);
}

// Every slot but the empty one: palettes of pose t (phase per slot); prev_t first when >= 0, so the
// previous palette is that pose's.
static void set_pose(float t, float prev_t) {
    float *pal = (float *)malloc(sizeof(float) * 16 * VGEO_MAX_BONES);
    for (int pass = prev_t >= 0 ? 0 : 1; pass < 2; pass++) {
        vg_palettes_begin(&g_pal, 0);
        for (u32 s = 0; s < 25; s++) {
            vgeo_gen_creature_pose(&g_cr, pass ? t : prev_t, 0.37f * (float)s, pal);
            vg_palettes_set(&g_pal, s, pal, g_cr.bone_count);
        }
        if (pass == 0) {   // upload the previous pose as its own frame
            VkCommandPool pool;
            VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
            vg_palettes_record(&g_pal, cmd);
            vgc_submit_wait(&g_h.ctx, pool, cmd);
        }
    }
    free(pal);
}

static u64 key(u32 inst, u32 packed) { return (u64)inst << 32 | packed; }
static int u64_cmp(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}
static bool has(const u64 *keys, u32 n, u64 k) { return bsearch(&k, keys, n, sizeof(u64), u64_cmp) != NULL; }

// Uploads the pending palettes (the frame's record), culls one view; sorted visible keys.
static u64 *run_view(const VgViewGpu *v, VgCounters *ctr, u32 *count) {
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
    vg_palettes_record(&g_pal, cmd);
    vg_cull_record(&g_cull, cmd, 0, 0, v, NULL, NULL);
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

static const Cam k_cams[4] = {
    { { 0, 6, 26 }, { 0, 0.5f, 0 } },          // the whole herd
    { { 2.5f, 1.4f, 5.0f }, { 0, 1.0f, 0 } },  // close to the centre creature
    { { -45, 3, -12 }, { 0, 1, 0 } },          // far, low
    { { 9, 0.6f, -3 }, { 7, 1.2f, -7 } },      // among them, looking up
};

static bool is_skinned_inst(u32 i) { return (g_scene.instances[i].flags_phase >> 16) & VGI_SKINNED; }

static void test_gpu_cut(void) {
    u32 cap = 1u << 20;
    VgRefCluster *ref = (VgRefCluster *)malloc(sizeof(VgRefCluster) * cap);
    const float poses[3] = { 0.0f, 2.7f, 6.1f };
    for (int pi = 0; pi < 3; pi++) {
        set_pose(poses[pi], -1.0f);
        for (int ci = 0; ci < 4; ci++) {
            VgViewGpu v;
            view_for(&k_cams[ci], VGV_NO_FRUSTUM | VGV_NO_CONE | VGV_NO_OCCLUSION, 32.0f, &v);
            VgCounters ctr;
            u32 n = 0;
            u64 *keys = run_view(&v, &ctr, &n);
            u32 rn = vgeo_scene_cpu_select(&g_scene, &v, ref, cap);
            CHECK(ctr.n[VGN_OVERFLOW] == 0, "pose %d cam %d: %u overflows", pi, ci, ctr.n[VGN_OVERFLOW]);
            u32 missing = 0, border = 0, skinned = 0, empty = 0;
            u64 *rk = (u64 *)malloc(sizeof(u64) * (rn + 1));
            for (u32 i = 0; i < rn; i++) {
                rk[i] = key(ref[i].instance, ref[i].packed);
                if (is_skinned_inst(ref[i].instance)) skinned++;
                if (!has(keys, n, rk[i])) {
                    if (ref[i].lod_margin < 1e-4f) border++;
                    else missing++;
                }
            }
            qsort(rk, rn, sizeof(u64), u64_cmp);
            u32 extra = 0;
            for (u32 i = 0; i < n; i++) {
                if (!has(rk, rn, keys[i])) extra++;
                u32 inst = (u32)(keys[i] >> 32);
                if (is_skinned_inst(inst) && g_scene.instances[inst].extra == EMPTY_SLOT) empty++;
            }
            CHECK(missing == 0, "pose %d cam %d: %u clusters of the CPU cut missing on the GPU (%u selected, %u reference)", pi, ci, missing, n, rn);
            CHECK(extra <= border + rn / 10000, "pose %d cam %d: %u GPU clusters not in the CPU cut", pi, ci, extra);
            CHECK(empty == 0, "pose %d cam %d: %u clusters of the empty-slot instance drawn", pi, ci, empty);
            CHECK(skinned > 0, "pose %d cam %d: no skinned cluster selected", pi, ci);
            // + frustum
            view_for(&k_cams[ci], VGV_NO_CONE | VGV_NO_OCCLUSION, 32.0f, &v);
            u32 rn2 = vgeo_scene_cpu_select(&g_scene, &v, ref, cap), n2 = 0;
            free(keys);
            keys = run_view(&v, &ctr, &n2);
            u32 lost = 0, kept = 0, inside = 0;
            for (u32 i = 0; i < rn2; i++) {
                if (ref[i].lod_margin < 1e-4f || ref[i].frustum < 0) continue;
                bool in = has(keys, n2, key(ref[i].instance, ref[i].packed));
                inside += ref[i].frustum == 1;
                if (ref[i].frustum == 1 && !in) lost++;
                if (ref[i].frustum == 0 && in) kept++;
            }
            CHECK(lost == 0, "pose %d cam %d: %u clusters inside the frustum lost", pi, ci, lost);
            CHECK(kept == 0, "pose %d cam %d: %u clusters outside the frustum kept", pi, ci, kept);
            printf("pose %.1f cam %d: cut %u clusters (cpu %u, %u skinned, %u borderline, %u extra); frustum %u (%u inside, %u HW, %u SW)\n", poses[pi], ci, n, rn,
                   skinned, border, extra, n2, inside, ctr.n[VGN_HW], ctr.n[VGN_SW]);
            free(keys);
            free(rk);
        }
    }
    free(ref);
}

// ------------------------------------------------------------------ (d) raster

typedef struct Px { u32 inst, packed, tri; } Px;
typedef struct Img { Px *px; u32 hw, sw, covered, skinned_px; } Img;
static void img_free(Img *m) { free(m->px); memset(m, 0, sizeof(*m)); }

static void render(const VgViewGpu *v, Img *out) {
    memset(out, 0, sizeof(*out));
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(&g_h.ctx, &pool);
    vg_palettes_record(&g_pal, cmd);
    vg_cull_record(&g_cull, cmd, 0, 0, v, NULL, NULL);
    vg_raster_clear(&g_rast, cmd);
    vg_raster_record(&g_rast, cmd, 0, 0, 1);
    vg_raster_record(&g_rast, cmd, 0, 0, 2);
    vgc_submit_wait(&g_h.ctx, pool, cmd);
    VgCounters ctr;
    VgViewRes *r = &g_cull.v[0];
    vgh_read(&g_h, &r->counters, 0, sizeof(VgCounters), &ctr);
    u32 cap = g_cull.caps.visible;
    out->hw = DG_MIN(ctr.n[VGN_HW], cap);
    out->sw = DG_MIN(ctr.n[VGN_SW], cap);
    u32 *hw = (u32 *)malloc(8ull * (out->hw + 1)), *sw = (u32 *)malloc(8ull * (out->sw + 1));
    vgh_read(&g_h, &r->visible, 0, 8ull * out->hw, hw);
    vgh_read(&g_h, &r->visible, 8ull * cap, 8ull * out->sw, sw);
    u64 *vis = (u64 *)malloc(8ull * W * H);
    vgh_read(&g_h, &g_rast.vis, 0, 8ull * W * H, vis);
    out->px = (Px *)malloc(sizeof(Px) * W * H);
    for (u32 i = 0; i < W * H; i++) {
        Px *p = &out->px[i];
        p->inst = ~0u;
        if (!vis[i]) continue;
        u32 lo = (u32)vis[i], idx = lo >> VG_VIS_TRI_BITS;
        const u32 *e = idx < cap ? (idx < out->hw ? &hw[2 * idx] : NULL) : (idx - cap < out->sw ? &sw[2 * (idx - cap)] : NULL);
        if (!e) continue;
        p->inst = e[0];
        p->packed = e[1];
        p->tri = lo & ((1u << VG_VIS_TRI_BITS) - 1);
        out->covered++;
        if (is_skinned_inst(e[0])) out->skinned_px++;
    }
    free(vis);
    free(hw);
    free(sw);
}

static bool same(const Px *a, const Px *b) { return a->inst == b->inst && (a->inst == ~0u || (a->packed == b->packed && a->tri == b->tri)); }

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
            if (!edge) inner++;
        }
    double frac = covered ? (double)diff / covered : 0.0;
    printf("  %s: %u of %u covered pixels differ (%.3f %%), %u not on an edge\n", what, diff, covered, frac * 100.0, inner);
    CHECK(covered > 10000, "%s: only %u pixels covered", what, covered);
    CHECK(frac <= max_frac, "%s: %.3f %% of covered pixels differ", what, frac * 100.0);
    CHECK(inner == 0, "%s: %u differing pixels not on a triangle edge", what, inner);
    return diff;
}

static void test_raster(void) {
    set_pose(4.4f, -1.0f);
    for (int ci = 0; ci < 4; ci += 1) {
        if (ci == 2) continue;   // far camera: few pixels
        VgViewGpu v;
        Img hw, sw, mix;
        view_for(&k_cams[ci], VGV_NO_OCCLUSION, 0.0f, &v);
        render(&v, &hw);
        view_for(&k_cams[ci], VGV_NO_OCCLUSION, SW_ALL, &v);
        render(&v, &sw);
        printf("raster cam %d: all-HW %u clusters, all-SW %u SW + %u HW (near plane); %u of %u covered pixels skinned\n", ci, hw.hw, sw.sw, sw.hw, hw.skinned_px, hw.covered);
        CHECK(hw.skinned_px > 5000, "cam %d: only %u skinned pixels", ci, hw.skinned_px);
        CHECK(sw.sw > hw.hw / 2, "cam %d: only %u of %u clusters went SW", ci, sw.sw, hw.hw);
        compare("HW vs SW", &hw, &sw, 0.005);
        view_for(&k_cams[ci], VGV_NO_OCCLUSION, 32.0f, &v);
        render(&v, &mix);
        compare("mixed vs HW", &mix, &hw, 0.005);
        if (g_rast.p_mesh) {
            Img vp;
            g_rast.use_mesh = false;
            view_for(&k_cams[ci], VGV_NO_OCCLUSION, 0.0f, &v);
            render(&v, &vp);
            g_rast.use_mesh = true;
            compare("mesh shader vs vertex pulling", &hw, &vp, 0.005);
            img_free(&vp);
        }
        img_free(&hw);
        img_free(&sw);
        img_free(&mix);
    }
}

// The shared skinning helper (vg_decode.glsl) through shaders/vgeo/vg_test_skin.comp vs the CPU skin.
static void test_skin_helper(void) {
    const float t = 5.3f, dt = 1.0f / 60.0f;
    set_pose(t, t - dt);
    // items: every vertex of every 3rd cluster of 4 instances (incl. the rotated + scaled one)
    u32 inst_pick[4] = { 0, 0, 0, 0 }, ni = 0;
    for (u32 i = 0; i < g_scene.instance_count && ni < 4; i++) {
        const VgInstance *in = &g_scene.instances[i];
        if (!is_skinned_inst(i) || in->extra == EMPTY_SLOT) continue;
        if (in->extra == 7 || in->extra == 0 || in->extra == 12 || in->extra == 24) inst_pick[ni++] = i;
    }
    const VgMesh *me = &g_scene.meshes[g_creature_mesh];
    u32 cap = 0;
    for (u32 i = 0; i < g_ndec; i += 3) cap += g_dec[i].vc;
    cap *= ni;
    u32 *items = (u32 *)malloc(16ull * cap);
    u32 n = 0;
    for (u32 k = 0; k < ni; k++)
        for (u32 i = 0; i < g_ndec; i += 3) {
            u32 slot = g_scene.page_table[me->page_base + g_dec[i].page];
            for (u32 v = 0; v < g_dec[i].vc; v++) {
                items[n * 4] = inst_pick[k];
                items[n * 4 + 1] = slot << VG_CLUSTER_BITS | g_dec[i].index;
                items[n * 4 + 2] = v;
                items[n * 4 + 3] = i;
                n++;
            }
        }
    const VgContext *c = &g_h.ctx;
    VgBuffer bin = vgc_buffer(c, 16ull * n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_DEVICE);
    VgBuffer bout = vgc_buffer(c, 64ull * n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VGM_DEVICE);
    vgc_upload(c, &bin, 0, items, 16ull * n);
    VkDescriptorSetLayoutBinding lb[2];
    memset(lb, 0, sizeof(lb));
    for (u32 i = 0; i < 2; i++) {
        lb[i].binding = i;
        lb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = lb;
    VkDescriptorSetLayout tl;
    vkCreateDescriptorSetLayout(c->device, &li, NULL, &tl);
    VkDescriptorSetLayout sets[2] = { g_cull.layout, tl };
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VgPush) };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 2;
    pi.pSetLayouts = sets;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    VkPipelineLayout pl;
    vkCreatePipelineLayout(c->device, &pi, NULL, &pl);
    VkPipeline pipe = vgc_compute_pipeline(c, pl, "vgeo/vg_test_skin.comp");
    CHECK(pipe != VK_NULL_HANDLE, "vg_test_skin.comp pipeline");
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    VkDescriptorPool dpool;
    vkCreateDescriptorPool(c->device, &dp, NULL, &dpool);
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &tl;
    VkDescriptorSet ts;
    vkAllocateDescriptorSets(c->device, &ai, &ts);
    VkDescriptorBufferInfo bi[2] = { { bin.buffer, 0, VK_WHOLE_SIZE }, { bout.buffer, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[2];
    memset(wr, 0, sizeof(wr));
    for (u32 i = 0; i < 2; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = ts;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(c->device, 2, wr, 0, NULL);
    VgViewGpu v;
    view_for(&k_cams[1], VGV_NO_OCCLUSION, 32.0f, &v);
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(c, &pool);
    vg_palettes_record(&g_pal, cmd);
    vg_cull_record(&g_cull, cmd, 0, 0, &v, NULL, NULL);   // uploads the view, writes the set
    VkDescriptorSet cs = vg_cull_set(&g_cull, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &cs, 0, NULL);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 1, 1, &ts, 0, NULL);
    VgPush p;
    memset(&p, 0, sizeof(p));
    p.a[0] = n;
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, (n + 63) / 64, 1, 1);
    vgc_submit_wait(c, pool, cmd);
    float *o = (float *)malloc(64ull * n);
    vgh_read(&g_h, &bout, 0, 64ull * n, o);
    // CPU: the slot's palettes as uploaded, the shaders' instance decode
    double worst_p = 0, worst_prev = 0, worst_n = 0, worst_clip = 0, worst_motion = 0;
    for (u32 i = 0; i < n; i++) {
        const DecCluster *dc = &g_dec[items[i * 4 + 3]];
        u32 vv = items[i * 4 + 2];
        VgXfCpu x;
        vgeo_xf_decode(&x, &g_scene.instances[items[i * 4]], me, &g_pal);
        float m[16], mp[16], s[3], sp[3], w[3], wp[3], nn[3], wn[3];
        vgeo_skin_matrix(&g_pal.cpu_bones[(size_t)x.first * 16], dc->j[vv], dc->w[vv], m);
        vgeo_skin_matrix(&g_pal.cpu_bones[(size_t)x.prev_first * 16], dc->j[vv], dc->w[vv], mp);
        vgeo_mat4_point(m, dc->pos[vv], s);
        vgeo_mat4_point(mp, dc->pos[vv], sp);
        vgeo_mat4_dir(m, dc->nrm[vv], nn);
        float z[3] = { 0, 0, 0 }, ws[4], wps[4];
        vgeo_xf_instance_sphere(&x, s, 0, ws);
        vgeo_xf_instance_sphere(&x, sp, 0, wps);
        memcpy(w, ws, 12);
        memcpy(wp, wps, 12);
        // normal: instance rotation only
        VgXfCpu xr = x;
        xr.pscale = 1.0f;
        memset(xr.pos, 0, 12);
        float rn[4];
        vgeo_xf_instance_sphere(&xr, nn, 0, rn);
        memcpy(wn, rn, 12);
        (void)z;
        const float *g = &o[i * 16];
        worst_p = fmax(worst_p, dist3(g, w));
        worst_prev = fmax(worst_prev, dist3(g + 4, wp));
        worst_motion = fmax(worst_motion, dist3(w, wp));
        float lg = sqrtf(g[8] * g[8] + g[9] * g[9] + g[10] * g[10]), lc = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
        float dot = (g[8] * wn[0] + g[9] * wn[1] + g[10] * wn[2]) / fmaxf(lg * lc, 1e-20f);
        worst_n = fmax(worst_n, 1.0 - dot);
        double cl[4];
        for (int k = 0; k < 4; k++) cl[k] = (double)v.raster_view_proj[k] * w[0] + (double)v.raster_view_proj[4 + k] * w[1] + (double)v.raster_view_proj[8 + k] * w[2] + v.raster_view_proj[12 + k];
        for (int k = 0; k < 4; k++) worst_clip = fmax(worst_clip, fabs(cl[k] - g[12 + k]) / fmax(fabs(cl[3]), 1.0));
    }
    printf("skin helper: %u vertices of 4 instances: worst |gpu - cpu| %.2e m (current), %.2e m (previous palette; motion up to %.3f m), normal 1 - cos %.2e, clip %.2e\n", n,
           worst_p, worst_prev, worst_motion, worst_n, worst_clip);
    CHECK(worst_p < 1e-4, "skin helper: position off by %.2e", worst_p);
    CHECK(worst_prev < 1e-4, "skin helper: previous-palette position off by %.2e", worst_prev);
    CHECK(worst_motion > 1e-3, "skin helper: the previous palette equals the current one (motion %.2e)", worst_motion);
    CHECK(worst_n < 1e-4, "skin helper: normal off by 1 - cos %.2e", worst_n);
    CHECK(worst_clip < 1e-4, "skin helper: raster clip position off by %.2e", worst_clip);
    free(o);
    free(items);
    vkDestroyPipeline(c->device, pipe, NULL);
    vkDestroyPipelineLayout(c->device, pl, NULL);
    vkDestroyDescriptorPool(c->device, dpool, NULL);
    vkDestroyDescriptorSetLayout(c->device, tl, NULL);
    vgc_buffer_free(c, &bin);
    vgc_buffer_free(c, &bout);
}

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++) validation |= !strcmp(argv[i], "--validation");
    double t0 = now_s();
    build_creature();
    decode_all();
    g_cpu_bones = (float *)calloc(2 * VGEO_MAX_BONES * 16, sizeof(float));
    g_cpu_pal.max_slots = 1;
    g_cpu_pal.cpu_bones = g_cpu_bones;
    g_cpu_pal.cpu_slots = &g_cpu_slot;
    test_builder();
    test_bounds();
    test_cuts();
    if (!vgh_init(&g_h, validation)) {
        printf("vgeo_skinned: no device, GPU tests skipped\n");
    } else {
        build_scene();
        VgCullCaps caps = { 1u << 20, 1u << 19, 1u << 21, 1u << 20, 4096 };
        if (!vg_cull_init(&g_cull, &g_h.ctx, &g_scene, &caps)) { printf("FAIL: cull init\n"); return 1; }
        if (!vg_palettes_init(&g_pal, &g_h.ctx, SLOTS)) { printf("FAIL: palettes init\n"); return 1; }
        vg_palettes_attach(&g_pal, &g_scene);
        if (!vg_raster_init(&g_rast, &g_h.ctx, &g_cull)) { printf("FAIL: raster init\n"); return 1; }
        vg_raster_resize(&g_rast, W, H);
        test_gpu_cut();
        test_raster();
        test_skin_helper();
        vg_raster_shutdown(&g_rast);
        vg_palettes_shutdown(&g_pal);
        vg_cull_shutdown(&g_cull);
        vgeo_scene_free(&g_scene);
        vgh_shutdown(&g_h);
        if (validation) CHECK(g_vgh_vk_messages == 0, "%d validation messages", g_vgh_vk_messages);
    }
    free(g_dec);
    free(g_cpu_bones);
    vgeo_free(&g_b);
    vgeo_gen_skinned_free(&g_cr);
    printf("vgeo_skinned: %d/%d checks passed (%.1f s)\n", g_checks - g_fail, g_checks, now_s() - t0);
    return g_fail ? 1 : 0;
}
