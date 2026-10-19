#include "inv_tree.h"
#include <string.h>

void inv_tree_init(InvTree *t, int root_def) {
    memset(t, 0, sizeof(*t));
    t->next_uid = 1;
    t->root = (i16)root_def;
}

InvItem *inv_get(InvTree *t, u16 uid) {
    if (!uid) return NULL;
    for (int i = 0; i < INV_MAX_ITEMS; i++)
        if (t->items[i].uid == uid) return inv_alive(&t->items[i]) ? &t->items[i] : NULL;
    return NULL;
}

const InvItem *inv_get_c(const InvTree *t, u16 uid) { return inv_get((InvTree *)t, uid); }

int inv_live_count(const InvTree *t) {
    int n = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) n += inv_alive(&t->items[i]);
    return n;
}

int inv_free_count(const InvTree *t) { return INV_MAX_ITEMS - inv_live_count(t); }

static bool uid_used(const InvTree *t, u16 uid) {
    for (int i = 0; i < INV_MAX_ITEMS; i++) if (t->items[i].uid == uid && inv_alive(&t->items[i])) return true;
    return false;
}

InvItem *inv_alloc(InvTree *t, const ItemDB *db, const InvSlot *s) {
    InvItem *it = NULL;
    for (int i = 0; i < INV_MAX_ITEMS && !it; i++) if (!inv_alive(&t->items[i])) it = &t->items[i];
    if (!it) return NULL;
    // Monotonic uids (wrapping past 0 and the ones in use): a client predicting the same intents
    // from the same tree allocates the same uids as the host.
    u16 uid = t->next_uid ? t->next_uid : 1;
    while (uid_used(t, uid)) uid = (u16)(uid + 1 ? uid + 1 : 1);
    t->next_uid = (u16)(uid + 1 ? uid + 1 : 1);
    memset(it, 0, sizeof(*it));
    it->s = *s;
    it->uid = uid;
    it->dur = item_def(db, s->item)->durability;
    return it;
}

// ------------------------------------------------------------------ defs

const InvContainerDef *inv_root_def(const InvTree *t, const ItemDB *db) {
    static InvContainerDef body;
    if (t->root >= 0 && t->root < db->container_count) return &db->containers[t->root];
    if (!body.equip) {   // no data/containers.json: a body (equipment slots, no grid of its own)
        dg_strcpy(body.id, sizeof(body.id), "body");
        body.equip = true;
    }
    return &body;
}

InvItem *inv_pack(InvTree *t) { return inv_equipped(t, EQ_BACKPACK); }
const InvItem *inv_pack_c(const InvTree *t) { return inv_equipped_c(t, EQ_BACKPACK); }

u16 inv_grid(const InvTree *t, const ItemDB *db) {
    if (!inv_root_def(t, db)->equip) return 0;
    const InvItem *pk = inv_pack_c(t);
    return pk ? pk->uid : INV_NO_GRID;
}

void inv_grid_size(const InvTree *t, const ItemDB *db, int *cols, int *rows) {
    *cols = *rows = 0;
    u16 g = inv_grid(t, db);
    const InvContainerDef *cd = g == INV_NO_GRID ? NULL : inv_parent_def(t, db, g);
    for (int i = 0; cd && i < cd->comp_count; i++) {
        *cols = DG_MAX(*cols, cd->comp[i].at_x + cd->comp[i].w);
        *rows = DG_MAX(*rows, cd->comp[i].at_y + cd->comp[i].h);
    }
}

int inv_child_count(const InvTree *t, u16 uid) {
    int n = 0;
    for (int i = 0; uid && i < INV_MAX_ITEMS; i++) n += inv_alive(&t->items[i]) && t->items[i].parent == uid;
    return n;
}

const InvContainerDef *inv_parent_def(const InvTree *t, const ItemDB *db, u16 parent) {
    if (!parent) return inv_root_def(t, db);
    const InvItem *p = inv_get_c(t, parent);
    return p ? item_container(db, p->s.item) : NULL;
}

void inv_base_size(const ItemDB *db, const InvSlot *s, u8 flags, int *w, int *h) {
    const ItemDef *d = item_def(db, s->item);
    int gw = DG_MAX(1, (int)d->grid_w), gh = DG_MAX(1, (int)d->grid_h);
    if (d->kind == ITEM_WEAPON)
        for (int k = 0; k < ATT_SLOTS; k++) {
            int a = s->att[k];
            if (a <= 0 || a > db->attachment_count) continue;
            gw += db->attachments[a].grid_dw;
            gh += db->attachments[a].grid_dh;
        }
    if ((flags & INVF_FOLDED) && d->fold_dw) gw -= d->fold_dw;
    *w = DG_CLAMP(gw, 1, 16);
    *h = DG_CLAMP(gh, 1, 16);
}

void inv_item_size(const ItemDB *db, const InvItem *it, int *w, int *h) {
    int bw, bh;
    inv_base_size(db, &it->s, it->flags, &bw, &bh);
    if (it->flags & INVF_ROT) { *w = bh; *h = bw; }
    else { *w = bw; *h = bh; }
}

// ------------------------------------------------------------------ structure

bool inv_is_ancestor(const InvTree *t, u16 anc, u16 uid) {
    for (int guard = 0; uid && guard < INV_MAX_ITEMS + 1; guard++) {
        if (uid == anc) return true;
        const InvItem *it = inv_get_c(t, uid);
        if (!it) return false;
        uid = it->parent;
    }
    return false;
}

int inv_subtree(const InvTree *t, u16 uid, int *idx, int max) {
    int n = 0;
    for (int i = 0; i < INV_MAX_ITEMS && n < max; i++)
        if (t->items[i].uid == uid && inv_alive(&t->items[i])) idx[n++] = i;
    // Breadth first: children of everything collected so far.
    for (int k = 0; k < n; k++) {
        u16 p = t->items[idx[k]].uid;
        for (int i = 0; i < INV_MAX_ITEMS && n < max; i++)
            if (inv_alive(&t->items[i]) && t->items[i].parent == p) idx[n++] = i;
    }
    return n;
}

void inv_free_subtree(InvTree *t, u16 uid) {
    int idx[INV_MAX_ITEMS];
    int n = inv_subtree(t, uid, idx, INV_MAX_ITEMS);
    for (int k = 0; k < n; k++) memset(&t->items[idx[k]], 0, sizeof(InvItem));
}

void inv_normalize(InvTree *t, const ItemDB *db) {
    for (int i = 0; i < INV_MAX_ITEMS; i++)
        if (!inv_alive(&t->items[i])) memset(&t->items[i], 0, sizeof(InvItem));
    // Orphans: only the tree's grid (the worn backpack) holds items.
    u16 g = inv_grid(t, db);
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *it = &t->items[i];
        if (inv_alive(it) && it->parent && it->parent != g) memset(it, 0, sizeof(*it));
    }
}

// ------------------------------------------------------------------ space

int inv_cells_used(const InvTree *t, const ItemDB *db, u16 parent) {
    int used = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *it = &t->items[i];
        if (!inv_alive(it) || it->parent != parent || it->loc >= INV_LOC_EQUIP) continue;
        int w, h;
        inv_item_size(db, it, &w, &h);
        used += w * h;
    }
    return used;
}

InvItem *inv_at(InvTree *t, const ItemDB *db, u16 parent, int loc, int x, int y) {
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *it = &t->items[i];
        if (!inv_alive(it) || it->parent != parent || it->loc != loc) continue;
        int w, h;
        inv_item_size(db, it, &w, &h);
        if (x >= it->x && x < it->x + w && y >= it->y && y < it->y + h) return it;
    }
    return NULL;
}

InvItem *inv_equipped(InvTree *t, int slot) {
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *it = &t->items[i];
        if (inv_alive(it) && !it->parent && it->loc == INV_LOC_EQUIP + slot) return it;
    }
    return NULL;
}

const InvItem *inv_equipped_c(const InvTree *t, int slot) { return inv_equipped((InvTree *)t, slot); }
