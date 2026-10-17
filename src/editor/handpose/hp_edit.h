// Hand pose editing (docs/HANDPOSE.md "Editor"): the pure part of the editor scene - which layer
// supplies a field (override vs inherited), seeding / clearing a field, presets, mirror, the undo /
// redo stack. No renderer, no game state: the scene (hp_editor.c), the rig (hp_rig.c) and tests.
//
// An edit target is a grip (an entry: its root + one layer per hand) or a case key (one layer per
// hand, relative to the case anchor; key < 0 = the case's anchor / markers only). A "field" is one
// layer bit (HPL_SOCKET = the IK target, HPL_POLE, HPL_IKW, HPL_FINGER(f), HPL_CONTACTS). Editing a
// field the layer doesn't set first seeds it with the value it resolves to (so turning the
// override on changes nothing visible), then applies the edit.
#ifndef DG_HP_EDIT_H
#define DG_HP_EDIT_H

#include "anim/handpose.h"

#define HP_UNDO_MAX 64

typedef struct HpTarget {
    int entry;                 // grip entry (-1 = a case)
    int ci, key;               // case + key (key -1: the anchor / markers)
} HpTarget;
DG_INLINE HpTarget hp_target_grip(int entry) { HpTarget t = { entry, -1, -1 }; return t; }
DG_INLINE HpTarget hp_target_case(int ci, int key) { HpTarget t = { -1, ci, key }; return t; }
bool hp_target_valid(const HandPoseDB *db, HpTarget t);
bool hp_target_has_layers(const HandPoseDB *db, HpTarget t);   // a grip, or a case with a key selected
// The layer edited for `hand` (NULL when the target has none).
HandPoseLayer *hp_target_layer(HandPoseDB *db, HpTarget t, int hand);
// The target's root: an entry's root (identity + scale 1 when unset) / a case's anchor.
Xform hp_target_root(const HandPoseDB *db, HpTarget t);
void hp_target_set_root(HandPoseDB *db, HpTarget t, Xform root);   // clamped (scale); grips: has_root set
// Resolved hand pose of the target in its own space (grips: item space; case keys: relative to the
// anchor - the socket the key would store).
void hp_target_resolve(const HandPoseDB *db, HpTarget t, int hand, HandPose *out);

// Values a field takes when nothing in the chain sets it (the item's own grip socket, the hold
// style's elbow hint): seeding a socket / pole override starts from these.
typedef struct HpEditDefaults {
    Xform socket[2];
    vec3 pole[2];
} HpEditDefaults;

// The layer whose value wins for a grip's (entry, hand): an entry index in the chain.
// entry = -1: nothing sets it (the base value).
typedef struct HpFieldSource { int entry; } HpFieldSource;
HpFieldSource hp_field_source(const HandPoseDB *db, int entry, int hand, u16 bit);
bool hp_field_overridden(const HandPoseDB *db, HpTarget t, int hand, u16 bit);
void hp_field_source_text(const HandPoseDB *db, HpFieldSource s, char *out, int size);   // "style/rifle" / "base"

// Sets `bit` on the target's hand layer, seeded from the resolved value; returns the layer (NULL
// for a target without layers). Already set: returned as is.
HandPoseLayer *hp_edit_touch(HandPoseDB *db, HpTarget t, int hand, u16 bit, const HpEditDefaults *def);
bool hp_edit_reset(HandPoseDB *db, HpTarget t, int hand, u16 bit);   // back to inherited
bool hp_edit_reset_root(HandPoseDB *db, HpTarget t);                 // identity root / anchor
// One joint (clamped to the joint limits); the finger's other joints keep their resolved values.
void hp_edit_set_joint(HandPoseDB *db, HpTarget t, int hand, int finger, int joint, HandJoint v);
void hp_edit_set_curl(HandPoseDB *db, HpTarget t, int hand, int finger, float curl);   // the three joints together
bool hp_edit_preset(HandPoseDB *db, HpTarget t, int hand, const char *preset);         // all five fingers
// Mirror from_hand onto the other hand (the grip, or every key of a case).
void hp_edit_mirror(HandPoseDB *db, HpTarget t, int from_hand);
// Contacts: append (false when full) / remove one.
bool hp_edit_add_contact(HandPoseDB *db, HpTarget t, int hand, int finger, vec3 point);
void hp_edit_remove_contact(HandPoseDB *db, HpTarget t, int hand, int index);
// After any edit: rehash (net identity + revision bump, the runtime picks the change up at once).
void hp_edit_commit(HandPoseDB *db);

// Undo / redo of whole-target snapshots (a grip entry: layers, root, reviewed; a case: everything),
// keyed by hash so a reload that reorders the DB keeps working. hp_undo_push is called before an
// edit; consecutive pushes with the same non-zero key (one control being dragged) coalesce into
// one step until hp_undo_break.
typedef struct HpUndoSnap {
    bool is_case;
    u32 hash;
    HandPoseEntry e;
    HandPoseCase c;
} HpUndoSnap;

typedef struct HpUndo {
    HpUndoSnap *undo, *redo;   // HP_UNDO_MAX each (allocated on the first push)
    int undo_count, redo_count;
    u32 last_key, last_hash;
} HpUndo;

void hp_undo_free(HpUndo *u);
void hp_undo_clear(HpUndo *u);
void hp_undo_push(HpUndo *u, const HandPoseDB *db, HpTarget t, u32 key);
void hp_undo_break(HpUndo *u);
void hp_undo_pop(HpUndo *u);   // drops the last pushed step (a cancelled transform)
// Restore the last snapshot (the current state goes to the other stack). Returns the target it
// restored (entry / ci -1: nothing to undo, or it is gone). The caller commits.
HpTarget hp_undo(HpUndo *u, HandPoseDB *db);
HpTarget hp_redo(HpUndo *u, HandPoseDB *db);

#endif
