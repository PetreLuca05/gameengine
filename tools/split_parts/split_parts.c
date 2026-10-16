// split_parts: cuts a full-body mesh into modular part meshes (.skm) by skeleton bone groups, for art
// that isn't split yet. Each triangle has one owner bone, so disjoint groups tile the body exactly.
//
//   split_parts [--skeleton human] [--regions part_head,part_torso,part_hands,part_legs] [--out dir] <mesh.fbx | mesh.skm>
//
// (Characters can also cut at load time: "region" on a part in assets/characters/*.json.)
#include "anim/skeleton.h"
#include "assets/anim_cache.h"
#include "assets/fbx_loader.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

int main(int argc, char **argv) {
    const char *skel_id = "human", *regions = "part_head,part_torso,part_hands,part_legs", *out_dir = "assets/meshes/characters", *input = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--skeleton") && i + 1 < argc) skel_id = argv[++i];
        else if (!strcmp(argv[i], "--regions") && i + 1 < argc) regions = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else input = argv[i];
    }
    if (!input) {
        printf("usage: split_parts [--skeleton human] [--regions a,b,c] [--out dir] <mesh.fbx | mesh.skm>\n");
        return 2;
    }
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    anim_cache_set_dir("assets/cache/");
    SkeletonAsset *sk = (SkeletonAsset *)calloc(1, sizeof(SkeletonAsset));
    SkeletalMesh *src = (SkeletalMesh *)calloc(1, sizeof(SkeletalMesh)), *part = (SkeletalMesh *)calloc(1, sizeof(SkeletalMesh));
    char err[512] = { 0 };
    if (!skel_asset_load(sk, skel_id, NULL, err, sizeof(err))) { fprintf(stderr, "skeleton: %s\n", err); return 2; }
    size_t len = strlen(input);
    bool ok = len > 4 && !SDL_strcasecmp(input + len - 4, ".skm") ? skm_read(src, input, err, sizeof(err)) : anim_cache_skm(input, sk, NULL, src, err, sizeof(err));
    if (!ok) { fprintf(stderr, "%s: %s\n", input, err); return 1; }
    char stem[64];
    fbx_file_stem(input, stem, sizeof(stem));
    SDL_CreateDirectory(out_dir);
    u32 total = 0;
    int failed = 0;
    char list[512];
    dg_strcpy(list, sizeof(list), regions);
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        int g = skel_asset_find_group(sk, tok);
        if (g < 0) { printf("  %s: no such bone group in skeleton '%s'\n", tok, skel_id); failed++; continue; }
        if (!skm_extract_region(src, &sk->groups[g].bones, part)) { printf("  %s: no triangles\n", tok); continue; }
        skm_finalize(part, sk);
        total += part->data.lods[0].icount / 3;
        char out[600];
        snprintf(out, sizeof(out), "%s/%s_%s.skm", out_dir, stem, tok);
        printf("  %-12s %6u tris, %2d bones -> %s %s\n", tok, part->data.lods[0].icount / 3, part->bone_count, out, skm_write(part, out) ? "" : "(WRITE FAILED)");
        skm_free(part);
    }
    u32 src_tris = src->data.lods[0].icount / 3;
    printf("%s: %u tris, parts %u tris (%s)\n", input, src_tris, total, total == src_tris ? "exact tiling" : "regions overlap or leave gaps");
    return failed ? 1 : 0;
}
