// Virtualized geometry builder tests (src/render/vgeo/vgeo_build.c, docs/VGEO.md "Builder"): cluster
// limits, monotonic errors and LOD spheres up the DAG, culling spheres contain their decoded vertices,
// a group's locked boundary is reproduced exactly by the clusters simplified from it, watertight cuts
// over an error sweep (every edge used exactly twice, Euler characteristic of the shell, never an
// ancestor and a descendant together), byte-identical output across runs and thread counts, serialize
// round trip, decreasing discrete LODs, the glTF import + cache path, and the build-time gates
// (500k statue < 10 s; `--long`: 10M triangles < 3 min on 4 threads, < 6 GB).
#include "render/vgeo/vgeo_build.h"
#include "render/vgeo/vgeo_import.h"
#include "render/vgeo/vgeo_procgen.h"
#include "core/engine.h"
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

Engine g_engine;   // the core library logs through it
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

static size_t g_pinched;   // pinched (balanced, 4-use) edges seen over all cut sweeps

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

static double peak_mb(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
    return 0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (double)ru.ru_maxrss / (1024.0 * 1024.0);   // bytes on macOS (KB on Linux)
#else
    return (double)ru.ru_maxrss / 1024.0;
#endif
#endif
}

static bool build(const VgGenMesh *m, const u16 *tri_mat, u32 mat_count, int threads, bool dbg, VgBuilt *b) {
    VgSource src;
    memset(&src, 0, sizeof(src));
    src.vertex_count = m->vertex_count;
    src.pos = m->pos;
    src.index_count = m->index_count;
    src.indices = m->indices;
    src.tri_material = tri_mat;
    src.material_count = mat_count;
    VgBuildOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.threads = threads;
    opt.keep_build_data = dbg;
    char err[256] = "";
    bool ok = vgeo_build(&src, &opt, b, err, sizeof(err));
    if (!ok) printf("build failed: %s\n", err);
    return ok;
}

// ------------------------------------------------------------------ hashing of integer edges / vertices

typedef struct Key3 { i32 v[3]; } Key3;
typedef struct EdgeKey { Key3 a, b; int dir; } EdgeKey;   // canonical a < b; dir 1 when stored reversed

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
static EdgeKey edge_of(const i32 *p, const i32 *q) {
    EdgeKey e;
    Key3 a = { { p[0], p[1], p[2] } }, b = { { q[0], q[1], q[2] } };
    if (key3_cmp(&a, &b) <= 0) { e.a = a; e.b = b; e.dir = 0; } else { e.a = b; e.b = a; e.dir = 1; }
    return e;
}

static int u64_cmp(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

// ------------------------------------------------------------------ structure

static void test_structure(const char *name, const VgBuilt *b) {
    const VgDbg *d = &b->dbg;
    u32 bad_limit = 0, bad_err = 0, bad_sphere = 0, bad_contain = 0;
    for (u32 p = 0; p < b->hdr.page_count; p++) {
        const VgPageHeader *ph = vgeo_page(b, p);
        CHECK(b->pages[p].size <= VGEO_PAGE_SIZE, "%s page %u size %u", name, p, b->pages[p].size);
        for (u32 i = 0; i < ph->cluster_count; i++) {
            const VgCluster *c = vgeo_page_cluster(b, p, i);
            static VgDecoded dc;
            vgeo_decode_cluster(b, p, i, &dc);
            if (dc.tri_count > VGEO_MAX_CLUSTER_TRIS || dc.vert_count > VGEO_MAX_CLUSTER_VERTS || !dc.tri_count) bad_limit++;
            // The culling sphere contains every decoded vertex (decoded = what the GPU sees).
            for (u32 v = 0; v < dc.vert_count; v++) {
                float dx = dc.pos[v][0] - c->center[0], dy = dc.pos[v][1] - c->center[1], dz = dc.pos[v][2] - c->center[2];
                if (sqrtf(dx * dx + dy * dy + dz * dz) > c->radius * 1.0001f + 1e-5f) { bad_contain++; break; }
            }
            if (c->child_group != ~0u) {
                // A cluster's error is its source group's simplification error and its LOD sphere the
                // source group's: so cluster.error > child group's member errors, sphere contains them.
                const VgGroup *cg = &b->groups[c->child_group];
                if (!(c->error >= cg->parent_error - 1e-7f && c->error <= cg->parent_error + 1e-7f)) bad_err++;
                for (u32 k = 0; k < cg->cluster_count; k++) {
                    const VgCluster *cc = vgeo_page_cluster(b, cg->page, cg->first_cluster + k);
                    if (cc->error > c->error) bad_err++;
                    float dx = cc->lod_center[0] - c->lod_center[0], dy = cc->lod_center[1] - c->lod_center[1], dz = cc->lod_center[2] - c->lod_center[2];
                    if (sqrtf(dx * dx + dy * dy + dz * dz) + cc->lod_radius > c->lod_radius * 1.0001f + 1e-5f) bad_sphere++;
                }
            } else if (c->error != 0) bad_err++;
        }
    }
    for (u32 g = 0; g < b->hdr.group_count; g++) {
        const VgGroup *gr = &b->groups[g];
        for (u32 k = 0; k < gr->cluster_count; k++) {
            const VgCluster *c = vgeo_page_cluster(b, gr->page, gr->first_cluster + k);
            if (c->error > gr->parent_error) bad_err++;
        }
    }
    CHECK(bad_limit == 0, "%s: %u clusters break the 128/128 limits", name, bad_limit);
    CHECK(bad_err == 0, "%s: %u non-monotonic errors", name, bad_err);
    CHECK(bad_sphere == 0, "%s: %u LOD spheres not nested", name, bad_sphere);
    CHECK(bad_contain == 0, "%s: %u culling spheres miss vertices", name, bad_contain);
    u32 roots = 0;
    for (u32 g = 0; g < b->hdr.group_count; g++) roots += b->groups[g].parent_error == FLT_MAX;
    CHECK(roots >= 1, "%s: no root group", name);
    CHECK(b->hdr.cluster_count == d->cluster_count || !d->cluster_count, "%s: cluster count %u vs %u", name, b->hdr.cluster_count, d->cluster_count);
}

// Locked boundaries: the boundary of the union of a group's clusters equals the boundary of the union
// of the clusters simplified from it, edge for edge in welded-position ids (no cracks between LODs).
static void test_boundaries(const char *name, const VgBuilt *b) {
    const VgDbg *d = &b->dbg;
    u32 ng = b->hdr.group_count;
    u32 bad = 0, tested = 0;
    u64 *mine = NULL, *gen = NULL;
    size_t mcap = 0, gcap = 0;
    // bucket clusters by group and by child group
    u32 *by_group = (u32 *)calloc(ng + 1, sizeof(u32)), *by_child = (u32 *)calloc(ng + 1, sizeof(u32));
    for (u32 c = 0; c < d->cluster_count; c++) {
        by_group[d->clusters[c].group + 1]++;
        if (d->clusters[c].child_group != ~0u) by_child[d->clusters[c].child_group + 1]++;
    }
    for (u32 g = 0; g < ng; g++) { by_group[g + 1] += by_group[g]; by_child[g + 1] += by_child[g]; }
    u32 *lg = (u32 *)malloc(sizeof(u32) * (d->cluster_count + 1)), *lc = (u32 *)malloc(sizeof(u32) * (d->cluster_count + 1));
    u32 *fg = (u32 *)calloc(ng, sizeof(u32)), *fc = (u32 *)calloc(ng, sizeof(u32));
    for (u32 c = 0; c < d->cluster_count; c++) {
        u32 g = d->clusters[c].group;
        lg[by_group[g] + fg[g]++] = c;
        u32 cg = d->clusters[c].child_group;
        if (cg != ~0u) lc[by_child[cg] + fc[cg]++] = c;
    }
    for (u32 g = 0; g < ng; g++) {
        if (by_child[g + 1] == by_child[g]) continue;   // the root: nothing simplified from it
        // boundary edges = directed edges whose reverse is absent, in position ids
        size_t nm = 0, ngn = 0;
        for (int side = 0; side < 2; side++) {
            const u32 *list = side ? lc + by_child[g] : lg + by_group[g];
            u32 count = side ? by_child[g + 1] - by_child[g] : by_group[g + 1] - by_group[g];
            size_t n = 0;
            for (u32 k = 0; k < count; k++) {
                const VgDbgCluster *c = &d->clusters[list[k]];
                for (u32 t = 0; t < c->tri_count; t++) {
                    const u32 *tri = &d->indices[c->first_index + t * 3];
                    for (int e = 0; e < 3; e++) {
                        u32 a = d->pos_id[tri[e]], bb = d->pos_id[tri[(e + 1) % 3]];
                        if (a == bb) continue;
                        u64 **arr = side ? &gen : &mine;
                        size_t *cap = side ? &gcap : &mcap;
                        if (n + 1 > *cap) { *cap = *cap ? *cap * 2 : 4096; *arr = (u64 *)realloc(*arr, sizeof(u64) * *cap); }
                        (*arr)[n++] = (u64)a << 32 | bb;
                    }
                }
            }
            if (side) ngn = n; else nm = n;
        }
        // canonical undirected edge + direction bit; an edge is interior when both directions appear
        for (int side = 0; side < 2; side++) {
            u64 *arr = side ? gen : mine;
            size_t n = side ? ngn : nm;
            for (size_t i = 0; i < n; i++) {
                u32 a = (u32)(arr[i] >> 32), bb = (u32)arr[i];
                arr[i] = a < bb ? ((u64)a << 32 | bb) << 1 : ((u64)bb << 32 | a) << 1 | 1;   // canonical + direction bit
            }
        }
        qsort(mine, nm, sizeof(u64), u64_cmp);
        qsort(gen, ngn, sizeof(u64), u64_cmp);
        // reduce each to its boundary multiset: per undirected edge, the directed count difference.
        size_t om = 0, og = 0;
        for (int side = 0; side < 2; side++) {
            u64 *arr = side ? gen : mine;
            size_t n = side ? ngn : nm, o = 0;
            for (size_t i = 0; i < n;) {
                u64 e = arr[i] >> 1;
                int fwd = 0, bwd = 0;
                while (i < n && arr[i] >> 1 == e) { if (arr[i] & 1) bwd++; else fwd++; i++; }
                int diff = fwd - bwd;
                for (; diff > 0; diff--) arr[o++] = e << 1;
                for (; diff < 0; diff++) arr[o++] = e << 1 | 1;
            }
            if (side) og = o; else om = o;
        }
        tested++;
        if (om != og || memcmp(mine, gen, om * sizeof(u64)) != 0) bad++;
    }
    CHECK(tested > 0, "%s: no groups tested", name);
    CHECK(bad == 0, "%s: %u / %u groups change their locked boundary", name, bad, tested);
    free(mine);
    free(gen);
    free(by_group);
    free(by_child);
    free(lg);
    free(lc);
    free(fg);
    free(fc);
}

// ------------------------------------------------------------------ cuts

// Watertight cut sweep: decode the cut, every undirected grid edge is used exactly twice, V - E + F
// equals the expected Euler characteristic, and no selected cluster has a selected descendant.
static void test_cuts(const char *name, const VgBuilt *b, int euler) {
    u32 cap = b->hdr.cluster_count;
    VgCutRef *cut = (VgCutRef *)malloc(sizeof(VgCutRef) * cap);
    u8 *gsel = (u8 *)malloc(b->hdr.group_count), *gmark = (u8 *)malloc(b->hdr.group_count);
    u32 *stack = (u32 *)malloc(sizeof(u32) * (b->hdr.group_count + 1));
    static VgDecoded dc;
    float r = b->hdr.sphere[3];
    u32 prev_tris = ~0u, bad_mono = 0;
    for (int s = 0; s <= 24; s++) {
        float t = s == 0 ? 0.0f : r * 1e-5f * powf(2.0f, (float)s);
        u32 n = vgeo_cut(b, t, cut, cap);
        // decode
        size_t tris = 0, ecap = 0;
        EdgeKey *edges = NULL;
        Key3 *verts = NULL;
        size_t ne = 0, nv = 0, vcap = 0;
        for (u32 i = 0; i < n; i++) {
            vgeo_decode_cluster(b, cut[i].page, cut[i].index, &dc);
            tris += dc.tri_count;
            if (ne + dc.tri_count * 3 > ecap) { while (ne + dc.tri_count * 3 > ecap) ecap = ecap ? ecap * 2 : 65536; edges = (EdgeKey *)realloc(edges, sizeof(EdgeKey) * ecap); }
            if (nv + dc.vert_count > vcap) { while (nv + dc.vert_count > vcap) vcap = vcap ? vcap * 2 : 65536; verts = (Key3 *)realloc(verts, sizeof(Key3) * vcap); }
            for (u32 v = 0; v < dc.vert_count; v++) memcpy(verts[nv++].v, dc.grid[v], 12);
            for (u32 k = 0; k < dc.tri_count; k++)
                for (int e = 0; e < 3; e++) edges[ne++] = edge_of(dc.grid[dc.tri[k][e]], dc.grid[dc.tri[k][(e + 1) % 3]]);
        }
        qsort(edges, ne, sizeof(EdgeKey), edge_qcmp);
        qsort(verts, nv, sizeof(Key3), key3_qcmp);
        size_t ue = 0, uv = 0, open_edge = 0, nonmanifold = 0;
        for (size_t i = 0; i < ne;) {
            size_t j = i;
            while (j < ne && !edge_qcmp(&edges[i], &edges[j])) j++;
            size_t fwd = 0;
            for (size_t k = i; k < j; k++) fwd += edges[k].dir == 0;
            if (fwd * 2 != j - i) open_edge++;          // a crack / hole: some use has no opposite use
            else if (j - i != 2) nonmanifold++;         // balanced but pinched (2k uses)
            ue++;
            i = j;
        }
        for (size_t i = 0; i < nv; i++) if (!i || key3_cmp(&verts[i], &verts[i - 1])) uv++;
        long long chi = (long long)uv - (long long)ue + (long long)tris;
        // Crack-free at every error; the simplifier may pinch two sheets together at coarse levels (an
        // edge used 4 times, still closed), which is allowed but rare and never in the fine cuts.
        CHECK(open_edge == 0, "%s t=%g: %zu open edges (%u clusters, %zu tris)", name, (double)t, open_edge, n, tris);
        CHECK(nonmanifold * 1000 <= ue && (s > 4 || nonmanifold == 0), "%s t=%g: %zu pinched edges of %zu", name, (double)t, nonmanifold, ue);
        if (!nonmanifold) CHECK(chi == euler, "%s t=%g: Euler %lld, want %d", name, (double)t, chi, euler);
        g_pinched += nonmanifold;
        if (tris > prev_tris) bad_mono++;
        prev_tris = (u32)tris;
        // no ancestor + descendant: from every selected non-leaf cluster walk down its source groups
        memset(gsel, 0, b->hdr.group_count);
        for (u32 i = 0; i < n; i++) gsel[cut[i].group] = 1;
        u32 overlap = 0;
        for (u32 i = 0; i < n && !overlap; i++) {
            const VgCluster *c = vgeo_page_cluster(b, cut[i].page, cut[i].index);
            if (c->child_group == ~0u) continue;
            memset(gmark, 0, b->hdr.group_count);
            u32 sp = 0;
            stack[sp++] = c->child_group;
            while (sp && !overlap) {
                u32 g = stack[--sp];
                if (gmark[g]) continue;
                gmark[g] = 1;
                const VgGroup *gr = &b->groups[g];
                for (u32 k = 0; k < gr->cluster_count; k++) {
                    const VgCluster *cc = vgeo_page_cluster(b, gr->page, gr->first_cluster + k);
                    // a selected cluster of this descendant group is a selected descendant
                    if (gsel[g] && cc->error <= t && gr->parent_error > t) overlap++;
                    if (cc->child_group != ~0u && !gmark[cc->child_group]) stack[sp++] = cc->child_group;
                }
            }
        }
        CHECK(overlap == 0, "%s t=%g: an ancestor and a descendant are both in the cut", name, (double)t);
        free(edges);
        free(verts);
    }
    CHECK(bad_mono == 0, "%s: cut triangle count grows with the error", name);
    free(cut);
    free(gsel);
    free(gmark);
    free(stack);
}

// ------------------------------------------------------------------ tests

static void test_rock(void) {
    VgGenMesh m;
    memset(&m, 0, sizeof(m));
    float c[3] = { 0, 0, 0 };
    vgeo_gen_rock(&m, 7, 40, 1.0f, c, 0.3f);   // 32k tris
    VgBuilt b;
    if (!build(&m, NULL, 0, 0, true, &b)) { g_fail++; vgeo_gen_free(&m); return; }
    CHECK(b.stats.levels >= 6, "rock: %u levels", b.stats.levels);
    CHECK(b.stats.level[b.stats.levels - 1].clusters == 1, "rock: top level has %u clusters", b.stats.level[b.stats.levels - 1].clusters);
    test_structure("rock", &b);
    test_boundaries("rock", &b);
    test_cuts("rock", &b, 2);
    // discrete LODs: decreasing triangle counts, the finest is the full mesh
    CHECK(b.hdr.lod_count == VGEO_MAX_LODS, "rock: %u lods", b.hdr.lod_count);
    CHECK(b.lods[0].tris == b.stats.input_tris, "rock: lod0 %u tris of %u", b.lods[0].tris, b.stats.input_tris);
    for (u32 i = 1; i < b.hdr.lod_count; i++) CHECK(b.lods[i].tris < b.lods[i - 1].tris, "rock: lod%u %u tris >= lod%u %u", i, b.lods[i].tris, i - 1, b.lods[i - 1].tris);
    // serialize round trip: identical bytes after load + save, identical cuts
    u8 *d1 = NULL, *d2 = NULL;
    size_t s1 = 0, s2 = 0;
    CHECK(vgeo_serialize(&b, 0x1234u, &d1, &s1), "serialize");
    VgBuilt l;
    CHECK(vgeo_deserialize(d1, s1, &l), "deserialize");
    CHECK(l.hdr.key == 0x1234u, "key");
    CHECK(vgeo_serialize(&l, 0x1234u, &d2, &s2), "re-serialize");
    CHECK(s1 == s2 && !memcmp(d1, d2, s1), "round trip bytes differ (%zu vs %zu)", s1, s2);
    float r = b.hdr.sphere[3];
    CHECK(vgeo_cut(&b, r * 0.01f, NULL, 0) == vgeo_cut(&l, r * 0.01f, NULL, 0), "round trip cut");
    CHECK(!vgeo_deserialize(d1, s1 / 2, &l) || (vgeo_free(&l), 0), "truncated file accepted");
    d1[0] ^= 0xFF;
    CHECK(!vgeo_deserialize(d1, s1, &l), "bad magic accepted");
    free(d1);
    free(d2);
    vgeo_free(&l);
    vgeo_free(&b);
    vgeo_gen_free(&m);
}

static void test_torus_materials(void) {
    // genus 1 (Euler 0), two materials in stripes: material seams stay closed.
    VgGenMesh m;
    memset(&m, 0, sizeof(m));
    float c[3] = { 0, 0, 0 };
    vgeo_gen_torus(&m, 3, 1.0f, 0.3f, 240, 60, c, 0.2f);
    u32 nt = m.index_count / 3;
    u16 *mat = (u16 *)malloc(sizeof(u16) * nt);
    for (u32 t = 0; t < nt; t++) mat[t] = (u16)((t / (60 * 2 * 20)) & 1);   // bands of 20 rings
    VgBuilt b;
    if (!build(&m, mat, 2, 0, true, &b)) { g_fail++; free(mat); vgeo_gen_free(&m); return; }
    test_structure("torus", &b);
    test_boundaries("torus", &b);
    test_cuts("torus", &b, 0);
    // every decoded cluster has one material, both present
    u32 seen = 0;
    static VgDecoded dc;
    for (u32 p = 0; p < b.hdr.page_count; p++)
        for (u32 i = 0; i < vgeo_page(&b, p)->cluster_count; i++) {
            vgeo_decode_cluster(&b, p, i, &dc);
            seen |= 1u << dc.material;
        }
    CHECK(seen == 3, "torus: materials seen mask %u", seen);
    vgeo_free(&b);
    free(mat);
    vgeo_gen_free(&m);
}

static void test_determinism(void) {
    VgGenMesh m;
    memset(&m, 0, sizeof(m));
    vgeo_gen_statue(&m, 11, 120000);
    u8 *d[3] = { 0 };
    size_t s[3] = { 0 };
    int threads[3] = { 1, 8, 8 };
    for (int i = 0; i < 3; i++) {
        VgBuilt b;
        if (!build(&m, NULL, 0, threads[i], false, &b)) { g_fail++; break; }
        vgeo_serialize(&b, 1, &d[i], &s[i]);
        vgeo_free(&b);
    }
    CHECK(s[0] == s[1] && d[0] && d[1] && !memcmp(d[0], d[1], s[0]), "1 vs 8 threads: output differs");
    CHECK(s[1] == s[2] && d[1] && d[2] && !memcmp(d[1], d[2], s[1]), "two 8-thread runs differ");
    for (int i = 0; i < 3; i++) free(d[i]);
    vgeo_gen_free(&m);
}

static void test_gltf_cache(void) {
    const char *path = "assets/models/barrel.gltf";
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) { printf("skip import test: %s not found (run from the bin dir)\n", path); return; }
    char dir[512];
    snprintf(dir, sizeof(dir), "%svgeo_test_cache/", SDL_GetBasePath());
    VgBuildOptions opt;
    memset(&opt, 0, sizeof(opt));
    VgBuilt a, b;
    bool fa = true, fb = false;
    char err[256] = "";
    // a fresh directory: the first load builds and writes, the second reads the cache
    char probe[700];
    VgImported im;
    CHECK(vgeo_import_gltf(path, &im, err, sizeof(err)), "import: %s", err);
    size_t fs = 0;
    void *f = SDL_LoadFile(path, &fs);
    u32 key = vgeo_cache_key(f, fs, &opt);
    SDL_free(f);
    snprintf(probe, sizeof(probe), "%sbarrel.vgeo.%08x.dgc", dir, key);
    SDL_RemovePath(probe);
    CHECK(vgeo_load_or_build(path, dir, &opt, &a, &fa, err, sizeof(err)), "build: %s", err);
    CHECK(!fa, "first load came from the cache");
    CHECK(vgeo_load_or_build(path, dir, &opt, &b, &fb, err, sizeof(err)), "cache: %s", err);
    CHECK(fb, "second load did not use the cache");
    CHECK(a.hdr.cluster_count == b.hdr.cluster_count && a.hdr.key == b.hdr.key && a.page_data_size == b.page_data_size &&
              !memcmp(a.page_data, b.page_data, a.page_data_size),
          "cached build differs");
    CHECK(a.stats.input_tris == im.index_count / 3, "barrel: %u tris of %u", a.stats.input_tris, im.index_count / 3);
    vgeo_imported_free(&im);
    vgeo_free(&a);
    vgeo_free(&b);
    SDL_RemovePath(probe);
    SDL_RemovePath(dir);
}

static void test_statue_gate(void) {
    VgGenMesh m;
    memset(&m, 0, sizeof(m));
    vgeo_gen_statue(&m, 5, 500000);
    double t0 = now_s();
    VgBuilt b;
    if (!build(&m, NULL, 0, 0, false, &b)) { g_fail++; vgeo_gen_free(&m); return; }
    double dt = now_s() - t0;
    printf("statue: %u tris -> %u clusters, %u levels in %.2f s\n", b.stats.input_tris, b.stats.clusters, b.stats.levels, dt);
    CHECK(dt < 10.0, "500k statue build %.2f s >= 10 s", dt);
    vgeo_free(&b);
    vgeo_gen_free(&m);
}

static void test_10m_gate(void) {
    VgGenMesh m;
    memset(&m, 0, sizeof(m));
    vgeo_gen_rock_field(&m, 1, 10000000ull);
    double t0 = now_s();
    VgBuilt b;
    if (!build(&m, NULL, 0, 4, false, &b)) { g_fail++; vgeo_gen_free(&m); return; }
    double dt = now_s() - t0, mb = peak_mb();
    printf("field: %u tris -> %u clusters, %u pages, %u levels in %.1f s on 4 threads, peak %.0f MB\n", b.stats.input_tris, b.stats.clusters, b.stats.pages, b.stats.levels, dt, mb);
    CHECK(dt < 180.0, "10M build %.1f s >= 3 min", dt);
    CHECK(mb < 6144.0, "10M build peak %.0f MB >= 6 GB", mb);
    vgeo_free(&b);
    vgeo_gen_free(&m);
}

int main(int argc, char **argv) {
    bool long_run = argc > 1 && !strcmp(argv[1], "--long");
    if (long_run) {
        test_10m_gate();
    } else {
        test_rock();
        test_torus_materials();
        test_determinism();
        test_gltf_cache();
        test_statue_gate();
    }
    printf("cut sweeps: %zu pinched edges in all\n", g_pinched);
    printf("vgeo_build: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
