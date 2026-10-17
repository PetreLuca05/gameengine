// Open-world levels (docs/ZOMBIE_MAP.md): a level JSON with a "world" block (terrain regions, sea,
// rivers, roads) and "pois" (locations) is generated at load by mapgen / mapgen_poi and turned into:
//   - the terrain: a 1 m raster, static heightfield collision tiles (128 m), streamed visual chunks
//     with LODs plus far tiles (chunks.c, custom provider), grass / crops per streamed chunk;
//   - static geometry: the POI primitives batched into meshes per 128 m tile and material, with
//     collision meshes per tile and surface;
//   - trees: placement chunks for the whole map (vgeo foliage, impostors), trunk collision per tile;
//   - entities (host): loot containers rolled from data/loot.json, doors, lamps, pickups; vehicles and
//     fuel pumps (game_vehicles.c); player spawns;
//   - water bodies (sea, lakes, rivers: game_water.c, swimming in movement.c); gameplay rules (host
//     tick): the map edge is walled,
//     and a POI zombie spawner keeps each region's density around the players (and culls far ones).
#ifndef DG_OPENWORLD_H
#define DG_OPENWORLD_H

#include "world.h"

struct cJSON;
struct MapGen;
struct MapPois;

bool openworld_load(World *w, const struct cJSON *root);   // level_load: the "world" / "pois" blocks
void openworld_tick(World *w, float dt);                   // host, every sim tick
void openworld_frame(World *w, vec3 cam_pos);              // every frame (grass streaming around the camera)
void openworld_register_commands(void);
bool openworld_active(const World *w);
const struct MapGen *openworld_gen(const World *w);
const struct MapPois *openworld_pois(const World *w);
// Water surface at (x, z) or -1e9 (no water body). 0 everywhere when the world is not open.
float openworld_water_level(const World *w, float x, float z);
float openworld_ground(const World *w, float x, float z);

#endif
