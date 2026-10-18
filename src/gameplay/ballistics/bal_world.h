// Ballistics in the game world (docs/BALLISTICS.md): every gun shot is a projectile from the pool
// below, stepped at the fixed tick and swept against the world, tires, legacy hitboxes and the
// lag-compensated character hitboxes. The host's bullets are authoritative (damage, EV_IMPACT);
// the shooter predicts its own (visuals only) and every other machine replays the shot from the
// EV_SHOT record (tracers, cracks, reports), so nothing per step goes over the network.
#ifndef DG_BAL_WORLD_H
#define DG_BAL_WORLD_H

#include "world/world.h"
#include "gameplay/ballistics/bal_data.h"
#include "gameplay/ballistics/bal_sim.h"
#include "gameplay/ballistics/bal_surface.h"
#include "gameplay/ballistics/bal_muzzle.h"

#define BAL_POOL 1024
#define BAL_TRAIL 8

typedef enum BalRole {
    BAL_ROLE_HOST,        // authoritative (damage, events)
    BAL_ROLE_PREDICT,     // the shooter's own prediction (impact effects locally)
    BAL_ROLE_REPLAY,      // another player's shot replayed for the visuals / sound
} BalRole;

// EV_SHOT payload (GameEvent.data): everything a machine needs to replay the shot bit for bit.
typedef struct BalShotRecord {
    u32 seed;              // shot seed (spread cone + bullet streams)
    u32 fire_tick;         // host tick of the shot
    u16 shot_id;           // (slot << 12) | (shot counter & 0xFFF)
    u16 weapon;            // ItemDB weapon index (base definition)
    u8 att[5];             // attachments (effective definition on the receiver)
    u8 cart;               // BalData cartridge index
    u8 pellets;
    u8 flags;              // BAL_SHOT_*
    u8 device;             // BalData device index
    u8 muzzle_type, vfx_device, caliber;   // observer flash / casing (vfx_library.h)
    float spread;          // full cone, degrees
    float speed;           // launch speed m/s
    float zero_angle;      // rad
    float poi[2];          // rad (right, up)
} BalShotRecord;

enum { BAL_SHOT_TRACER = 1, BAL_SHOT_FORCED_TRACER = 2 };

// EV_IMPACT id bits 16..31 (and fx flags passed to the impact effects).
#define BAL_IMPACT_EXTRA(energy_j, outcome_bits) ((((u32)bal_energy_code(energy_j)) << 16) | ((u32)(outcome_bits) << 24))
// Energy: J arriving at the surface (flesh: J deposited in the body).
// Bit 0 is free (it was the ricochet; bullets no longer ricochet).
enum { BAL_IMP_PENETRATE = 2, BAL_IMP_EXIT = 4, BAL_IMP_FRAGMENT = 8, BAL_IMP_BALLISTIC = 16, BAL_IMP_TRACER = 32, BAL_IMP_YAWED = 64 };
// Flesh wound scale from an impact id (the same rule as bal_hit_flesh's cavity).
float bal_impact_cavity(u32 id);
u8 bal_energy_code(float joules);       // log scale 0..255 (0 = none, 255 ≈ 40 kJ)
float bal_energy_decode(u8 code);
DG_INLINE float bal_impact_energy(u32 id) { return bal_energy_decode((u8)(id >> 16)); }
DG_INLINE u32 bal_impact_outcome(u32 id) { return (id >> 24) & 0xFF; }

typedef struct BalLive {
    BalBullet b;
    u8 role;               // BalRole
    u8 slot;               // shooter
    u16 shot_id;
    u16 weapon;
    u8 att[5];
    u32 fire_tick;
    u32 view_tick256;      // host: the shooter's view when firing (0 = no rewind)
    float flesh_depth;     // tissue crossed so far
    vec3 launch;           // where it was fired from (report distance, debug)
    vec3 trail[BAL_TRAIL]; // last tick positions (newest first): curved tracer tails
    u8 trail_count;
    u8 crack_done;         // this machine's listener already heard the crack / whizz
    vec3 prev_pos;         // position at the start of the last step (render interpolation)
} BalLive;

void bal_world_register(void);          // cvars / commands (once)
bool bal_world_load(void);              // assets (also on bal_reload / when the files change)
const BalData *bal_data(void);
bool bal_world_enabled(void);           // bal_enable: guns with a cartridge fire projectiles
void bal_world_clear(void);             // map change
// Cartridge index of a weapon (its "cartridge"), -1 = not ballistic.
int bal_weapon_cartridge(const WeaponDef *d);
int bal_weapon_device(const WeaponDef *d);

// A gun shot (weapons.c fire()): host spawns the authoritative bullets and the EV_SHOT event; a
// predicting client spawns its own visuals. The bullets start at origin (the muzzle, docs/AIM.md)
// along aim (muzzle -> the camera ray's aim point, before the spread cone).
typedef struct BalFireIn {
    int slot;
    const WeaponDef *def;    // effective (attachments applied)
    int weapon_index;        // base definition index
    const u8 *att;           // InvSlot.att
    vec3 eye, origin, aim;   // eye: the shooter's report
    float spread;            // full cone deg
    u32 seed;
    u32 view_tick256;
    int shot_in_mag, mag_size;
    u32 mag_seed;
    u16 shot_counter;
    bool local_view, first;
} BalFireIn;
void bal_fire(World *w, const BalFireIn *in);
// EV_SHOT (other players' shots): the report (delayed / filtered by distance) and, on clients, the
// replay of the bullets from the shot's origin (ev->pos). muzzle: the shooter's visible barrel (report).
void bal_on_shot_event(World *w, const GameEvent *ev, vec3 muzzle);
// The shooter's own report (immediate; a suppressor's level and tone).
void bal_shooter_report(const WeaponDef *d, vec3 eye);

// Fixed tick (host_tick / client tick): steps every bullet.
void bal_world_tick(World *w, float dt);
// Per frame: debug drawing, hot reload, crack / report audio.
void bal_world_frame(World *w, vec3 listener, float dt);

// Live bullets (tracers, debug): the pool and its high-water mark.
const BalLive *bal_world_bullets(int *count);
// The air for this tick (weather temperature, the replicated wind).
BalEnv bal_world_env(const World *w, vec3 pos);

// ballistics_bench: host bullets stepped per tick and the time it took (last tick).
typedef struct BalStats { int live, steps, rays; double tick_ms; double worst_ms; } BalStats;
const BalStats *bal_world_stats(void);

#endif
