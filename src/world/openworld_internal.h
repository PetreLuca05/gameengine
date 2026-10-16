// Open-world internals shared by openworld.c (terrain, geometry, entities, rules) and
// openworld_foliage.c (trees, grass, crops).
#ifndef DG_OPENWORLD_INTERNAL_H
#define DG_OPENWORLD_INTERNAL_H

#include "openworld.h"
#include "mapgen.h"
#include "mapgen_poi.h"
#include "gameplay/loot.h"

typedef struct OwSpot { vec3 pos; u8 poi; } OwSpot;

typedef struct OwFolChunk { int handle; int cx, cz; } OwFolChunk;

typedef struct OpenWorld {
    MapGen gen;
    MapPois pois;
    MgRaster raster;
    LootTable loot;
    OwSpot *spots;               // indoor zombie spots
    int spot_count;
    // rules
    vec3 last_dry[MAX_PLAYERS];
    bool have_dry[MAX_PLAYERS];
    float spawn_timer;
    float wild_density, despawn_dist, spawn_min, spawn_max, sense;
    int wild_max, zombie_cap;
    int spawned, despawned;
    u32 rng;
    // foliage (openworld_foliage.c)
    OwFolChunk *fol;             // static tree chunks
    int fol_count, fol_loaded;
    int tree_instances, grass_chunks;
    bool fol_types;
} OpenWorld;

void ow_foliage_load(World *w, OpenWorld *o);              // trees for the whole map (+ trunk collision)
void ow_foliage_free(World *w, OpenWorld *o);
void ow_foliage_frame(World *w, OpenWorld *o, vec3 cam);
void *ow_foliage_chunk(void *user, int cx, int cz, size_t *size);   // grass / crops of a streamed chunk (thread-safe)
float ow_water_fn(void *user, float x, float z);

#endif
