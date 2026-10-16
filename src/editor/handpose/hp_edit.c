// Hand pose editing: field provenance, layer seeding, undo / redo (docs/HANDPOSE.md "Editor").
#include "hp_edit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool valid(const HandPoseDB *db, int entry, int hand, int state) {
    return db && entry >= 0 && entry < db->count && hand >= 0 && hand < 2 && state >= 0 && state < HPS_COUNT;
}

// Resolution applies root first (default, then state), so the winner is the first hit walking leaf
// first with the state layer before the default one.
HpFieldSource hp_field_source(const HandPoseDB *db, int entry, int hand, int state, u16 bit) {
    HpFieldSource r = { -1, -1 };
    if (!valid(db, entry, hand, state)) return r;
    int n = 0;
    for (int k = entry; k >= 0 && k < db->count && n < 8; k = db->e[k].parent, n++) {
        const HandPoseEntry *e = &db->e[k];
        if (state != HPS_DEFAULT && (e->layer[hand][state].set & bit)) { r.entry = k; r.state = state; return r; }
        if (e->layer[hand][HPS_DEFAULT].set & bit) { r.entry = k; r.state = HPS_DEFAULT; return r; }
    }
    return r;
}

bool hp_field_overridden(const HandPoseDB *db, int entry, int hand, int state, u16 bit) {
    return valid(db, entry, hand, state) && (db->e[entry].layer[hand][state].set & bit) != 0;
}

// The entry is the leaf of its chain: after its default layer only its own state layer applies.
bool hp_field_shadowed(const HandPoseDB *db, int entry, int hand, int state, u16 bit) {
    return valid(db, entry, hand, state) && state != HPS_DEFAULT && (db->e[entry].layer[hand][HPS_DEFAULT].set & bit) &&
           (db->e[entry].layer[hand][state].set & bit);
}

void hp_field_source_text(const HandPoseDB *db, HpFieldSource s, char *out, int size) {
    if (!db || s.entry < 0 || s.entry >= db->count) snprintf(out, (size_t)size, "base");
    else snprintf(out, (size_t)size, "%s (%s)", db->e[s.entry].id, handpose_state_name(s.state));
}

HandPoseLayer *hp_edit_touch(HandPoseDB *db, int entry, int hand, int state, u16 bit, const HpEditDefaults *def) {
    if (!valid(db, entry, hand, state)) return NULL;
    HandPoseLayer *l = &db->e[entry].layer[hand][state];
    u16 missing = (u16)(bit & ~l->set);
    if (!missing) return l;
    HandPose p;
    handpose_resolve(db, entry, hand, state, &p);
    if (missing & HPL_SOCKET) l->socket = p.has_socket ? p.socket : (def ? def->socket[hand] : xf_identity());
    if (missing & HPL_WRIST) l->wrist = p.wrist;
    if (missing & HPL_POLE) l->pole = p.has_pole ? p.pole : (def ? def->pole[hand] : v3(0, 0, 0));
    if (missing & HPL_IKW) l->ik_weight = p.ik_weight;
    for (int f = 0; f < HAND_FINGERS; f++)
        if (missing & HPL_FINGER(f)) memcpy(l->j[f], p.j[f], sizeof(l->j[f]));
    if (missing & HPL_CONTACTS) {
        l->contact_count = p.contact_count;
        memcpy(l->contacts, p.contacts, sizeof(l->contacts));
    }
    l->set |= missing;
    return l;
}

bool hp_edit_reset(HandPoseDB *db, int entry, int hand, int state, u16 bit) {
    if (!valid(db, entry, hand, state)) return false;
    HandPoseLayer *l = &db->e[entry].layer[hand][state];
    if (!(l->set & bit)) return false;
    l->set = (u16)(l->set & ~bit);
    // Cleared values go back to the layer defaults: the canonical writer skips them anyway, but a
    // later seed must not see stale data.
    HandPoseLayer z;
    handpose_layer_clear(&z);
    if (bit & HPL_SOCKET) l->socket = z.socket;
    if (bit & HPL_WRIST) l->wrist = z.wrist;
    if (bit & HPL_POLE) l->pole = z.pole;
    if (bit & HPL_IKW) l->ik_weight = z.ik_weight;
    for (int f = 0; f < HAND_FINGERS; f++)
        if (bit & HPL_FINGER(f)) memset(l->j[f], 0, sizeof(l->j[f]));
    if (bit & HPL_CONTACTS) { l->contact_count = 0; memset(l->contacts, 0, sizeof(l->contacts)); }
    return true;
}

void hp_edit_set_joint(HandPoseDB *db, int entry, int hand, int state, int finger, int joint, HandJoint v) {
    if (finger < 0 || finger >= HAND_FINGERS || joint < 0 || joint >= HAND_FINGER_JOINTS) return;
    HandPoseLayer *l = hp_edit_touch(db, entry, hand, state, (u16)HPL_FINGER(finger), NULL);
    if (l) l->j[finger][joint] = handpose_clamp_joint(finger, joint, v);
}

bool hp_edit_preset(HandPoseDB *db, int entry, int hand, int state, const char *preset) {
    HandFingers j;
    if (!preset || !handpose_preset(preset, j)) return false;
    HandPoseLayer *l = hp_edit_touch(db, entry, hand, state, (u16)HPL_FINGERS_ALL, NULL);
    if (!l) return false;
    memcpy(l->j, j, sizeof(l->j));
    return true;
}

void hp_edit_mirror(HandPoseDB *db, int entry, int from_hand) {
    if (!valid(db, entry, from_hand, HPS_DEFAULT)) return;
    handpose_mirror_entry_hand(&db->e[entry], from_hand);
}

bool hp_edit_add_contact(HandPoseDB *db, int entry, int hand, int state, int finger, vec3 point) {
    HandPoseLayer *l = hp_edit_touch(db, entry, hand, state, HPL_CONTACTS, NULL);
    if (!l || l->contact_count >= HANDPOSE_CONTACTS || finger < 0 || finger >= HAND_FINGERS) return false;
    l->contacts[l->contact_count].finger = (u8)finger;
    l->contacts[l->contact_count].point = point;
    l->contact_count++;
    return true;
}

void hp_edit_remove_contact(HandPoseDB *db, int entry, int hand, int state, int index) {
    HandPoseLayer *l = hp_edit_touch(db, entry, hand, state, HPL_CONTACTS, NULL);
    if (!l || index < 0 || index >= l->contact_count) return;
    memmove(&l->contacts[index], &l->contacts[index + 1], sizeof(HandContact) * (size_t)(l->contact_count - index - 1));
    l->contact_count--;
    memset(&l->contacts[l->contact_count], 0, sizeof(HandContact));
}

void hp_edit_commit(HandPoseDB *db) {
    if (db) handpose_rehash(db);
}

// ------------------------------------------------------------------ undo / redo

void hp_undo_free(HpUndo *u) {
    free(u->undo);
    free(u->redo);
    memset(u, 0, sizeof(*u));
}
void hp_undo_clear(HpUndo *u) { u->undo_count = u->redo_count = 0; u->last_key = u->last_hash = 0; }
void hp_undo_break(HpUndo *u) { u->last_key = 0; }

static void snap(HpUndoSnap *s, const HandPoseEntry *e) {
    s->hash = e->hash;
    s->reviewed = e->reviewed;
    memcpy(s->layer, e->layer, sizeof(s->layer));
}

// Push onto a bounded stack (the oldest step drops off).
static void stack_push(HpUndoSnap *v, int *count, const HandPoseEntry *e) {
    if (*count >= HP_UNDO_MAX) {
        memmove(v, v + 1, sizeof(HpUndoSnap) * (HP_UNDO_MAX - 1));
        *count = HP_UNDO_MAX - 1;
    }
    snap(&v[(*count)++], e);
}

static bool ensure(HpUndo *u) {
    if (!u->undo) u->undo = (HpUndoSnap *)malloc(sizeof(HpUndoSnap) * HP_UNDO_MAX);
    if (!u->redo) u->redo = (HpUndoSnap *)malloc(sizeof(HpUndoSnap) * HP_UNDO_MAX);
    return u->undo && u->redo;
}

void hp_undo_push(HpUndo *u, const HandPoseDB *db, int entry, u32 key) {
    if (!db || entry < 0 || entry >= db->count || !ensure(u)) return;
    const HandPoseEntry *e = &db->e[entry];
    if (key && key == u->last_key && e->hash == u->last_hash) return;   // same drag
    stack_push(u->undo, &u->undo_count, e);
    u->redo_count = 0;
    u->last_key = key;
    u->last_hash = e->hash;
}

static int swap_step(HpUndo *u, HandPoseDB *db, HpUndoSnap *from, int *from_count, HpUndoSnap *to, int *to_count) {
    u->last_key = 0;
    while (*from_count > 0) {
        HpUndoSnap *s = &from[--*from_count];
        int i = handpose_find_hash(db, s->hash);
        if (i < 0) continue;   // the entry is gone (reload): skip the step
        HandPoseEntry *e = &db->e[i];
        stack_push(to, to_count, e);
        memcpy(e->layer, s->layer, sizeof(e->layer));
        e->reviewed = s->reviewed;
        return i;
    }
    return -1;
}

int hp_undo(HpUndo *u, HandPoseDB *db) { return db && u->undo ? swap_step(u, db, u->undo, &u->undo_count, u->redo, &u->redo_count) : -1; }
int hp_redo(HpUndo *u, HandPoseDB *db) { return db && u->redo ? swap_step(u, db, u->redo, &u->redo_count, u->undo, &u->undo_count) : -1; }
