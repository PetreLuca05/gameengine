// Ballistics visuals (docs/BALLISTICS.md): the muzzle flash composed from physics (bal_muzzle.c)
// into component effects, tracers (head, curved tail, smoke; they glow, no light),
// bullet traces through magnified optics and vapour trails, the tracer fire hook, debug drawing.
#ifndef DG_BAL_FX_H
#define DG_BAL_FX_H

#include "world/world.h"

void bal_fx_init(void);                // installs the muzzle composer into the VFX layer
// A gun's flash: cartridge / device indices (BalData), barrel m; slot = the shooter (heat of
// recent fire, suppressor pop), viewmodel = the local first-person barrel.
void bal_fx_muzzle(int slot, int cart, int device, float barrel, vec3 pos, vec3 dir, bool viewmodel);
void bal_fx_muzzle_weapon(int slot, const WeaponDef *d, vec3 pos, vec3 dir, bool viewmodel);
// Per frame: tracer visuals for the live bullets.
void bal_fx_frame(World *w, vec3 cam, float dt);
void bal_fx_clear(void);

// Tracers that end in dry grass start a fire (stub until foliage burns: log + debug marker).
void fire_try_ignite(World *w, vec3 pos, float energy);
float bal_fx_heat(int slot);   // a shooter's barrel heat now (g of powder; the thermal scope)

#endif
