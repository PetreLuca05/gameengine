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
EntityId spawn_pickup(World *w, vec3 pos, u16 item, int count, i16 ammo, float respawn);
EntityId spawn_prop(World *w, vec3 pos, quat rot, VisShape shape, vec3 half, u32 color, float mass);
EntityId spawn_dummy(World *w, vec3 feet, float yaw, float hp);
EntityId spawn_marker(World *w, vec3 pos, quat rot, VisShape shape, vec3 half, u32 color, bool solid);

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

#endif
