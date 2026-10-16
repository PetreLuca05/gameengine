// Flesh impacts on characters (cosmetic, every machine runs it locally from the replicated hit /
// death events; no extra network data): entry burst by surface (flesh / cloth / armour), exit
// spray through the body (thickness from the hit capsule, rules in assets/vfx/flesh_rules.json),
// splatter decals behind the target, drips, skinned wound / stain decals, lingering mist,
// headshots, helmets, kills (bigger burst, pool under the corpse, wounds that keep bleeding).
// gore_level 0 replaces all of it with dust puffs and no decals.
#ifndef DG_FLESH_FX_H
#define DG_FLESH_FX_H

#include "core/base.h"
#include "core/dgmath.h"
#include "vfx/vfx_flesh.h"

struct World;

enum FleshHitFlags {
    FLESH_HEAD = 1u << 0,
    FLESH_KILL = 1u << 1,         // this hit killed the target
    FLESH_MELEE = 1u << 2,
    FLESH_ENTRY_ONLY = 1u << 3,   // shooter's prediction: entry burst only
    FLESH_NO_ENTRY = 1u << 4,     // the confirmation of a predicted hit: everything but the entry
};

typedef struct FleshHit {
    u32 target;                   // EntityId
    int part;                     // BodyPart
    vec3 pos, normal, dir;        // entry point, surface normal, bullet / swing direction (unit)
    int caliber;                  // VfxCaliber
    float damage, dist;           // dealt damage, shooter distance (m)
    u32 flags;
    u32 seed;                     // same on every machine for the same hit (exit decision)
} FleshHit;

typedef struct FleshStats {
    int hits, exits, splats, drips, wounds, pools, bleeders;
    float ms_last_entry, ms_last_full, ms_last_kill, ms_max_entry, ms_max_kill;
    double ms_sum_entry, ms_sum_full;
    int n_entry, n_full;
    int last_rays, last_ray_hits;
} FleshStats;

void flesh_fx_init(void);
void flesh_fx_reload(void);                  // assets/vfx/flesh_rules.json
void flesh_fx_hit(struct World *w, const FleshHit *h);
void flesh_fx_update(struct World *w, float dt);
void flesh_fx_clear(void);                   // level change
const FleshRules *flesh_fx_rules(void);
const FleshStats *flesh_fx_stats(void);
void flesh_fx_reset_stats(void);
// The last exit / splatter (tests, flesh range debug).
typedef struct FleshDebug {
    bool valid, exited;
    vec3 entry, exit, dir;
    float thickness;
    int rays;
    FleshSplat splats[16];
} FleshDebug;
const FleshDebug *flesh_fx_last(void);

// EV_IMPACT on a character: ent = target, value = damage, data: incoming vec3 (12 bytes),
// part (u8), flags (u8: 1 kill, 2 melee), dist (f32).
void flesh_pack_event(u8 *data, vec3 incoming, int part, bool kill, bool melee, float dist);
void flesh_unpack_event(const u8 *data, vec3 *incoming, int *part, bool *kill, bool *melee, float *dist);

#endif
