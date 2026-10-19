// Inventory screen, pure parts (docs/INVENTORY.md "Screen"): the layout at 16:9 / 21:9 / 4:3 /
// 720p / 4K / 3440x1440 with and without loot and for every backpack (everything inside the safe area,
// nothing overlapping, usable cells; the cell size never shrinks for a taller grid: the right column
// scrolls; the loot above the player's grid), the drag-and-drop state machine for every source ->
// target pair (the intent it produces, or the rejection; the character as a drop target), click
// gestures, rotation while dragging, the pad carry mode, the category bar (it only dims) and pad
// navigation reaching every target at 16:9 and 21:9. Run from the build output directory.
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

static InvLayoutIn layout_in(float w, float h, float scale, float safe_aspect, int rows, bool loot, int loot_cols, int loot_rows, bool body) {
    InvLayoutIn in;
    memset(&in, 0, sizeof(in));
    float m = 0.035f * h;
    float sw = w - 2 * m;
    if (safe_aspect > 0) sw = fminf(sw, (h - 2 * m) * safe_aspect);
    in.safe = (GuiRect){ (w - sw) * 0.5f, m, sw, h - 2 * m };
    in.scale = scale;
    in.grid_cols = 10;
    in.grid_rows = rows;
    in.loot = loot;
    in.loot_body = body;
    in.loot_cols = loot_cols;
    in.loot_rows = loot_rows;
    return in;
}

static void test_layout(void) {
    static const struct { float w, h, scale, safe_aspect; const char *name; } res[] = {
        { 1920, 1080, 1.0f, 0, "16:9 1080p" }, { 2560, 1080, 1.0f, 2.39f, "21:9" }, { 1024, 768, 0.711f, 0, "4:3" },
        { 1280, 720, 0.667f, 0, "16:9 720p" }, { 3840, 2160, 2.0f, 0, "16:9 4K" }, { 3440, 1440, 1.333f, 2.39f, "21:9 1440p" },
    };
    static const int rows[3] = { 4, 6, 8 };   // the three backpacks
    for (int r = 0; r < (int)ARRAY_COUNT(res); r++) {
        float cell0 = -1;
        for (int k = 0; k < 3; k++)
            for (int loot = 0; loot < 3; loot++) {   // none, a trunk (8x5), a corpse (its equipment row + a 10x8 pack)
                InvLayoutIn in = layout_in(res[r].w, res[r].h, res[r].scale, res[r].safe_aspect, rows[k], loot > 0, loot == 1 ? 8 : 10, loot == 1 ? 5 : 8, loot == 2);
                InvLayout L;
                inv_layout(&in, &L);
                const char *n = res[r].name;
                if (cell0 < 0) cell0 = L.cell;
                CHECK(fabsf(L.cell - cell0) < 0.01f, "%s: one cell size for every pack and container (%.1f vs %.1f)", n, L.cell, cell0);
                CHECK(L.cell >= 20.0f * res[r].scale, "%s: cells usable (%.1f px)", n, L.cell);
                float sb = in.safe.y + in.safe.h;
                CHECK(inside(L.left, in.safe) && inside(L.center, in.safe) && inside(L.right, in.safe) && inside(L.gear, in.safe), "%s: columns in the safe area", n);
                CHECK(!overlaps(L.left, L.center) && !overlaps(L.center, L.right) && !overlaps(L.right, L.gear) && L.left.x < L.center.x && L.center.x < L.right.x && L.right.x < L.gear.x,
                      "%s: left, centre, grid panel, gear apart", n);
                CHECK(L.player.w >= 280.0f * res[r].scale && fabsf(L.player.x - L.center.x) < 0.5f, "%s: the character between the panels (%.0f px)", n, L.player.w);
                // The header: the category bar above the divider (the header's bottom), right of the title or on a row of its own.
                CHECK(inside(L.cats, L.header) && fabsf(L.header.x - in.safe.x) < 0.5f && fabsf(L.header.w - in.safe.w) < 0.5f, "%s: the category bar in the header", n);
                CHECK(L.cats.x >= L.header.x + L.header.w * 0.5f + 60 * res[r].scale || L.cats.y >= L.header.y + 40 * res[r].scale, "%s: the category bar clear of the centred title", n);
                CHECK(L.right.y >= L.header.y + L.header.h && L.left.y >= L.header.y + L.header.h, "%s: the columns under the divider", n);
                // The grid panel and the gear column reach the bottom of the safe area; the footer stays left of them.
                CHECK(fabsf(L.right.y + L.right.h - sb) < 0.5f && fabsf(L.gear.y + L.gear.h - sb) < 0.5f, "%s: the grid panel down to the bottom", n);
                CHECK(L.footer.x + L.footer.w <= L.right.x + 0.5f && L.footer.y >= L.left.y + L.left.h - 0.5f, "%s: the footer under the left and centre columns", n);
                static const int weapons[3] = { EQ_PRIMARY, EQ_SECONDARY, EQ_HOLSTER }, gear[3] = { EQ_HEADWEAR, EQ_BODY_ARMOR, EQ_BACKPACK };
                for (int a = 0; a < 3; a++) {
                    GuiRect wr = L.equip[weapons[a]], gr = L.equip[gear[a]];
                    CHECK(inside(wr, L.left) && fabsf(wr.w - L.left.w) < 0.5f && wr.h >= 90 * res[r].scale, "%s: %s across the left column, tall (%.0f px)", n, equip_slot_name(weapons[a]), wr.h);
                    CHECK(inside(gr, L.gear) && fabsf(gr.w - gr.h) < 0.5f && gr.w >= 48 * res[r].scale, "%s: %s a square in the gear column", n, equip_slot_name(gear[a]));
                    if (a) CHECK(gr.y > L.equip[gear[a - 1]].y + L.equip[gear[a - 1]].h - 0.5f && fabsf(gr.x - L.equip[gear[a - 1]].x) < 0.5f, "%s: %s stacked under %s", n, equip_slot_name(gear[a]), equip_slot_name(gear[a - 1]));
                }
                for (int a = 0; a < EQ_COUNT; a++) {
                    for (int b = a + 1; b < EQ_COUNT; b++) CHECK(!overlaps(L.equip[a], L.equip[b]), "%s: %s / %s apart", n, equip_slot_name(a), equip_slot_name(b));
                    for (int c = 0; c < INV_UI_WEAPON_ROWS; c++) CHECK(!overlaps(L.equip[a], L.att_row[c]), "%s: %s clear of the chip rows", n, equip_slot_name(a));
                }
                for (int c = 0; c < INV_UI_WEAPON_ROWS; c++) {
                    int slot = c == 0 ? EQ_PRIMARY : EQ_SECONDARY;
                    CHECK(inside(L.att_row[c], L.left) && L.att_row[c].y >= L.equip[slot].y + L.equip[slot].h - 0.5f && L.att_row[c].h >= 30 * res[r].scale,
                          "%s: the %s's chips right under it", n, equip_slot_name(slot));
                    CHECK(fabsf(L.chip * ATT_SLOTS + L.chip_gap * (ATT_SLOTS - 1) - L.equip[slot].w) < 0.5f && fabsf(L.att_row[c].h - L.chip) < 0.5f,
                          "%s: the %s's chips are squares across its width", n, equip_slot_name(slot));
                }
                CHECK(inside(L.view, L.right) && L.grid.w >= 10 * L.cell - 0.5f && L.grid.h >= L.title_h + rows[k] * L.cell, "%s: the player's grid fits the panel (%d rows)", n, rows[k]);
                CHECK(fabsf(L.grid.x - L.view.x) < 0.5f && L.grid.y >= L.view.y - 0.5f, "%s: the grid in the grid panel", n);
                if (loot) {
                    CHECK(L.loot.w >= in.loot_cols * L.cell && L.loot.h >= L.title_h + in.loot_rows * L.cell, "%s: the loot grid fits", n);
                    CHECK(L.loot.y + L.loot.h <= L.grid.y + 0.5f && fabsf(L.loot.y - L.view.y) < 0.5f, "%s: the world container above the player's grid", n);
                }
                CHECK(fabsf(L.content_h - (L.grid.y + L.grid.h - L.view.y)) < 0.5f, "%s: content height", n);
                CHECK(inside(L.discard, L.center), "%s: discard zone", n);
            }
        // A 10x20 stash over a 10x8 pack doesn't shrink the cells: the column scrolls.
        InvLayoutIn big = layout_in(res[r].w, res[r].h, res[r].scale, res[r].safe_aspect, 8, true, 10, 20, false);
        InvLayout L;
        inv_layout(&big, &L);
        CHECK(fabsf(L.cell - cell0) < 0.01f && L.content_h > L.view.h, "%s: a tall stash scrolls (content %.0f > view %.0f), cells unchanged", res[r].name, L.content_h, L.view.h);
    }
}

// Pad navigation: from every target, every other is reachable (16:9 and 21:9).
static void test_nav(void) {
    float aspect[2] = { 16.0f / 9.0f, 2.37f };
    for (int a = 0; a < 2; a++) {
        InvLayoutIn in = layout_in(1080 * aspect[a], 1080, 1, 0, 6, true, 6, 4, false);
        InvLayout L;
        inv_layout(&in, &L);
        GuiRect t[512];
        int n = 0;
        for (int e = 0; e < EQ_COUNT; e++) t[n++] = L.equip[e];
        for (int c = 0; c < INV_UI_WEAPON_ROWS; c++)
            for (int k = 0; k < ATT_SLOTS; k++) t[n++] = (GuiRect){ L.att_row[c].x + k * (L.chip + L.chip_gap), L.att_row[c].y, L.chip, L.chip };
        for (int y = 0; y < in.loot_rows; y++)
            for (int x = 0; x < in.loot_cols; x++) t[n++] = (GuiRect){ L.loot.x + x * L.cell, L.loot.y + L.title_h + y * L.cell, L.cell, L.cell };
        for (int y = 0; y < in.grid_rows; y++)
            for (int x = 0; x < in.grid_cols; x++) t[n++] = (GuiRect){ L.grid.x + x * L.cell, L.grid.y + L.title_h + y * L.cell, L.cell, L.cell };
        bool seen[512] = { false };
        int stack[512], sp = 0, reached = 0;
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
    InvItem *old = inv_pack(&g_inv.t);
    if (old) memset(old, 0, sizeof(*old));
    InvItem *pack = inv_alloc(&g_inv.t, &g_db, &bp);
    pack->loc = INV_LOC_EQUIP + EQ_BACKPACK;
    u16 pk = pack->uid;
    u16 rifle = put(&g_inv.t, "rifle", 1, pk, 0, 0, 0);
    u16 ammo = put(&g_inv.t, "ammo_rifle", 60, pk, 0, 0, 2);
    u16 band = put(&g_inv.t, "bandage", 1, pk, 0, 9, 7);
    u16 smg = put(&g_inv.t, "smg", 1, 0, INV_LOC_EQUIP + EQ_PRIMARY, 0, 0);
    u16 lpistol = put(&g_loot, "pistol", 1, 0, 0, 2, 1);
    u16 scope = put(&g_loot, "att_scope_4x", 1, 0, 0, 0, 3);
    (void)smg;
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
    CHECK(o.action == IDA_REJECT && o.reason == IIR_EQUIP, "pistol -> helmet slot: rejected");
    o = drag(src, equip(EQ_BACKPACK), false, false);
    CHECK(o.action == IDA_REJECT && o.reason == IIR_EQUIP, "rifle -> backpack slot: rejected");
    // CELL -> the character: worn in its own slot; not wearable: rejected.
    o = drag(src, simple(IT_CHAR), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_EQUIP && o.in.aux == 255 && o.in.src == rifle, "rifle -> the character: equip into its slot");
    o = drag(cell(1, 0, 0, 2, 1), simple(IT_CHAR), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_EQUIP && o.in.src_side == INV_SIDE_LOOT, "loot pistol -> the character: equip");
    o = drag(cell(0, pk, 0, 9, 7), simple(IT_CHAR), false, false);
    CHECK(o.action == IDA_REJECT && o.reason == IIR_EQUIP, "bandage -> the character: not wearable");
    o = drag(equip(EQ_PRIMARY), simple(IT_CHAR), false, false);
    CHECK(o.action == IDA_CANCEL, "a worn weapon dropped back on the character: nothing");
    // CELL -> ATTACH: mount (compatible) / reject.
    o = drag(cell(1, 0, 0, 0, 3), att(0, rifle, ATT_OPTIC), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_MOUNT && o.in.src == scope && o.in.dst == rifle && o.in.dst_side == INV_SIDE_SELF, "scope -> rifle optic: mount");
    o = drag(cell(1, 0, 0, 0, 3), att(0, rifle, ATT_MUZZLE), false, false);
    CHECK(o.action == IDA_REJECT, "scope -> muzzle slot: rejected");
    o = drag(cell(0, pk, 0, 9, 7), att(0, rifle, ATT_OPTIC), false, false);
    CHECK(o.action == IDA_REJECT && o.reason == IIR_INVALID, "bandage -> optic: rejected (%s)", idd_reason_text(o.reason));
    // ATTACH -> CELL (unmount there) / -> outside (first space). An empty chip can't be picked up.
    InvItem *ri = inv_get(&g_inv.t, rifle);
    ri->s.att[ATT_OPTIC] = attach_find(&g_db, "red_dot");
    o = drag(att(0, rifle, ATT_OPTIC), cell(0, pk, 0, 4, 0), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_UNMOUNT && o.in.aux == ATT_OPTIC && o.in.dst == pk && o.in.x == 4, "chip -> cell: unmount there");
    o = drag(att(0, rifle, ATT_OPTIC), simple(IT_OUTSIDE), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_UNMOUNT && o.in.dst == INV_AUTO, "chip -> outside: unmount to the first space");
    ri->s.att[ATT_OPTIC] = 0;
    {
        InvDrag d;
        idd_reset(&d);
        InvView v = view();
        InvDDIn in;
        memset(&in, 0, sizeof(in));
        in.hover = att(0, rifle, ATT_OPTIC);
        in.pressed = true;
        idd_update(&d, &in, &v);
        CHECK(d.state == IDD_IDLE, "an empty chip isn't picked up");
    }
    // -> DISCARD / OUTSIDE: a weapon asks first, a stack doesn't.
    o = drag(src, simple(IT_DISCARD), false, false);
    CHECK(o.action == IDA_CONFIRM && o.in.type == II_DISCARD && o.in.src == rifle, "rifle -> discard: confirm");
    o = drag(cell(0, pk, 0, 0, 2), simple(IT_OUTSIDE), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_DISCARD, "ammo -> outside: discard");
    // EQUIP -> CELL: off the back into a crate.
    o = drag(equip(EQ_BACKPACK), cell(1, 0, 0, 0, 0), false, false);
    CHECK(o.action == IDA_INTENT && o.in.type == II_MOVE && o.in.src == pk && o.in.dst_side == INV_SIDE_LOOT, "backpack -> crate cell");
    // -> PANEL: nothing.
    o = drag(src, simple(IT_PANEL), false, false);
    CHECK(o.action == IDA_CANCEL, "released over the panel: nothing");
    // Clicks: select, Ctrl quick move, Alt equip, right click context, double click inspect.
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
    CHECK(o.action == IDA_OPEN && o.at.uid == pk, "double click: inspect");
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
    (void)band;
}

// The category bar only dims: the item tree is untouched, the dims agree with the categories.
static void test_categories(void) {
    InvTree before = g_inv.t;
    int lit[ICAT_COUNT + 1] = { 0 };
    for (int cat = 0; cat <= ICAT_COUNT; cat++)
        for (int i = 1; i < g_db.item_count; i++) {
            const ItemDef *d = &g_db.items[i];
            bool dim = inv_cat_dims(cat, d);
            CHECK(dim == (cat > 0 && d->category != cat - 1), "%s under category %d", d->id, cat);
            lit[cat] += !dim;
        }
    CHECK(lit[0] == g_db.item_count - 1, "All lights everything");
    for (int c = 1; c <= ICAT_COUNT; c++) CHECK(lit[c] > 0, "category %s has items", inv_category_name(c - 1));
    CHECK(!memcmp(&before, &g_inv.t, sizeof(before)), "highlighting never moves anything");
}

int main(void) {
    CHECK(items_load(&g_db, "data/items.json", "data/weapons"), "items load");
    test_layout();
    test_nav();
    test_dragdrop();
    test_categories();
    printf("inventory_ui: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
