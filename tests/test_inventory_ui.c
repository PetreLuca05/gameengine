// Inventory screen, pure parts (docs/INVENTORY.md "Screen"): the layout at 16:9 / 21:9 / 4:3 /
// 720p / 4K (everything inside the safe area, nothing overlapping, usable cells), the drag-and-drop
// state machine for every source -> target pair (the intent it produces, or the rejection), click
// gestures, rotation while dragging, the pad carry mode, and pad navigation reaching every target
// at 16:9 and 21:9. Run from the build output directory (data/, assets/).
#include "ui/inventory/inv_ui.h"
#include "gameplay/attachments.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
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

static bool inside(GuiRect in, GuiRect out) {
    return in.x >= out.x - 0.5f && in.y >= out.y - 0.5f && in.x + in.w <= out.x + out.w + 0.5f && in.y + in.h <= out.y + out.h + 0.5f;
}
static bool overlaps(GuiRect a, GuiRect b) {
    return a.x < b.x + b.w - 0.5f && b.x < a.x + a.w - 0.5f && a.y < b.y + b.h - 0.5f && b.y < a.y + a.h - 0.5f;
}

// ------------------------------------------------------------------ layout

static void test_layout(void) {
    static const struct { float w, h, scale, safe_aspect; const char *name; } res[] = {
        { 1920, 1080, 1.0f, 0, "16:9 1080p" }, { 2560, 1080, 1.0f, 2.37f, "21:9" }, { 1024, 768, 0.8f, 0, "4:3" },
        { 1280, 720, 0.667f, 0, "16:9 720p" }, { 3840, 2160, 2.0f, 0, "16:9 4K" }, { 3440, 1440, 1.333f, 2.37f, "21:9 1440p" },
    };
    for (int r = 0; r < (int)ARRAY_COUNT(res); r++) {
        for (int loot = 0; loot < 2; loot++) {
            InvLayoutIn in;
            memset(&in, 0, sizeof(in));
            float m = 0.03f * res[r].h;
            float sw = res[r].w - 2 * m;
            if (res[r].safe_aspect > 0) sw = fminf(sw, (res[r].h - 2 * m) * res[r].safe_aspect);
            in.safe = (GuiRect){ (res[r].w - sw) * 0.5f, m, sw, res[r].h - 2 * m };
            in.scale = res[r].scale;
            in.loot = loot;
            in.loot_cols = 8;
            in.loot_rows = 5;
            in.panel_n = 4;
            in.panel[0].cols = 4; in.panel[0].rows = 1;   // pockets
            in.panel[1].cols = 4; in.panel[1].rows = 4;   // rig
            in.panel[2].cols = 5; in.panel[2].rows = 6;   // backpack
            in.panel[3].cols = 2; in.panel[3].rows = 2;   // secure
            InvLayout L;
            inv_layout(&in, &L);
            const char *n = res[r].name;
            CHECK(L.cell >= 20.0f * res[r].scale, "%s: cells usable (%.1f px)", n, L.cell);
            CHECK(inside(L.left, in.safe) && inside(L.center, in.safe) && inside(L.right, in.safe), "%s: columns in the safe area", n);
            CHECK(!overlaps(L.left, L.center) && !overlaps(L.center, L.right), "%s: columns apart", n);
            CHECK(L.player.w >= 120.0f * res[r].scale, "%s: room for the player (%.0f px)", n, L.player.w);
            for (int i = 0; i < in.panel_n; i++) {
                CHECK(L.panel[i].w >= in.panel[i].cols * L.cell, "%s: panel %d holds its grid", n, i);
                CHECK(fabsf(L.panel[i].x - L.left.x) < 0.5f, "%s: panel %d in the left column", n, i);
            }
            for (int a = 0; a < EQ_COUNT; a++) {
                CHECK(inside(L.equip[a], L.center), "%s: equipment slot %s in the centre", n, equip_slot_name(a));
                CHECK(!overlaps(L.equip[a], L.player) && L.equip[a].w > 10, "%s: %s beside the player", n, equip_slot_name(a));
                for (int b = a + 1; b < EQ_COUNT; b++) CHECK(!overlaps(L.equip[a], L.equip[b]), "%s: %s / %s apart", n, equip_slot_name(a), equip_slot_name(b));
            }
            for (int i = 0; i < INV_HOTBAR; i++) {
                CHECK(inside(L.hot[i], L.right), "%s: hotbar %d in the right column", n, i + 1);
                CHECK(fabsf(L.hot[i].w - L.cell) < 0.5f && fabsf(L.hot[i].h - L.cell) < 0.5f, "%s: hotbar %d is one grid cell (%.0f x %.0f, cell %.0f)", n, i + 1,
                      L.hot[i].w, L.hot[i].h, L.cell);
                CHECK(fabsf(L.hot[i].y - L.hot[0].y) < 0.5f, "%s: hotbar %d in the row", n, i + 1);
                for (int j = i + 1; j < INV_HOTBAR; j++) CHECK(!overlaps(L.hot[i], L.hot[j]), "%s: hotbar tiles apart", n);
                if (loot) CHECK(!overlaps(L.hot[i], L.loot), "%s: hotbar above the loot", n);
            }
            if (loot) {
                CHECK(inside(L.loot, L.right) && L.loot.w >= 8 * L.cell, "%s: the 8-wide trunk fits", n);
                CHECK(L.loot.h >= L.title_h + 3 * L.cell, "%s: loot grid has height (%.0f)", n, L.loot.h);
            } else {
                CHECK(inside(L.hot_info, L.right) && L.hot_info.h > 0 && L.hot_info.y >= L.hot[0].y + L.hot[0].h, "%s: the binds listed under the hotbar row", n);
            }
            CHECK(inside(L.discard, L.center) && !overlaps(L.discard, L.equip[EQ_PRIMARY]), "%s: discard zone", n);
        }
    }
}

// Pad navigation: from every target, every other is reachable (16:9 and 21:9).
static void test_nav(void) {
    float aspect[2] = { 16.0f / 9.0f, 2.37f };
    for (int a = 0; a < 2; a++) {
        InvLayoutIn in;
        memset(&in, 0, sizeof(in));
        in.safe = (GuiRect){ 0, 0, 1080 * aspect[a], 1080 };
        in.scale = 1;
        in.loot = true;
        in.loot_cols = 6;
        in.loot_rows = 4;
        in.panel_n = 2;
        in.panel[0].cols = 4; in.panel[0].rows = 1;
        in.panel[1].cols = 5; in.panel[1].rows = 6;
        InvLayout L;
        inv_layout(&in, &L);
        GuiRect t[256];
        int n = 0;
        for (int p = 0; p < in.panel_n; p++)
            for (int y = 0; y < in.panel[p].rows; y++)
                for (int x = 0; x < in.panel[p].cols; x++)
                    t[n++] = (GuiRect){ L.panel[p].x + L.pad + x * L.cell, L.panel[p].y + L.title_h + y * L.cell, L.cell, L.cell };
        for (int e = 0; e < EQ_COUNT; e++) t[n++] = L.equip[e];
        for (int h = 0; h < INV_HOTBAR; h++) t[n++] = L.hot[h];
        for (int y = 0; y < in.loot_rows; y++)
            for (int x = 0; x < in.loot_cols; x++) t[n++] = (GuiRect){ L.loot.x + L.pad + x * L.cell, L.loot.y + L.title_h + y * L.cell, L.cell, L.cell };
        bool seen[256] = { false };
        int stack[256], sp = 0, reached = 0;
        stack[sp++] = 0;
        seen[0] = true;
        while (sp) {
            int c = stack[--sp];
            reached++;
            for (int d = 0; d < 4; d++) {
                int nx = inv_nav_step(t, n, c, d);
                if (nx >= 0 && !seen[nx]) { seen[nx] = true; stack[sp++] = nx; }
            }
        }
        CHECK(reached == n, "pad navigation reaches all %d targets at %.2f:1 (%d)", n, aspect[a], reached);
    }
}

// ------------------------------------------------------------------ drag and drop

static Inventory g_inv;
static InvTree g_loot;

static u16 put(InvTree *t, const char *id, int count, u16 parent, int loc, int x, int y) {
    InvSlot s = { item_find(&g_db, id), (u16)count, 0, { 0 }, 0 };
    InvItem *it = inv_alloc(t, &g_db, &s);
    it->parent = parent;
    it->loc = (u8)loc;
    it->x = (u8)x;
    it->y = (u8)y;
    return it->uid;
}

static InvView view(void) {
    InvView v;
    v.db = &g_db;
    v.tree[INV_SIDE_SELF] = &g_inv.t;
    v.tree[INV_SIDE_LOOT] = &g_loot;
    v.quick = g_inv.quick;
    return v;
}

static InvTarget cell(int side, u16 parent, int loc, int x, int y) {
    InvTree *t = side ? &g_loot : &g_inv.t;
    InvTarget c = { IT_CELL, (u8)side, (u8)loc, 0, parent, 0, (i16)x, (i16)y };
    InvItem *it = inv_at(t, &g_db, parent, loc, x, y);
    c.uid = it ? it->uid : 0;
    return c;
}
static InvTarget equip(int slot) {
    InvTarget t = { IT_EQUIP, 0, 0, (i8)slot, 0, 0, 0, 0 };
    InvItem *it = inv_equipped(&g_inv.t, slot);
    t.uid = it ? it->uid : 0;
    return t;
}
static InvTarget hot(int k) { InvTarget t = { IT_HOT, 0, 0, (i8)k, 0, g_inv.quick[k], 0, 0 }; return t; }
static InvTarget att(int side, u16 weapon, int slot) { InvTarget t = { IT_ATTACH, (u8)side, 0, (i8)slot, 0, weapon, 0, 0 }; return t; }
static InvTarget simple(int kind) { InvTarget t = { (u8)kind, 0, 0, 0, 0, 0, 0, 0 }; return t; }

// A full gesture: press on src, move past the threshold, release on dst (rotating on the way).
static InvDDOut drag(InvTarget src, InvTarget dst, bool shift, bool rotate) {
    InvDrag d;
    idd_reset(&d);
    InvView v = view();
    InvDDIn in;
    memset(&in, 0, sizeof(in));
    in.threshold = 6;
    in.hover = src;
    in.pressed = true;
    in.shift = shift;
    in.mouse = v2(100, 100);
    idd_update(&d, &in, &v);
    memset(&in, 0, sizeof(in));
    in.threshold = 6;
    in.hover = dst;
    in.mouse = v2(130, 120);
    InvDDOut picked = idd_update(&d, &in, &v);
    CHECK(picked.action == IDA_PICKED, "drag starts past the threshold");
    if (rotate) { in.rotate = true; idd_update(&d, &in, &v); in.rotate = false; }
    in.released = true;
    return idd_update(&d, &in, &v);
}

static void test_dragdrop(void) {
    memset(&g_inv, 0, sizeof(g_inv));
    inv_init(&g_inv, &g_db);
    inv_tree_init(&g_loot, container_find(&g_db, "crate"));
    InvSlot bp = { item_find(&g_db, "backpack_hiking"), 1, 0, { 0 }, 0 };
    InvItem *pack = inv_alloc(&g_inv.t, &g_db, &bp);
    pack->loc = INV_LOC_EQUIP + EQ_BACKPACK;
    u16 pk = pack->uid;
    u16 rifle = put(&g_inv.t, "rifle", 1, pk, 0, 0, 0);
    u16 ammo = put(&g_inv.t, "ammo_rifle", 60, pk, 0, 0, 2);
    u16 band = put(&g_inv.t, "bandage", 1, 0, 1, 0, 0);   // a pocket
    g_inv.quick[2] = band;
    u16 lpistol = put(&g_loot, "pistol", 1, 0, 0, 2, 1);
    u16 scope = put(&g_loot, "att_scope_4x", 1, 0, 0, 0, 3);
    InvDDOut o;
    // CELL -> CELL: a move with the grab offset (grabbed at the rifle's 2nd column).
    InvTarget src = cell(0, pk, 0, 1, 0);
    o = drag(src, cell(0, pk, 0, 2, 3), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_MOVE && o.in.src == rifle && o.in.dst == pk && o.in.x == 1 && o.in.y == 3 && !o.in.rot,
          "cell -> cell: move to (1,3) (grab offset; got %d,%d)", o.in.x, o.in.y);
    // Rotation while dragging: the grab offset turns with it.
    o = drag(src, cell(0, pk, 0, 2, 3), false, true);
    CHECK(o.action == IDA_INTENT && o.in.rot == 1 && o.in.x == 2 && o.in.y == 2, "R while dragging: rotated, grab (0,1) -> (%d,%d)", o.in.x, o.in.y);
    // Off the grid's left edge: rejected before sending.
    o = drag(src, cell(0, pk, 0, 0, 3), false, false);
    CHECK(o.action == IDA_REJECT && o.reason == IIR_NO_ROOM, "footprint off the grid: rejected");
    // Shift-drag a stack: the split dialog with half.
    o = drag(cell(0, pk, 0, 0, 2), cell(0, pk, 0, 4, 4), true, false);
    CHECK(o.action == IDA_SPLIT && o.in.count == 30 && o.in.src == ammo, "shift-drag: split dialog, 30 of 60");
    // CELL (loot) -> CELL (self): across trees.
    o = drag(cell(1, 0, 0, 2, 1), cell(0, pk, 0, 3, 5), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_MOVE && o.in.src_side == INV_SIDE_LOOT && o.in.dst_side == INV_SIDE_SELF && o.in.src == lpistol, "loot -> self move");
    // CELL -> EQUIP: equip (accepting slot) / reject.
    o = drag(cell(1, 0, 0, 2, 1), equip(EQ_HOLSTER), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_EQUIP && o.in.aux == EQ_HOLSTER, "pistol -> holster: equip");
    o = drag(cell(1, 0, 0, 2, 1), equip(EQ_HEADWEAR), false, false);
    CHECK(o.action == IDA_REJECT && o.reason == IIR_EQUIP, "pistol -> headwear: rejected");
    // CELL -> HOT: bind (self) / reject (loot).
    o = drag(src, hot(0), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_BIND && o.in.src == rifle && o.in.aux == 0, "rifle -> hotbar 1: bind");
    o = drag(cell(1, 0, 0, 2, 1), hot(1), false, false);
    CHECK(o.action == IDA_REJECT && o.reason == IDR_TAKE_FIRST, "loot -> hotbar: take it first");
    // HOT -> HOT (moves the bind) / HOT -> elsewhere (unbind).
    o = drag(hot(2), hot(4), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_BIND && o.in.src == band && o.in.aux == 4, "hotbar 3 -> 5: rebind");
    o = drag(hot(2), simple(IT_OUTSIDE), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_BIND && o.in.src == 0 && o.in.aux == 2, "hotbar tile off the bar: unbind");
    // CELL -> ATTACH: mount (compatible) / reject.
    o = drag(cell(1, 0, 0, 0, 3), att(0, rifle, ATT_OPTIC), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_MOUNT && o.in.src == scope && o.in.dst == rifle && o.in.dst_side == INV_SIDE_SELF, "scope -> rifle optic: mount");
    o = drag(cell(1, 0, 0, 0, 3), att(0, rifle, ATT_MUZZLE), false, false);
    CHECK(o.action == IDA_REJECT, "scope -> muzzle slot: rejected");
    o = drag(cell(0, 0, 1, 0, 0), att(0, rifle, ATT_OPTIC), false, false);
    CHECK(o.action == IDA_REJECT && o.reason == IIR_INVALID, "bandage -> optic: rejected (%s)", idd_reason_text(o.reason));
    // ATTACH -> CELL (unmount there) / -> outside (first space).
    o = drag(att(0, rifle, ATT_OPTIC), cell(0, pk, 0, 4, 0), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_UNMOUNT && o.in.aux == ATT_OPTIC && o.in.dst == pk && o.in.x == 4, "chip -> cell: unmount there");
    o = drag(att(0, rifle, ATT_OPTIC), simple(IT_OUTSIDE), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_UNMOUNT && o.in.dst == INV_AUTO, "chip -> outside: unmount to the first space");
    // -> DISCARD / OUTSIDE: a weapon asks first, a stack doesn't.
    o = drag(src, simple(IT_DISCARD), false, false);
    CHECK(o.action == IDA_CONFIRM && o.in.type == II_DISCARD && o.in.src == rifle, "rifle -> discard: confirm");
    o = drag(cell(0, pk, 0, 0, 2), simple(IT_OUTSIDE), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_DISCARD, "ammo -> outside: discard");
    // EQUIP -> CELL: unequip into a grid.
    o = drag(equip(EQ_BACKPACK), cell(1, 0, 0, 0, 0), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_MOVE && o.in.src == pk && o.in.dst_side == INV_SIDE_LOOT, "backpack -> crate cell");
    // -> PANEL / same place: nothing.
    o = drag(src, simple(IT_PANEL), false, false);
    CHECK(o.action == IDA_CANCEL, "released over the panel: nothing");
    // Clicks: select, Ctrl quick move, Alt equip, right click context, double click open.
    InvDrag d;
    idd_reset(&d);
    InvView v = view();
    InvDDIn in;
    memset(&in, 0, sizeof(in));
    in.threshold = 6;
    in.hover = src;
    in.pressed = true;
    idd_update(&d, &in, &v);
    in.pressed = false;
    in.released = true;
    in.ctrl = true;
    o = idd_update(&d, &in, &v);
    CHECK(o.action == IDA_INTENT && o.in.type == II_QUICK_MOVE && o.in.src == rifle, "ctrl+click: quick move");
    in.ctrl = false;
    in.alt = true;
    in.released = false;
    in.pressed = true;
    idd_update(&d, &in, &v);
    in.pressed = false;
    in.released = true;
    o = idd_update(&d, &in, &v);
    CHECK(o.action == IDA_INTENT && o.in.type == II_EQUIP && o.in.aux == 255, "alt+click: equip");
    memset(&in, 0, sizeof(in));
    in.hover = src;
    in.right_pressed = true;
    o = idd_update(&d, &in, &v);
    CHECK(o.action == IDA_CONTEXT && o.at.uid == rifle, "right click: context");
    memset(&in, 0, sizeof(in));
    in.hover = equip(EQ_BACKPACK);
    in.double_clicked = true;
    o = idd_update(&d, &in, &v);
    CHECK(o.action == IDA_OPEN && o.at.uid == pk, "double click: open");
    // Esc while dragging cancels.
    idd_reset(&d);
    memset(&in, 0, sizeof(in));
    in.threshold = 6;
    in.hover = src;
    in.pressed = true;
    idd_update(&d, &in, &v);
    in.pressed = false;
    in.mouse = v2(50, 0);
    idd_update(&d, &in, &v);
    in.cancel = true;
    o = idd_update(&d, &in, &v);
    CHECK(o.action == IDA_CANCEL && d.state == IDD_IDLE, "Esc cancels the drag");
    // Pad carry: A picks up, X rotates, A places.
    idd_reset(&d);
    o = idd_carry_accept(&d, src, &v, false);
    CHECK(o.action == IDA_PICKED && d.carry, "A: pick up");
    memset(&in, 0, sizeof(in));
    in.rotate = true;
    idd_update(&d, &in, &v);
    o = idd_carry_accept(&d, cell(0, pk, 0, 3, 2), &v, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_MOVE && o.in.rot == 1 && !d.carry, "A: place rotated");
    o = idd_carry_accept(&d, cell(0, pk, 0, 4, 5), &v, false);
    CHECK(o.action == IDA_SELECT, "A on an empty cell: select");
}

int main(void) {
    CHECK(items_load(&g_db, "data/items.json", "data/weapons"), "items load");
    test_layout();
    test_nav();
    test_dragdrop();
    printf("inventory_ui: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
