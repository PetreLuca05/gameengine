// Weapons: hitscan / projectile guns, melee sweeps, consumables, reload, fire modes, recoil,
// spread and sway. Runs inside the predicted player simulation; hit detection only on the
// authority. Recoil math lives in gameplay/recoil.c (pure, unit tested).
#ifndef DG_WEAPONS_H
#define DG_WEAPONS_H

#include "world/world.h"

typedef struct WeaponOutput {
    vec2 view_kick;      // aim recoil + recovery (yaw, pitch) radians to add to the local view
    bool fired;
    bool swung;
    bool mode_changed;   // fire mode switched this tick
    int shots;           // shots fired this tick (sub-tick RPM can fire several)
} WeaponOutput;

// Per-shot hook (cosmetic side effects: visual kick, shell eject, bolt cycling, ...). Called on
// the first prediction of every shot, for every player simulated on this machine.
typedef struct ShotEvent {
    World *world;
    int slot;
    bool local_view;          // the shooter is this machine's player
    const WeaponDef *def;
    vec3 eye, muzzle, dir;    // dir = aim direction before spread
    float cone_deg;           // full spread cone used for this shot
    u16 pattern_index;        // shot index inside the recoil pattern (0 = first)
    u32 shot_number;          // lifetime shot counter
    int mag_left;
    FireMode mode;
    float ads_t;
    u32 seed;                 // deterministic per-shot seed
} ShotEvent;
typedef void (*ShotHook)(const ShotEvent *ev, void *user);
void weapons_add_shot_hook(ShotHook fn, void *user);

typedef struct WeaponCvars {
    struct Cvar *recoil_scale, *visual_scale, *spread_scale, *recoil_debug, *infinite_ammo, *sway_scale, *kick_scale, *bob_scale, *ads_sway, *ads_bob, *sway_ads_scale, *sway_cosmetic;
} WeaponCvars;
void weapons_register_cvars(void);
const WeaponCvars *weapons_cvars(void);

void weapons_tick(World *w, int slot, PlayerSim *sim, const UserCmd *cmd, u16 pressed, float dt, bool first_predict, WeaponOutput *out);
void projectiles_tick(World *w, float dt);
// Equipped slot contents (or NULL when empty).
const InvSlot *weapon_equipped(const World *w, int slot);
const WeaponDef *weapon_equipped_def(const World *w, int slot);
// Current bullet cone (full angle, degrees) of the player's equipped gun; 0 for melee/none.
float weapon_current_spread(const World *w, int slot);
// Deterministic aim sway offset (degrees, x = yaw right, y = pitch up) at time t (s).
vec2 weapon_sway_deg(const WeaponDef *d, const WeaponState *ws, float t);
// Where the sight points relative to the view (degrees, x = yaw right, y = pitch up): the aim sway
// (x wpn_ads_sway) plus the ADS walk bob (wpn_ads_bob). Deterministic from the simulated state, so
// the host's bullets go exactly through the centre of the sight the shooter sees (the camera stays
// steady; the held weapon is rotated about the eye by this offset).
struct PlayerMove;
vec2 weapon_aim_offset_deg(const WeaponDef *d, const WeaponState *ws, const struct PlayerMove *m, float t);
// Sway clock: derived from the command sequence so host and client agree.
DG_INLINE float weapon_sway_time(u32 seq) { return (float)(seq % 360000u) * (1.0f / 60.0f); }
const char *fire_mode_name(FireMode m);
// Host: damage taken widens the victim's cone (flinch).
void weapon_flinch(World *w, int slot, float damage);
// wpn_recoil_debug: every locally predicted bullet is checked against its cone.
typedef struct WeaponDebugStats { u32 shots, outside; float max_ratio; } WeaponDebugStats;
const WeaponDebugStats *weapons_debug_stats(void);
void weapons_debug_reset(void);
// First hitbox hit along a ray, ENT_NONE if world geometry is closer (crosshair target tint).
EntityId weapon_trace_target(World *w, int shooter, vec3 origin, vec3 dir, float range);

#endif
