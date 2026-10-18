// Projectile point-mass simulation (pure, deterministic). One BalBullet is stepped at the fixed
// sim tick (SIM_DT), sub-stepped for accuracy at high speed. It integrates gravity, drag on the
// air-relative velocity (that is where wind drift comes from), a transonic instability term and
// spin drift. The caller sweeps a ray from the old to the new position against the world.
//
// Determinism: float32 in a fixed operation order, no FMA contraction (CMake: -ffp-contract=off,
// MSVC /fp:precise). The host and the predicting client run the same binary on the same inputs
// (eye, aim, seed, tick, weather), so they agree bit for bit until the worlds differ.
#ifndef DG_BAL_SIM_H
#define DG_BAL_SIM_H

#include "core/base.h"
#include "core/dgmath.h"
#include "gameplay/ballistics/bal_data.h"

#define BAL_GRAVITY 9.80665f
#define BAL_MIN_SPEED 60.0f       // m/s: below this a bullet is spent
#define BAL_MAX_TIME 6.0f         // s of flight
#define BAL_SUBSTEP_LEN 20.0f     // m per integration sub-step at most
#define BAL_MAX_SUBSTEPS 4

typedef enum BalBulletFlags {
    BF_SUPERSONIC_EVER = 1 << 0,   // was above Mach 1 at some point (crack)
    BF_SUBSONIC        = 1 << 1,   // launched below Mach 1 (whizz, no crack)
    BF_TRACER          = 1 << 2,
    BF_PENETRATED      = 1 << 4,
    BF_FRAGMENTED      = 1 << 5,
    BF_PREDICTED       = 1 << 6,   // client-side prediction / replay copy (visual only)
    BF_DEAD            = 1 << 7,
} BalBulletFlags;

typedef struct BalBullet {
    vec3 pos, vel;
    float bc;              // effective BC (tracer variant, × deformation / yaw after an exit)
    float mass;            // kg (tracer variant)
    float yaw;             // rad: transonic yaw (extra drag + lateral random walk)
    float dist;            // m of path flown
    float time;            // s of flight
    Rng rng;               // per-bullet stream: shot seed ^ pellet
    u8 cart;               // BalData.cart index
    u8 flags;              // BalBulletFlags
    u8 events;             // penetrations so far
    u8 pellet;
} BalBullet;

// Air and wind for one step. Build it once per tick (bal_env_make) and set .wind per bullet.
typedef struct BalEnv {
    float rho;             // kg/m³
    float sound_speed;     // m/s
    int drag_model;        // BalDragModel
    vec3 wind;             // m/s at the bullet
    bool spin_drift;
} BalEnv;

float bal_sound_speed(float temp_c);                       // 331.3 + 0.606·T
float bal_air_density(float temp_c, float altitude_m);     // ideal gas, standard pressure lapse
BalEnv bal_env_make(float temp_c, float altitude_m, int drag_model);

// Launch: pos / unit dir / speed; seed is the shot seed, pellet its index. tracer selects the
// cartridge's tracer variant mass / BC.
void bal_launch(BalBullet *b, const BalCartridge *c, int cart_index, vec3 pos, vec3 dir, float speed,
                u32 seed, int pellet, bool tracer);
// Deceleration vector (drag only) for a bullet velocity in the given wind.
vec3 bal_drag_accel(const BalBullet *b, const BalCartridge *c, const BalEnv *e, vec3 vel);
// Advances by dt. Returns false when the bullet is spent (speed, time, range); it is then BF_DEAD.
bool bal_step(BalBullet *b, const BalCartridge *c, const BalEnv *e, float dt);
float bal_bullet_speed(const BalBullet *b);
float bal_bullet_energy(const BalBullet *b);               // J
float bal_bullet_mach(const BalBullet *b, const BalEnv *e);

// Launch elevation (rad) so the bullet crosses the line of sight again at zero_m with the sight
// sight_height above the bore (standard atmosphere, no wind). Solved by bisection on the sim.
float bal_zero_angle(const BalCartridge *c, float speed, float zero_m, float sight_height, int drag_model);

// Deterministic unit direction inside a cone of half-angle cone_rad around dir.
vec3 bal_cone_dir(vec3 dir, float cone_rad, Rng *rng);
float bal_rng_gauss(Rng *rng);                             // approx N(0,1) (sum of 4 uniforms)

#endif
