// Building kit layout (docs/BUILDING_KIT.md "Prefabs"): a level's "kit" instances and "prefabs"
// (assets/kits/building/prefabs/<name>.json, nested, as instance lists, never merged meshes) expanded into
// one flat list of placed pieces with world transforms. Pure: the loader callback reads prefab files;
// src/world/kit_world.c turns the list into draws, collision and entities; tests/test_kit.c tests it.
//
// Instance: {"piece": id, "pos": [x, y, z], "yaw": deg, "name": "...", <entity params>, "mat": {...}}
// Prefab use: {"prefab": name, "pos", "yaw", "prefix": "10c_" (child names), "override": {child name: {params}},
//              "batch": true (static batching: the children merge into big meshes, no entities,
//              no collision unless a child says "collide": true)}
#ifndef DG_KIT_LAYOUT_H
#define DG_KIT_LAYOUT_H

#include "core/base.h"

struct cJSON;

#define KIT_LAYOUT_DEPTH 8

typedef struct KitPlaced {
    char piece[48];
    float pos[3];
    float yaw;                   // degrees
    struct cJSON *entry;         // the instance's parameters (merged overrides; owned by the layout)
    int prefab_depth;            // 0 = placed by the level itself
    bool batch;                  // merged with its kind into one mesh per material ("batch": true on it or an enclosing prefab use)
} KitPlaced;

typedef struct KitLayout {
    KitPlaced *items;
    int n, cap;
    struct cJSON *owned;         // every entry copy lives under this array
    int errors;
    char err[256];               // the first error
} KitLayout;

// Returns the prefab's JSON text (malloc'd; NULL if missing).
typedef char *(*KitPrefabLoader)(void *user, const char *name);

// level: the level JSON root ("kit" and "prefabs" arrays). Errors (missing prefab, depth, no piece) are
// counted; the rest still expands.
void kit_layout_build(KitLayout *L, const struct cJSON *level, KitPrefabLoader load, void *user);
void kit_layout_free(KitLayout *L);
// Rotates (x, z) by yaw degrees the engine's way (yaw 0 looks -Z, positive turns left / counter-clockwise from above).
void kit_rotate_xz(float yaw_deg, float x, float z, float *ox, float *oz);

#endif
