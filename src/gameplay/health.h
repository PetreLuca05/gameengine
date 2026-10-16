// Health, armor, damage types, death and respawn.
#ifndef DG_HEALTH_H
#define DG_HEALTH_H

#include "world/world.h"

#define ARMOR_ABSORB 0.6f    // fraction of damage soaked by armor while it lasts
#define PLAYER_RESPAWN_TIME 3.0f

void health_init(Health *h, float hp, float armor);
// Applies damage on the authority. Returns the amount of health removed.
float apply_damage(World *w, EntityId target, float amount, DamageType type, EntityId attacker, vec3 point, vec3 dir, bool headshot);
// Body-part aware variant (amount already multiplied): part = BodyPart or -1, part_mult for dev numbers.
float apply_damage_part(World *w, EntityId target, float amount, DamageType type, EntityId attacker, vec3 point, vec3 dir, int part, float part_mult);
void heal(World *w, EntityId target, float hp, float armor);
void health_tick_dummy(World *w, EntityId e, float dt);

#endif
