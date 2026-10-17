// Hand pose editing: targets, field provenance, layer seeding, undo / redo (docs/HANDPOSE.md "Editor").
#include "hp_edit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool hp_target_valid(const HandPoseDB *db, HpTarget t) {
    if (!db) return false;
    if (t.entry >= 0) return t.entry < db->count;
    return t.ci >= 0 && t.ci < db->case_count && t.key < db->c[t.ci].key_count;
}

bool hp_target_has_layers(const HandPoseDB *db, HpTarget t) { return hp_target_valid(db, t) && (t.entry >= 0 || t.key >= 0); }

HandPoseLayer *hp_target_layer(HandPoseDB *db, HpTarget t, int hand) {
    if (!hp_target_has_layers(db, t) || hand < 0 || hand > 1) return NULL;
    return t.entry >= 0 ? &db->e[t.entry].grip[hand] : &db->c[t.ci].keys[t.key].hand[hand];
}

Xform hp_target_root(const HandPoseDB *db, HpTarget t) {
    if (!hp_target_valid(db, t)) return xf_identity();
    return t.entry >= 0 ? handpose_root(db, t.entry) : db->c[t.ci].anchor;
}

void hp_target_set_root(HandPoseDB *db, HpTarget t, Xform root) {
    if (!hp_target_valid(db, t)) return;
    if (t.entry >= 0) {
        db->e[t.entry].root = handpose_root_clamp(root);
        db->e[t.entry].has_root = true;
    } else {
        root.r = quat_norm(root.r);
        root.s = v3(1, 1, 1);   // anchors don't scale
        db->c[t.ci].anchor = root;
    }
}

void hp_target_resolve(const HandPoseDB *db, HpTarget t, int hand, HandPose *out) {
    if (t.entry >= 0) { handpose_resolve(db, t.entry, hand, out); return; }
    handpose_case_key_pose(db, t.ci, -1, t.key, hand, out);
    if (hp_target_valid(db, t)) {
        out->socket = xf_mul(xf_inverse(handpose_case_anchor_applied(&db->c[t.ci])), out->socket);
        out->socket.r = quat_norm(out->socket.r);
        out->socket.s = v3(1, 1, 1);
    }
}

// Resolution applies root first, so the winner is the first hit walking leaf first.
HpFieldSource hp_field_source(const HandPoseDB *db, int entry, int hand, u16 bit) {
    HpFieldSource r = { -1 };
    if (!db || entry < 0 || entry >= db->count || hand < 0 || hand > 1) return r;
    int n = 0;
    for (int k = entry; k >= 0 && k < db->count && n < 8; k = db->e[k].parent, n++)
        if (db->e[k].grip[hand].set & bit) { r.entry = k; return r; }
    return r;
}

bool hp_field_overridden(const HandPoseDB *db, HpTarget t, int hand, u16 bit) {
    HandPoseLayer *l = hp_target_layer((HandPoseDB *)db, t, hand);
    return l && (l->set & bit) != 0;
}

void hp_field_source_text(const HandPoseDB *db, HpFieldSource s, char *out, int size) {
    if (!db || s.entry < 0 || s.entry >= db->count) snprintf(out, (size_t)size, "base");
    else snprintf(out, (size_t)size, "%s", db->e[s.entry].id);
}

HandPoseLayer *hp_edit_touch(HandPoseDB *db, HpTarget t, int hand, u16 bit, const HpEditDefaults *def) {
    HandPoseLayer *l = hp_target_layer(db, t, hand);
    if (!l) return NULL;
    u16 missing = (u16)(bit & ~l->set);
    if (!missing) return l;
    HandPose p;
    hp_target_resolve(db, t, hand, &p);
    // Case keys always resolve a socket (the anchor itself); grips fall back to the item's socket.
    bool has_socket = t.entry < 0 || p.has_socket;
    if (missing & HPL_SOCKET) l->socket = has_socket ? p.socket : (def ? def->socket[hand] : xf_identity());
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

bool hp_edit_reset(HandPoseDB *db, HpTarget t, int hand, u16 bit) {
    HandPoseLayer *l = hp_target_layer(db, t, hand);
    if (!l || !(l->set & bit)) return false;
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

bool hp_edit_reset_root(HandPoseDB *db, HpTarget t) {
    if (!hp_target_valid(db, t)) return false;
    if (t.entry >= 0) {
        bool was = db->e[t.entry].has_root;
        db->e[t.entry].has_root = false;
        db->e[t.entry].root = xf_identity();
        return was;
    }
    db->c[t.ci].anchor = xf_identity();
    return true;
}

void hp_edit_set_joint(HandPoseDB *db, HpTarget t, int hand, int finger, int joint, HandJoint v) {
    if (finger < 0 || finger >= HAND_FINGERS || joint < 0 || joint >= HAND_FINGER_JOINTS) return;
    HandPoseLayer *l = hp_edit_touch(db, t, hand, (u16)HPL_FINGER(finger), NULL);
    if (l) l->j[finger][joint] = handpose_clamp_joint(finger, joint, v);
}

void hp_edit_set_curl(HandPoseDB *db, HpTarget t, int hand, int finger, float curl) {
    if (finger < 0 || finger >= HAND_FINGERS) return;
    HandPoseLayer *l = hp_edit_touch(db, t, hand, (u16)HPL_FINGER(finger), NULL);
    if (l) handpose_set_finger_curl(l->j, finger, curl);
}

bool hp_edit_preset(HandPoseDB *db, HpTarget t, int hand, const char *preset) {
    HandFingers j;
    if (!preset || !handpose_preset(preset, j)) return false;
    HandPoseLayer *l = hp_edit_touch(db, t, hand, (u16)HPL_FINGERS_ALL, NULL);
    if (!l) return false;
    memcpy(l->j, j, sizeof(l->j));
    return true;
}

void hp_edit_mirror(HandPoseDB *db, HpTarget t, int from_hand) {
    if (!hp_target_valid(db, t)) return;
    if (t.entry >= 0) handpose_mirror_entry_hand(&db->e[t.entry], from_hand);
    else handpose_mirror_case_hand(&db->c[t.ci], from_hand);
}

bool hp_edit_add_contact(HandPoseDB *db, HpTarget t, int hand, int finger, vec3 point) {
    HandPoseLayer *l = hp_edit_touch(db, t, hand, HPL_CONTACTS, NULL);
    if (!l || l->contact_count >= HANDPOSE_CONTACTS || finger < 0 || finger >= HAND_FINGERS) return false;
    l->contacts[l->contact_count].finger = (u8)finger;
    l->contacts[l->contact_count].point = point;
    l->contact_count++;
    return true;
}

void hp_edit_remove_contact(HandPoseDB *db, HpTarget t, int hand, int index) {
    HandPoseLayer *l = hp_edit_touch(db, t, hand, HPL_CONTACTS, NULL);
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

static u32 target_hash(const HandPoseDB *db, HpTarget t) {
    if (t.entry >= 0 && t.entry < db->count) return db->e[t.entry].hash;
    if (t.ci >= 0 && t.ci < db->case_count) return db->c[t.ci].hash;
    return 0;
}

static bool snap(HpUndoSnap *s, const HandPoseDB *db, bool is_case, u32 hash) {
    s->is_case = is_case;
    s->hash = hash;
    if (is_case) {
        int i = handpose_case_find_hash(db, hash);
        if (i < 0) return false;
        s->c = db->c[i];
    } else {
        int i = handpose_find_hash(db, hash);
        if (i < 0) return false;
        s->e = db->e[i];
    }
    return true;
}

// Push onto a bounded stack (the oldest step drops off).
static void stack_push(HpUndoSnap *v, int *count, const HandPoseDB *db, bool is_case, u32 hash) {
    if (*count >= HP_UNDO_MAX) {
        memmove(v, v + 1, sizeof(HpUndoSnap) * (HP_UNDO_MAX - 1));
        *count = HP_UNDO_MAX - 1;
    }
    if (snap(&v[*count], db, is_case, hash)) (*count)++;
}

static bool ensure(HpUndo *u) {
    if (!u->undo) u->undo = (HpUndoSnap *)malloc(sizeof(HpUndoSnap) * HP_UNDO_MAX);
    if (!u->redo) u->redo = (HpUndoSnap *)malloc(sizeof(HpUndoSnap) * HP_UNDO_MAX);
    return u->undo && u->redo;
}

void hp_undo_push(HpUndo *u, const HandPoseDB *db, HpTarget t, u32 key) {
    if (!db || !hp_target_valid(db, t) || !ensure(u)) return;
    bool is_case = t.entry < 0;
    u32 h = target_hash(db, t);
    if (key && key == u->last_key && h == u->last_hash) return;   // same drag
    stack_push(u->undo, &u->undo_count, db, is_case, h);
    u->redo_count = 0;
    u->last_key = key;
    u->last_hash = h;
}

static HpTarget swap_step(HpUndo *u, HandPoseDB *db, HpUndoSnap *from, int *from_count, HpUndoSnap *to, int *to_count) {
    u->last_key = 0;
    HpTarget none = { -1, -1, -1 };
    while (*from_count > 0) {
        HpUndoSnap *s = &from[--*from_count];
        if (s->is_case) {
            int i = handpose_case_find_hash(db, s->hash);
            if (i < 0) continue;   // the case is gone (reload): skip the step
            stack_push(to, to_count, db, true, s->hash);
            bool on_disk = db->c[i].on_disk, seen = db->c[i].seen, orphan = db->c[i].orphan;
            i16 parent = db->c[i].parent;
            db->c[i] = s->c;
            db->c[i].on_disk = on_disk;
            db->c[i].seen = seen;
            db->c[i].orphan = orphan;
            db->c[i].parent = parent;
            return hp_target_case(i, -1);
        }
        int i = handpose_find_hash(db, s->hash);
        if (i < 0) continue;
        stack_push(to, to_count, db, false, s->hash);
        HandPoseEntry *e = &db->e[i];
        memcpy(e->grip, s->e.grip, sizeof(e->grip));
        e->has_root = s->e.has_root;
        e->root = s->e.root;
        e->reviewed = s->e.reviewed;
        return hp_target_grip(i);
    }
    return none;
}

HpTarget hp_undo(HpUndo *u, HandPoseDB *db) {
    HpTarget none = { -1, -1, -1 };
    return db && u->undo ? swap_step(u, db, u->undo, &u->undo_count, u->redo, &u->redo_count) : none;
}
HpTarget hp_redo(HpUndo *u, HandPoseDB *db) {
    HpTarget none = { -1, -1, -1 };
    return db && u->redo ? swap_step(u, db, u->redo, &u->redo_count, u->undo, &u->undo_count) : none;
}
