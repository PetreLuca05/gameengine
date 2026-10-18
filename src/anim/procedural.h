// Procedural animation post-pass (client-side, cosmetic, derived from replicated state).
//
// Fixed order (each step only touches what comes after it):
//   0. pelvis   foot IK pelvis lowering + slope / acceleration lean (moves the whole body)
//   1. spine    turn-in-place counter-rotation + aim pitch / yaw spread over spine..head
//   2. right    hold point (camera = head bone + eye socket, view rotation; item = camera * hold
//               point: state, ADS from the sight socket, sway / lag / bob / kick, wall collision,
//               src/player/holdpoint.c) -> right-arm IK places the item
//   3. item     item transform finalized from the solved right hand (glued by construction)
//   4. left     left-hand IK to the item's grip_l socket (position + rotation, item space)
//   5. fingers  per-style finger curl
//   6. legs     two-bone leg IK onto the ground, foot alignment, foot locking
// Nothing after step 4 moves the right hand, the spine or the item. Additive layers
// (damage reactions, breathing, recoil) run before step 0 so the hand solve corrects for them.
#ifndef DG_PROCEDURAL_H
#define DG_PROCEDURAL_H

#include "anim.h"
#include "handpose.h"
#include "player/holdpoint.h"

struct cJSON;

#define HOLD_STYLE_MAX 16
#define DMG_PRESET_MAX 8

typedef enum HoldStyleId {
    HOLD_NONE, HOLD_PISTOL, HOLD_RIFLE, HOLD_SHOTGUN, HOLD_SMG, HOLD_LMG, HOLD_MELEE_1H, HOLD_MELEE_2H, HOLD_ITEM_SMALL, HOLD_ITEM_LARGE,
    HOLD_PHONE, HOLD_PHONE_LANDSCAPE,   // docs/PHONE.md: one-handed portrait, two-handed landscape
    HOLD_COUNT
} HoldStyleId;

typedef struct HoldStyle {
    char name[16];
    u32 hash;
    bool left_hand;            // left hand IK to grip_l
    bool left_to_wrist;        // pistol: left hand cups the right wrist
    vec3 elbow_r, elbow_l;     // pole hints (camera space directions)
    vec3 shoulder_r, shoulder_l; // clavicle rotation (deg)
    float max_shift;           // how far the item may move towards the body to keep the left hand on its grip
    HoldPointStyle hp;         // where the item sits relative to the camera (assets/human/holdpoint.json)
} HoldStyle;

// Item sockets in item space (item origin = right grip, -Z = barrel forward, +Y up).
// A socket frame uses the palm convention: -Z = finger direction, +Y = back of the hand.
typedef struct ItemSockets {
    Xform grip_r, grip_l;
    Xform anchor;              // the socket placed on the hold point (default grip_r)
    vec3 muzzle;
    vec3 sight;                // ADS: this point lands on the view axis at the style's eye relief
    vec3 pivot;                // rotation pivot of sway / kick / lag (default between the hands)
    float sight_height;        // top of the item above its origin
    float length;              // item length (wall collision cast)
    bool has_left;
    HoldPointOverride hp;      // per item hold point override ("holdpoint" block)
    // Hand poses (docs/HANDPOSE.md): the entry per hand (dg_hash_str of its id, 0 = the style
    // only) and, for a socket defined in another space (an attachment grip), that space in item space.
    u32 pose_id[2];
    bool has_pose_space[2];
    Xform pose_space[2];
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
    float spine_weights_ads_up[5]; // looking up in ADS: mostly the lower spine, the upper body rotates with the gun
    float spine_weights_up[5]; // pitch weights when looking up: mostly neck / head, so the torso
                               // doesn't lean back (the head stays over the body)
    float yaw_limit, pitch_limit;  // degrees (pitch_limit: looking down)
    float pitch_limit_up;          // degrees, looking up (ADS: the body follows the raised gun)
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
bool proc_holdpoint_load(ProcParams *p, const struct cJSON *holdpoint_root);   // after proc_params_load
void proc_params_rehash(ProcParams *p);
const HoldStyle *proc_hold_style(const ProcParams *p, int style);
int proc_hold_style_index(const ProcParams *p, const char *name);
// How far the support hand may slide back along the handguard (item +Z) in ADS to stay in reach
// instead of moving the item off its exact ADS transform (keeps 8 cm ahead of the grip).
float proc_grip_l_slide_max(const ItemSockets *k);

// Hand frames derived from the skeleton (palm frame relative to the hand bone).
typedef struct HandFrames {
    Xform palm[2];             // [0] left, [1] right: hand bone local -> palm frame
    vec3 curl_axis[2];         // finger curl axis in the palm frame
    float thumb_side[2];       // sign of the thumb along the palm frame's x (finger spread direction)
} HandFrames;
void proc_hand_frames(const Skeleton *s, HandFrames *out);

// One hand's resolved grip on the held item (item space socket incl. the wrist offset and the root
// scale, pole hint, IK weight, fingers), inertialized when the entry / style / data changes.
typedef struct HandHold {
    bool init;
    u32 key;                   // entry, base, state, data revision
    float t, dur;              // blend since the last change (t >= dur: settled)
    Xform sock;                // item space palm target (socket * wrist)
    vec3 pole;
    float ikw;
    HandFingers j;
    // decaying offsets captured at the change (current - new target)
    vec3 off_t;
    quat off_r;
    vec3 off_pole;
    float off_ikw;
    HandFingers off_j;
} HandHold;

// Per-character procedural state (render side).
typedef struct FootState {
    bool locked;
    vec3 lock_pos;             // world
    float lock_w;              // lock blend weight (eased in / out: no snaps)
    float ground_y, prev_offset;
    vec3 ground_n;
} FootState;

typedef struct DamageSpring { i16 bone; vec3 x, v; float k, c; } DamageSpring;

typedef struct ProcState {
    bool init;
    float render_body_yaw;     // smoothed lower-body yaw (turn in place)
    float kneel_w;             // smoothed kneel weight (proc_kneel)
    float pelvis_offset;       // foot IK pelvis drop (smoothed)
    FootState foot[2];
    vec3 prev_vel;
    vec3 lean;                 // smoothed lean angles
    float ads_lean;            // 0..1: looking up in ADS leans the upper body back with the gun (smoothed ads_t)
    float clip_chest_yaw;      // the clip's chest yaw off the body this tick (deg, + = left): the hold's bladed stance turns the chest to its twist from it
    // hold style blend
    int style, prev_style;
    float style_t;             // 0..1 blend from prev to style
    Xform item_model;          // finalized item transform (model space) this evaluation
    float item_scale;          // the grip root's calibration scale (drawn items scale by it)
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
    float reach_shift;
    float grip_l_slide;              // ADS: support hand slid back along the handguard (m, item +Z) to stay in reach
    // hold point (camera space item placement)
    HoldPointState hp;
    HoldPointOutput hp_out;
    Xform cam_model;           // camera (head bone + eye socket, view rotation), model space
    Xform item_cam;            // item target in camera space (pre-IK; the owner re-solves per frame)
    u32 warned_styles;
    // Phone driven by a paired real phone (docs/PHONE.md): blend weight towards the device
    // rotation (eases in on pairing, back to the mouse aim on disconnect) and the last rotation.
    float phone_w;
    quat phone_rot;
    // Hand poses on the held item this evaluation ([0] left, [1] right; valid with has_item).
    HandHold hand[2];
    // Swimming (swim.c): blend weight, smoothed body tilt (deg), stroke phase.
    float swim_w, swim_tilt, swim_phase;
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
    float lod_w;               // weight of the LOD0 systems (foot IK, lean, turn), faded over a pixel band (docs/LOD.md); 1 = full
    vec3 item_kick_pos, item_kick_rot;   // recoil kick applied to the item (aim space, m / deg)
    float crouch_drop;         // procedural crouch: pelvis lowering (m) when no crouch clips are bound
    float kneel;               // 0..1 target: knocked down (docs/REVIVE.md), knees on the ground, torso up
    bool first_person;         // the local player's own body (diagnostics only: never changes the pose)
    int eye_bone;              // camera parent bone (Head) and the eye socket relative to it
    Xform eye_local;
    bool prone, inspect, empty_hands;   // hold point state flags (empty: mantle / hang / vault)
    int item_key;              // hold point blend key (changes when the held item changes)
    bool hp_static;            // hold point without its procedural layer (tests / editor)
    float hp_sway_scale;       // hold point look lag / sway / breathing / sprint bob (wpn_sway_scale, 0 = none)
    HoldKickParams hold_kick;  // kick / sway anchors, ADS kick shaping (from the weapon's recoil data)
    float hp_bob_scale;        // hold point movement bob outside sprint (wpn_bob_scale)
    bool hp_phase_valid;       // deterministic hold point phases (players simulated here): clock (s) + stride
    double hp_clock;
    float hp_stride;
    vec3 hand_anchor[2];       // world-space ledge anchors (mantle / hang), weight below
    float hand_anchor_w;
    // Seated (vehicles, docs/VEHICLES.md): the pelvis on the seat's hip point, thighs flexed, feet
    // IK'd to the pedals / pegs (world space). Foot IK and lean are off while seated.
    bool seated;
    bool hand_anchor_rot_valid;   // hand_anchor_rot: the palms' world orientation (else palm down)
    quat hand_anchor_rot[2];
    bool vehicle_onehand;      // hold point HP_VEHICLE_ONEHAND (driver aiming one-handed)
    u8 hand_anchor_skip;       // bit h: hand h is not anchored (one-handed driving: the gun hand)
    // Phone (docs/PHONE.md): camera mode (1 portrait, 2 landscape; hold point HP_PHONE_*) and the
    // device rotation relative to the view yaw while a real phone drives it.
    u8 phone_mode;
    bool phone_driven;
    quat phone_rot;
    vec3 seat_hip;
    vec3 seat_feet[2];
    // Hand poses (docs/HANDPOSE.md). NULL = open hands on the item's sockets.
    const HandPoseDB *handposes;
    bool no_fingers;           // editor / contact solver: leave the fingers at the clip pose
    // World anchors (vehicle controls, ledges): the case (hash) + its timeline time and the grip
    // style under it, per hand; the case socket is an offset from the anchor.
    u32 anchor_case[2], anchor_pose_base[2];
    float anchor_case_t[2];
    // Swimming (docs/WATER.md "Swim animation", swim.c): 0 no, 1 at the surface, 2 under water; the
    // swim speed (m/s), the surface height (world, -1e9 unknown), the simulation's stroke phase when
    // this machine simulates the player (else a local one), hands holding the item (bit h: skip
    // the stroke on hand h).
    u8 swim;
    u8 swim_hold_mask;
    bool swim_clips;           // the swim clips play (controller state 'swim'): no procedural stroke
    bool swim_phase_valid;
    float swim_speed, swim_phase, water_y;
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
// The held item's grip root (its alignment in the hold frame, .s = calibration scale) and one
// hand's universal grip target on it (the same code as the runtime hold: tests, editor).
Xform proc_item_root(const ProcInput *in, const ItemSockets *it);
void proc_hand_grip(const HandPoseDB *db, const HoldStyle *hs, const ItemSockets *it, int hand, float root_scale, HandHold *o);
// Socket world transform from an item transform.
DG_INLINE Xform proc_socket_world(Xform item, Xform socket) { return xf_mul(item, socket); }
// Hand bone transform that puts the palm frame on a socket.
DG_INLINE Xform proc_hand_for_socket(Xform socket_model, Xform palm_local) { return xf_mul(socket_model, xf_inverse(palm_local)); }

#endif
