#include "inv_wire.h"
#include <string.h>

static u8 *w16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); return p + 2; }
static u8 *w32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); return p + 4; }
static u16 r16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 r32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

// tree: root i16, next_uid u16, count u8, then per item: item, count, ammo, att[5], uid, parent,
// loc, x, y, flags, dur (21 bytes).
int inv_wire_write_tree(const InvTree *t, u8 *out, int max) {
    int n = inv_live_count(t);
    int size = 5 + n * INV_WIRE_ITEM;
    if (size > max) return 0;
    u8 *p = out;
    p = w16(p, (u16)t->root);
    p = w16(p, t->next_uid);
    *p++ = (u8)n;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *it = &t->items[i];
        if (!inv_alive(it)) continue;
        p = w16(p, it->s.item);
        p = w16(p, it->s.count);
        p = w16(p, (u16)it->s.ammo);
        memcpy(p, it->s.att, ATT_SLOTS);
        p += ATT_SLOTS;
        p = w16(p, it->uid);
        p = w16(p, it->parent);
        *p++ = it->loc;
        *p++ = it->x;
        *p++ = it->y;
        *p++ = it->flags;
        p = w16(p, it->dur);
    }
    return (int)(p - out);
}

int inv_wire_read_tree(InvTree *t, const u8 *in, int size) {
    if (size < 5) return 0;
    int n = in[4];
    if (n > INV_MAX_ITEMS || size < 5 + n * INV_WIRE_ITEM) return 0;
    memset(t, 0, sizeof(*t));
    t->root = (i16)r16(in);
    t->next_uid = r16(in + 2);
    const u8 *p = in + 5;
    for (int i = 0; i < n; i++) {
        InvItem *it = &t->items[i];
        it->s.item = r16(p);
        it->s.count = r16(p + 2);
        it->s.ammo = (i16)r16(p + 4);
        memcpy(it->s.att, p + 6, ATT_SLOTS);
        it->uid = r16(p + 11);
        it->parent = r16(p + 13);
        it->loc = p[15];
        it->x = p[16];
        it->y = p[17];
        it->flags = p[18];
        it->dur = r16(p + 19);
        p += INV_WIRE_ITEM;
    }
    return (int)(p - in);
}

void inv_wire_write_intent(const InvIntent *in, u8 out[INV_WIRE_INTENT]) {
    u8 *p = out;
    *p++ = in->type;
    *p++ = (u8)(in->src_side | (in->dst_side << 4));
    *p++ = in->loc;
    p = w16(p, in->src);
    p = w16(p, in->dst);
    *p++ = in->x;
    *p++ = in->y;
    *p++ = in->rot;
    *p++ = in->aux;
    p = w16(p, in->count);
    p = w32(p, in->seq);
    *p++ = 0;   // reserved
}

void inv_wire_read_intent(InvIntent *in, const u8 b[INV_WIRE_INTENT]) {
    memset(in, 0, sizeof(*in));
    in->type = b[0];
    in->src_side = (u8)(b[1] & 15);
    in->dst_side = (u8)(b[1] >> 4);
    in->loc = b[2];
    in->src = r16(b + 3);
    in->dst = r16(b + 5);
    in->x = b[7];
    in->y = b[8];
    in->rot = b[9];
    in->aux = b[10];
    in->count = r16(b + 11);
    in->seq = r32(b + 13);
}
