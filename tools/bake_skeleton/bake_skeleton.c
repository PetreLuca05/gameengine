// bake_skeleton: bakes the Skeleton asset (rig FBX + assets/skeletons/<id>.json metadata) into
// assets/skeletons/<id>.skel and prints it (bones, sockets, virtual bones, groups, LOD sets, hashes).
//
//   bake_skeleton [--id human] [--source <rig.fbx>] [--out <file.skel>] [--list]
//
// The bake is deterministic (checked: two bakes give the same bytes). The runtime uses the .skel when
// its source hash matches the FBX (or the FBX isn't shipped), else bakes through assets/cache/.
#include "anim/skeleton.h"
#include "assets/anim_cache.h"
#include "assets/fbx_loader.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

static bool bake(const char *id, const char *source, SkeletonAsset *a, char *err, int n) {
    char rel[256], buf[512];
    snprintf(rel, sizeof(rel), "assets/skeletons/%s.json", id);
    char *meta = platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
    Skeleton *s = (Skeleton *)calloc(1, sizeof(Skeleton));
    const char *fbx = platform_data_path(source, buf, sizeof(buf));
    bool ok = fbx_load_skeleton(fbx, s, err, n);   // straight from the FBX (no cache)
    if (ok) {
        skel_asset_from_skeleton(a, s, id, source, fbx_file_hash(fbx));
        if (meta) skel_asset_apply_meta(a, meta, err, n);
        else skel_asset_apply_meta(a, "{}", err, n);
    }
    free(s);
    free(meta);
    return ok;
}

int main(int argc, char **argv) {
    const char *id = "human", *source = NULL, *out = NULL;
    bool list = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--id") && i + 1 < argc) id = argv[++i];
        else if (!strcmp(argv[i], "--source") && i + 1 < argc) source = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--list")) list = true;
        else { printf("usage: bake_skeleton [--id human] [--source <rig.fbx>] [--out <file.skel>] [--list]\n"); return 0; }
    }
    log_init(NULL);
    char rel[256], buf[512], src[256] = "assets/human/Dummy.fbx";
    if (source) dg_strcpy(src, sizeof(src), source);
    else {
        // The JSON names the rig.
        snprintf(rel, sizeof(rel), "assets/skeletons/%s.json", id);
        char *meta = platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
        const char *k = meta ? strstr(meta, "\"source\"") : NULL;
        if (k && (k = strchr(k + 8, '"'))) {
            const char *e = strchr(k + 1, '"');
            if (e && e - k - 1 < (int)sizeof(src)) { memcpy(src, k + 1, (size_t)(e - k - 1)); src[e - k - 1] = 0; }
        }
        free(meta);
    }
    SkeletonAsset *a = (SkeletonAsset *)calloc(1, sizeof(SkeletonAsset)), *b = (SkeletonAsset *)calloc(1, sizeof(SkeletonAsset));
    char err[1024] = { 0 };
    if (!bake(id, src, a, err, sizeof(err)) || !bake(id, src, b, err, sizeof(err))) { fprintf(stderr, "bake_skeleton: %s\n", err); return 1; }
    if (err[0]) printf("warnings:\n%s", err);
    u8 *pa, *pb;
    size_t na, nb;
    skel_asset_serialize(a, &pa, &na);
    skel_asset_serialize(b, &pb, &nb);
    bool det = na == nb && !memcmp(pa, pb, na);
    const Skeleton *s = &a->skel;
    printf("skeleton '%s' from %s: %d bones, height %.3f m, bones hash %08x, asset hash %08x, %zu bytes, deterministic: %s\n", id, src, s->count, s->height,
           s->hash, skel_asset_hash(a), na, det ? "yes" : "NO");
    printf("  %d sockets, %d virtual bones, %d groups; LOD bone sets:", a->socket_count, a->vbone_count, a->group_count);
    for (int l = 0; l < SKEL_LOD_COUNT; l++) printf(" %d", boneset_count(&a->lod_bones[l]));
    printf("\n");
    if (list) {
        for (int i = 0; i < s->count; i++) printf("  bone %2d %-36s parent %2d%s\n", i, s->names[i], s->parent[i], s->lod_keep[i] ? "" : "  (LOD2+ folded)");
        for (int i = 0; i < a->socket_count; i++)
            printf("  socket %-12s on %-24s (%.3f %.3f %.3f)%s\n", a->sockets[i].name, a->sockets[i].bone >= 0 ? s->names[a->sockets[i].bone] : "?",
                   a->sockets[i].offset.x, a->sockets[i].offset.y, a->sockets[i].offset.z, a->sockets[i].bind_space ? " bind" : " bone");
        for (int i = 0; i < a->vbone_count; i++) printf("  virtual bone %-22s %s -> %s\n", a->vbones[i].name, a->vbones[i].source_name, a->vbones[i].target_name);
        for (int i = 0; i < a->group_count; i++) printf("  group %-12s %d bones\n", a->groups[i].name, boneset_count(&a->groups[i].bones));
    }
    if (!out) { snprintf(rel, sizeof(rel), "assets/skeletons/%s.skel", id); out = rel; }
    bool ok = det && platform_write_file(out, pa, na);
    printf("%s %s\n", ok ? "wrote" : "FAILED to write", out);
    free(pa);
    free(pb);
    return ok ? 0 : 1;
}
