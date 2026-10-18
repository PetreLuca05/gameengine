#include "inv_intent.h"
#include "gameplay/attachments.h"
#include <stdlib.h>
#include <string.h>

// Sides inside an intent: the two trees, a scratch "hand" holding what a pickup adds and the
// effect's drop tree (what a discard leaves on the ground, InvEffect.dropped).
enum { SIDE_HAND = INV_SIDES, SIDE_DROP, WORK_SIDES };

typedef struct Work {
    InvTree *t[WORK_SIDES];
    u16 quick[16];
    int quick_n;
    const ItemDB *db;
    InvOrder order;
    InvEffect *fx;
    u16 last_uid;      // the last transfer's new uid
    int last_side;
    u16 arrived[INV_MAX_ITEMS];   // self uids that came over from the loot side (bound on commit, like a pickup)
    int arrived_n;
} Work;

static InvTree g_scratch[WORK_SIDES];

const char *inv_intent_name(int type) {
    static const char *n[II_COUNT] = { "none", "move", "insert", "rotate", "equip", "quick_move", "split", "discard", "use",
                                       "mount", "unmount", "fold", "unload", "bind", "take_all", "sort" };
    return type >= 0 && type < II_COUNT ? n[type] : "?";
}

int inv_intent_from_name(const char *s) {
    for (int i = 1; i < II_COUNT; i++) if (!strcmp(s, inv_intent_name(i))) return i;
    return II_NONE;
}

InvCtx inv_ctx_make(const ItemDB *db, InvTree *self, u16 *quick, int quick_n, InvTree *loot) {
    InvCtx c;
    memset(&c, 0, sizeof(c));
    c.tree[INV_SIDE_SELF] = self;
    c.tree[INV_SIDE_LOOT] = loot;
    c.quick = quick;
    c.quick_n = quick_n;
    c.db = db;
    c.order = inv_order_default();
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

// Placement check of a (living on side as) at p on side d: a foreign item has no uid there.
static int check(Work *W, int d, const InvItem *a, int as, InvPlace p, u16 ignore, u16 *blocker) {
    InvItem tmp = *a;
    if (as != d) tmp.uid = 0;
    return inv_check_place(W->t[d], W->db, &tmp, p, ignore, blocker);
}

// Moves uid's subtree from side s to side d at p (checked by the caller). The top keeps its uid
// inside a tree; across trees every item gets a new uid. Returns the top's uid (0: pool full).
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
    if (s == INV_SIDE_LOOT && d == INV_SIDE_SELF && W->arrived_n < INV_MAX_ITEMS) W->arrived[W->arrived_n++] = new_uid[0];
    return new_uid[0];
}

// A part of a stack (n of a) as a new instance at p on side d (checked by the caller).
static u16 place_part(Work *W, int s, InvItem *a, int d, InvPlace p, int n) {
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
    if (s == INV_SIDE_LOOT && d == INV_SIDE_SELF && W->arrived_n < INV_MAX_ITEMS) W->arrived[W->arrived_n++] = it->uid;
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

// Merges a's stack into same-def stacks on side d (a may be on another side). Returns moved.
static int merge_anywhere(Work *W, InvItem *a, int d, bool root_only) {
    if (max_stack(W, a->s.item) <= 1) return 0;
    int moved = 0;
    InvTree *D = W->t[d];
    for (int i = 0; i < INV_MAX_ITEMS && inv_alive(a); i++) {
        InvItem *b = &D->items[i];
        if (!inv_alive(b) || b == a || b->s.item != a->s.item || (root_only && b->parent)) continue;
        moved += merge(W, a, b, a->s.count);
    }
    return moved;
}

// A free place for a (on side as) on side d: self = the containers in order, else the root.
static bool find_space(Work *W, int d, const InvItem *a, int as, InvPlace *out) {
    InvItem tmp = *a;
    if (as != d) tmp.uid = 0;
    if (d == INV_SIDE_SELF) return inv_find_space_order(W->t[d], W->db, &tmp, W->order, out);
    return inv_find_space(W->t[d], W->db, &tmp, 0, out);
}

// The pickup rule into self: merge, equip into a free slot, first space in the order.
static int auto_into_self(Work *W, int s, u16 uid, bool allow_equip) {
    InvItem *a = inv_get(W->t[s], uid);
    if (!a) return IIR_GONE;
    int merged = merge_anywhere(W, a, INV_SIDE_SELF, false);
    if (!inv_alive(a)) return IIR_OK;
    if (allow_equip) {
        InvItem tmp = *a;
        if (s != INV_SIDE_SELF) tmp.uid = 0;
        int slot = inv_equip_target(W->t[INV_SIDE_SELF], W->db, &tmp);
        if (slot != EQ_NONE && !inv_equipped(W->t[INV_SIDE_SELF], slot)) {
            InvPlace p = { 0, (u8)(INV_LOC_EQUIP + slot), 0, 0, 0 };
            if (check(W, INV_SIDE_SELF, a, s, p, 0, NULL) == IIR_OK) return transfer(W, s, uid, INV_SIDE_SELF, p) ? IIR_OK : IIR_FULL;
        }
    }
    InvPlace p;
    if (find_space(W, INV_SIDE_SELF, a, s, &p)) return transfer(W, s, uid, INV_SIDE_SELF, p) ? IIR_OK : IIR_FULL;
    return merged ? IIR_OK : IIR_NO_ROOM;
}

// Into a world container's root: merge into its root stacks, then the first space.
static int auto_into_root(Work *W, int s, u16 uid, int d) {
    InvItem *a = inv_get(W->t[s], uid);
    if (!a) return IIR_GONE;
    int merged = merge_anywhere(W, a, d, true);
    if (!inv_alive(a)) return IIR_OK;
    InvPlace p;
    if (find_space(W, d, a, s, &p)) return transfer(W, s, uid, d, p) ? IIR_OK : IIR_FULL;
    return merged ? IIR_OK : IIR_NO_ROOM;
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
    u16 au = a->uid, bu = b->uid;
    if (s == d && (inv_is_ancestor(W->t[s], au, bu) || inv_is_ancestor(W->t[s], bu, au))) return IIR_NESTING;
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

// What a hotbar slot may hold: things used from the hands. Never a worn container (a bound backpack
// would be "held" and dropped on downed) and never the built-in phone (its own key, WPN_SLOT_PHONE).
bool inv_bindable(const ItemDef *d) {
    if (d->builtin) return false;
    return d->kind == ITEM_WEAPON || d->kind == ITEM_CONSUMABLE || d->kind == ITEM_THROWABLE || d->camera || d->hotbar;   // "hotbar": a held tool (the flashlight)
}

// Binds a self item to the first free hotbar slot (the pickup rule), unless it is bound already or
// not bindable.
static void auto_bind(Work *W, u16 uid) {
    InvTree *T = W->t[INV_SIDE_SELF];
    InvItem *it = T ? inv_get(T, uid) : NULL;
    if (!it || !inv_bindable(item_def(W->db, it->s.item))) return;
    for (int k = 0; k < W->quick_n; k++) if (W->quick[k] == uid) return;
    for (int k = 0; k < W->quick_n; k++)
        if (!W->quick[k] || !inv_get(T, W->quick[k])) { W->quick[k] = uid; return; }
}

// Why a can't go into container b (no free space found): its filter, the nesting rule, or room.
static int insert_why(Work *W, const InvItem *a, const InvItem *b) {
    const InvContainerDef *bc = item_container(W->db, b->s.item);
    int w, h;
    inv_base_size(W->db, &a->s, a->flags, &w, &h);
    int why = inv_accepts(W->db, bc, &a->s, w, h);
    const InvContainerDef *own = item_container(W->db, a->s.item);
    if (why == IIR_OK && own && own->capacity >= bc->capacity) why = IIR_NESTING;
    return why != IIR_OK ? why : IIR_NO_ROOM;
}

// Sort keys: kind then id, weight (heaviest first), size (largest first); ties keep the uid order.
static const Work *g_sort_w;
static int g_sort_key;
static InvTree *g_sort_t;
static int sort_cmp(const void *pa, const void *pb) {
    const InvItem *a = &g_sort_t->items[*(const int *)pa], *b = &g_sort_t->items[*(const int *)pb];
    const ItemDef *da = item_def(g_sort_w->db, a->s.item), *db = item_def(g_sort_w->db, b->s.item);
    int wa, ha, wb, hb;
    inv_base_size(g_sort_w->db, &a->s, a->flags, &wa, &ha);
    inv_base_size(g_sort_w->db, &b->s, b->flags, &wb, &hb);
    float ka = 0, kb = 0;
    switch (g_sort_key) {
    case 1: ka = -inv_item_weight(g_sort_t, g_sort_w->db, a); kb = -inv_item_weight(g_sort_t, g_sort_w->db, b); break;
    case 2: ka = (float)-(wa * ha); kb = (float)-(wb * hb); break;
    default: ka = (float)da->kind; kb = (float)db->kind; break;
    }
    if (ka != kb) return ka < kb ? -1 : 1;
    int c = strcmp(da->id, db->id);
    if (c) return c;
    if (wa * ha != wb * hb) return wb * hb - wa * ha;
    return (int)a->uid - (int)b->uid;
}

// Re-packs the direct contents of one container (all compartments): everything detached, then
// placed in key order at the first free space (unrotated first). All or nothing.
static int sort_container(Work *W, int side, u16 parent, int key) {
    InvTree *T = W->t[side];
    if (parent && (!inv_get(T, parent) || !item_container(W->db, inv_get(T, parent)->s.item))) return IIR_INVALID;
    int idx[INV_MAX_ITEMS], n = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *it = &T->items[i];
        if (inv_alive(it) && it->parent == parent && it->loc < INV_LOC_EQUIP) idx[n++] = i;
    }
    if (n < 2) return IIR_NOOP;
    g_sort_w = W;
    g_sort_t = T;
    g_sort_key = key;
    qsort(idx, (size_t)n, sizeof(int), sort_cmp);
    for (int k = 0; k < n; k++) T->items[idx[k]].loc = INV_LOC_NONE;
    for (int k = 0; k < n; k++) {
        InvItem *it = &T->items[idx[k]];
        InvItem tmp = *it;
        tmp.flags &= (u8)~INVF_ROT;
        InvPlace p;
        if (!inv_find_space(T, W->db, &tmp, parent, &p)) return IIR_NO_ROOM;
        set_place(it, p);
    }
    return IIR_OK;
}

// ------------------------------------------------------------------ apply

static bool side_ok(const Work *W, const InvCtx *ctx, int side) {
    return side >= 0 && side < INV_SIDES && W->t[side] && (side != INV_SIDE_LOOT || ctx->loot_reachable);
}

static int quick_move(Work *W, int s, u16 uid) {
    InvItem *a = inv_get(W->t[s], uid);
    if (!a) return IIR_GONE;
    bool loot = W->t[INV_SIDE_LOOT] != NULL;
    if (s == INV_SIDE_LOOT) return auto_into_self(W, s, uid, true);
    if (loot) return auto_into_root(W, s, uid, INV_SIDE_LOOT);
    InvTree *T = W->t[s];
    // In an equipment slot: into the containers. Else: equip into a free slot, else the next
    // container in the order that doesn't hold it already.
    if (a->loc < INV_LOC_EQUIP || a->parent) {
        int slot = inv_equip_target(T, W->db, a);
        if (slot != EQ_NONE && !inv_equipped(T, slot)) {
            InvPlace p = { 0, (u8)(INV_LOC_EQUIP + slot), 0, 0, 0 };
            if (check(W, s, a, s, p, 0, NULL) == IIR_OK) return transfer(W, s, uid, s, p) ? IIR_OK : IIR_FULL;
        }
    }
    u16 cur = a->loc >= INV_LOC_EQUIP && !a->parent ? 0xFFFE : a->parent;
    for (int i = 0; i < W->order.n; i++) {
        u16 parent;
        if (!inv_quick_parent(T, (InvQuickTarget)W->order.t[i], &parent) || parent == cur) continue;
        if (parent && inv_is_ancestor(T, uid, parent)) continue;
        InvPlace p;
        if (inv_find_space(T, W->db, a, parent, &p)) return transfer(W, s, uid, s, p) ? IIR_OK : IIR_FULL;
    }
    return IIR_NO_ROOM;
}

static int apply(Work *W, const InvCtx *ctx, const InvIntent *in) {
    const ItemDB *db = W->db;
    int s = in->src_side, d = in->dst_side;
    if (in->type == II_TAKE_ALL) {
        if (!W->t[INV_SIDE_LOOT]) return IIR_GONE;
        if (!ctx->loot_reachable) return IIR_TOO_FAR;
        InvTree *L = W->t[INV_SIDE_LOOT];
        int moved = 0, last = IIR_NOOP;
        bool tried[INV_MAX_ITEMS] = { false };   // by pool index (entries never move inside a tree)
        for (int i = 0; i < INV_MAX_ITEMS; i++) {
            InvItem *a = &L->items[i];
            if (!inv_alive(a) || a->parent) continue;
            tried[i] = true;
            int r = auto_into_self(W, INV_SIDE_LOOT, a->uid, true);
            if (r == IIR_OK) moved++;
            else last = r;
        }
        // A container that stayed behind (a corpse's backpack when the player wears one): its
        // contents one by one, top down (a pouch inside it after the pouch itself was tried).
        for (bool progress = true; progress;) {
            progress = false;
            for (int i = 0; i < INV_MAX_ITEMS; i++) {
                InvItem *a = &L->items[i];
                if (tried[i] || !inv_alive(a) || !a->parent) continue;
                const InvItem *c = inv_get_c(L, a->parent);
                if (!c || !tried[c - L->items]) continue;
                tried[i] = true;
                progress = true;
                int r = auto_into_self(W, INV_SIDE_LOOT, a->uid, true);
                if (r == IIR_OK) moved++;
                else last = r;
            }
        }
        return moved ? IIR_OK : last;
    }
    if (in->type == II_SORT) {
        if (d >= INV_SIDES || !W->t[d]) return IIR_GONE;
        if (!side_ok(W, ctx, d)) return IIR_TOO_FAR;
        return sort_container(W, d, in->dst, in->aux);
    }
    if (in->type == II_BIND) {
        if (!W->quick_n || in->aux >= W->quick_n) return IIR_INVALID;
        if (!in->src) { if (!W->quick[in->aux]) return IIR_NOOP; W->quick[in->aux] = 0; return IIR_OK; }
        const InvItem *b = inv_get(W->t[INV_SIDE_SELF], in->src);
        if (!b) return IIR_GONE;
        if (!inv_bindable(item_def(db, b->s.item))) return IIR_NOT_USABLE;
        for (int k = 0; k < W->quick_n; k++) if (W->quick[k] == in->src) W->quick[k] = 0;
        W->quick[in->aux] = in->src;
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
        bool partial = in->count && in->count < a->s.count && max_stack(W, a->s.item) > 1;
        if (in->dst == INV_AUTO) {
            if (partial) return IIR_INVALID;
            return d == INV_SIDE_SELF ? auto_into_self(W, s, a->uid, false) : auto_into_root(W, s, a->uid, d);
        }
        InvPlace p = { in->dst, in->loc, in->x, in->y, (u8)(in->rot ? 1 : 0) };
        if (s == d && !partial) {
            InvPlace cur = place_of(a);
            if (!memcmp(&cur, &p, sizeof(p))) return IIR_NOOP;
        }
        u16 blk = 0;
        InvItem chk = *a;
        if (partial) chk.uid = 0;   // a split: the source stack keeps its cells
        int r = check(W, d, &chk, partial ? d : s, p, 0, &blk);
        if (r == IIR_OK) {
            if (partial) return place_part(W, s, a, d, p, in->count) ? IIR_OK : IIR_FULL;
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
        // Onto a container: into its first free space.
        if (item_container(db, b->s.item) && !(s == d && inv_is_ancestor(W->t[s], a->uid, b->uid))) {
            InvItem tmp = *a;
            if (s != d) tmp.uid = 0;
            if (partial) tmp.s.count = in->count;
            InvPlace ip;
            if (!inv_find_space(W->t[d], db, &tmp, b->uid, &ip)) return insert_why(W, a, b);
            if (partial) return place_part(W, s, a, d, ip, in->count) ? IIR_OK : IIR_FULL;
            return transfer(W, s, a->uid, d, ip) ? IIR_OK : IIR_FULL;
        }
        if (partial) return IIR_NO_ROOM;
        // A different single item: swap.
        InvPlace target = p.loc >= INV_LOC_EQUIP ? p : place_of(b);
        if (p.loc < INV_LOC_EQUIP) target.rot = p.rot;
        return swap(W, s, a, d, b, target);
    }
    case II_INSERT: {
        if (d >= INV_SIDES || !W->t[d]) return IIR_GONE;
        if (!side_ok(W, ctx, d)) return IIR_TOO_FAR;
        InvItem *b = inv_get(W->t[d], in->dst);
        if (!b) return IIR_GONE;
        if (!item_container(db, b->s.item)) return IIR_INVALID;
        if (s == d && inv_is_ancestor(W->t[s], a->uid, b->uid)) return IIR_NESTING;
        InvItem tmp = *a;
        if (s != d) tmp.uid = 0;
        InvPlace ip;
        if (!inv_find_space(W->t[d], db, &tmp, b->uid, &ip)) return insert_why(W, a, b);
        return transfer(W, s, a->uid, d, ip) ? IIR_OK : IIR_FULL;
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
        if (s != d) tmp.uid = 0;
        InvPlace p = { in->dst, in->loc, in->x, in->y, (u8)(in->rot ? 1 : 0) };
        if (in->dst == INV_AUTO) {
            tmp.uid = 0;   // the new stack: a's own cells stay taken
            bool ok = d == INV_SIDE_SELF ? inv_find_space_order(W->t[d], db, &tmp, W->order, &p) : inv_find_space(W->t[d], db, &tmp, 0, &p);
            if (!ok) return IIR_NO_ROOM;
        } else {
            tmp.uid = 0;
            int r = inv_check_place(W->t[d], db, &tmp, p, 0, NULL);
            if (r != IIR_OK) return r;
        }
        return place_part(W, s, a, d, p, n) ? IIR_OK : IIR_FULL;
    }
    case II_DISCARD: {
        // Into the effect's drop tree as it was: a container keeps its contents and their layout (the
        // host spawns it as one pickup), however many entries the subtree has.
        InvTree *D = W->t[SIDE_DROP];
        if (!D) return IIR_INVALID;
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

static void finish(Work *W) {
    for (int i = 0; i < INV_SIDES; i++) if (W->t[i]) inv_normalize(W->t[i], W->db);
    for (int k = 0; k < W->quick_n; k++)
        if (W->quick[k] && (!W->t[INV_SIDE_SELF] || !inv_get(W->t[INV_SIDE_SELF], W->quick[k]))) W->quick[k] = 0;
    // Weapons / consumables / throwables taken from a world container bind like a pickup (after the
    // stale binds cleared: a swap's outgoing item frees its slot first).
    for (int k = 0; k < W->arrived_n; k++) auto_bind(W, W->arrived[k]);
}

// Scratch copies of the sides the context has; committed back by commit().
static void work_begin(Work *W, InvCtx *ctx, InvEffect *fx) {
    memset(W, 0, sizeof(*W));
    W->db = ctx->db;
    W->order = ctx->order.n ? ctx->order : inv_order_default();
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
    W->quick_n = DG_MIN(ctx->quick_n, (int)ARRAY_COUNT(W->quick));
    if (ctx->quick) memcpy(W->quick, ctx->quick, sizeof(u16) * (size_t)W->quick_n);
}

static void commit(Work *W, InvCtx *ctx) {
    finish(W);
    for (int i = 0; i < INV_SIDES; i++) if (ctx->tree[i]) *ctx->tree[i] = *W->t[i];
    if (ctx->quick) memcpy(ctx->quick, W->quick, sizeof(u16) * (size_t)W->quick_n);
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
    InvTree *H = W.t[SIDE_HAND];
    int ms = max_stack(&W, s->item);
    int left = s->count;
    while (left > 0) {
        InvSlot part = *s;
        part.count = (u16)DG_MIN(left, ms);
        InvItem *h = inv_alloc(H, W.db, &part);
        if (!h) break;
        W.last_uid = 0;
        auto_into_self(&W, SIDE_HAND, h->uid, true);
        int stuck = inv_alive(h) ? h->s.count : 0;
        left -= part.count - stuck;
        if (W.last_uid && W.last_side == INV_SIDE_SELF) auto_bind(&W, W.last_uid);
        if (stuck) break;
    }
    commit(&W, ctx);
    return left;
}

int inv_give_with(InvCtx *ctx, const InvSlot *s, const InvTree *contents) {
    if (!s->item || !s->count || !ctx->tree[INV_SIDE_SELF]) return s->count;
    if (!contents || !inv_live_count(contents)) return inv_give(ctx, s);
    Work W;
    InvEffect fx;
    memset(&fx, 0, sizeof(fx));
    work_begin(&W, ctx, &fx);
    InvTree *H = W.t[SIDE_HAND];
    InvItem *top = inv_alloc(H, W.db, s);
    if (!top) return s->count;
    u16 tu = top->uid;
    // The contents under the new top, every entry with its compartment / cell / rotation: uids mapped
    // (the drop tree's root level is the container itself).
    u16 old_uid[INV_MAX_ITEMS], new_uid[INV_MAX_ITEMS];
    int n = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *c = &contents->items[i];
        if (!inv_alive(c)) continue;
        InvItem *h = inv_alloc(H, W.db, &c->s);
        if (!h) return s->count;
        h->dur = c->dur;
        h->flags = c->flags;
        h->loc = c->loc;
        h->x = c->x;
        h->y = c->y;
        h->parent = c->parent;   // remapped below
        old_uid[n] = c->uid;
        new_uid[n++] = h->uid;
    }
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *h = &H->items[i];
        if (!inv_alive(h) || h->uid == tu) continue;
        u16 np = tu;
        for (int k = 0; k < n && h->parent; k++) if (old_uid[k] == h->parent) np = new_uid[k];
        h->parent = np;
    }
    W.last_uid = 0;
    int r = auto_into_self(&W, SIDE_HAND, tu, true);
    if (r != IIR_OK || inv_get(H, tu)) return s->count;   // nothing committed
    if (W.last_side == INV_SIDE_SELF) auto_bind(&W, W.last_uid);
    commit(&W, ctx);
    return 0;
}

int inv_give_root(InvTree *t, const ItemDB *db, const InvSlot *s) {
    InvCtx ctx = inv_ctx_make(db, t, NULL, 0, NULL);
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

void inv_strip_to_corpse(InvTree *body, u16 *quick, int quick_n, InvTree *corpse, const ItemDB *db) {
    InvCtx ctx = inv_ctx_make(db, body, quick, quick_n, corpse);
    Work W;
    work_begin(&W, &ctx, NULL);
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *it = &W.t[INV_SIDE_SELF]->items[i];
        if (!inv_alive(it) || it->parent || it->loc == INV_LOC_EQUIP + EQ_SECURE) continue;
        InvPlace p = place_of(it);
        if (check(&W, INV_SIDE_LOOT, it, INV_SIDE_SELF, p, 0, NULL) != IIR_OK && !find_space(&W, INV_SIDE_LOOT, it, INV_SIDE_SELF, &p)) {
            inv_free_subtree(W.t[INV_SIDE_SELF], it->uid);   // the corpse is full: lost
            continue;
        }
        transfer(&W, INV_SIDE_SELF, it->uid, INV_SIDE_LOOT, p);
    }
    commit(&W, &ctx);
}
