// bake_anim: bakes FBX skeleton / meshes / clips into assets/cache/ and prints a report.
//
//   bake_anim [--rate 30|60] [--rebuild] [--cache <dir>] [--skeleton <rig.fbx>] [files or dirs...]
//
// Defaults: skeleton assets/human/Dummy.fbx, every *.fbx in assets/human/, rate 30, cache assets/cache/.
#include "anim/anim.h"
#include "assets/fbx_loader.h"
#include "assets/anim_cache.h"
#include "core/engine.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && SDL_strcasecmp(s + a - b, suffix) == 0;
}

static Clip g_clips[64];

static void bake_file(const char *path, const Skeleton *skel, float rate) {
    char err[512] = { 0 };
    int n = anim_cache_clips(path, skel, rate, g_clips, 64, err, sizeof(err));
    if (n <= 0) { printf("  %-28s no clips (%s)\n", path, err[0] ? err : "mesh only?"); return; }
    for (int i = 0; i < n; i++) {
        Clip *c = &g_clips[i];
        printf("  %-30s %4d frames @%2.0f Hz  %6.3f s  %s  travel %5.2f m (%4.2f m/s)  yaw %+6.1f deg  markers %s L%.2f R%.2f  hash %08x\n",
               c->name, c->frames, c->rate, c->duration, c->loop ? "loop" : "once", v3_len(c->root_t[c->frames - 1]), c->avg_speed,
               c->root_yaw[c->frames - 1] * DG_RAD2DEG, c->has_markers ? "yes" : "no ", c->plant[0], c->plant[1], c->hash);
        clip_free(c);
    }
}

int main(int argc, char **argv) {
    float rate = 30.0f;
    const char *skel_path = NULL;
    const char *cache = NULL;
    const char *inputs[256];
    int nin = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--rebuild")) anim_cache_set_rebuild(true);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--skeleton") && i + 1 < argc) skel_path = argv[++i];
        else if (nin < 256) inputs[nin++] = argv[i];
    }
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/", cache ? cache : "assets/cache");
    anim_cache_set_dir(cache_dir);
    if (!skel_path) skel_path = "assets/human/Dummy.fbx";
    Skeleton *skel = (Skeleton *)calloc(1, sizeof(Skeleton));
    char err[512] = { 0 };
    if (!anim_cache_skeleton(skel_path, skel, err, sizeof(err))) { fprintf(stderr, "skeleton: %s\n", err); return 1; }
    printf("skeleton %s: %d bones, hash %08x, height %.3f m\n", skel_path, skel->count, skel->hash, skel->height);
    SkeletonAsset *sk = (SkeletonAsset *)calloc(1, sizeof(SkeletonAsset));
    skel_asset_from_skeleton(sk, skel, "human", skel_path, fbx_file_hash(skel_path));
    SkeletalMesh *mesh = (SkeletalMesh *)calloc(1, sizeof(SkeletalMesh));
    if (anim_cache_skm(skel_path, sk, NULL, mesh, err, sizeof(err))) {
        const SkinMeshData *md = &mesh->data;
        printf("mesh: %d LODs, %d bones, %d materials, bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f)\n", md->lod_count, mesh->bone_count, md->material_count,
               md->bounds.min.x, md->bounds.min.y, md->bounds.min.z, md->bounds.max.x, md->bounds.max.y, md->bounds.max.z);
        for (int l = 0; l < md->lod_count; l++)
            printf("  LOD%d: %u verts, %u tris%s\n", l, md->lods[l].vcount, md->lods[l].icount / 3, md->lods[l].generated ? " (generated)" : "");
        for (int m = 0; m < md->material_count; m++)
            printf("  material %d '%s' (%.2f %.2f %.2f)\n", m, md->material_names[m], md->material_colors[m].x, md->material_colors[m].y,
                   md->material_colors[m].z);
        skm_free(mesh);
    } else {
        printf("mesh: %s\n", err);
    }
    free(mesh);
    free(sk);
    if (nin == 0) inputs[nin++] = "assets/human";
    for (int i = 0; i < nin; i++) {
        if (ends_with_ci(inputs[i], ".fbx")) { bake_file(inputs[i], skel, rate); continue; }
        int count = 0;
        char **files = SDL_GlobDirectory(inputs[i], "*.fbx", SDL_GLOB_CASEINSENSITIVE, &count);
        if (!files) { printf("  %s: not found\n", inputs[i]); continue; }
        for (int k = 0; k < count; k++) {
            char p[600];
            snprintf(p, sizeof(p), "%s/%s", inputs[i], files[k]);
            bake_file(p, skel, rate);
        }
        SDL_free(files);
    }
    AnimCacheStats st = anim_cache_stats();
    printf("cache %s: %d hits, %d misses, %d written\n", cache_dir, st.hits, st.misses, st.writes);
    free(skel);
    return 0;
}
