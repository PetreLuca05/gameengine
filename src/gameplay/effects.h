// Client-side cosmetic effects: tracers, impact decals/sparks, muzzle flashes, damage numbers.
#ifndef DG_EFFECTS_H
#define DG_EFFECTS_H

#include "core/base.h"
#include "core/dgmath.h"

struct BasicMeshes;

void fx_init(const struct BasicMeshes *meshes);
void fx_shutdown(void);
void fx_tracer(vec3 from, vec3 to);
void fx_impact(vec3 pos, vec3 normal, int kind);   // kind 0 = surface, 1 = flesh/hitbox (legacy)
// Impacts through the VFX system (src/vfx/vfx.c): per surface material, caliber scaled.
enum { FX_IMPACT_HEAD = 1u, FX_IMPACT_FLESH = 2u, FX_IMPACT_DECAL = 4u };
void fx_impact_ex(vec3 pos, vec3 normal, vec3 incoming, int material, int caliber, u32 flags);
// EV_IMPACT id: material (8) | caliber (4) << 8 | flags (4) << 12.
DG_INLINE u32 fx_impact_pack(int material, int caliber, u32 flags) { return (u32)(material & 0xFF) | ((u32)(caliber & 0xF) << 8) | ((flags & 0xF) << 12); }
bool fx_predict_impacts(void);   // cl_predict_impacts
void fx_muzzle(vec3 pos, vec3 dir);
void fx_muzzle_ex(vec3 pos, vec3 dir, int type, int device, bool viewmodel);
void fx_shell(vec3 pos, vec3 vel);                 // ejected casing
void fx_shell_ex(vec3 pos, vec3 vel, int caliber);
void fx_damage_number(vec3 pos, float amount, bool headshot);
void fx_damage_number_label(vec3 pos, float amount, bool headshot, const char *label);  // "head x4.0"
void fx_sound(const char *name, vec3 pos, float volume);
void fx_update(float dt);
void fx_render(vec3 cam);   // world-space parts (decals, sparks, tracers, flashes)
void fx_draw_hud(void); // screen-space parts (damage numbers)

#endif
