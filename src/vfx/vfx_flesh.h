// Flesh impact rules (pure, no world): which hits exit the body, how thick the body is along the
// bullet (from the hit capsule), how big each part of the effect is (caliber / distance / damage)
// and where the blood lands behind the target (splatter rays from the exit cone). The game layer
// (src/gameplay/flesh_fx.c) feeds it hitboxes and a raycast, then spawns the effects and decals.
// Data: assets/vfx/flesh_rules.json (docs/VFX_REFERENCE.md, "Flesh impacts").
#ifndef DG_VFX_FLESH_H
#define DG_VFX_FLESH_H

#include "core/base.h"
#include "core/dgmath.h"
#include "anim/body_parts.h"
#include "vfx_library.h"

typedef struct FleshCaliberRule {
    float exit_chance;        // 0..1 for a hit that could exit (thickness / distance permitting)
    float max_thickness;      // m of body a bullet of this caliber still goes through
    float max_dist;           // m: beyond this no exit (shotgun pellets at range); 0 = any
    float scale;              // effect size scale
    float mist, droplets;     // count multipliers
    int splatter_rays;        // rays from the exit cone (gore high)
    float splatter_range;     // m
    float cone_deg;           // exit cone half angle
} FleshCaliberRule;

typedef struct FleshRules {
    FleshCaliberRule cal[VFX_CAL_COUNT];
    float part_thickness[BP_COUNT];   // multiplier on the capsule chord (bone, armour of muscle)
    float part_exit[BP_COUNT];        // multiplier on the exit chance
    float dist_near, dist_far, dist_far_scale;      // effects shrink with distance to the shooter
    float dmg_ref, dmg_min_scale, dmg_max_scale;    // and grow with the damage dealt
    float splat_size[2];              // m (at scale 1, near the exit)
    float splat_stretch_max;          // grazing splats stretch up to this aspect
    float drip_chance, drip_len[2], drip_time;      // vertical surfaces
    float pool_size[2], pool_grow_time, pool_delay; // under a corpse
    float stain_size[2], stain_grow, stain_grow_time;   // soaking into clothes / skin around a wound
    float wound_entry, wound_exit;    // wound decal sizes (m)
    float bleed_rate, bleed_decay, bleed_time;      // corpse wounds: drops / s, 1/s, s
    float kill_scale, head_scale, limb_droplets;
    int skinned_max;                  // per character (<= VFX_SKINNED_DECALS_PER_CHAR)
} FleshRules;

void flesh_rules_defaults(FleshRules *r);
bool flesh_rules_parse(FleshRules *r, const char *json, char *err, int err_size);

// Effect scale for a hit: caliber x distance falloff x damage.
float flesh_hit_scale(const FleshRules *r, int caliber, float damage, float dist);
// Chord through the capsule (a-b, radius) from the entry point along dir: the far side.
bool flesh_capsule_exit(vec3 a, vec3 b, float radius, vec3 entry, vec3 dir, vec3 *exit, float *thickness);
// Exit rule: caliber chance x part bias, thickness and distance limits. seed: per-hit random.
bool flesh_should_exit(const FleshRules *r, int caliber, int part, float thickness, float dist, u32 seed);

// Splatter: rays inside the exit cone; each hit becomes a splat stretched by the incidence angle,
// dripping on vertical surfaces.
typedef bool (*FleshRayFn)(void *ctx, vec3 from, vec3 delta, vec3 *hit, vec3 *normal);
typedef struct FleshSplat {
    vec3 from, to;                // the ray (debug)
    bool hit;
    vec3 pos, normal, tangent;    // tangent: stretch direction (along the ray, on the surface)
    float size, aspect, alpha, dist;
    bool drip;
    float drip_len;
} FleshSplat;
int flesh_plan_splatter(const FleshRules *r, vec3 origin, vec3 dir, float scale, int rays, float range, float cone_deg,
                        FleshRayFn fn, void *ctx, u32 seed, FleshSplat *out, int max);

u32 flesh_hash(u32 x);
float flesh_rand01(u32 *s);

#endif
