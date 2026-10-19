// Grid inventory rules (docs/INVENTORY.md "Rules"): fit, rotation, equipment slots, filters, where
// items may go (a world container's root grid or a body's worn backpack: nothing nests), the
// free-space search. Pure.
#ifndef DG_INV_RULES_H
#define DG_INV_RULES_H

#include "inv_tree.h"

typedef enum InvResult {
    IIR_OK, IIR_NOOP, IIR_INVALID, IIR_NO_ROOM, IIR_FILTER, IIR_NESTING, IIR_GONE, IIR_TOO_FAR, IIR_FULL, IIR_EQUIP,
    IIR_NOT_USABLE, IIR_INCOMPATIBLE, IIR_NO_SLOT, IIR_NOT_WEAPON, IIR_NOT_ROTATABLE, IIR_NOT_EMPTY, IIR_DEFAULT_PACK, IIR_COUNT
} InvResult;
const char *inv_result_text(int r);

// Where an item goes: the parent (0 = the root, or a body's worn backpack uid), the compartment (or
// INV_LOC_EQUIP + slot), the top-left cell and the rotation.
typedef struct InvPlace { u16 parent; u8 loc, x, y, rot; } InvPlace;

bool inv_equip_accepts(int slot, const ItemDef *d);
// May the item go into container def cd? Filters and the size limit.
int inv_accepts(const ItemDB *db, const InvContainerDef *cd, const InvSlot *s, int w, int h);

// Can it (payload + flags; its uid in t when moving inside t, else 0) be placed at p in t?
// ignore: an extra uid treated as absent (a swap partner). blocker: the single live item in the
// way (0 if none or several). Returns IIR_OK or why not.
int inv_check_place(const InvTree *t, const ItemDB *db, const InvItem *it, InvPlace p, u16 ignore, u16 *blocker);
// First free placement of it in parent's compartments (row-major, unrotated first, then rotated).
bool inv_find_space(const InvTree *t, const ItemDB *db, const InvItem *it, u16 parent, InvPlace *out);
// The same in the tree's grid (inv_grid).
bool inv_find_grid_space(const InvTree *t, const ItemDB *db, const InvItem *it, InvPlace *out);
// The equipment slot it would go into (EQ_NONE: not wearable): its own; a long gun takes the
// secondary slot when the primary is taken.
int inv_equip_target(const InvTree *t, const ItemDB *db, const InvItem *it);

#endif
