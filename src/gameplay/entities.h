// Entity archetypes (doors, gates, levers, switches, lights, containers, pickups, props, dummies)
// with their interaction callbacks and per-tick behaviour.
#ifndef DG_ENTITIES_H
#define DG_ENTITIES_H

#include "world/world.h"

EntityId spawn_door(World *w, vec3 base, float yaw, float width, float height, bool locked, u16 key_item, u32 color);
EntityId spawn_gate(World *w, vec3 base, float yaw, float width, float height, float lift, u32 color);
EntityId spawn_lever(World *w, vec3 base, float yaw, const char *target);
EntityId spawn_switch(World *w, vec3 pos, float yaw, u32 group);
EntityId spawn_light(World *w, vec3 pos, vec3 color, float radius, float intensity, u32 group, bool on);
EntityId spawn_container(World *w, vec3 base, float yaw);
EntityId spawn_container_ex(World *w, vec3 base, float yaw, vec3 half, bool visible);
EntityId spawn_pickup(World *w, vec3 pos, u16 item, int count, i16 ammo, float respawn);
EntityId spawn_bench(World *w, vec3 pos, float yaw);   // weapon bench interaction (attachments UI)
EntityId spawn_prop(World *w, vec3 pos, quat rot, VisShape shape, vec3 half, u32 color, float mass);
EntityId spawn_dummy(World *w, vec3 feet, float yaw, float hp);
EntityId spawn_marker(World *w, vec3 pos, quat rot, VisShape shape, vec3 half, u32 color, bool solid);

// Windows (building kit casements, docs/BUILDING_KIT.md "Windows"): a door whose leaf is glass with the
// colour alpha DOOR_ALPHA_WINDOW (replicated with the colour): "Open Window" prompts, swings out, no door
// wheel / viewer. A lean spot (a marker named "lean" in the opening) proxies the closed window ("Open
// Window") and, open, lets the player lean out (client camera, game_lean.c); one named "lean_over"
// (balcony railings) is always open.
#define DOOR_ALPHA_GLASS 0x60u
#define DOOR_ALPHA_WINDOW 0x40u
EntityId spawn_window(World *w, vec3 base, float yaw, float width, float height, u32 color);
bool door_is_window(const World *w, EntityId e);
EntityId spawn_lean_spot(World *w, vec3 pos, float yaw, bool railing);
bool ent_is_lean_spot(const World *w, EntityId e);
EntityId lean_spot_window(const World *w, EntityId spot);   // the window it opens (nearest within 1.3 m)
bool lean_spot_open(const World *w, EntityId spot);         // its window stands open (a lean is possible)

// Attaches type-specific components (interactable defs, hitboxes, physics) to an entity.
// Called by spawners on the host and by replication on clients.
void entity_setup(World *w, EntityId e);
// Host: per-tick behaviour (door animation, pickups respawn, dummies regen, ...).
void entities_tick(World *w, float dt);
// Replica: drive kinematic bodies from replicated transforms so local prediction collides.
void entities_client_sync(World *w);
// Links lever targets by name once all level entities exist.
void entities_link(World *w);

const InteractDef *interact_def_for(EntityType t);
typedef enum DoorAction { DOOR_ACT_TOGGLE, DOOR_ACT_SLAM } DoorAction;
// Door wheel actions (RPC_DOOR_ACTION, host): toggle = the normal use; slam = 3.5x the speed, loud,
// heard by zombies (horde_noise). False when the player can't (range, locked without the key).
bool door_action(World *w, EntityId door, int player, int action);
// Peek: the visor on the far side of a closed door from `from` (world), looking away from it.
bool door_visor_view(const World *w, EntityId door, vec3 from, vec3 *eye, vec3 *dir);
void entities_set_interact_def(EntityType t, const InteractDef *def);

#endif
