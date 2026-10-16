// Game UI (docs/UI.md): the layout engine at 16:9 / 21:9 / 4:3 (+ 720p, 4K), the inventory drag
// and drop state machine, stack merge math, attachment compatibility and mount / unmount,
// client prediction + reconcile of intents, and an icon for every item. Run from the build
// output directory (data/, assets/).
#include "ui/gui/gui_layout.h"
#include "ui/inv_dragdrop.h"
#include "gameplay/inv_ops.h"
#include "gameplay/attachments.h"
#include "core/engine.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;

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

// ------------------------------------------------------------------ layout

static void test_layout_basics(void) {
    LayoutNode n[6];
    memset(n, 0, sizeof(n));
    // root (overlay) -> row (bottom centre, gap 10) with a fixed child, a flex child and an aspect child
    for (int i = 0; i < 6; i++) n[i].parent = -1;
    dg_strcpy(n[0].id, sizeof(n[0].id), "root");
    dg_strcpy(n[1].id, sizeof(n[1].id), "row");
    n[1].parent = 0; n[1].dir = GD_ROW; n[1].anchor = GA_B; n[1].w = 500; n[1].h = 40; n[1].gap = 10; n[1].align = GAL_STRETCH;
    n[2].parent = 1; n[2].w = 100;
    n[3].parent = 1; n[3].flex = 1;
    n[4].parent = 1; n[4].aspect = 2.0f; n[4].h = 40;
    LayoutParams p = { 1920, 1080, 1.0f, 0.0f, 0.0f };
    gui_layout_solve(n, 5, &p);
    CHECK(fabsf(n[1].rect.x - 710) < 0.5f && fabsf(n[1].rect.y - 1040) < 0.5f, "bottom-centre anchor (%.1f %.1f)", n[1].rect.x, n[1].rect.y);
    CHECK(fabsf(n[4].rect.w - 80) < 0.5f, "aspect child 80 wide (%.1f)", n[4].rect.w);
    CHECK(fabsf(n[3].rect.w - (500 - 100 - 80 - 20)) < 0.5f, "flex child takes the rest (%.1f)", n[3].rect.w);
    CHECK(fabsf(n[2].rect.h - 40) < 0.5f, "stretch on the cross axis");
    CHECK(fabsf(n[3].rect.x - (n[2].rect.x + 110)) < 0.5f, "gap between stacked children");
    // DPI scale: everything doubles at 4K.
    LayoutParams p2 = { 3840, 2160, 2.0f, 0.0f, 0.0f };
    gui_layout_solve(n, 5, &p2);
    CHECK(fabsf(n[1].rect.w - 1000) < 0.5f && fabsf(n[3].rect.w - 600) < 0.5f, "scale 2: sizes double");
    CHECK(fabsf(gui_layout_scale(1920, 1080, 0) - 1.0f) < 1e-4f && fabsf(gui_layout_scale(3840, 2160, 0) - 2.0f) < 1e-4f, "auto scale 1080p / 4K");
    CHECK(gui_layout_scale(1024, 768, 0) * 1280.0f <= 1024.5f, "4:3 keeps 1280 design px across");
    CHECK(fabsf(gui_layout_scale(1920, 1080, 1.5f) - 1.5f) < 1e-4f, "ui_scale overrides");
}

static void test_layout_pause(void) {
    size_t size = 0;
    char *json = platform_read_file("assets/ui/screens/pause.json", &size);
    CHECK(json != NULL, "assets/ui/screens/pause.json readable");
    if (!json) return;
    static LayoutNode base[GUI_LAYOUT_MAX_NODES];
    int count = 0;
    char err[160];
    bool ok = gui_layout_parse(json, base, GUI_LAYOUT_MAX_NODES, &count, err, sizeof(err));
    free(json);
    CHECK(ok && count > 5, "pause.json parses (%s)", err);
    static const struct { float w, h; const char *name; } res[] = {
        { 1920, 1080, "16:9 1080p" }, { 2560, 1080, "21:9" }, { 1024, 768, "4:3" }, { 1280, 720, "16:9 720p" }, { 3840, 2160, "16:9 4K" }, { 3440, 1440, "21:9 1440p" },
    };
    float stack_h_1080 = 0;
    for (int r = 0; r < (int)ARRAY_COUNT(res); r++) {
        static LayoutNode n[GUI_LAYOUT_MAX_NODES];
        memcpy(n, base, sizeof(LayoutNode) * (size_t)count);
        LayoutParams p = { res[r].w, res[r].h, gui_layout_scale(res[r].w, res[r].h, 0), 0.035f, 2.39f };
        gui_layout_solve(n, count, &p);
        GuiRect safe = gui_layout_safe_rect(&p), view = { 0, 0, res[r].w, res[r].h };
        int st = gui_layout_find(n, count, "stack"), md = gui_layout_find(n, count, "modal"), br = gui_layout_find(n, count, "brand"),
            dl = gui_layout_find(n, count, "dialog");
        CHECK(st >= 0 && md >= 0 && br >= 0 && dl >= 0, "%s: nodes present", res[r].name);
        if (st < 0 || md < 0 || br < 0 || dl < 0) continue;
        CHECK(inside(n[st].rect, safe), "%s: button stack inside the safe area", res[r].name);
        CHECK(fabsf(n[st].rect.x - safe.x) < 0.5f && fabsf(n[st].rect.y + n[st].rect.h - (safe.y + safe.h)) < 0.5f, "%s: stack anchored bottom left", res[r].name);
        CHECK(inside(n[br].rect, safe) && fabsf(n[br].rect.x + n[br].rect.w - (safe.x + safe.w)) < 0.5f, "%s: logo bottom right", res[r].name);
        CHECK(inside(n[md].rect, view), "%s: modal on screen", res[r].name);
        CHECK(!gui_rect_overlap(n[md].rect, n[st].rect), "%s: modal beside the stack (%.0f..%.0f vs stack ..%.0f)", res[r].name, n[md].rect.x,
              n[md].rect.x + n[md].rect.w, n[st].rect.x + n[st].rect.w);
        CHECK(n[md].rect.w <= res[r].w * 0.6f + 0.5f || n[md].rect.w <= n[md].min_w * p.scale + 0.5f, "%s: modal at most 60%% wide (%.0f)", res[r].name, n[md].rect.w);
        CHECK(n[md].rect.w >= 300 * p.scale, "%s: modal usable width (%.0f)", res[r].name, n[md].rect.w);
        CHECK(fabsf(n[dl].rect.x + n[dl].rect.w * 0.5f - res[r].w * 0.5f) < 1.0f, "%s: dialog centred", res[r].name);
        // Buttons: every stack child inside the stack, ordered top to bottom.
        float prev = -1;
        for (int i = 0; i < count; i++) {
            if (n[i].parent != st) continue;
            CHECK(inside(n[i].rect, n[st].rect), "%s: %s inside the stack", res[r].name, n[i].id);
            CHECK(n[i].rect.y >= prev, "%s: %s in order", res[r].name, n[i].id);
            prev = n[i].rect.y;
        }
        if (res[r].w == 1920) stack_h_1080 = n[st].rect.h;
        if (res[r].w == 3840) CHECK(fabsf(n[st].rect.h - stack_h_1080 * 2.0f) < 1.0f, "4K: the stack is exactly twice the 1080p size");
        if (res[r].w / res[r].h > 2.3f) CHECK(safe.w / safe.h <= 2.39f + 1e-3f, "%s: safe area aspect limited", res[r].name);
    }
    // Hidden nodes take no space.
    static LayoutNode n[GUI_LAYOUT_MAX_NODES];
    memcpy(n, base, sizeof(LayoutNode) * (size_t)count);
    LayoutParams p = { 1920, 1080, 1.0f, 0.035f, 2.39f };
    gui_layout_solve(n, count, &p);
    float h_all = n[gui_layout_find(n, count, "stack")].rect.h;
    n[gui_layout_find(n, count, "invite")].hidden = true;
    gui_layout_solve(n, count, &p);
    CHECK(n[gui_layout_find(n, count, "stack")].rect.h < h_all - 30, "a hidden button shrinks the stack");
    // Errors: unknown parent.
    int c2;
    CHECK(!gui_layout_parse("{\"nodes\":[{\"id\":\"a\",\"parent\":\"nope\"}]}", n, 8, &c2, err, sizeof(err)) && strstr(err, "nope"), "unknown parent reported");
}

// ------------------------------------------------------------------ inventory

static ItemDB g_db;

static u16 item(const char *id) {
    u16 i = item_find(&g_db, id);
    CHECK(i != ITEM_NONE, "item %s exists", id);
    return i;
}

static void put(Inventory *inv, int slot, const char *id, int count) {
    memset(&inv->slots[slot], 0, sizeof(InvSlot));
    inv->slots[slot].item = item(id);
    inv->slots[slot].count = (u16)count;
    const WeaponDef *wd = item_weapon(&g_db, inv->slots[slot].item);
    if (wd) inv->slots[slot].ammo = (i16)wd->mag_size;
}

static InvOp mk(int type, int a, int b, int c, int count) {
    InvOp o;
    memset(&o, 0, sizeof(o));
    o.type = (u8)type; o.a = (u8)a; o.b = (u8)b; o.c = (u8)c; o.count = (u16)count;
    return o;
}

static void test_merge(void) {
    // ammo_rifle stacks to 90.
    u16 ammo = item("ammo_rifle");
    int ms = item_def(&g_db, ammo)->max_stack;
    InvSlot a = { ammo, 60, 0, { 0 }, 0 }, b = { ammo, 50, 0, { 0 }, 0 };
    int moved = inv_merge(&a, &b, &g_db, 0);
    CHECK(moved == ms - 60 && a.count == ms && b.count == 50 - (ms - 60), "merge fills the stack, the remainder stays (%d moved, %d left)", moved, b.count);
    InvSlot e = { 0 }, c = { ammo, 40, 0, { 0 }, 0 };
    moved = inv_merge(&e, &c, &g_db, 15);
    CHECK(moved == 15 && e.count == 15 && c.count == 25 && e.item == ammo, "partial move into an empty slot");
    InvSlot d = { ammo, 10, 0, { 0 }, 0 }, f = { item("scrap"), 5, 0, { 0 }, 0 };
    CHECK(inv_merge(&f, &d, &g_db, 0) == 0 && d.count == 10, "different items never merge");
    InvSlot g = { ammo, 7, 0, { 0 }, 0 }, h = { ammo, 3, 0, { 0 }, 0 };
    inv_merge(&g, &h, &g_db, 0);
    CHECK(g.count == 10 && h.item == ITEM_NONE && h.count == 0, "an emptied source slot is cleared");
    // Totals are conserved for any pair of counts.
    int bad = 0;
    for (int x = 1; x <= ms; x += 7)
        for (int y = 1; y <= ms; y += 5)
            for (int n = 0; n <= 3; n++) {
                InvSlot p = { ammo, (u16)x, 0, { 0 }, 0 }, q = { ammo, (u16)y, 0, { 0 }, 0 };
                inv_merge(&p, &q, &g_db, n * 13);
                if (p.count + q.count != x + y || p.count > ms) bad++;
            }
    CHECK(bad == 0, "merge conserves the total and respects the stack size (%d bad)", bad);

    Inventory inv;
    memset(&inv, 0, sizeof(inv));
    put(&inv, 9, "ammo_rifle", 80);
    put(&inv, 10, "ammo_rifle", 30);
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_MOVE, 10, 9, 0, 0, 0 }, NULL) == IOR_OK && inv.slots[9].count == 90 && inv.slots[10].count == 20, "move merges, remainder stays");
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_MOVE, 10, 9, 0, 0, 0 }, NULL) == IOR_OK && inv.slots[9].count == 20 && inv.slots[10].count == 90, "full target: swap");
    put(&inv, 11, "scrap", 9);
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_SPLIT, 11, 255, 0, 0, 0 }, NULL) == IOR_OK && inv.slots[11].count == 5, "split leaves the bigger half");
    int split_to = -1;
    for (int i = 0; i < INV_SLOTS; i++) if (i != 11 && inv.slots[i].item == item("scrap")) split_to = i;
    CHECK(split_to >= 0 && inv.slots[split_to].count == 4, "split half in a free slot");
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_MOVE, 11, 9, 0, 2, 0 }, NULL) == IOR_INVALID, "a partial stack can't swap with another item");
    InvOpEffect fx;
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_DROP, 11, 0, 0, 2, 0 }, &fx) == IOR_OK && fx.dropped.count == 2 && inv.slots[11].count == 3, "drop part of a stack");
    put(&inv, 2, "medkit", 2);
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_USE, 2, 0, 0, 0, 0 }, &fx) == IOR_OK && fx.used_item == item("medkit") && inv.slots[2].count == 1, "use a consumable");
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_USE, 9, 0, 0, 0, 0 }, &fx) == IOR_NOT_USABLE, "ammo can't be used");
    // Quick move: hotbar -> backpack merges into an existing stack first.
    memset(&inv, 0, sizeof(inv));
    put(&inv, 3, "scrap", 6);
    put(&inv, 15, "scrap", 18);
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_QUICK_MOVE, 3, 0, 0, 0, 0 }, NULL) == IOR_OK && inv.slots[15].count == 20 && inv.slots[3].item == 0,
          "quick move merges into the backpack stack, the remainder goes to a free slot");
    int scrap_total = 0;
    for (int i = 0; i < INV_SLOTS; i++) if (inv.slots[i].item == item("scrap")) scrap_total += inv.slots[i].count;
    CHECK(scrap_total == 24, "quick move conserves the count (%d)", scrap_total);
    // Equip from the backpack: the first empty hotbar slot.
    memset(&inv, 0, sizeof(inv));
    put(&inv, 0, "rifle", 1);
    put(&inv, 20, "pistol", 1);
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_EQUIP, 20, 0, 0, 0, 0 }, NULL) == IOR_OK && inv.slots[1].item == item("pistol"), "equip into the first free hotbar slot");
}

static void test_attachments(void) {
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
            int aslot = -1;
            int r = inv_attach_check(&g_db, &ws, ai, &aslot);
            bool expect = wd->kind != WPN_MELEE && wd->att_slots[ad->slot].present && attach_allowed(wd, ad);
            if ((r == IOR_OK) != expect) mismatch++;
            if (r == IOR_OK && aslot != ad->slot) mismatch++;
            combos++;
            // Mount / unmount round trip in a real inventory.
            if (r == IOR_OK) {
                Inventory inv;
                memset(&inv, 0, sizeof(inv));
                inv.slots[0] = ws;
                inv.slots[12].item = ai;
                inv.slots[12].count = 1;
                int m = inv_op_apply(&inv, &g_db, &(InvOp){ IOP_MOUNT, 0, 12, 0, 0, 0 }, NULL);
                if (m != IOR_OK || inv.slots[0].att[ad->slot] != a || inv.slots[12].item) mismatch++;
                int u = inv_op_apply(&inv, &g_db, &(InvOp){ IOP_UNMOUNT, 0, (u8)ad->slot, 255, 0, 0 }, NULL);
                int back = -1;
                for (int i = 0; i < INV_SLOTS; i++) if (inv.slots[i].item == ai) back = i;
                if (u != IOR_OK || inv.slots[0].att[ad->slot] || back < 0) mismatch++;
            }
        }
    }
    CHECK(combos > 50 && mismatch == 0, "attachment compatibility + mount / unmount for every weapon x attachment (%d combos, %d mismatches)", combos, mismatch);
    // Replacing an attachment returns the old one in the dragged item's place.
    Inventory inv;
    memset(&inv, 0, sizeof(inv));
    put(&inv, 0, "rifle", 1);
    memset(inv.slots[0].att, 0, ATT_SLOTS);
    u8 rd = attach_find(&g_db, "red_dot"), s4 = attach_find(&g_db, "scope_4x");
    inv.slots[0].att[ATT_OPTIC] = rd;
    inv.slots[9].item = item_for_attachment(&g_db, s4);
    inv.slots[9].count = 1;
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_MOUNT, 0, 9, 0, 0, 0 }, NULL) == IOR_OK && inv.slots[0].att[ATT_OPTIC] == s4 &&
              inv.slots[9].item == item_for_attachment(&g_db, rd), "swap optics: the red dot comes back where the scope was");
    // Not a weapon / no slot.
    memset(&inv, 0, sizeof(inv));
    put(&inv, 0, "medkit", 1);
    inv.slots[9].item = item_for_attachment(&g_db, rd);
    inv.slots[9].count = 1;
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_MOUNT, 0, 9, 0, 0, 0 }, NULL) == IOR_NOT_WEAPON, "mount onto a medkit: not a weapon");
    put(&inv, 1, "knife", 1);
    CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_MOUNT, 1, 9, 0, 0, 0 }, NULL) == IOR_NOT_WEAPON, "melee weapons take no attachments");
    // Extended magazine removed: the extra rounds go back to the reserve.
    u8 em = attach_find(&g_db, "extended_mag");
    if (em) {
        memset(&inv, 0, sizeof(inv));
        put(&inv, 0, "rifle", 1);
        memset(inv.slots[0].att, 0, ATT_SLOTS);
        inv.slots[0].att[ATT_MAGAZINE] = em;
        WeaponDef eff;
        attach_weapon_effective(&g_db, item_weapon(&g_db, inv.slots[0].item), inv.slots[0].att, &eff);
        inv.slots[0].ammo = (i16)eff.mag_size;
        int before = inv.slots[0].ammo;
        CHECK(inv_op_apply(&inv, &g_db, &(InvOp){ IOP_UNMOUNT, 0, ATT_MAGAZINE, 255, 0, 0 }, NULL) == IOR_OK, "unmount the extended magazine");
        int reserve = inv_count(&inv, item_weapon(&g_db, inv.slots[0].item)->ammo_item);
        CHECK(inv.slots[0].ammo + reserve == before && inv.slots[0].ammo == item_weapon(&g_db, inv.slots[0].item)->mag_size, "extra rounds to the reserve (%d + %d)",
              inv.slots[0].ammo, reserve);
    }
}

// ------------------------------------------------------------------ drag and drop

static DDInput frame(float x, float y) {
    DDInput in;
    memset(&in, 0, sizeof(in));
    in.mouse = v2(x, y);
    in.threshold = 6;
    in.weapon_slot = -1;
    return in;
}

static void test_dragdrop(void) {
    Inventory inv;
    memset(&inv, 0, sizeof(inv));
    put(&inv, 0, "rifle", 1);
    memset(inv.slots[0].att, 0, ATT_SLOTS);
    put(&inv, 1, "knife", 1);
    put(&inv, 9, "ammo_rifle", 60);
    put(&inv, 10, "scrap", 10);
    inv.slots[11].item = item_for_attachment(&g_db, attach_find(&g_db, "red_dot"));
    inv.slots[11].count = 1;
    DragDrop d;
    dd_reset(&d);
    DDTarget s9 = { DT_SLOT, 9 }, s20 = { DT_SLOT, 20 }, s0 = { DT_SLOT, 0 }, s1 = { DT_SLOT, 1 }, s11 = { DT_SLOT, 11 }, s10 = { DT_SLOT, 10 };
    // Click (press + release without moving) selects.
    DDInput in = frame(10, 10);
    in.pressed = true;
    in.hover = s9;
    DDResult r = dd_update(&d, &in, &inv, &g_db);
    CHECK(d.state == DD_PRESSED && r.action == DDA_NONE, "press: pressed");
    in = frame(12, 11);
    in.released = true;
    in.hover = s9;
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(r.action == DDA_SELECT && r.slot == 9 && d.state == DD_IDLE, "click selects");
    // Drag past the threshold, release on an empty slot: move.
    in = frame(10, 10); in.pressed = true; in.hover = s9;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(30, 10); in.hover = s9;
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(d.state == DD_DRAGGING && r.action == DDA_PICKED, "moving past the threshold starts the drag");
    in = frame(200, 10); in.released = true; in.hover = s20;
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(r.action == DDA_OP && r.op.type == IOP_MOVE && r.op.a == 9 && r.op.b == 20 && r.op.count == 0, "release on a slot: move");
    // Under the threshold no drag starts.
    in = frame(10, 10); in.pressed = true; in.hover = s9;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(13, 12); in.hover = s9;
    dd_update(&d, &in, &inv, &g_db);
    CHECK(d.state == DD_PRESSED, "a 3 px wobble is still a click");
    in = frame(13, 12); in.released = true; in.hover = s9; in.shift = true;
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(r.action == DDA_OP && r.op.type == IOP_QUICK_MOVE && r.op.a == 9, "shift+click: quick move");
    // Ctrl-drag: split half the stack into an empty slot.
    in = frame(10, 10); in.pressed = true; in.ctrl = true; in.hover = s10;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(40, 10); in.hover = s10;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(90, 10); in.released = true; in.hover = s20;
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(r.action == DDA_OP && r.op.type == IOP_SPLIT && r.op.a == 10 && r.op.b == 20 && r.op.count == 5, "ctrl-drag: split half");
    // Attachment onto a weapon: mount; onto the knife: reject with a reason.
    in = frame(10, 10); in.pressed = true; in.hover = s11;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(40, 10); in.hover = s11;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(90, 10); in.released = true; in.hover = s0;
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(r.action == DDA_OP && r.op.type == IOP_MOUNT && r.op.a == 0 && r.op.b == 11, "attachment onto the rifle: mount");
    in = frame(10, 10); in.pressed = true; in.hover = s11;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(40, 10); in.hover = s11;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(90, 10); in.released = true; in.hover = s1;
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(r.action == DDA_REJECT && r.reject_reason == IOR_NOT_WEAPON && r.slot == 1, "attachment onto a melee weapon: rejected with a reason");
    // Attachment onto the preview's slot of another type: rejected.
    DragDrop d2;
    dd_reset(&d2);
    d2.state = DD_DRAGGING;
    d2.src = s11;
    DDTarget muzzle = { DT_ATTACH, ATT_MUZZLE }, optic = { DT_ATTACH, ATT_OPTIC };
    r = dd_drop_on(&d2, muzzle, &inv, &g_db, 0);
    CHECK(r.action == DDA_REJECT && r.reject_reason == IOR_NO_SLOT && r.reject_target.kind == DT_ATTACH, "red dot onto the muzzle slot: rejected");
    r = dd_drop_on(&d2, optic, &inv, &g_db, 0);
    CHECK(r.action == DDA_OP && r.op.type == IOP_MOUNT, "red dot onto the optic slot: mount");
    DDTarget pv = { DT_PREVIEW, 0 };
    r = dd_drop_on(&d2, pv, &inv, &g_db, 0);
    CHECK(r.action == DDA_OP && r.op.type == IOP_MOUNT, "red dot onto the preview: mount into its slot");
    // Outside the panel: stackables drop, non-stackables ask first.
    DDTarget out = { DT_OUTSIDE, -1 };
    d2.src = s9;
    r = dd_drop_on(&d2, out, &inv, &g_db, 0);
    CHECK(r.action == DDA_OP && r.op.type == IOP_DROP, "stack dragged outside: drop");
    d2.src = s0;
    r = dd_drop_on(&d2, out, &inv, &g_db, -1);
    CHECK(r.action == DDA_CONFIRM_DROP && r.op.type == IOP_DROP && r.op.a == 0, "weapon dragged outside: confirm first");
    // Mounted attachment dragged out of the preview: unmount.
    inv.slots[0].att[ATT_OPTIC] = attach_find(&g_db, "red_dot");
    d2.src = optic;
    r = dd_drop_on(&d2, s20, &inv, &g_db, 0);
    CHECK(r.action == DDA_OP && r.op.type == IOP_UNMOUNT && r.op.a == 0 && r.op.b == ATT_OPTIC && r.op.c == 20, "optic out of the preview onto a slot: unmount there");
    r = dd_drop_on(&d2, s9, &inv, &g_db, 0);
    CHECK(r.action == DDA_OP && r.op.type == IOP_UNMOUNT && r.op.c == 255, "onto an occupied slot: unmount into the first free one");
    // Esc / B cancels a drag; right click opens the context menu; double click equips.
    in = frame(10, 10); in.pressed = true; in.hover = s9;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(40, 10); in.hover = s9;
    dd_update(&d, &in, &inv, &g_db);
    in = frame(40, 10); in.cancel = true;
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(r.action == DDA_CANCEL && d.state == DD_IDLE, "back cancels a drag");
    in = frame(10, 10); in.right_pressed = true; in.hover = s9;
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(r.action == DDA_CONTEXT && r.slot == 9, "right click: context menu");
    in = frame(10, 10); in.double_clicked = true; in.pressed = true; in.hover = s20;
    inv.slots[20] = inv.slots[1];
    r = dd_update(&d, &in, &inv, &g_db);
    CHECK(r.action == DDA_OP && r.op.type == IOP_EQUIP && r.op.a == 20, "double click: equip");
    // Gamepad / keyboard carry: pick up with A, place with A.
    dd_reset(&d);
    r = dd_carry_accept(&d, s9, &inv, &g_db, 0, false);
    CHECK(r.action == DDA_PICKED && d.carry, "A on an item: carry");
    r = dd_carry_accept(&d, s20, &inv, &g_db, 0, false);
    CHECK(r.action == DDA_OP && r.op.type == IOP_MOVE && r.op.a == 9 && r.op.b == 20 && !d.carry, "A on the target: place");
    r = dd_carry_accept(&d, (DDTarget){ DT_SLOT, 30 }, &inv, &g_db, 0, false);
    CHECK(r.action == DDA_NONE && !d.carry, "A on an empty slot carries nothing");
    // Dropping on itself / on the panel cancels.
    d2.src = s9;
    r = dd_drop_on(&d2, s9, &inv, &g_db, 0);
    CHECK(r.action == DDA_CANCEL, "drop on itself: cancel");
    r = dd_drop_on(&d2, (DDTarget){ DT_PANEL, -1 }, &inv, &g_db, 0);
    CHECK(r.action == DDA_CANCEL, "drop on the panel background: cancel");
    // Crate: put / take.
    r = dd_drop_on(&d2, (DDTarget){ DT_CONTAINER, 3 }, &inv, &g_db, 0);
    CHECK(r.action == DDA_CONTAINER_PUT && r.slot == 9, "into a crate slot: put");
    d2.src = (DDTarget){ DT_CONTAINER, 4 };
    r = dd_drop_on(&d2, s20, &inv, &g_db, 0);
    CHECK(r.action == DDA_CONTAINER_TAKE && r.slot == 4, "crate item into the inventory: take");
}

// ------------------------------------------------------------------ prediction / reconcile

// The client applies its intents at once; the host applies them in order (possibly with its own
// changes in between) and acknowledges a prefix. Rebuilding host state + unacknowledged intents
// must equal what the host ends with once everything is acknowledged.
static void test_reconcile(void) {
    Inventory host;
    memset(&host, 0, sizeof(host));
    put(&host, 0, "rifle", 1);
    memset(host.slots[0].att, 0, ATT_SLOTS);
    put(&host, 1, "pistol", 1);
    put(&host, 9, "ammo_rifle", 70);
    put(&host, 10, "scrap", 13);
    host.slots[11].item = item_for_attachment(&g_db, attach_find(&g_db, "red_dot"));
    host.slots[11].count = 1;
    InvOp ops[] = { mk(IOP_MOUNT, 0, 11, 0, 0), mk(IOP_SPLIT, 10, 255, 0, 0), mk(IOP_MOVE, 9, 20, 0, 30), mk(IOP_QUICK_MOVE, 1, 0, 0, 0), mk(IOP_DROP, 10, 0, 0, 2),
                    mk(IOP_UNMOUNT, 0, ATT_OPTIC, 255, 0), mk(IOP_MOVE, 20, 9, 0, 0) };
    int n = (int)ARRAY_COUNT(ops);
    Inventory client = host;
    for (int i = 0; i < n; i++) { ops[i].seq = (u32)(i + 1); inv_op_apply(&client, &g_db, &ops[i], NULL); }
    int bad = 0;
    for (int acked = 0; acked <= n; acked++) {
        // Host has applied the first `acked` intents (and picked up 10 rounds meanwhile).
        Inventory h = host;
        for (int i = 0; i < acked; i++) inv_op_apply(&h, &g_db, &ops[i], NULL);
        // Client reconcile: host state + the rest.
        Inventory c = h;
        for (int i = acked; i < n; i++) inv_op_apply(&c, &g_db, &ops[i], NULL);
        if (memcmp(c.slots, client.slots, sizeof(c.slots))) bad++;
    }
    CHECK(bad == 0, "reconcile: host prefix + unacknowledged intents == the prediction, for every acknowledgement point (%d bad)", bad);
    // A host-side change in between (a pickup): the rebuilt state keeps it and the intents still apply.
    Inventory h = host;
    inv_op_apply(&h, &g_db, &ops[0], NULL);
    inv_add(&h, &g_db, item("ammo_rifle"), 10, 0);
    Inventory c = h;
    for (int i = 1; i < n; i++) inv_op_apply(&c, &g_db, &ops[i], NULL);
    CHECK(inv_count(&c, item("ammo_rifle")) == 80, "the host's pickup survives the reconcile (%d rounds)", inv_count(&c, item("ammo_rifle")));
    // Wire encoding round trip.
    InvOp o = mk(IOP_UNMOUNT, 7, 3, 255, 1234), o2;
    o.seq = 987654;
    int a, b, cc;
    inv_op_pack(&o, &a, &b, &cc);
    inv_op_unpack(a, b, cc, &o2);
    CHECK(!memcmp(&o, &o2, sizeof(o)), "intent pack / unpack");
}

// ------------------------------------------------------------------ icons

static void test_icons(void) {
    int missing = 0;
    for (int i = 1; i < g_db.item_count; i++) {
        const ItemDef *d = &g_db.items[i];
        for (int s = 0; s < 2; s++) {
            char path[256];
            if (d->icon[0]) snprintf(path, sizeof(path), "%s", d->icon);
            else snprintf(path, sizeof(path), "assets/ui/icons/%d/%s.png", s ? 256 : 128, d->id);
            if (!platform_file_exists(path)) { missing++; printf("  no icon: %s\n", path); }
        }
    }
    CHECK(missing == 0, "every item has a 128 and a 256 px icon (%d missing; tools/bake_icons/bake_icons.sh)", missing);
}

int main(void) {
    printf("UI tests\n");
    test_layout_basics();
    test_layout_pause();
    CHECK(items_load(&g_db, "data/items.json", "data/weapons"), "items load");
    test_merge();
    test_attachments();
    test_dragdrop();
    test_reconcile();
    test_icons();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
