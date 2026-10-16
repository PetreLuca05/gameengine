// Hand pose editor undo / redo (docs/HANDPOSE.md).
#include "hp_undo.h"
#include <stdlib.h>

static void stack_push(HpUndoStack *s, const HandPoseEntry *e) {
    if (s->n >= s->cap) {
        int cap = s->cap ? s->cap * 2 : 32;
        HpUndoStep *n = (HpUndoStep *)realloc(s->v, sizeof(HpUndoStep) * (size_t)cap);
        if (!n) return;
        s->v = n;
        s->cap = cap;
    }
    s->v[s->n++].e = *e;
}

void hpu_free(HpUndo *u) {
    free(u->undo.v);
    free(u->redo.v);
    memset(u, 0, sizeof(*u));
}

void hpu_push(HpUndo *u, const HandPoseEntry *before) {
    stack_push(&u->undo, before);
    u->redo.n = 0;
}

// Pops `from` into the DB (its entry by id hash), pushing the current state onto `to`.
static int swap(HpUndoStack *from, HpUndoStack *to, HandPoseDB *db) {
    while (from->n > 0) {
        HandPoseEntry *snap = &from->v[--from->n].e;
        int i = handpose_find_hash(db, snap->hash);
        if (i < 0) continue;   // the entry was removed since: skip its steps
        stack_push(to, &db->e[i]);
        i16 parent = db->e[i].parent;
        bool on_disk = db->e[i].on_disk;
        db->e[i] = *snap;
        db->e[i].parent = parent;      // links / file state belong to the DB, not the snapshot
        db->e[i].on_disk = on_disk;
        handpose_link(db);
        handpose_rehash(db);
        return i;
    }
    return -1;
}

int hpu_undo(HpUndo *u, HandPoseDB *db) { return swap(&u->undo, &u->redo, db); }
int hpu_redo(HpUndo *u, HandPoseDB *db) { return swap(&u->redo, &u->undo, db); }
int hpu_undo_count(const HpUndo *u) { return u->undo.n; }
int hpu_redo_count(const HpUndo *u) { return u->redo.n; }
