// Hand poses (docs/HANDPOSE.md): per entry (weapon, item, attachment grip, vehicle controls,
// interactable, hold style) x hand x state - the hand socket the palm is glued to, a wrist
// offset, the elbow pole hint, the IK weight and every finger joint (curl, spread, twist).
//
// Files: assets/handposes/<id>.json (one per entry, only overrides) inheriting from
// assets/handposes/styles/<style>.json. Resolution for (entry, hand, state):
//   base (open hand, no socket override) <- for each ancestor, root first: layer[default], layer[state]
// so an item stores only what differs from its style, and a field set on the item's "default"
// state wins over the style's per-state value. Entries without overrides resolve to the style,
// which is generated from the old finger-curl tables: nothing changes until a pose is edited.
//
// Pure data + math (no renderer): the runtime hold (src/anim/weapon_hold.c), the first-person
// interaction pass, the editor (src/editor/handpose) and the tests all use the same code.
#ifndef DG_HANDPOSE_H
#define DG_HANDPOSE_H

#include "anim.h"

struct cJSON;
struct HandFrames;

#define HAND_FINGERS 5
#define HAND_FINGER_JOINTS 3
#define HANDPOSE_SCHEMA 1          // bump + add a step to handpose_migrate when the file format changes
#define HANDPOSE_CONTACTS 5        // one fingertip target per finger
#define HANDPOSE_DIR "assets/handposes"

typedef enum HandFinger { HF_THUMB, HF_INDEX, HF_MIDDLE, HF_RING, HF_PINKY } HandFinger;

// States: "default" (every state inherits it), the hold point states (HoldState + 1), the
// interactable and vehicle-control states.
typedef enum HandPoseState {
    HPS_DEFAULT,
    HPS_HIP, HPS_ADS, HPS_SPRINT, HPS_CROUCH, HPS_PRONE, HPS_EMPTY, HPS_INSPECT, HPS_VEHICLE_ONEHAND,
    HPS_PHONE_PORTRAIT, HPS_PHONE_LANDSCAPE,
    HPS_USE, HPS_HOLD,             // interactables: the press / a hold interaction (container lid)
    HPS_DRIVE, HPS_DRIVE_ONEHAND,  // vehicle controls: both hands / the other hand holds a gun
    HPS_COUNT
} HandPoseState;
#define HPS_FROM_HOLD(hs) ((int)(hs) + 1)

typedef enum HandPoseCategory { HPC_STYLE, HPC_WEAPON, HPC_ITEM, HPC_ATTACHMENT, HPC_VEHICLE, HPC_INTERACT, HPC_COUNT } HandPoseCategory;

// One finger joint (degrees): curl = flexion about the palm's curl axis, spread = abduction
// (+ = towards the thumb; thumb: lifts it off the palm), twist = about the bone's own axis.
typedef struct HandJoint { float curl, spread, twist; } HandJoint;
typedef HandJoint HandFingers[HAND_FINGERS][HAND_FINGER_JOINTS];

// Fingertip target on the item (entry space): the contact solver closes the finger onto it.
typedef struct HandContact { u8 finger; vec3 point; } HandContact;

enum HandPoseLayerBits {
    HPL_SOCKET = 1 << 0, HPL_WRIST = 1 << 1, HPL_POLE = 1 << 2, HPL_IKW = 1 << 3,
    HPL_FINGER0 = 1 << 4,          // << finger (5 bits): that finger's three joints are set
    HPL_CONTACTS = 1 << 9,
};
#define HPL_FINGER(f) (HPL_FINGER0 << (f))
#define HPL_FINGERS_ALL (0x1Fu << 4)

// Sparse layer: only the fields whose bit is in `set` apply.
typedef struct HandPoseLayer {
    u16 set;
    u8 contact_count;
    Xform socket;                  // palm target: entry space (absolute) or an offset from the gameplay target (relative entries)
    Xform wrist;                   // palm offset relative to the socket (palm frame)
    vec3 pole;                     // elbow hint (camera space direction, the style's elbow_r / elbow_l)
    float ik_weight;               // multiplies the hold's hand IK weight
    HandFingers j;
    HandContact contacts[HANDPOSE_CONTACTS];
} HandPoseLayer;

// Resolved (dense) pose.
typedef struct HandPose {
    bool has_socket, has_pole;
    u8 contact_count;
    Xform socket, wrist;
    vec3 pole;
    float ik_weight;
    HandFingers j;
    HandContact contacts[HANDPOSE_CONTACTS];
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
    u32 states;                    // bit per HandPoseState the source uses
    bool relative;                 // socket = offset from the gameplay target (vehicles, interactables)
    bool reviewed;                 // a person saved it in the editor
    bool auto_placed;              // its sockets were placed by discovery (bounds / type): needs review
    bool orphan;                   // the source asset is gone (listed for removal, never deleted silently)
    bool on_disk;                  // loaded from / saved to a file
    bool seen;                     // discovery found its source this scan
    bool dirty;                    // edited since the last save / load (editor; never written)
    HandPoseLayer layer[2][HPS_COUNT];   // [hand 0 left, 1 right][state]
} HandPoseEntry;

typedef struct HandPoseDB {
    HandPoseEntry *e;
    int count, cap;
    u32 hash;                      // net identity: every entry that changes a pose (stubs don't count)
    u32 revision;                  // bumped on every load / edit commit
} HandPoseDB;

const char *handpose_state_name(int s);
int handpose_state_from_name(const char *s);
const char *handpose_category_name(int c);
const char *handpose_finger_name(int f);
u32 handpose_style_hash(const char *style_name);   // dg_hash_str("style/<name>")
// Entry id for a source: weapons / items use their id, the others get a prefix so ids never
// collide ("att_vertical_grip", "veh_car", "use_door", "style/rifle").
void handpose_entry_id(char *out, int out_size, int category, const char *name);
u32 handpose_entry_hash(int category, const char *name);

void handpose_db_init(HandPoseDB *db);
void handpose_db_free(HandPoseDB *db);
void handpose_db_clear(HandPoseDB *db);
int handpose_find(const HandPoseDB *db, const char *id);
int handpose_find_hash(const HandPoseDB *db, u32 hash);
// Adds (or returns) the entry with `id`. Pointers into db->e are invalidated by adds.
int handpose_add(HandPoseDB *db, const char *id, int category);
void handpose_link(HandPoseDB *db);          // resolves `inherits` -> parent (cycles cut)
void handpose_rehash(HandPoseDB *db);
bool handpose_entry_has_overrides(const HandPoseEntry *e);
void handpose_layer_clear(HandPoseLayer *l);
void handpose_entry_clear_layers(HandPoseEntry *e);

// Resolution of one entry's chain (entry < 0 or unknown -> open hand, no overrides).
void handpose_resolve(const HandPoseDB *db, int entry, int hand, int state, HandPose *out);
// The runtime form: the base chain (the hold style in use) first, then the entry's own chain on
// top (an item, an attachment grip, an interactable). A base already in the entry's chain is
// applied once. Either may be < 0.
void handpose_resolve_over(const HandPoseDB *db, int base, int entry, int hand, int state, HandPose *out);

// Joint limits (degrees) and clamping (applied on load, on every edit and when resolving).
void handpose_joint_limits(int finger, int joint, HandJoint *lo, HandJoint *hi);
HandJoint handpose_clamp_joint(int finger, int joint, HandJoint v);
void handpose_clamp_fingers(HandFingers j);
// Legacy 5-value curl (thumb..pinky degrees, the old curl tables) -> joints, exactly as
// proc_curl_fingers split it (proximal 30 %, middle 38 %, distal 32 %; thumb x 0.6).
void handpose_from_curl(const float curl[5], HandFingers out);

// Finger presets (applied per hand, then tweaked).
int handpose_preset_count(void);
const char *handpose_preset_name(int i);
bool handpose_preset(const char *name, HandFingers out);

// Mirror L <-> R: socket / wrist reflected across the entry's YZ plane (x -> -x, the palm frame
// kept right-handed), the pole hint's x negated; finger values are hand-relative and unchanged.
Xform handpose_mirror_xform(Xform x);
void handpose_mirror_layer(const HandPoseLayer *in, HandPoseLayer *out);
// Copies the left / right layers of every state of `e` onto the other hand, mirrored.
void handpose_mirror_entry_hand(HandPoseEntry *e, int from_hand);

// Applies one hand's finger joints (side 0 left, 1 right), scaled by weight. Fingers never move
// the wrist; the finger chains' model transforms are updated. Zero spread / twist cost nothing.
void handpose_apply_fingers(const Skeleton *s, Pose *pose, Xform *model, int side, const HandFingers j, const struct HandFrames *hf, float weight);
// Fingertip (model space) of finger f of the hand: the end of the distal joint.
vec3 handpose_fingertip(const Skeleton *s, const Xform *model, int side, int finger);

// Contact solver: closes finger f from open (curl 0, spread / twist kept; the curl split over the
// joints like the presets) until its middle / distal segments (spheres of `radius`) touch one of
// the boxes (model space OBBs) or the curl limit; the finger ends on the surface within `tol` (m).
// With a target point (non-NULL) and no contact it stops at the curl closest to the target.
// `pose` / `model`: the hand before its finger pose. blocked = the open finger is already inside
// the geometry (the socket is in the item): the joints are left unchanged.
typedef struct HandBox { vec3 center, half; quat rot; } HandBox;
typedef struct HandContactResult { bool touched, blocked; float tip_dist; float curl_add; int steps; } HandContactResult;
HandContactResult handpose_contact_solve(const Skeleton *s, const Pose *pose, const Xform *model, int side, int finger, HandFingers j,
                                         const struct HandFrames *hf, const HandBox *boxes, int box_count, const vec3 *target, float radius, float tol);
float handpose_box_signed_dist(const HandBox *b, vec3 p);   // < 0 inside

// JSON. Parse fills an entry (migrating older schemas); write produces the canonical text
// (stable key order, shortest round-trip floats, only set fields) - malloc'd, caller frees.
bool handpose_entry_parse(HandPoseEntry *e, const struct cJSON *root, char *err, int err_size);
char *handpose_entry_write(const HandPoseEntry *e);
int handpose_migrate(struct cJSON *root);   // in place, returns the schema it read (0 = unversioned)
// Files: every assets/handposes/*.json and styles/*.json (sorted, deterministic). Missing
// directory = empty DB. Returns the number of files read.
int handpose_db_load_dir(HandPoseDB *db, const char *dir_rel);
bool handpose_entry_load_file(HandPoseDB *db, const char *path_abs, char *err, int err_size);
bool handpose_entry_save_file(const HandPoseEntry *e, const char *path_abs);
// "<dir>/<id>.json" (styles: "<dir>/styles/<name>.json").
void handpose_entry_rel_path(const HandPoseEntry *e, const char *dir_rel, char *out, int out_size);

// Style defaults: every hold style ("style/<name>", from its curl_r / curl_l) and every grip
// style ("style/grip_<grip>", the first-person interaction grips: relaxed, fist, point, grab,
// pistol, rifle, melee, palm) that has no file of its own is created in memory from the old
// curl tables, so the data reproduces the previous behaviour. Entries loaded from a file are
// left alone. Call after loading the directory; relinks and rehashes.
struct ProcParams;
void handpose_add_style_defaults(HandPoseDB *db, const struct ProcParams *p);
int handpose_grip_count(void);
const char *handpose_grip_name(int grip);     // "relaxed", ... (GripType order, src/player/hands.h)
u32 handpose_grip_style_hash(int grip);       // "style/grip_<name>"

// Euler (pitch, yaw, roll degrees, the hold point convention) <-> quaternion, for the editor's fields.
vec3 handpose_quat_to_euler_deg(quat q);
quat handpose_euler_deg_to_quat(vec3 e);

#endif
