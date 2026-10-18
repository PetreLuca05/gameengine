// Grid inventory intents (docs/INVENTORY.md "Intents"): the single implementation of every change
// the UI (or a script) can ask for. The host applies them authoritatively; a client applies the
// same function to its copies as a prediction and re-applies its unacknowledged intents on top of
// every tree the host sends. An intent works on up to two trees: the player's (INV_SIDE_SELF) and
// the open world container's (INV_SIDE_LOOT). It is all-or-nothing: applied to scratch copies,
// committed only when it succeeds. Pure.
#ifndef DG_INV_INTENT_H
#define DG_INV_INTENT_H

#include "inv_rules.h"

typedef enum InvSide { INV_SIDE_SELF, INV_SIDE_LOOT, INV_SIDES } InvSide;

typedef enum InvIntentType {
    II_NONE,
    II_MOVE,         // src -> (dst side, dst parent, loc, x, y, rot), count (0 = all): place / merge / insert / swap
    II_INSERT,       // src -> into container item dst (first free space)
    II_ROTATE,       // src rotated in place
    II_EQUIP,        // src -> equipment slot aux (255 = its own); the occupant swaps out
    II_QUICK_MOVE,   // src -> across (loot open) or the next player container (inv_quick_order)
    II_SPLIT,        // count (0 = half) of src -> dst place (dst INV_AUTO: the first space)
    II_DISCARD,      // src, count (0 = all): removed, the host spawns the pickup (a container with its contents)
    II_USE,          // src (self): a consumable used once
    II_MOUNT,        // attachment src -> weapon dst (on dst side)
    II_UNMOUNT,      // weapon src, attachment slot aux -> dst place (dst INV_AUTO: the first space)
    II_FOLD,         // weapon src: fold / unfold the stock
    II_UNLOAD,       // weapon src: the loaded rounds back into an ammo stack
    II_BIND,         // hotbar slot aux <- src (0 = clear)
    II_TAKE_ALL,     // every loot root item that fits -> self
    II_SORT,         // re-pack container dst (dst side; 0 = the root grids) by aux: 0 type, 1 weight, 2 size
    II_COUNT
} InvIntentType;

#define INV_AUTO 0xFFFF   // dst: find a place

typedef struct InvIntent {
    u8 type;          // InvIntentType
    u8 src_side, dst_side;
    u8 loc;           // destination compartment, or INV_LOC_EQUIP + slot
    u16 src;          // item uid in the src side's tree
    u16 dst;          // destination parent uid (0 = the root, INV_AUTO), or the target item (insert / mount)
    u8 x, y, rot;
    u8 aux;           // attachment slot / hotbar slot / equipment slot
    u16 count;
    u32 seq;          // client sequence (reconcile); 0 = unsequenced
} InvIntent;

typedef struct InvEffect {
    InvTree dropped;                  // II_DISCARD: what the host spawns as pickups, as it was: each top at the root,
                                      // a container's contents under it with their layout (one pickup keeps them)
    u16 used_item;                    // II_USE: the consumed item (host: heal / armour / vehicle use)
    u16 moved_uid;                    // where the moved item ended up (the selection follows it)
    u8 moved_side;
} InvEffect;

typedef struct InvCtx {
    InvTree *tree[INV_SIDES];   // NULL: that side isn't open
    u16 *quick;                 // the hotbar binds (self uids)
    int quick_n;
    const ItemDB *db;
    InvOrder order;
    bool loot_reachable;        // host: the open container is in range (clients predict true)
} InvCtx;

InvCtx inv_ctx_make(const ItemDB *db, InvTree *self, u16 *quick, int quick_n, InvTree *loot);
int inv_intent_apply(InvCtx *ctx, const InvIntent *in, InvEffect *fx);
const char *inv_intent_name(int type);
int inv_intent_from_name(const char *s);   // II_NONE if unknown

// Attachment compatibility of an attachment item for a weapon instance: IIR_OK and the slot it
// goes into, or why not (IIR_NOT_WEAPON / IIR_NO_SLOT / IIR_INCOMPATIBLE).
int inv_attach_check(const ItemDB *db, const InvSlot *weapon, u16 att_item, int *aslot_out);

// What a hotbar slot may hold (II_BIND, the pickup binds): weapons, consumables, throwables, the
// "hotbar": true tools. Never a container / wearable or the built-in phone.
bool inv_bindable(const ItemDef *d);

// Pickup rule (docs/INVENTORY.md): merge into stacks, equip a weapon / wearable into a free slot,
// else the first space in the order; weapons, consumables and throwables bind to a free hotbar
// slot (so do the ones moved over from a world container by any intent). Returns how many could
// NOT be added.
int inv_give(InvCtx *ctx, const InvSlot *s);
// A container picked up with what it held (a dropped backpack's pickup): contents' root level goes
// into it, every entry in its own place. All or nothing: returns 0 or s->count.
int inv_give_with(InvCtx *ctx, const InvSlot *s, const InvTree *contents);
// The same into a world container's root grids (no equipment, no binds).
int inv_give_root(InvTree *t, const ItemDB *db, const InvSlot *s);

// Death: everything but the secure container's subtree moves from the body into the corpse tree
// (same places: the corpse shows the equipment). Binds into what left are cleared.
void inv_strip_to_corpse(InvTree *body, u16 *quick, int quick_n, InvTree *corpse, const ItemDB *db);

#endif
