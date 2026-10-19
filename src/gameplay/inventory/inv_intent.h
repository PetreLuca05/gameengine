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
    II_MOVE,         // src -> (dst side, dst parent, loc, x, y, rot), count (0 = all): place / merge / swap
    II_ROTATE,       // src rotated in place
    II_EQUIP,        // src -> equipment slot aux (255 = its own); the occupant swaps out
    II_QUICK_MOVE,   // src -> across (loot open), else between the equipment slots and the grid
    II_SPLIT,        // count (0 = half) of src -> dst place (dst INV_AUTO: the first space)
    II_DISCARD,      // src, count (0 = all): removed, the host spawns the pickup
    II_USE,          // src (self): a consumable used once
    II_MOUNT,        // attachment src -> weapon dst (on dst side)
    II_UNMOUNT,      // weapon src, attachment slot aux -> dst place (dst INV_AUTO: the first space)
    II_FOLD,         // weapon src: fold / unfold the stock
    II_UNLOAD,       // weapon src: the loaded rounds back into an ammo stack
    II_HOLD,         // src (self) into the hands (0 = put away): a consumable / throwable / hand tool
    II_TAKE_ALL,     // everything of the loot tree that fits -> self
    II_COMPACT,      // re-pack the grid of side dst (largest first, reading order kept)
    II_COUNT
} InvIntentType;

#define INV_AUTO 0xFFFF   // dst: find a place

typedef struct InvIntent {
    u8 type;          // InvIntentType
    u8 src_side, dst_side;
    u8 loc;           // destination compartment, or INV_LOC_EQUIP + slot
    u16 src;          // item uid in the src side's tree
    u16 dst;          // destination parent uid (0 = the root, a worn backpack's uid, INV_AUTO), or the target weapon (mount)
    u8 x, y, rot;
    u8 aux;           // attachment slot / equipment slot
    u16 count;
    u32 seq;          // client sequence (reconcile); 0 = unsequenced
} InvIntent;

typedef struct InvEffect {
    InvTree dropped;                  // II_DISCARD: what the host spawns as a pickup (one entry at the root)
    u16 used_item;                    // II_USE: the consumed item (host: heal / armour / vehicle use)
    u16 moved_uid;                    // where the moved item ended up (the selection follows it)
    u8 moved_side;
} InvEffect;

typedef struct InvCtx {
    InvTree *tree[INV_SIDES];   // NULL: that side isn't open
    u16 *hands;                 // the item in the hands (self uid, 0 = none); NULL: no hands
    const ItemDB *db;
    bool loot_reachable;        // host: the open container is in range (clients predict true)
} InvCtx;

InvCtx inv_ctx_make(const ItemDB *db, InvTree *self, u16 *hands, InvTree *loot);
int inv_intent_apply(InvCtx *ctx, const InvIntent *in, InvEffect *fx);
const char *inv_intent_name(int type);
int inv_intent_from_name(const char *s);   // II_NONE if unknown

// Attachment compatibility of an attachment item for a weapon instance: IIR_OK and the slot it
// goes into, or why not (IIR_NOT_WEAPON / IIR_NO_SLOT / IIR_INCOMPATIBLE).
int inv_attach_check(const ItemDB *db, const InvSlot *weapon, u16 att_item, int *aslot_out);

// What the hands may hold from the grid (II_HOLD, the wheels): consumables, throwables, the
// "hands": true tools. Weapons are drawn from their equipment slots; never the built-in phone.
bool inv_holdable(const ItemDef *d);

// Pickup rule (docs/INVENTORY.md): merge into the grid's stacks, a weapon into a free slot, a bigger
// backpack onto the back over the default bag (the contents move over), else the first space in
// the grid. Returns how many could NOT be added.
int inv_give(InvCtx *ctx, const InvSlot *s);
// The same into a world container's root grid (no equipment).
int inv_give_root(InvTree *t, const ItemDB *db, const InvSlot *s);
// A body tree without a backpack gets the default bag (spawn, after death); true if it got one.
bool inv_ensure_pack(InvTree *t, const ItemDB *db);

// Death: everything moves from the body into the corpse tree (same places: the corpse wears the
// weapons and the backpack with its contents); the body gets a fresh default bag.
void inv_strip_to_corpse(InvTree *body, u16 *hands, InvTree *corpse, const ItemDB *db);

#endif
