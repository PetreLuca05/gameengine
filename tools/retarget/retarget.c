// retarget: maps another humanoid rig onto the canonical Skeleton (Unity Avatar-style), so a store
// model / clip can be used without re-rigging.
//
//   retarget [--skeleton human] [--write-sidecar] [--skm <out.skm>] <model.fbx>
//
// Name mapping (exact, synonyms: UE mannequin / Unity humanoid / Bip, end-bone patterns) -> an alias
// table; base-pose correction per bone (source bind -> the Skeleton's retarget base pose). With
// --write-sidecar the aliases go to <model>.json next to the FBX (the game then loads it as-is);
// --skm bakes the compatible SkeletalMesh. Clip retargeting is stubbed (retarget_clip).
#include "anim/retarget.h"
#include "assets/character.h"
#include "assets/anim_cache.h"
#include "assets/fbx_loader.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

int main(int argc, char **argv) {
    const char *skel_id = "human", *input = NULL, *skm_out = NULL;
    bool write_side = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--skeleton") && i + 1 < argc) skel_id = argv[++i];
        else if (!strcmp(argv[i], "--skm") && i + 1 < argc) skm_out = argv[++i];
        else if (!strcmp(argv[i], "--write-sidecar")) write_side = true;
        else input = argv[i];
    }
    if (!input) { printf("usage: retarget [--skeleton human] [--write-sidecar] [--skm out.skm] <model.fbx>\n"); return 2; }
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    anim_cache_set_dir("assets/cache/");
    SkeletonAsset *sk = (SkeletonAsset *)calloc(1, sizeof(SkeletonAsset));
    Skeleton *src = (Skeleton *)calloc(1, sizeof(Skeleton)), *aliased = (Skeleton *)calloc(1, sizeof(Skeleton));
    RetargetMap *map = (RetargetMap *)calloc(1, sizeof(RetargetMap));
    BoneAliasTable *al = (BoneAliasTable *)calloc(1, sizeof(BoneAliasTable));
    RigReport *rr = (RigReport *)calloc(1, sizeof(RigReport));
    char err[512] = { 0 }, report[8192];
    if (!skel_asset_load(sk, skel_id, NULL, err, sizeof(err))) { fprintf(stderr, "skeleton: %s\n", err); return 2; }
    if (!fbx_load_skeleton(input, src, err, sizeof(err))) { fprintf(stderr, "%s: %s\n", input, err); return 1; }
    retarget_map_skeleton(sk, src, map, al, report, sizeof(report));
    printf("%s: %d bones, height %.3f m (skeleton %.3f m, ratio %.3f)\n%s", input, src->count, src->height, sk->skel.height, map->height_ratio, report);
    // Base-pose correction summary.
    float worst = 0;
    int worst_bone = -1;
    for (int c = 0; c < sk->skel.count; c++) {
        if (map->src_of[c] < 0) continue;
        float a = quat_angle_between(map->correction[c], quat_identity()) * DG_RAD2DEG;
        if (a > worst) { worst = a; worst_bone = c; }
    }
    printf("base-pose correction: largest %.1f deg (%s)\n", worst, worst_bone >= 0 ? sk->skel.names[worst_bone] : "-");
    *aliased = *src;
    skm_alias_apply_skeleton(al, aliased);
    bool ok = rig_validate(&sk->skel, aliased, 0.25f, rr);
    printf("compatibility after aliasing: %s", rr->text);
    if (write_side && al->count) {
        char side[512];
        dg_strcpy(side, sizeof(side), input);
        char *dot = strrchr(side, '.');
        if (dot) dg_strcpy(dot, sizeof(side) - (size_t)(dot - side), ".json");
        cJSON *root = cJSON_CreateObject(), *a = cJSON_AddObjectToObject(root, "aliases");
        for (int i = 0; i < al->count; i++) cJSON_AddStringToObject(a, al->a[i].from, al->a[i].to);
        char *text = cJSON_Print(root);
        printf("%s %s\n", platform_write_file(side, text, strlen(text)) ? "wrote" : "FAILED to write", side);
        free(text);
        cJSON_Delete(root);
    }
    if (skm_out && ok) {
        SkeletalMesh *m = (SkeletalMesh *)calloc(1, sizeof(SkeletalMesh));
        if (fbx_load_skeletal_mesh(input, sk, al, m, err, sizeof(err))) {
            skin_generate_lods(&m->data);
            skm_compact_bones(m);
            skm_finalize(m, sk);
            printf("%s %s (%d bones, %d mesh-only)\n", skm_write(m, skm_out) ? "wrote" : "FAILED to write", skm_out, m->bone_count, skm_mesh_only_count(m));
            skm_free(m);
        } else {
            printf("mesh: %s\n", err);
        }
        free(m);
    }
    printf("clips: retarget_clip is a stub (name mapping + base-pose correction are in; per-frame clip import not yet)\n");
    return ok ? 0 : 1;
}
