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
    float sub_tick;           // 0..1: when inside the tick the shot fired (exact RPM timing)
    float interval;           // s to the next shot at the current rate (auto / burst), 0 = single
} ShotEvent;
typedef void (*ShotHook)(const ShotEvent *ev, void *user);
void weapons_add_shot_hook(ShotHook fn, void *user);
// The local player's muzzle flash: deferred to the game, which spawns it on the first-person gun's
// final (kicked) transform of the frame (NULL: spawned at once at the predicted muzzle).
typedef void (*LocalFlashFn)(vec3 dir, int type, int device);
void weapons_set_local_flash(LocalFlashFn fn);

typedef struct WeaponCvars {
    struct Cvar *recoil_scale, *visual_scale, *spread_scale, *recoil_debug, *infinite_ammo, *sway_scale, *kick_scale, *bob_scale, *ads_sway, *ads_bob, *sway_ads_scale, *sway_cosmetic;
} WeaponCvars;
void weapons_register_cvars(void);
const WeaponCvars *weapons_cvars(void);

void weapons_tick(World *w, int slot, PlayerSim *sim, const UserCmd *cmd, u16 pressed, float dt, bool first_predict, WeaponOutput *out);
void projectiles_tick(World *w, float dt);
// The shot trace (world, tires, hitboxes) without damage: pings, aim queries. False = nothing.
bool weapon_trace(World *w, int shooter, vec3 origin, vec3 dir, float range, vec3 *point, EntityId *ent);
// Equipped slot contents (or NULL when empty).
// WeaponState.slot of the built-in phone (taken out with its own key, docs/PHONE.md): not an
// inventory slot; weapon_equipped returns a virtual slot holding ItemDB.builtin_phone.
#define WPN_SLOT_PHONE INV_HOTBAR
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

// Vehicles (src/game/game_vehicles.c): seated shooting, tire hits, vehicle consumables.
typedef struct WeaponVehicleHooks {
    bool (*can_shoot)(World *w, int slot, const PlayerSim *sim, const WeaponDef *d);   // seated: allowed?
    float (*spread_mult)(World *w, int slot);                                         // 1 = none
    BodyId (*ignore_body)(World *w, int slot);                                        // own vehicle
    // Ray vs wheel cylinders (lag compensated on the host): wheel index or -1, t along dir.
    int (*ray_tires)(World *w, vec3 origin, vec3 dir, float max_t, u32 view_tick, EntityId *veh, float *t);
    void (*tire_hit)(World *w, EntityId veh, int wheel, vec3 point, vec3 dir);
    bool (*use_item)(World *w, int slot, const ItemDef *it);                          // host: consumed?
} WeaponVehicleHooks;
void weapons_set_vehicle_hooks(const WeaponVehicleHooks *h);

#endif
