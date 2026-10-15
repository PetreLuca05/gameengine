// Slot-based inventory: 9 hotbar slots + a 6x4 backpack grid.
#ifndef DG_INVENTORY_H
#define DG_INVENTORY_H

#include "core/base.h"
#include "items.h"

#define INV_HOTBAR 9
#define INV_BACKPACK_W 6
#define INV_BACKPACK_H 4
#define INV_BACKPACK (INV_BACKPACK_W * INV_BACKPACK_H)
#define INV_SLOTS (INV_HOTBAR + INV_BACKPACK)
#define CONTAINER_SLOTS 12

typedef struct InvSlot {
    u16 item;
    u16 count;
    i16 ammo;     // rounds loaded (weapons)
    u8 att[ATT_SLOTS];   // attachment ids per slot (weapons; 0 = none, gameplay/attachments.h)
    u8 pad;
} InvSlot;

typedef struct Inventory {
    InvSlot slots[INV_SLOTS];
    u32 revision;
} Inventory;

// Adds up to count, filling existing stacks first. Weapons prefer hotbar slots.
// Returns how many could NOT be added.
int inv_add(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo);
int inv_count(const Inventory *inv, u16 item);
int inv_remove(Inventory *inv, u16 item, int count);   // returns removed
bool inv_has(const Inventory *inv, u16 item);
void inv_move(Inventory *inv, const ItemDB *db, int from, int to); // swap or merge
void inv_clear(Inventory *inv);

// Generic slot array helpers (containers use these too).
int slots_add(InvSlot *slots, int n, const ItemDB *db, u16 item, int count, i16 ammo, int prefer_first, int prefer_count);
// With attachments (att NULL = the weapon's default attachments).
int slots_add_att(InvSlot *slots, int n, const ItemDB *db, u16 item, int count, i16 ammo, const u8 *att, int prefer_first, int prefer_count);
int inv_add_att(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo, const u8 *att);

#endif
