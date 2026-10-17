// Water game glue (src/game/game_water.c, docs/WATER.md): the level's water world and its queries
// at a server tick (fractional for rendering), buoyancy for the host's props, commands.
#ifndef DG_GAME_WATER_H
#define DG_GAME_WATER_H

#include "world/water/water.h"
#include "world/world.h"

struct BuoySystem;

void water_register(void);
void water_level_loaded(World *w, const char *map);   // host and clients: same bodies from the same level
void water_tick(World *w);                            // every sim tick (host: buoyancy before phys_step)
void water_frame(World *w, vec3 cam);                 // every frame (clients' wind, debug view)
struct UserCmd;
void water_test_cmd(struct UserCmd *c);               // water_test: scripted input for the local player

const WaterWorld *game_water(void);
WaterWorld *game_water_mut(void);
struct BuoySystem *game_water_buoyancy(void);
const WaterParams *game_water_preset(const char *name);
void game_water_set_level(int body, float level);
// Buoyant bodies owned elsewhere (vehicles): probes in body space, the vehicle sets b->volume as it
// floods. step_one: clients' predicted vehicle (the host steps every body in water_tick).
struct Buoyancy;
struct BuoyProbe;
struct BuoyDesc;
struct Buoyancy *game_water_buoy_attach(World *w, BodyId body, const struct BuoyProbe *probes, int n, const struct BuoyDesc *d);
void game_water_buoy_detach(BodyId body);
struct Buoyancy *game_water_buoy_of(BodyId body);
void game_water_buoy_step_one(World *w, BodyId body);

// Queries at a server tick (host: w->tick + alpha, clients: the render / view tick).
float game_water_time(double tick);
float game_water_height(float x, float z, double tick);   // WATER_NONE without water
float game_water_depth(float x, float z, double tick);    // 0 without water
bool game_water_sample(float x, float z, double tick, WaterSample *s);

#endif
