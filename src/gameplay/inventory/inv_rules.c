#include "inv_rules.h"
#include <string.h>

const char *inv_result_text(int r) {
    static const char *t[IIR_COUNT] = { "", "Nothing to do", "Invalid", "No room", "Doesn't go in there", "Only the worn backpack holds items",
                                        "Someone else took it", "Too far away", "Inventory full", "Doesn't go in that slot", "Can't be used",
                                        "Doesn't fit this weapon", "This weapon has no slot for it", "Takes no attachments", "Can't be rotated",
                                        "Empty it first", "The default bag stays on" };
    return r >= 0 && r < IIR_COUNT ? t[r] : "?";
}

bool inv_equip_accepts(int slot, const ItemDef *d) {
    if (slot < 0 || slot >= EQ_COUNT || d->equip_slot == EQ_NONE) return false;
    if (slot == EQ_PRIMARY || slot == EQ_SECONDARY) return d->equip_slot == EQ_PRIMARY;
    return d->equip_slot == slot;
}

int inv_accepts(const ItemDB *db, const InvContainerDef *cd, const InvSlot *s, int w, int h) {
    const ItemDef *d = item_def(db, s->item);
    u16 bit = (u16)(1u << d->kind);
    if (cd->allow_kinds && !(cd->allow_kinds & bit)) return IIR_FILTER;
    if (cd->deny_kinds & bit) return IIR_FILTER;
    for (int i = 0; i < cd->deny_count; i++) if (cd->deny_items[i] == s->item) return IIR_FILTER;
    if (cd->max_w && cd->max_h) {
        int lo = DG_MIN(w, h), hi = DG_MAX(w, h);
        int clo = DG_MIN(cd->max_w, cd->max_h), chi = DG_MAX(cd->max_w, cd->max_h);
        if (lo > clo || hi > chi) return IIR_FILTER;
    }
    return IIR_OK;
}

static bool overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

int inv_check_place(const InvTree *t, const ItemDB *db, const InvItem *it, InvPlace p, u16 ignore, u16 *blocker) {
    if (blocker) *blocker = 0;
    const ItemDef *d = item_def(db, it->s.item);
    if (!it->s.item) return IIR_INVALID;
    u16 self = it->uid;
    u16 found = 0;
    int blockers = 0;
    if (p.loc >= INV_LOC_EQUIP) {
        int slot = p.loc - INV_LOC_EQUIP;
        if (p.parent || !inv_root_def(t, db)->equip || slot >= EQ_COUNT) return IIR_INVALID;
        if (!inv_equip_accepts(slot, d)) return IIR_EQUIP;
        const InvItem *occ = inv_equipped_c(t, slot);
        if (occ && occ->uid != self && occ->uid != ignore && occ->loc != INV_LOC_NONE) {
            if (blocker) *blocker = occ->uid;
            return IIR_NO_ROOM;
        }
        return IIR_OK;
    }
    // Only a tree's grid holds items: a world container's root, a body's worn backpack.
    if (p.parent != inv_grid(t, db)) return IIR_NESTING;
    if (p.parent && p.parent == self) return IIR_NESTING;
    const InvContainerDef *cd = inv_parent_def(t, db, p.parent);
    if (!cd) return IIR_INVALID;
    if (p.loc >= cd->comp_count) return IIR_INVALID;
    if (p.rot && !d->rotatable) return IIR_NOT_ROTATABLE;
    // A backpack in a grid is an empty item (its contents would nest).
    if (self && item_container(db, it->s.item) && inv_child_count(t, self)) return IIR_NOT_EMPTY;
    int bw, bh;
    inv_base_size(db, &it->s, it->flags, &bw, &bh);
    int w = p.rot ? bh : bw, h = p.rot ? bw : bh;
    int r = inv_accepts(db, cd, &it->s, w, h);
    if (r != IIR_OK) return r;
    const InvCompDef *c = &cd->comp[p.loc];
    if (p.x + w > c->w || p.y + h > c->h) return IIR_NO_ROOM;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *o = &t->items[i];
        if (!inv_alive(o) || o->parent != p.parent || o->loc != p.loc || o->uid == self || o->uid == ignore) continue;
        int ow, oh;
        inv_item_size(db, o, &ow, &oh);
        if (!overlap(p.x, p.y, w, h, o->x, o->y, ow, oh)) continue;
        if (!blockers++) found = o->uid;
    }
    if (blockers) {
        if (blocker && blockers == 1) *blocker = found;
        return IIR_NO_ROOM;
    }
    return IIR_OK;
}

bool inv_find_space(const InvTree *t, const ItemDB *db, const InvItem *it, u16 parent, InvPlace *out) {
    const InvContainerDef *cd = parent == INV_NO_GRID ? NULL : inv_parent_def(t, db, parent);
    if (!cd) return false;
    const ItemDef *d = item_def(db, it->s.item);
    for (int c = 0; c < cd->comp_count; c++)
        for (int rot = 0; rot < (d->rotatable ? 2 : 1); rot++)
            for (int y = 0; y < cd->comp[c].h; y++)
                for (int x = 0; x < cd->comp[c].w; x++) {
                    InvPlace p = { parent, (u8)c, (u8)x, (u8)y, (u8)rot };
                    if (inv_check_place(t, db, it, p, 0, NULL) == IIR_OK) { *out = p; return true; }
                }
    return false;
}

bool inv_find_grid_space(const InvTree *t, const ItemDB *db, const InvItem *it, InvPlace *out) {
    return inv_find_space(t, db, it, inv_grid(t, db), out);
}

int inv_equip_target(const InvTree *t, const ItemDB *db, const InvItem *it) {
    const ItemDef *d = item_def(db, it->s.item);
    if (d->equip_slot == EQ_NONE || !inv_root_def(t, db)->equip) return EQ_NONE;
    if (d->equip_slot == EQ_PRIMARY) {
        const InvItem *p = inv_equipped_c(t, EQ_PRIMARY);
        if (p && p->uid != it->uid && !inv_equipped_c(t, EQ_SECONDARY)) return EQ_SECONDARY;
        return EQ_PRIMARY;
    }
    return d->equip_slot;
}
