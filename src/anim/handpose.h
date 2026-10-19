// Hand poses (docs/HANDPOSE.md): universal grips and simulated cases.
//
// A grip is defined once per item (weapon, item, grip attachment) and used in every state and
// view: the item's root alignment relative to its hold frame (position, rotation, uniform scale)
// and per hand the IK target (palm socket in item space, a child of the root), the elbow pole, the
// IK weight and every finger joint (curl, spread, twist). States only move the hold frame.
//
// A case is an interaction whose hands are not simply holding an item (door knob, lever, steering
// wheel, ledge, reload hand-off): an anchor (the case root, an offset from the gameplay target)
// and keyframed hand poses on a timeline (s) with contact / use / release markers. The runtime
// blends the keys exactly as the editor shows them (handpose_case_eval).
//
// Files: assets/handposes/items/<id>.json (grips), cases/<id>.json, styles/<style>.json (the hold
// style / grip style defaults every grip and case inherits). Sparse: a grip stores only what
// differs from its style. Resolution for (entry, hand): base (open hand, no socket) <- each
// ancestor's grip layer, root first.
//
// Pure data + math (no renderer): the runtime hold (src/anim/weapon_hold.c), the interaction and
// vehicle hand IK, the editor (src/editor/handpose) and the tests all use the same code.
#ifndef DG_HANDPOSE_H
#define DG_HANDPOSE_H

#include "anim.h"

struct cJSON;
struct HandFrames;

#define HAND_FINGERS 5
#define HAND_FINGER_JOINTS 3
#define HANDPOSE_SCHEMA 2          // 2: universal grips + cases; bump + add a step to the migration when the file format changes
#define HANDPOSE_CASE_KEYS 12
#define HANDPOSE_DIR "assets/handposes"
#define HANDPOSE_ROOT_SCALE_MIN 0.5f
#define HANDPOSE_ROOT_SCALE_MAX 2.0f

typedef enum HandFinger { HF_THUMB, HF_INDEX, HF_MIDDLE, HF_RING, HF_PINKY } HandFinger;

// Entry categories (grips). Vehicle / interactable entries only exist in schema 1 files: they
// migrate to cases.
typedef enum HandPoseCategory { HPC_STYLE, HPC_WEAPON, HPC_ITEM, HPC_ATTACHMENT, HPC_VEHICLE, HPC_INTERACT, HPC_COUNT } HandPoseCategory;

typedef enum HandCaseKind {
    HCK_INTERACT,              // interactables (door, lever, container, pickup, ...)
    HCK_VEHICLE,               // vehicle seats (steering wheel / handlebar, passenger)
    HCK_ITEM,                  // an item in a forced hold state (phone_portrait): edits the item's grip
    HCK_RELOAD,                // reload_<weapon>: the clip owns the hands, the hand-off markers are editable
    HCK_MANTLE,                // ledge grab
    HCK_STUB,                  // authored, no runtime yet (ladder_rung)
    HCK_COUNT
} HandCaseKind;

// Interaction phases (src/player/hands.c) mapped onto a case timeline.
typedef enum HandCasePhase { HCP_REACH, HCP_HOLD, HCP_RETURN } HandCasePhase;

// One finger joint (degrees): curl = flexion about the palm's curl axis, spread = abduction
// (+ = towards the thumb; thumb: lifts it off the palm), twist = about the bone's own axis.
typedef struct HandJoint { float curl, spread, twist; } HandJoint;
typedef HandJoint HandFingers[HAND_FINGERS][HAND_FINGER_JOINTS];

enum HandPoseLayerBits {
    HPL_SOCKET = 1 << 0,           // the IK target ("ik" in the files)
    HPL_WRIST = 1 << 1,            // legacy palm offset from the socket (schema 1; folded into the IK target when that is set)
    HPL_POLE = 1 << 2, HPL_IKW = 1 << 3,
    HPL_FINGER0 = 1 << 4,          // << finger (5 bits): that finger's three joints are set
    // 1 << 9: unused (was the removed fingertip contacts; the other bits keep their values).
};
#define HPL_FINGER(f) (HPL_FINGER0 << (f))
#define HPL_FINGERS_ALL (0x1Fu << 4)

// Sparse hand layer (a grip hand, a case key hand): only the fields whose bit is in `set` apply.
typedef struct HandPoseLayer {
    u16 set;
    Xform socket;                  // IK target: item space (grips) / relative to the case anchor (cases)
    Xform wrist;                   // legacy palm offset relative to the socket
    vec3 pole;                     // elbow hint (camera space direction, the style's elbow_r / elbow_l)
    float ik_weight;               // multiplies the hold's hand IK weight
    HandFingers j;
} HandPoseLayer;

// Resolved (dense) pose of one hand.
typedef struct HandPose {
    bool has_socket, has_pole;
    Xform socket, wrist;
    vec3 pole;
    float ik_weight;
    HandFingers j;
} HandPose;

typedef struct HandPoseEntry {
    char id[48];                   // file name without .json; styles: "style/<name>"
    u32 hash;                      // dg_hash_str(id)
    char inherits[48];             // parent entry id ("style/rifle"), "" = none
    char source[128];              // asset it was discovered from ("data/weapons/02_rifle.json")
    char label[48];                // display name
    i16 parent;                    // resolved index of `inherits` (-1)
    u8 category;                   // HandPoseCategory
    u8 hands;                      // bit 0 left, bit 1 right: the hands the source uses
    bool reviewed;                 // a person saved it in the editor
    bool auto_placed;              // its sockets were placed by discovery: needs review
    bool orphan;                   // the source asset is gone (listed for removal, never deleted silently)
    bool on_disk;                  // loaded from / saved to a file
    bool seen;                     // discovery found its source this scan
    bool has_root;                 // root set (else identity, scale 1)
    Xform root;                    // the item relative to its hold frame; root.s = uniform scale (x = y = z)
    HandPoseLayer grip[2];         // [0 left, 1 right]
} HandPoseEntry;

typedef struct HandCaseKey {
    float t;                       // s on the case timeline
    char name[16];                 // "contact", "hold", "release" (display)
    HandPoseLayer hand[2];
} HandCaseKey;

typedef struct HandPoseCase {
    char id[48];
    u32 hash;
    char label[48];
    char source[128];
    char inherits[48];             // finger / pole defaults ("style/grip_grab")
    i16 parent;                    // resolved index of `inherits` in the entries (-1)
    u8 kind;                       // HandCaseKind
    u8 hands;                      // bit 0 left, bit 1 right
    char bind[64];                 // "interactable:door", "vehicle:car:driver", ... ("" = unbound)
    u32 bind_hash;                 // dg_hash_str(bind), 0 = unbound
    // editor setup (how the simulated scene is built)
    char prop[24];                 // interactable to spawn ("door", "lever", "container", "pickup", "ledge")
    float distance;                // character to the prop (m)
    char vehicle[24];              // vehicle def ("car", "moto")
    i8 seat;                       // seat index (vehicles)
    char item[32];                 // item / weapon id (item, reload cases)
    char hold_state[24];           // forced hold point state (item cases: "phone_portrait")
    Xform anchor;                  // the case root: offset from the gameplay target
    float duration;                // timeline length (s)
    float contact, use, release;   // markers (s)
    int key_count;
    HandCaseKey keys[HANDPOSE_CASE_KEYS];   // sorted by t
    bool reviewed, on_disk, seen, orphan;
    bool auto_placed;              // the anchor was placed by discovery (no hand target in code): needs review
} HandPoseCase;

typedef struct HandPoseDB {
    HandPoseEntry *e;
    int count, cap;
    HandPoseCase *c;
    int case_count, case_cap;
    u32 hash;                      // net identity: every grip / case that changes a pose (stubs don't count)
    u32 revision;                  // bumped on every load / edit commit
} HandPoseDB;

// What a schema 1 file became (tools/handpose_migrate, the load log).
typedef struct HandPoseMigrateNote {
    int from;                      // schema read
    int dropped;                   // state layers that differed from the grip (dropped)
    char text[512];                // "ads: right index differs; sprint: left socket differs"
} HandPoseMigrateNote;

const char *handpose_category_name(int c);
const char *handpose_case_kind_name(int k);
int handpose_case_kind_from_name(const char *s);
const char *handpose_finger_name(int f);
int handpose_finger_from_name(const char *s);
u32 handpose_style_hash(const char *style_name);   // dg_hash_str("style/<name>")
// Entry id for a source: weapons / items use their id, attachments get "att_" ("att_vertical_grip"),
// styles "style/".
void handpose_entry_id(char *out, int out_size, int category, const char *name);
u32 handpose_entry_hash(int category, const char *name);

void handpose_db_init(HandPoseDB *db);
void handpose_db_free(HandPoseDB *db);
void handpose_db_clear(HandPoseDB *db);
int handpose_find(const HandPoseDB *db, const char *id);
int handpose_find_hash(const HandPoseDB *db, u32 hash);
// Adds (or returns) the entry with `id`. Pointers into db->e are invalidated by adds.
int handpose_add(HandPoseDB *db, const char *id, int category);
void handpose_link(HandPoseDB *db);          // resolves `inherits` -> parent (cycles cut), entries and cases
void handpose_rehash(HandPoseDB *db);
bool handpose_entry_has_overrides(const HandPoseEntry *e);
void handpose_layer_clear(HandPoseLayer *l);
void handpose_entry_clear_layers(HandPoseEntry *e);   // grip + root

// Cases.
int handpose_case_add(HandPoseDB *db, const char *id, int kind);   // adds (or returns); defaults set on add
int handpose_case_find(const HandPoseDB *db, const char *id);
int handpose_case_find_hash(const HandPoseDB *db, u32 hash);
int handpose_case_find_bind(const HandPoseDB *db, const char *bind);
int handpose_case_find_bind_hash(const HandPoseDB *db, u32 bind_hash);
void handpose_case_defaults(HandPoseCase *c);   // markers / duration of the interaction defaults, no keys
bool handpose_case_has_overrides(const HandPoseCase *c);
void handpose_case_sort_keys(HandPoseCase *c);
// Adds a key at t (or returns the one within 1 ms of it), seeded from the case's pose at t.
int handpose_case_key_at(HandPoseDB *db, int ci, float t);
bool handpose_case_key_remove(HandPoseCase *c, int k);

// Grip resolution of one entry's chain (entry < 0 or unknown -> open hand, no overrides).
void handpose_resolve(const HandPoseDB *db, int entry, int hand, HandPose *out);
// The runtime form: the base chain (the hold style in use) first, then the entry's own chain on
// top (an item, an attachment grip). A base already in the entry's chain is applied once.
void handpose_resolve_over(const HandPoseDB *db, int base, int entry, int hand, HandPose *out);
// The entry's root (identity, scale 1 when unset or entry < 0). out.s = uniform scale.
Xform handpose_root(const HandPoseDB *db, int entry);
Xform handpose_root_clamp(Xform r);   // normalized rotation, uniform scale clamped

// Case evaluation at timeline time t (s): the base chain (a grip style), the case's `inherits`
// chain, then the keys blended (smoothstep between the neighbouring keys, held before the first
// and after the last). The socket is the anchor * the key's IK target (relative to the gameplay
// target); has_socket is always true. ci < 0: the base alone (identity socket).
void handpose_case_eval(const HandPoseDB *db, int ci, int base, int hand, float t, HandPose *out);
// The anchor the evaluation applies (identity for a driver seat: its anchor moves the steering
// frame, so the gameplay target already contains it).
bool handpose_case_anchor_in_target(const HandPoseCase *c);
Xform handpose_case_anchor_applied(const HandPoseCase *c);
// One key's dense pose (the same layering, no blend).
void handpose_case_key_pose(const HandPoseDB *db, int ci, int base, int key, int hand, HandPose *out);
// Phase (reach / hold / return) + progress 0..1 -> case time, and back. Reach spans [0, contact],
// hold [contact, release], return [release, duration].
float handpose_case_time(const HandPoseCase *c, int phase, float u);
void handpose_case_phase(const HandPoseCase *c, float t, int *phase, float *u);
// Durations the interaction runtime uses: the reach (= contact marker) and the return.
float handpose_case_reach_time(const HandPoseCase *c);
float handpose_case_return_time(const HandPoseCase *c);

// Joint ranges (degrees): every finger joint rotates freely on every axis (curl, spread, twist),
// -180 .. +180 (a full turn). The clamp (on load, on every edit and when resolving) only keeps
// values inside that turn: it never restricts a pose. No automatic finger placement.
void handpose_joint_limits(int finger, int joint, HandJoint *lo, HandJoint *hi);
HandJoint handpose_clamp_joint(int finger, int joint, HandJoint v);
void handpose_clamp_fingers(HandFingers j);
// 5-value curl (thumb..pinky degrees, the "curl" shorthand of the files and the presets) ->
// joints: proximal 30 %, middle 38 %, distal 32 %; thumb x 0.6.
void handpose_from_curl(const float curl[5], HandFingers out);
// The curl of one finger as the shorthand reads it (the proximal joint / its share).
float handpose_finger_curl(const HandFingers j, int finger);
void handpose_set_finger_curl(HandFingers j, int finger, float curl);   // the three joints together (each kept within +-180)

// Finger presets (applied per hand, then tweaked).
int handpose_preset_count(void);
const char *handpose_preset_name(int i);
bool handpose_preset(const char *name, HandFingers out);

// Mirror L <-> R: socket / wrist reflected across the YZ plane (x -> -x, the palm frame kept
// right-handed), the pole hint's x negated; finger values are hand-relative and unchanged.
Xform handpose_mirror_xform(Xform x);
void handpose_mirror_layer(const HandPoseLayer *in, HandPoseLayer *out);
void handpose_mirror_entry_hand(HandPoseEntry *e, int from_hand);   // grip: copies from_hand onto the other, mirrored
void handpose_mirror_case_hand(HandPoseCase *c, int from_hand);     // every key

// Applies one hand's finger joints (side 0 left, 1 right), scaled by weight. Fingers never move
// the wrist; the finger chains' model transforms are updated. Zero spread / twist cost nothing.
void handpose_apply_fingers(const Skeleton *s, Pose *pose, Xform *model, int side, const HandFingers j, const struct HandFrames *hf, float weight);
// Fingertip (model space) of finger f of the hand: the end of the distal joint.
vec3 handpose_fingertip(const Skeleton *s, const Xform *model, int side, int finger);
// Bone of finger f, joint k (0..2) of a hand side (-1 when the skeleton lacks it).
int handpose_finger_bone(const Skeleton *s, int side, int finger, int joint);

// JSON. Parse fills an entry / a case, migrating older schemas (note: what the migration did, may
// be NULL); write produces the canonical text (stable key order, shortest round-trip floats, only
// set fields) - malloc'd, caller frees.
bool handpose_entry_parse(HandPoseEntry *e, const struct cJSON *root, char *err, int err_size, HandPoseMigrateNote *note);
char *handpose_entry_write(const HandPoseEntry *e);
bool handpose_case_parse(HandPoseCase *c, const struct cJSON *root, char *err, int err_size);
char *handpose_case_write(const HandPoseCase *c);
// Schema 1 vehicle / interactable entries -> a case: the id and binding it gets
// ("use_door" -> "use_door", "interactable:door"; "veh_car" -> "vehicle_car_driver", "vehicle:car:driver")
// and the single key (the default layer with use / drive on top).
bool handpose_case_from_v1(HandPoseCase *c, const struct cJSON *root, const char *id, HandPoseMigrateNote *note);
// Files: styles/*.json, items/*.json, cases/*.json (sorted, deterministic) and, only when items/
// doesn't exist, schema 1 files directly in the directory (migrated in memory). Missing directory
// = empty DB. Returns the number of files read.
int handpose_db_load_dir(HandPoseDB *db, const char *dir_rel);
bool handpose_load_file(HandPoseDB *db, const char *path_abs, char *err, int err_size);   // a grip or a case (by its directory)
bool handpose_entry_save_file(const HandPoseEntry *e, const char *path_abs);
bool handpose_case_save_file(const HandPoseCase *c, const char *path_abs);
// "<dir>/items/<id>.json" (styles: "<dir>/styles/<name>.json"), "<dir>/cases/<id>.json".
void handpose_entry_rel_path(const HandPoseEntry *e, const char *dir_rel, char *out, int out_size);
void handpose_case_rel_path(const HandPoseCase *c, const char *dir_rel, char *out, int out_size);

// First-person interaction grips (GripType order, src/player/hands.h): "style/grip_<name>".
int handpose_grip_count(void);
const char *handpose_grip_name(int grip);     // "relaxed", "fist", "point", "grab", "pistol", "rifle", "melee", "palm"
u32 handpose_grip_style_hash(int grip);

// Euler (pitch, yaw, roll degrees, the hold point convention) <-> quaternion, for the editor's fields.
vec3 handpose_quat_to_euler_deg(quat q);
quat handpose_euler_deg_to_quat(vec3 e);

#endif
