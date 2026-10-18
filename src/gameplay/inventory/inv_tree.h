// Grid inventory tree (docs/INVENTORY.md): a pool of item instances forming a tree. The root is a
// container def (a body: equipment slots + pockets; a crate; a trunk); every other container is an
// item with its own compartments (rig, backpack, case). Pure: no world, no network.
#ifndef DG_INV_TREE_H
#define DG_INV_TREE_H

#include "core/base.h"
#include "gameplay/items.h"

// The stack payload of an instance: what gameplay code (weapons, HUD, previews) reads.
typedef struct InvSlot {
    u16 item;
    u16 count;
    i16 ammo;            // rounds loaded (weapons), DNA sample id, charges left (INV_USE_CHARGES)
    u8 att[ATT_SLOTS];   // attachment ids per slot (weapons; 0 = none, gameplay/attachments.h)
    u8 pad;
} InvSlot;

#define INV_MAX_ITEMS 128
#define INV_LOC_EQUIP 64        // loc >= INV_LOC_EQUIP: equipment slot loc - INV_LOC_EQUIP (body roots)
#define INV_LOC_NONE 255        // detached (inside an intent, never stored)
#define INVF_ROT 1              // rotated 90 degrees
#define INVF_FOLDED 2           // stock folded (weapons with fold_dw)

typedef struct InvItem {
    InvSlot s;
    u16 uid;          // unique within its tree; 0 = free entry
    u16 parent;       // uid of the container item it is in; 0 = the root
    u8 loc;           // compartment index, or INV_LOC_EQUIP + slot at the root of a body
    u8 x, y;          // top-left cell
    u8 flags;         // INVF_*
    u16 dur;          // durability left
} InvItem;

typedef struct InvTree {
    InvItem items[INV_MAX_ITEMS];
    u16 next_uid;
    i16 root;         // container def index of the root (ItemDB.containers)
} InvTree;

void inv_tree_init(InvTree *t, int root_def);
static inline bool inv_alive(const InvItem *it) { return it->uid && it->s.item && it->s.count; }
InvItem *inv_get(InvTree *t, u16 uid);                       // NULL: no such live item
const InvItem *inv_get_c(const InvTree *t, u16 uid);
int inv_live_count(const InvTree *t);
int inv_free_count(const InvTree *t);
// A new instance (uid assigned, durability full); NULL when the pool is full.
InvItem *inv_alloc(InvTree *t, const ItemDB *db, const InvSlot *s);

// The root container def (a 4-pocket body when the def is missing) / the def of a parent (0 = root).
const InvContainerDef *inv_root_def(const InvTree *t, const ItemDB *db);
const InvContainerDef *inv_parent_def(const InvTree *t, const ItemDB *db, u16 parent);

// Footprint: the def's grid + attachment deltas - the folded stock (unrotated), then rotated.
void inv_base_size(const ItemDB *db, const InvSlot *s, u8 flags, int *w, int *h);
void inv_item_size(const ItemDB *db, const InvItem *it, int *w, int *h);

// True when anc is uid itself or one of its ancestors.
bool inv_is_ancestor(const InvTree *t, u16 anc, u16 uid);
// uid and every descendant, parents before children; returns the count (indices into t->items).
int inv_subtree(const InvTree *t, u16 uid, int *idx, int max);
void inv_free_subtree(InvTree *t, u16 uid);
// Drops dead entries (count 0 / no item) and orphans (a parent that is gone or not a container).
void inv_normalize(InvTree *t, const ItemDB *db);

// Weight (kg, informational): the instance x count plus its contents; the whole tree.
float inv_item_weight(const InvTree *t, const ItemDB *db, const InvItem *it);
float inv_tree_weight(const InvTree *t, const ItemDB *db);
float inv_parent_weight(const InvTree *t, const ItemDB *db, u16 parent);   // everything in a container (0 = the root grids)
// Cells used in a container's compartments (all of them); the capacity is the def's.
int inv_cells_used(const InvTree *t, const ItemDB *db, u16 parent);

// The live item covering cell (x, y) of compartment loc of parent; the item in an equipment slot.
InvItem *inv_at(InvTree *t, const ItemDB *db, u16 parent, int loc, int x, int y);
InvItem *inv_equipped(InvTree *t, int slot);
const InvItem *inv_equipped_c(const InvTree *t, int slot);

#endif
