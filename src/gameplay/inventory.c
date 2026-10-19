#include "inventory.h"
#include "attachments.h"
#include <string.h>

void inv_init(Inventory *inv, const ItemDB *db) {
    u32 rev = inv->revision;
    memset(inv, 0, sizeof(*inv));
    inv_tree_init(&inv->t, db ? container_find(db, "body") : 0);
    if (db) inv_ensure_pack(&inv->t, db);
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

InvCtx inv_ctx(Inventory *inv, const ItemDB *db, InvTree *loot) { return inv_ctx_make(db, &inv->t, &inv->hands, loot); }

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

u16 inv_armor_plate(const Inventory *inv, const ItemDB *db) {
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *it = &inv->t.items[i];
        if (!inv_alive(it) || it->loc == INV_LOC_NONE) continue;
        const ItemDef *d = item_def(db, it->s.item);
        if (d->kind == ITEM_CONSUMABLE && d->armor > 0 && d->heal <= 0) return it->s.item;
    }
    return 0;
}

int inv_remove(Inventory *inv, u16 item, int count) {
    int removed = 0;
    // Stacks not in the hands first so the held one stays.
    for (int pass = 0; pass < 2; pass++)
        for (int i = INV_MAX_ITEMS - 1; i >= 0 && removed < count; i--) {
            InvItem *it = &inv->t.items[i];
            if (!inv_alive(it) || it->s.item != item || (inv->hands == it->uid) != (pass == 1)) continue;
            int take = DG_MIN((int)it->s.count, count - removed);
            it->s.count = (u16)(it->s.count - take);
            removed += take;
            if (!it->s.count) memset(it, 0, sizeof(*it));
        }
    if (removed) {
        if (inv->hands && !inv_get(&inv->t, inv->hands)) inv->hands = 0;
        inv->revision++;
    }
    return removed;
}

static InvItem *sel_item(Inventory *inv, int k) {
    if (k >= 0 && k < INV_SEL_WEAPONS) return inv_equipped(&inv->t, k);
    if (k == INV_SEL_HANDS) return inv_get(&inv->t, inv->hands);
    return NULL;
}

InvSlot *inv_sel(Inventory *inv, int k) {
    InvItem *it = sel_item(inv, k);
    if (it) return &it->s;
    memset(&inv->none, 0, sizeof(inv->none));
    return &inv->none;
}

const InvSlot *inv_sel_c(const Inventory *inv, int k) {
    static const InvSlot empty;
    const InvItem *it = sel_item((Inventory *)inv, k);
    return it ? &it->s : &empty;
}

int inv_sel_find(const Inventory *inv, u16 item) {
    for (int k = 0; k < INV_SEL_COUNT; k++) if (item && inv_sel_c(inv, k)->item == item) return k;
    return -1;
}

void inv_use_one(const ItemDB *db, InvSlot *s) {
    const ItemDef *d = item_def(db, s->item);
    if (d->use_type == INV_USE_CHARGES && d->charges > 1 && ++s->ammo < d->charges) return;
    s->ammo = 0;
    if (s->count <= 1) memset(s, 0, sizeof(*s));
    else s->count--;
}

InvSlot *inv_dev_sel(Inventory *inv, const ItemDB *db, int k) {
    k = DG_CLAMP(k, 0, INV_SEL_COUNT - 1);
    InvItem *it = sel_item(inv, k);
    if (it) return &it->s;
    InvSlot ph = { 1, 1, 0, { 0 }, 0 };   // a placeholder payload: the caller writes the real one
    it = inv_alloc(&inv->t, db, &ph);
    if (!it) { memset(&inv->none, 0, sizeof(inv->none)); return &inv->none; }
    if (k < INV_SEL_WEAPONS) it->loc = (u8)(INV_LOC_EQUIP + k);
    else {
        it->loc = INV_LOC_NONE;
        inv->hands = it->uid;
    }
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
