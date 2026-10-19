// The player's inventory (docs/INVENTORY.md): a grid tree rooted at the body (the primary,
// secondary, holster and backpack slots; the worn backpack's grid holds everything else,
// gameplay/inventory/) plus the hands: a grid item taken out by the wheels or Hold. Gameplay code
// reads what a selection holds through inv_sel (an InvSlot: item, count, ammo, attachments) and
// adds / counts / removes items by definition; every UI change is an intent
// (gameplay/inventory/inv_intent.h).
#ifndef DG_INVENTORY_H
#define DG_INVENTORY_H

#include "core/base.h"
#include "items.h"
#include "inventory/inv_intent.h"

// Selections (UserCmd.slot, keys 1-3): the three weapon slots (= EQ_PRIMARY, EQ_SECONDARY,
// EQ_HOLSTER), then the item in the hands; the built-in phone's virtual slot is WPN_SLOT_PHONE =
// INV_SEL_COUNT.
#define INV_SEL_WEAPONS 3
#define INV_SEL_HANDS 3
#define INV_SEL_COUNT 4

typedef struct Inventory {
    InvTree t;
    u16 hands;               // the grid item in the hands (uid, 0 = none)
    InvSlot none;            // what an empty selection reads as (writes to it are harmless)
    u32 revision;
} Inventory;

void inv_clear(Inventory *inv);    // empty body (keeps nothing, not even the default bag)
void inv_init(Inventory *inv, const ItemDB *db);   // empty, wearing the default bag
InvCtx inv_ctx(Inventory *inv, const ItemDB *db, InvTree *loot);

// Pickup rule (stacks, a free weapon slot, a bigger backpack, the grid). Returns how many could NOT
// be added. att NULL = the weapon's default attachments.
int inv_add(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo);
int inv_add_att(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo, const u8 *att);
int inv_count(const Inventory *inv, u16 item);
int inv_remove(Inventory *inv, u16 item, int count);   // returns removed (stacks not in the hands first)
bool inv_has(const Inventory *inv, u16 item);
// The first armor plate carried (a consumable that only adds armor: docs/DAMAGE.md "Armor"), 0 none.
u16 inv_armor_plate(const Inventory *inv, const ItemDB *db);

// Selection k (0-based, INV_SEL_*): the payload of an equipped weapon or of the item in the hands,
// or an empty slot.
InvSlot *inv_sel(Inventory *inv, int k);
const InvSlot *inv_sel_c(const Inventory *inv, int k);
int inv_sel_find(const Inventory *inv, u16 item);      // the selection holding item (-1 none)
// One use of a consumable payload: a charge (INV_USE_CHARGES: ammo counts the charges used), or
// one of the stack; an emptied payload is cleared (its instance drops out of the tree).
void inv_use_one(const ItemDB *db, InvSlot *s);
// Dev tools (hold point / hand pose editors, sight tests): selection k's payload, writable; an empty
// weapon slot gets an instance put straight into it, empty hands an instance held outside the grid
// (loc INV_LOC_NONE). Writing an empty payload back removes it.
InvSlot *inv_dev_sel(Inventory *inv, const ItemDB *db, int k);
InvItem *inv_find(Inventory *inv, u16 item);           // the first instance of item (NULL none)
InvItem *inv_item_of(Inventory *inv, const InvSlot *s);   // the instance owning payload s (NULL: not in the tree)

#endif
