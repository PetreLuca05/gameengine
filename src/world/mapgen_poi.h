// Points of interest of an open-world map (docs/ZOMBIE_MAP.md): the data-driven location list
// ("pois" of a level: id, kind, centre, size, zombie density, loot tier, kind parameters) is planned
// into streets, pads, painted surfaces, keep-outs and building lots before the terrain is finalized
// (mappoi_plan), then built on the finished terrain into primitives (boxes, cylinders, wedges with a
// colour, surface and flags) and entity requests (loot containers, doors, lights, vehicles, zombie
// spots, player spawns) by a small building kit (mappoi_build). Roadside encounters along the road
// network come from the same kit. Pure: the game glue (openworld.c) turns prims into tiled meshes and
// collision and entity requests into entities. Deterministic for a description.
#ifndef DG_MAPGEN_POI_H
#define DG_MAPGEN_POI_H

#include "mapgen.h"

#define MP_MAX_POIS 32
#define MP_NAME 40

typedef enum MpShape { MPS_BOX, MPS_CYL, MPS_WEDGE } MpShape;   // wedge: triangular prism, ridge along local X at the top

enum {
    MPF_COLLIDE = 1 << 0,      // static collision
    MPF_NO_SHADOW = 1 << 1,
    MPF_FOOTPRINT = 1 << 2,    // not drawn: a lot outline (preview, tests)
    MPF_DETAIL = 1 << 3,       // small: drawn within the detail distance only
    MPF_EMISSIVE = 1 << 4,     // lamps, signs
    MPF_GLOSS = 1 << 5,        // low roughness (glass, paint, water-worn)
    MPF_METAL = 1 << 6,
    MPF_GLASS = 1 << 7,        // translucent (windows, chain-link panels)
    MPF_INVISIBLE = 1 << 8,    // collision only (stair ramps)
};

typedef struct MpPrim {
    vec3 pos;                  // centre
    vec3 half;                 // box / wedge half extents; cylinder: x = radius, y = half height, z = top radius / radius
    quat rot;
    u32 color;                 // RGBA8, linear (render.h rgba8)
    u16 flags;
    u8 shape, poi;             // poi: index + 1 (0 = roadside / none)
    const char *surf;          // surface material name (vfx_types.h names: "concrete", "metal_thin", ...)
} MpPrim;

typedef enum MpEntKind {
    MPE_CONTAINER,             // loot container: tier
    MPE_PICKUP,                // item on the floor: item, count
    MPE_DOOR,                  // door in an opening: w, h, locked, key
    MPE_LIGHT,                 // lamp: color, radius, intensity, scheduled
    MPE_VEHICLE,               // drivable vehicle: item = vehicle def id
    MPE_ZOMBIE_SPOT,           // indoor / anchored zombie position (the POI spawner picks among them)
    MPE_SPAWN,                 // player spawn
    MPE_PUMP,                  // fuel pump (vehicles refuel)
    MPE_KIND_COUNT
} MpEntKind;

typedef struct MpEnt {
    u8 kind, poi;              // poi: index + 1 (0 = none)
    bool locked, scheduled;
    vec3 pos;
    float yaw, w, h, radius, intensity;
    vec3 color;
    int count;
    char tier[16];             // container loot tier (data/loot.json)
    char item[24];             // pickup item / vehicle id / door key item
} MpEnt;

typedef struct MpPoi {
    char id[24], name[MP_NAME], kind[16];
    vec2 center, half;         // region (rotated rectangle; yaw)
    float yaw;
    float zombie_density;      // zombies per 10 000 m2 around a player inside the region
    int zombie_max;            // cap near one player
    float indoor;              // fraction of spawns at indoor zombie spots
    char loot[16];             // default container tier
    char ambience[24];         // ambient sound set (informative)
    int enterable;             // planned enterable buildings (stats / tests)
} MpPoi;

// Building lot: what mappoi_build turns into geometry (planned in 2D).
typedef struct MpLot {
    u16 kind;                  // MpLotKind (mapgen_poi.c)
    u8 poi, storeys, flags;    // flags: MPL_ENTERABLE, ...
    u8 variant;
    vec2 pos, size;            // centre, full extents (x along the frontage, z depth)
    float yaw;                 // front faces local +Z rotated by yaw
    u32 seed;
    char tier[16];
} MpLot;

enum { MPL_ENTERABLE = 1, MPL_DAMAGED = 2, MPL_LOCKED = 4, MPL_ROOF = 8 };

typedef struct MapPois {
    MpPoi pois[MP_MAX_POIS];
    int poi_count;
    MpLot *lots;
    int lot_count, lot_cap;
    MpPrim *prims;
    int prim_count, prim_cap;
    MpEnt *ents;
    int ent_count, ent_cap;
    int enterable_count;
    u32 seed;
} MapPois;

struct cJSON;
// Plans every POI (adds streets / pads / paints / keep-outs to g; before mapgen_finalize).
bool mappoi_plan(MapPois *p, MapGen *g, const struct cJSON *pois, char *err, size_t err_size);
// Builds the lots and the roadside encounters on the finished terrain (raster of the same generator).
void mappoi_build(MapPois *p, const MapGen *g, const MgRaster *r);
void mappoi_free(MapPois *p);
int mappoi_find(const MapPois *p, float x, float z);         // POI region containing (x, z), -1 none
const char *mappoi_lot_name(int kind);

#endif
