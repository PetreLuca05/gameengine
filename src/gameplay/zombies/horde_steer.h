// Horde steering (pure, no world): configuration (assets/ai/zombie.json), simulation tiers by
// distance with hysteresis, and the per-zombie steering kernel (seek the target through the
// Pathfinder hook + separation / alignment from the spatial grid + wall avoidance from the
// round-robin casts). The kernel reads last tick's arrays and writes each zombie's own outputs,
// so a chunked / threaded run is bit-identical to a serial one.
#ifndef DG_HORDE_STEER_H
#define DG_HORDE_STEER_H

#include "core/base.h"
#include "core/dgmath.h"
#include "horde_grid.h"

typedef enum HordeTier { HT_NEAR, HT_MID, HT_FAR, HT_DORMANT, HT_COUNT } HordeTier;

typedef struct HordeConfig {
    // movement
    float speed_mult, accel, turn_rate_deg, stop_dist;
    float sep_radius, sep_weight, align_radius, align_weight, avoid_weight, seek_weight;
    float speed_jitter;                 // +- fraction per zombie
    // attack
    float attack_range, attack_facing_deg, attack_cooldown, attack_hit_time, attack_damage;
    // targets / casts
    int retarget_ticks, cast_interval, ground_interval;
    float cast_len, step_height, max_slope_deg;
    // tiers: distance to the nearest player, hysteresis band, update interval (ticks)
    float tier_dist[HT_COUNT - 1];      // near / mid / far upper bounds (beyond far: dormant)
    float tier_hyst;
    int tier_interval[HT_COUNT];        // steering + mover
    int anim_interval[HT_COUNT];        // host animation / hitbox evaluation
    float physics_dist, physics_hyst;   // Jolt capsule + per-bone hitboxes inside this
    // spawn
    int spawn_per_tick;
    float spawn_radius;
    // network
    int net_interval[HT_COUNT];         // snapshots between updates (0 = not sent)
    float net_near, net_mid;            // distance bands to the viewer (m)
    int net_budget;                     // bytes / s per client for zombie updates
    int capacity;
} HordeConfig;

void horde_config_defaults(HordeConfig *c);
bool horde_config_parse(HordeConfig *c, const char *json, char *err, int err_size);

// Tier for a distance, keeping the current tier inside the hysteresis band.
int horde_tier_for(const HordeConfig *c, int current, float dist);

// Optional navigation: next point to walk to from `from` towards `to` (false = straight line).
// Stub today; a navmesh plugs in here (docs/ZOMBIES.md).
typedef bool (*HordePathFn)(void *ctx, vec3 from, vec3 to, vec3 *next);

typedef struct HordeSteerIn {
    int count;
    const vec3 *pos, *vel;
    const float *yaw, *speed_max;
    const vec3 *target;         // where to go (feet)
    const u8 *has_target, *tier, *active, *busy;   // busy: attacking / staggered (stands)
    const vec3 *avoid;          // wall avoidance push from the last casts (XZ)
    const HordeGrid *grid;
    const HordeConfig *cfg;
    HordePathFn path;
    void *path_ctx;
    float dt;                   // per tier: dt * tier_interval
} HordeSteerIn;

typedef struct HordeSteerOut { vec3 *vel; float *yaw; } HordeSteerOut;

// Steers zombies [begin, end): writes out->vel[i], out->yaw[i] only for i in range.
void horde_steer_range(const HordeSteerIn *in, HordeSteerOut *out, int begin, int end);

#endif
