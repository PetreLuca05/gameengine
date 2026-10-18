// Zombie hordes (host-authoritative; clients only animate the replicated state). Zombies stay
// world entities with a CharacterInst (rendering, hitboxes, replication), while the horde owns
// their movement and decisions in SoA arrays: follow the nearest player, steer around each other
// and walls, attack when close. Work scales by tier (distance to the nearest player): near zombies
// every tick with a Jolt capsule and per-bone hitboxes, mid / far at lower rates, dormant ones
// barely at all. See docs/ZOMBIES.md.
#ifndef DG_HORDE_H
#define DG_HORDE_H

#include "core/base.h"
#include "core/dgmath.h"
#include "horde_steer.h"

struct World;

typedef struct HordeStats {
    int count, pending, failed_spawns;
    int tier_count[HT_COUNT];
    int bodies, steered, casts, ground_casts, attacks, hits;
    int players;
    float ms_sync;
    float ms_total, ms_grid, ms_steer, ms_casts, ms_move, ms_phys, ms_anim;
    float ms_avg, ms_max;            // horde + character evaluation, rolling
    int anim_evals;
} HordeStats;

void horde_init(void);                          // cvars, commands, assets/ai/zombie.json
void horde_host_tick(struct World *w, float dt); // before chars_host_tick
void horde_post_tick(struct World *w);          // after chars_host_tick (stats)
void horde_level_reset(void);
void horde_debug_draw(struct World *w);         // horde_debug 1
const HordeStats *horde_stats(void);
const HordeConfig *horde_config(void);
int horde_count(void);
// Spawn: count zombies around center (radius, ring / random / cluster), character id string or
// "mixed". Queued, spawned over the next ticks. Returns how many were queued.
enum { HSPAWN_RING, HSPAWN_RANDOM, HSPAWN_CLUSTER };   // horde_spawn modes
int horde_spawn(struct World *w, vec3 center, int count, float radius, int mode, const char *character);
void horde_despawn(struct World *w);
// One zombie at exactly these feet (indoor spots: no search from above); false when blocked.
bool horde_spawn_at(struct World *w, vec3 feet, float yaw, const char *character);
// An existing (idle, wandering) zombie joins the horde: it senses, chases and attacks from now on
// (story zombies waking up as the player comes near). False when not a zombie or the horde is full.
bool horde_adopt(struct World *w, u32 e);   // e: an EntityId (world/world.h)
// Open-world maps: water surface lookup (deep water stops zombies; NULL = none) and the sense radius
// (0 = always chase the nearest player, the horde default).
void horde_set_water(float (*fn)(void *user, float x, float z), void *user);
void horde_set_sense(float radius);
int horde_aggro_count(void);
// A loud noise (slammed doors, explosions): zombies within radius chase (sense radius on) for a
// second's worth of ticks after it.
void horde_noise(vec3 pos, float radius);                     // zombies chasing (sense radius on)

#endif
