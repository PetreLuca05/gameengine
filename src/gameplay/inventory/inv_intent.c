#include "inv_intent.h"
#include "gameplay/attachments.h"
#include <stdlib.h>
#include <string.h>

// Sides inside an intent: the two trees, a scratch "hand" holding what a pickup adds and the
// effect's drop tree (what a discard leaves on the ground, InvEffect.dropped).
enum { SIDE_HAND = INV_SIDES, SIDE_DROP, WORK_SIDES };

typedef struct Work {
    InvTree *t[WORK_SIDES];
    u16 hands;
    bool has_hands;
    const ItemDB *db;
    InvEffect *fx;
    u16 last_uid;      // the last transfer's new uid
    int last_side;
} Work;

static InvTree g_scratch[WORK_SIDES];

const char *inv_intent_name(int type) {
    static const char *n[II_COUNT] = { "none", "move", "rotate", "equip", "quick_move", "split", "discard", "use",
                                       "mount", "unmount", "fold", "unload", "hold", "take_all", "compact" };
    return type >= 0 && type < II_COUNT ? n[type] : "?";
}

int inv_intent_from_name(const char *s) {
    for (int i = 1; i < II_COUNT; i++) if (!strcmp(s, inv_intent_name(i))) return i;
    return II_NONE;
}

InvCtx inv_ctx_make(const ItemDB *db, InvTree *self, u16 *hands, InvTree *loot) {
    InvCtx c;
    memset(&c, 0, sizeof(c));
    c.tree[INV_SIDE_SELF] = self;
    c.tree[INV_SIDE_LOOT] = loot;
    c.hands = hands;
    c.db = db;
    c.loot_reachable = true;
    return c;
}

// ------------------------------------------------------------------ helpers

static int max_stack(const Work *W, u16 item) { return DG_MAX(1, item_def(W->db, item)->max_stack); }

static InvPlace place_of(const InvItem *it) {
    InvPlace p = { it->parent, it->loc, it->x, it->y, (u8)((it->flags & INVF_ROT) ? 1 : 0) };
    return p;
}

static void set_place(InvItem *it, InvPlace p) {
    it->parent = p.parent;
    it->loc = p.loc;
    it->x = p.x;
    it->y = p.y;
    it->flags = (u8)((it->flags & ~INVF_ROT) | (p.rot ? INVF_ROT : 0));
}

static bool is_default_pack(const Work *W, const InvItem *it) { return it->s.item && it->s.item == W->db->default_backpack; }
static bool is_worn(const InvItem *it, int slot) { return !it->parent && it->loc == INV_LOC_EQUIP + slot; }

// Placement check of a (living on side as) at p on side d: a foreign item has no uid there, so its
// contents are checked here (a full backpack never leaves its back).
static int check(Work *W, int d, const InvItem *a, int as, InvPlace p, u16 ignore, u16 *blocker) {
    InvItem tmp = *a;
    if (as != d) {
        if (item_container(W->db, a->s.item) && inv_child_count(W->t[as], a->uid)) return IIR_NOT_EMPTY;
        tmp.uid = 0;
    }
    return inv_check_place(W->t[d], W->db, &tmp, p, ignore, blocker);
}

// Moves uid (with its contents) from side s to side d at p (checked by the caller). The top keeps
// its uid inside a tree; across trees every item gets a new uid. Returns the top's uid (0: pool full).
static u16 transfer(Work *W, int s, u16 uid, int d, InvPlace p) {
    InvTree *S = W->t[s], *D = W->t[d];
    if (s == d) {
        InvItem *it = inv_get(S, uid);
        if (!it) return 0;
        set_place(it, p);
        W->last_uid = uid;
        W->last_side = d;
        return uid;
    }
    int idx[INV_MAX_ITEMS];
    int n = inv_subtree(S, uid, idx, INV_MAX_ITEMS);
    if (!n || inv_free_count(D) < n) return 0;
    u16 old_uid[INV_MAX_ITEMS], new_uid[INV_MAX_ITEMS];
    for (int k = 0; k < n; k++) {
        const InvItem *src = &S->items[idx[k]];
        InvItem *dst = inv_alloc(D, W->db, &src->s);
        old_uid[k] = src->uid;
        new_uid[k] = dst->uid;
        dst->dur = src->dur;
        dst->flags = src->flags;
        dst->loc = src->loc;
        dst->x = src->x;
        dst->y = src->y;
        dst->parent = 0;
        if (k == 0) set_place(dst, p);
        else
            for (int j = 0; j < k; j++) if (old_uid[j] == src->parent) dst->parent = new_uid[j];
    }
    for (int k = 0; k < n; k++) memset(&S->items[idx[k]], 0, sizeof(InvItem));
    W->last_uid = new_uid[0];
    W->last_side = d;
    return new_uid[0];
}

// A part of a stack (n of a) as a new instance at p on side d (checked by the caller).
static u16 place_part(Work *W, InvItem *a, int d, InvPlace p, int n) {
    InvSlot part = a->s;
    part.count = (u16)n;
    u16 dur = a->dur;
    InvItem *it = inv_alloc(W->t[d], W->db, &part);
    if (!it) return 0;
    it->dur = dur;
    set_place(it, p);
    a->s.count = (u16)(a->s.count - n);   // n < a's count (callers): a stays alive
    W->last_uid = it->uid;
    W->last_side = d;
    return it->uid;
}

// Moves up to want of a into b (same def); returns how many moved. An emptied a is freed.
static int merge(Work *W, InvItem *a, InvItem *b, int want) {
    if (a == b || a->s.item != b->s.item) return 0;
    int room = max_stack(W, b->s.item) - (int)b->s.count;
    int n = DG_MIN(want, DG_MIN(room, (int)a->s.count));
    if (n <= 0) return 0;
    b->s.count = (u16)(b->s.count + n);
    a->s.count = (u16)(a->s.count - n);
    if (!a->s.count) memset(a, 0, sizeof(*a));
    return n;
}

// Merges a's stack into same-def stacks in side d's grid (a may be on another side). Returns moved.
static int merge_anywhere(Work *W, InvItem *a, int d) {
    if (max_stack(W, a->s.item) <= 1) return 0;
    int moved = 0;
    InvTree *D = W->t[d];
    u16 g = inv_grid(D, W->db);
    for (int i = 0; i < INV_MAX_ITEMS && inv_alive(a); i++) {
        InvItem *b = &D->items[i];
        if (!inv_alive(b) || b == a || b->s.item != a->s.item || b->parent != g || b->loc >= INV_LOC_EQUIP) continue;
        moved += merge(W, a, b, a->s.count);
    }
    return moved;
}

// A free place in side d's grid for a (living on side as).
static bool find_space(Work *W, int d, const InvItem *a, int as, InvPlace *out) {
    InvItem tmp = *a;
    if (as != d) tmp.uid = 0;
    return inv_find_grid_space(W->t[d], W->db, &tmp, out);
}

// ------------------------------------------------------------------ backpacks

// The children of backpack from (side) into backpack to's grid (both on side, to worn): every
// item in its own cell when that is inside the new grid and free, the rest into the first free
// cells, largest first. All or nothing (the intent fails with IIR_NO_ROOM).
static const ItemDB *g_cmp_db;
static const InvTree *g_cmp_t;
static int area_cmp(const void *pa, const void *pb) {
    const InvItem *a = &g_cmp_t->items[*(const int *)pa], *b = &g_cmp_t->items[*(const int *)pb];
    int wa, ha, wb, hb;
    inv_item_size(g_cmp_db, a, &wa, &ha);
    inv_item_size(g_cmp_db, b, &wb, &hb);
    if (wa * ha != wb * hb) return wb * hb - wa * ha;
    if (a->y != b->y) return (int)a->y - (int)b->y;
    if (a->x != b->x) return (int)a->x - (int)b->x;
    return (int)a->uid - (int)b->uid;
}

static void sort_by_area(const ItemDB *db, const InvTree *t, int *idx, int n) {
    g_cmp_db = db;
    g_cmp_t = t;
    qsort(idx, (size_t)n, sizeof(int), area_cmp);
}

static int move_contents(Work *W, int side, u16 from, u16 to) {
    InvTree *T = W->t[side];
    int idx[INV_MAX_ITEMS], n = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++)
        if (inv_alive(&T->items[i]) && T->items[i].parent == from) idx[n++] = i;
    for (int k = 0; k < n; k++) T->items[idx[k]].loc = INV_LOC_NONE;   // detached: none blocks another
    int left[INV_MAX_ITEMS], ln = 0;
    for (int k = 0; k < n; k++) {
        InvItem *it = &T->items[idx[k]];
        InvPlace p = { to, 0, it->x, it->y, (u8)((it->flags & INVF_ROT) ? 1 : 0) };
        if (inv_check_place(T, W->db, it, p, 0, NULL) == IIR_OK) set_place(it, p);
        else left[ln++] = idx[k];
    }
    sort_by_area(W->db, T, left, ln);
    for (int k = 0; k < ln; k++) {
        InvItem *it = &T->items[left[k]];
        InvItem tmp = *it;
        tmp.flags &= (u8)~INVF_ROT;
        InvPlace p;
        if (!inv_find_space(T, W->db, &tmp, to, &p)) return IIR_NO_ROOM;
        set_place(it, p);
    }
    return IIR_OK;
}

// Takes the worn backpack a off side s's back: its contents move into a new default bag (the bag
// everyone wears when they wear no other). a stays detached (loc INV_LOC_NONE) for the caller to
// place. *grid: the new bag's uid. Another tree's worn pack (a corpse's) only comes off empty.
static int pack_off(Work *W, int s, InvItem *a, u16 *grid) {
    if (is_default_pack(W, a)) return IIR_DEFAULT_PACK;
    InvTree *T = W->t[s];
    *grid = INV_NO_GRID;
    if (s != INV_SIDE_SELF || !W->db->default_backpack) {
        if (inv_child_count(T, a->uid)) return IIR_NOT_EMPTY;
        a->loc = INV_LOC_NONE;
        return IIR_OK;
    }
    u16 au = a->uid;
    a->loc = INV_LOC_NONE;
    InvSlot ds = { W->db->default_backpack, 1, 0, { 0 }, 0 };
    InvItem *np = inv_alloc(T, W->db, &ds);
    if (!np) return IIR_FULL;
    np->loc = INV_LOC_EQUIP + EQ_BACKPACK;
    *grid = np->uid;
    return move_contents(W, s, au, np->uid);
}

// Puts backpack a (side s; empty unless it is self's grid's owner) on self's back: the worn pack's
// contents move into it, the old pack goes where a was (or into the new grid); the default bag is
// simply gone (it comes back when a pack comes off).
static int equip_pack(Work *W, int s, InvItem *a) {
    InvTree *T = W->t[INV_SIDE_SELF];
    if (is_default_pack(W, a)) return IIR_DEFAULT_PACK;
    InvItem *old = inv_pack(T);
    if (s == INV_SIDE_SELF && old == a) return IIR_NOOP;
    if (inv_child_count(W->t[s], a->uid)) return IIR_NOT_EMPTY;
    InvPlace from = place_of(a);
    u16 ou = old ? old->uid : 0;
    if (old) old->loc = INV_LOC_NONE;   // off the back: the slot is free
    InvPlace p = { 0, (u8)(INV_LOC_EQUIP + EQ_BACKPACK), 0, 0, 0 };
    int r = check(W, INV_SIDE_SELF, a, s, p, 0, NULL);
    if (r != IIR_OK) return r;
    u16 nu = transfer(W, s, a->uid, INV_SIDE_SELF, p);
    if (!nu) return IIR_FULL;
    if (!ou) return IIR_OK;
    r = move_contents(W, INV_SIDE_SELF, ou, nu);
    if (r != IIR_OK) return r;
    InvItem *o = inv_get(T, ou);
    if (!o) return IIR_INVALID;
    if (is_default_pack(W, o)) { memset(o, 0, sizeof(*o)); W->last_uid = nu; W->last_side = INV_SIDE_SELF; return IIR_OK; }
    // The old pack (empty now): where the new one was, unless that was inside the old pack's grid.
    bool back = (s == INV_SIDE_SELF || s == INV_SIDE_LOOT) && !(s == INV_SIDE_SELF && from.parent == ou);
    if (back && check(W, s, o, INV_SIDE_SELF, from, 0, NULL) == IIR_OK) {
        if (!transfer(W, INV_SIDE_SELF, ou, s, from)) return IIR_FULL;
    } else {
        InvPlace q;
        if (!find_space(W, INV_SIDE_SELF, o, INV_SIDE_SELF, &q)) return IIR_NO_ROOM;
        transfer(W, INV_SIDE_SELF, ou, INV_SIDE_SELF, q);
    }
    W->last_uid = nu;
    W->last_side = INV_SIDE_SELF;
    return IIR_OK;
}

// ------------------------------------------------------------------ pickup rule

// Into self: merge, a weapon into a free slot, a bigger pack over the default bag, the first space.
static int auto_into_self(Work *W, int s, u16 uid, bool allow_equip) {
    InvItem *a = inv_get(W->t[s], uid);
    if (!a) return IIR_GONE;
    if (is_default_pack(W, a)) return IIR_DEFAULT_PACK;
    if (s != INV_SIDE_SELF && item_container(W->db, a->s.item) && inv_child_count(W->t[s], uid)) return IIR_NOT_EMPTY;
    int merged = merge_anywhere(W, a, INV_SIDE_SELF);
    if (!inv_alive(a)) return IIR_OK;
    if (allow_equip) {
        InvTree *T = W->t[INV_SIDE_SELF];
        InvItem tmp = *a;
        if (s != INV_SIDE_SELF) tmp.uid = 0;
        int slot = inv_equip_target(T, W->db, &tmp);
        if (slot == EQ_BACKPACK) {
            const InvItem *pk = inv_pack_c(T);
            if (!pk || is_default_pack(W, pk)) return equip_pack(W, s, a);
        } else if (slot != EQ_NONE && !inv_equipped(T, slot)) {
            InvPlace p = { 0, (u8)(INV_LOC_EQUIP + slot), 0, 0, 0 };
            if (check(W, INV_SIDE_SELF, a, s, p, 0, NULL) == IIR_OK) return transfer(W, s, uid, INV_SIDE_SELF, p) ? IIR_OK : IIR_FULL;
        }
    }
    InvPlace p;
    if (find_space(W, INV_SIDE_SELF, a, s, &p)) return transfer(W, s, uid, INV_SIDE_SELF, p) ? IIR_OK : IIR_FULL;
    return merged ? IIR_OK : IIR_NO_ROOM;
}

// Into a world container's root grid: merge into its stacks, then the first space.
static int auto_into_root(Work *W, int s, u16 uid, int d) {
    InvItem *a = inv_get(W->t[s], uid);
    if (!a) return IIR_GONE;
    if (s == INV_SIDE_SELF && is_worn(a, EQ_BACKPACK)) {
        u16 g;
        int r = pack_off(W, s, a, &g);
        if (r != IIR_OK) return r;
        a = inv_get(W->t[s], uid);
    }
    int merged = merge_anywhere(W, a, d);
    if (!inv_alive(a)) return IIR_OK;
    InvPlace p;
    int why = check(W, d, a, s, (InvPlace){ inv_grid(W->t[d], W->db), 0, 0, 0, 0 }, 0, NULL);
    if (find_space(W, d, a, s, &p)) return transfer(W, s, uid, d, p) ? IIR_OK : IIR_FULL;
    if (merged) return IIR_OK;
    return why == IIR_FILTER || why == IIR_NOT_EMPTY ? why : IIR_NO_ROOM;
}

// A new stack (ammo, a removed attachment) into side d: merge, then the first space.
static int add_stack(Work *W, int d, const InvSlot *s) {
    InvTree *H = W->t[SIDE_HAND];
    inv_tree_init(H, -1);
    int left = s->count;
    int ms = max_stack(W, s->item);
    while (left > 0) {
        InvSlot part = *s;
        part.count = (u16)DG_MIN(left, ms);
        InvItem *h = inv_alloc(H, W->db, &part);
        if (!h) return IIR_FULL;
        left -= part.count;
        int r = d == INV_SIDE_SELF ? auto_into_self(W, SIDE_HAND, h->uid, false) : auto_into_root(W, SIDE_HAND, h->uid, d);
        if (r != IIR_OK || inv_alive(h)) return r != IIR_OK ? r : IIR_NO_ROOM;
    }
    return IIR_OK;
}

// a (side s) goes to target (side d); b (side d) goes to a's old place (or, failing that, the
// first space on its side). Both are detached while the places are checked.
static int swap(Work *W, int s, InvItem *a, int d, InvItem *b, InvPlace target) {
    if (a == b) return IIR_NOOP;
    if (is_default_pack(W, b) || (item_container(W->db, b->s.item) && inv_child_count(W->t[d], b->uid))) return IIR_NO_ROOM;
    u16 au = a->uid, bu = b->uid;
    InvPlace pa = place_of(a);
    a->loc = INV_LOC_NONE;
    b->loc = INV_LOC_NONE;
    int r = check(W, d, a, s, target, 0, NULL);
    if (r != IIR_OK && target.loc < INV_LOC_EQUIP && item_def(W->db, a->s.item)->rotatable) {
        target.rot ^= 1;
        r = check(W, d, a, s, target, 0, NULL);
    }
    if (r != IIR_OK) return r;
    if (!transfer(W, s, au, d, target)) return IIR_FULL;
    b = inv_get(W->t[d], bu);
    if (!b) return IIR_INVALID;
    r = check(W, s, b, d, pa, 0, NULL);
    if (r != IIR_OK && pa.loc < INV_LOC_EQUIP && item_def(W->db, b->s.item)->rotatable) {
        pa.rot ^= 1;
        r = check(W, s, b, d, pa, 0, NULL);
    }
    if (r == IIR_OK) return transfer(W, d, bu, s, pa) ? IIR_OK : IIR_FULL;
    InvPlace any;
    if (find_space(W, d, b, d, &any)) return transfer(W, d, bu, d, any) ? IIR_OK : IIR_FULL;
    return IIR_NO_ROOM;
}

int inv_attach_check(const ItemDB *db, const InvSlot *weapon, u16 att_item, int *aslot_out) {
    const WeaponDef *wd = item_weapon(db, weapon->item);
    if (!wd || wd->kind == WPN_MELEE) return IIR_NOT_WEAPON;
    const ItemDef *ai = item_def(db, att_item);
    const AttachmentDef *a = ai->kind == ITEM_ATTACHMENT ? attach_def(db, ai->attachment) : NULL;
    if (!a) return IIR_INVALID;
    if (aslot_out) *aslot_out = a->slot;
    if (a->slot < 0 || a->slot >= ATT_SLOTS || !wd->att_slots[a->slot].present) return IIR_NO_SLOT;
    if (!attach_allowed(wd, a)) return IIR_INCOMPATIBLE;
    return IIR_OK;
}

// The weapon's footprint changed (attachment, fold): it must still fit where it lies.
static int refit(Work *W, int side, InvItem *w) {
    if (w->loc >= INV_LOC_EQUIP) return IIR_OK;
    return inv_check_place(W->t[side], W->db, w, place_of(w), 0, NULL);
}

// Clamp the magazine to the new capacity; the extra rounds go back as an ammo stack.
static int fix_magazine(Work *W, int side, InvItem *w) {
    const WeaponDef *wd = item_weapon(W->db, w->s.item);
    if (!wd) return IIR_OK;
    WeaponDef eff;
    attach_weapon_effective(W->db, wd, w->s.att, &eff);
    if (eff.mag_size <= 0 || w->s.ammo <= eff.mag_size) return IIR_OK;
    int extra = w->s.ammo - eff.mag_size;
    w->s.ammo = (i16)eff.mag_size;
    if (!wd->ammo_item) return IIR_OK;
    InvSlot am = { wd->ammo_item, (u16)extra, 0, { 0 }, 0 };
    return add_stack(W, side, &am);
}

static void consume_one(Work *W, InvItem *a) {
    const ItemDef *d = item_def(W->db, a->s.item);
    if (d->use_type == INV_USE_CHARGES && d->charges > 1) {
        a->s.ammo++;   // charges used
        if (a->s.ammo < d->charges) return;
        a->s.ammo = 0;
    }
    if (--a->s.count == 0) memset(a, 0, sizeof(*a));
}

bool inv_holdable(const ItemDef *d) {
    if (d->builtin) return false;
    return d->kind == ITEM_CONSUMABLE || d->kind == ITEM_THROWABLE || d->hands;
}

// Re-packs side's grid: everything detached, then placed largest first (reading order kept among
// equal sizes) at the first free space, unrotated first. All or nothing.
static int compact(Work *W, int side) {
    InvTree *T = W->t[side];
    u16 g = inv_grid(T, W->db);
    if (g == INV_NO_GRID) return IIR_INVALID;
    int idx[INV_MAX_ITEMS], n = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *it = &T->items[i];
        if (inv_alive(it) && it->parent == g && it->loc < INV_LOC_EQUIP) idx[n++] = i;
    }
    if (n < 1) return IIR_NOOP;
    InvItem before[INV_MAX_ITEMS];
    memcpy(before, T->items, sizeof(before));
    sort_by_area(W->db, T, idx, n);
    for (int k = 0; k < n; k++) T->items[idx[k]].loc = INV_LOC_NONE;
    for (int k = 0; k < n; k++) {
        InvItem *it = &T->items[idx[k]];
        InvItem tmp = *it;
        tmp.flags &= (u8)~INVF_ROT;
        InvPlace p;
        if (!inv_find_space(T, W->db, &tmp, g, &p)) return IIR_NO_ROOM;
        set_place(it, p);
    }
    return memcmp(before, T->items, sizeof(before)) ? IIR_OK : IIR_NOOP;
}

// ------------------------------------------------------------------ apply

static bool side_ok(const Work *W, const InvCtx *ctx, int side) {
    return side >= 0 && side < INV_SIDES && W->t[side] && (side != INV_SIDE_LOOT || ctx->loot_reachable);
}

static int quick_move(Work *W, int s, u16 uid) {
    InvItem *a = inv_get(W->t[s], uid);
    if (!a) return IIR_GONE;
    if (s == INV_SIDE_LOOT) return auto_into_self(W, s, uid, true);
    if (W->t[INV_SIDE_LOOT]) return auto_into_root(W, s, uid, INV_SIDE_LOOT);
    // No loot open: between the equipment slots and the grid.
    InvTree *T = W->t[s];
    if (is_worn(a, EQ_BACKPACK)) return IIR_NOOP;
    InvPlace p;
    if (a->loc >= INV_LOC_EQUIP) {
        if (!find_space(W, s, a, s, &p)) return IIR_NO_ROOM;
        return transfer(W, s, uid, s, p) ? IIR_OK : IIR_FULL;
    }
    int slot = inv_equip_target(T, W->db, a);
    if (slot == EQ_NONE || slot == EQ_BACKPACK || inv_equipped(T, slot)) return IIR_NOOP;
    InvPlace e = { 0, (u8)(INV_LOC_EQUIP + slot), 0, 0, 0 };
    int r = check(W, s, a, s, e, 0, NULL);
    if (r != IIR_OK) return r;
    return transfer(W, s, uid, s, e) ? IIR_OK : IIR_FULL;
}

static int take_all(Work *W, const InvCtx *ctx) {
    if (!W->t[INV_SIDE_LOOT]) return IIR_GONE;
    if (!ctx->loot_reachable) return IIR_TOO_FAR;
    InvTree *L = W->t[INV_SIDE_LOOT];
    int moved = 0, last = IIR_NOOP;
    // The grid first (a crate's root, a corpse's backpack), then what a corpse wears: its weapons, its
    // emptied backpack.
    u16 g = inv_grid(L, W->db);
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < INV_MAX_ITEMS; i++) {
            InvItem *a = &L->items[i];
            if (!inv_alive(a)) continue;
            bool grid = a->parent == g && a->loc < INV_LOC_EQUIP;
            bool weapon = !a->parent && a->loc >= INV_LOC_EQUIP && a->loc != INV_LOC_EQUIP + EQ_BACKPACK;
            bool pack = is_worn(a, EQ_BACKPACK);
            if (!(pass == 0 ? grid : pass == 1 ? weapon : pack)) continue;
            if (pack && (is_default_pack(W, a) || inv_child_count(L, a->uid))) continue;
            int r = auto_into_self(W, INV_SIDE_LOOT, a->uid, true);
            if (r == IIR_OK) moved++;
            else last = r;
        }
    return moved ? IIR_OK : last;
}

static int apply(Work *W, const InvCtx *ctx, const InvIntent *in) {
    const ItemDB *db = W->db;
    int s = in->src_side, d = in->dst_side;
    if (in->type == II_TAKE_ALL) return take_all(W, ctx);
    if (in->type == II_COMPACT) {
        if (d >= INV_SIDES || !W->t[d]) return IIR_GONE;
        if (!side_ok(W, ctx, d)) return IIR_TOO_FAR;
        return compact(W, d);
    }
    if (in->type == II_HOLD) {
        if (!W->has_hands) return IIR_INVALID;
        if (!in->src) { if (!W->hands) return IIR_NOOP; W->hands = 0; return IIR_OK; }
        const InvItem *b = inv_get(W->t[INV_SIDE_SELF], in->src);
        if (!b) return IIR_GONE;
        if (!inv_holdable(item_def(db, b->s.item))) return IIR_NOT_USABLE;
        if (W->hands == in->src) return IIR_NOOP;
        W->hands = in->src;
        return IIR_OK;
    }
    if (s >= INV_SIDES || !W->t[s]) return IIR_GONE;
    if (!side_ok(W, ctx, s)) return IIR_TOO_FAR;
    InvItem *a = inv_get(W->t[s], in->src);
    if (!a) return IIR_GONE;
    const ItemDef *ad = item_def(db, a->s.item);
    switch (in->type) {
    case II_MOVE: {
        if (d >= INV_SIDES || !W->t[d]) return IIR_GONE;
        if (!side_ok(W, ctx, d)) return IIR_TOO_FAR;
        if (d == INV_SIDE_LOOT && in->loc >= INV_LOC_EQUIP) return IIR_EQUIP;   // a corpse is looted, not dressed
        if (d == INV_SIDE_SELF && in->loc == INV_LOC_EQUIP + EQ_BACKPACK && in->dst != INV_AUTO) return equip_pack(W, s, a);
        bool partial = in->count && in->count < a->s.count && max_stack(W, a->s.item) > 1;
        u16 dst = in->dst;
        bool off = !a->parent && a->loc == INV_LOC_EQUIP + EQ_BACKPACK;   // a worn backpack comes off
        if (off) {
            u16 g;
            u16 au = a->uid;
            int r = pack_off(W, s, a, &g);
            if (r != IIR_OK) return r;
            a = inv_get(W->t[s], au);
            if (s == d && in->dst == au && in->loc < INV_LOC_EQUIP) dst = g;   // into its own grid: the new bag's
        }
        if (dst == INV_AUTO) {
            if (partial) return IIR_INVALID;
            return d == INV_SIDE_SELF ? auto_into_self(W, s, a->uid, false) : auto_into_root(W, s, a->uid, d);
        }
        InvPlace p = { dst, in->loc, in->x, in->y, (u8)(in->rot ? 1 : 0) };
        if (s == d && !partial && !off) {
            InvPlace cur = place_of(a);
            if (!memcmp(&cur, &p, sizeof(p))) return IIR_NOOP;
        }
        u16 blk = 0;
        InvItem chk = *a;
        if (partial) chk.uid = 0;   // a split: the source stack keeps its cells
        int r = check(W, d, &chk, partial ? d : s, p, 0, &blk);
        if (r == IIR_OK) {
            if (partial) return place_part(W, a, d, p, in->count) ? IIR_OK : IIR_FULL;
            return transfer(W, s, a->uid, d, p) ? IIR_OK : IIR_FULL;
        }
        if (r != IIR_NO_ROOM || !blk) return r;
        InvItem *b = inv_get(W->t[d], blk);
        if (!b) return r;
        // Onto a stack of the same item: merge (the remainder stays).
        if (b->s.item == a->s.item && max_stack(W, a->s.item) > 1) {
            int want = partial ? in->count : a->s.count;
            if (merge(W, a, b, want) > 0) { W->last_uid = b->uid; W->last_side = d; return IIR_OK; }
            if (partial) return IIR_FULL;
        }
        if (partial || off) return IIR_NO_ROOM;
        // A different single item: swap.
        InvPlace target = p.loc >= INV_LOC_EQUIP ? p : place_of(b);
        if (p.loc < INV_LOC_EQUIP) target.rot = p.rot;
        return swap(W, s, a, d, b, target);
    }
    case II_ROTATE: {
        if (a->loc >= INV_LOC_EQUIP) return IIR_NOOP;
        if (!ad->rotatable) return IIR_NOT_ROTATABLE;
        InvPlace p = place_of(a);
        p.rot ^= 1;
        int r = inv_check_place(W->t[s], db, a, p, 0, NULL);
        if (r != IIR_OK) return r;
        set_place(a, p);
        return IIR_OK;
    }
    case II_EQUIP: {
        if (!W->t[INV_SIDE_SELF]) return IIR_INVALID;
        InvItem tmp = *a;
        if (s != INV_SIDE_SELF) tmp.uid = 0;
        int slot = in->aux == 255 ? inv_equip_target(W->t[INV_SIDE_SELF], db, &tmp) : in->aux;
        if (slot == EQ_NONE || slot >= EQ_COUNT) return IIR_EQUIP;
        if (slot == EQ_BACKPACK) {
            if (!inv_equip_accepts(slot, ad)) return IIR_EQUIP;
            return equip_pack(W, s, a);
        }
        InvPlace p = { 0, (u8)(INV_LOC_EQUIP + slot), 0, 0, 0 };
        if (s == INV_SIDE_SELF && a->loc == p.loc && !a->parent) return IIR_NOOP;
        u16 blk = 0;
        int r = check(W, INV_SIDE_SELF, a, s, p, 0, &blk);
        if (r == IIR_OK) return transfer(W, s, a->uid, INV_SIDE_SELF, p) ? IIR_OK : IIR_FULL;
        if (r != IIR_NO_ROOM || !blk) return r;
        InvItem *b = inv_get(W->t[INV_SIDE_SELF], blk);
        if (!b) return r;
        return swap(W, s, a, INV_SIDE_SELF, b, p);
    }
    case II_QUICK_MOVE: return quick_move(W, s, a->uid);
    case II_SPLIT: {
        if (a->s.count < 2 || max_stack(W, a->s.item) <= 1) return IIR_NOOP;
        if (d >= INV_SIDES || !W->t[d]) return IIR_GONE;
        if (!side_ok(W, ctx, d)) return IIR_TOO_FAR;
        int n = in->count ? DG_MIN((int)in->count, a->s.count - 1) : a->s.count / 2;
        if (n <= 0) return IIR_NOOP;
        InvItem tmp = *a;
        tmp.s.count = (u16)n;
        tmp.uid = 0;   // the new stack: a's own cells stay taken
        InvPlace p = { in->dst, in->loc, in->x, in->y, (u8)(in->rot ? 1 : 0) };
        if (in->dst == INV_AUTO) {
            if (!inv_find_grid_space(W->t[d], db, &tmp, &p)) return IIR_NO_ROOM;
        } else {
            int r = inv_check_place(W->t[d], db, &tmp, p, 0, NULL);
            if (r != IIR_OK) return r;
        }
        return place_part(W, a, d, p, n) ? IIR_OK : IIR_FULL;
    }
    case II_DISCARD: {
        // Into the effect's drop tree (the host spawns it as a pickup). A worn backpack comes off
        // first: its contents stay with the player, in the default bag.
        InvTree *D = W->t[SIDE_DROP];
        if (!D) return IIR_INVALID;
        if (is_default_pack(W, a)) return IIR_DEFAULT_PACK;
        u16 lu = W->last_uid;
        int ls = W->last_side;
        if (in->count && in->count < a->s.count) {
            InvSlot part = a->s;
            part.count = in->count;
            InvItem *o = inv_alloc(D, db, &part);
            if (!o) return IIR_FULL;
            o->dur = a->dur;
            a->s.count = (u16)(a->s.count - in->count);
            return IIR_OK;
        }
        if (is_worn(a, EQ_BACKPACK)) {
            u16 g, au = a->uid;
            int r = pack_off(W, s, a, &g);
            if (r != IIR_OK) return r;
            a = inv_get(W->t[s], au);
        }
        InvPlace p = { 0, 0, 0, 0, 0 };
        u16 top = transfer(W, s, a->uid, SIDE_DROP, p);
        W->last_uid = lu;   // the selection doesn't follow it to the ground
        W->last_side = ls;
        return top ? IIR_OK : IIR_FULL;
    }
    case II_USE: {
        if (s != INV_SIDE_SELF) return IIR_NOT_USABLE;
        if (ad->kind != ITEM_CONSUMABLE && ad->use_type == INV_USE_NONE) return IIR_NOT_USABLE;
        if (ad->vehicle_use) return IIR_NOT_USABLE;   // used from the hands on a vehicle (docs/VEHICLES.md)
        if (W->fx) W->fx->used_item = a->s.item;
        consume_one(W, a);
        return IIR_OK;
    }
    case II_MOUNT: {
        if (d >= INV_SIDES || !W->t[d]) return IIR_GONE;
        if (!side_ok(W, ctx, d)) return IIR_TOO_FAR;
        InvItem *w = inv_get(W->t[d], in->dst);
        if (!w) return IIR_GONE;
        int aslot = -1;
        int r = inv_attach_check(db, &w->s, a->s.item, &aslot);
        if (r != IIR_OK) return r;
        u8 new_id = ad->attachment, old_id = w->s.att[aslot];
        if (new_id == old_id) return IIR_NOOP;
        u16 wu = w->uid;
        InvPlace pa = place_of(a);
        // The attachment item is consumed: its cells free up first (the weapon may grow into them).
        if (a->s.count > 1) a->s.count--;
        else inv_free_subtree(W->t[s], a->uid);
        w = inv_get(W->t[d], wu);
        w->s.att[aslot] = new_id;
        r = refit(W, d, w);
        if (r != IIR_OK) return r;
        r = fix_magazine(W, d, w);
        if (r != IIR_OK) return r;
        u16 old_item = item_for_attachment(db, old_id);
        if (old_item) {
            InvSlot os = { old_item, 1, 0, { 0 }, 0 };
            InvItem tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.s = os;
            if (inv_check_place(W->t[s], db, &tmp, pa, 0, NULL) == IIR_OK) {
                InvItem *o = inv_alloc(W->t[s], db, &os);
                if (!o) return IIR_FULL;
                set_place(o, pa);
            } else {
                r = add_stack(W, s, &os);
                if (r != IIR_OK) return r;
            }
        }
        return IIR_OK;
    }
    case II_UNMOUNT: {
        if (in->aux >= ATT_SLOTS) return IIR_INVALID;
        if (!item_weapon(db, a->s.item)) return IIR_NOT_WEAPON;
        u8 id = a->s.att[in->aux];
        if (!id) return IIR_NOOP;
        u16 item = item_for_attachment(db, id);
        if (!item) return IIR_INVALID;
        if (d >= INV_SIDES || !W->t[d]) return IIR_GONE;
        if (!side_ok(W, ctx, d)) return IIR_TOO_FAR;
        a->s.att[in->aux] = 0;
        int r = fix_magazine(W, s, a);
        if (r != IIR_OK) return r;
        InvSlot os = { item, 1, 0, { 0 }, 0 };
        if (in->dst == INV_AUTO) return add_stack(W, d, &os);
        InvItem tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.s = os;
        InvPlace p = { in->dst, in->loc, in->x, in->y, (u8)(in->rot ? 1 : 0) };
        r = inv_check_place(W->t[d], db, &tmp, p, 0, NULL);
        if (r != IIR_OK) return r;
        InvItem *o = inv_alloc(W->t[d], db, &os);
        if (!o) return IIR_FULL;
        set_place(o, p);
        return IIR_OK;
    }
    case II_FOLD: {
        if (!ad->fold_dw) return IIR_NOT_WEAPON;
        a->flags ^= INVF_FOLDED;
        return refit(W, s, a);
    }
    case II_UNLOAD: {
        const WeaponDef *wd = item_weapon(db, a->s.item);
        if (!wd || wd->kind == WPN_MELEE) return IIR_NOT_WEAPON;
        if (a->s.ammo <= 0 || !wd->ammo_item) return IIR_NOOP;
        InvSlot am = { wd->ammo_item, (u16)a->s.ammo, 0, { 0 }, 0 };
        u16 au = a->uid;
        int r = add_stack(W, s, &am);
        if (r != IIR_OK) return r;
        a = inv_get(W->t[s], au);
        if (a) a->s.ammo = 0;
        return IIR_OK;
    }
    default: return IIR_INVALID;
    }
}

bool inv_ensure_pack(InvTree *t, const ItemDB *db) {
    if (!db->default_backpack || !inv_root_def(t, db)->equip || inv_pack(t)) return false;
    InvSlot ds = { db->default_backpack, 1, 0, { 0 }, 0 };
    InvItem *it = inv_alloc(t, db, &ds);
    if (!it) return false;
    it->loc = INV_LOC_EQUIP + EQ_BACKPACK;
    return true;
}

static void finish(Work *W) {
    for (int i = 0; i < INV_SIDES; i++) if (W->t[i]) inv_normalize(W->t[i], W->db);
    inv_ensure_pack(W->t[INV_SIDE_SELF], W->db);
    if (W->hands && !inv_get(W->t[INV_SIDE_SELF], W->hands)) W->hands = 0;
}

// Scratch copies of the sides the context has; committed back by commit().
static void work_begin(Work *W, InvCtx *ctx, InvEffect *fx) {
    memset(W, 0, sizeof(*W));
    W->db = ctx->db;
    W->fx = fx;
    for (int i = 0; i < INV_SIDES; i++) {
        if (!ctx->tree[i]) continue;
        g_scratch[i] = *ctx->tree[i];
        W->t[i] = &g_scratch[i];
    }
    W->t[SIDE_HAND] = &g_scratch[SIDE_HAND];
    inv_tree_init(W->t[SIDE_HAND], -1);
    if (fx) {
        inv_tree_init(&fx->dropped, -1);
        W->t[SIDE_DROP] = &fx->dropped;
    }
    W->has_hands = ctx->hands != NULL;
    W->hands = ctx->hands ? *ctx->hands : 0;
}

static void commit(Work *W, InvCtx *ctx) {
    finish(W);
    for (int i = 0; i < INV_SIDES; i++) if (ctx->tree[i]) *ctx->tree[i] = *W->t[i];
    if (ctx->hands) *ctx->hands = W->hands;
}

int inv_intent_apply(InvCtx *ctx, const InvIntent *in, InvEffect *fx) {
    InvEffect local;
    if (!fx) fx = &local;
    memset(fx, 0, sizeof(*fx));
    if (!ctx->db || !ctx->tree[INV_SIDE_SELF] || in->type == II_NONE || in->type >= II_COUNT) return IIR_INVALID;
    Work W;
    work_begin(&W, ctx, fx);
    int r = apply(&W, ctx, in);
    if (r != IIR_OK) {
        memset(fx, 0, sizeof(*fx));
        return r;
    }
    fx->moved_uid = W.last_uid;
    fx->moved_side = (u8)W.last_side;
    commit(&W, ctx);
    return IIR_OK;
}

int inv_give(InvCtx *ctx, const InvSlot *s) {
    if (!s->item || !s->count || !ctx->tree[INV_SIDE_SELF]) return s->count;
    Work W;
    InvEffect fx;
    memset(&fx, 0, sizeof(fx));
    work_begin(&W, ctx, &fx);
    inv_ensure_pack(W.t[INV_SIDE_SELF], W.db);   // a fresh tree: the default bag first
    InvTree *H = W.t[SIDE_HAND];
    int ms = max_stack(&W, s->item);
    int left = s->count;
    while (left > 0) {
        InvSlot part = *s;
        part.count = (u16)DG_MIN(left, ms);
        InvItem *h = inv_alloc(H, W.db, &part);
        if (!h) break;
        auto_into_self(&W, SIDE_HAND, h->uid, true);
        int stuck = inv_alive(h) ? h->s.count : 0;
        left -= part.count - stuck;
        if (stuck) break;
    }
    commit(&W, ctx);
    return left;
}

int inv_give_root(InvTree *t, const ItemDB *db, const InvSlot *s) {
    InvCtx ctx = inv_ctx_make(db, t, NULL, NULL);
    Work W;
    work_begin(&W, &ctx, NULL);
    InvTree *H = W.t[SIDE_HAND];
    int ms = max_stack(&W, s->item);
    int left = s->count;
    while (left > 0) {
        InvSlot part = *s;
        part.count = (u16)DG_MIN(left, ms);
        InvItem *h = inv_alloc(H, db, &part);
        if (!h) break;
        auto_into_root(&W, SIDE_HAND, h->uid, INV_SIDE_SELF);
        int stuck = inv_alive(h) ? h->s.count : 0;
        left -= part.count - stuck;
        if (stuck) break;
    }
    commit(&W, &ctx);
    return left;
}

void inv_strip_to_corpse(InvTree *body, u16 *hands, InvTree *corpse, const ItemDB *db) {
    InvCtx ctx = inv_ctx_make(db, body, hands, corpse);
    Work W;
    work_begin(&W, &ctx, NULL);
    InvTree *B = W.t[INV_SIDE_SELF], *C = W.t[INV_SIDE_LOOT];
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *it = &B->items[i];
        if (!inv_alive(it) || it->parent) continue;
        // What it wears, in the same slots (the backpack with its contents); a detached dev item is lost.
        int slot = it->loc - INV_LOC_EQUIP;
        bool ok = it->loc >= INV_LOC_EQUIP && slot < EQ_COUNT && inv_root_def(C, db)->equip && !inv_equipped(C, slot);
        if (!ok || !transfer(&W, INV_SIDE_SELF, it->uid, INV_SIDE_LOOT, place_of(it))) inv_free_subtree(B, it->uid);
    }
    commit(&W, &ctx);
}
