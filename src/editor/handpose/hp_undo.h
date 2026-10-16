// Hand pose editor undo / redo (docs/HANDPOSE.md): unlimited in session, whole-entry snapshots
// keyed by the entry's id hash (indices move when orphans are removed). Pure.
#ifndef DG_HP_UNDO_H
#define DG_HP_UNDO_H

#include "anim/handpose.h"

typedef struct HpUndoStep { HandPoseEntry e; } HpUndoStep;
typedef struct HpUndoStack { HpUndoStep *v; int n, cap; } HpUndoStack;
typedef struct HpUndo { HpUndoStack undo, redo; } HpUndo;

void hpu_free(HpUndo *u);
// Records `before` (the entry's state before an edit); clears the redo stack.
void hpu_push(HpUndo *u, const HandPoseEntry *before);
// Restores the last recorded state into db (the current one goes to the other stack). Returns
// the restored entry's index, or -1 when there is nothing to undo / redo.
int hpu_undo(HpUndo *u, HandPoseDB *db);
int hpu_redo(HpUndo *u, HandPoseDB *db);
int hpu_undo_count(const HpUndo *u);
int hpu_redo_count(const HpUndo *u);

#endif
