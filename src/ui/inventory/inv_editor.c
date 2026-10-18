// Inventory editor panel and commands (docs/INVENTORY.md "Commands"): give any item, fill a
// container, run intents by hand, and the fit map (every cell where the chosen item fits, in
// every open grid). Host-side changes go straight into the trees (dev); intents go through the
// normal path (prediction + host validation).
#include "inv_screen.h"
#include "ui/screens/screens.h"
#include "core/cvar.h"
#include "core/log.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "gameplay/entities.h"
#include "gameplay/attachments.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TH (gui_theme())

static struct {
    bool open;
    char filter[32];
    char line[96];
    u16 fit;                  // fit map item
    int target;               // fill target: 0 pockets, 1 rig, 2 backpack, 3 secure, 4 loot
    GuiScroll scroll;
    vec2 pos;
    bool dragging;
    vec2 grab;
} E;

bool inv_editor_open(void) { return E.open; }
u16 inv_editor_fit_item(void) { return E.open ? E.fit : 0; }

static Game *G_(void) { return game(); }
static const ItemDB *items(void) { Game *g = G_(); return g && g->world ? g->world->items : NULL; }

static bool host_player(Player **p, World **w) {
    Game *g = G_();
    *w = g ? g->world : NULL;
    *p = game_local_player();
    if (!*w || !*p || g->mode != GM_HOST) { log_warn("inventory: host only"); return false; }
    return true;
}

// The tree and parent behind a fill target name (pockets, rig, backpack, secure, loot, or a uid).
static bool resolve_target(const char *name, InvTree **t, u16 *parent, int *side) {
    Player *p = game_local_player();
    if (!p) return false;
    *side = INV_SIDE_SELF;
    *t = &p->inv.t;
    if (!strcmp(name, "loot")) {
        *t = game_loot_tree();
        *parent = 0;
        *side = INV_SIDE_LOOT;
        return *t != NULL;
    }
    InvQuickTarget q = !strcmp(name, "pockets") ? IQ_POCKETS : !strcmp(name, "rig") ? IQ_RIG : !strcmp(name, "backpack") ? IQ_BACKPACK
                     : !strcmp(name, "secure") ? IQ_SECURE : IQ_COUNT;
    if (q != IQ_COUNT) return inv_quick_parent(*t, q, parent);
    u16 uid = (u16)atoi(name);
    const InvItem *it = inv_get_c(*t, uid);
    if (!it || !item_container(items(), it->s.item)) return false;
    *parent = uid;
    return true;
}

// Fills free cells of one container with item (up to count stacks; 0 = until full). Host.
static int fill(const char *target, u16 item, int count) {
    Player *p;
    World *w;
    if (!host_player(&p, &w)) return 0;
    InvTree *t;
    u16 parent;
    int side;
    if (!resolve_target(target, &t, &parent, &side)) { log_warn("inv_fill: no container '%s' (pockets, rig, backpack, secure, loot, or a uid)", target); return 0; }
    const ItemDef *d = item_def(w->items, item);
    const WeaponDef *wd = item_weapon(w->items, item);
    int n = 0;
    for (; !count || n < count; n++) {
        InvItem probe;
        memset(&probe, 0, sizeof(probe));
        probe.s.item = item;
        probe.s.count = (u16)DG_MAX(1, d->max_stack);
        probe.s.ammo = wd ? (i16)wd->mag_size : 0;
        attach_defaults(wd, probe.s.att);
        InvPlace pl;
        if (!inv_find_space(t, w->items, &probe, parent, &pl)) break;
        InvItem *it = inv_alloc(t, w->items, &probe.s);
        if (!it) break;
        it->parent = pl.parent;
        it->loc = pl.loc;
        it->x = pl.x;
        it->y = pl.y;
        it->flags = pl.rot ? INVF_ROT : 0;
    }
    if (side == INV_SIDE_SELF) p->inv.revision++;
    else if (ent_valid(w, p->open_container)) w->cont[ENT_INDEX(p->open_container)].revision++;
    return n;
}

static void cmd_inv_give(int argc, const char **argv) {
    Player *p;
    World *w;
    if (argc < 2) { log_info("usage: inv_give <item> [count]  (the pickup rule: stacks, free equipment slot, containers in inv_quick_order)"); return; }
    if (!host_player(&p, &w)) return;
    u16 item = item_find(w->items, argv[1]);
    if (!item) { log_warn("inv_give: unknown item '%s'", argv[1]); return; }
    const ItemDef *d = item_def(w->items, item);
    int count = argc > 2 ? atoi(argv[2]) : DG_MAX(1, d->max_stack);
    const WeaponDef *wd = item_weapon(w->items, item);
    int left = inv_add(&p->inv, w->items, item, count, wd ? (i16)wd->mag_size : 0);
    InvItem *it = inv_find(&p->inv, item);
    if (it) log_info("inv_give: %d x %s -> parent %d loc %d (%d,%d)%s", count - left, d->id, it->parent, it->loc, it->x, it->y, left ? " (some didn't fit)" : "");
    else log_info("inv_give: no room for %s", d->id);
}

static void cmd_inv_fill(int argc, const char **argv) {
    if (argc < 3) { log_info("usage: inv_fill <pockets|rig|backpack|secure|loot|uid> <item> [stacks]"); return; }
    const ItemDB *db = items();
    u16 item = db ? item_find(db, argv[2]) : 0;
    if (!item) { log_warn("inv_fill: unknown item '%s'", argv[2]); return; }
    int n = fill(argv[1], item, argc > 3 ? atoi(argv[3]) : 0);
    log_info("inv_fill: %d x %s into %s", n, argv[2], argv[1]);
}

static void dump_tree(const InvTree *t, const ItemDB *db, const char *what) {
    log_info("%s: root %s, %d items, %.2f kg, next uid %d", what, inv_root_def(t, db)->id, inv_live_count(t), inv_tree_weight(t, db), t->next_uid);
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *it = &t->items[i];
        if (!inv_alive(it)) continue;
        int w, h;
        inv_item_size(db, it, &w, &h);
        char loc[32];
        if (it->loc >= INV_LOC_EQUIP && it->loc != INV_LOC_NONE) snprintf(loc, sizeof(loc), "slot %s", equip_slot_name(it->loc - INV_LOC_EQUIP));
        else snprintf(loc, sizeof(loc), "c%d (%d,%d)", it->loc, it->x, it->y);
        log_info("  uid %3d %-18s x%-3d in %3d %-16s %dx%d%s%s ammo %d", it->uid, item_def(db, it->s.item)->id, it->s.count, it->parent, loc, w, h,
                 (it->flags & INVF_ROT) ? " rot" : "", (it->flags & INVF_FOLDED) ? " folded" : "", it->s.ammo);
    }
}

static void cmd_inv_dump(int argc, const char **argv) {
    Player *p = game_local_player();
    const ItemDB *db = items();
    if (!p || !db) return;
    if (argc > 1 && !strcmp(argv[1], "loot")) {
        InvTree *t = game_loot_tree();
        // Host with a container name / id after "loot": that container's tree.
        if (argc > 2 && G_()->mode == GM_HOST) {
            World *w = G_()->world;
            EntityId e = ent_find_by_name(w, argv[2]);
            if (!ent_valid(w, e)) e = (EntityId)strtoul(argv[2], NULL, 0);
            t = container_tree(w, e, false);
        }
        if (t) dump_tree(t, db, "loot");
        else log_info("inv_dump: no container open");
        return;
    }
    // inv_dump <player slot> (host): anyone's inventory (the two-client loot test sums them).
    if (argc > 1 && G_()->mode == GM_HOST && argv[1][0] >= '0' && argv[1][0] <= '9') {
        int slot = atoi(argv[1]);
        World *w = G_()->world;
        if (slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) { log_warn("inv_dump: no player in slot %d", slot); return; }
        p = &w->players[slot];
    }
    dump_tree(&p->inv.t, db, "inventory");
    char hb[96] = "";
    for (int k = 0; k < INV_HOTBAR; k++) {
        char e[16];
        snprintf(e, sizeof(e), " %d:%d", k + 1, p->inv.quick[k]);
        strncat(hb, e, sizeof(hb) - strlen(hb) - 1);
    }
    log_info("  hotbar%s  (pending %d)", hb, game_inv_pending());
}

// inv_intent <name> <self|loot> <src uid> [<self|loot> <dst uid|auto> [loc x y rot [count [aux]]]]
static bool parse_intent(int argc, const char **argv, InvIntent *in) {
    memset(in, 0, sizeof(*in));
    if (argc < 2) return false;
    in->type = (u8)inv_intent_from_name(argv[1]);
    if (!in->type) return false;
    in->src_side = in->dst_side = argc > 2 && !strcmp(argv[2], "loot") ? INV_SIDE_LOOT : INV_SIDE_SELF;
    if (argc > 3) in->src = (u16)atoi(argv[3]);
    if (argc > 4) in->dst_side = !strcmp(argv[4], "loot") ? INV_SIDE_LOOT : INV_SIDE_SELF;
    if (argc > 5) in->dst = !strcmp(argv[5], "auto") ? INV_AUTO : (u16)atoi(argv[5]);
    if (argc > 6) {
        const char *l = argv[6];
        int slot = equip_slot_from_name(l);
        in->loc = slot != EQ_NONE ? (u8)(INV_LOC_EQUIP + slot) : (u8)atoi(l);
    }
    if (argc > 7) in->x = (u8)atoi(argv[7]);
    if (argc > 8) in->y = (u8)atoi(argv[8]);
    if (argc > 9) in->rot = (u8)atoi(argv[9]);
    if (argc > 10) in->count = (u16)atoi(argv[10]);
    if (argc > 11) in->aux = (u8)atoi(argv[11]);
    if (in->type == II_EQUIP && argc <= 11) in->aux = 255;
    return true;
}

static void cmd_inv_intent(int argc, const char **argv) {
    InvIntent in;
    if (!parse_intent(argc, argv, &in)) {
        log_info("usage: inv_intent <move|insert|rotate|equip|quick_move|split|discard|use|mount|unmount|fold|unload|bind|take_all|sort> <self|loot> <src uid> "
                 "[<self|loot> <dst uid|auto> [loc|slot x y rot [count [aux]]]]");
        return;
    }
    int r = game_inv_intent(&in);
    log_info("inv_intent %s: %s", inv_intent_name(in.type), r == IIR_OK ? "ok" : inv_result_text(r));
}

static void cmd_inv_open(int argc, const char **argv) {
    Game *g = G_();
    Player *p;
    World *w;
    if (!host_player(&p, &w)) return;
    EntityId e = argc > 1 ? (EntityId)strtoul(argv[1], NULL, 0) : g->focus;
    if (argc > 1 && !ent_valid(w, e)) e = ent_find_by_name(w, argv[1]);
    if (!ent_valid(w, e)) { log_warn("inv_open: no entity (look at a container, or give an entity id / name)"); return; }
    int slot = argc > 2 ? atoi(argv[2]) : g->local_slot;   // another player's (tests: tools/tests/inv_net.sh)
    if (slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) { log_warn("inv_open: no player in slot %d", slot); return; }
    container_send(w, e, slot);
}

static void cmd_inv_editor(int argc, const char **argv) {
    E.open = argc > 1 ? atoi(argv[1]) != 0 : !E.open;
    if (E.open) log_info("inv_editor: open the inventory (Tab) to see the panel");
}

void inv_editor_init(void) {
    memset(&E, 0, sizeof(E));
    E.target = 2;
    cmd_register("inv_give", cmd_inv_give, "inv_give <item> [count]: add through the pickup rule (host)");
    cmd_register("inv_fill", cmd_inv_fill, "inv_fill <pockets|rig|backpack|secure|loot|uid> <item> [stacks]: fill a container's free cells (host)");
    cmd_register("inv_dump", cmd_inv_dump, "inv_dump [loot|player slot]: log the inventory tree (the open container's; host: another player's)");
    cmd_register("inv_intent", cmd_inv_intent, "inv_intent <type> <self|loot> <src> [...]: run an inventory intent (docs/INVENTORY.md)");
    cmd_register("inv_open", cmd_inv_open, "inv_open [entity id|name] [player slot]: open a world container / corpse / trunk (host; default: the focused entity, for you)");
    cmd_register("inv_editor", cmd_inv_editor, "inv_editor [0|1]: the inventory editor panel (item set, fill, intents, fit map)");
}

void inv_editor_draw(void) {
    if (!E.open) return;
    const ItemDB *db = items();
    Game *g = G_();
    if (!db || !g) return;
    float w = gui_s(380), h = gui_s(520);
    if (E.pos.x == 0 && E.pos.y == 0) E.pos = v2(gui_w() * 0.5f - w * 0.5f, gui_s(70));
    GuiRect r = { E.pos.x, E.pos.y, w, h };
    int layer = gd_layer();
    gd_set_layer(GL_OVERLAY);
    gw_panel(r, true);
    gui_block_rect(r);
    GuiRect bar = { r.x, r.y, r.w, gui_s(28) };
    GuiInteract bi = gui_interact(gui_id("inv_ed_bar"), bar, 0);
    if (bi.pressed) { E.dragging = true; E.grab = v2(gui_mouse().x - r.x, gui_mouse().y - r.y); }
    if (E.dragging) { if (!gui_mouse_down(0)) E.dragging = false; else E.pos = v2(gui_mouse().x - E.grab.x, gui_mouse().y - E.grab.y); }
    gw_label_caps(gui_rect(r.x + gui_s(12), r.y + gui_s(6), r.w, gui_s(18)), "Inventory editor", gui_s(TH->t_label), TH->text, GUI_LEFT);
    GuiCursor c = gc_make(gui_rect(r.x + gui_s(12), r.y + gui_s(32), r.w - gui_s(24), h - gui_s(40)), gui_s(4));
    gw_textfield(gui_id("inv_ed_filter"), gc_row(&c, gui_s(28)), E.filter, sizeof(E.filter), "Filter items");
    static const char *targets[5] = { "Pockets", "Rig", "Backpack", "Secure", "Loot" };
    GuiRect tr = gc_row(&c, gui_s(28));
    GuiRect a, b;
    gui_split_h(tr, 0.5f, gui_s(6), &a, &b);
    gw_dropdown(gui_id("inv_ed_target"), a, targets, 5, &E.target);
    bool fill_btn = gw_button(gui_id("inv_ed_fill"), b, E.fit ? "Fill with the fit item" : "Fill (pick a fit item)", GB_SECONDARY, E.fit != 0);
    if (fill_btn && E.fit) {
        static const char *names[5] = { "pockets", "rig", "backpack", "secure", "loot" };
        int n = fill(names[E.target], E.fit, 0);
        log_info("inv_editor: %d x %s into %s", n, item_def(db, E.fit)->id, names[E.target]);
    }
    if (gw_textfield(gui_id("inv_ed_line"), gc_row(&c, gui_s(28)), E.line, sizeof(E.line), "Intent: move self 3 self 7 0 1 2 0") && E.line[0]) {
        char buf[96];
        dg_strcpy(buf, sizeof(buf), E.line);
        const char *argv[16];
        int argc = 1;
        argv[0] = "inv_intent";
        for (char *tok = strtok(buf, " "); tok && argc < 16; tok = strtok(NULL, " ")) argv[argc++] = tok;
        cmd_inv_intent(argc, argv);
    }
    GuiText hint = { gui_s(TH->t_tiny + 1), GW_REGULAR, TH->text_faint, GUI_LEFT, 0, false, false, 0 };
    GuiRect hr = gc_row(&c, gui_s(16));
    gd_text(&hint, hr.x, hr.y, "Click: give one   Right click: fit map (dots where it fits)");
    GuiRect list = { c.area.x, c.y, c.area.w, r.y + r.h - c.y - gui_s(8) };
    gw_scroll_begin(&E.scroll, gui_id("inv_ed_scroll"), list);
    float y = gw_scroll_y(&E.scroll), rh = gui_s(24);
    int n = 0;
    for (int i = 1; i < db->item_count; i++) {
        const ItemDef *d = &db->items[i];
        if (E.filter[0] && !strstr(d->id, E.filter) && !strstr(d->name, E.filter)) continue;
        GuiRect ir = { list.x, y + (float)n * rh, list.w - gui_s(10), rh - 2 };
        n++;
        if (ir.y + ir.h < list.y || ir.y > list.y + list.h) continue;
        GuiInteract in = gui_interact(gui_idi("inv_ed_item", i), ir, 0);
        if (in.hot || E.fit == i) gd_round(ir, gui_s(TH->radius), E.fit == i ? gui_rgba_mul(TH->accent, 0.3f) : TH->hover);
        char lab[96];
        snprintf(lab, sizeof(lab), "%s  %dx%d  %.2f kg%s", d->name, d->grid_w, d->grid_h, d->inv_weight, d->container >= 0 ? "  [container]" : "");
        gw_label(gui_rect(ir.x + gui_s(6), ir.y, ir.w, ir.h), lab, gui_s(TH->t_small), GW_REGULAR, in.hot ? TH->text : TH->text_dim, GUI_LEFT);
        if (in.clicked) {
            const char *argv[3] = { "inv_give", d->id, "1" };
            cmd_inv_give(3, argv);
        }
        if (in.right_clicked) E.fit = E.fit == i ? 0 : (u16)i;
    }
    gw_scroll_end(&E.scroll, (float)n * rh);
    gd_set_layer(layer);
}
