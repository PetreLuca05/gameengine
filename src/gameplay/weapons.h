// Weapons: hitscan / projectile guns, melee sweeps, consumables, reload, fire modes, recoil,
// spread and sway. Runs inside the predicted player simulation; hit detection only on the
// authority. Recoil math lives in gameplay/recoil.c (pure, unit tested).
#ifndef DG_WEAPONS_H
#define DG_WEAPONS_H

#include "world/world.h"
#include "gameplay/aim.h"

typedef struct WeaponOutput {
    vec2 view_kick;      // aim recoil + recovery (yaw, pitch) radians to add to the local view
    bool fired;
    bool swung;
    bool mode_changed;   // fire mode switched this tick
    bool barrel_blocked; // block_fire_when_barrel_blocked held the trigger this tick
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
    struct Cvar *block_barrel, *muzzle_tolerance, *barrel_tolerance, *aim_debug, *hold_aim_max;
} WeaponCvars;
void weapons_register_cvars(void);
const WeaponCvars *weapons_cvars(void);

// One hit of a shot (hitscan trace or a ballistic segment, gameplay/ballistics/bal_world.c).
typedef struct HitResult {
    bool hit;
    float dist;        // along the trace; ballistics: the bullet's path length (damage falloff)
    vec3 point, normal;
    EntityId ent;
    BodyId body;
    bool hitbox;
    bool headshot;
    int part;          // BodyPart for character hitboxes, -1 otherwise
    bool character;
    int material;      // SurfaceMaterial (flesh for hitboxes)
    int tire;          // vehicle wheel hit (-1 none)
} HitResult;
// Host: damage / impulse of a hit (damage_scale: e.g. the energy left after a penetration).
void weapon_apply_hit(World *w, int shooter, const WeaponDef *d, const HitResult *h, vec3 dir, float damage_scale);
// Host: the EV_IMPACT event of a hit (every machine; the shooter skips it when it predicted it).
// extra: bits 16..31 of the event id (ballistic energy / outcome, gameplay/ballistics/bal_world.h).
void weapon_push_impact(World *w, int shooter, const WeaponDef *d, const HitResult *h, vec3 dir, bool killed, u32 extra);
// The shooter's own predicted impact effects (flesh entry burst or surface impact).
void weapon_local_impact(World *w, const WeaponDef *d, const HitResult *h, vec3 dir, u32 extra);
// The shooter's vehicle body to ignore (seated) or its character body.
BodyId weapon_shooter_ignore_body(World *w, int shooter);
// Lag-compensated legacy C_HITBOX entities (not characters) along a ray: true and *h filled when
// one is closer than h->dist (or than range when !h->hit).
bool weapon_trace_legacy_hitboxes(World *w, int shooter, vec3 origin, vec3 dir, float range, u32 view_tick256, HitResult *h);

// Aim model (docs/AIM.md, gameplay/aim.h): the gun's gameplay hold points its barrel at the camera's
// aim point P (the hold aim, a pose); a shot leaves the muzzle socket along that barrel with no
// correction; Q is where that line ends, and the crosshair moves there only when the line is
// obstructed (Q clearly closer than what the camera sees, or the line misses P).
typedef struct AimQuery {
    vec3 eye;                 // the simulated eye (player_eye_pos: inside the capsule, the frame of the claims)
    float yaw, pitch;         // the view the muzzle claim is relative to (command angles)
    vec3 cam_dir;             // aim direction (view + recoil + sight offset), before the spread cone
    bool have_claim;
    vec3 claim;               // the shooter's camera-space muzzle (UserCmd.muzzle_cam)
    bool have_eye_claim;
    vec3 eye_claim;           // the shooter's first-person camera, same frame (UserCmd.eye_cam)
    bool have_barrel;
    vec3 barrel_claim;        // the shooter's camera-space barrel forward (UserCmd.barrel_cam, unit)
    float kick_yaw, kick_pitch; // view change since the pose (recoil of earlier shots this tick, rad): turns the barrel too
    bool hand;                // thrown items: the hand socket, converging on P (a palm has no barrel)
    bool from_pose;           // the muzzle / barrel of this machine's pose re-aimed at this P (crosshair,
                              // aim_report): no claims (they are aimed at the command's own P)
    u16 aim_dist;             // the shooter's P distance along its own camera ray (UserCmd.aim_dist, cm, 0 = none)
    float max_range;          // camera ray and barrel line length (no hit: P = eye + cam_dir * max_range)
    AimObstructRule rule;     // obstruct_near / obstruct_margin / obstruct_margin_rel of the weapon
    u32 view_tick256;         // host lag compensation of the traces (0 = the present world)
} AimQuery;
typedef struct AimSolve {
    vec3 eye, cam_dir;        // the camera ray: from the first-person camera (the validated eye claim)
    vec3 p;                   // aim point
    bool p_hit;
    vec3 muzzle;              // the (validated) muzzle socket
    vec3 origin;              // where the bullet starts: the muzzle, or the last free point when it is inside geometry
    bool recovered;           // the muzzle was inside geometry (origin pulled back towards the eye)
    vec3 dir;                 // bullet direction: the barrel's forward axis, before the spread cone
    int cls;                  // AimClass (clear / obstructed / barrel / friendly)
    vec3 q;                   // where the barrel line ends (first hit, or origin + dir * max_range)
    float q_dist;             // |Q - origin|
    bool q_hit;
    float far;                // obstruct_far: the camera ray's hit distance, or max_range
    float parallax;           // the rule's parallax allowance at the camera's hit (m)
    EntityId q_ent;
    float claim_error;        // |claim - the host's reconstruction| (m; 0 = not checked)
    bool claim_clamped;       // out of tolerance: moved onto the host's tolerance sphere
    float eye_error;          // the same for the camera eye claim
    bool eye_clamped;
    float barrel_error;       // degrees between the barrel claim and the host's pose (0 = not checked)
    bool barrel_clamped;
    bool alt;                 // out of tolerance of the host's P, in tolerance of the shooter's own (aim_dist)
    float miss;               // P's distance from the barrel line (0 = the line ends at P)
    float tol;                // aim_reach_tol(far)
    bool have_pose;           // this machine's gameplay pose re-aimed at P (below)
    vec3 pose_muzzle, pose_dir;   // ... its muzzle and barrel (world, before the in-tick recoil)
    float hold_deg;           // ... the hold aim's turn (deg)
    bool hold_clamped;        // ... at its limit
} AimSolve;
AimQuery weapon_aim_query(World *w, int slot, const PlayerMove *m, const UserCmd *cmd, vec3 cam_dir, const WeaponDef *d, u32 view_tick256);
void weapon_aim_solve(World *w, int slot, const AimQuery *q, AimSolve *out);
// The viewed player's aim this frame (crosshair, aim_debug): view angles + sight offset of the frame.
bool weapon_aim_preview(World *w, int slot, vec3 eye, float yaw, float pitch, const UserCmd *cmd, AimSolve *out);
// The camera ray's aim point distance (m): the shot trace (world, tires, hitboxes; the present world)
// from eye along dir, max_range without a hit. The hold aim's P (docs/AIM.md "Where P comes from").
float weapons_aim_point(World *w, int slot, vec3 eye, vec3 dir, float max_range);
// The command's hold-aimed claims (build_cmd): the player's gameplay pose re-aimed at P traced along
// the command's view; fills muzzle_cam / barrel_cam / aim_dist. False: no pose / no gun.
bool weapons_fill_aim_claim(World *w, int slot, UserCmd *cmd);
// aim_debug: draws the viewed player's aim of this frame (per frame, from the game's debug pass).
void weapons_aim_debug_frame(World *w);
// Host aim checks (aim_stats): shots, claims clamped, worst claim error (m), aim point mismatches.
typedef struct AimStats { u32 shots, checked, clamped, p_checked, p_mismatch, recovered, blocked, eye_checked, eye_clamped, dir_checked, dir_clamped, alt; float max_error, max_p_error, max_eye_error, max_dir_error; } AimStats;
const AimStats *weapons_aim_stats(void);
void weapons_aim_stats_reset(void);

void weapons_tick(World *w, int slot, PlayerSim *sim, const UserCmd *cmd, u16 pressed, float dt, bool first_predict, WeaponOutput *out);
void projectiles_tick(World *w, float dt);
// The shot trace (world, tires, hitboxes) without damage: pings, aim queries. False = nothing.
bool weapon_trace(World *w, int shooter, vec3 origin, vec3 dir, float range, vec3 *point, EntityId *ent);
// Equipped slot contents (or NULL when empty).
// WeaponState.slot of the built-in phone (taken out with its own key, docs/PHONE.md): not an
// inventory slot; weapon_equipped returns a virtual slot holding ItemDB.builtin_phone.
#define WPN_SLOT_PHONE INV_SEL_COUNT
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

const struct WeaponVehicleHooks *weapons_vehicle_hooks(void);

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
