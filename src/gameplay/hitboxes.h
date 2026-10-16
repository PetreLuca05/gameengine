// Per-bone hitboxes on the shared skeleton + per-body-part damage.
//
// Hitboxes are the ragdoll capsules (physics/ragdoll.h), scaled by the character's
// ragdoll_scale, posed by the host's sim-only animation every tick (never by render LOD or
// IK). Each is tagged with a body part (assets/human/hitboxes.json). The host keeps a ring of
// the last HB_HISTORY ticks per character and rewinds to the shooter's view tick.
//
// Damage resolution (host only):
//   final = base_damage * part_mult[character][part] * weapon_part_mult[weapon][part] * range_falloff
// Unlisted parts default to 1.0. Overlapping hitboxes resolve to the one nearest the ray origin.
#ifndef DG_HITBOXES_H
#define DG_HITBOXES_H

#include "anim/anim.h"
#include "anim/body_parts.h"
#include "physics/ragdoll.h"

struct cJSON;

#define HB_MAX 24
#define HB_HISTORY 32         // 0.53 s at 60 Hz (lag compensation window)

typedef struct HitCapsule { vec3 a, b; float r; u8 part; u8 body; } HitCapsule;
typedef struct HitboxSet { u32 tick; int count; HitCapsule c[HB_MAX]; } HitboxSet;
typedef struct HitboxHistory { HitboxSet ring[HB_HISTORY]; int head, count; } HitboxHistory;

typedef struct HitboxTable {
    u32 hash;
    u8 body_part[RAG_MAX_BODIES];     // ragdoll body -> body part
    float radius_scale[RAG_MAX_BODIES];
} HitboxTable;

typedef struct HitboxRayHit { float t; int index; u8 part; vec3 point; } HitboxRayHit;

// Limb status bits (host-authoritative, replicated).
enum LimbStatus { LIMB_LEGS = 1 << 0, LIMB_ARMS = 1 << 1 };

bool hitbox_table_load(HitboxTable *t, const RagdollDef *rd, const struct cJSON *json, char *err, int err_size);
void hitbox_build(const HitboxTable *t, const RagdollDef *rd, const Xform *bone_world, float scale, HitboxSet *out);
void hitbox_record(HitboxHistory *h, const HitboxSet *s);
// Rewinds to a (fixed point *256) tick: exact for whole ticks, interpolated between them.
// Returns false (and the newest set) if the tick is outside the window.
bool hitbox_rewind(const HitboxHistory *h, u32 tick256, HitboxSet *out);
bool hitbox_raycast(const HitboxSet *s, vec3 origin, vec3 dir, float max_dist, HitboxRayHit *out);
// Segment sweep against hitboxes (melee): nearest capsule within radius of the segment.
bool hitbox_sweep(const HitboxSet *s, vec3 a, vec3 b, float radius, HitboxRayHit *out);
// Multiplier tables (BP_COUNT entries each). Missing entries are 1.0.
void part_mult_defaults(float *mult);
void part_mult_parse(float *mult, const struct cJSON *obj);
float damage_falloff(float dist, float start, float end, float min_mult);
DG_INLINE float damage_resolve(float base, const float *char_mult, const float *weapon_mult, int part, float falloff) {
    float cm = (char_mult && part >= 0 && part < BP_COUNT) ? char_mult[part] : 1.0f;
    float wm = (weapon_mult && part >= 0 && part < BP_COUNT) ? weapon_mult[part] : 1.0f;
    return base * cm * wm * falloff;
}

#endif
