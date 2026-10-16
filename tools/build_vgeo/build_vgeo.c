// build_vgeo: offline virtualized-geometry build (docs/VGEO.md "Builder").
//
//   build_vgeo <mesh.gltf> [--out file.vgeo] [--stats] [--threads N]
//   build_vgeo --gen rock|field|statue|torus|building [--tris N] [--seed S] [--out file.vgeo] [--stats] [--threads N]
//
// Prints the build report (levels, clusters, triangles, error ranges, pages, ms per stage) with
// --stats, plus wall time and the process peak memory. Generated meshes are never committed: tests and
// the build produce them with this tool.
#include "render/vgeo/vgeo_build.h"
#include "render/vgeo/vgeo_import.h"
#include "render/vgeo/vgeo_procgen.h"
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

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *gen = NULL;
    bool stats = false;
    int threads = 0;
    u64 tris = 0;
    u32 seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--stats")) stats = true;
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gen") && i + 1 < argc) gen = argv[++i];
        else if (!strcmp(argv[i], "--tris") && i + 1 < argc) tris = (u64)atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (u32)atoi(argv[++i]);
        else if (argv[i][0] != '-') in = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!in && !gen) {
        fprintf(stderr, "usage: build_vgeo <mesh.gltf> | --gen rock|field|statue|torus|building [--tris N] [--seed S] [--out f] [--stats] [--threads N]\n");
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
    if (gen) {
        VgGenMesh m;
        memset(&m, 0, sizeof(m));
        float c[3] = { 0, 0, 0 };
        if (!strcmp(gen, "rock")) vgeo_gen_rock(&m, seed, (u32)SDL_sqrt((double)(tris ? tris : 80000) / 20.0), 1.0f, c, 0.3f);
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
        printf("generated %s: %u triangles, %u vertices\n", gen, m.index_count / 3, m.vertex_count);
        ok = vgeo_build(&src, &opt, &b, err, sizeof(err));
        key = vgeo_fnv1a(gen, strlen(gen), seed);
        vgeo_gen_free(&m);
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
