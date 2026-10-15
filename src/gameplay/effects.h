// Client-side cosmetic effects: tracers, impact decals/sparks, muzzle flashes, damage numbers.
#ifndef DG_EFFECTS_H
#define DG_EFFECTS_H

#include "core/base.h"
#include "core/dgmath.h"

struct BasicMeshes;

void fx_init(const struct BasicMeshes *meshes);
void fx_shutdown(void);
void fx_tracer(vec3 from, vec3 to);
void fx_impact(vec3 pos, vec3 normal, int kind);   // kind 0 = surface, 1 = flesh/hitbox
void fx_muzzle(vec3 pos, vec3 dir);
void fx_shell(vec3 pos, vec3 vel);                 // ejected casing (brass spark with gravity)
void fx_damage_number(vec3 pos, float amount, bool headshot);
void fx_sound(const char *name, vec3 pos, float volume);
void fx_update(float dt);
void fx_render(void);   // world-space parts (decals, sparks, tracers, flashes)
void fx_draw_hud(void); // screen-space parts (damage numbers)

#endif
