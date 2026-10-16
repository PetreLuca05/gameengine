// handpose_report: lists unreviewed, auto-placed and orphaned hand pose entries (docs/HANDPOSE.md).
// Run from the repository root (or the build's bin/):
//   handpose_report            report (discovery in memory: entries that would be created show as "not saved")
//   handpose_report --create   also write the missing stubs to assets/handposes (never over a file)
//   handpose_report --strict   exit 1 when anything is unreviewed or orphaned (CI)
// Weapons, items, attachments and vehicles are discovered here; interactables are C definitions
// registered by the game, so their entries are read from disk and never orphaned by this tool (the
// in-game "handpose_report" command covers them too).
#include "anim/anim_library.h"
#include "editor/handpose/hp_discover.h"
#include "gameplay/items.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

int main(int argc, char **argv) {
    bool create = false, strict = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--create")) create = true;
        else if (!strcmp(argv[i], "--strict")) strict = true;
        else { printf("usage: handpose_report [--create] [--strict]\n"); return 2; }
    }
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    AnimLibrary *lib = (AnimLibrary *)calloc(1, sizeof(AnimLibrary));
    if (!anim_lib_load(lib, 30.0f)) { printf("handpose_report: animation library failed to load (run from the repo root)\n"); return 2; }
    ItemDB *db = (ItemDB *)calloc(1, sizeof(ItemDB));
    items_load(db, "data/items.json", "data/weapons");
    HandPoseSourceList src = { 0 };
    hp_sources_from_items(&src, db, &lib->proc);
    hp_sources_from_vehicle_dir(&src, "assets/vehicles");
    char dir[600];
    platform_data_path(HANDPOSE_DIR, dir, sizeof(dir));
    HandPoseScanOpts o = { create ? dir : NULL, NULL, false };
    HandPoseScanReport r;
    hp_scan(&lib->handposes, &src, &o, &r);
    printf("discovery: %d sources, %d new entries%s\n", r.sources, r.created, create ? "" : " (not written: --create)");
    if (create && r.written) printf("wrote %d stubs to %s\n", r.written, dir);
    char *buf = (char *)malloc(1 << 17);
    hp_report_text(&lib->handposes, buf, 1 << 17);
    fputs(buf, stdout);
    free(buf);
    hp_sources_free(&src);
    return strict && (r.unreviewed || r.orphans) ? 1 : 0;
}
