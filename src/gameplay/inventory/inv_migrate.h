// Story saves from before the inventory simplification (format 1, docs/INVENTORY.md "Saves"): item
// indices of the old items.json order, thirteen equipment slots, pockets and nested containers.
// Rebuilt into today's trees: the weapons, helmet, plate carrier and worn backpack in their slots, the
// pack's contents in their own cells when they fit, everything else into the grid. Pure.
#ifndef DG_INV_MIGRATE_H
#define DG_INV_MIGRATE_H

#include "inv_tree.h"

// One entry as format 1 saved it.
typedef struct InvV1Entry {
    int item, count, ammo, uid, parent, loc, x, y, flags, dur;
    u8 att[ATT_SLOTS];
} InvV1Entry;

// Today's item of a format-1 item index (0: deleted since).
u16 inv_v1_item(const ItemDB *db, int idx);
// Rebuilds t (root container def index root) from n format-1 entries; returns how many entries
// were dropped (deleted items, containers, no room).
int inv_migrate_v1(InvTree *t, const ItemDB *db, int root, const InvV1Entry *e, int n);

#endif
