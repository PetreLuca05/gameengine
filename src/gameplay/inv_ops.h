// Inventory operations (docs/UI.md "Inventory intents"): the single implementation of every
// inventory change the UI can ask for. The host applies them authoritatively (RPC_INV_OP); the
// client applies the same function to its copy immediately (prediction) and re-applies its
// unacknowledged operations on top of every inventory the host sends (reconcile). Pure: no
// world, no network (tests/test_inventory_ui.c).
#ifndef DG_INV_OPS_H
#define DG_INV_OPS_H

#include "inventory.h"

typedef enum InvOpType {
    IOP_NONE,
    IOP_MOVE,         // a -> b, count (0 = all): merge (remainder stays) / swap / split into an empty slot
    IOP_SPLIT,        // half of a's stack (count > 0: that many) into b (255: the first empty slot)
    IOP_DROP,         // a, count (0 = all): removed (the host spawns the pickup)
    IOP_USE,          // a: consumable used (one)
    IOP_EQUIP,        // a: into the hotbar (b = the selected hotbar slot to swap with when it is full)
    IOP_QUICK_MOVE,   // a: hotbar <-> backpack (merge first, then the first empty slot)
    IOP_MOUNT,        // weapon slot a <- attachment item in slot b (a replaced attachment goes to b)
    IOP_UNMOUNT,      // weapon slot a, attachment slot b -> inventory slot c (255 = first empty)
    IOP_COUNT
} InvOpType;

typedef enum InvOpResult {
    IOR_OK, IOR_NOOP, IOR_INVALID, IOR_FULL, IOR_INCOMPATIBLE, IOR_NO_SLOT, IOR_NOT_USABLE, IOR_NOT_WEAPON, IOR_COUNT
} InvOpResult;

typedef struct InvOp {
    u8 type;          // InvOpType
    u8 a, b, c;
    u16 count;
    u32 seq;          // client sequence (reconcile); 0 = unsequenced
} InvOp;

// What the host must do in the world after a successful op (pickups, healing).
typedef struct InvOpEffect {
    InvSlot dropped;          // IOP_DROP: the removed stack
    u16 used_item;            // IOP_USE: the consumed item (heal / armour)
} InvOpEffect;

int inv_op_apply(Inventory *inv, const ItemDB *db, const InvOp *op, InvOpEffect *fx);
const char *inv_op_result_text(int result);
const char *inv_op_name(int type);

// Stack merge: moves up to count (0 = all) from src into dst (same item or empty dst); returns
// how many moved. The remainder stays in src.
int inv_merge(InvSlot *dst, InvSlot *src, const ItemDB *db, int count);
// Attachment compatibility of an attachment item for a weapon instance: IOR_OK and the slot it
// goes into, or why not (IOR_NOT_WEAPON / IOR_NO_SLOT / IOR_INCOMPATIBLE).
int inv_attach_check(const ItemDB *db, const InvSlot *weapon, u16 att_item, int *aslot_out);
int inv_first_empty(const Inventory *inv, int from, int to);   // [from, to), -1 none

// Wire encoding (RPC_INV_OP a / b / c ints).
void inv_op_pack(const InvOp *op, int *a, int *b, int *c);
void inv_op_unpack(int a, int b, int c, InvOp *op);

#endif
