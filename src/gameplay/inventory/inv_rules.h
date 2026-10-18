// Grid inventory rules (docs/INVENTORY.md "Rules"): fit, rotation, equipment slots, filters,
// nesting, free-space search, the quick-move order. Pure.
#ifndef DG_INV_RULES_H
#define DG_INV_RULES_H

#include "inv_tree.h"

typedef enum InvResult {
    IIR_OK, IIR_NOOP, IIR_INVALID, IIR_NO_ROOM, IIR_FILTER, IIR_NESTING, IIR_GONE, IIR_TOO_FAR, IIR_FULL, IIR_EQUIP,
    IIR_NOT_USABLE, IIR_INCOMPATIBLE, IIR_NO_SLOT, IIR_NOT_WEAPON, IIR_NOT_ROTATABLE, IIR_COUNT
} InvResult;
const char *inv_result_text(int r);

// Where an item goes: the parent container (0 = the root), the compartment (or INV_LOC_EQUIP +
// slot), the top-left cell and the rotation.
typedef struct InvPlace { u16 parent; u8 loc, x, y, rot; } InvPlace;

// The quick-move / pickup order of the player's containers (cvar inv_quick_order).
typedef enum InvQuickTarget { IQ_RIG, IQ_BACKPACK, IQ_POCKETS, IQ_SECURE, IQ_COUNT } InvQuickTarget;
typedef struct InvOrder { u8 n; u8 t[IQ_COUNT]; } InvOrder;
InvOrder inv_order_default(void);                            // rig, backpack, pockets
InvOrder inv_order_parse(const char *s);                     // "rig,backpack,pockets,secure"

bool inv_equip_accepts(int slot, const ItemDef *d);
// May item (a top of a subtree in src) go into container def cd? Filters, size limit, nesting.
int inv_accepts(const ItemDB *db, const InvContainerDef *cd, const InvSlot *s, int w, int h);

// Can it (payload + flags; its uid in t when moving inside t, else 0) be placed at p in t?
// ignore: an extra uid treated as absent (a swap partner). blocker: the single live item in the
// way (0 if none or several). Returns IIR_OK or why not.
int inv_check_place(const InvTree *t, const ItemDB *db, const InvItem *it, InvPlace p, u16 ignore, u16 *blocker);
// First free placement of it in parent's compartments (row-major, unrotated first, then rotated).
bool inv_find_space(const InvTree *t, const ItemDB *db, const InvItem *it, u16 parent, InvPlace *out);
// In the player's containers in the given order (rig, backpack, pockets, ...).
bool inv_find_space_order(const InvTree *t, const ItemDB *db, const InvItem *it, InvOrder order, InvPlace *out);
// The container uid (0 = the root pockets) behind a quick target; false if not equipped.
bool inv_quick_parent(const InvTree *t, InvQuickTarget q, u16 *parent);
// The equipment slot it would go into (EQ_NONE: not wearable): its own; a long gun takes the
// secondary slot when the primary is taken.
int inv_equip_target(const InvTree *t, const ItemDB *db, const InvItem *it);

#endif
