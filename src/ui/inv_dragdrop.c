#include "inv_dragdrop.h"
#include "gameplay/attachments.h"
#include <string.h>

void dd_reset(DragDrop *d) { memset(d, 0, sizeof(*d)); }

static DDResult none(void) {
    DDResult r;
    memset(&r, 0, sizeof(r));
    return r;
}

static DDResult op(int type, int a, int b, int c, int count) {
    DDResult r = none();
    r.action = DDA_OP;
    r.op.type = (u8)type;
    r.op.a = (u8)a;
    r.op.b = (u8)b;
    r.op.c = (u8)c;
    r.op.count = (u16)count;
    return r;
}

static DDResult reject(int reason, DDTarget t) {
    DDResult r = none();
    r.action = DDA_REJECT;
    r.reject_reason = reason;
    r.reject_target = t;
    r.slot = t.index;
    return r;
}

static bool is_item_slot(DDTarget t) { return t.kind == DT_SLOT && t.index >= 0 && t.index < INV_SLOTS; }

DDResult dd_drop_on(const DragDrop *d, DDTarget t, const Inventory *inv, const ItemDB *db, int weapon_slot) {
    // Carrying a crate item: anywhere in the inventory takes it.
    if (d->src.kind == DT_CONTAINER) {
        if (t.kind == DT_SLOT || t.kind == DT_PANEL || t.kind == DT_PREVIEW || t.kind == DT_ATTACH) {
            DDResult r = none();
            r.action = DDA_CONTAINER_TAKE;
            r.slot = d->src.index;
            return r;
        }
        DDResult r = none();
        r.action = DDA_CANCEL;
        return r;
    }
    // An attachment carried out of the preview: unmount into the slot (or the first free one).
    if (d->src.kind == DT_ATTACH) {
        if (weapon_slot < 0) return reject(IOR_NOT_WEAPON, t);
        if (is_item_slot(t)) return op(IOP_UNMOUNT, weapon_slot, d->src.index, inv->slots[t.index].item ? 255 : t.index, 0);
        if (t.kind == DT_PANEL || t.kind == DT_OUTSIDE) return op(IOP_UNMOUNT, weapon_slot, d->src.index, 255, 0);
        DDResult r = none();
        r.action = DDA_CANCEL;
        return r;
    }
    if (!is_item_slot(d->src)) { DDResult r = none(); r.action = DDA_CANCEL; return r; }
    int from = d->src.index;
    const InvSlot *s = &inv->slots[from];
    if (!s->item) { DDResult r = none(); r.action = DDA_CANCEL; return r; }
    const ItemDef *it = item_def(db, s->item);
    int count = d->split && s->count > 1 ? s->count / 2 : 0;
    switch (t.kind) {
    case DT_SLOT: {
        if (!is_item_slot(t) || t.index == from) { DDResult r = none(); r.action = DDA_CANCEL; return r; }
        const InvSlot *dst = &inv->slots[t.index];
        // Attachment onto a weapon: mount.
        if (it->kind == ITEM_ATTACHMENT && dst->item && item_weapon(db, dst->item)) {
            int aslot;
            int res = inv_attach_check(db, dst, s->item, &aslot);
            if (res != IOR_OK) return reject(res, t);
            return op(IOP_MOUNT, t.index, from, 0, 0);
        }
        // A partial stack can't swap into another item's slot.
        if (count && dst->item && dst->item != s->item) return reject(IOR_FULL, t);
        if (count && !dst->item) return op(IOP_SPLIT, from, t.index, 0, count);
        return op(IOP_MOVE, from, t.index, 0, count);
    }
    case DT_ATTACH: {
        if (weapon_slot < 0) return reject(IOR_NOT_WEAPON, t);
        if (it->kind != ITEM_ATTACHMENT) return reject(IOR_INCOMPATIBLE, t);
        int aslot;
        int res = inv_attach_check(db, &inv->slots[weapon_slot], s->item, &aslot);
        if (res != IOR_OK) return reject(res, t);
        if (aslot != t.index) return reject(IOR_NO_SLOT, t);
        return op(IOP_MOUNT, weapon_slot, from, 0, 0);
    }
    case DT_PREVIEW: {
        if (it->kind == ITEM_ATTACHMENT && weapon_slot >= 0) {
            int aslot;
            int res = inv_attach_check(db, &inv->slots[weapon_slot], s->item, &aslot);
            if (res != IOR_OK) return reject(res, t);
            return op(IOP_MOUNT, weapon_slot, from, 0, 0);
        }
        DDResult r = none();
        r.action = DDA_SELECT;
        r.slot = from;
        return r;
    }
    case DT_OUTSIDE: {
        DDResult r = op(IOP_DROP, from, 0, 0, count);
        if (DG_MAX(1, it->max_stack) <= 1) r.action = DDA_CONFIRM_DROP;   // weapons, keys, attachments: ask
        return r;
    }
    case DT_CONTAINER: {
        DDResult r = none();
        r.action = DDA_CONTAINER_PUT;
        r.slot = from;
        return r;
    }
    default: {
        DDResult r = none();
        r.action = DDA_CANCEL;
        return r;
    }
    }
}

static bool carriable(DDTarget t, const Inventory *inv) {
    if (t.kind == DT_SLOT) return is_item_slot(t) && inv->slots[t.index].item;
    return t.kind == DT_ATTACH || t.kind == DT_CONTAINER;
}

DDResult dd_update(DragDrop *d, const DDInput *in, const Inventory *inv, const ItemDB *db) {
    DDResult r = none();
    if (d->carry) {
        if (in->cancel) { dd_reset(d); r.action = DDA_CANCEL; }
        return r;   // the carry ends with dd_carry_accept
    }
    switch (d->state) {
    case DD_IDLE:
        if (in->right_pressed && in->hover.kind == DT_SLOT && is_item_slot(in->hover) && inv->slots[in->hover.index].item) {
            r.action = DDA_CONTEXT;
            r.slot = in->hover.index;
            return r;
        }
        if (in->double_clicked && is_item_slot(in->hover) && inv->slots[in->hover.index].item) {
            r = op(IOP_EQUIP, in->hover.index, 0, 0, 0);
            return r;
        }
        if (in->pressed && carriable(in->hover, inv)) {
            d->state = DD_PRESSED;
            d->src = in->hover;
            d->press_pos = in->mouse;
            d->split = in->ctrl;
        }
        return r;
    case DD_PRESSED: {
        vec2 dv = v2(in->mouse.x - d->press_pos.x, in->mouse.y - d->press_pos.y);
        if (dv.x * dv.x + dv.y * dv.y > in->threshold * in->threshold) {
            d->state = DD_DRAGGING;
            r.action = DDA_PICKED;
            return r;
        }
        if (in->released) {
            DDTarget src = d->src;
            dd_reset(d);
            if (src.kind == DT_SLOT && in->shift) return op(IOP_QUICK_MOVE, src.index, 0, 0, 0);
            if (src.kind == DT_CONTAINER && in->shift) { r.action = DDA_CONTAINER_TAKE; r.slot = src.index; return r; }
            r.action = DDA_SELECT;
            r.slot = src.kind == DT_SLOT ? src.index : -1;
            return r;
        }
        return r;
    }
    case DD_DRAGGING:
        if (in->cancel) { dd_reset(d); r.action = DDA_CANCEL; return r; }
        if (in->released) {
            r = dd_drop_on(d, in->hover, inv, db, in->weapon_slot);
            dd_reset(d);
            return r;
        }
        return r;
    }
    return r;
}

DDResult dd_carry_accept(DragDrop *d, DDTarget focus, const Inventory *inv, const ItemDB *db, int weapon_slot, bool ctrl) {
    DDResult r = none();
    if (!d->carry) {
        if (!carriable(focus, inv)) return r;
        dd_reset(d);
        d->carry = true;
        d->state = DD_DRAGGING;
        d->src = focus;
        d->split = ctrl;
        r.action = DDA_PICKED;
        return r;
    }
    if (focus.kind == d->src.kind && focus.index == d->src.index) {   // placed back: cancel
        dd_reset(d);
        r.action = DDA_CANCEL;
        return r;
    }
    r = dd_drop_on(d, focus, inv, db, weapon_slot);
    dd_reset(d);
    return r;
}
