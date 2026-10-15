// Procedural animation post-pass (client-side, cosmetic, derived from replicated state).
//
// Fixed order (each step only touches what comes after it):
//   0. pelvis   foot IK pelvis lowering + slope / acceleration lean (moves the whole body)
//   1. spine    turn-in-place counter-rotation + aim pitch / yaw spread over spine..head
//   2. right    right-arm IK places the held item (hold style, ADS, sprint, wall pull-back)
//   3. item     item transform finalized from the solved right hand (glued by construction)
//   4. left     left-hand IK to the item's grip_l socket (position + rotation, item space)
//   5. fingers  per-style finger curl
//   6. legs     two-bone leg IK onto the ground, foot alignment, foot locking
// Nothing after step 4 moves the right hand, the spine or the item. Additive layers
// (damage reactions, breathing, recoil) run before step 0 so the hand solve corrects for them.
#ifndef DG_PROCEDURAL_H
#define DG_PROCEDURAL_H

#include "anim.h"

struct cJSON;

#define HOLD_STYLE_MAX 12
#define DMG_PRESET_MAX 8

typedef enum HoldStyleId {
    HOLD_NONE, HOLD_PISTOL, HOLD_RIFLE, HOLD_SHOTGUN, HOLD_SMG, HOLD_LMG, HOLD_MELEE_1H, HOLD_MELEE_2H, HOLD_ITEM_SMALL, HOLD_ITEM_LARGE,
    HOLD_COUNT
} HoldStyleId;

typedef struct HoldStyle {
    char name[16];
    u32 hash;
    bool left_hand;            // left hand IK to grip_l
    bool left_to_wrist;        // pistol: left hand cups the right wrist
    vec3 hip_offset;           // item origin in aim space (x right, y up, -z forward) relative to the chest anchor
    vec3 hip_rot;              // item rotation in aim space (deg pitch, yaw, roll)
    float ads_distance;        // sights distance in front of the eye
    vec3 sprint_offset;        // lowered sprint pose (aim space)
    vec3 sprint_rot;
    vec3 crouch_offset;        // additional offset while crouched
    vec3 elbow_r, elbow_l;     // pole hints (aim space directions)
    vec3 shoulder_r, shoulder_l; // clavicle rotation (deg)
    vec3 chest_offset;         // aim anchor relative to the Spine2 bone (model axes after yaw)
    float max_shift;           // how far the item may move towards the body to keep the left hand on its grip
    float curl_r[5], curl_l[5];// finger curl (deg) thumb, index, middle, ring, pinky
    float aim_pitch_scale;     // how much the item follows aim pitch (1 = fully)
    float ads_weight;          // how far ADS moves the item to the eye (0 = no ADS pose, melee)
} HoldStyle;

// Item sockets in item space (item origin = right grip, -Z = barrel forward, +Y up).
// A socket frame uses the palm convention: -Z = finger direction, +Y = back of the hand.
typedef struct ItemSockets {
    Xform grip_r, grip_l;
    vec3 muzzle;
    float sight_height;        // top of the item above its origin (ADS eye line)
    float length;              // item length (wall pull-back)
    bool has_left;
} ItemSockets;

typedef struct DamagePreset {
    char name[16];
    float strength;            // degrees per unit hit strength at the hit bone
    float falloff;             // per parent up the chain
    int chain_depth;
    float stiffness, damping;  // spring
    float pelvis_drop;         // leg buckle (m per unit)
    float translate;           // hips push (m per unit)
} DamagePreset;

typedef struct ProcParams {
    // spine / look
    float spine_weights[5];    // spine, spine1, spine2, neck, head (sum ~1)
    float yaw_limit, pitch_limit;  // degrees
    // turn in place
    float turn_threshold;      // degrees of aim vs body before the body turns (sim)
    float turn_speed;          // body turn rate (deg/s, sim)
    int turn_mode;             // 0 procedural, 1 clips
    float foot_lock_release;   // m of foot slide before a step
    // foot IK
    float foot_ray_up, foot_ray_down, pelvis_max_drop, foot_align, foot_smooth, foot_height;
    // lean
    float slope_lean, accel_lean;
    // hold
    HoldStyle styles[HOLD_STYLE_MAX];
    int style_count;
    float style_blend_time;
    // damage
    DamagePreset presets[DMG_PRESET_MAX];
    int preset_count;
    int part_preset[32];       // body part -> preset index
    int hit_mode;              // 0 procedural, 1 clips, 2 both
    u32 hash;
} ProcParams;

bool proc_params_load(ProcParams *p, const struct cJSON *controller_root);
const HoldStyle *proc_hold_style(const ProcParams *p, int style);
int proc_hold_style_index(const ProcParams *p, const char *name);

// Hand frames derived from the skeleton (palm frame relative to the hand bone).
typedef struct HandFrames {
    Xform palm[2];             // [0] left, [1] right: hand bone local -> palm frame
    vec3 curl_axis[2];         // finger curl axis in the palm frame
} HandFrames;
void proc_hand_frames(const Skeleton *s, HandFrames *out);

// Per-character procedural state (render side).
typedef struct FootState {
    bool locked;
    vec3 lock_pos;             // world
    float ground_y, prev_offset;
    vec3 ground_n;
} FootState;

typedef struct DamageSpring { i16 bone; vec3 x, v; float k, c; } DamageSpring;

typedef struct ProcState {
    bool init;
    float render_body_yaw;     // smoothed lower-body yaw (turn in place)
    float pelvis_offset;       // foot IK pelvis drop (smoothed)
    FootState foot[2];
    vec3 prev_vel;
    vec3 lean;                 // smoothed lean angles
    // hold style blend
    int style, prev_style;
    float style_t;             // 0..1 blend from prev to style
    Xform item_model;          // finalized item transform (model space) this evaluation
    Xform item_world;
    bool has_item;
    // damage reactions
    DamageSpring springs[16];
    int spring_count;
    float pelvis_drop_x, pelvis_drop_v;
    vec3 hips_push_x, hips_push_v;
    // diagnostics (glue)
    float glue_err_pos, glue_err_ang;   // left hand <-> grip_l after the solve (m, deg)
    float right_err_pos;
    bool reach_shifted;
    u32 warned_styles;
} ProcState;

// World raycast for foot IK and muzzle pull-back (local physics world, static geometry).
// Returns true on hit with the fraction along dir_len, the point and the surface normal.
typedef bool (*ProcRaycastFn)(void *ctx, vec3 origin, vec3 dir_len, float *fraction, vec3 *point, vec3 *normal);

typedef struct ProcInput {
    const Skeleton *skel;
    const ProcParams *params;
    const HandFrames *hands;
    ProcRaycastFn raycast;     // NULL = flat ground at the root height, no wall checks
    void *ray_ctx;
    Xform root;                // character world transform (feet position, body yaw)
    float dt;
    float body_yaw;            // replicated body yaw
    float aim_yaw, aim_pitch;  // world aim (radians)
    vec3 velocity;
    bool grounded, crouched, sprinting, moving;
    float ads_t;               // 0..1
    int hold_style;            // HoldStyleId
    const ItemSockets *item;   // NULL = nothing held
    float hand_ik_weight;      // controller: 0 while reload / melee clips own the hands
    bool foot_ik, spine, turn, hold, lean, damage;  // per-system enables (LOD + cvars)
    vec3 item_kick_pos, item_kick_rot;   // recoil kick applied to the item (aim space, m / deg)
    float crouch_drop;         // procedural crouch: pelvis lowering (m) when no crouch clips are bound
    vec3 hand_anchor[2];       // world-space ledge anchors (mantle / hang), weight below
    float hand_anchor_w;
} ProcInput;

// Body yaw (simulation, host-owned, predicted locally): faces the aim while moving; while
// standing it only turns once the aim drifts past turn_threshold. Deterministic.
float anim_body_yaw_step(float body_yaw, float aim_yaw, bool moving, const ProcParams *p, float dt, u8 *turning);

void proc_reset(ProcState *s);
// Applies damage springs as additive rotations (call before proc_run). Advances springs by dt.
void proc_damage_apply(ProcState *s, const ProcInput *in, Pose *pose, Xform *model);
// Starts a damage reaction on `bone` from a model-space hit direction.
void proc_damage_hit(ProcState *s, const ProcParams *p, const Skeleton *skel, int body_part, int bone, vec3 dir_model, float strength);
bool proc_damage_active(const ProcState *s);
// Runs the post-pass (steps 0..6). pose/model are in model space relative to in->root.
void proc_run(ProcState *s, const ProcInput *in, Pose *pose, Xform *model);
// Socket world transform from an item transform.
DG_INLINE Xform proc_socket_world(Xform item, Xform socket) { return xf_mul(item, socket); }
// Hand bone transform that puts the palm frame on a socket.
DG_INLINE Xform proc_hand_for_socket(Xform socket_model, Xform palm_local) { return xf_mul(socket_model, xf_inverse(palm_local)); }

#endif
