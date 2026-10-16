// Hand pose editing (docs/HANDPOSE.md "Editor"): the pure part of the editor scene - which layer
// supplies a field (override vs inherited), seeding / clearing a layer field, presets, mirror, the
// undo / redo stack. No renderer, no game state: the scene (hp_editor.c) and tests/test_hp_edit.c.
//
// A "field" is one layer bit (HPL_SOCKET, HPL_WRIST, HPL_POLE, HPL_IKW, HPL_FINGER(f),
// HPL_CONTACTS). Editing a field the layer doesn't set first seeds it with the value the layer's
// state resolves to (so turning the override on changes nothing visible), then applies the edit.
#ifndef DG_HP_EDIT_H
#define DG_HP_EDIT_H

#include "anim/handpose.h"

#define HP_UNDO_MAX 64

// Values a field takes when nothing in the chain sets it (the item's own grip socket, the hold
// style's elbow hint): seeding a socket / pole override starts from these.
typedef struct HpEditDefaults {
    Xform socket[2];
    vec3 pole[2];
} HpEditDefaults;

// The layer whose value wins for (entry, hand, state): entry index (in the entry's chain) and the
// state layer (HPS_DEFAULT or `state`). entry = -1: nothing sets it (the base value).
typedef struct HpFieldSource { int entry, state; } HpFieldSource;
HpFieldSource hp_field_source(const HandPoseDB *db, int entry, int hand, int state, u16 bit);
// True when the entry's own (hand, state) layer sets the field.
bool hp_field_overridden(const HandPoseDB *db, int entry, int hand, int state, u16 bit);
// True when a field set on the entry's default layer is hidden at `state` by the entry's own state
// layer (ancestors apply first, so only that one can): editing "default" then shows nothing there.
bool hp_field_shadowed(const HandPoseDB *db, int entry, int hand, int state, u16 bit);
// "style/rifle (hip)" / "base" into out.
void hp_field_source_text(const HandPoseDB *db, HpFieldSource s, char *out, int size);

// Sets `bit` on (entry, hand, state), seeded from the resolved value; returns the layer (NULL for
// a bad index). Already set: returned as is.
HandPoseLayer *hp_edit_touch(HandPoseDB *db, int entry, int hand, int state, u16 bit, const HpEditDefaults *def);
// Reset to inherited: clears the bit (contacts: the list too). False if it wasn't set.
bool hp_edit_reset(HandPoseDB *db, int entry, int hand, int state, u16 bit);
// One joint (clamped to the joint limits); the finger's other joints keep their resolved values.
void hp_edit_set_joint(HandPoseDB *db, int entry, int hand, int state, int finger, int joint, HandJoint v);
bool hp_edit_preset(HandPoseDB *db, int entry, int hand, int state, const char *preset);   // all five fingers
// Mirror every state of from_hand onto the other hand (handpose_mirror_entry_hand).
void hp_edit_mirror(HandPoseDB *db, int entry, int from_hand);
// Contacts: append (false when full) / remove one.
bool hp_edit_add_contact(HandPoseDB *db, int entry, int hand, int state, int finger, vec3 point);
void hp_edit_remove_contact(HandPoseDB *db, int entry, int hand, int state, int index);
// After any edit: rehash (net identity + revision bump, the runtime picks the change up at once).
void hp_edit_commit(HandPoseDB *db);

// Undo / redo of whole-entry snapshots (layers + reviewed), keyed by the entry hash so a reload
// that reorders the DB keeps working. hp_undo_push is called before an edit; consecutive pushes
// with the same non-zero key (one slider being dragged) coalesce into one step until hp_undo_break.
typedef struct HpUndoSnap {
    u32 hash;
    bool reviewed;
    HandPoseLayer layer[2][HPS_COUNT];
} HpUndoSnap;

typedef struct HpUndo {
    HpUndoSnap *undo, *redo;   // HP_UNDO_MAX each (allocated on the first push)
    int undo_count, redo_count;
    u32 last_key, last_hash;
} HpUndo;

void hp_undo_free(HpUndo *u);
void hp_undo_clear(HpUndo *u);
void hp_undo_push(HpUndo *u, const HandPoseDB *db, int entry, u32 key);
void hp_undo_break(HpUndo *u);
// Restore the last snapshot (the current state goes to the other stack). Returns the entry index
// it restored (-1: nothing to undo, or the entry is gone). The caller commits.
int hp_undo(HpUndo *u, HandPoseDB *db);
int hp_redo(HpUndo *u, HandPoseDB *db);

#endif
