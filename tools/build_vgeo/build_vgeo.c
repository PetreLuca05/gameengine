// build_vgeo: offline virtualized-geometry build (docs/VGEO.md "Builder").
//
//   build_vgeo <mesh.gltf> [--out file.vgeo] [--stats] [--threads N] [--impostor [size] [grid]]
//   build_vgeo --gen rock|field|statue|torus|building|creature|tuft|bush [--tris N] [--seed S] [--out file.vgeo] [--stats] [--threads N]
//              [--impostor [size] [grid]]
//   build_vgeo --trees <tree dir/> <out dir/>
//
// --impostor bakes the hemi-octahedral impostor atlas (grid x grid frames in a size^2 atlas, default
// 1024 / 8) into the file on the CPU (vgeo_impostor.h): glTF and plain generated meshes in white,
// the foliage generators (tuft, bush) with their materials and alpha masks.
// --trees imports every tree species (vgeo_tree.h) from the tree directory and writes
// <out dir>/<name>.dgtree (clusters, DAG, impostor, materials, capsule), the species in parallel.
// Prints the build report (levels, clusters, triangles, error ranges, pages, ms per stage) with
// --stats, plus wall time and the process peak memory. Generated meshes are never committed: tests and
// the build produce them with this tool.
#include "render/vgeo/vgeo_build.h"
#include "render/vgeo/vgeo_import.h"
#include "render/vgeo/vgeo_procgen.h"
#include "render/vgeo/vgeo_procgen_skinned.h"
#include "render/vgeo/vgeo_procgen_foliage.h"
#include "render/vgeo/vgeo_impostor.h"
#include "render/vgeo/vgeo_tree.h"
#include "core/engine.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
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

Engine g_engine;

static double peak_mb(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
    return 0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_maxrss / 1024.0;
#endif
}

static void print_line(const char *l) { printf("%s\n", l); }

typedef struct TreeJob { const char *dir, *out; SDL_AtomicInt next; int failed; } TreeJob;

static int tree_thread(void *u) {
    TreeJob *j = (TreeJob *)u;
    for (;;) {
        u32 i = (u32)SDL_AddAtomicInt(&j->next, 1);
        const VgTreeSpecies *sp = vgeo_tree_species(i);
        if (!sp) return 0;
        double t0 = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
        VgTree t;
        char err[512] = "";
        if (!vgeo_tree_load_or_build(sp, j->dir, NULL, NULL, &t, err, sizeof(err))) {
            fprintf(stderr, "%s: %s\n", sp->name, err);
            j->failed = 1;
            continue;
        }
        char path[700];
        snprintf(path, sizeof(path), "%s%s.dgtree", j->out, sp->name);
        if (!vgeo_tree_write(&t, t.b.hdr.key, path)) { fprintf(stderr, "cannot write %s\n", path); j->failed = 1; }
        char mats[256] = "";
        for (u32 k = 0; k < t.material_count; k++)
            snprintf(mats + strlen(mats), sizeof(mats) - strlen(mats), "%s%s%s", k ? ", " : "", t.tex[k], t.masked[k] ? " (masked)" : "");
        printf("%-14s %6u tris, %5u clusters, %2u levels, lod tris %u/%u/%u/%u, scale %.4f (%.1f -> %.1f m), capsule r %.2f h %.1f, %s; %.2f s\n", sp->name,
               t.b.stats.input_tris, t.b.stats.clusters, t.b.stats.levels, t.b.lods[0].tris, t.b.lods[1].tris, t.b.lods[2].tris, t.b.lods[3].tris, (double)t.scale,
               (double)t.src_height, (double)vgeo_tree_height(sp), (double)t.cap_r, (double)t.cap_h, mats,
               (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency() - t0);
        vgeo_tree_free(&t);
    }
}

static int build_trees(const char *dir, const char *out) {
    TreeJob j;
    memset(&j, 0, sizeof(j));
    j.dir = dir;
    j.out = out;
    SDL_Thread *th[16];
    int n = DG_CLAMP(SDL_GetNumLogicalCPUCores(), 1, 16);
    for (int i = 0; i < n; i++) th[i] = SDL_CreateThread(tree_thread, "tree", &j);
    for (int i = 0; i < n; i++) SDL_WaitThread(th[i], NULL);
    printf("build_vgeo: %u tree species, peak memory %.0f MB\n", vgeo_tree_species_count(), peak_mb());
    return j.failed;
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *gen = NULL;
    bool stats = false;
    int threads = 0;
    u64 tris = 0;
    u32 seed = 1, imp_size = 0, imp_grid = 0;
    bool impostor = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--impostor")) {
            impostor = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') imp_size = (u32)atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') imp_grid = (u32)atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--trees") && i + 2 < argc) return build_trees(argv[i + 1], argv[i + 2]);
        else if (!strcmp(argv[i], "--stats")) stats = true;
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gen") && i + 1 < argc) gen = argv[++i];
        else if (!strcmp(argv[i], "--tris") && i + 1 < argc) tris = (u64)atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (u32)atoi(argv[++i]);
        else if (argv[i][0] != '-') in = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!in && !gen) {
        fprintf(stderr, "usage: build_vgeo <mesh.gltf> | --gen rock|field|statue|torus|building|creature|tuft|bush [--tris N] [--seed S] [--out f] [--stats] [--threads N] "
                        "[--impostor [size] [grid]]\n");
        return 2;
    }
    VgBuildOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.threads = threads;
    VgBuilt b;
    char err[512] = "";
    double t0 = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    bool ok;
    u32 key = 0;
    int foliage = gen && !strcmp(gen, "tuft") ? VG_FOLIAGE_TUFT : gen && !strcmp(gen, "bush") ? VG_FOLIAGE_BUSH : -1;
    if (foliage >= 0) {
        VgFoliageMesh fm;
        vgeo_gen_foliage(&fm, foliage, seed);
        VgSource src;
        vgeo_foliage_source(&fm, &src);
        printf("generated %s: %u triangles, %u vertices, %u materials\n", gen, fm.index_count / 3, fm.vertex_count, fm.material_count);
        ok = vgeo_build(&src, &opt, &b, err, sizeof(err));
        key = vgeo_fnv1a(gen, strlen(gen), seed);
        vgeo_foliage_mesh_free(&fm);
    } else if (gen) {
        VgGenMesh m;
        memset(&m, 0, sizeof(m));
        float c[3] = { 0, 0, 0 };
        VgGenSkinned sk;
        memset(&sk, 0, sizeof(sk));
        if (!strcmp(gen, "creature")) {   // skinned: body + 5 tentacles of 6 bones
            vgeo_gen_creature(&sk, seed, 5, 6, tris ? tris : 30000ull);
            m = sk.mesh;
        } else if (!strcmp(gen, "rock")) vgeo_gen_rock(&m, seed, (u32)SDL_sqrt((double)(tris ? tris : 80000) / 20.0), 1.0f, c, 0.3f);
        else if (!strcmp(gen, "field")) vgeo_gen_rock_field(&m, seed, tris ? tris : 10000000ull);
        else if (!strcmp(gen, "statue")) vgeo_gen_statue(&m, seed, tris ? tris : 500000ull);
        else if (!strcmp(gen, "building")) vgeo_gen_building(&m, seed, tris ? tris : 2000000ull);
        else if (!strcmp(gen, "torus")) {
            u32 s = (u32)SDL_sqrt((double)(tris ? tris : 50000) / 2.0);
            vgeo_gen_torus(&m, seed, 1.0f, 0.3f, s * 2, s / 2 + 3, c, 0.2f);
        } else { fprintf(stderr, "unknown generator %s\n", gen); return 2; }
        VgSource src;
        memset(&src, 0, sizeof(src));
        src.vertex_count = m.vertex_count;
        src.pos = m.pos;
        src.index_count = m.index_count;
        src.indices = m.indices;
        src.joints = sk.joints;
        src.weights = sk.weights;
        printf("generated %s: %u triangles, %u vertices\n", gen, m.index_count / 3, m.vertex_count);
        ok = vgeo_build(&src, &opt, &b, err, sizeof(err));
        key = vgeo_fnv1a(gen, strlen(gen), seed);
        if (sk.joints) vgeo_gen_skinned_free(&sk);
        else vgeo_gen_free(&m);
    } else {
        VgImported im;
        ok = vgeo_import_gltf(in, &im, err, sizeof(err));
        if (ok) {
            VgSource src;
            vgeo_imported_source(&im, &src);
            ok = vgeo_build(&src, &opt, &b, err, sizeof(err));
            vgeo_imported_free(&im);
            size_t fs = 0;
            char *f = platform_read_file(in, &fs);
            if (f) { key = vgeo_cache_key(f, fs, &opt); free(f); }
        }
    }
    double t1 = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    if (!ok) { fprintf(stderr, "build failed: %s\n", err); return 1; }
    if (impostor) {
        VgImpostorOptions io;
        memset(&io, 0, sizeof(io));
        io.size = imp_size;
        io.grid = imp_grid;
        VgImpostorMaterial im[2];
        u8 *masks[2] = { NULL, NULL };
        const u32 msize = 256;
        if (foliage >= 0) {
            for (u32 k = 0; k < 2; k++) {
                masks[k] = (u8 *)malloc(msize * msize * 4);
                if (!vgeo_gen_foliage_mask(masks[k], msize, foliage, k, seed)) { free(masks[k]); masks[k] = NULL; }
            }
            vgeo_foliage_impostor_materials(foliage, (const u8 *const *)masks, msize, im);
            io.materials = im;
            io.material_count = 2;
        }
        double i0 = (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
        if (!vgeo_impostor_bake(&b, &io)) { fprintf(stderr, "impostor bake failed (size must be a multiple of grid)\n"); return 1; }
        const VgImpostorHeader *ih = vgeo_impostor(&b, NULL, NULL);
        printf("impostor: %u x %u frames in a %u px atlas in %.2f s\n", ih->grid, ih->grid, ih->size,
               (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency() - i0);
        free(masks[0]);
        free(masks[1]);
    }
    if (stats) vgeo_print_stats(&b.stats, print_line);
    printf("build_vgeo: %u tris -> %u clusters, %u pages, %u levels in %.2f s (%d threads), peak memory %.0f MB\n", b.stats.input_tris, b.stats.clusters, b.stats.pages,
           b.stats.levels, t1 - t0, b.stats.threads, peak_mb());
    if (out) {
        u8 *data = NULL;
        size_t size = 0;
        vgeo_serialize(&b, key, &data, &size);
        if (!platform_write_file(out, data, size)) { fprintf(stderr, "cannot write %s\n", out); return 1; }
        printf("wrote %s (%.2f MB)\n", out, (double)size / (1024.0 * 1024.0));
        free(data);
    }
    vgeo_free(&b);
    return 0;
}
