#include "inv_ops.h"
#include "attachments.h"
#include <string.h>

static bool valid_slot(int s) { return s >= 0 && s < INV_SLOTS; }
static int max_stack(const ItemDB *db, u16 item) { return DG_MAX(1, item_def(db, item)->max_stack); }

const char *inv_op_result_text(int r) {
    static const char *t[IOR_COUNT] = { "", "Nothing to do", "Invalid", "Inventory full", "Doesn't fit this weapon", "This weapon has no slot for it",
                                        "Can't be used", "Takes no attachments" };
    return r >= 0 && r < IOR_COUNT ? t[r] : "?";
}

const char *inv_op_name(int type) {
    static const char *n[IOP_COUNT] = { "none", "move", "split", "drop", "use", "equip", "quick_move", "mount", "unmount" };
    return type >= 0 && type < IOP_COUNT ? n[type] : "?";
}

int inv_first_empty(const Inventory *inv, int from, int to) {
    for (int i = DG_MAX(0, from); i < DG_MIN(to, INV_SLOTS); i++) if (!inv->slots[i].item) return i;
    return -1;
}

int inv_merge(InvSlot *dst, InvSlot *src, const ItemDB *db, int count) {
    if (!src->item || (dst->item && dst->item != src->item)) return 0;
    int ms = max_stack(db, src->item);
    int want = count > 0 ? DG_MIN(count, (int)src->count) : (int)src->count;
    int room = dst->item ? ms - (int)dst->count : ms;
    int move = DG_MIN(want, room);
    if (move <= 0) return 0;
    if (!dst->item) {
        *dst = *src;
        dst->count = 0;
    }
    dst->count = (u16)(dst->count + move);
    src->count = (u16)(src->count - move);
    if (!src->count) memset(src, 0, sizeof(*src));
    return move;
}

int inv_attach_check(const ItemDB *db, const InvSlot *weapon, u16 att_item, int *aslot_out) {
    const WeaponDef *wd = item_weapon(db, weapon->item);
    if (!wd || wd->kind == WPN_MELEE) return IOR_NOT_WEAPON;
    const ItemDef *ai = item_def(db, att_item);
    const AttachmentDef *a = ai->kind == ITEM_ATTACHMENT ? attach_def(db, ai->attachment) : NULL;
    if (!a) return IOR_INVALID;
    if (aslot_out) *aslot_out = a->slot;
    if (a->slot < 0 || a->slot >= ATT_SLOTS || !wd->att_slots[a->slot].present) return IOR_NO_SLOT;
    if (!attach_allowed(wd, a)) return IOR_INCOMPATIBLE;
    return IOR_OK;
}

// A slot of the other section (hotbar <-> backpack) for quick moves: an existing stack, then empty.
static int quick_target(const Inventory *inv, const ItemDB *db, int from) {
    const InvSlot *s = &inv->slots[from];
    bool hot = from < INV_HOTBAR;
    int lo = hot ? INV_HOTBAR : 0, hi = hot ? INV_SLOTS : INV_HOTBAR;
    if (max_stack(db, s->item) > 1)
        for (int i = lo; i < hi; i++)
            if (inv->slots[i].item == s->item && inv->slots[i].count < max_stack(db, s->item)) return i;
    return inv_first_empty(inv, lo, hi);
}

// Clamp the magazine to the new capacity; the extra rounds go back to the reserve.
static void fix_magazine(Inventory *inv, const ItemDB *db, InvSlot *w) {
    const WeaponDef *wd = item_weapon(db, w->item);
    if (!wd) return;
    WeaponDef eff;
    attach_weapon_effective(db, wd, w->att, &eff);
    if (eff.mag_size > 0 && w->ammo > eff.mag_size) {
        int extra = w->ammo - eff.mag_size;
        w->ammo = (i16)eff.mag_size;
        if (wd->ammo_item) slots_add(inv->slots, INV_SLOTS, db, wd->ammo_item, extra, 0, INV_HOTBAR, INV_BACKPACK);
    }
}

int inv_op_apply(Inventory *inv, const ItemDB *db, const InvOp *op, InvOpEffect *fx) {
    if (fx) memset(fx, 0, sizeof(*fx));
    int a = op->a, b = op->b;
    switch (op->type) {
    case IOP_MOVE: {
        if (!valid_slot(a) || !valid_slot(b) || a == b) return IOR_INVALID;
        InvSlot *s = &inv->slots[a], *d = &inv->slots[b];
        if (!s->item) return IOR_NOOP;
        int count = op->count && op->count < s->count ? op->count : 0;
        if (!d->item || d->item == s->item) {
            if (d->item && max_stack(db, s->item) <= 1) {
                InvSlot t = *s; *s = *d; *d = t;   // two weapons of the same kind: swap (attachments / ammo differ)
                inv->revision++;
                return IOR_OK;
            }
            int moved = inv_merge(d, s, db, count);
            if (!moved) {
                if (d->item == s->item && !count) { InvSlot t = *s; *s = *d; *d = t; inv->revision++; return IOR_OK; }   // full stack: swap
                return IOR_FULL;
            }
            inv->revision++;
            return IOR_OK;
        }
        if (count) return IOR_INVALID;   // a partial stack can't swap with another item
        InvSlot t = *s;
        *s = *d;
        *d = t;
        inv->revision++;
        return IOR_OK;
    }
    case IOP_SPLIT: {
        if (!valid_slot(a)) return IOR_INVALID;
        InvSlot *s = &inv->slots[a];
        if (!s->item || s->count < 2) return IOR_NOOP;
        int to = b == 255 ? inv_first_empty(inv, INV_HOTBAR, INV_SLOTS) : b;
        if (to < 0 && b == 255) to = inv_first_empty(inv, 0, INV_HOTBAR);
        if (!valid_slot(to) || to == a) return IOR_FULL;
        if (inv->slots[to].item) return IOR_FULL;
        int n = op->count ? DG_MIN((int)op->count, s->count - 1) : s->count / 2;
        if (n <= 0) return IOR_NOOP;
        inv_merge(&inv->slots[to], s, db, n);
        inv->revision++;
        return IOR_OK;
    }
    case IOP_DROP: {
        if (!valid_slot(a)) return IOR_INVALID;
        InvSlot *s = &inv->slots[a];
        if (!s->item) return IOR_NOOP;
        int n = op->count && op->count < s->count ? op->count : s->count;
        if (fx) { fx->dropped = *s; fx->dropped.count = (u16)n; }
        s->count = (u16)(s->count - n);
        if (!s->count) memset(s, 0, sizeof(*s));
        inv->revision++;
        return IOR_OK;
    }
    case IOP_USE: {
        if (!valid_slot(a)) return IOR_INVALID;
        InvSlot *s = &inv->slots[a];
        if (!s->item) return IOR_NOOP;
        if (item_def(db, s->item)->kind != ITEM_CONSUMABLE) return IOR_NOT_USABLE;
        if (fx) fx->used_item = s->item;
        if (--s->count == 0) memset(s, 0, sizeof(*s));
        inv->revision++;
        return IOR_OK;
    }
    case IOP_EQUIP: {
        if (!valid_slot(a)) return IOR_INVALID;
        if (!inv->slots[a].item) return IOR_NOOP;
        if (a < INV_HOTBAR) return IOR_OK;   // already in the hotbar (the client selects it)
        int to = inv_first_empty(inv, 0, INV_HOTBAR);
        if (to < 0) to = b < INV_HOTBAR ? b : 0;
        InvSlot t = inv->slots[a];
        inv->slots[a] = inv->slots[to];
        inv->slots[to] = t;
        inv->revision++;
        return IOR_OK;
    }
    case IOP_QUICK_MOVE: {
        if (!valid_slot(a) || !inv->slots[a].item) return IOR_NOOP;
        int to = quick_target(inv, db, a);
        if (to < 0) return IOR_FULL;
        InvSlot *s = &inv->slots[a];
        if (inv_merge(&inv->slots[to], s, db, 0) <= 0) return IOR_FULL;
        // Remainder: another pass (the next stack / empty slot).
        while (s->item) {
            int t2 = quick_target(inv, db, a);
            if (t2 < 0 || inv_merge(&inv->slots[t2], s, db, 0) <= 0) break;
        }
        inv->revision++;
        return IOR_OK;
    }
    case IOP_MOUNT: {
        if (!valid_slot(a) || !valid_slot(b) || a == b) return IOR_INVALID;
        InvSlot *w = &inv->slots[a], *it = &inv->slots[b];
        if (!it->item) return IOR_NOOP;
        int aslot = -1;
        int r = inv_attach_check(db, w, it->item, &aslot);
        if (r != IOR_OK) return r;
        u8 new_id = item_def(db, it->item)->attachment;
        u8 old_id = w->att[aslot];
        if (old_id == new_id) return IOR_NOOP;
        // The attachment item is consumed; a replaced attachment takes its place in the inventory.
        memset(it, 0, sizeof(*it));
        u16 old_item = item_for_attachment(db, old_id);
        if (old_item) { it->item = old_item; it->count = 1; }
        w->att[aslot] = new_id;
        fix_magazine(inv, db, w);
        inv->revision++;
        return IOR_OK;
    }
    case IOP_UNMOUNT: {
        if (!valid_slot(a) || b >= ATT_SLOTS) return IOR_INVALID;
        InvSlot *w = &inv->slots[a];
        if (!item_weapon(db, w->item)) return IOR_NOT_WEAPON;
        u8 id = w->att[b];
        if (!id) return IOR_NOOP;
        u16 item = item_for_attachment(db, id);
        if (!item) return IOR_INVALID;
        int to = op->c == 255 ? inv_first_empty(inv, INV_HOTBAR, INV_SLOTS) : op->c;
        if (to < 0 && op->c == 255) to = inv_first_empty(inv, 0, INV_HOTBAR);
        if (!valid_slot(to) || to == a || inv->slots[to].item) return IOR_FULL;
        inv->slots[to].item = item;
        inv->slots[to].count = 1;
        w->att[b] = 0;
        fix_magazine(inv, db, w);
        inv->revision++;
        return IOR_OK;
    }
    default: return IOR_INVALID;
    }
}

// a: type | slot a << 8 | slot b << 16 | slot c << 24; b: count; c: sequence.
void inv_op_pack(const InvOp *op, int *a, int *b, int *c) {
    *a = (int)((u32)op->type | ((u32)op->a << 8) | ((u32)op->b << 16) | ((u32)op->c << 24));
    *b = (int)op->count;
    *c = (int)op->seq;
}

void inv_op_unpack(int a, int b, int c, InvOp *op) {
    u32 u = (u32)a;
    op->type = (u8)(u & 255);
    op->a = (u8)((u >> 8) & 255);
    op->b = (u8)((u >> 16) & 255);
    op->c = (u8)((u >> 24) & 255);
    op->count = (u16)DG_CLAMP(b, 0, 65535);
    op->seq = (u32)c;
}
