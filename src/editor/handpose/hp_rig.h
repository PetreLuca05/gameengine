// Hand pose control rig (docs/HANDPOSE.md "Control rig"): the controls the editor selects and
// moves, their hierarchy, their world transforms and how a new world transform of a control is
// written back into a grip / case key. Pure (no world, no renderer): the editor fills an HpRigCtx
// from the live character each frame; tests fill it by hand.
//
//   Root (item root / case anchor)          translate / rotate / scale (scale: grips only, uniform)
//   |- IK_hand_L, IK_hand_R                 translate / rotate   (the whole arm follows by IK)
//   |  |- <side> <finger> 1..3              rotate (x = curl, y = spread, z = twist, free)
//   |- Pole_L, Pole_R                       translate            (elbow direction)
//
// The item mesh is not a control: it moves only through the root.
#ifndef DG_HP_RIG_H
#define DG_HP_RIG_H

#include "hp_edit.h"

typedef enum HpCtrlKind { HPK_ROOT, HPK_IK, HPK_POLE, HPK_FINGER } HpCtrlKind;
enum { HPA_T = 1, HPA_R = 2, HPA_S = 4 };   // allowed transform channels

#define HP_RIG_FINGER_CTRLS (2 * HAND_FINGERS * HAND_FINGER_JOINTS)
#define HP_RIG_CTRLS (5 + HP_RIG_FINGER_CTRLS)
#define HP_RIG_ROOT 0
#define HP_RIG_IK(h) (1 + (h))
#define HP_RIG_POLE(h) (3 + (h))
#define HP_RIG_FINGER(h, f, j) (5 + ((h) * HAND_FINGERS + (f)) * HAND_FINGER_JOINTS + (j))

typedef struct HpCtrl {
    u8 kind;                   // HpCtrlKind
    i8 hand, finger, joint;    // -1 when not applicable
    i16 parent;                // -1 = the root has no parent control (its parent frame is the hold frame)
    u8 allow;                  // HPA_* channels the control accepts
    char name[24];             // "Root", "IK_hand_L", "Pole_R", "L_index_2"
} HpCtrl;

const HpCtrl *hp_rig_ctrl(int i);   // 0 .. HP_RIG_CTRLS-1 (NULL out of range)
int hp_rig_find(const char *name);
int hp_rig_parent(int i);
int hp_rig_first_child(int i);       // -1 = none
int hp_rig_children(int i, int *out, int max);
int hp_rig_siblings(int i, int *out, int max);   // same parent, i excluded
int hp_rig_mirror(int i);            // the other hand's control (the root mirrors to itself)
bool hp_rig_is_ancestor(int a, int i);   // a is a strict ancestor of i
// Selection helpers (sel = HP_RIG_CTRLS bools).
void hp_rig_select_finger(bool *sel, int hand, int finger, bool on);   // a whole finger
void hp_rig_select_hand(bool *sel, int hand, bool on);                 // IK + every finger of the hand
// Drops controls whose ancestor is also selected (a parent carries them: no double transform).
int hp_rig_top_level(const bool *sel, int *out, int max);

// Where the rig sits this frame (world space), filled by the editor from the live character.
typedef struct HpRigCtx {
    Xform root_parent;         // the root's parent frame: the hold frame (grips) / the case's base target
    Xform grip_space[2];       // grips: a hand's IK target frame inside the item (an attachment's grip frame; identity)
    float grip_scale;          // grips: > 0 overrides the target's root scale (an attachment: the weapon's root)
    Xform hand_base[2];        // cases: the frame each hand's key IK target is relative to (before the anchor); grips: unused
    bool anchor_applied;       // cases: the anchor sits between hand_base and the key (false: a driver seat, the base has it)
    bool is_case;
    quat cam_rot;              // pole hints are camera space directions
    vec3 shoulder[2], elbow[2], wrist[2];   // the solved arms (display)
    Xform finger_bone[2][HAND_FINGERS][HAND_FINGER_JOINTS];   // finger joint frames (world; the bone's own rotation)
    vec3 finger_tip[2][HAND_FINGERS];
    Xform palm_frame[2];       // the palm frames (for the finger axes: curl axis, palm up)
    vec3 curl_axis[2];         // the curl axis in the palm frame (HandFrames)
    float thumb_side[2];       // HandFrames.thumb_side (the spread direction of the fingers)
    float pole_len;            // display distance of the pole control from the elbow (m)
} HpRigCtx;

// World transform of every control (out[HP_RIG_CTRLS]) for a target: the root (scale in .s), the
// IK controls on their resolved palm targets, the poles at elbow + direction, the finger bones.
// def: the item's unset sockets / the style's elbow hints (grips).
void hp_rig_world(const HandPoseDB *db, HpTarget t, const HpRigCtx *ctx, const HpEditDefaults *def, Xform *out);
// The local axes a finger joint rotates about (world): x = curl, y = spread (palm up), z = twist (bone).
void hp_rig_finger_axes(const HpRigCtx *ctx, int hand, int finger, int joint, vec3 axes[3]);

// Locks: bits 0-2 location x / y / z, 3-5 rotation x / y / z, 6 scale (in the control's local space).
enum { HPL_LOCK_LOC = 0x7, HPL_LOCK_ROT = 0x38, HPL_LOCK_SCALE = 0x40 };

// Writes a new world transform of control `ctrl` into the target (what the control allows; the
// rest of `world` is ignored), given the transform it had when the edit started (`start`, from
// hp_rig_world) and the data at that time already in the DB as the base. Fingers: the rotation
// from start to world is decomposed into curl / spread / twist about the joint's axes and added to
// the joint values `j0` the edit started from (free on every axis, kept within +-180). Returns
// false when the target / control can't take it. Locks keep the locked local components.
bool hp_rig_apply(HandPoseDB *db, HpTarget t, const HpRigCtx *ctx, const HpEditDefaults *def, int ctrl, Xform start, Xform world, const HandJoint *j0, u8 locks);

// Local values shown in the N-panel: root (pos, rot, scale), IK (item space / key space socket),
// pole (the direction), finger joint (curl, spread, twist as rotation x / y / z).
typedef struct HpLocal { vec3 loc, rot_deg; quat rot; float scale; } HpLocal;
HpLocal hp_rig_local(const HandPoseDB *db, HpTarget t, int ctrl, const HpEditDefaults *def);
// Sets the local values (the N-panel / typed values); clamps the root scale (finger joints are
// free, kept within +-180).
bool hp_rig_set_local(HandPoseDB *db, HpTarget t, int ctrl, const HpEditDefaults *def, HpLocal v);
// Back to the default (inherited / identity) for location, rotation or scale (Alt+G / R / S).
bool hp_rig_clear(HandPoseDB *db, HpTarget t, int ctrl, int channel);   // channel HPA_T / HPA_R / HPA_S

#endif
