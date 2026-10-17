// Throwables (docs/UI_WHEELS_HUD.md "Throwables"): frag grenades, flares and molotovs. Equipped
// from the hotbar or the throwables wheel; hold attack to pull the pin / charge (the grenade fuse
// runs while you cook it, the timed ring shows it), release to throw (strength from the charge).
// The host spawns a bouncing ET_PROJECTILE carrying the item; grenades go off when the fuse ends
// (radius damage with falloff and line of sight, zombies hear it), flares burn red for light_time,
// molotovs burst on impact into a short fire that hurts what stands in it.
#ifndef DG_THROWABLES_H
#define DG_THROWABLES_H

#include "world/world.h"

// Inside weapons_tick for an equipped throwable (ws->use_t = the hold time, < 0 idle).
void throwables_held_tick(World *w, int slot, PlayerSim *sim, const ItemDef *def, InvSlot *s, bool attack, bool busy, float dt, bool first);
// projectiles_tick: a thrown item (pr->item != 0).
void throwable_projectile_tick(World *w, int i, float dt);
// Client: EV_EXPLOSION presentation (effects, sound, shake).
void throwables_event(const GameEvent *ev);
// Render: flare / fire lights for thrown items.
void throwables_render(World *w, int i);
// Timed-ring values for the local HUD: true while holding one. mode 0 = drain (the grenade fuse),
// 1 = charge (throw strength), as TimedRingMode (ui/widgets/timed_ring.h).
bool throwables_ring(const World *w, int slot, const PlayerSim *sim, float *duration, float *remaining, int *mode, const char **label);

#endif
