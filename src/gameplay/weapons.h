// Weapons: hitscan / projectile guns, melee sweeps, consumables, reload, spread and recoil.
// Runs inside the predicted player simulation; hit detection only on the authority.
#ifndef DG_WEAPONS_H
#define DG_WEAPONS_H

#include "world/world.h"

typedef struct WeaponOutput {
    vec2 view_kick;      // recoil (yaw, pitch) radians to add to the local view
    bool fired;
    bool swung;
} WeaponOutput;

void weapons_tick(World *w, int slot, PlayerSim *sim, const UserCmd *cmd, u16 pressed, float dt, bool first_predict, WeaponOutput *out);
void projectiles_tick(World *w, float dt);
// Equipped slot contents (or NULL when empty).
const InvSlot *weapon_equipped(const World *w, int slot);
const WeaponDef *weapon_equipped_def(const World *w, int slot);
float weapon_current_spread(const World *w, int slot);

#endif
