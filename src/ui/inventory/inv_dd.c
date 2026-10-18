// Drag and drop (docs/INVENTORY.md "Screen"):
//
//   IDLE --press on an item--> PRESSED --moved > threshold--> DRAGGING --release--> IDLE + intent
//   PRESSED --release--> IDLE + select (Ctrl: quick move, Alt: equip)   DRAGGING --Esc / B--> IDLE
//   double click: open / inspect; right click: context menu; R: rotate the carried item;
//   pad / keyboard: A picks up what the cursor is on, A again places it (the same rules).
//
// Release rules: a grid cell = move (place / merge onto a stack / into a container / swap, Shift:
// a split dialog); an equipment slot = equip; a hotbar tile = bind (a loot item: rejected); an
// attachment slot of the inspected weapon = mount; an attachment chip onto a cell = unmount there;
// the discard zone / outside the screen = discard (weapons and containers ask first); a hotbar
// tile dragged off the hotbar = unbind.
#include "inv_ui.h"
#include <string.h>

void idd_reset(InvDrag *d) { memset(d, 0, sizeof(*d)); }

const char *idd_reason_text(int reason) {
    if (reason == IDR_TAKE_FIRST) return "Take it first";
    return inv_result_text(reason);
}

static const InvItem *item_at(const InvView *v, int side, u16 uid) {
    if (side < 0 || side >= INV_SIDES || !v->tree[side] || !uid) return NULL;
    return inv_get_c(v->tree[side], uid);
}

static InvIntent mk(int type, int ss, u16 src, int ds, u16 dst) {
    InvIntent in;
    memset(&in, 0, sizeof(in));
    in.type = (u8)type;
    in.src_side = (u8)ss;
    in.dst_side = (u8)ds;
    in.src = src;
    in.dst = dst;
    return in;
}

static InvDDOut out_intent(InvIntent in, InvTarget at) {
    InvDDOut o;
    memset(&o, 0, sizeof(o));
    o.action = IDA_INTENT;
    o.in = in;
    o.at = at;
    return o;
}

static InvDDOut out_reject(InvTarget at, int reason) {
    InvDDOut o;
    memset(&o, 0, sizeof(o));
    o.action = IDA_REJECT;
    o.at = at;
    o.reason = reason;
    return o;
}

static InvDDOut out_act(int action, InvTarget at) {
    InvDDOut o;
    memset(&o, 0, sizeof(o));
    o.action = action;
    o.at = at;
    return o;
}

// The grabbed cell in the carried orientation (rotating swaps the axes of the grab offset).
static void grab_rot(const InvDrag *d, const InvView *v, int *gx, int *gy) {
    const InvItem *it = item_at(v, d->side, d->uid);
    int bw = 1, bh = 1;
    if (it) inv_base_size(v->db, &it->s, it->flags, &bw, &bh);
    int ox = DG_CLAMP(d->grab_x, 0, bw - 1), oy = DG_CLAMP(d->grab_y, 0, bh - 1);
    if (d->rot) { *gx = oy; *gy = ox; }
    else { *gx = ox; *gy = oy; }
}

bool idd_footprint(const InvDrag *d, InvTarget t, const InvView *v, int *x, int *y, int *w, int *h) {
    if (t.kind != IT_CELL || d->state == IDD_IDLE) return false;
    const InvItem *it = item_at(v, d->side, d->uid);
    if (!it || d->src.kind == IT_ATTACH || d->src.kind == IT_HOT) {
        *x = t.x; *y = t.y; *w = *h = 1;
        if (d->src.kind == IT_HOT) return false;
        return d->src.kind == IT_ATTACH;
    }
    int bw, bh;
    inv_base_size(v->db, &it->s, it->flags, &bw, &bh);
    int gx, gy;
    grab_rot(d, v, &gx, &gy);
    *w = d->rot ? bh : bw;
    *h = d->rot ? bw : bh;
    *x = t.x - gx;
    *y = t.y - gy;
    return true;
}

static bool confirm_discard(const InvItem *it, const InvView *v) {
    const ItemDef *d = item_def(v->db, it->s.item);
    return DG_MAX(1, d->max_stack) <= 1 && (d->kind == ITEM_WEAPON || item_container(v->db, it->s.item) || d->equip_slot != EQ_NONE);
}

InvDDOut idd_drop_on(const InvDrag *d, InvTarget t, const InvView *v) {
    InvDDOut none;
    memset(&none, 0, sizeof(none));
    none.action = IDA_CANCEL;
    const InvTarget *s = &d->src;
    // A hotbar tile: bind elsewhere, or unbind anywhere off the hotbar.
    if (s->kind == IT_HOT) {
        if (t.kind == IT_HOT && t.index == s->index) return none;
        InvIntent in = mk(II_BIND, INV_SIDE_SELF, t.kind == IT_HOT ? s->uid : 0, INV_SIDE_SELF, 0);
        in.aux = (u8)(t.kind == IT_HOT ? t.index : s->index);
        return out_intent(in, t);
    }
    // An attachment chip of the inspected weapon: onto a cell (unmount there) or away (first space).
    if (s->kind == IT_ATTACH) {
        InvIntent in = mk(II_UNMOUNT, s->side, s->uid, s->side, INV_AUTO);
        in.aux = (u8)s->index;
        if (t.kind == IT_ATTACH) return none;
        if (t.kind == IT_CELL) {
            in.dst_side = t.side;
            in.dst = t.parent;
            in.loc = t.loc;
            in.x = (u8)DG_MAX(0, t.x);
            in.y = (u8)DG_MAX(0, t.y);
        }
        return out_intent(in, t);
    }
    const InvItem *it = item_at(v, d->side, d->uid);
    if (!it) return none;
    switch (t.kind) {
    case IT_CELL: {
        int x, y, w, h;
        idd_footprint(d, t, v, &x, &y, &w, &h);
        if (x < 0 || y < 0) return out_reject(t, IIR_NO_ROOM);
        InvIntent in = mk(II_MOVE, d->side, d->uid, t.side, t.parent);
        in.loc = t.loc;
        in.x = (u8)x;
        in.y = (u8)y;
        in.rot = d->rot;
        if (d->split && it->s.count > 1) {
            in.count = (u16)(it->s.count / 2);
            InvDDOut o = out_intent(in, t);
            o.action = IDA_SPLIT;
            return o;
        }
        return out_intent(in, t);
    }
    case IT_EQUIP: {
        if (t.side != INV_SIDE_SELF) return out_reject(t, IIR_EQUIP);   // a corpse's slots only give
        if (s->kind == IT_EQUIP && s->side == INV_SIDE_SELF && s->index == t.index) return none;
        InvIntent in = mk(II_EQUIP, d->side, d->uid, INV_SIDE_SELF, 0);
        in.aux = (u8)t.index;
        if (!inv_equip_accepts(t.index, item_def(v->db, it->s.item))) return out_reject(t, IIR_EQUIP);
        return out_intent(in, t);
    }
    case IT_HOT: {
        if (d->side != INV_SIDE_SELF) return out_reject(t, IDR_TAKE_FIRST);
        InvIntent in = mk(II_BIND, INV_SIDE_SELF, d->uid, INV_SIDE_SELF, 0);
        in.aux = (u8)t.index;
        return out_intent(in, t);
    }
    case IT_ATTACH: {
        InvIntent in = mk(II_MOUNT, d->side, d->uid, t.side, t.uid);
        const InvItem *w = item_at(v, t.side, t.uid);
        if (!w) return none;
        int aslot = -1;
        int r = inv_attach_check(v->db, &w->s, it->s.item, &aslot);
        if (r != IIR_OK) return out_reject(t, r);
        if (aslot != t.index) return out_reject(t, IIR_INCOMPATIBLE);
        return out_intent(in, t);
    }
    case IT_DISCARD:
    case IT_OUTSIDE: {
        InvIntent in = mk(II_DISCARD, d->side, d->uid, d->side, 0);
        InvDDOut o = out_intent(in, t);
        if (confirm_discard(it, v)) o.action = IDA_CONFIRM;
        return o;
    }
    default: return none;
    }
}

InvDDOut idd_update(InvDrag *d, const InvDDIn *in, const InvView *v) {
    InvDDOut none;
    memset(&none, 0, sizeof(none));
    if (d->carry) {
        if (in->cancel) { idd_reset(d); return out_act(IDA_CANCEL, in->hover); }
        if (in->rotate) d->rot ^= 1;
        return none;
    }
    switch (d->state) {
    case IDD_IDLE: {
        const InvTarget *h = &in->hover;
        if (in->right_pressed && h->uid && (h->kind == IT_CELL || h->kind == IT_EQUIP || h->kind == IT_HOT)) return out_act(IDA_CONTEXT, *h);
        if (in->double_clicked && h->uid && (h->kind == IT_CELL || h->kind == IT_EQUIP)) return out_act(IDA_OPEN, *h);
        if (!in->pressed || !h->uid) return none;
        if (h->kind != IT_CELL && h->kind != IT_EQUIP && h->kind != IT_HOT && h->kind != IT_ATTACH) return none;
        d->state = IDD_PRESSED;
        d->src = *h;
        d->side = h->kind == IT_HOT ? INV_SIDE_SELF : h->side;
        d->uid = h->uid;
        d->press_pos = in->mouse;
        d->split = in->shift;
        d->rot = 0;
        d->grab_x = d->grab_y = 0;
        const InvItem *it = item_at(v, d->side, d->uid);
        if (it && h->kind == IT_CELL) {
            d->rot = (it->flags & INVF_ROT) ? 1 : 0;
            int dx = h->x - it->x, dy = h->y - it->y;   // in the item's current orientation
            if (d->rot) { d->grab_x = (i16)dy; d->grab_y = (i16)dx; }
            else { d->grab_x = (i16)dx; d->grab_y = (i16)dy; }
        }
        return none;
    }
    case IDD_PRESSED: {
        if (in->released) {
            InvTarget at = d->src;
            idd_reset(d);
            if (at.kind == IT_CELL || at.kind == IT_EQUIP) {
                if (in->ctrl) return out_intent(mk(II_QUICK_MOVE, at.side, at.uid, 0, 0), at);
                if (in->alt) {
                    InvIntent e = mk(II_EQUIP, at.side, at.uid, INV_SIDE_SELF, 0);
                    e.aux = 255;
                    return out_intent(e, at);
                }
            }
            return out_act(IDA_SELECT, at);
        }
        float dx = in->mouse.x - d->press_pos.x, dy = in->mouse.y - d->press_pos.y;
        if (dx * dx + dy * dy >= in->threshold * in->threshold) {
            d->state = IDD_DRAGGING;
            return out_act(IDA_PICKED, d->src);
        }
        return none;
    }
    case IDD_DRAGGING: {
        if (in->cancel) { idd_reset(d); return out_act(IDA_CANCEL, in->hover); }
        if (in->rotate) d->rot ^= 1;
        if (!in->released) return none;
        InvDDOut o = idd_drop_on(d, in->hover, v);
        idd_reset(d);
        return o;
    }
    default: idd_reset(d); return none;
    }
}

InvDDOut idd_carry_accept(InvDrag *d, InvTarget focus, const InvView *v, bool split) {
    InvDDOut none;
    memset(&none, 0, sizeof(none));
    if (!d->carry) {
        if (!focus.uid || (focus.kind != IT_CELL && focus.kind != IT_EQUIP && focus.kind != IT_HOT && focus.kind != IT_ATTACH)) return out_act(IDA_SELECT, focus);
        InvDDIn in;
        memset(&in, 0, sizeof(in));
        in.pressed = true;
        in.hover = focus;
        in.shift = split;
        idd_update(d, &in, v);
        d->state = IDD_DRAGGING;
        d->carry = true;
        return out_act(IDA_PICKED, focus);
    }
    InvDDOut o = idd_drop_on(d, focus, v);
    idd_reset(d);
    return o;
}
