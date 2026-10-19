// Health, armor, damage types, death and respawn.
#ifndef DG_HEALTH_H
#define DG_HEALTH_H

#include "world/world.h"

#define ARMOR_ABSORB 0.6f    // fraction of damage soaked by armor while it lasts
#define ARMOR_PER_PLATE 50.0f   // armor points of one plate (docs/DAMAGE.md "Armor")
#define ARMOR_BASE_PLATES 1     // plates a player holds without a plate carrier (its "plates" with one)
#define HELMET_ABSORB 0.5f   // fraction of a head hit a helmet takes while its durability lasts
#define PLAYER_RESPAWN_TIME 3.0f

void health_init(Health *h, float hp, float armor);
// Applies damage on the authority. Returns the amount of health removed.
float apply_damage(World *w, EntityId target, float amount, DamageType type, EntityId attacker, vec3 point, vec3 dir, bool headshot);
// Body-part aware variant (amount already multiplied): part = BodyPart or -1, part_mult for dev numbers.
float apply_damage_part(World *w, EntityId target, float amount, DamageType type, EntityId attacker, vec3 point, vec3 dir, int part, float part_mult);
// With DamageFlags (gameplay/revive/revive.h): DMGF_KILL skips the downed state.
float apply_damage_ex(World *w, EntityId target, float amount, DamageType type, EntityId attacker, vec3 point, vec3 dir, int part, float part_mult, u32 flags);
// Kill feed: the item doing the damage until reset to 0 (thrown grenades, fire); 0 = the attacker's held item.
void health_set_source_item(u16 item);
// The bullet applying this damage already killed someone: a kill now is a collateral (KILLF_COLLATERAL).
void health_set_collateral(bool on);
void heal(World *w, EntityId target, float hp, float armor);
// Host, every tick: the armor cap from the worn plate carrier; the carrier holds the plates above
// the base ones (its InvSlot.ammo, armor points), so taken off or looted it keeps them.
void health_sync_armor(World *w, int slot);
void health_tick_dummy(World *w, EntityId e, float dt);

#endif
