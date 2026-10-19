// Grid inventory (docs/INVENTORY.md): definitions + class defaults, the fit / rotation / filter /
// nesting matrix over every item and container, stacking, quick-move order, swaps, attachment
// footprints, folding, the secure container on death, the corpse tree, the wire format, a fuzz of
// random intents (the tree invariants always hold, nothing is created or lost), and two clients
// looting one crate through a 150 ms host round trip with conflicting moves (no duplication or
// loss, every copy converges); binds of what comes over inside a container (a corpse's rig, a dropped
// rig picked up again); every stocked world container's loot fits its root (the arguments: level and
// building-kit prefab files). Run from the build output directory (data/, assets/).
#include "gameplay/inventory.h"
#include "gameplay/inventory/inv_wire.h"
#include "gameplay/attachments.h"
#include "core/engine.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;
static ItemDB g_db;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static u16 I(const char *id) {
    u16 it = item_find(&g_db, id);
    if (!it) { printf("FAIL: no item %s\n", id); g_fail++; }
    return it;
}

static u32 g_rng = 12345;
static u32 rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

// ------------------------------------------------------------------ invariants

// Every live item lies inside its compartment, overlaps nothing, has a live container parent,
// no cycles, counts within the stack size, equipment slots hold what they accept.
static bool tree_ok(const InvTree *t, const char *what) {
    bool ok = true;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *a = &t->items[i];
        if (!inv_alive(a)) continue;
        const ItemDef *d = item_def(&g_db, a->s.item);
        if (a->s.count > DG_MAX(1, d->max_stack)) { printf("  %s: %s count %d > stack\n", what, d->id, a->s.count); ok = false; }
        for (int j = i + 1; j < INV_MAX_ITEMS; j++)
            if (inv_alive(&t->items[j]) && t->items[j].uid == a->uid) { printf("  %s: duplicate uid %d\n", what, a->uid); ok = false; }
        if (a->loc >= INV_LOC_EQUIP) {
            int slot = a->loc - INV_LOC_EQUIP;
            if (a->parent || !inv_root_def(t, &g_db)->equip || !inv_equip_accepts(slot, d)) { printf("  %s: %s in slot %d\n", what, d->id, slot); ok = false; }
            for (int j = i + 1; j < INV_MAX_ITEMS; j++)
                if (inv_alive(&t->items[j]) && !t->items[j].parent && t->items[j].loc == a->loc) { printf("  %s: two in slot %d\n", what, slot); ok = false; }
            continue;
        }
        const InvContainerDef *cd = inv_parent_def(t, &g_db, a->parent);
        if (!cd || a->loc >= cd->comp_count) { printf("  %s: %s has no container (parent %d loc %d)\n", what, d->id, a->parent, a->loc); ok = false; continue; }
        if (a->parent && inv_is_ancestor(t, a->uid, a->parent)) { printf("  %s: cycle at %d\n", what, a->uid); ok = false; }
        int w, h;
        inv_item_size(&g_db, a, &w, &h);
        if (a->x + w > cd->comp[a->loc].w || a->y + h > cd->comp[a->loc].h) { printf("  %s: %s out of bounds\n", what, d->id); ok = false; }
        if (inv_accepts(&g_db, cd, &a->s, w, h) != IIR_OK) { printf("  %s: %s breaks %s's filter\n", what, d->id, cd->id); ok = false; }
        for (int j = i + 1; j < INV_MAX_ITEMS; j++) {
            const InvItem *b = &t->items[j];
            if (!inv_alive(b) || b->parent != a->parent || b->loc != a->loc) continue;
            int bw, bh;
            inv_item_size(&g_db, b, &bw, &bh);
            if (a->x < b->x + bw && b->x < a->x + w && a->y < b->y + bh && b->y < a->y + h) {
                printf("  %s: %s overlaps %s\n", what, d->id, item_def(&g_db, b->s.item)->id);
                ok = false;
            }
        }
    }
    return ok;
}

static void totals(const InvTree *t, int *per_item) {
    for (int i = 0; i < INV_MAX_ITEMS; i++)
        if (inv_alive(&t->items[i])) per_item[t->items[i].s.item] += t->items[i].s.count;
}

static InvIntent intent(int type, int ss, u16 src, int ds, u16 dst, int loc, int x, int y, int rot) {
    InvIntent in;
    memset(&in, 0, sizeof(in));
    in.type = (u8)type;
    in.src_side = (u8)ss;
    in.dst_side = (u8)ds;
    in.src = src;
    in.dst = dst;
    in.loc = (u8)loc;
    in.x = (u8)x;
    in.y = (u8)y;
    in.rot = (u8)rot;
    return in;
}

static int apply(Inventory *inv, InvTree *loot, InvIntent in) {
    InvCtx c = inv_ctx(inv, &g_db, loot);
    return inv_intent_apply(&c, &in, NULL);
}

static u16 give(Inventory *inv, const char *id, int count) {
    u16 it = I(id);
    CHECK(inv_add(inv, &g_db, it, count, 0) == 0, "give %s x%d", id, count);
    InvItem *e = NULL;
    for (int i = 0; i < INV_MAX_ITEMS; i++)
        if (inv_alive(&inv->t.items[i]) && inv->t.items[i].s.item == it) e = &inv->t.items[i];
    return e ? e->uid : 0;
}

static void fresh(Inventory *inv) {
    memset(inv, 0, sizeof(*inv));
    inv_init(inv, &g_db);
}

// ------------------------------------------------------------------ definitions

static void test_defs(void) {
    CHECK(container_find(&g_db, "body") == 0, "body is container 0 (a zeroed tree's root)");
    struct { const char *id; int w, h; } exp[] = {
        { "pistol", 2, 1 }, { "smg", 3, 2 }, { "rifle", 4, 2 }, { "lmg", 5, 2 }, { "sniper", 5, 2 }, { "medkit", 2, 2 }, { "bandage", 1, 1 },
        { "frag_grenade", 1, 1 }, { "fuel_can", 2, 3 }, { "phone", 1, 1 }, { "syringe", 1, 1 }, { "dna_sample", 1, 1 }, { "tire_repair", 2, 2 },
    };
    for (int i = 0; i < (int)ARRAY_COUNT(exp); i++) {
        const ItemDef *d = item_def(&g_db, I(exp[i].id));
        CHECK(d->grid_w == exp[i].w && d->grid_h == exp[i].h, "%s is %dx%d (got %dx%d)", exp[i].id, exp[i].w, exp[i].h, d->grid_w, d->grid_h);
    }
    for (int i = 1; i < g_db.item_count; i++) {
        const ItemDef *d = &g_db.items[i];
        CHECK(d->grid_w >= 1 && d->grid_h >= 1 && d->inv_weight >= 0, "%s has a grid size and a weight", d->id);
        if (d->kind == ITEM_WEAPON) CHECK(d->equip_slot != EQ_NONE, "weapon %s has an equipment slot", d->id);
    }
    CHECK(item_def(&g_db, I("pistol"))->equip_slot == EQ_HOLSTER && item_def(&g_db, I("knife"))->equip_slot == EQ_MELEE, "pistol -> holster, knife -> melee");
    CHECK(item_container(&g_db, I("rig_chest")) && item_container(&g_db, I("rig_chest"))->comp_count == 6, "chest rig: six pouches");
    u8 sup = attach_find(&g_db, "suppressor");
    CHECK(sup && g_db.attachments[sup].grid_dw == 1, "a suppressor adds a column");
}

// Every item into every container (empty, at 0,0 unrotated and rotated): the rule agrees with
// an independent reading of the definitions.
static void test_fit_matrix(void) {
    int pairs = 0;
    for (int c = 0; c < g_db.container_count; c++) {
        const InvContainerDef *cd = &g_db.containers[c];
        InvTree t;
        inv_tree_init(&t, c);
        for (int i = 1; i < g_db.item_count; i++) {
            const ItemDef *d = &g_db.items[i];
            for (int rot = 0; rot < 2; rot++) {
                InvItem it;
                memset(&it, 0, sizeof(it));
                it.s.item = (u16)i;
                it.s.count = 1;
                InvPlace p = { 0, 0, 0, 0, (u8)rot };
                int r = inv_check_place(&t, &g_db, &it, p, 0, NULL);
                int w = rot ? d->grid_h : d->grid_w, h = rot ? d->grid_w : d->grid_h;
                bool kind_ok = (!cd->allow_kinds || (cd->allow_kinds & (1u << d->kind))) && !(cd->deny_kinds & (1u << d->kind));
                for (int k = 0; k < cd->deny_count; k++) kind_ok &= cd->deny_items[k] != i;
                if (cd->max_w) kind_ok &= DG_MIN(w, h) <= DG_MIN(cd->max_w, cd->max_h) && DG_MAX(w, h) <= DG_MAX(cd->max_w, cd->max_h);
                bool nest_ok = !(cd->no_containers && d->container >= 0);
                bool fits = w <= cd->comp[0].w && h <= cd->comp[0].h;
                bool rot_ok = !rot || d->rotatable;
                bool expect = kind_ok && nest_ok && fits && rot_ok;
                CHECK((r == IIR_OK) == expect, "%s (rot %d) into %s: got %s", d->id, rot, cd->id, inv_result_text(r));
                pairs++;
            }
        }
    }
    printf("  fit matrix: %d item x container x rotation cases\n", pairs);
}

static void test_rotation_stack(void) {
    InvTree t;
    inv_tree_init(&t, container_find(&g_db, "crate"));
    InvSlot ps = { I("pistol"), 1, 0, { 0 }, 0 };
    InvItem *p = inv_alloc(&t, &g_db, &ps);
    int w, h;
    inv_item_size(&g_db, p, &w, &h);
    CHECK(w == 2 && h == 1, "pistol 2x1");
    p->flags |= INVF_ROT;
    inv_item_size(&g_db, p, &w, &h);
    CHECK(w == 1 && h == 2, "rotated pistol 1x2");
    // Rotation blocked by a neighbour.
    Inventory inv;
    fresh(&inv);
    InvTree crate;
    inv_tree_init(&crate, container_find(&g_db, "crate_small"));   // 4x3
    InvSlot a = { I("pistol"), 1, 0, { 0 }, 0 }, b = { I("bandage"), 1, 0, { 0 }, 0 };
    InvItem *pa = inv_alloc(&crate, &g_db, &a);
    InvItem *pb = inv_alloc(&crate, &g_db, &b);
    pb->x = 0; pb->y = 1;
    u16 pu = pa->uid, bu = pb->uid;
    InvIntent rot = intent(II_ROTATE, INV_SIDE_LOOT, pu, 0, 0, 0, 0, 0, 0);
    CHECK(apply(&inv, &crate, rot) == IIR_NO_ROOM, "rotation blocked by the bandage below");
    InvIntent mv = intent(II_MOVE, INV_SIDE_LOOT, bu, INV_SIDE_LOOT, 0, 0, 3, 2, 0);
    CHECK(apply(&inv, &crate, mv) == IIR_OK, "bandage moved away");
    CHECK(apply(&inv, &crate, rot) == IIR_OK, "rotation now fits");
    CHECK(inv_get(&crate, pu)->flags & INVF_ROT, "rotated");
    // Stacks: merge with a remainder; a split drag.
    fresh(&inv);
    give(&inv, "secure_alpha", 1);
    InvTree c2;
    inv_tree_init(&c2, container_find(&g_db, "crate"));
    InvSlot s1 = { I("ammo_9mm"), 50, 0, { 0 }, 0 }, s2 = { I("ammo_9mm"), 30, 0, { 0 }, 0 };
    InvItem *x1 = inv_alloc(&c2, &g_db, &s1), *x2 = inv_alloc(&c2, &g_db, &s2);
    x2->x = 2;
    u16 u1 = x1->uid, u2 = x2->uid;
    CHECK(apply(&inv, &c2, intent(II_MOVE, INV_SIDE_LOOT, u2, INV_SIDE_LOOT, 0, 0, 0, 0, 0)) == IIR_OK, "merge onto a stack");
    CHECK(inv_get(&c2, u1)->s.count == 60 && inv_get(&c2, u2)->s.count == 20, "60 / 20 remainder (%d / %d)", inv_get(&c2, u1)->s.count,
          inv_get(&c2, u2) ? inv_get(&c2, u2)->s.count : -1);
    InvIntent sp = intent(II_MOVE, INV_SIDE_LOOT, u2, INV_SIDE_LOOT, 0, 0, 5, 3, 0);
    sp.count = 5;
    CHECK(apply(&inv, &c2, sp) == IIR_OK && inv_get(&c2, u2)->s.count == 15 && inv_count(&inv, I("ammo_9mm")) == 0, "split drag: 5 off");
    int tot[MAX_ITEM_DEFS] = { 0 };
    totals(&c2, tot);
    CHECK(tot[I("ammo_9mm")] == 80, "no rounds created or lost (%d)", tot[I("ammo_9mm")]);
}

static void test_nesting(void) {
    Inventory inv;
    fresh(&inv);
    u16 hike = give(&inv, "backpack_hiking", 1);
    CHECK(inv_equipped(&inv.t, EQ_BACKPACK) && inv_equipped(&inv.t, EQ_BACKPACK)->uid == hike, "a picked-up backpack is worn");
    InvTree crate;
    inv_tree_init(&crate, container_find(&g_db, "crate"));
    InvSlot sch = { I("backpack"), 1, 0, { 0 }, 0 };
    u16 school = inv_alloc(&crate, &g_db, &sch)->uid;
    InvIntent in = intent(II_INSERT, INV_SIDE_LOOT, school, INV_SIDE_SELF, hike, 0, 0, 0, 0);
    CHECK(apply(&inv, &crate, in) == IIR_OK, "school pack (16) into the hiking pack (30)");
    InvItem *s = inv_find(&inv, I("backpack"));
    CHECK(s && s->parent == hike, "inside");
    // The hiking pack into the school pack inside it: a cycle and too large.
    in = intent(II_INSERT, INV_SIDE_SELF, hike, INV_SIDE_SELF, s->uid, 0, 0, 0, 0);
    CHECK(apply(&inv, NULL, in) == IIR_NESTING, "no cycles");
    // Same size: a second hiking pack into the worn one.
    InvSlot h2 = { I("backpack_hiking"), 1, 0, { 0 }, 0 };
    InvItem *hi2 = inv_alloc(&crate, &g_db, &h2);
    hi2->flags |= INVF_ROT;   // 5x4 in the 6x4 crate
    hi2->y = 0;
    u16 hu2 = hi2->uid;
    in = intent(II_INSERT, INV_SIDE_LOOT, hu2, INV_SIDE_SELF, hike, 0, 0, 0, 0);
    CHECK(apply(&inv, &crate, in) == IIR_NESTING, "a pack never holds one of its size");
    // The secure container takes no containers; the ammo case only ammo.
    u16 sec = give(&inv, "secure_alpha", 1);
    InvSlot ac = { I("ammo_case"), 1, 0, { 0 }, 0 };
    u16 acu = inv_alloc(&crate, &g_db, &ac)->uid;
    CHECK(apply(&inv, &crate, intent(II_INSERT, INV_SIDE_LOOT, acu, INV_SIDE_SELF, sec, 0, 0, 0, 0)) == IIR_NESTING, "no cases in the secure container");
    CHECK(apply(&inv, &crate, intent(II_INSERT, INV_SIDE_LOOT, acu, INV_SIDE_SELF, hike, 0, 0, 0, 0)) == IIR_OK, "the case into the pack");
    u16 case_uid = inv_find(&inv, I("ammo_case"))->uid;
    u16 band = give(&inv, "bandage", 1);
    CHECK(apply(&inv, NULL, intent(II_INSERT, INV_SIDE_SELF, band, INV_SIDE_SELF, case_uid, 0, 0, 0, 0)) == IIR_FILTER, "no bandage in the ammo case");
    CHECK(tree_ok(&inv.t, "nesting") && tree_ok(&crate, "nesting crate"), "invariants");
}

static void test_quick_order_and_swap(void) {
    Inventory inv;
    fresh(&inv);
    give(&inv, "rig_chest", 1);
    give(&inv, "backpack", 1);
    u16 tape = give(&inv, "duct_tape", 1);
    InvItem *t = inv_get(&inv.t, tape);
    CHECK(t && t->parent == inv_equipped(&inv.t, EQ_RIG)->uid, "pickups go to the rig first (default order)");
    inv_set_order(inv_order_parse("backpack,rig,pockets"));
    u16 cloth = give(&inv, "cloth", 1);
    CHECK(inv_get(&inv.t, cloth)->parent == inv_equipped(&inv.t, EQ_BACKPACK)->uid, "order backpack first");
    // Quick move from the backpack: the next container in the order that doesn't hold it (rig).
    CHECK(apply(&inv, NULL, intent(II_QUICK_MOVE, INV_SIDE_SELF, cloth, 0, 0, 0, 0, 0, 0)) == IIR_OK, "quick move");
    CHECK(inv_get(&inv.t, cloth)->parent == inv_equipped(&inv.t, EQ_RIG)->uid, "into the rig");
    inv_set_order(inv_order_default());
    // Quick move with a crate open: across.
    InvTree crate;
    inv_tree_init(&crate, container_find(&g_db, "crate"));
    CHECK(apply(&inv, &crate, intent(II_QUICK_MOVE, INV_SIDE_SELF, cloth, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv_live_count(&crate) == 1, "across to the crate");
    // Swap: pistol (2x1) dropped on a single 1x1 in a crate; the bandage goes where the pistol was.
    InvTree c2;
    inv_tree_init(&c2, container_find(&g_db, "crate_small"));
    InvSlot ps = { I("pistol"), 1, 0, { 0 }, 0 }, bs = { I("bandage"), 1, 0, { 0 }, 0 };
    InvItem *pp = inv_alloc(&c2, &g_db, &ps), *bb = inv_alloc(&c2, &g_db, &bs);
    pp->x = 0; pp->y = 0; bb->x = 3; bb->y = 2;
    u16 pu = pp->uid, bu = bb->uid;
    // Pistol onto the bandage's cell rotated: 1x2 at (3,1)? The target is the bandage's origin (3,2): a
    // 2x1 can't start at x 3 of a 4-wide grid -> rotated tried -> 1x2 at (3,2) doesn't fit either.
    CHECK(apply(&inv, &c2, intent(II_MOVE, INV_SIDE_LOOT, pu, INV_SIDE_LOOT, 0, 0, 3, 2, 0)) == IIR_NO_ROOM, "no room to swap there");
    bb = inv_get(&c2, bu);
    bb->x = 2; bb->y = 1;
    CHECK(apply(&inv, &c2, intent(II_MOVE, INV_SIDE_LOOT, pu, INV_SIDE_LOOT, 0, 0, 2, 1, 0)) == IIR_OK, "swap");
    CHECK(inv_get(&c2, pu)->x == 2 && inv_get(&c2, pu)->y == 1 && inv_get(&c2, bu)->x == 0 && inv_get(&c2, bu)->y == 0, "swapped places");
    // Equip swap: a second rifle onto the primary slot.
    fresh(&inv);
    give(&inv, "backpack_hiking", 1);
    u16 r1 = give(&inv, "rifle", 1);
    CHECK(inv_get(&inv.t, r1)->loc == INV_LOC_EQUIP + EQ_PRIMARY, "first rifle: primary");
    u16 r2 = give(&inv, "smg", 1);
    CHECK(inv_get(&inv.t, r2)->loc == INV_LOC_EQUIP + EQ_SECONDARY, "second long gun: secondary");
    u16 r3 = give(&inv, "dmr", 1);
    CHECK(inv_get(&inv.t, r3)->parent == inv_equipped(&inv.t, EQ_BACKPACK)->uid, "third: the backpack");
    InvIntent eq = intent(II_EQUIP, INV_SIDE_SELF, r3, INV_SIDE_SELF, 0, 0, 0, 0, 0);
    eq.aux = EQ_PRIMARY;
    CHECK(apply(&inv, NULL, eq) == IIR_OK, "equip swap");
    CHECK(inv_get(&inv.t, r3)->loc == INV_LOC_EQUIP + EQ_PRIMARY && inv_get(&inv.t, r1)->parent == inv_equipped(&inv.t, EQ_BACKPACK)->uid, "the old primary in the pack");
    // Hotbar binds follow the instance through the move.
    CHECK(inv_hot_find(&inv, I("rifle")) >= 0, "rifle bound to the hotbar");
    eq.aux = EQ_HOLSTER;
    CHECK(apply(&inv, NULL, eq) == IIR_EQUIP, "a rifle doesn't go in the holster");
    CHECK(tree_ok(&inv.t, "swap"), "invariants");
}

static void test_attachments_fold(void) {
    Inventory inv;
    fresh(&inv);
    u16 pack = give(&inv, "backpack", 1);   // 4x4
    InvTree crate;
    inv_tree_init(&crate, container_find(&g_db, "crate"));
    InvSlot rs = { I("rifle"), 1, 30, { 0 }, 0 };
    InvItem *r = inv_alloc(&crate, &g_db, &rs);
    u16 ru = r->uid;
    CHECK(apply(&inv, &crate, intent(II_MOVE, INV_SIDE_LOOT, ru, INV_SIDE_SELF, pack, 0, 0, 0, 0)) == IIR_OK, "bare rifle (4x2) into the 4x4 pack");
    InvItem *rr = inv_find(&inv, I("rifle"));
    u16 sup = item_for_attachment(&g_db, attach_find(&g_db, "suppressor"));
    InvSlot ss = { sup, 1, 0, { 0 }, 0 };
    u16 su = inv_alloc(&crate, &g_db, &ss)->uid;
    InvIntent m = intent(II_MOUNT, INV_SIDE_LOOT, su, INV_SIDE_SELF, rr->uid, 0, 0, 0, 0);
    CHECK(apply(&inv, &crate, m) == IIR_NO_ROOM, "suppressed (5x2) no longer fits the pack: rejected");
    int w, h;
    inv_item_size(&g_db, rr, &w, &h);
    CHECK(w == 4 && h == 2, "unchanged after the rejection");
    // In the hiking pack it fits.
    fresh(&inv);
    u16 hp = give(&inv, "backpack_hiking", 1);
    CHECK(apply(&inv, &crate, intent(II_MOVE, INV_SIDE_LOOT, su, INV_SIDE_SELF, hp, 0, 4, 5, 0)) == IIR_OK, "suppressor into the pack");
    InvSlot r2 = { I("rifle"), 1, 30, { 0 }, 0 };
    u16 r2u = inv_alloc(&crate, &g_db, &r2)->uid;
    CHECK(apply(&inv, &crate, intent(II_MOVE, INV_SIDE_LOOT, r2u, INV_SIDE_SELF, hp, 0, 0, 0, 0)) == IIR_OK, "rifle into the pack");
    rr = inv_find(&inv, I("rifle"));
    CHECK(apply(&inv, NULL, intent(II_MOUNT, INV_SIDE_SELF, inv_find(&inv, sup)->uid, INV_SIDE_SELF, rr->uid, 0, 0, 0, 0)) == IIR_OK, "mounted");
    rr = inv_find(&inv, I("rifle"));
    inv_item_size(&g_db, rr, &w, &h);
    CHECK(w == 5 && h == 2 && !inv_find(&inv, sup), "5x2 with the suppressor; the item consumed");
    InvIntent um = intent(II_UNMOUNT, INV_SIDE_SELF, rr->uid, INV_SIDE_SELF, INV_AUTO, 0, 0, 0, 0);
    um.aux = ATT_MUZZLE;
    CHECK(apply(&inv, NULL, um) == IIR_OK && inv_find(&inv, sup), "unmounted back into the pack");
    rr = inv_find(&inv, I("rifle"));
    inv_item_size(&g_db, rr, &w, &h);
    CHECK(w == 4, "4x2 again");
    CHECK(apply(&inv, NULL, intent(II_FOLD, INV_SIDE_SELF, rr->uid, 0, 0, 0, 0, 0, 0)) == IIR_OK, "fold");
    inv_item_size(&g_db, inv_find(&inv, I("rifle")), &w, &h);
    CHECK(w == 3, "folded 3x2");
    CHECK(apply(&inv, NULL, intent(II_UNLOAD, INV_SIDE_SELF, rr->uid, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv_count(&inv, I("ammo_rifle")) == 30, "unloaded 30 rounds");
    CHECK(tree_ok(&inv.t, "attachments"), "invariants");
}

// Every weapon x attachment: compatibility agrees with the definitions, and a mount / unmount
// round trip in a tree works (the weapon in a slot, the attachment in a 30-cell pack). Swapping
// optics brings the old one back where the new one lay; removing an extended magazine returns
// the extra rounds as a stack.
static void test_attachment_matrix(void) {
    int combos = 0, mismatch = 0;
    for (int w = 1; w < g_db.item_count; w++) {
        const WeaponDef *wd = item_weapon(&g_db, (u16)w);
        if (!wd) continue;
        for (int a = 1; a <= g_db.attachment_count; a++) {
            const AttachmentDef *ad = attach_def(&g_db, (u8)a);
            u16 ai = item_for_attachment(&g_db, (u8)a);
            CHECK(ai != ITEM_NONE && item_def(&g_db, ai)->kind == ITEM_ATTACHMENT, "attachment %s has an item", ad->id);
            InvSlot ws;
            memset(&ws, 0, sizeof(ws));
            ws.item = (u16)w;
            ws.count = 1;
            int aslot = -1;
            int r = inv_attach_check(&g_db, &ws, ai, &aslot);
            bool expect = wd->kind != WPN_MELEE && wd->att_slots[ad->slot].present && attach_allowed(wd, ad);
            if ((r == IIR_OK) != expect) mismatch++;
            if (r == IIR_OK && aslot != ad->slot) mismatch++;
            combos++;
            if (r != IIR_OK) continue;
            Inventory inv;
            fresh(&inv);
            give(&inv, "backpack_hiking", 1);
            InvItem *wi = inv_alloc(&inv.t, &g_db, &ws);
            wi->loc = INV_LOC_EQUIP + (wd->hold_style[0] && !strcmp(wd->hold_style, "pistol") ? EQ_HOLSTER : EQ_PRIMARY);
            if (!inv_equip_accepts(wi->loc - INV_LOC_EQUIP, item_def(&g_db, (u16)w))) wi->loc = INV_LOC_EQUIP + item_def(&g_db, (u16)w)->equip_slot;
            u16 wu = wi->uid;
            u16 au = give(&inv, item_def(&g_db, ai)->id, 1);
            if (apply(&inv, NULL, intent(II_MOUNT, INV_SIDE_SELF, au, INV_SIDE_SELF, wu, 0, 0, 0, 0)) != IIR_OK || inv_get(&inv.t, wu)->s.att[ad->slot] != a ||
                inv_count(&inv, ai)) { mismatch++; continue; }
            InvIntent um = intent(II_UNMOUNT, INV_SIDE_SELF, wu, INV_SIDE_SELF, INV_AUTO, 0, 0, 0, 0);
            um.aux = (u8)ad->slot;
            if (apply(&inv, NULL, um) != IIR_OK || inv_get(&inv.t, wu)->s.att[ad->slot] || inv_count(&inv, ai) != 1) mismatch++;
            if (!tree_ok(&inv.t, ad->id)) mismatch++;
        }
    }
    CHECK(combos > 50 && mismatch == 0, "attachment compatibility + mount / unmount for every weapon x attachment (%d combos, %d mismatches)", combos, mismatch);
    // Swapping optics: the red dot comes back where the scope lay.
    Inventory inv;
    fresh(&inv);
    u16 pack = give(&inv, "backpack_hiking", 1);
    u8 rd = attach_find(&g_db, "red_dot"), s4 = attach_find(&g_db, "scope_4x");
    InvSlot rs = { I("rifle"), 1, 30, { 0 }, 0 };
    rs.att[ATT_OPTIC] = rd;
    InvItem *ri = inv_alloc(&inv.t, &g_db, &rs);
    ri->loc = INV_LOC_EQUIP + EQ_PRIMARY;
    u16 ru = ri->uid;
    InvSlot ss = { item_for_attachment(&g_db, s4), 1, 0, { 0 }, 0 };
    InvItem *si = inv_alloc(&inv.t, &g_db, &ss);
    si->parent = pack;
    si->x = 3;
    si->y = 4;
    CHECK(apply(&inv, NULL, intent(II_MOUNT, INV_SIDE_SELF, si->uid, INV_SIDE_SELF, ru, 0, 0, 0, 0)) == IIR_OK && inv_get(&inv.t, ru)->s.att[ATT_OPTIC] == s4,
          "scope mounted over the red dot");
    InvItem *back = inv_find(&inv, item_for_attachment(&g_db, rd));
    CHECK(back && back->parent == pack && back->x == 3 && back->y == 4, "the red dot comes back where the scope lay");
    // Not a weapon / melee.
    u16 med = give(&inv, "medkit", 1);
    CHECK(apply(&inv, NULL, intent(II_MOUNT, INV_SIDE_SELF, back->uid, INV_SIDE_SELF, med, 0, 0, 0, 0)) == IIR_NOT_WEAPON, "onto a medkit: not a weapon");
    u16 knife = give(&inv, "knife", 1);
    CHECK(apply(&inv, NULL, intent(II_MOUNT, INV_SIDE_SELF, back->uid, INV_SIDE_SELF, knife, 0, 0, 0, 0)) == IIR_NOT_WEAPON, "melee weapons take no attachments");
    // Extended magazine removed: the extra rounds come back as a stack.
    u8 em = attach_find(&g_db, "extended_mag");
    if (em) {
        fresh(&inv);
        give(&inv, "backpack_hiking", 1);
        InvSlot r2 = { I("rifle"), 1, 0, { 0 }, 0 };
        r2.att[ATT_MAGAZINE] = em;
        WeaponDef eff;
        attach_weapon_effective(&g_db, item_weapon(&g_db, r2.item), r2.att, &eff);
        r2.ammo = (i16)eff.mag_size;
        InvItem *w2 = inv_alloc(&inv.t, &g_db, &r2);
        w2->loc = INV_LOC_EQUIP + EQ_PRIMARY;
        u16 w2u = w2->uid;
        InvIntent um = intent(II_UNMOUNT, INV_SIDE_SELF, w2u, INV_SIDE_SELF, INV_AUTO, 0, 0, 0, 0);
        um.aux = ATT_MAGAZINE;
        CHECK(apply(&inv, NULL, um) == IIR_OK, "unmount the extended magazine");
        int reserve = inv_count(&inv, item_weapon(&g_db, r2.item)->ammo_item);
        CHECK(inv_get(&inv.t, w2u)->s.ammo + reserve == eff.mag_size && inv_get(&inv.t, w2u)->s.ammo == item_weapon(&g_db, r2.item)->mag_size,
              "extra rounds back as a stack (%d + %d)", inv_get(&inv.t, w2u)->s.ammo, reserve);
    }
}

static void test_sort(void) {
    Inventory inv;
    fresh(&inv);
    u16 pack = give(&inv, "backpack_hiking", 1);
    InvTree crate;
    inv_tree_init(&crate, container_find(&g_db, "crate"));
    const char *ids[] = { "bandage", "rifle", "ammo_9mm", "medkit", "battery", "smg", "cloth" };
    for (int i = 0; i < (int)ARRAY_COUNT(ids); i++) {
        InvSlot s = { I(ids[i]), 1, 0, { 0 }, 0 };
        inv_give_root(&crate, &g_db, &s);
    }
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        InvItem *it = &crate.items[i];
        if (!inv_alive(it)) continue;
        apply(&inv, &crate, intent(II_INSERT, INV_SIDE_LOOT, it->uid, INV_SIDE_SELF, pack, 0, 0, 0, 0));
    }
    int before[MAX_ITEM_DEFS] = { 0 }, after[MAX_ITEM_DEFS] = { 0 };
    totals(&inv.t, before);
    InvIntent so = intent(II_SORT, 0, 0, INV_SIDE_SELF, pack, 0, 0, 0, 0);
    so.aux = 2;   // size
    CHECK(apply(&inv, NULL, so) == IIR_OK, "sort the pack by size");
    totals(&inv.t, after);
    CHECK(!memcmp(before, after, sizeof(before)) && tree_ok(&inv.t, "sort"), "sorting keeps every item and the invariants");
    InvItem *top = inv_at(&inv.t, &g_db, pack, 0, 0, 0);
    int w, h;
    if (top) inv_item_size(&g_db, top, &w, &h);
    CHECK(top && w * h == 8, "the largest item (a 4x2 rifle) first (%s)", top ? item_def(&g_db, top->s.item)->id : "none");
    so.aux = 0;
    CHECK(apply(&inv, NULL, so) == IIR_OK && tree_ok(&inv.t, "sort type"), "sort by type");
}

static void test_death_corpse(void) {
    Inventory inv;
    fresh(&inv);
    give(&inv, "rig_chest", 1);
    give(&inv, "backpack_hiking", 1);
    give(&inv, "rifle", 1);
    give(&inv, "pistol", 1);
    give(&inv, "helmet", 1);
    u16 sec = give(&inv, "secure_alpha", 1);
    u16 dna = give(&inv, "dna_sample", 1);
    give(&inv, "ammo_rifle", 120);
    CHECK(apply(&inv, NULL, intent(II_INSERT, INV_SIDE_SELF, dna, INV_SIDE_SELF, sec, 0, 0, 0, 0)) == IIR_OK, "DNA into the secure container");
    int before[MAX_ITEM_DEFS] = { 0 }, after[MAX_ITEM_DEFS] = { 0 };
    totals(&inv.t, before);
    InvTree corpse;
    inv_tree_init(&corpse, container_find(&g_db, "body"));
    inv_strip_to_corpse(&inv.t, inv.quick, INV_HOTBAR, &corpse, &g_db);
    CHECK(inv_count(&inv, I("dna_sample")) == 1 && inv_equipped(&inv.t, EQ_SECURE), "the secure container and its contents survive");
    CHECK(inv_live_count(&inv.t) == 2, "nothing else stays on the body (%d)", inv_live_count(&inv.t));
    CHECK(inv_equipped(&corpse, EQ_PRIMARY) && inv_equipped(&corpse, EQ_HOLSTER) && inv_equipped(&corpse, EQ_HEADWEAR) && inv_equipped(&corpse, EQ_RIG),
          "the corpse shows the equipment in its slots");
    CHECK(inv_hot_c(&inv, 0)->item == 0, "binds into what left are cleared");
    totals(&inv.t, after);
    totals(&corpse, after);
    bool same = !memcmp(before, after, sizeof(before));
    CHECK(same, "body + corpse = what was carried");
    CHECK(tree_ok(&corpse, "corpse") && tree_ok(&inv.t, "body after death"), "invariants");
}

static void test_wire(void) {
    Inventory inv;
    fresh(&inv);
    give(&inv, "backpack_hiking", 1);
    give(&inv, "rifle", 1);
    give(&inv, "medkit", 2);
    give(&inv, "ammo_9mm", 200);
    static u8 buf[INV_WIRE_TREE_MAX];
    int n = inv_wire_write_tree(&inv.t, buf, sizeof(buf));
    InvTree back;
    CHECK(n > 0 && inv_wire_read_tree(&back, buf, n) == n, "tree round trip size");
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *a = &inv.t.items[i];
        if (!inv_alive(a)) continue;
        const InvItem *b = inv_get_c(&back, a->uid);
        CHECK(b && !memcmp(&a->s, &b->s, sizeof(a->s)) && a->parent == b->parent && a->loc == b->loc && a->x == b->x && a->y == b->y && a->flags == b->flags,
              "item %d round trips", a->uid);
    }
    CHECK(back.next_uid == inv.t.next_uid && back.root == inv.t.root, "header");
    InvIntent in = intent(II_SPLIT, INV_SIDE_LOOT, 513, INV_SIDE_SELF, INV_AUTO, 3, 7, 2, 1);
    in.aux = 4;
    in.count = 777;
    in.seq = 0xDEADBEEF;
    u8 w[INV_WIRE_INTENT];
    inv_wire_write_intent(&in, w);
    InvIntent o;
    inv_wire_read_intent(&o, w);
    CHECK(!memcmp(&in, &o, sizeof(in)), "intent round trip");
    CHECK(inv_wire_read_tree(&back, buf, 3) == 0, "a truncated tree is rejected");
}

// ------------------------------------------------------------------ fuzz

static u16 pick_uid(const InvTree *t) {
    int live[INV_MAX_ITEMS], n = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) if (inv_alive(&t->items[i])) live[n++] = i;
    if (!n || rnd() % 16 == 0) return (u16)(rnd() % 300);
    return t->items[live[rnd() % n]].uid;
}

static InvIntent random_intent(const Inventory *inv, const InvTree *loot) {
    InvIntent in;
    memset(&in, 0, sizeof(in));
    in.type = (u8)(1 + rnd() % (II_COUNT - 1));
    in.src_side = (u8)(rnd() % 2);
    in.dst_side = (u8)(rnd() % 2);
    in.src = pick_uid(in.src_side ? loot : &inv->t);
    int r = rnd() % 4;
    in.dst = r == 0 ? 0 : r == 1 ? INV_AUTO : pick_uid(in.dst_side ? loot : &inv->t);
    in.loc = (u8)(rnd() % 3 == 0 ? INV_LOC_EQUIP + rnd() % EQ_COUNT : rnd() % 6);
    in.x = (u8)(rnd() % 7);
    in.y = (u8)(rnd() % 7);
    in.rot = (u8)(rnd() % 2);
    in.aux = (u8)(rnd() % 3 == 0 ? 255 : rnd() % 13);
    in.count = (u16)(rnd() % 3 ? 0 : rnd() % 40);
    if (rnd() % 64 == 0) in.type = (u8)(rnd() % 255);   // garbage
    return in;
}

static void test_fuzz(void) {
    Inventory inv;
    fresh(&inv);
    InvTree crate;
    inv_tree_init(&crate, container_find(&g_db, "crate"));
    const char *loadout[] = { "rig_chest", "backpack_hiking", "rifle", "pistol", "helmet", "secure_alpha", "medkit", "ammo_rifle", "ammo_9mm", "bandage", "backpack",
                              "ammo_case", "frag_grenade", "fuel_can", "knife" };
    for (int i = 0; i < (int)ARRAY_COUNT(loadout); i++) inv_add(&inv, &g_db, I(loadout[i]), 3, 10);
    const char *loot[] = { "smg", "sling_bag", "med_case", "ammo_shell", "water_bottle", "scope_4x", "att_suppressor", "att_red_dot", "helmet", "dmr" };
    for (int i = 0; i < (int)ARRAY_COUNT(loot); i++) {
        u16 it = item_find(&g_db, loot[i]);
        if (!it) continue;
        InvSlot s = { it, (u16)DG_MIN(2, DG_MAX(1, item_def(&g_db, it)->max_stack)), 0, { 0 }, 0 };
        attach_defaults(item_weapon(&g_db, it), s.att);
        inv_give_root(&crate, &g_db, &s);
    }
    int tot0[MAX_ITEM_DEFS] = { 0 };
    totals(&inv.t, tot0);
    totals(&crate, tot0);
    int dropped[MAX_ITEM_DEFS] = { 0 }, used[MAX_ITEM_DEFS] = { 0 };
    int ok_n = 0, bad = 0, iters = 20000;
    for (int k = 0; k < iters && !bad; k++) {
        InvIntent in = random_intent(&inv, &crate);
        InvCtx c = inv_ctx(&inv, &g_db, &crate);
        InvEffect fx;
        int r = inv_intent_apply(&c, &in, &fx);
        if (r == IIR_OK) {
            ok_n++;
            for (int d = 0; d < INV_MAX_ITEMS; d++)
                if (inv_alive(&fx.dropped.items[d])) dropped[fx.dropped.items[d].s.item] += fx.dropped.items[d].s.count;
            if (fx.used_item) used[fx.used_item]++;
        }
        if (!tree_ok(&inv.t, "fuzz self") || !tree_ok(&crate, "fuzz crate")) { bad = 1; printf("  after %s (#%d)\n", inv_intent_name(in.type), k); }
        for (int q = 0; q < INV_HOTBAR; q++)
            if (inv.quick[q] && !inv_get(&inv.t, inv.quick[q])) { bad = 1; printf("  stale bind after %s\n", inv_intent_name(in.type)); }
        // Conservation (ammo changes form: loaded rounds <-> stacks; attachments: mounted <-> items).
        if (k % 97 == 0) {
            int tot[MAX_ITEM_DEFS] = { 0 };
            totals(&inv.t, tot);
            totals(&crate, tot);
            for (int i = 1; i < g_db.item_count && !bad; i++) {
                const ItemDef *d = &g_db.items[i];
                if (d->kind == ITEM_AMMO || d->kind == ITEM_ATTACHMENT || d->kind == ITEM_CONSUMABLE) continue;
                if (tot[i] + dropped[i] != tot0[i]) { bad = 1; printf("  %s: %d + %d dropped != %d\n", d->id, tot[i], dropped[i], tot0[i]); }
            }
        }
        // Refill now and then so the fuzz keeps having material.
        if (k % 500 == 499) {
            for (int i = 0; i < (int)ARRAY_COUNT(loot); i++) {
                u16 it = item_find(&g_db, loot[i]);
                if (!it) continue;
                InvSlot s = { it, 1, 0, { 0 }, 0 };
                int left = inv_give_root(&crate, &g_db, &s);
                tot0[it] += 1 - left;
            }
        }
    }
    (void)used;
    CHECK(!bad, "fuzz: the invariants and conservation hold over %d random intents", iters);
    CHECK(ok_n > iters / 50, "fuzz: enough intents applied to mean something (%d)", ok_n);
    printf("  fuzz: %d / %d random intents applied\n", ok_n, iters);
}

// ------------------------------------------------------------------ two clients, one crate

// Host: the crate + both players' trees. Clients: predicted copies (their own tree + the crate),
// unacknowledged intents re-applied on every state message. Messages take 150 ms each way
// (9 ticks at 60 Hz). Both clients race for the same items and move things around.
#define LAG 9
typedef struct Msg { int at; int kind; int who; InvIntent in; Inventory inv; InvTree crate; u32 ack; } Msg;   // kind 0: intent, 1: state
static Msg g_q[4096];
static int g_qn;

typedef struct Client {
    Inventory inv;       // predicted
    InvTree crate;       // predicted
    InvIntent pending[256];
    int pending_n;
    u32 seq;
} Client;

static void test_two_clients(void) {
    Inventory host_inv[2];
    InvTree host_crate;
    u32 acked[2] = { 0, 0 };
    fresh(&host_inv[0]);
    fresh(&host_inv[1]);
    give(&host_inv[0], "backpack_hiking", 1);
    give(&host_inv[1], "backpack", 1);
    inv_tree_init(&host_crate, container_find(&g_db, "crate"));
    const char *loot[] = { "rifle", "pistol", "medkit", "ammo_rifle", "bandage", "frag_grenade", "helmet", "knife", "battery", "cloth", "smg", "scrap" };
    for (int i = 0; i < (int)ARRAY_COUNT(loot); i++) {
        InvSlot s = { I(loot[i]), (u16)DG_MIN(3, DG_MAX(1, item_def(&g_db, I(loot[i]))->max_stack)), 0, { 0 }, 0 };
        inv_give_root(&host_crate, &g_db, &s);
    }
    int tot0[MAX_ITEM_DEFS] = { 0 };
    totals(&host_crate, tot0);
    totals(&host_inv[0].t, tot0);
    totals(&host_inv[1].t, tot0);
    Client cl[2];
    memset(cl, 0, sizeof(cl));
    for (int c = 0; c < 2; c++) { cl[c].inv = host_inv[c]; cl[c].crate = host_crate; }
    g_qn = 0;
    int rejected = 0, applied = 0, conflicts = 0;
    for (int tick = 0; tick < 2400; tick++) {
        // Clients: an intent now and then (both mostly grab from the crate, the same items).
        for (int c = 0; c < 2 && tick < 2000; c++) {
            if (rnd() % 5) continue;
            Client *C = &cl[c];
            InvIntent in;
            memset(&in, 0, sizeof(in));
            int roll = rnd() % 10;
            if (roll < 5) { in.type = II_QUICK_MOVE; in.src_side = INV_SIDE_LOOT; in.src = pick_uid(&C->crate); }
            else if (roll < 7) { in.type = II_QUICK_MOVE; in.src_side = INV_SIDE_SELF; in.src = pick_uid(&C->inv.t); }
            else if (roll < 9) { in = random_intent(&C->inv, &C->crate); if (in.type == II_DISCARD || in.type == II_USE) in.type = II_MOVE; }
            else { in.type = II_TAKE_ALL; }
            in.seq = ++C->seq;
            InvCtx ctx = inv_ctx(&C->inv, &g_db, &C->crate);
            inv_intent_apply(&ctx, &in, NULL);   // prediction
            if (C->pending_n < 256) C->pending[C->pending_n++] = in;
            Msg *m = &g_q[g_qn++ % 4096];
            memset(m, 0, sizeof(*m));
            m->at = tick + LAG;
            m->kind = 0;
            m->who = c;
            m->in = in;
        }
        // Deliver due messages in order.
        for (int i = 0; i < g_qn; i++) {
            Msg *m = &g_q[i % 4096];
            if (m->at != tick) continue;
            if (m->kind == 0) {
                InvCtx ctx = inv_ctx(&host_inv[m->who], &g_db, &host_crate);
                InvEffect fx;
                int r = inv_intent_apply(&ctx, &m->in, &fx);
                if (r == IIR_OK) applied++;
                else { rejected++; if (r == IIR_GONE || r == IIR_NO_ROOM) conflicts++; }
                acked[m->who] = m->in.seq;
                // State to both (the crate changed for both; each its own inventory).
                for (int c = 0; c < 2; c++) {
                    Msg *s = &g_q[g_qn++ % 4096];
                    memset(s, 0, sizeof(*s));
                    s->at = tick + LAG;
                    s->kind = 1;
                    s->who = c;
                    s->inv = host_inv[c];
                    s->crate = host_crate;
                    s->ack = acked[c];
                }
            } else {
                // Reconcile: take the host's state, drop acknowledged intents, re-apply the rest.
                Client *C = &cl[m->who];
                C->inv = m->inv;
                C->crate = m->crate;
                int n = 0;
                for (int k = 0; k < C->pending_n; k++) if (C->pending[k].seq > m->ack) C->pending[n++] = C->pending[k];
                C->pending_n = n;
                for (int k = 0; k < n; k++) {
                    InvCtx ctx = inv_ctx(&C->inv, &g_db, &C->crate);
                    inv_intent_apply(&ctx, &C->pending[k], NULL);
                }
            }
        }
        CHECK(g_qn < 4000, "queue");
        if (g_qn >= 4000) break;
        // Compact the queue: drop delivered messages.
        int w = 0;
        for (int i = 0; i < g_qn; i++) if (g_q[i].at > tick) g_q[w++] = g_q[i];
        g_qn = w;
    }
    int tot[MAX_ITEM_DEFS] = { 0 };
    totals(&host_crate, tot);
    totals(&host_inv[0].t, tot);
    totals(&host_inv[1].t, tot);
    bool same = true;
    for (int i = 1; i < g_db.item_count; i++) {
        const ItemDef *d = &g_db.items[i];
        if (d->kind == ITEM_AMMO || d->kind == ITEM_ATTACHMENT) continue;   // rounds / attachments change form (unload, mount)
        if (tot[i] != tot0[i]) { same = false; printf("  %s: %d != %d\n", d->id, tot[i], tot0[i]); }
    }
    CHECK(same, "two clients, one crate, 150 ms: nothing duplicated or lost");
    for (int c = 0; c < 2; c++) {
        CHECK(cl[c].pending_n == 0, "client %d: every intent acknowledged", c);
        CHECK(!memcmp(&cl[c].inv.t, &host_inv[c].t, sizeof(InvTree)) && !memcmp(cl[c].inv.quick, host_inv[c].quick, sizeof(host_inv[c].quick)),
              "client %d converged on its inventory", c);
        CHECK(!memcmp(&cl[c].crate, &host_crate, sizeof(InvTree)), "client %d converged on the crate", c);
    }
    CHECK(tree_ok(&host_crate, "race crate") && tree_ok(&host_inv[0].t, "race 0") && tree_ok(&host_inv[1].t, "race 1"), "invariants");
    CHECK(conflicts > 0, "the race produced conflicts (%d)", conflicts);
    printf("  two clients: %d applied, %d rejected (%d conflicts)\n", applied, rejected, conflicts);
}

static void test_weight(void) {
    Inventory inv;
    fresh(&inv);
    give(&inv, "backpack_hiking", 1);
    give(&inv, "ammo_rifle", 90);
    float expect = item_def(&g_db, I("backpack_hiking"))->inv_weight + 90 * item_def(&g_db, I("ammo_rifle"))->inv_weight;
    CHECK(fabsf(inv_tree_weight(&inv.t, &g_db) - expect) < 1e-3f, "total weight %.3f == %.3f", inv_tree_weight(&inv.t, &g_db), expect);
    const InvItem *bp = inv_equipped_c(&inv.t, EQ_BACKPACK);
    CHECK(fabsf(inv_item_weight(&inv.t, &g_db, bp) - inv_tree_weight(&inv.t, &g_db)) < 1e-3f || inv_find(&inv, I("ammo_rifle"))->parent != bp->uid,
          "a container weighs its contents");
}

// The hotbar takes only what is used from the hands: no worn container, not the built-in phone; a
// "hotbar": true tool (the flashlight) binds like a weapon.
static void test_binds(void) {
    Inventory inv;
    fresh(&inv);
    u16 bp = give(&inv, "backpack_hiking", 1);
    u16 rig = give(&inv, "rig_chest", 1);
    InvIntent b = intent(II_BIND, INV_SIDE_SELF, bp, 0, 0, 0, 0, 0, 0);
    b.aux = 0;
    CHECK(apply(&inv, NULL, b) == IIR_NOT_USABLE && !inv.quick[0], "a worn backpack can't be bound");
    b.src = rig;
    CHECK(apply(&inv, NULL, b) == IIR_NOT_USABLE && !inv.quick[0], "a worn rig can't be bound");
    u16 phone = item_find(&g_db, "phone");
    if (phone && item_def(&g_db, phone)->builtin) {
        inv_add(&inv, &g_db, phone, 1, 0);
        CHECK(inv_hot_find(&inv, phone) < 0, "give phone: the built-in phone is never bound to a hotbar slot");
        InvItem *pi = inv_find(&inv, phone);
        if (pi) {
            b.src = pi->uid;
            CHECK(apply(&inv, NULL, b) == IIR_NOT_USABLE, "nor by hand");
        }
    }
    u16 fl = item_find(&g_db, "flashlight");
    if (fl) {
        inv_add(&inv, &g_db, fl, 1, 0);
        CHECK(inv_hot_find(&inv, fl) >= 0, "the flashlight (\"hotbar\": true) binds like a weapon");
    }
    u16 med = give(&inv, "medkit", 1);
    b.src = med;
    b.aux = 5;
    CHECK(apply(&inv, NULL, b) == IIR_OK && inv.quick[5] == med, "a consumable binds by hand");
}

// Weapons / consumables / throwables taken from a world container bind like a pickup, by every path:
// quick move, take all, onto an equipment slot, a drag into a grid.
static void test_loot_binds(void) {
    Inventory inv;
    fresh(&inv);
    give(&inv, "backpack_hiking", 1);
    InvTree crate;
    inv_tree_init(&crate, container_find(&g_db, "crate"));
    const char *loot[] = { "rifle", "medkit", "frag_grenade", "pistol", "bandage" };
    u16 lu[5];
    for (int i = 0; i < 5; i++) {
        InvSlot s = { I(loot[i]), 1, 0, { 0 }, 0 };
        attach_defaults(item_weapon(&g_db, s.item), s.att);
        inv_give_root(&crate, &g_db, &s);
        lu[i] = 0;
        for (int k = 0; k < INV_MAX_ITEMS; k++)
            if (inv_alive(&crate.items[k]) && crate.items[k].s.item == s.item) lu[i] = crate.items[k].uid;
    }
    CHECK(apply(&inv, &crate, intent(II_QUICK_MOVE, INV_SIDE_LOOT, lu[0], 0, 0, 0, 0, 0, 0)) == IIR_OK, "rifle: quick move out of the crate");
    CHECK(inv_hot_find(&inv, I("rifle")) >= 0, "a looted rifle (Ctrl+click) is bound, so it can be drawn");
    InvIntent eq = intent(II_EQUIP, INV_SIDE_LOOT, lu[3], 0, 0, 0, 0, 0, 0);
    eq.aux = 255;
    CHECK(apply(&inv, &crate, eq) == IIR_OK && inv_equipped(&inv.t, EQ_HOLSTER), "pistol: dragged onto the holster");
    CHECK(inv_hot_find(&inv, I("pistol")) >= 0, "a pistol dragged onto its slot is bound");
    InvIntent mv = intent(II_MOVE, INV_SIDE_LOOT, lu[4], INV_SIDE_SELF, inv_equipped(&inv.t, EQ_BACKPACK)->uid, 0, 4, 5, 0);
    CHECK(apply(&inv, &crate, mv) == IIR_OK && inv_hot_find(&inv, I("bandage")) >= 0, "a bandage dragged into the backpack is bound");
    InvIntent ta = intent(II_TAKE_ALL, INV_SIDE_LOOT, 0, INV_SIDE_SELF, 0, 0, 0, 0, 0);
    CHECK(apply(&inv, &crate, ta) == IIR_OK && inv_hot_find(&inv, I("medkit")) >= 0 && inv_hot_find(&inv, I("frag_grenade")) >= 0, "take all binds the medkit and the grenade");
    for (int k = 0; k < INV_HOTBAR; k++) CHECK(!inv.quick[k] || inv_get(&inv.t, inv.quick[k]), "binds stay valid");
    // A full hotbar: the next looted weapon is carried unbound (as a pickup).
    InvSlot s = { I("smg"), 1, 0, { 0 }, 0 };
    inv_give_root(&crate, &g_db, &s);
    InvItem *sm = NULL;
    for (int k = 0; k < INV_MAX_ITEMS; k++) if (inv_alive(&crate.items[k]) && crate.items[k].s.item == s.item) sm = &crate.items[k];
    bool full = true;
    for (int k = 0; k < INV_HOTBAR; k++) full &= inv.quick[k] != 0;
    if (sm && full) CHECK(apply(&inv, &crate, intent(II_QUICK_MOVE, INV_SIDE_LOOT, sm->uid, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv_hot_find(&inv, s.item) < 0, "hotbar full: taken, not bound");
    CHECK(tree_ok(&inv.t, "loot binds"), "invariants");
}

// The pickup's tree of a dropped container (entities.c spawn_pickup_tree): the container item's grid
// is its root, its contents under it as they lay.
static void pickup_tree(const InvTree *dropped, u16 top, InvTree *pk) {
    inv_tree_init(pk, item_def(&g_db, inv_get_c(dropped, top)->s.item)->container);
    int idx[INV_MAX_ITEMS];
    int n = inv_subtree(dropped, top, idx, INV_MAX_ITEMS);
    u16 ou[INV_MAX_ITEMS], nu[INV_MAX_ITEMS];
    for (int k = 1; k < n; k++) {
        const InvItem *src = &dropped->items[idx[k]];
        InvItem *dst = inv_alloc(pk, &g_db, &src->s);
        dst->flags = src->flags, dst->loc = src->loc, dst->x = src->x, dst->y = src->y, dst->parent = 0;
        for (int j = 1; j < k; j++) if (ou[j] == src->parent) dst->parent = nu[j];
        ou[k] = src->uid, nu[k] = dst->uid;
    }
}

// A new instance of id x count inside container uid parent of t, at its first free space.
static u16 put_in(InvTree *t, u16 parent, const char *id, int count) {
    InvSlot s = { I(id), (u16)count, 0, { 0 }, 0 };
    InvItem probe;
    memset(&probe, 0, sizeof(probe));
    probe.s = s;
    InvPlace p;
    if (!inv_find_space(t, &g_db, &probe, parent, &p)) { CHECK(false, "no room for %s", id); return 0; }
    InvItem *it = inv_alloc(t, &g_db, &s);
    it->parent = p.parent, it->loc = p.loc, it->x = p.x, it->y = p.y;
    if (p.rot) it->flags |= INVF_ROT;
    return it->uid;
}

// A discarded container keeps everything in it (more entries than the old 32-drop limit) as one
// subtree, and picked up again (the pickup's tree) it comes back with the same layout.
static void test_container_drop(void) {
    Inventory inv;
    fresh(&inv);
    u16 bp = give(&inv, "backpack_hiking", 1);
    const InvContainerDef *bc = item_container(&g_db, I("backpack_hiking"));
    u16 arm = I("armband"), gl = I("glasses"), sling = I("sling_bag");
    InvSlot ss = { sling, 1, 0, { 0 }, 0 };
    InvItem *sb = inv_alloc(&inv.t, &g_db, &ss);
    sb->parent = bp, sb->loc = 0, sb->x = 0, sb->y = 0;
    u16 sbu = sb->uid;
    int placed = 0;
    for (int y = 0; y < bc->comp[0].h; y++)
        for (int x = 0; x < bc->comp[0].w; x++) {
            if (x < 2 && y < 2) continue;   // the sling bag
            InvSlot a = { arm, 1, 0, { 0 }, 0 };
            InvItem *it = inv_alloc(&inv.t, &g_db, &a);
            it->parent = bp, it->loc = 0, it->x = (u8)x, it->y = (u8)y;
            placed++;
        }
    const InvContainerDef *sc = item_container(&g_db, sling);
    for (int y = 0; y < sc->comp[0].h; y++)
        for (int x = 0; x < sc->comp[0].w; x++) {
            InvSlot g = { gl, 1, 0, { 0 }, 0 };
            InvItem *it = inv_alloc(&inv.t, &g_db, &g);
            it->parent = sbu, it->loc = 0, it->x = (u8)x, it->y = (u8)y;
            placed++;
        }
    CHECK(tree_ok(&inv.t, "full pack") && placed + 1 > 32, "a backpack with %d entries in it", placed + 1);
    int before[MAX_ITEM_DEFS] = { 0 };
    totals(&inv.t, before);
    InvCtx c = inv_ctx(&inv, &g_db, NULL);
    InvEffect fx;
    InvIntent d = intent(II_DISCARD, INV_SIDE_SELF, bp, 0, 0, 0, 0, 0, 0);
    CHECK(inv_intent_apply(&c, &d, &fx) == IIR_OK, "discard the full backpack");
    CHECK(inv_live_count(&inv.t) == 0, "nothing of it stays on the body");
    CHECK(inv_live_count(&fx.dropped) == placed + 2, "every entry is in the drop (%d of %d)", inv_live_count(&fx.dropped), placed + 2);
    int tops = 0;
    u16 top = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++)
        if (inv_alive(&fx.dropped.items[i]) && !fx.dropped.items[i].parent) tops++, top = fx.dropped.items[i].uid;
    CHECK(tops == 1 && inv_get(&fx.dropped, top)->s.item == I("backpack_hiking"), "one pickup: the backpack, its contents under it");
    InvTree pk;
    pickup_tree(&fx.dropped, top, &pk);
    CHECK(tree_ok(&pk, "pickup tree"), "the pickup's tree is a valid backpack grid");
    Inventory other;
    fresh(&other);
    InvSlot top_s = inv_get(&fx.dropped, top)->s;
    CHECK(inv_add_with(&other, &g_db, &top_s, &pk) == 0, "picked up whole");
    int after[MAX_ITEM_DEFS] = { 0 };
    totals(&other.t, after);
    CHECK(!memcmp(before, after, sizeof(before)), "the same items");
    const InvItem *nb = inv_equipped_c(&other.t, EQ_BACKPACK);
    CHECK(nb && inv_cells_used(&other.t, &g_db, nb->uid) == bc->capacity, "worn, full");
    const InvItem *corner = nb ? inv_at(&other.t, &g_db, nb->uid, 0, bc->comp[0].w - 1, bc->comp[0].h - 1) : NULL;
    CHECK(corner && corner->s.item == arm, "the layout kept (an armband in the last cell)");
    const InvItem *nsb = nb ? inv_at(&other.t, &g_db, nb->uid, 0, 0, 0) : NULL;
    CHECK(nsb && nsb->s.item == sling && inv_cells_used(&other.t, &g_db, nsb->uid) == sc->capacity, "the sling bag inside, still full");
    CHECK(tree_ok(&other.t, "picked up"), "invariants");
    // All or nothing: a second one doesn't fit anywhere.
    Inventory keep = other;
    CHECK(inv_add_with(&other, &g_db, &top_s, &pk) == 1 && !memcmp(&keep.t, &other.t, sizeof(keep.t)), "no room: nothing changes");
}

// Take all: a corpse's backpack that can't come over (the player wears one of its size) is emptied.
static void test_take_all_contents(void) {
    Inventory inv;
    fresh(&inv);
    give(&inv, "backpack_hiking", 1);
    InvTree corpse;
    inv_tree_init(&corpse, container_find(&g_db, "body"));
    InvSlot bs = { I("backpack_hiking"), 1, 0, { 0 }, 0 };
    InvItem *cb = inv_alloc(&corpse, &g_db, &bs);
    cb->loc = INV_LOC_EQUIP + EQ_BACKPACK;
    u16 cbu = cb->uid;
    InvSlot ms = { I("medkit"), 1, 0, { 0 }, 0 }, as = { I("ammo_rifle"), 30, 0, { 0 }, 0 };
    InvItem *m = inv_alloc(&corpse, &g_db, &ms);
    m->parent = cbu, m->x = 0, m->y = 0;
    InvItem *a = inv_alloc(&corpse, &g_db, &as);
    a->parent = cbu, a->x = 3, a->y = 0;
    CHECK(tree_ok(&corpse, "corpse"), "a corpse wearing a full-size pack");
    CHECK(apply(&inv, &corpse, intent(II_TAKE_ALL, INV_SIDE_LOOT, 0, INV_SIDE_SELF, 0, 0, 0, 0, 0)) == IIR_OK, "take all");
    CHECK(inv_count(&inv, I("medkit")) == 1 && inv_count(&inv, I("ammo_rifle")) == 30, "its contents came over");
    CHECK(inv_get(&corpse, cbu) && inv_live_count(&corpse) == 1, "the pack itself stays, empty");
}

// Binds of a container's contents: a fresh player taking all from a corpse wearing a rig gets the
// rifle and the medkits / grenades / bandages in the rig bound (not only the rig); the rig dropped
// (discarded) and picked up again comes back with its consumables bound.
static void test_container_binds(void) {
    static const char *meds[] = { "medkit", "frag_grenade", "bandage" };
    Inventory inv;
    fresh(&inv);
    InvTree corpse;
    inv_tree_init(&corpse, container_find(&g_db, "body"));
    InvSlot rs = { I("rifle"), 1, 0, { 0 }, 0 };
    attach_defaults(item_weapon(&g_db, rs.item), rs.att);
    InvItem *rf = inv_alloc(&corpse, &g_db, &rs);
    rf->loc = INV_LOC_EQUIP + EQ_PRIMARY;
    InvSlot gs = { I("rig_chest"), 1, 0, { 0 }, 0 };
    InvItem *rig = inv_alloc(&corpse, &g_db, &gs);
    rig->loc = INV_LOC_EQUIP + EQ_RIG;
    u16 ru = rig->uid;
    put_in(&corpse, ru, "medkit", 2);
    put_in(&corpse, ru, "frag_grenade", 2);
    put_in(&corpse, ru, "bandage", 3);
    CHECK(tree_ok(&corpse, "corpse rig"), "a corpse wearing a rifle and a stocked rig");
    CHECK(apply(&inv, &corpse, intent(II_TAKE_ALL, INV_SIDE_LOOT, 0, INV_SIDE_SELF, 0, 0, 0, 0, 0)) == IIR_OK, "take all from the corpse");
    CHECK(inv_live_count(&corpse) == 0 && inv_equipped(&inv.t, EQ_RIG), "the rig came over whole, worn");
    CHECK(inv_hot_find(&inv, I("rifle")) >= 0, "the rifle is bound");
    for (int i = 0; i < 3; i++) CHECK(inv_hot_find(&inv, I(meds[i])) >= 0, "the %s in the rig is bound", meds[i]);
    CHECK(inv_hot_find(&inv, I("rig_chest")) < 0, "the worn rig itself never binds");
    // Drop the rig (G / Discard) and pick it up again: the consumables in it are bound again.
    InvCtx c = inv_ctx(&inv, &g_db, NULL);
    InvEffect fx;
    InvIntent d = intent(II_DISCARD, INV_SIDE_SELF, inv_equipped(&inv.t, EQ_RIG)->uid, 0, 0, 0, 0, 0, 0);
    CHECK(inv_intent_apply(&c, &d, &fx) == IIR_OK, "discard the rig");
    for (int i = 0; i < 3; i++) CHECK(inv_hot_find(&inv, I(meds[i])) < 0, "%s left with the rig: unbound", meds[i]);
    u16 top = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) if (inv_alive(&fx.dropped.items[i]) && !fx.dropped.items[i].parent) top = fx.dropped.items[i].uid;
    InvTree pk;
    pickup_tree(&fx.dropped, top, &pk);
    InvSlot top_s = inv_get(&fx.dropped, top)->s;
    CHECK(inv_add_with(&inv, &g_db, &top_s, &pk) == 0, "the rig picked up whole");
    for (int i = 0; i < 3; i++) CHECK(inv_hot_find(&inv, I(meds[i])) >= 0, "the %s in the picked-up rig is bound again", meds[i]);
    CHECK(inv_hot_find(&inv, I("rifle")) >= 0, "the rifle keeps its bind");
    for (int k = 0; k < INV_HOTBAR; k++) CHECK(!inv.quick[k] || inv_get(&inv.t, inv.quick[k]), "binds stay valid");
    CHECK(tree_ok(&inv.t, "rig binds"), "invariants");
}

// Every stocked world container (level "container" entities, building-kit furniture with "loot")
// holds its loot: the root it names ("grid"; furniture "cabinet", a level container "crate") takes
// every entry the way the loader adds it (container_add: loaded weapons, default attachments). A
// 3x3 backpack in three 4x2 drawers was silently dropped by the loader. Files: the arguments
// (CMake passes data/levels/*.json and the building kit's prefabs).
static int g_loot_n;
static void loot_walk(const cJSON *j, const char *file, bool kit) {
    if (cJSON_IsArray(j) || cJSON_IsObject(j)) {
        const cJSON *loot = cJSON_IsObject(j) ? cJSON_GetObjectItemCaseSensitive(j, "loot") : NULL;
        if (cJSON_IsArray(loot)) {
            const char *grid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "grid"));
            bool piece = cJSON_GetObjectItemCaseSensitive(j, "piece") != NULL;
            const char *root_id = grid ? grid : (kit || piece) ? "cabinet" : "crate";
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "name"));
            int root = container_find(&g_db, root_id);
            CHECK(root >= 0, "%s: %s: unknown root '%s'", file, name ? name : "?", root_id);
            InvTree t;
            inv_tree_init(&t, root >= 0 ? root : 0);
            const cJSON *e;
            cJSON_ArrayForEach(e, loot) {
                if (!cJSON_IsArray(e)) continue;
                const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(e, 0));
                u16 item = id ? item_find(&g_db, id) : 0;
                CHECK(item, "%s: %s: unknown loot item '%s'", file, name ? name : "?", id ? id : "?");
                if (!item) continue;
                int count = cJSON_GetArraySize(e) > 1 ? cJSON_GetArrayItem(e, 1)->valueint : 1;
                const WeaponDef *wd = item_weapon(&g_db, item);
                InvSlot s = { item, (u16)count, wd ? (i16)wd->mag_size : 0, { 0 }, 0 };
                attach_defaults(wd, s.att);
                int left = inv_give_root(&t, &g_db, &s);
                CHECK(left == 0, "%s: %s (%s): %d of %s x%d don't fit", file, name ? name : "?", root_id, left, id, count);
            }
            g_loot_n++;
        }
        const cJSON *c;
        cJSON_ArrayForEach(c, j) loot_walk(c, file, kit);
    }
}

static void test_world_loot(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *buf = platform_read_file(argv[i], NULL);
        CHECK(buf, "read %s", argv[i]);
        if (!buf) continue;
        cJSON *j = cJSON_Parse(buf);
        free(buf);
        CHECK(j, "parse %s", argv[i]);
        if (!j) continue;
        const char *base = strrchr(argv[i], '/');
        loot_walk(j, base ? base + 1 : argv[i], strstr(argv[i], "prefabs") != NULL);
        cJSON_Delete(j);
    }
    if (argc > 1) printf("world loot: %d stocked containers checked\n", g_loot_n);
}

// Mounted attachments weigh: a red dot moved onto the rifle still counts in the totals.
static void test_attachment_weight(void) {
    Inventory inv;
    fresh(&inv);
    give(&inv, "backpack_hiking", 1);
    u16 rifle = give(&inv, "rifle", 1);
    u16 rd = give(&inv, "att_red_dot", 1);
    float before = inv_tree_weight(&inv.t, &g_db);
    InvIntent m = intent(II_MOUNT, INV_SIDE_SELF, rd, INV_SIDE_SELF, rifle, 0, 0, 0, 0);
    int r = apply(&inv, NULL, m);
    CHECK(r == IIR_OK && item_def(&g_db, I("att_red_dot"))->inv_weight > 0, "red dot onto the rifle (%s)", inv_result_text(r));
    if (r == IIR_OK) {
        CHECK(fabsf(inv_tree_weight(&inv.t, &g_db) - before) < 1e-3f, "the total keeps the red dot's weight (%.3f vs %.3f)", inv_tree_weight(&inv.t, &g_db), before);
        const InvItem *w = inv_get_c(&inv.t, rifle);
        float expect = item_def(&g_db, I("rifle"))->inv_weight + item_def(&g_db, I("att_red_dot"))->inv_weight;
        for (int k = 0; k < ATT_SLOTS; k++) {
            u16 ai = w->s.att[k] ? item_for_attachment(&g_db, w->s.att[k]) : 0;
            if (ai && ai != I("att_red_dot")) expect += item_def(&g_db, ai)->inv_weight;
        }
        CHECK(fabsf(inv_item_weight(&inv.t, &g_db, w) - expect) < 1e-3f, "the rifle weighs its attachments (%.3f vs %.3f)", inv_item_weight(&inv.t, &g_db, w), expect);
    }
}

int main(int argc, char **argv) {
    CHECK(items_load(&g_db, "data/items.json", "data/weapons"), "items load");
    test_defs();
    test_fit_matrix();
    test_rotation_stack();
    test_nesting();
    test_quick_order_and_swap();
    test_attachments_fold();
    test_attachment_matrix();
    test_sort();
    test_death_corpse();
    test_wire();
    test_weight();
    test_binds();
    test_loot_binds();
    test_container_drop();
    test_take_all_contents();
    test_container_binds();
    test_world_loot(argc, argv);
    test_attachment_weight();
    test_fuzz();
    test_two_clients();
    printf("inventory: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
