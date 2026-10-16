// Skeletal animation core: transforms, skeletons, baked clips and pose operations.
//
// Everything here is pure (no engine state) and deterministic: the same inputs give the
// same bits on every machine (no fast-math, fixed evaluation order), so host and clients
// sample identical poses for the same tick. Unit tested by tests/test_anim.c.
//
// Conventions: meters, Y up, a character faces -Z in model space (the game's yaw 0).
// Bones are stored topologically sorted (parent index < child index).
#ifndef DG_ANIM_H
#define DG_ANIM_H

#include "core/base.h"
#include "core/dgmath.h"

#define ANIM_MAX_BONES 128
#define ANIM_NAME_MAX 48
#define ANIM_DEFAULT_RATE 30.0f
#define ANIM_MAX_CLIP_EVENTS 8

typedef struct Xform { vec3 t; quat r; vec3 s; } Xform;

DG_INLINE Xform xf_identity(void) { Xform x = { { 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1 } }; return x; }
DG_INLINE Xform xf_make(vec3 t, quat r) { Xform x = { t, r, { 1, 1, 1 } }; return x; }
// a * b: apply b first, then a (parent * local).
DG_INLINE Xform xf_mul(Xform a, Xform b) {
    Xform r;
    r.t = v3_add(a.t, quat_rotate(a.r, v3_mul(a.s, b.t)));
    r.r = quat_mul(a.r, b.r);
    r.s = v3_mul(a.s, b.s);
    return r;
}
DG_INLINE Xform xf_inverse(Xform a) {
    Xform r;
    r.r = quat_conj(a.r);
    r.s = v3(1.0f / a.s.x, 1.0f / a.s.y, 1.0f / a.s.z);
    r.t = v3_mul(r.s, quat_rotate(r.r, v3_neg(a.t)));
    return r;
}
DG_INLINE vec3 xf_point(Xform a, vec3 p) { return v3_add(a.t, quat_rotate(a.r, v3_mul(a.s, p))); }
DG_INLINE vec3 xf_dir(Xform a, vec3 d) { return quat_rotate(a.r, d); }
DG_INLINE quat quat_dot_fix(quat a, quat b) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    return b;
}
DG_INLINE Xform xf_lerp(Xform a, Xform b, float t) {
    Xform r;
    r.t = v3_lerp(a.t, b.t, t);
    r.r = quat_nlerp(a.r, b.r, t);
    r.s = v3_lerp(a.s, b.s, t);
    return r;
}
DG_INLINE mat4 xf_to_mat4(Xform a) { return mat4_trs(a.t, a.r, a.s); }
DG_INLINE float quat_dot(quat a, quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
// FNV-1a over bytes, chained through h (start with 2166136261u). Stable wire / cache hashes.
DG_INLINE u32 anim_hash(const void *data, size_t n, u32 h) {
    const u8 *p = (const u8 *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
#define ANIM_HASH_SEED 2166136261u
DG_INLINE u32 anim_hash_str(const char *s, u32 h) { return anim_hash(s, strlen(s), h); }

// Shortest rotation taking unit vector a onto unit vector b.
quat quat_from_to(vec3 a, vec3 b);
// Rotation as a scaled axis (angle * axis) and back; used by springs and inertialization.
vec3 quat_to_scaled_axis(quat q);
quat quat_from_scaled_axis(vec3 v);
// Angle between two rotations (radians, 0..pi).
float quat_angle_between(quat a, quat b);
// Twist of q around a unit axis.
quat quat_twist(quat q, vec3 axis);

// ------------------------------------------------------------------ skeleton

typedef enum BoneRole {
    BR_HIPS, BR_SPINE, BR_SPINE1, BR_SPINE2, BR_NECK, BR_HEAD, BR_HEAD_END,
    BR_L_SHOULDER, BR_L_ARM, BR_L_FOREARM, BR_L_HAND,
    BR_R_SHOULDER, BR_R_ARM, BR_R_FOREARM, BR_R_HAND,
    BR_L_UPLEG, BR_L_LEG, BR_L_FOOT, BR_L_TOE, BR_L_TOE_END,
    BR_R_UPLEG, BR_R_LEG, BR_R_FOOT, BR_R_TOE, BR_R_TOE_END,
    BR_L_THUMB1, BR_L_INDEX1, BR_L_MIDDLE1, BR_L_RING1, BR_L_PINKY1,
    BR_R_THUMB1, BR_R_INDEX1, BR_R_MIDDLE1, BR_R_RING1, BR_R_PINKY1,
    BR_COUNT
} BoneRole;

typedef struct Skeleton {
    u32 hash;                               // stable id: names + hierarchy + bind (wire identity)
    int count;
    char names[ANIM_MAX_BONES][ANIM_NAME_MAX];
    i16 parent[ANIM_MAX_BONES];
    Xform bind[ANIM_MAX_BONES];             // local bind pose
    Xform bind_model[ANIM_MAX_BONES];       // model-space bind pose
    float length[ANIM_MAX_BONES];           // distance to the first child (leaf: offset from parent)
    i16 mirror[ANIM_MAX_BONES];             // left <-> right counterpart (self if centered)
    i16 role[BR_COUNT];                     // -1 if missing
    u8 lod_keep[ANIM_MAX_BONES];            // 1 = animated at reduced-bone LODs (LOD2+)
    float height;                           // head_end height in bind pose
    char source[128];
} Skeleton;

int skel_find(const Skeleton *s, const char *name);           // exact name, -1 if missing
int skel_find_suffix(const Skeleton *s, const char *suffix);  // "Hips" matches "mixamorig1:Hips"
// Fills derived data (bind_model, length, mirror, roles, lod_keep, height, hash) from names/parent/bind.
void skel_finalize(Skeleton *s);
const char *bone_role_name(BoneRole r);
bool skel_is_descendant(const Skeleton *s, int bone, int ancestor);

// ------------------------------------------------------------------ clips

typedef enum ClipEventType { CEV_NONE, CEV_FOOTSTEP_L, CEV_FOOTSTEP_R, CEV_WEAPON_FIRE, CEV_CUSTOM } ClipEventType;
typedef struct ClipEvent { float time; u8 type; char name[23]; } ClipEvent;

typedef struct Clip {
    u32 hash;                   // name + content (wire identity)
    char name[64];              // slot-independent clip name ("walking", "file:take")
    float rate;                 // baked samples per second
    int frames;                 // sample count (>= 1)
    float duration;             // seconds ((frames - 1) / rate, looping clips wrap)
    int bones;
    Xform *data;                // frames * bones local transforms, root motion stripped (in place)
    vec3 *root_t;               // per frame: extracted root travel in model space (XZ, relative to frame 0)
    float *root_yaw;            // per frame: extracted yaw (radians, relative to frame 0)
    bool loop;
    bool root_motion;           // gameplay applies root_t / root_yaw (host only)
    bool additive;
    int additive_ref;           // reference pose clip index for additive (-1 = own first frame)
    float speed;                // playback multiplier
    float avg_speed;            // m/s of the extracted travel (locomotion speed matching)
    float plant[2];             // normalized phase of left / right foot plant (sync markers)
    bool has_markers;
    float blend_in, blend_out;  // defaults (seconds)
    ClipEvent events[ANIM_MAX_CLIP_EVENTS];
    int event_count;
} Clip;

void clip_free(Clip *c);
// Phase helpers: clip time from integer ticks (never accumulated float seconds).
float clip_time_from_ticks(const Clip *c, u32 ticks, float tick_dt, float speed);
// Samples local pose at time (seconds). Looping clips wrap, others clamp.
typedef struct Pose { int count; Xform local[ANIM_MAX_BONES]; } Pose;
void clip_sample(const Clip *c, float time, Pose *out);
void clip_sample_phase(const Clip *c, float phase, Pose *out);   // phase 0..1
// Root travel between two times (model space XZ + yaw), handling loop wrap.
void clip_root_delta(const Clip *c, float t0, float t1, vec3 *dpos, float *dyaw);
// Auto-detect foot plant markers from foot heights (fills plant[], has_markers).
void clip_detect_markers(Clip *c, const Skeleton *s);

// ------------------------------------------------------------------ poses

void pose_bind(const Skeleton *s, Pose *out);
void pose_copy(Pose *dst, const Pose *src);
// out = lerp(a, b, t). a == out allowed.
void pose_blend(const Pose *a, const Pose *b, float t, Pose *out);
// Per-bone weights (0..1, NULL = uniform t).
void pose_blend_masked(const Pose *a, const Pose *b, const float *weights, float t, Pose *out);
// out = base + (add - ref) * w per bone (local space). w == 0 returns base exactly.
void pose_additive(const Pose *base, const Pose *add, const Pose *ref, float w, const float *mask, Pose *out);
// Left/right mirror using the skeleton's mirror table (model-space reflection across X).
void pose_mirror(const Skeleton *s, const Pose *in, Pose *out);
// Local -> model space. model must hold s->count entries.
void pose_to_model(const Skeleton *s, const Pose *p, Xform *model);
// Recompute one bone's local from desired model transform (parent model given).
void pose_set_model(const Skeleton *s, Pose *p, Xform *model, int bone, Xform desired_model);
// Recompute model transforms of all descendants of bone after its model changed.
void pose_update_children(const Skeleton *s, const Pose *p, Xform *model, int bone);

// Bone weight masks (per bone 0..1).
typedef struct BoneMask { float w[ANIM_MAX_BONES]; } BoneMask;
void mask_fill(BoneMask *m, float v);
// Sets weight v on bone and all its descendants.
void mask_set_chain(const Skeleton *s, BoneMask *m, int bone, float v);

// ------------------------------------------------------------------ IK (shared with the first-person hands)

// Analytic two-bone IK on points: elbow/wrist for a chain rooted at `root` with segment
// lengths a, b towards target, bending towards `pole` (a direction). Returns false when the
// target was out of reach (chain fully extended along the target direction).
bool anim_ik_two_bone(vec3 root, vec3 target, vec3 pole, float a, float b, vec3 *mid, vec3 *end);
// Pose version: rotates bones ia (upper) and ib (lower) so bone ic lands on target_pos;
// if target_rot != NULL, ic's model rotation is set to it. weight blends with the input.
// model must be current for the whole skeleton; it is updated for the chain and descendants.
bool anim_ik_two_bone_pose(const Skeleton *s, Pose *p, Xform *model, int ia, int ib, int ic, vec3 target_pos,
                           const quat *target_rot, vec3 pole_dir, float weight);
// Rotates bone so that its child direction (model) points at target (look-at style), weight 0..1.
void anim_aim_bone(const Skeleton *s, Pose *p, Xform *model, int bone, vec3 local_axis, vec3 target_dir, float weight);

#endif
