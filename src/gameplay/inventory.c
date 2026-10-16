#include "inventory.h"
#include "attachments.h"

int slots_add(InvSlot *slots, int n, const ItemDB *db, u16 item, int count, i16 ammo, int prefer_first, int prefer_count) {
    return slots_add_att(slots, n, db, item, count, ammo, NULL, prefer_first, prefer_count);
}

int slots_add_att(InvSlot *slots, int n, const ItemDB *db, u16 item, int count, i16 ammo, const u8 *att, int prefer_first, int prefer_count) {
    const ItemDef *d = item_def(db, item);
    u8 a[ATT_SLOTS];
    if (att) memcpy(a, att, ATT_SLOTS);
    else attach_defaults(item_weapon(db, item), a);
    int max_stack = DG_MAX(1, d->max_stack);
    // Merge into existing stacks.
    if (max_stack > 1) {
        for (int i = 0; i < n && count > 0; i++) {
            if (slots[i].item != item || slots[i].count >= max_stack) continue;
            int add = DG_MIN(count, max_stack - (int)slots[i].count);
            slots[i].count = (u16)(slots[i].count + add);
            count -= add;
        }
    }
    // Empty slots: preferred range first, then the rest.
    for (int pass = 0; pass < 2 && count > 0; pass++) {
        for (int i = 0; i < n && count > 0; i++) {
            bool in_pref = i >= prefer_first && i < prefer_first + prefer_count;
            if ((pass == 0) != in_pref) continue;
            if (slots[i].item != ITEM_NONE) continue;
            int add = DG_MIN(count, max_stack);
            slots[i].item = item;
            slots[i].count = (u16)add;
            slots[i].ammo = ammo;
            memcpy(slots[i].att, a, ATT_SLOTS);
            count -= add;
        }
    }
    return count;
}

int inv_add(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo) { return inv_add_att(inv, db, item, count, ammo, NULL); }

int inv_add_att(Inventory *inv, const ItemDB *db, u16 item, int count, i16 ammo, const u8 *att) {
    if (item == ITEM_NONE || count <= 0) return count;
    const ItemDef *d = item_def(db, item);
    bool hotbar_first = d->kind == ITEM_WEAPON || d->kind == ITEM_CONSUMABLE || d->camera;   // the phone is equipped from the hotbar
    int left = slots_add_att(inv->slots, INV_SLOTS, db, item, count, ammo, att, hotbar_first ? 0 : INV_HOTBAR,
                             hotbar_first ? INV_HOTBAR : INV_BACKPACK);
    if (left != count) inv->revision++;
    return left;
}

int inv_count(const Inventory *inv, u16 item) {
    int c = 0;
    for (int i = 0; i < INV_SLOTS; i++) if (inv->slots[i].item == item) c += inv->slots[i].count;
    return c;
}

bool inv_has(const Inventory *inv, u16 item) { return inv_count(inv, item) > 0; }

int inv_remove(Inventory *inv, u16 item, int count) {
    int removed = 0;
    // Take from the backpack first so the hotbar keeps its items.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = INV_SLOTS - 1; i >= 0 && removed < count; i--) {
            bool hot = i < INV_HOTBAR;
            if ((pass == 0) == hot) continue;
            InvSlot *s = &inv->slots[i];
            if (s->item != item) continue;
            int take = DG_MIN((int)s->count, count - removed);
            s->count = (u16)(s->count - take);
            removed += take;
            if (s->count == 0) memset(s, 0, sizeof(*s));
        }
    }
    if (removed) inv->revision++;
    return removed;
}

void inv_move(Inventory *inv, const ItemDB *db, int from, int to) {
    if (from < 0 || to < 0 || from >= INV_SLOTS || to >= INV_SLOTS || from == to) return;
    InvSlot *a = &inv->slots[from], *b = &inv->slots[to];
    if (a->item == ITEM_NONE) return;
    if (a->item == b->item) {
        int max_stack = DG_MAX(1, item_def(db, a->item)->max_stack);
        int move = DG_MIN((int)a->count, max_stack - (int)b->count);
        if (move > 0) {
            b->count = (u16)(b->count + move);
            a->count = (u16)(a->count - move);
            if (a->count == 0) memset(a, 0, sizeof(*a));
            inv->revision++;
            return;
        }
    }
    InvSlot t = *a;
    *a = *b;
    *b = t;
    inv->revision++;
}

void inv_clear(Inventory *inv) {
    memset(inv->slots, 0, sizeof(inv->slots));
    inv->revision++;
}
