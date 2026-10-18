// The player's inventory (docs/INVENTORY.md): a grid tree rooted at the body (equipment slots +
// pockets, gameplay/inventory/) plus the hotbar, six binds to item instances. Gameplay code reads
// the hotbar through inv_hot (an InvSlot: item, count, ammo, attachments) and adds / counts /
// removes items by definition; every UI change is an intent (gameplay/inventory/inv_intent.h).
#ifndef DG_INVENTORY_H
#define DG_INVENTORY_H

#include "core/base.h"
#include "items.h"
#include "inventory/inv_intent.h"

#define INV_HOTBAR 6   // keys 1-6, `slot 1-6`; the built-in phone's virtual slot is WPN_SLOT_PHONE = INV_HOTBAR

typedef struct Inventory {
    InvTree t;
    u16 quick[INV_HOTBAR];   // hotbar slots 1-6: item uids (0 = empty)
    InvSlot none;            // what an empty hotbar slot reads as (writes to it are harmless)
    u32 revision;
} Inventory;

void inv_clear(Inventory *inv);    // empty body (keeps nothing)
void inv_init(Inventory *inv, const ItemDB *db);
// The quick-move / pickup order (cvar inv_quick_order, set by the game).
void inv_set_order(InvOrder order);
InvOrder inv_get_order(void);
InvCtx inv_ctx(Inventory *inv, const ItemDB *db, InvTree *loot);

// Pickup rule (stacks, free equipment slot, containers in order, hotbar bind). Returns how many
// could NOT be added. att NULL = the weapon's default attachments.
int inv_add(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo);
int inv_add_att(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo, const u8 *att);
int inv_count(const Inventory *inv, u16 item);
int inv_remove(Inventory *inv, u16 item, int count);   // returns removed (unbound stacks first)
bool inv_has(const Inventory *inv, u16 item);

// Hotbar slot k (0-based): the bound instance's payload, or an empty slot.
InvSlot *inv_hot(Inventory *inv, int k);
const InvSlot *inv_hot_c(const Inventory *inv, int k);
int inv_hot_find(const Inventory *inv, u16 item);      // the hotbar slot holding item (-1 none)
// One use of a consumable payload: a charge (INV_USE_CHARGES: ammo counts the charges used), or
// one of the stack; an emptied payload is cleared (its instance drops out of the tree).
void inv_use_one(const ItemDB *db, InvSlot *s);
// Dev tools (hold point / hand pose editors, sight tests): hotbar slot k's payload, writable; an
// empty slot gets an instance held outside the grids (loc INV_LOC_NONE) bound to it. Writing an
// empty payload back removes it.
InvSlot *inv_dev_hot(Inventory *inv, const ItemDB *db, int k);
InvItem *inv_find(Inventory *inv, u16 item);           // the first instance of item (NULL none)
InvItem *inv_item_of(Inventory *inv, const InvSlot *s);   // the instance owning payload s (NULL: not in the tree)

#endif
