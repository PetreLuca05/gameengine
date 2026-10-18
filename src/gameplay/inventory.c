#include "inventory.h"
#include "attachments.h"
#include <string.h>

static InvOrder g_order;

void inv_set_order(InvOrder order) { g_order = order; }
InvOrder inv_get_order(void) { return g_order.n ? g_order : inv_order_default(); }

void inv_init(Inventory *inv, const ItemDB *db) {
    u32 rev = inv->revision;
    memset(inv, 0, sizeof(*inv));
    inv_tree_init(&inv->t, db ? container_find(db, "body") : 0);
    inv->revision = rev + 1;
}

// Without a database: root 0 is the body (data/containers.json lists it first).
void inv_clear(Inventory *inv) {
    i16 root = inv->t.root;
    u32 rev = inv->revision;
    memset(inv, 0, sizeof(*inv));
    inv_tree_init(&inv->t, root);
    inv->revision = rev + 1;
}

InvCtx inv_ctx(Inventory *inv, const ItemDB *db, InvTree *loot) {
    InvCtx c = inv_ctx_make(db, &inv->t, inv->quick, INV_HOTBAR, loot);
    c.order = inv_get_order();
    return c;
}

int inv_add(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo) { return inv_add_att(inv, db, item, count, ammo, NULL); }

int inv_add_att(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo, const u8 *att) {
    if (item == ITEM_NONE || item >= db->item_count || count <= 0) return count;
    InvSlot s;
    memset(&s, 0, sizeof(s));
    s.item = item;
    s.count = (u16)DG_MIN(count, 65535);
    s.ammo = ammo;
    if (att) memcpy(s.att, att, ATT_SLOTS);
    else attach_defaults(item_weapon(db, item), s.att);
    InvCtx c = inv_ctx(inv, db, NULL);
    int left = inv_give(&c, &s);
    if (left != count) inv->revision++;
    return left;
}

int inv_count(const Inventory *inv, u16 item) {
    int c = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *it = &inv->t.items[i];
        if (inv_alive(it) && it->s.item == item) c += it->s.count;
    }
    return c;
}

bool inv_has(const Inventory *inv, u16 item) { return inv_count(inv, item) > 0; }

static bool bound(const Inventory *inv, u16 uid) {
    for (int k = 0; k < INV_HOTBAR; k++) if (inv->quick[k] == uid) return true;
    return false;
}

int inv_remove(Inventory *inv, u16 item, int count) {
    int removed = 0;
    // Unbound stacks first so the hotbar keeps its items.
    for (int pass = 0; pass < 2; pass++)
        for (int i = INV_MAX_ITEMS - 1; i >= 0 && removed < count; i--) {
            InvItem *it = &inv->t.items[i];
            if (!inv_alive(it) || it->s.item != item || bound(inv, it->uid) != (pass == 1)) continue;
            int take = DG_MIN((int)it->s.count, count - removed);
            it->s.count = (u16)(it->s.count - take);
            removed += take;
            if (!it->s.count) memset(it, 0, sizeof(*it));
        }
    if (removed) inv->revision++;
    return removed;
}

InvSlot *inv_hot(Inventory *inv, int k) {
    InvItem *it = k >= 0 && k < INV_HOTBAR ? inv_get(&inv->t, inv->quick[k]) : NULL;
    if (it) return &it->s;
    memset(&inv->none, 0, sizeof(inv->none));
    return &inv->none;
}

const InvSlot *inv_hot_c(const Inventory *inv, int k) {
    static const InvSlot empty;
    const InvItem *it = k >= 0 && k < INV_HOTBAR ? inv_get_c(&inv->t, inv->quick[k]) : NULL;
    return it ? &it->s : &empty;
}

int inv_hot_find(const Inventory *inv, u16 item) {
    for (int k = 0; k < INV_HOTBAR; k++) if (item && inv_hot_c(inv, k)->item == item) return k;
    return -1;
}

void inv_use_one(const ItemDB *db, InvSlot *s) {
    const ItemDef *d = item_def(db, s->item);
    if (d->use_type == INV_USE_CHARGES && d->charges > 1 && ++s->ammo < d->charges) return;
    s->ammo = 0;
    if (s->count <= 1) memset(s, 0, sizeof(*s));
    else s->count--;
}

InvSlot *inv_dev_hot(Inventory *inv, const ItemDB *db, int k) {
    k = DG_CLAMP(k, 0, INV_HOTBAR - 1);
    InvItem *it = inv_get(&inv->t, inv->quick[k]);
    if (it) return &it->s;
    InvSlot ph = { 1, 1, 0, { 0 }, 0 };   // a placeholder payload: the caller writes the real one
    it = inv_alloc(&inv->t, db, &ph);
    if (!it) { memset(&inv->none, 0, sizeof(inv->none)); return &inv->none; }
    it->loc = INV_LOC_NONE;
    inv->quick[k] = it->uid;
    return &it->s;
}

InvItem *inv_find(Inventory *inv, u16 item) {
    for (int i = 0; i < INV_MAX_ITEMS; i++)
        if (inv_alive(&inv->t.items[i]) && inv->t.items[i].s.item == item) return &inv->t.items[i];
    return NULL;
}

InvItem *inv_item_of(Inventory *inv, const InvSlot *s) {
    const u8 *p = (const u8 *)s, *b = (const u8 *)inv->t.items;
    if (p < b || p >= b + sizeof(inv->t.items)) return NULL;
    InvItem *it = &inv->t.items[(size_t)(p - b) / sizeof(InvItem)];
    return inv_alive(it) ? it : NULL;
}
