// Grid inventory (docs/INVENTORY.md): the schema (exactly the six equipment slots, three backpacks,
// the default bag), class defaults, the fit / rotation / filter matrix over every item and world
// container, stacking, swaps, the backpack rules (a bigger pack takes the contents over, taking one
// off puts them in the default bag, packs off a back are empty, nothing nests), the hands, Compact,
// attachment footprints and the attachment matrix, folding, death into a corpse, the wire format,
// format-1 save migration, a fuzz of random intents (the invariants and conservation always hold and
// the grid's size changes only with the worn pack), two clients looting one crate through a 150 ms
// host round trip with conflicting moves (no duplication or loss, every copy converges), and every
// stocked world container's loot fits its root (the arguments: level and building-kit prefab
// files). Run from the build output directory (data/, assets/).
#include "gameplay/inventory.h"
#include "gameplay/inventory/inv_wire.h"
#include "gameplay/inventory/inv_migrate.h"
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

// Every live item: a body holds only equipment at its root and the worn pack's contents; a world
// container only its root grid; inside its grid, overlapping nothing, within its filter; counts within
// the stack size; equipment slots hold what they accept; a pack off a back is empty.
static bool tree_ok(const InvTree *t, const char *what) {
    bool ok = true;
    const InvContainerDef *root = inv_root_def(t, &g_db);
    u16 grid = inv_grid(t, &g_db);
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *a = &t->items[i];
        if (!inv_alive(a)) continue;
        const ItemDef *d = item_def(&g_db, a->s.item);
        if (a->s.count > DG_MAX(1, d->max_stack)) { printf("  %s: %s count %d > stack\n", what, d->id, a->s.count); ok = false; }
        for (int j = i + 1; j < INV_MAX_ITEMS; j++)
            if (inv_alive(&t->items[j]) && t->items[j].uid == a->uid) { printf("  %s: duplicate uid %d\n", what, a->uid); ok = false; }
        if (item_container(&g_db, a->s.item) && a->uid != grid && inv_child_count(t, a->uid)) { printf("  %s: %s off a back holds items\n", what, d->id); ok = false; }
        if (a->loc == INV_LOC_NONE) continue;   // a dev instance held outside the grid
        if (a->loc >= INV_LOC_EQUIP) {
            int slot = a->loc - INV_LOC_EQUIP;
            if (a->parent || !root->equip || !inv_equip_accepts(slot, d)) { printf("  %s: %s in slot %d\n", what, d->id, slot); ok = false; }
            for (int j = i + 1; j < INV_MAX_ITEMS; j++)
                if (inv_alive(&t->items[j]) && !t->items[j].parent && t->items[j].loc == a->loc) { printf("  %s: two in slot %d\n", what, slot); ok = false; }
            continue;
        }
        if (a->parent != grid) { printf("  %s: %s is outside the grid (parent %d, grid %d)\n", what, d->id, a->parent, grid); ok = false; continue; }
        const InvContainerDef *cd = inv_parent_def(t, &g_db, a->parent);
        if (!cd || a->loc >= cd->comp_count) { printf("  %s: %s has no compartment (parent %d loc %d)\n", what, d->id, a->parent, a->loc); ok = false; continue; }
        int w, h;
        inv_item_size(&g_db, a, &w, &h);
        if (a->x + w > cd->comp[a->loc].w || a->y + h > cd->comp[a->loc].h) { printf("  %s: %s out of bounds\n", what, d->id); ok = false; }
        if (inv_accepts(&g_db, cd, &a->s, w, h) != IIR_OK) { printf("  %s: %s breaks %s's filter\n", what, d->id, cd->id); ok = false; }
        for (int j = i + 1; j < INV_MAX_ITEMS; j++) {
            const InvItem *b = &t->items[j];
            if (!inv_alive(b) || b->parent != a->parent || b->loc != a->loc || b->loc >= INV_LOC_EQUIP) continue;
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

// Item totals (the default bag is created and destroyed by the rules: not counted).
static void totals(const InvTree *t, int *per_item) {
    for (int i = 0; i < INV_MAX_ITEMS; i++)
        if (inv_alive(&t->items[i]) && t->items[i].s.item != g_db.default_backpack) per_item[t->items[i].s.item] += t->items[i].s.count;
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

static u16 grid_of(const Inventory *inv) { return inv_grid(&inv->t, &g_db); }

static void grid_dims(const InvTree *t, int *c, int *r) { inv_grid_size(t, &g_db, c, r); }

// An item placed straight into the tree's grid at (x, y) (test setup; checked).
static u16 put(InvTree *t, const char *id, int count, int x, int y) {
    InvSlot s = { I(id), (u16)count, 0, { 0 }, 0 };
    attach_defaults(item_weapon(&g_db, s.item), s.att);
    InvItem *it = inv_alloc(t, &g_db, &s);
    if (!it) return 0;
    InvPlace p = { inv_grid(t, &g_db), 0, (u8)x, (u8)y, 0 };
    it->loc = INV_LOC_NONE;
    int r = inv_check_place(t, &g_db, it, p, 0, NULL);
    CHECK(r == IIR_OK, "put %s at %d,%d: %s", id, x, y, inv_result_text(r));
    it->parent = p.parent;
    it->loc = 0;
    it->x = (u8)x;
    it->y = (u8)y;
    return it->uid;
}

// ------------------------------------------------------------------ schema / definitions

static void test_schema(void) {
    // Exactly the six equipment slots (the deleted ones are gone from the enum and the names).
    static const char *slots[] = { "primary", "secondary", "holster", "backpack", "headwear", "body_armor" };
    CHECK(EQ_COUNT == (int)ARRAY_COUNT(slots), "%d equipment slots (want %d)", EQ_COUNT, (int)ARRAY_COUNT(slots));
    for (int i = 0; i < EQ_COUNT && i < (int)ARRAY_COUNT(slots); i++) CHECK(!strcmp(equip_slot_name(i), slots[i]), "slot %d is %s", i, slots[i]);
    static const char *gone[] = { "melee", "earpiece", "face_cover", "eyewear", "tactical_rig", "rig", "armband", "secure_container", "secure", "pockets" };
    for (int i = 0; i < (int)ARRAY_COUNT(gone); i++) CHECK(equip_slot_from_name(gone[i]) == EQ_NONE, "no %s slot", gone[i]);
    CHECK(INV_SEL_WEAPONS == 3 && INV_SEL_HANDS == 3 && INV_SEL_COUNT == 4 && EQ_PRIMARY == 0 && EQ_SECONDARY == 1 && EQ_HOLSTER == 2,
          "selections 1-3 are the weapon slots, then the hands");
    // The deleted items are gone.
    static const char *deleted[] = { "rig_chest", "secure_alpha", "ammo_case", "med_case", "headset", "balaclava", "glasses", "armband" };
    for (int i = 0; i < (int)ARRAY_COUNT(deleted); i++) CHECK(!item_find(&g_db, deleted[i]), "%s is deleted", deleted[i]);
    // Three backpacks, growing, ten columns; the smallest is the default bag.
    struct { const char *id; int cols, rows; bool def; } packs[] = { { "sling_bag", 10, 4, true }, { "backpack", 10, 6, false }, { "backpack_hiking", 10, 8, false } };
    for (int i = 0; i < (int)ARRAY_COUNT(packs); i++) {
        u16 it = I(packs[i].id);
        const InvContainerDef *cd = item_container(&g_db, it);
        int c = 0, r = 0;
        if (cd) { c = cd->comp[0].w; r = cd->comp[0].h; }
        CHECK(cd && cd->comp_count == 1 && c == packs[i].cols && r == packs[i].rows, "%s: one %dx%d grid (got %dx%d)", packs[i].id, packs[i].cols, packs[i].rows, c, r);
        CHECK(item_def(&g_db, it)->equip_slot == EQ_BACKPACK && item_def(&g_db, it)->default_pack == packs[i].def, "%s is a backpack%s", packs[i].id, packs[i].def ? " (the default)" : "");
    }
    CHECK(g_db.default_backpack == I("sling_bag"), "the default bag is the sling bag");
    int n_cont = 0;
    for (int i = 1; i < g_db.item_count; i++) n_cont += g_db.items[i].container >= 0;
    CHECK(n_cont == 3, "only the three backpacks have a grid (%d)", n_cont);
    // The root containers: one grid each (no compartments, no dividers); the body none.
    CHECK(container_find(&g_db, "body") == 0 && g_db.containers[0].equip && g_db.containers[0].comp_count == 0, "body: container 0, equipment only");
    for (int c = 1; c < g_db.container_count; c++) {
        const InvContainerDef *cd = &g_db.containers[c];
        CHECK(cd->comp_count == 1 && cd->comp[0].at_x == 0 && cd->comp[0].at_y == 0, "%s: one grid", cd->id);
    }
    // The helmet and the plate carrier.
    CHECK(item_def(&g_db, I("helmet"))->equip_slot == EQ_HEADWEAR && item_def(&g_db, I("helmet"))->durability > 0, "helmet: headwear with durability");
    CHECK(item_def(&g_db, I("plate_carrier"))->equip_slot == EQ_BODY_ARMOR && item_def(&g_db, I("plate_carrier"))->plates == 4, "plate carrier: body armor, 4 plates");
}

static void test_defs(void) {
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
        CHECK(d->grid_w >= 1 && d->grid_h >= 1, "%s has a grid size", d->id);
        CHECK(d->category < ICAT_COUNT, "%s has a category", d->id);
        if (d->kind == ITEM_WEAPON) CHECK(d->equip_slot == EQ_PRIMARY || d->equip_slot == EQ_HOLSTER, "weapon %s goes in a weapon slot", d->id);
    }
    CHECK(item_def(&g_db, I("pistol"))->equip_slot == EQ_HOLSTER && item_def(&g_db, I("knife"))->equip_slot == EQ_HOLSTER && item_def(&g_db, I("bat"))->equip_slot == EQ_PRIMARY,
          "pistol, knife -> holster; bat -> a long-gun slot");
    struct { const char *id; int cat; } cats[] = { { "rifle", ICAT_WEAPONS }, { "ammo_9mm", ICAT_WEAPONS }, { "att_red_dot", ICAT_ATTACHMENTS }, { "medkit", ICAT_MEDICAL },
                                                   { "armor", ICAT_MEDICAL }, { "frag_grenade", ICAT_THROWABLES }, { "flashlight", ICAT_TOOLS }, { "fuel_can", ICAT_TOOLS },
                                                   { "key_red", ICAT_TOOLS }, { "syringe", ICAT_TOOLS }, { "scrap", ICAT_MISC } };
    for (int i = 0; i < (int)ARRAY_COUNT(cats); i++) CHECK(item_def(&g_db, I(cats[i].id))->category == cats[i].cat, "%s is in %s", cats[i].id, inv_category_name(cats[i].cat));
    u8 sup = attach_find(&g_db, "suppressor");
    CHECK(sup && g_db.attachments[sup].grid_dw == 1, "a suppressor adds a column");
}

// Every item into every world container and into a fresh body's default bag (empty, at 0,0
// unrotated and rotated): the rule agrees with an independent reading of the definitions.
static void test_fit_matrix(void) {
    int pairs = 0;
    for (int c = -1; c < g_db.container_count; c++) {
        InvTree t;
        if (c < 0) { inv_tree_init(&t, 0); inv_ensure_pack(&t, &g_db); }
        else if (g_db.containers[c].equip || !g_db.containers[c].comp_count) continue;
        else inv_tree_init(&t, c);
        u16 grid = inv_grid(&t, &g_db);
        const InvContainerDef *cd = inv_parent_def(&t, &g_db, grid);
        for (int i = 1; i < g_db.item_count; i++) {
            const ItemDef *d = &g_db.items[i];
            for (int rot = 0; rot < 2; rot++) {
                InvItem it;
                memset(&it, 0, sizeof(it));
                it.s.item = (u16)i;
                it.s.count = 1;
                InvPlace p = { grid, 0, 0, 0, (u8)rot };
                int r = inv_check_place(&t, &g_db, &it, p, 0, NULL);
                int w = rot ? d->grid_h : d->grid_w, h = rot ? d->grid_w : d->grid_h;
                bool kind_ok = (!cd->allow_kinds || (cd->allow_kinds & (1u << d->kind))) && !(cd->deny_kinds & (1u << d->kind));
                for (int k = 0; k < cd->deny_count; k++) kind_ok &= cd->deny_items[k] != i;
                if (cd->max_w) kind_ok &= DG_MIN(w, h) <= DG_MIN(cd->max_w, cd->max_h) && DG_MAX(w, h) <= DG_MAX(cd->max_w, cd->max_h);
                bool fits = w <= cd->comp[0].w && h <= cd->comp[0].h;
                bool rot_ok = !rot || d->rotatable;
                bool expect = kind_ok && fits && rot_ok;
                CHECK((r == IIR_OK) == expect, "%s (rot %d) into %s: got %s", d->id, rot, cd->id, inv_result_text(r));
                pairs++;
            }
        }
        // Nothing nests: not into a pack lying in the grid, never into the body's root.
        if (c < 0) {
            InvItem it;
            memset(&it, 0, sizeof(it));
            it.s.item = I("bandage");
            it.s.count = 1;
            InvPlace root = { 0, 0, 0, 0, 0 };
            CHECK(inv_check_place(&t, &g_db, &it, root, 0, NULL) == IIR_NESTING, "a body has no grid of its own");
            u16 spare = put(&t, "backpack", 1, 0, 0);
            InvPlace in_spare = { spare, 0, 0, 0, 0 };
            CHECK(inv_check_place(&t, &g_db, &it, in_spare, 0, NULL) == IIR_NESTING, "nothing goes into a spare pack");
        }
    }
    printf("  fit matrix: %d item x grid x rotation cases\n", pairs);
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
    CHECK(apply(&inv, &crate, intent(II_MOVE, INV_SIDE_LOOT, bu, INV_SIDE_LOOT, 0, 0, 3, 2, 0)) == IIR_OK, "bandage moved away");
    CHECK(apply(&inv, &crate, rot) == IIR_OK && (inv_get(&crate, pu)->flags & INVF_ROT), "rotation now fits");
    // Stacks: merge with a remainder; a split drag.
    InvTree c2;
    inv_tree_init(&c2, container_find(&g_db, "crate"));
    InvSlot s1 = { I("ammo_9mm"), 50, 0, { 0 }, 0 }, s2 = { I("ammo_9mm"), 30, 0, { 0 }, 0 };
    InvItem *x1 = inv_alloc(&c2, &g_db, &s1), *x2 = inv_alloc(&c2, &g_db, &s2);
    x2->x = 2;
    u16 u1 = x1->uid, u2 = x2->uid;
    CHECK(apply(&inv, &c2, intent(II_MOVE, INV_SIDE_LOOT, u2, INV_SIDE_LOOT, 0, 0, 0, 0, 0)) == IIR_OK, "merge onto a stack");
    CHECK(inv_get(&c2, u1)->s.count == 60 && inv_get(&c2, u2)->s.count == 20, "60 / 20 remainder");
    InvIntent sp = intent(II_MOVE, INV_SIDE_LOOT, u2, INV_SIDE_LOOT, 0, 0, 5, 3, 0);
    sp.count = 5;
    CHECK(apply(&inv, &c2, sp) == IIR_OK && inv_get(&c2, u2)->s.count == 15, "split drag: 5 off");
    int tot[MAX_ITEM_DEFS] = { 0 };
    totals(&c2, tot);
    CHECK(tot[I("ammo_9mm")] == 80, "no rounds created or lost (%d)", tot[I("ammo_9mm")]);
    // A split into the backpack (II_SPLIT, auto).
    u16 st = give(&inv, "ammo_rifle", 60);
    InvIntent sa = intent(II_SPLIT, INV_SIDE_SELF, st, INV_SIDE_SELF, INV_AUTO, 0, 0, 0, 0);
    sa.count = 25;
    CHECK(apply(&inv, NULL, sa) == IIR_OK && inv_count(&inv, I("ammo_rifle")) == 60 && inv_get(&inv.t, st)->s.count == 35, "split 25 off into the grid");
    CHECK(tree_ok(&inv.t, "split") && tree_ok(&c2, "stacks"), "invariants");
}

// ------------------------------------------------------------------ backpacks

static void test_backpacks(void) {
    Inventory inv;
    fresh(&inv);
    int c, r;
    grid_dims(&inv.t, &c, &r);
    const InvItem *pk = inv_pack_c(&inv.t);
    CHECK(pk && pk->s.item == I("sling_bag") && c == 10 && r == 4, "a fresh body wears the default bag: 10x4 (%dx%d)", c, r);
    // Items in the bag, then a bigger pack picked up: worn over the default bag, the contents in their cells.
    u16 med = put(&inv.t, "medkit", 1, 8, 2);
    u16 tape = put(&inv.t, "duct_tape", 1, 3, 1);
    u16 school = give(&inv, "backpack", 1);
    pk = inv_pack_c(&inv.t);
    grid_dims(&inv.t, &c, &r);
    CHECK(pk && pk->uid == school && c == 10 && r == 6, "the school pack goes on over the default bag (10x6)");
    CHECK(!inv_find(&inv, I("sling_bag")), "the default bag is gone (not an item to carry)");
    const InvItem *m = inv_get_c(&inv.t, med), *t = inv_get_c(&inv.t, tape);
    CHECK(m && m->parent == school && m->x == 8 && m->y == 2 && t && t->parent == school && t->x == 3 && t->y == 1, "the contents keep their cells");
    // A second bigger pack picked up while one is worn: into the grid as an empty item.
    u16 hike = give(&inv, "backpack_hiking", 1);
    const InvItem *hk = inv_get_c(&inv.t, hike);
    CHECK(hk && hk->parent == school && inv_pack_c(&inv.t)->uid == school, "the hiking pack lands in the grid");
    // Equip it: the contents move over, the school pack goes where the hiking pack was... inside the
    // old grid, so into the new one.
    InvIntent eq = intent(II_EQUIP, INV_SIDE_SELF, hike, INV_SIDE_SELF, 0, 0, 0, 0, 0);
    eq.aux = 255;
    CHECK(apply(&inv, NULL, eq) == IIR_OK, "equip the hiking pack");
    grid_dims(&inv.t, &c, &r);
    m = inv_get_c(&inv.t, med);
    const InvItem *sc = inv_get_c(&inv.t, school);
    CHECK(inv_pack_c(&inv.t)->uid == hike && c == 10 && r == 8, "worn: 10x8");
    CHECK(m && m->parent == hike && m->x == 8 && m->y == 2, "the medkit kept its cell");
    CHECK(sc && sc->parent == hike && sc->loc == 0 && inv_child_count(&inv.t, school) == 0, "the school pack is an empty item in the new grid");
    CHECK(tree_ok(&inv.t, "packs"), "invariants");
    // The default bag never moves: discard, quick move, a drag to the grid.
    Inventory d0;
    fresh(&d0);
    u16 sling = inv_pack_c(&d0.t)->uid;
    CHECK(apply(&d0, NULL, intent(II_DISCARD, INV_SIDE_SELF, sling, 0, 0, 0, 0, 0, 0)) == IIR_DEFAULT_PACK, "the default bag can't be discarded");
    CHECK(apply(&d0, NULL, intent(II_MOVE, INV_SIDE_SELF, sling, INV_SIDE_SELF, sling, 0, 0, 0, 0)) == IIR_DEFAULT_PACK, "nor dragged into its grid");
    CHECK(inv_add(&d0, &g_db, I("sling_bag"), 1, 0) == 1, "a second default bag isn't taken");
    // Taking the hiking pack off: the contents go into a new default bag (10x4), refused if they don't fit.
    Inventory full;
    fresh(&full);
    u16 hp = give(&full, "backpack_hiking", 1);
    for (int y = 0; y < 8; y += 2)
        for (int x = 0; x < 10; x += 2) put(&full.t, "medkit", 1, x, y);   // 20 medkits: 80 cells
    InvIntent off = intent(II_MOVE, INV_SIDE_SELF, hp, INV_SIDE_SELF, INV_AUTO, 0, 0, 0, 0);
    CHECK(apply(&full, NULL, off) == IIR_NO_ROOM, "80 cells don't go into the 40-cell bag: refused");
    CHECK(inv_pack_c(&full.t)->uid == hp && inv_live_count(&full.t) == 21, "nothing changed");
    // Half of them: they fit (packed into free cells); the pack ends up in the bag's grid? No room
    // (4x5 or 5x4 in a full 10x4 bag): refused too. Discard it instead: it drops empty.
    Inventory half;
    fresh(&half);
    u16 hp2 = give(&half, "backpack_hiking", 1);
    for (int i = 0; i < 6; i++) put(&half.t, "medkit", 1, (i % 5) * 2, 6 - (i / 5) * 2);   // rows 4..7 + row 6..7
    InvEffect fx;
    InvCtx ctx = inv_ctx(&half, &g_db, NULL);
    InvIntent dis = intent(II_DISCARD, INV_SIDE_SELF, hp2, 0, 0, 0, 0, 0, 0);
    int rr = inv_intent_apply(&ctx, &dis, &fx);
    CHECK(rr == IIR_OK, "discard the worn hiking pack (%s)", inv_result_text(rr));
    CHECK(inv_live_count(&fx.dropped) == 1 && inv_alive(&fx.dropped.items[0]) && fx.dropped.items[0].s.item == I("backpack_hiking"), "it drops empty, alone");
    pk = inv_pack_c(&half.t);
    CHECK(pk && pk->s.item == I("sling_bag") && inv_count(&half, I("medkit")) == 6, "the six medkits are in a new default bag");
    CHECK(tree_ok(&half.t, "pack off"), "invariants");
    // A corpse's full pack can't be taken; once emptied it can, and it goes on over the default bag.
    Inventory me;
    fresh(&me);
    InvTree corpse;
    inv_tree_init(&corpse, container_find(&g_db, "body"));
    InvSlot cs = { I("backpack"), 1, 0, { 0 }, 0 };
    InvItem *cp = inv_alloc(&corpse, &g_db, &cs);
    cp->loc = INV_LOC_EQUIP + EQ_BACKPACK;
    u16 cpu = cp->uid;
    u16 cmed = put(&corpse, "medkit", 1, 0, 0);
    CHECK(apply(&me, &corpse, intent(II_QUICK_MOVE, INV_SIDE_LOOT, cpu, 0, 0, 0, 0, 0, 0)) == IIR_NOT_EMPTY, "a corpse's full pack: empty it first");
    CHECK(apply(&me, &corpse, intent(II_QUICK_MOVE, INV_SIDE_LOOT, cmed, 0, 0, 0, 0, 0, 0)) == IIR_OK, "its medkit taken");
    CHECK(apply(&me, &corpse, intent(II_QUICK_MOVE, INV_SIDE_LOOT, cpu, 0, 0, 0, 0, 0, 0)) == IIR_OK, "the emptied pack taken");
    pk = inv_pack_c(&me.t);
    grid_dims(&me.t, &c, &r);
    CHECK(pk && pk->s.item == I("backpack") && r == 6 && inv_count(&me, I("medkit")) == 1, "worn, the medkit moved into it");
    CHECK(apply(&me, &corpse, intent(II_MOVE, INV_SIDE_SELF, give(&me, "bandage", 1), INV_SIDE_LOOT, 0, INV_LOC_EQUIP + EQ_BACKPACK, 0, 0, 0)) == IIR_EQUIP, "a corpse is looted, not dressed");
    CHECK(tree_ok(&me.t, "corpse pack") && tree_ok(&corpse, "corpse"), "invariants");
}

// ------------------------------------------------------------------ quick move, swaps, the hands

static void test_quick_and_swap(void) {
    Inventory inv;
    fresh(&inv);
    u16 r1 = give(&inv, "rifle", 1);
    CHECK(inv_get(&inv.t, r1)->loc == INV_LOC_EQUIP + EQ_PRIMARY, "first rifle: primary");
    u16 r2 = give(&inv, "smg", 1);
    CHECK(inv_get(&inv.t, r2)->loc == INV_LOC_EQUIP + EQ_SECONDARY, "second long gun: secondary");
    u16 r3 = give(&inv, "dmr", 1);
    CHECK(inv_get(&inv.t, r3)->parent == grid_of(&inv), "third: the grid");
    u16 kn = give(&inv, "knife", 1);
    CHECK(inv_get(&inv.t, kn)->loc == INV_LOC_EQUIP + EQ_HOLSTER, "the knife: the holster");
    InvIntent eq = intent(II_EQUIP, INV_SIDE_SELF, r3, INV_SIDE_SELF, 0, 0, 0, 0, 0);
    eq.aux = EQ_PRIMARY;
    CHECK(apply(&inv, NULL, eq) == IIR_OK, "equip swap");
    CHECK(inv_get(&inv.t, r3)->loc == INV_LOC_EQUIP + EQ_PRIMARY && inv_get(&inv.t, r1)->parent == grid_of(&inv), "the old primary in the grid");
    eq.aux = EQ_HOLSTER;
    CHECK(apply(&inv, NULL, eq) == IIR_EQUIP, "a rifle doesn't go in the holster");
    eq.aux = EQ_BACKPACK;
    CHECK(apply(&inv, NULL, eq) == IIR_EQUIP, "nor on the back");
    // Quick move without loot: a slot -> the grid, a grid weapon -> a free slot.
    CHECK(apply(&inv, NULL, intent(II_QUICK_MOVE, INV_SIDE_SELF, kn, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv_get(&inv.t, kn)->parent == grid_of(&inv), "the knife to the grid");
    CHECK(apply(&inv, NULL, intent(II_QUICK_MOVE, INV_SIDE_SELF, kn, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv_get(&inv.t, kn)->loc == INV_LOC_EQUIP + EQ_HOLSTER, "and back");
    u16 cloth = give(&inv, "cloth", 1);
    CHECK(apply(&inv, NULL, intent(II_QUICK_MOVE, INV_SIDE_SELF, cloth, 0, 0, 0, 0, 0, 0)) == IIR_NOOP, "cloth: nowhere to go without loot");
    // With a crate open: across.
    InvTree crate;
    inv_tree_init(&crate, container_find(&g_db, "crate"));
    CHECK(apply(&inv, &crate, intent(II_QUICK_MOVE, INV_SIDE_SELF, cloth, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv_live_count(&crate) == 1, "across to the crate");
    // Swap: pistol (2x1) dropped on a single 1x1 in a crate; the bandage goes where the pistol was.
    InvTree c2;
    inv_tree_init(&c2, container_find(&g_db, "crate_small"));
    InvSlot ps = { I("pistol"), 1, 0, { 0 }, 0 }, bs = { I("bandage"), 1, 0, { 0 }, 0 };
    InvItem *pp = inv_alloc(&c2, &g_db, &ps), *bb = inv_alloc(&c2, &g_db, &bs);
    pp->x = 0; pp->y = 0; bb->x = 2; bb->y = 1;
    u16 pu = pp->uid, bu = bb->uid;
    CHECK(apply(&inv, &c2, intent(II_MOVE, INV_SIDE_LOOT, pu, INV_SIDE_LOOT, 0, 0, 2, 1, 0)) == IIR_OK, "swap");
    CHECK(inv_get(&c2, pu)->x == 2 && inv_get(&c2, pu)->y == 1 && inv_get(&c2, bu)->x == 0 && inv_get(&c2, bu)->y == 0, "swapped places");
    CHECK(tree_ok(&inv.t, "swap") && tree_ok(&c2, "swap crate"), "invariants");
}

static void test_hands(void) {
    Inventory inv;
    fresh(&inv);
    u16 rifle = give(&inv, "rifle", 1);
    u16 med = give(&inv, "medkit", 2);
    u16 fl = give(&inv, "flashlight", 1);
    CHECK(inv_sel_c(&inv, 0)->item == I("rifle") && inv_sel_c(&inv, INV_SEL_HANDS)->item == 0, "selection 1 is the primary; the hands are empty");
    CHECK(apply(&inv, NULL, intent(II_HOLD, INV_SIDE_SELF, rifle, 0, 0, 0, 0, 0, 0)) == IIR_NOT_USABLE, "weapons are drawn from their slots, not held");
    CHECK(apply(&inv, NULL, intent(II_HOLD, INV_SIDE_SELF, med, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv.hands == med, "the medkit in the hands");
    CHECK(inv_sel_c(&inv, INV_SEL_HANDS)->item == I("medkit") && inv_sel_find(&inv, I("medkit")) == INV_SEL_HANDS, "selection 4 reads it");
    // It stays in the grid: moving it keeps it in the hands.
    const InvItem *m = inv_get_c(&inv.t, med);
    CHECK(m && m->parent == grid_of(&inv), "a held item keeps its cell");
    CHECK(apply(&inv, NULL, intent(II_MOVE, INV_SIDE_SELF, med, INV_SIDE_SELF, grid_of(&inv), 0, 6, 2, 0)) == IIR_OK && inv.hands == med, "moved, still held");
    CHECK(apply(&inv, NULL, intent(II_HOLD, INV_SIDE_SELF, fl, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv.hands == fl, "the flashlight (a hand tool) instead");
    // Leaving the tree clears the hands.
    CHECK(apply(&inv, NULL, intent(II_DISCARD, INV_SIDE_SELF, fl, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv.hands == 0, "discarded: the hands are empty");
    CHECK(apply(&inv, NULL, intent(II_HOLD, INV_SIDE_SELF, 0, 0, 0, 0, 0, 0, 0)) == IIR_NOOP, "put away with nothing held");
    // Removing by definition takes the stacks not in the hands first.
    u16 b1 = give(&inv, "bandage", 5);
    apply(&inv, NULL, intent(II_HOLD, INV_SIDE_SELF, b1, 0, 0, 0, 0, 0, 0));
    u16 b2 = put(&inv.t, "bandage", 2, 9, 3);
    CHECK(inv_remove(&inv, I("bandage"), 2) == 2 && !inv_get(&inv.t, b2) && inv_get(&inv.t, b1)->s.count == 5, "the held stack is used last");
}

// ------------------------------------------------------------------ attachments

static void test_attachments_fold(void) {
    Inventory inv;
    fresh(&inv);   // the 10x4 default bag
    u16 rifle = put(&inv.t, "rifle", 1, 6, 0);   // cols 6..9: no room to grow right
    u16 sup = give(&inv, "att_suppressor", 1);
    InvIntent m = intent(II_MOUNT, INV_SIDE_SELF, sup, INV_SIDE_SELF, rifle, 0, 0, 0, 0);
    CHECK(apply(&inv, NULL, m) == IIR_NO_ROOM, "suppressed (5x2) no longer fits at column 6: rejected");
    int w, h;
    inv_item_size(&g_db, inv_get(&inv.t, rifle), &w, &h);
    CHECK(w == 4 && h == 2 && inv_get(&inv.t, sup), "unchanged after the rejection");
    CHECK(apply(&inv, NULL, intent(II_MOVE, INV_SIDE_SELF, rifle, INV_SIDE_SELF, grid_of(&inv), 0, 0, 2, 0)) == IIR_OK, "rifle moved to the left");
    CHECK(apply(&inv, NULL, m) == IIR_OK, "now it mounts");
    inv_item_size(&g_db, inv_get(&inv.t, rifle), &w, &h);
    CHECK(w == 5 && h == 2 && !inv_find(&inv, I("att_suppressor")), "5x2 with the suppressor; the item consumed");
    InvIntent um = intent(II_UNMOUNT, INV_SIDE_SELF, rifle, INV_SIDE_SELF, INV_AUTO, 0, 0, 0, 0);
    um.aux = ATT_MUZZLE;
    CHECK(apply(&inv, NULL, um) == IIR_OK && inv_find(&inv, I("att_suppressor")), "unmounted back into the grid");
    inv_item_size(&g_db, inv_get(&inv.t, rifle), &w, &h);
    CHECK(w == 4, "4x2 again");
    CHECK(apply(&inv, NULL, intent(II_FOLD, INV_SIDE_SELF, rifle, 0, 0, 0, 0, 0, 0)) == IIR_OK, "fold");
    inv_item_size(&g_db, inv_get(&inv.t, rifle), &w, &h);
    CHECK(w == 3, "folded 3x2");
    inv_get(&inv.t, rifle)->s.ammo = 30;
    CHECK(apply(&inv, NULL, intent(II_UNLOAD, INV_SIDE_SELF, rifle, 0, 0, 0, 0, 0, 0)) == IIR_OK && inv_count(&inv, I("ammo_rifle")) == 30, "unloaded 30 rounds");
    // An equipped weapon's attachments (the slot column's chips): mounting doesn't depend on room.
    InvIntent eq = intent(II_EQUIP, INV_SIDE_SELF, rifle, INV_SIDE_SELF, 0, 0, 0, 0, 0);
    eq.aux = EQ_PRIMARY;
    CHECK(apply(&inv, NULL, eq) == IIR_OK && inv_get(&inv.t, rifle)->loc == INV_LOC_EQUIP + EQ_PRIMARY, "the rifle worn");
    CHECK(apply(&inv, NULL, intent(II_MOUNT, INV_SIDE_SELF, inv_find(&inv, I("att_suppressor"))->uid, INV_SIDE_SELF, rifle, 0, 0, 0, 0)) == IIR_OK, "a suppressor onto the worn rifle");
    CHECK(tree_ok(&inv.t, "attachments"), "invariants");
}

// Every weapon x attachment: compatibility agrees with the definitions, and a mount / unmount
// round trip works (the weapon in a slot, the attachment in the grid).
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
            wi->loc = (u8)(INV_LOC_EQUIP + (item_def(&g_db, (u16)w)->equip_slot == EQ_HOLSTER ? EQ_HOLSTER : EQ_PRIMARY));
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
    u16 su = put(&inv.t, item_def(&g_db, item_for_attachment(&g_db, s4))->id, 1, 3, 4);
    CHECK(apply(&inv, NULL, intent(II_MOUNT, INV_SIDE_SELF, su, INV_SIDE_SELF, ru, 0, 0, 0, 0)) == IIR_OK && inv_get(&inv.t, ru)->s.att[ATT_OPTIC] == s4,
          "scope mounted over the red dot");
    InvItem *back = inv_find(&inv, item_for_attachment(&g_db, rd));
    CHECK(back && back->parent == pack && back->x == 3 && back->y == 4, "the red dot comes back where the scope lay");
    u16 med = give(&inv, "medkit", 1);
    CHECK(apply(&inv, NULL, intent(II_MOUNT, INV_SIDE_SELF, back->uid, INV_SIDE_SELF, med, 0, 0, 0, 0)) == IIR_NOT_WEAPON, "onto a medkit: not a weapon");
    u16 knife = give(&inv, "knife", 1);
    CHECK(apply(&inv, NULL, intent(II_MOUNT, INV_SIDE_SELF, back->uid, INV_SIDE_SELF, knife, 0, 0, 0, 0)) == IIR_NOT_WEAPON, "melee weapons take no attachments");
    // Extended magazine removed: the extra rounds come back as a stack.
    u8 em = attach_find(&g_db, "extended_mag");
    if (em) {
        fresh(&inv);
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

// ------------------------------------------------------------------ compact, death, wire

static void test_compact(void) {
    Inventory inv;
    fresh(&inv);
    give(&inv, "backpack", 1);   // 10x6
    put(&inv.t, "bandage", 1, 9, 5);
    put(&inv.t, "rifle", 1, 5, 3);
    put(&inv.t, "battery", 1, 0, 5);
    put(&inv.t, "medkit", 1, 2, 2);
    int before[MAX_ITEM_DEFS] = { 0 }, after[MAX_ITEM_DEFS] = { 0 };
    totals(&inv.t, before);
    int c0, r0;
    grid_dims(&inv.t, &c0, &r0);
    InvIntent co = intent(II_COMPACT, 0, 0, INV_SIDE_SELF, 0, 0, 0, 0, 0);
    CHECK(apply(&inv, NULL, co) == IIR_OK, "compact");
    totals(&inv.t, after);
    int c1, r1;
    grid_dims(&inv.t, &c1, &r1);
    CHECK(!memcmp(before, after, sizeof(before)) && tree_ok(&inv.t, "compact") && c0 == c1 && r0 == r1, "compacting keeps every item, the invariants and the grid size");
    InvItem *top = inv_at(&inv.t, &g_db, grid_of(&inv), 0, 0, 0);
    CHECK(top && top->s.item == I("rifle"), "the largest item (the rifle) first (%s)", top ? item_def(&g_db, top->s.item)->id : "none");
    CHECK(apply(&inv, NULL, co) == IIR_NOOP, "compacting a compact grid does nothing");
}

static void test_death_corpse(void) {
    Inventory inv;
    fresh(&inv);
    give(&inv, "backpack_hiking", 1);
    give(&inv, "rifle", 1);
    give(&inv, "pistol", 1);
    give(&inv, "helmet", 1);
    give(&inv, "plate_carrier", 1);
    u16 med = give(&inv, "medkit", 2);
    give(&inv, "ammo_rifle", 120);
    apply(&inv, NULL, intent(II_HOLD, INV_SIDE_SELF, med, 0, 0, 0, 0, 0, 0));
    int before[MAX_ITEM_DEFS] = { 0 }, after[MAX_ITEM_DEFS] = { 0 };
    totals(&inv.t, before);
    InvTree corpse;
    inv_tree_init(&corpse, container_find(&g_db, "body"));
    inv_strip_to_corpse(&inv.t, &inv.hands, &corpse, &g_db);
    const InvItem *pk = inv_pack_c(&inv.t);
    CHECK(pk && pk->s.item == I("sling_bag") && inv_live_count(&inv.t) == 1, "the body keeps only a fresh default bag (%d)", inv_live_count(&inv.t));
    CHECK(inv.hands == 0, "nothing in the hands");
    CHECK(inv_equipped(&corpse, EQ_PRIMARY) && inv_equipped(&corpse, EQ_HOLSTER) && inv_equipped(&corpse, EQ_HEADWEAR) && inv_equipped(&corpse, EQ_BODY_ARMOR) &&
          inv_equipped(&corpse, EQ_BACKPACK), "the corpse wears it all");
    totals(&inv.t, after);
    totals(&corpse, after);
    CHECK(!memcmp(before, after, sizeof(before)), "body + corpse = what was carried");
    CHECK(tree_ok(&corpse, "corpse") && tree_ok(&inv.t, "body after death"), "invariants");
    // Take all from the corpse: the pack's contents, the weapons and gear, then the emptied pack.
    Inventory looter;
    fresh(&looter);
    InvIntent ta = intent(II_TAKE_ALL, INV_SIDE_LOOT, 0, INV_SIDE_LOOT, 0, 0, 0, 0, 0);
    CHECK(apply(&looter, &corpse, ta) == IIR_OK, "take all");
    pk = inv_pack_c(&looter.t);
    CHECK(pk && pk->s.item == I("backpack_hiking"), "the corpse's pack, emptied, went on over the default bag");
    CHECK(inv_count(&looter, I("medkit")) == 2 && inv_count(&looter, I("ammo_rifle")) == 120 && inv_equipped(&looter.t, EQ_PRIMARY), "its contents and weapons came over");
    CHECK(tree_ok(&looter.t, "looter") && tree_ok(&corpse, "looted corpse"), "invariants");
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

// ------------------------------------------------------------------ format-1 saves

static void test_migrate(void) {
    // A format-1 body: hiking pack worn (old slot 10) holding a medkit at (1,1); a chest rig (9) with
    // rounds; the secure container (12) with a DNA sample; a bat in the old melee slot (3); a helmet
    // (4); a rifle (0); a balaclava (6, deleted); a backpack in the old backpack's grid.
    InvV1Entry e[16];
    int n = 0;
    memset(e, 0, sizeof(e));
    // old item indices: 1-based in the old items.json order (inv_migrate.c)
    static const char *order[] = { "ammo_9mm", "ammo_rifle", "ammo_shell", "ammo_bolt", "ammo_762", "medkit", "bandage", "armor", "painkillers", "frag_grenade",
        "flare", "molotov", "canned_food", "water_bottle", "pistol", "rifle", "smg", "lmg", "dmr", "sniper", "shotgun", "crossbow",
        "bat", "knife", "key_red", "key_blue", "scrap", "battery", "cloth", "duct_tape", "mech_parts", "seeds", "syringe",
        "dna_sample", "fishing_kit", "phone", "fuel_can", "tire_repair", "spare_tire", "energy_bar", "backpack", "backpack_hiking",
        "sling_bag", "rig_chest", "plate_carrier", "helmet", "headset", "balaclava", "glasses", "armband", "secure_alpha",
        "ammo_case", "med_case", "flashlight" };
    struct { const char *id; int count, uid, parent, loc, x, y; } spec[] = {
        { "backpack_hiking", 1, 1, 0, 64 + 10, 0, 0 },
        { "medkit", 2, 2, 1, 0, 1, 1 },
        { "rig_chest", 1, 3, 0, 64 + 9, 0, 0 },
        { "ammo_rifle", 60, 4, 3, 0, 0, 0 },
        { "secure_alpha", 1, 5, 0, 64 + 12, 0, 0 },
        { "dna_sample", 1, 6, 5, 0, 0, 0 },
        { "bat", 1, 7, 0, 64 + 3, 0, 0 },
        { "helmet", 1, 8, 0, 64 + 4, 0, 0 },
        { "rifle", 1, 9, 0, 64 + 0, 0, 0 },
        { "balaclava", 1, 10, 0, 64 + 6, 0, 0 },
        { "backpack", 1, 11, 1, 0, 5, 0 },
        { "scrap", 3, 12, 0, 1, 0, 0 },   // a pocket
    };
    for (int i = 0; i < (int)ARRAY_COUNT(spec); i++) {
        int oi = 0;
        for (int k = 0; k < (int)ARRAY_COUNT(order); k++) if (!strcmp(order[k], spec[i].id)) oi = k + 1;
        e[n++] = (InvV1Entry){ oi, spec[i].count, 0, spec[i].uid, spec[i].parent, spec[i].loc, spec[i].x, spec[i].y, 0, 0, { 0 } };
    }
    CHECK(inv_v1_item(&g_db, 6) == I("medkit") && inv_v1_item(&g_db, 48) == 0, "format-1 indices: medkit kept, balaclava deleted");
    InvTree t;
    int dropped = inv_migrate_v1(&t, &g_db, 0, e, n);
    Inventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.t = t;
    const InvItem *pk = inv_pack_c(&t);
    CHECK(pk && pk->s.item == I("backpack_hiking"), "the worn hiking pack stays worn");
    const InvItem *m = inv_find(&inv, I("medkit"));
    CHECK(m && m->parent == pk->uid && m->x == 1 && m->y == 1, "its medkit keeps its cell");
    CHECK(inv_equipped(&t, EQ_PRIMARY) && inv_equipped(&t, EQ_PRIMARY)->s.item == I("rifle"), "rifle: primary");
    CHECK(inv_equipped(&t, EQ_SECONDARY) && inv_equipped(&t, EQ_SECONDARY)->s.item == I("bat"), "the bat from the melee slot: secondary");
    CHECK(inv_equipped(&t, EQ_HEADWEAR) && inv_equipped(&t, EQ_HEADWEAR)->s.item == I("helmet"), "helmet: headwear");
    CHECK(inv_count(&inv, I("ammo_rifle")) == 60 && inv_count(&inv, I("dna_sample")) == 1 && inv_count(&inv, I("scrap")) == 3, "the rig's, secure container's and pockets' contents moved into the grid");
    CHECK(inv_count(&inv, I("backpack")) == 1, "the spare backpack is an item in the grid");
    CHECK(dropped == 3, "the rig, the secure container and the balaclava were dropped (%d)", dropped);
    CHECK(tree_ok(&t, "migrated"), "invariants");
    // A format-1 cabinet (three 4x2 drawers): everything into the one 4x6 grid.
    InvV1Entry ce[3] = { { 15, 1, 0, 1, 0, 0, 0, 0, 0, 0, { 0 } }, { 6, 1, 0, 2, 0, 1, 0, 0, 0, 0, { 0 } }, { 7, 3, 0, 3, 0, 2, 2, 0, 0, 0, { 0 } } };
    InvTree cab;
    int cd = inv_migrate_v1(&cab, &g_db, container_find(&g_db, "cabinet"), ce, 3);
    CHECK(cd == 0 && inv_live_count(&cab) == 3 && tree_ok(&cab, "cabinet"), "a format-1 cabinet: all three drawers' items in the one grid");
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
    const InvTree *dt = in.dst_side ? loot : &inv->t;
    in.dst = r == 0 ? inv_grid(dt, &g_db) : r == 1 ? INV_AUTO : pick_uid(dt);
    in.loc = (u8)(rnd() % 3 == 0 ? INV_LOC_EQUIP + rnd() % EQ_COUNT : rnd() % 2);
    in.x = (u8)(rnd() % 10);
    in.y = (u8)(rnd() % 8);
    in.rot = (u8)(rnd() % 2);
    in.aux = (u8)(rnd() % 3 == 0 ? 255 : rnd() % 7);
    in.count = (u16)(rnd() % 3 ? 0 : rnd() % 40);
    if (rnd() % 64 == 0) in.type = (u8)(rnd() % 255);   // garbage
    return in;
}

static void test_fuzz(void) {
    Inventory inv;
    fresh(&inv);
    InvTree crate;
    inv_tree_init(&crate, container_find(&g_db, "crate"));
    const char *loadout[] = { "rifle", "pistol", "helmet", "plate_carrier", "medkit", "ammo_rifle", "ammo_9mm", "bandage", "frag_grenade", "fuel_can", "knife", "flashlight" };
    for (int i = 0; i < (int)ARRAY_COUNT(loadout); i++) inv_add(&inv, &g_db, I(loadout[i]), 3, 10);
    const char *loot[] = { "smg", "backpack", "backpack_hiking", "ammo_shell", "water_bottle", "att_scope_4x", "att_suppressor", "att_red_dot", "helmet", "dmr" };
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
    int dropped[MAX_ITEM_DEFS] = { 0 };
    int ok_n = 0, bad = 0, iters = 20000, swaps = 0;
    int crate_c, crate_r;
    grid_dims(&crate, &crate_c, &crate_r);
    for (int k = 0; k < iters && !bad; k++) {
        const InvItem *pk = inv_pack_c(&inv.t);
        u16 pack_item = pk ? pk->s.item : 0, pack_uid = pk ? pk->uid : 0;
        int c0, r0;
        grid_dims(&inv.t, &c0, &r0);
        InvIntent in = random_intent(&inv, &crate);
        InvCtx c = inv_ctx(&inv, &g_db, &crate);
        InvEffect fx;
        int r = inv_intent_apply(&c, &in, &fx);
        if (r == IIR_OK) {
            ok_n++;
            for (int d = 0; d < INV_MAX_ITEMS; d++)
                if (inv_alive(&fx.dropped.items[d]) && fx.dropped.items[d].s.item != g_db.default_backpack) dropped[fx.dropped.items[d].s.item] += fx.dropped.items[d].s.count;
        }
        if (!tree_ok(&inv.t, "fuzz self") || !tree_ok(&crate, "fuzz crate")) { bad = 1; printf("  after %s (#%d)\n", inv_intent_name(in.type), k); }
        if (inv.hands && !inv_get(&inv.t, inv.hands)) { bad = 1; printf("  stale hands after %s\n", inv_intent_name(in.type)); }
        // The grid never changes size except when the worn pack changes (moves, splits, equips of
        // anything else leave it alone); the crate's never changes.
        pk = inv_pack_c(&inv.t);
        int c1, r1;
        grid_dims(&inv.t, &c1, &r1);
        bool pack_changed = !pk || pk->s.item != pack_item || pk->uid != pack_uid;
        if (pack_changed) swaps++;
        if (!pk) { bad = 1; printf("  no backpack after %s\n", inv_intent_name(in.type)); }
        if (!pack_changed && (c1 != c0 || r1 != r0)) { bad = 1; printf("  grid %dx%d -> %dx%d after %s\n", c0, r0, c1, r1, inv_intent_name(in.type)); }
        const InvContainerDef *pcd = pk ? item_container(&g_db, pk->s.item) : NULL;
        if (pcd && (c1 != pcd->comp[0].w || r1 != pcd->comp[0].h)) { bad = 1; printf("  grid %dx%d isn't the pack's\n", c1, r1); }
        int cc, cr;
        grid_dims(&crate, &cc, &cr);
        if (cc != crate_c || cr != crate_r) { bad = 1; printf("  the crate's grid changed\n"); }
        // Conservation (ammo changes form: loaded rounds <-> stacks; attachments: mounted <-> items).
        if (k % 97 == 0) {
            int tot[MAX_ITEM_DEFS] = { 0 };
            totals(&inv.t, tot);
            totals(&crate, tot);
            for (int i = 1; i < g_db.item_count && !bad; i++) {
                const ItemDef *d = &g_db.items[i];
                if (d->kind == ITEM_AMMO || d->kind == ITEM_ATTACHMENT || d->kind == ITEM_CONSUMABLE || d->default_pack) continue;
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
    CHECK(!bad, "fuzz: the invariants, conservation and the fixed grid hold over %d random intents", iters);
    CHECK(ok_n > iters / 50, "fuzz: enough intents applied to mean something (%d)", ok_n);
    CHECK(swaps > 0, "fuzz: the worn pack changed at least once (%d)", swaps);
    printf("  fuzz: %d / %d random intents applied, %d pack changes\n", ok_n, iters, swaps);
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
        if (d->kind == ITEM_AMMO || d->kind == ITEM_ATTACHMENT || d->default_pack) continue;
        if (tot[i] != tot0[i]) { same = false; printf("  %s: %d != %d\n", d->id, tot[i], tot0[i]); }
    }
    CHECK(same, "two clients, one crate, 150 ms: nothing duplicated or lost");
    for (int c = 0; c < 2; c++) {
        CHECK(cl[c].pending_n == 0, "client %d: every intent acknowledged", c);
        CHECK(!memcmp(&cl[c].inv.t, &host_inv[c].t, sizeof(InvTree)) && cl[c].inv.hands == host_inv[c].hands, "client %d converged on its inventory", c);
        CHECK(!memcmp(&cl[c].crate, &host_crate, sizeof(InvTree)), "client %d converged on the crate", c);
    }
    CHECK(tree_ok(&host_crate, "race crate") && tree_ok(&host_inv[0].t, "race 0") && tree_ok(&host_inv[1].t, "race 1"), "invariants");
    CHECK(conflicts > 0, "the race produced conflicts (%d)", conflicts);
    printf("  two clients: %d applied, %d rejected (%d conflicts)\n", applied, rejected, conflicts);
}

// ------------------------------------------------------------------ world loot

// Every stocked world container of the levels and the building kit's prefabs holds its loot: the
// root it names ("grid"; furniture "cabinet", a level container "crate") takes every entry the way
// the loader adds it (loaded weapons, default attachments). Files: the arguments (CMake passes
// data/levels/*.json and the building kit's prefabs).
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
                CHECK(!item_def(&g_db, item)->default_pack, "%s: %s: the default bag is never loot", file, name ? name : "?");
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

int main(int argc, char **argv) {
    CHECK(items_load(&g_db, "data/items.json", "data/weapons"), "items load");
    test_schema();
    test_defs();
    test_fit_matrix();
    test_rotation_stack();
    test_backpacks();
    test_quick_and_swap();
    test_hands();
    test_attachments_fold();
    test_attachment_matrix();
    test_compact();
    test_death_corpse();
    test_wire();
    test_migrate();
    test_world_loot(argc, argv);
    test_fuzz();
    test_two_clients();
    printf("inventory: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
