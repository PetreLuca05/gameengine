// validate_rig: checks model FBX skeletons against the canonical rig (assets/human/Dummy.fbx).
//
//   validate_rig [--canon <rig.fbx>] [--tolerance 0.25] <model.fbx | dir>...
//
// Per file: identical / subset (missing bones take the canonical bind) / extra leaf bones are OK;
// renamed or re-parented bones and missing required bones fail. Proportion differences beyond
// the tolerance (relative bone length) are warnings. Exit code 1 if any file fails.
#include "anim/anim.h"
#include "assets/fbx_loader.h"
#include "assets/character.h"
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

static int g_failed, g_files;

static void check_file(const char *path, const Skeleton *canon, float tol) {
    Skeleton *s = (Skeleton *)calloc(1, sizeof(Skeleton));
    char err[512] = { 0 };
    g_files++;
    if (!fbx_load_skeleton(path, s, err, sizeof(err))) {
        printf("FAIL %s: %s\n", path, err);
        g_failed++;
        free(s);
        return;
    }
    RigReport *r = (RigReport *)calloc(1, sizeof(RigReport));
    bool ok = rig_validate(canon, s, tol, r);
    const char *kind = r->identical ? "identical" : (r->missing ? (r->extra ? "subset + extra" : "subset") : (r->extra ? "extra bones" : "compatible"));
    printf("%s %s: %d bones, %s (missing %d, extra %d, hierarchy errors %d, proportion warnings %d)\n", ok ? "OK  " : "FAIL", path, s->count, kind,
           r->missing, r->extra, r->hierarchy_errors, r->proportion_warnings);
    if (r->len && !r->identical) printf("%s", r->text);
    if (!ok) g_failed++;
    free(r);
    free(s);
}

int main(int argc, char **argv) {
    const char *canon_path = "assets/human/Dummy.fbx";
    float tol = 0.25f;
    const char *inputs[256];
    int nin = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--canon") && i + 1 < argc) canon_path = argv[++i];
        else if (!strcmp(argv[i], "--tolerance") && i + 1 < argc) tol = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: validate_rig [--canon <rig.fbx>] [--tolerance 0.25] <model.fbx | dir>...\n");
            return 0;
        } else if (nin < 256) inputs[nin++] = argv[i];
    }
    Skeleton *canon = (Skeleton *)calloc(1, sizeof(Skeleton));
    char err[512] = { 0 };
    if (!fbx_load_skeleton(canon_path, canon, err, sizeof(err))) { fprintf(stderr, "canonical rig %s: %s\n", canon_path, err); return 2; }
    printf("canonical rig %s: %d bones, hash %08x, height %.3f m\n", canon_path, canon->count, canon->hash, canon->height);
    if (nin == 0) inputs[nin++] = "assets/human";
    for (int i = 0; i < nin; i++) {
        if (ends_with_ci(inputs[i], ".fbx")) { check_file(inputs[i], canon, tol); continue; }
        int count = 0;
        char **files = SDL_GlobDirectory(inputs[i], "*.fbx", SDL_GLOB_CASEINSENSITIVE, &count);
        if (!files) { printf("  %s: not found\n", inputs[i]); continue; }
        for (int k = 0; k < count; k++) {
            char p[600];
            snprintf(p, sizeof(p), "%s/%s", inputs[i], files[k]);
            check_file(p, canon, tol);
        }
        SDL_free(files);
    }
    printf("%d files, %d failed\n", g_files, g_failed);
    return g_failed ? 1 : 0;
}
