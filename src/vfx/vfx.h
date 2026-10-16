// VFX game layer: owns the effect library and the simulation, connects them to the world
// (Jolt raycasts for CPU particles, sounds), the renderer (GPU spawns, CPU sprites, mesh
// particles, decals, lights) and gameplay hooks (muzzle flashes, impacts, casings). Effects are
// never replicated: they are spawned from local prediction and from replicated game events.
#ifndef DG_VFX_H
#define DG_VFX_H

#include "core/base.h"
#include "core/dgmath.h"
#include "vfx_sim.h"

struct BasicMeshes;

void vfx_game_init(const struct BasicMeshes *meshes);
void vfx_game_shutdown(void);
bool vfx_game_active(void);            // vfx_enable and a loaded library
void vfx_game_reload(void);            // re-reads assets/vfx (effects, textures)
void vfx_game_update(float dt);        // frame dt: runs fixed 60 Hz simulation steps
void vfx_game_render(vec3 cam_pos);    // feeds the renderer for this frame
void vfx_game_set_environment(float sun_intensity, vec3 wind);
// Gameplay hooks.
u32 vfx_game_spawn(const char *effect, const VfxSpawnParams *p);
u32 vfx_game_spawn_id(int effect, const VfxSpawnParams *p);
void vfx_game_impact(vec3 pos, vec3 normal, vec3 incoming, int material, int caliber, u32 flags);
u32 vfx_game_muzzle(vec3 pos, vec3 dir, int type, int device, bool viewmodel);
void vfx_game_shell(vec3 pos, vec3 vel, int caliber);
// Local first-person muzzle socket: flashes follow the barrel while they live.
void vfx_game_set_local_muzzle(vec3 pos, vec3 dir, bool valid);
bool vfx_game_night(void);
int vfx_game_gore(void);             // gore_level: 0 off, 1 low, 2 high             // env_night (dev day / night toggle)
VfxSystem *vfx_game_system(void);
VfxLibrary *vfx_game_library(void);
const char *vfx_game_stats_text(void);
// World transform of a character bone (skinned decals, bleeding emitters): false when the
// entity or bone no longer exists. Installed by the game layer.
typedef bool (*VfxBoneFn)(u32 entity, int bone, vec3 *pos, quat *rot);
void vfx_game_set_bone_fn(VfxBoneFn fn);

#endif
