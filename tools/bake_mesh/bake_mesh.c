// bake_mesh: bakes model FBX files into SkeletalMesh assets (.skm) bound to a Skeleton asset, with the
// compatibility report (replaces / extends validate_rig).
//
//   bake_mesh [--skeleton human] [--out assets/meshes/characters] [--aliases <file.json>]
//             [--report-only] [--write-sidecar] <model.fbx | dir>...
//
// Per file: rig check by bone name (aliases applied: --aliases file, else the model's same-name .json
// sidecar): missing bones (not skinned), extra bones (mesh-only leaves), renamed bones / another
// hierarchy (rejected), proportion warnings, bind-pose deltas, suggested aliases (humanoid name
// mapping, src/anim/retarget.h). --write-sidecar writes the suggestions to <Model>.json when it has
// none, so the model loads as-is next time. Exit code 1 if any file is rejected.
#include "anim/retarget.h"
#include "assets/anim_cache.h"
#include "assets/character.h"
#include "assets/fbx_loader.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

static const SkeletonAsset *g_sk;
static const char *g_out = "assets/meshes/characters";
static const char *g_alias_file;
static bool g_report_only, g_write_sidecar;
static int g_failed, g_files;

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && SDL_strcasecmp(s + a - b, suffix) == 0;
}

static void sidecar_path(const char *fbx, char *out, size_t n) {
    dg_strcpy(out, n, fbx);
    char *dot = strrchr(out, '.');
    if (dot) dg_strcpy(dot, n - (size_t)(dot - out), ".json");
}

static void load_aliases(const char *path, BoneAliasTable *t) {
    char *text = platform_read_file(path, NULL);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    if (root) skm_alias_parse(t, cJSON_GetObjectItemCaseSensitive(root, "aliases"));
    cJSON_Delete(root);
    free(text);
}

static void bake_file(const char *path) {
    g_files++;
    char err[512] = { 0 }, side[512];
    BoneAliasTable *aliases = (BoneAliasTable *)calloc(1, sizeof(BoneAliasTable));
    sidecar_path(path, side, sizeof(side));
    bool have_side = platform_file_exists(side);
    load_aliases(g_alias_file ? g_alias_file : side, aliases);
    Skeleton *ms = (Skeleton *)calloc(1, sizeof(Skeleton)), *aliased = (Skeleton *)calloc(1, sizeof(Skeleton));
    RigReport *r = (RigReport *)calloc(1, sizeof(RigReport));
    if (!fbx_load_skeleton(path, ms, err, sizeof(err))) {
        printf("FAIL %s: %s\n", path, err);
        g_failed++;
        goto done;
    }
    *aliased = *ms;
    skm_alias_apply_skeleton(aliases, aliased);
    bool ok = rig_validate(&g_sk->skel, aliased, 0.25f, r);
    printf("%s %s: %d bones, %d aliases%s\n%s", ok ? "OK  " : "FAIL", path, ms->count, aliases->count, have_side && !g_alias_file ? " (sidecar)" : "", r->text);
    // Suggested aliases: the humanoid name mapping on the file's own names.
    {
        RetargetMap *map = (RetargetMap *)calloc(1, sizeof(RetargetMap));
        BoneAliasTable *sug = (BoneAliasTable *)calloc(1, sizeof(BoneAliasTable));
        retarget_map_skeleton(g_sk, ms, map, sug, NULL, 0);
        int fresh = 0;
        for (int i = 0; i < sug->count; i++) fresh += skm_alias_resolve(aliases, sug->a[i].from) == sug->a[i].from;
        if (fresh) {
            printf("  suggested aliases (add to %s):\n  \"aliases\": {", side);
            for (int i = 0, k = 0; i < sug->count; i++)
                if (skm_alias_resolve(aliases, sug->a[i].from) == sug->a[i].from) printf("%s\n    \"%s\": \"%s\"", k++ ? "," : "", sug->a[i].from, sug->a[i].to);
            printf("\n  }\n");
            if (g_write_sidecar && !have_side) {
                cJSON *root = cJSON_CreateObject(), *al = cJSON_AddObjectToObject(root, "aliases");
                for (int i = 0; i < sug->count; i++) cJSON_AddStringToObject(al, sug->a[i].from, sug->a[i].to);
                char *text = cJSON_Print(root);
                printf("  %s %s\n", platform_write_file(side, text, strlen(text)) ? "wrote" : "FAILED to write", side);
                free(text);
                cJSON_Delete(root);
                *aliases = *sug;
            }
        }
        free(sug);
        free(map);
    }
    if (!ok) { g_failed++; goto done; }
    if (g_report_only) goto done;
    {
        SkeletalMesh *m = (SkeletalMesh *)calloc(1, sizeof(SkeletalMesh));
        if (!fbx_load_skeletal_mesh(path, g_sk, aliases, m, err, sizeof(err))) { printf("FAIL %s: %s\n", path, err); g_failed++; free(m); goto done; }
        m->source_hash = fbx_file_hash(path);
        skin_generate_lods(&m->data);
        skm_compact_bones(m);
        skm_finalize(m, g_sk);
        int skel_bones = m->bone_count - skm_mesh_only_count(m);
        printf("  mesh: %d bones (%d of the skeleton's %d, %d mesh-only), %d aliased, %d skeleton bones missing, bind delta max %.3f m / %.1f deg\n",
               m->bone_count, skel_bones, g_sk->skel.count, skm_mesh_only_count(m), m->aliased, m->missing, m->max_bind_delta_m, m->max_bind_delta_deg);
        for (int b = 0; b < m->bone_count; b++)
            if (m->bones[b].skel < 0) printf("    mesh-only leaf '%s' rides on '%s'\n", m->bones[b].name, m->bones[m->bones[b].parent].name);
        for (int l = 0; l < m->data.lod_count; l++)
            printf("    LOD%d %u verts %u tris%s\n", l, m->data.lods[l].vcount, m->data.lods[l].icount / 3, m->data.lods[l].generated ? " (generated)" : "");
        char stem[64], out[600];
        fbx_file_stem(path, stem, sizeof(stem));
        snprintf(out, sizeof(out), "%s/%s.skm", g_out, stem);
        SDL_CreateDirectory(g_out);
        printf("  %s %s\n", skm_write(m, out) ? "wrote" : "FAILED to write", out);
        skm_free(m);
        free(m);
    }
done:
    free(r);
    free(ms);
    free(aliased);
    free(aliases);
}

int main(int argc, char **argv) {
    const char *skel_id = "human";
    const char *inputs[256];
    int nin = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--skeleton") && i + 1 < argc) skel_id = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) g_out = argv[++i];
        else if (!strcmp(argv[i], "--aliases") && i + 1 < argc) g_alias_file = argv[++i];
        else if (!strcmp(argv[i], "--report-only")) g_report_only = true;
        else if (!strcmp(argv[i], "--write-sidecar")) g_write_sidecar = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: bake_mesh [--skeleton human] [--out dir] [--aliases file.json] [--report-only] [--write-sidecar] <model.fbx | dir>...\n");
            return 0;
        } else if (nin < 256) inputs[nin++] = argv[i];
    }
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    anim_cache_set_dir("assets/cache/");
    SkeletonAsset *sk = (SkeletonAsset *)calloc(1, sizeof(SkeletonAsset));
    char err[512] = { 0 };
    if (!skel_asset_load(sk, skel_id, NULL, err, sizeof(err))) { fprintf(stderr, "skeleton %s: %s\n", skel_id, err); return 2; }
    g_sk = sk;
    printf("skeleton '%s': %d bones, hash %08x\n", skel_id, sk->skel.count, sk->skel.hash);
    if (nin == 0) { printf("no input (a model .fbx or a directory)\n"); return 2; }
    for (int i = 0; i < nin; i++) {
        if (ends_with_ci(inputs[i], ".fbx")) { bake_file(inputs[i]); continue; }
        int count = 0;
        char **files = SDL_GlobDirectory(inputs[i], "*.fbx", SDL_GLOB_CASEINSENSITIVE, &count);
        if (!files) { printf("  %s: not found\n", inputs[i]); continue; }
        for (int k = 0; k < count; k++) {
            char p[600];
            snprintf(p, sizeof(p), "%s/%s", inputs[i], files[k]);
            bake_file(p);
        }
        SDL_free(files);
    }
    printf("%d files, %d failed\n", g_files, g_failed);
    return g_failed ? 1 : 0;
}
