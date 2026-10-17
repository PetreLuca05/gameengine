// Inventory screen (docs/UI.md "Inventory"): a centred modal over the blurred game.
//   left:  backpack grid (6 x 4) and the hotbar (1-6, grid-sized slots), the same slot widget as the HUD;
//   right: the live 3D preview of the selected item (auto-rotates, drag to rotate, wheel to zoom),
//          name / type / description / stats with attachment deltas, attachment slots (drag an
//          attachment onto a slot or onto the weapon to mount it, drag it out to unmount);
//          with a crate open, the crate replaces the preview.
// Mouse: drag and drop (ui/inv_dragdrop.c), right click = context menu, Shift+click = quick move,
// double click = equip, dropping outside the panel = drop into the world. Gamepad / keyboard:
// the focus is the cursor, A / Enter picks up and places, X = menu, Y = quick move, B = cancel.
// Every change is an intent (gameplay/inv_ops.h) predicted locally and confirmed by the host.
#include "screens.h"
#include "ui/inv_dragdrop.h"
#include "ui/icons.h"
#include "ui/item_view.h"
#include "core/engine.h"
#include "core/log.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "gameplay/attachments.h"
#include "gameplay/weapons.h"
#include "gameplay/revive/revive.h"
#include "player/camera.h"
#include "core/cvar.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TH (gui_theme())
#define PREVIEW_W 1024
#define PREVIEW_H 720

typedef struct Ctx { bool open; int slot; vec2 pos; double t; } Ctx;

static struct {
    DragDrop dd;
    int selected;               // previewed inventory slot (-1: the equipped one)
    Ctx ctx;
    InvOp pending_drop;
    float shake[INV_SLOTS + CONTAINER_SLOTS + ATT_SLOTS];
    char toast[96];
    double toast_t;
    TextureId preview;
    ItemViewCam cam;
    u16 cam_item;
    double last_touch;
    bool rotating;
    vec2 rot_from;
    int hover_aslot;
    // this frame's hit rects
    GuiRect slot_r[INV_SLOTS], cont_r[CONTAINER_SLOTS], att_r[ATT_SLOTS], panel, view;
    bool att_vis[ATT_SLOTS];
    vec2 att_socket[ATT_SLOTS];
    bool att_socket_vis[ATT_SLOTS];
} V;

static World *world(void) { Game *g = game(); return g ? g->world : NULL; }
static const ItemDB *items(void) { World *w = world(); return w ? w->items : NULL; }

// ------------------------------------------------------------------ scripted pointer (tests)
// ui_inv_* commands queue pointer steps, one per frame, injected as SDL events before the
// inventory reads its input (tools/tests/ui_inventory.cfg).

typedef struct PStep { int kind; int target; int sub; float dx, dy; bool shift, ctrl; } PStep;   // kind: 0 move, 1 down, 2 up, 3 rdown, 4 rup, 5 mods
enum { TGT_SLOT, TGT_ATT, TGT_OUT, TGT_CONT };
static PStep g_steps[128];
static int g_step_head, g_step_tail;

static void push_step(int kind, int target, int sub, float dx, float dy, bool shift, bool ctrl) {
    if (g_step_tail >= 128) return;
    PStep s = { kind, target, sub, dx, dy, shift, ctrl };
    g_steps[g_step_tail++] = s;
}

static vec2 step_pos(const PStep *s) {
    GuiRect r;
    switch (s->target) {
    case TGT_ATT: r = V.att_r[DG_CLAMP(s->sub, 0, ATT_SLOTS - 1)]; break;
    case TGT_CONT: r = V.cont_r[DG_CLAMP(s->sub, 0, CONTAINER_SLOTS - 1)]; break;
    case TGT_OUT: r = gui_rect(V.panel.x - gui_s(60), V.panel.y + V.panel.h * 0.5f, 1, 1); break;
    default: r = V.slot_r[DG_CLAMP(s->sub, 0, INV_SLOTS - 1)]; break;
    }
    return v2(r.x + r.w * 0.5f + s->dx, r.y + r.h * 0.5f + s->dy);
}

static void run_step(void) {
    if (g_step_head >= g_step_tail) { g_step_head = g_step_tail = 0; return; }
    PStep *s = &g_steps[g_step_head++];
    float pd = g_engine.window ? SDL_GetWindowPixelDensity(g_engine.window) : 1.0f;
    if (pd <= 0) pd = 1;
    vec2 p = step_pos(s);
    gui_set_mods(s->shift, s->ctrl);
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    if (s->kind == 0) {
        ev.type = SDL_EVENT_MOUSE_MOTION;
        ev.motion.x = p.x / pd;
        ev.motion.y = p.y / pd;
        ev.motion.xrel = 5;
    } else if (s->kind >= 1 && s->kind <= 4) {
        ev.type = (s->kind == 1 || s->kind == 3) ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        ev.button.button = s->kind >= 3 ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        ev.button.x = p.x / pd;
        ev.button.y = p.y / pd;
        ev.button.clicks = 1;
    } else {
        return;
    }
    gui_handle_event(&ev);
}

static void queue_drag(int t0, int a, int t1, int b, bool ctrl) {
    push_step(0, t0, a, 0, 0, false, ctrl);
    push_step(1, t0, a, 0, 0, false, ctrl);
    push_step(0, t0, a, 14, 10, false, ctrl);
    push_step(0, t1, b, -6, 4, false, ctrl);
    push_step(0, t1, b, 0, 0, false, ctrl);
    push_step(2, t1, b, 0, 0, false, false);
    push_step(5, t1, b, 0, 0, false, false);
}

static void cmd_drag(int argc, const char **argv) {
    if (argc < 3) return;
    queue_drag(TGT_SLOT, atoi(argv[1]), TGT_SLOT, atoi(argv[2]), argc > 3 && !strcmp(argv[3], "ctrl"));
}
static void cmd_drag_att(int argc, const char **argv) { if (argc >= 3) queue_drag(TGT_ATT, atoi(argv[1]), TGT_SLOT, atoi(argv[2]), false); }
static void cmd_drag_to_att(int argc, const char **argv) { if (argc >= 3) queue_drag(TGT_SLOT, atoi(argv[1]), TGT_ATT, atoi(argv[2]), false); }
static void cmd_drag_crate(int argc, const char **argv) { if (argc >= 3) queue_drag(TGT_CONT, atoi(argv[1]), TGT_SLOT, atoi(argv[2]), false); }
static void cmd_drag_out(int argc, const char **argv) { if (argc >= 2) queue_drag(TGT_SLOT, atoi(argv[1]), TGT_OUT, 0, false); }
static void cmd_rclick(int argc, const char **argv) {
    if (argc < 2) return;
    push_step(0, TGT_SLOT, atoi(argv[1]), 0, 0, false, false);
    push_step(3, TGT_SLOT, atoi(argv[1]), 0, 0, false, false);
    push_step(4, TGT_SLOT, atoi(argv[1]), 0, 0, false, false);
}
static void click(int slot, bool shift) {
    push_step(0, TGT_SLOT, slot, 0, 0, shift, false);
    push_step(1, TGT_SLOT, slot, 0, 0, shift, false);
    push_step(2, TGT_SLOT, slot, 0, 0, shift, false);
    push_step(5, TGT_SLOT, slot, 0, 0, false, false);
}
static void cmd_shiftclick(int argc, const char **argv) { if (argc >= 2) click(atoi(argv[1]), true); }
static void cmd_select(int argc, const char **argv) { if (argc >= 2) click(atoi(argv[1]), false); }

static void cmd_dump(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    Player *p = game_local_player();
    const ItemDB *db = items();
    if (!p || !db) return;
    char line[1024] = "";
    for (int i = 0; i < INV_SLOTS; i++) {
        const InvSlot *s = &p->inv.slots[i];
        if (!s->item) continue;
        char e[96];
        int n = snprintf(e, sizeof(e), " %d:%s", i, item_def(db, s->item)->id);
        if (s->count > 1) n += snprintf(e + n, sizeof(e) - (size_t)n, "x%d", s->count);
        for (int k = 0; k < ATT_SLOTS; k++)
            if (s->att[k]) { const AttachmentDef *a = attach_def(db, s->att[k]); n += snprintf(e + n, sizeof(e) - (size_t)n, "+%s", a ? a->id : "?"); }
        strncat(line, e, sizeof(line) - strlen(line) - 1);
    }
    log_info("inv:%s  (pending %d, selected %d)", line, game_inv_pending(), V.selected);
}

void inventory_register_commands(void) {
    cmd_register("ui_inv_drag", cmd_drag, "ui_inv_drag <from> <to> [ctrl]: scripted drag between inventory slots (tests)");
    cmd_register("ui_inv_drag_att", cmd_drag_att, "ui_inv_drag_att <attachment slot> <to>: drag a mounted attachment out of the preview");
    cmd_register("ui_inv_drag_to_att", cmd_drag_to_att, "ui_inv_drag_to_att <from> <attachment slot>: drag onto the preview's attachment slot");
    cmd_register("ui_inv_drag_out", cmd_drag_out, "ui_inv_drag_out <from>: drag outside the panel (drop into the world)");
    cmd_register("ui_inv_drag_from_crate", cmd_drag_crate, "ui_inv_drag_from_crate <crate slot> <to>: take from an open crate by dragging");
    cmd_register("ui_inv_rclick", cmd_rclick, "ui_inv_rclick <slot>: context menu");
    cmd_register("ui_inv_shiftclick", cmd_shiftclick, "ui_inv_shiftclick <slot>: quick move");
    cmd_register("ui_inv_select", cmd_select, "ui_inv_select <slot>: click (select for the preview)");
    cmd_register("ui_inv_dump", cmd_dump, "log the local inventory");
}

void inventory_open(void) {
    dd_reset(&V.dd);
    V.ctx.open = false;
    V.selected = -1;
    V.toast[0] = 0;
    gui_anim_restart(gui_id("inv_open"));
}

void inventory_close(void) {
    dd_reset(&V.dd);
    V.ctx.open = false;
}

bool inventory_wants_back(void) { return V.dd.state == DD_IDLE && !V.ctx.open; }

static void toast(const char *msg) {
    dg_strcpy(V.toast, sizeof(V.toast), msg);
    V.toast_t = gui_time();
}

static void shake_target(DDTarget t) {
    if (t.kind == DT_SLOT && t.index >= 0 && t.index < INV_SLOTS) V.shake[t.index] = 1.0f;
    if (t.kind == DT_CONTAINER && t.index >= 0 && t.index < CONTAINER_SLOTS) V.shake[INV_SLOTS + t.index] = 1.0f;
    if (t.kind == DT_ATTACH && t.index >= 0 && t.index < ATT_SLOTS) V.shake[INV_SLOTS + CONTAINER_SLOTS + t.index] = 1.0f;
}

static u32 type_color(const ItemDef *d) {
    switch (d->kind) {
    case ITEM_WEAPON: return TH->type_weapon;
    case ITEM_AMMO: return TH->type_ammo;
    case ITEM_CONSUMABLE: return TH->type_consumable;
    case ITEM_THROWABLE: return TH->type_weapon;
    case ITEM_ATTACHMENT: return TH->type_attachment;
    case ITEM_KEY: return TH->type_key;
    default: return TH->type_misc;
    }
}

// The slot view for an inventory slot (also used by the HUD hotbar through hud.c).
void inventory_slot_view(const InvSlot *s, const ItemDB *db, int mag_override, GuiSlotView *v) {
    memset(v, 0, sizeof(*v));
    v->ammo_frac = -1;
    v->empty = !s->item;
    if (!s->item) return;
    const ItemDef *d = item_def(db, s->item);
    v->tint = type_color(d);
    v->name = revive_item_name(db, s->item, s->ammo);   // DNA samples carry the player's name
    v->count = s->count;
    v->icon = 0;
    icons_get(s->item, &v->icon, &v->uv);
    const WeaponDef *wd = item_weapon(db, s->item);
    if (wd && wd->mag_size > 0 && wd->kind != WPN_MELEE) {
        WeaponDef eff;
        attach_weapon_effective(db, wd, s->att, &eff);
        int ammo = mag_override >= 0 ? mag_override : s->ammo;
        v->ammo = ammo;
        v->ammo_frac = (float)ammo / (float)DG_MAX(1, eff.mag_size);
    }
}

static void send(InvOp op) {
    Game *g0 = game();
    Player *p0 = game_local_player();
    // Equip: a hotbar item is just selected; a backpack item goes to the first empty hotbar slot
    // (or swaps with the selected one) and becomes selected.
    if (op.type == IOP_EQUIP && g0 && p0) {
        if (op.a < INV_HOTBAR) { g0->hotbar_slot = op.a; gui_sound(TH->snd_equip); return; }
        int to = -1;
        for (int i = 0; i < INV_HOTBAR; i++) if (!p0->inv.slots[i].item) { to = i; break; }
        op.b = g0->hotbar_slot;
        if (to < 0) to = op.b;
        int r0 = game_inv_op(&op);
        if (r0 == IOR_OK) { g0->hotbar_slot = (u8)to; V.selected = to; gui_sound(TH->snd_equip); }
        else { toast(inv_op_result_text(r0)); gui_sound(TH->snd_error); }
        return;
    }
    int r = game_inv_op(&op);
    const ItemDB *db = items();
    if (r != IOR_OK && r != IOR_NOOP) {
        toast(inv_op_result_text(r));
        gui_sound(TH->snd_error);
        return;
    }
    if (r == IOR_OK) {
        if (op.type == IOP_DROP) gui_sound(TH->snd_drop);
        else if (op.type == IOP_MOUNT || op.type == IOP_UNMOUNT || op.type == IOP_EQUIP) gui_sound(TH->snd_equip);
        else gui_sound(TH->snd_click);
    }
    // The selection follows the moved item.
    if (db && r == IOR_OK) {
        if (op.type == IOP_MOVE && V.selected == op.a) V.selected = op.b;
        if (op.type == IOP_QUICK_MOVE && V.selected == op.a) V.selected = -1;
    }
}

void inventory_confirm_drop(bool yes) {
    if (yes) send(V.pending_drop);
}

static void apply(const DDResult *r) {
    Game *g = game();
    switch (r->action) {
    case DDA_OP: send(r->op); break;
    case DDA_SELECT: if (r->slot >= 0) { V.selected = r->slot; gui_sound(TH->snd_click); } break;
    case DDA_CONTEXT:
        V.ctx.open = true;
        V.ctx.slot = r->slot;
        V.ctx.pos = gui_mouse();
        V.ctx.t = gui_time();
        V.selected = r->slot;
        gui_set_focus(0);
        break;
    case DDA_CONFIRM_DROP: {
        V.pending_drop = r->op;
        const ItemDB *db = items();
        Player *p = game_local_player();
        char title[96];
        snprintf(title, sizeof(title), "Drop %s?", p && db ? item_def(db, p->inv.slots[r->op.a].item)->name : "item");
        screens_confirm(CONFIRM_DROP, title, "It will be left on the ground in front of you.", 0);
        break;
    }
    case DDA_REJECT:
        shake_target(r->reject_target);
        toast(inv_op_result_text(r->reject_reason));
        gui_sound(TH->snd_error);
        break;
    case DDA_CONTAINER_PUT: if (g) { game_rpc(RPC_CONTAINER_PUT, r->slot, 0, 0); gui_sound(TH->snd_click); } break;
    case DDA_CONTAINER_TAKE: if (g) { game_rpc(RPC_CONTAINER_TAKE, r->slot, 0, 0); gui_sound(TH->snd_click); } break;
    case DDA_PICKED: gui_sound(TH->snd_click); break;
    default: break;
    }
}

// ------------------------------------------------------------------ slots

static GuiId slot_id(int i) { return gui_idi("inv_slot", i); }
static GuiId cont_id(int i) { return gui_idi("inv_cont", i); }
static GuiId att_id(int k) { return gui_idi("inv_att", k); }

static bool carrying_from(DDTarget t) {
    return V.dd.state == DD_DRAGGING && V.dd.src.kind == t.kind && V.dd.src.index == t.index;
}

static void draw_slot(int i, GuiRect r, const Player *p, const ItemDB *db, int equipped, DDTarget hover) {
    const InvSlot *s = &p->inv.slots[i];
    GuiInteract in = gui_interact(slot_id(i), r, GUI_FOCUSABLE);
    GuiSlotView v;
    inventory_slot_view(s, db, i == equipped ? p->sim.wpn.mag : -1, &v);
    v.hotkey = i < INV_HOTBAR ? i + 1 : 0;
    v.selected = i == V.selected || (V.selected < 0 && i == equipped);
    v.hot = in.hot;
    v.focus = in.focused;
    DDTarget me = { DT_SLOT, (i16)i };
    if (carrying_from(me)) v.dim = true;
    if (V.dd.state == DD_DRAGGING && hover.kind == DT_SLOT && hover.index == i && !carrying_from(me)) {
        DDResult pr = dd_drop_on(&V.dd, me, &p->inv, db, -1);
        v.drop_state = pr.action == DDA_REJECT ? 2 : 1;
    }
    V.shake[i] = fmaxf(0.0f, V.shake[i] - gui_dt() / fmaxf(0.05f, TH->shake));
    v.shake = V.shake[i];
    gw_slot(r, &v);
    if (in.hovered && s->item && V.dd.state == DD_IDLE && !V.ctx.open) {
        const ItemDef *d = item_def(db, s->item);
        char tip[200];
        snprintf(tip, sizeof(tip), "%s\n%s", revive_item_name(db, s->item, s->ammo), item_kind_name(d->kind));
        gui_tooltip(slot_id(i), tip);
    }
}

// ------------------------------------------------------------------ preview

static void stat_line(GuiCursor *c, const char *label, const char *value, float delta, bool higher_better) {
    GuiRect r = gc_row(c, gui_s(20));
    gw_label(gui_rect(r.x, r.y, r.w * 0.55f, r.h), label, gui_s(TH->t_small + 1), GW_REGULAR, TH->text_dim, GUI_LEFT);
    char buf[64];
    u32 col = TH->text;
    if (fabsf(delta) >= 0.5f) {
        bool good = higher_better ? delta > 0 : delta < 0;
        col = good ? TH->ok : TH->danger;
        snprintf(buf, sizeof(buf), "%s  %+.0f%%", value, delta);
    } else {
        snprintf(buf, sizeof(buf), "%s", value);
    }
    gw_label(gui_rect(r.x + r.w * 0.45f, r.y, r.w * 0.55f, r.h), buf, gui_s(TH->t_small + 1), GW_MEDIUM, col, GUI_RIGHT);
}

static float pct(float now, float base) { return base != 0.0f ? 100.0f * (now - base) / fabsf(base) : 0.0f; }

static void item_stats(GuiCursor *c, const InvSlot *s, const ItemDB *db) {
    const ItemDef *d = item_def(db, s->item);
    const WeaponDef *wd = item_weapon(db, s->item);
    char v[64];
    if (wd) {
        WeaponDef eff;
        attach_weapon_effective(db, wd, s->att, &eff);
        if (wd->kind == WPN_MELEE) {
            snprintf(v, sizeof(v), "%.0f", eff.damage);
            stat_line(c, "Damage", v, 0, true);
            snprintf(v, sizeof(v), "%.1f / s", eff.fire_rate);
            stat_line(c, "Swing rate", v, 0, true);
            snprintf(v, sizeof(v), "%.1f m", eff.reach);
            stat_line(c, "Reach", v, 0, true);
            return;
        }
        snprintf(v, sizeof(v), "%.0f%s", eff.damage, eff.pellets > 1 ? " x pellets" : "");
        stat_line(c, "Damage", v, pct(eff.damage, wd->damage), true);
        snprintf(v, sizeof(v), "%.0f rpm", eff.fire_rate * 60.0f);
        stat_line(c, "Fire rate", v, pct(eff.fire_rate, wd->fire_rate), true);
        snprintf(v, sizeof(v), "%d", eff.mag_size);
        stat_line(c, "Magazine", v, pct((float)eff.mag_size, (float)wd->mag_size), true);
        snprintf(v, sizeof(v), "%.0f m", eff.range);
        stat_line(c, "Range", v, pct(eff.range, wd->range), true);
        snprintf(v, sizeof(v), "%.2f", eff.recoil.v_per);
        stat_line(c, "Vertical recoil", v, pct(eff.recoil.v_per, wd->recoil.v_per), false);
        snprintf(v, sizeof(v), "%.2f", eff.recoil.h_per);
        stat_line(c, "Horizontal recoil", v, pct(eff.recoil.h_per, wd->recoil.h_per), false);
        snprintf(v, sizeof(v), "%.2f s", eff.recoil.ads_time);
        stat_line(c, "Aim time", v, pct(eff.recoil.ads_time, wd->recoil.ads_time), false);
        snprintf(v, sizeof(v), "%.2f s", eff.reload_time);
        stat_line(c, "Reload", v, pct(eff.reload_time, wd->reload_time), false);
        return;
    }
    if (d->kind == ITEM_CONSUMABLE) {
        if (d->heal > 0) { snprintf(v, sizeof(v), "+%.0f", d->heal); stat_line(c, "Health", v, 0, true); }
        if (d->armor > 0) { snprintf(v, sizeof(v), "+%.0f", d->armor); stat_line(c, "Armour", v, 0, true); }
        snprintf(v, sizeof(v), "%.1f s", d->use_time);
        stat_line(c, "Use time", v, 0, true);
    }
    if (d->kind == ITEM_ATTACHMENT) {
        const AttachmentDef *a = attach_def(db, d->attachment);
        if (!a) return;
        snprintf(v, sizeof(v), "%c%s", attach_slot_name(a->slot)[0] - 32, attach_slot_name(a->slot) + 1);
        stat_line(c, "Slot", v, 0, true);
        if (a->zoom > 1.01f) { snprintf(v, sizeof(v), "%.1fx", a->zoom); stat_line(c, "Magnification", v, 0, true); }
        struct { const char *n; float m; bool hb; } mods[] = {
            { "Vertical recoil", a->recoil_v, false }, { "Horizontal recoil", a->recoil_h, false }, { "Aim time", a->ads_time, false },
            { "Sway", a->sway_scale, false }, { "Damage", a->damage, true }, { "Range", a->range, true }, { "Magazine", a->mag_mult, true },
            { "Reload", a->reload_time, false },
        };
        for (int i = 0; i < (int)ARRAY_COUNT(mods); i++) {
            if (mods[i].m <= 0 || fabsf(mods[i].m - 1.0f) < 0.005f) continue;
            snprintf(v, sizeof(v), "x%.2f", mods[i].m);
            stat_line(c, mods[i].n, v, (mods[i].m - 1.0f) * 100.0f, mods[i].hb);
        }
    }
    if (d->max_stack > 1) {
        snprintf(v, sizeof(v), "%d / %d", s->count, d->max_stack);
        stat_line(c, "Stack", v, 0, true);
    }
}

static void draw_preview(GuiRect r, const Player *p, const ItemDB *db, int sel, DDTarget hover) {
    gd_round(r, gui_s(TH->radius_panel), UI_RGBA(255, 255, 255, 6));
    V.view = gui_rect(r.x, r.y, r.w, r.w * 0.42f);
    for (int k = 0; k < ATT_SLOTS; k++) { V.att_vis[k] = false; V.att_socket_vis[k] = false; }
    if (sel < 0 || !p->inv.slots[sel].item) {
        gw_label(V.view, "Select an item", gui_s(TH->t_body), GW_REGULAR, TH->text_faint, GUI_CENTER);
        return;
    }
    const InvSlot *s = &p->inv.slots[sel];
    const ItemDef *d = item_def(db, s->item);
    const WeaponDef *wd = item_weapon(db, s->item);
    if (V.cam_item != s->item) {
        item_view_default_cam(db, s->item, &V.cam);
        V.cam.zoom = wd ? 1.25f : 0.85f;
        V.cam_item = s->item;
        gui_anim_restart(gui_id("inv_preview_in"));
    }
    // Interaction: drag rotates, wheel zooms; idle: slow auto-rotation.
    GuiId vid = gui_id("inv_view");
    GuiInteract in = gui_interact(vid, V.view, 0);
    if (in.pressed && V.dd.state == DD_IDLE) { V.rotating = true; V.rot_from = gui_mouse(); }
    if (V.rotating) {
        if (!gui_mouse_down(0)) V.rotating = false;
        else {
            vec2 m = gui_mouse();
            V.cam.yaw += (m.x - V.rot_from.x) * 0.01f;
            V.cam.pitch = clampf(V.cam.pitch + (m.y - V.rot_from.y) * 0.008f, -1.3f, 1.3f);
            V.rot_from = m;
            V.last_touch = gui_time();
        }
    }
    if (in.hovered && gui_wheel() != 0) {
        V.cam.zoom = clampf(V.cam.zoom * (gui_wheel() > 0 ? 1.12f : 1.0f / 1.12f), 0.6f, 3.0f);
        gui_wheel_consume();
        V.last_touch = gui_time();
    }
    vec2 stick = gui_pad_stick();
    if (gui_nav_mode() && gui_focus() == gui_id("inv_view_focus")) { V.cam.yaw += stick.x * gui_dt() * 2.0f; V.last_touch = gui_time(); }
    if (gui_time() - V.last_touch > 2.5) V.cam.yaw += gui_dt() * 0.35f;
    // Highlight: the hovered chip's socket, or where a dragged attachment would go.
    int hl = V.hover_aslot;
    if (V.dd.state == DD_DRAGGING && V.dd.src.kind == DT_SLOT && wd) {
        const InvSlot *src = &p->inv.slots[V.dd.src.index];
        int aslot;
        if (src->item && inv_attach_check(db, s, src->item, &aslot) == IOR_OK) hl = aslot;
    }
    static RenderUi3dPart parts[ITEM_VIEW_MAX_PARTS];
    vec3 mn, mx;
    int n = item_view_parts(db, s->item, s->att, hl, parts, ITEM_VIEW_MAX_PARTS, &mn, &mx);
    if (!V.preview) V.preview = render_ui3d_target_create(PREVIEW_W, PREVIEW_H);
    RenderUi3dView rv;
    memset(&rv, 0, sizeof(rv));
    rv.target = V.preview;
    float aspect = V.view.w / fmaxf(1.0f, V.view.h);
    item_view_matrices(&V.cam, mn, mx, aspect, &rv.view, &rv.proj);
    rv.clear = v4(0, 0, 0, 0);
    rv.parts = parts;
    rv.part_count = n;
    render_ui3d(&rv);
    // Floor glow + the view.
    float pin = gui_ease_out(gui_anim_t(gui_id("inv_preview_in")) / 0.25f);
    gd_rect_grad(gui_rect(V.view.x, V.view.y + V.view.h * 0.55f, V.view.w, V.view.h * 0.45f), UI_RGBA(255, 255, 255, 0), UI_RGBA(255, 255, 255, 10));
    gd_alpha_push(pin);
    gd_image(V.view, V.preview, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, 255));
    gd_alpha_pop();
    // Drop target: the weapon itself (mount into the matching slot).
    bool drop_here = V.dd.state == DD_DRAGGING && hover.kind == DT_PREVIEW;
    if (drop_here) gd_round_border(V.view, gui_s(TH->radius_panel), 1, 0, gui_rgba_mul(TH->accent, 0.7f));
    // Socket positions on screen (for the chips' leader lines).
    if (wd)
        for (int k = 0; k < ATT_SLOTS; k++) {
            if (!wd->att_slots[k].present) continue;
            vec2 sp;
            if (item_view_project(&rv.view, &rv.proj, wd->att_slots[k].socket.t, &sp)) {
                V.att_socket[k] = v2(V.view.x + sp.x * V.view.w, V.view.y + sp.y * V.view.h);
                V.att_socket_vis[k] = gui_rect_contains(V.view, V.att_socket[k].x, V.att_socket[k].y);
            }
        }
    GuiCursor c = gc_make(gui_rect(r.x + gui_s(16), V.view.y + V.view.h + gui_s(4), r.w - gui_s(32), 10000), gui_s(2));
    // Attachment slots (weapons).
    V.hover_aslot = -1;
    if (wd && wd->kind != WPN_MELEE) {
        int present = 0;
        for (int k = 0; k < ATT_SLOTS; k++) present += wd->att_slots[k].present;
        float gap = gui_s(6), cw = (c.area.w - gap * (float)(ATT_SLOTS - 1)) / (float)ATT_SLOTS;
        GuiRect row = gc_row(&c, cw + gui_s(18));
        int col = 0;
        for (int k = 0; k < ATT_SLOTS; k++) {
            if (!wd->att_slots[k].present) continue;
            GuiRect chip = { row.x + (cw + gap) * (float)col++, row.y, cw, cw };
            V.att_r[k] = chip;
            V.att_vis[k] = true;
            GuiInteract ci = gui_interact(att_id(k), chip, GUI_FOCUSABLE);
            if (ci.hot) V.hover_aslot = k;
            GuiSlotView v;
            InvSlot fake;
            memset(&fake, 0, sizeof(fake));
            fake.item = item_for_attachment(db, s->att[k]);
            fake.count = fake.item ? 1 : 0;
            inventory_slot_view(&fake, db, -1, &v);
            v.hot = ci.hot;
            v.focus = ci.focused;
            DDTarget me = { DT_ATTACH, (i16)k };
            if (carrying_from(me)) v.dim = true;
            if (V.dd.state == DD_DRAGGING && hover.kind == DT_ATTACH && hover.index == k && !carrying_from(me)) {
                DDResult pr = dd_drop_on(&V.dd, me, &p->inv, db, sel);
                v.drop_state = pr.action == DDA_REJECT ? 2 : 1;
            }
            float *sh = &V.shake[INV_SLOTS + CONTAINER_SLOTS + k];
            *sh = fmaxf(0.0f, *sh - gui_dt() / fmaxf(0.05f, TH->shake));
            v.shake = *sh;
            gw_slot(chip, &v);
            char sl[24];
            snprintf(sl, sizeof(sl), "%s", attach_slot_name(k));
            gw_label_caps(gui_rect(chip.x, chip.y + chip.h + gui_s(1), chip.w, gui_s(16)), sl, gui_s(TH->t_tiny), ci.hot ? TH->text : TH->text_faint, GUI_CENTER);
            if ((ci.hot || (V.dd.state == DD_DRAGGING && hl == k)) && V.att_socket_vis[k]) {
                vec2 a = v2(chip.x + chip.w * 0.5f, chip.y);
                gd_line(a.x, a.y, V.att_socket[k].x, V.att_socket[k].y, gui_s(1), gui_rgba_mul(TH->accent, 0.7f));
                gd_circle(V.att_socket[k].x, V.att_socket[k].y, gui_s(3), TH->accent);
            }
            if (ci.hovered && fake.item && V.dd.state == DD_IDLE) gui_tooltip(att_id(k), item_def(db, fake.item)->name);
        }
        (void)present;
    }
    // Name, type, description, stats.
    GuiRect nr = gc_row(&c, gui_s(30));
    GuiText name = { gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT, 0, false, false, nr.w };
    gd_text(&name, nr.x, nr.y, revive_item_name(db, s->item, s->ammo));
    GuiRect tr = gc_row(&c, gui_s(18));
    gw_label_caps(tr, item_kind_name(d->kind), gui_s(TH->t_label), type_color(d), GUI_LEFT);
    GuiText desc = { gui_s(TH->t_small + 1), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, false, 0 };
    float dh = gd_text_wrap(&desc, c.area.x, c.y + gui_s(2), c.area.w, d->desc, true);
    c.y += dh + gui_s(10);
    gd_clip_push(gui_rect(r.x, c.y, r.w, r.y + r.h - c.y - gui_s(8)));
    item_stats(&c, s, db);
    gd_clip_pop();
}

// ------------------------------------------------------------------ context menu

static void draw_context(const Player *p, const ItemDB *db, int equipped) {
    if (!V.ctx.open) return;
    const InvSlot *s = &p->inv.slots[V.ctx.slot];
    if (!s->item) { V.ctx.open = false; return; }
    const ItemDef *d = item_def(db, s->item);
    const WeaponDef *wd = item_weapon(db, s->item);
    typedef struct Entry { const char *label; int op; int count; } Entry;
    Entry e[8];
    int n = 0;
    if (d->kind == ITEM_CONSUMABLE) { Entry x = { "Use", IOP_USE, 0 }; e[n++] = x; }
    if (V.ctx.slot >= INV_HOTBAR || V.ctx.slot != equipped) { Entry x = { "Equip", IOP_EQUIP, 0 }; e[n++] = x; }
    if (s->count > 1) { Entry x = { "Split", IOP_SPLIT, 0 }; e[n++] = x; }
    { Entry x = { V.ctx.slot < INV_HOTBAR ? "Move to backpack" : "Move to hotbar", IOP_QUICK_MOVE, 0 }; e[n++] = x; }
    if (wd) {
        bool any = false;
        for (int k = 0; k < ATT_SLOTS; k++) any |= s->att[k] != 0;
        if (any) { Entry x = { "Remove attachments", IOP_UNMOUNT, 0 }; e[n++] = x; }
    }
    { Entry x = { s->count > 1 ? "Drop one" : "Drop", IOP_DROP, s->count > 1 ? 1 : 0 }; e[n++] = x; }
    if (s->count > 1) { Entry x = { "Drop all", IOP_DROP, 0 }; e[n++] = x; }
    float ih = gui_s(30), w = gui_s(190);
    GuiRect r = { V.ctx.pos.x + gui_s(4), V.ctx.pos.y + gui_s(4), w, ih * (float)n + gui_s(33) };
    if (r.x + r.w > gui_w()) r.x = V.ctx.pos.x - r.w;
    if (r.y + r.h > gui_h()) r.y = V.ctx.pos.y - r.h;
    int layer = gd_layer();
    gd_set_layer(GL_OVERLAY);
    float a = gui_ease_out((float)(gui_time() - V.ctx.t) / 0.1f);
    gd_alpha_push(a);
    gd_shadow(r, gui_s(TH->radius), gui_s(16), TH->shadow);
    gd_round_border(r, gui_s(TH->radius), 1, UI_RGBA(22, 24, 27, 250), UI_RGBA(255, 255, 255, 30));
    gui_block_rect(r);
    gw_label_caps(gui_rect(r.x + gui_s(15), r.y + gui_s(6), r.w - gui_s(30), gui_s(20)), d->name, gui_s(TH->t_label), TH->text_faint, GUI_LEFT);
    for (int i = 0; i < n; i++) {
        GuiRect ir = { r.x + gui_s(5), r.y + gui_s(28) + ih * (float)i, r.w - gui_s(10), ih };
        GuiId id = gui_idi("inv_ctx", i);
        GuiInteract in = gui_interact(id, ir, GUI_FOCUSABLE | (i == 0 ? GUI_DEFAULT_FOCUS : 0));
        if (in.hot) gd_round(ir, gui_s(TH->radius), TH->hover);
        bool danger = e[i].op == IOP_DROP;
        gw_label(gui_rect(ir.x + gui_s(10), ir.y, ir.w - gui_s(20), ir.h), e[i].label, gui_s(TH->t_body - 1), GW_MEDIUM,
                 danger ? gui_rgba_lerp(TH->text_dim, TH->danger, 0.6f) : (in.hot ? TH->text : TH->text_dim), GUI_LEFT);
        if (in.clicked) {
            InvOp op;
            memset(&op, 0, sizeof(op));
            op.type = (u8)e[i].op;
            op.a = (u8)V.ctx.slot;
            op.count = (u16)e[i].count;
            V.ctx.open = false;
            if (e[i].op == IOP_SPLIT) op.b = 255;
            if (e[i].op == IOP_EQUIP) op.b = game()->hotbar_slot;
            if (e[i].op == IOP_UNMOUNT) {
                for (int k = 0; k < ATT_SLOTS; k++)
                    if (s->att[k]) { InvOp u = op; u.b = (u8)k; u.c = 255; send(u); }
                break;
            }
            if (e[i].op == IOP_DROP && !e[i].count && DG_MAX(1, d->max_stack) <= 1) {
                DDResult rr;
                memset(&rr, 0, sizeof(rr));
                rr.action = DDA_CONFIRM_DROP;
                rr.op = op;
                apply(&rr);
                break;
            }
            send(op);
            break;
        }
    }
    gui_focus_scope(r);
    gd_alpha_pop();
    gd_set_layer(layer);
    if (gui_nav(NAV_BACK)) { gui_nav_consume(NAV_BACK); V.ctx.open = false; }
    else if ((gui_mouse_pressed(0) || gui_mouse_pressed(1)) && !gui_rect_contains(r, gui_mouse().x, gui_mouse().y) && gui_time() - V.ctx.t > 0.05) V.ctx.open = false;
}

// ------------------------------------------------------------------ screen

static DDTarget hit_test(vec2 m, bool container) {
    DDTarget t = { DT_OUTSIDE, -1 };
    for (int i = 0; i < INV_SLOTS; i++) if (gui_rect_contains(V.slot_r[i], m.x, m.y)) { t.kind = DT_SLOT; t.index = (i16)i; return t; }
    if (container)
        for (int i = 0; i < CONTAINER_SLOTS; i++) if (gui_rect_contains(V.cont_r[i], m.x, m.y)) { t.kind = DT_CONTAINER; t.index = (i16)i; return t; }
    for (int k = 0; k < ATT_SLOTS; k++) if (V.att_vis[k] && gui_rect_contains(V.att_r[k], m.x, m.y)) { t.kind = DT_ATTACH; t.index = (i16)k; return t; }
    if (!container && gui_rect_contains(V.view, m.x, m.y)) { t.kind = DT_PREVIEW; t.index = 0; return t; }
    if (gui_rect_contains(V.panel, m.x, m.y)) { t.kind = DT_PANEL; t.index = -1; }
    return t;
}

static DDTarget focus_target(bool container) {
    GuiId f = gui_focus();
    DDTarget t = { DT_NONE, -1 };
    for (int i = 0; i < INV_SLOTS; i++) if (f == slot_id(i)) { t.kind = DT_SLOT; t.index = (i16)i; return t; }
    if (container) for (int i = 0; i < CONTAINER_SLOTS; i++) if (f == cont_id(i)) { t.kind = DT_CONTAINER; t.index = (i16)i; return t; }
    for (int k = 0; k < ATT_SLOTS; k++) if (f == att_id(k)) { t.kind = DT_ATTACH; t.index = (i16)k; return t; }
    return t;
}

static void draw_ghost(const Player *p, const ItemDB *db, bool container) {
    if (V.dd.state != DD_DRAGGING) return;
    InvSlot s;
    memset(&s, 0, sizeof(s));
    if (V.dd.src.kind == DT_SLOT) s = p->inv.slots[V.dd.src.index];
    else if (V.dd.src.kind == DT_CONTAINER && container) s = game()->container_slots[V.dd.src.index];
    else if (V.dd.src.kind == DT_ATTACH) {
        int sel = V.selected >= 0 ? V.selected : game()->hotbar_slot;
        s.item = item_for_attachment(db, p->inv.slots[sel].att[V.dd.src.index]);
        s.count = 1;
    }
    if (!s.item) return;
    if (V.dd.split && s.count > 1) s.count = (u16)(s.count / 2);
    vec2 m = gui_mouse();
    GuiRect fr;
    if (V.dd.carry && gui_focus_rect(&fr)) m = v2(fr.x + fr.w * 0.7f, fr.y + fr.h * 0.7f);
    float sz = gui_s(TH->slot * 1.05f);
    GuiRect r = { m.x - sz * 0.5f, m.y - sz * 0.5f, sz, sz };
    int layer = gd_layer();
    gd_set_layer(GL_TOP);
    gd_shadow(r, gui_s(TH->radius), gui_s(14), UI_RGBA(0, 0, 0, 150));
    GuiSlotView v;
    inventory_slot_view(&s, db, -1, &v);
    v.selected = true;
    gd_alpha_push(0.92f);
    gw_slot(r, &v);
    gd_alpha_pop();
    gd_set_layer(layer);
}

bool inventory_draw(GuiRect area, bool as_modal) {
    Game *g = game();
    World *w = world();
    Player *p = game_local_player();
    const ItemDB *db = items();
    if (!g || !w || !p || !db) return true;
    icons_tick();
    bool container = g->container_open;
    int equipped = DG_CLAMP((int)g->hotbar_slot, 0, INV_HOTBAR - 1);
    int sel = V.selected >= 0 ? V.selected : equipped;
    if (sel >= 0 && !p->inv.slots[sel].item && V.selected >= 0) sel = V.selected;

    // Metrics: grid slots; the hotbar uses the same slot size (one grid row: INV_HOTBAR == INV_BACKPACK_W).
    float gap = gui_s(TH->slot_gap);
    float gs = gui_s(TH->slot * 1.18f);
    float grid_w = gs * INV_BACKPACK_W + gap * (INV_BACKPACK_W - 1);
    float hs = gs;
    float side_w = container ? gs * 4 + gap * 3 : gui_s(400);
    float pad = gui_s(TH->pad + 6), mid = gui_s(28);
    float pw = pad * 2 + grid_w + mid + side_w;
    float ph = gui_s(64) + gui_s(26) + gs * INV_BACKPACK_H + gap * (INV_BACKPACK_H - 1) + gui_s(36) + hs + gui_s(44) + pad;
    ph = fmaxf(ph, gui_s(container ? 560.0f : 680.0f));
    GuiRect panel = { area.x + (area.w - pw) * 0.5f, area.y + (area.h - ph) * 0.5f, pw, ph };
    if (as_modal) panel.y = area.y;
    float a = gui_ease_out(gui_anim_t(gui_id("inv_open")) / TH->modal_in);
    if (!as_modal) gd_rect(gui_rect(0, 0, gui_w(), gui_h()), gui_rgba_mul(TH->scrim, a));
    panel.y += (1.0f - a) * gui_s(14);
    V.panel = panel;
    gd_alpha_push(a);
    GuiRect c = screens_modal_frame(panel, "Inventory", game_inv_pending() ? "syncing\xE2\x80\xA6" : NULL);
    if (gui_input_enabled() && !V.ctx.open) gui_focus_scope(panel);

    run_step();
    // Input for the drag and drop (the hover target from this frame's rects - drawn last frame).
    DDInput in;
    memset(&in, 0, sizeof(in));
    in.mouse = gui_mouse();
    bool en = gui_input_enabled() && !V.ctx.open;
    in.pressed = en && gui_mouse_pressed(0);
    in.released = gui_mouse_released(0);
    in.right_pressed = en && gui_mouse_pressed(1);
    in.double_clicked = en && gui_double_clicked();
    in.shift = gui_shift();
    in.ctrl = gui_ctrl();
    in.threshold = gui_s(6);
    in.weapon_slot = item_weapon(db, p->inv.slots[sel].item) ? sel : -1;
    in.hover = hit_test(in.mouse, container);
    in.cancel = V.dd.state != DD_IDLE && gui_nav(NAV_BACK);
    if (in.cancel) gui_nav_consume(NAV_BACK);

    // Backpack grid.
    float gx = c.x, gy = c.y + gui_s(4);
    gw_label_caps(gui_rect(gx, gy, grid_w, gui_s(20)), "Backpack", gui_s(TH->t_label), TH->text_faint, GUI_LEFT);
    gy += gui_s(26);
    for (int row = 0; row < INV_BACKPACK_H; row++)
        for (int col = 0; col < INV_BACKPACK_W; col++) {
            int i = INV_HOTBAR + row * INV_BACKPACK_W + col;
            V.slot_r[i] = gui_rect(gx + (gs + gap) * (float)col, gy + (gs + gap) * (float)row, gs, gs);
            draw_slot(i, V.slot_r[i], p, db, equipped, in.hover);
        }
    float hy = gy + gs * INV_BACKPACK_H + gap * (INV_BACKPACK_H - 1) + gui_s(16);
    gw_label_caps(gui_rect(gx, hy, grid_w, gui_s(20)), "Hotbar", gui_s(TH->t_label), TH->text_faint, GUI_LEFT);
    hy += gui_s(22);
    for (int i = 0; i < INV_HOTBAR; i++) {
        V.slot_r[i] = gui_rect(gx + (hs + gap) * (float)i, hy, hs, hs);
        draw_slot(i, V.slot_r[i], p, db, equipped, in.hover);
    }

    // Right side: crate or preview.
    GuiRect side = { gx + grid_w + mid, c.y + gui_s(4), side_w, c.h - gui_s(48) };
    if (container) {
        gw_label_caps(gui_rect(side.x, side.y, side.w, gui_s(20)), "Crate", gui_s(TH->t_label), TH->text_faint, GUI_LEFT);
        float cy = side.y + gui_s(26);
        for (int i = 0; i < CONTAINER_SLOTS; i++) {
            GuiRect r = { side.x + (gs + gap) * (float)(i % 4), cy + (gs + gap) * (float)(i / 4), gs, gs };
            V.cont_r[i] = r;
            GuiInteract ci = gui_interact(cont_id(i), r, GUI_FOCUSABLE);
            GuiSlotView v;
            inventory_slot_view(&g->container_slots[i], db, -1, &v);
            v.hot = ci.hot;
            v.focus = ci.focused;
            DDTarget me = { DT_CONTAINER, (i16)i };
            if (carrying_from(me)) v.dim = true;
            if (V.dd.state == DD_DRAGGING && in.hover.kind == DT_CONTAINER && in.hover.index == i && V.dd.src.kind != DT_CONTAINER) v.drop_state = 1;
            gw_slot(r, &v);
        }
        GuiRect tb = { side.x, cy + (gs + gap) * 3 + gui_s(8), gs * 2 + gap, gui_s(TH->button_h + 4) };
        if (gw_button(gui_id("inv_take_all"), tb, "Take all", GB_PRIMARY, true)) game_rpc(RPC_CONTAINER_TAKE, 255, 0, 0);
    } else {
        draw_preview(side, p, db, sel, in.hover);
    }

    // Drag and drop (mouse) / carry (keyboard, gamepad).
    if (gui_input_enabled() && !V.ctx.open) {
        DDResult r = dd_update(&V.dd, &in, &p->inv, db);
        if (r.action) apply(&r);
        if (gui_nav_mode()) {
            DDTarget ft = focus_target(container);
            if (gui_nav(NAV_ACCEPT) && ft.kind != DT_NONE) {
                gui_nav_consume(NAV_ACCEPT);
                DDResult cr = dd_carry_accept(&V.dd, ft, &p->inv, db, in.weapon_slot, false);
                if (cr.action == DDA_SELECT || cr.action == DDA_NONE) { if (ft.kind == DT_SLOT) V.selected = ft.index; }
                apply(&cr);
                if (ft.kind == DT_SLOT && cr.action == DDA_PICKED) V.selected = ft.index;
            }
            if (gui_nav(NAV_ALT) && ft.kind == DT_SLOT && p->inv.slots[ft.index].item && V.dd.state == DD_IDLE) {
                gui_nav_consume(NAV_ALT);
                GuiRect fr;
                V.ctx.open = true;
                V.ctx.slot = ft.index;
                V.ctx.pos = gui_focus_rect(&fr) ? v2(fr.x + fr.w, fr.y) : gui_mouse();
                V.ctx.t = gui_time();
                gui_set_focus(0);
            }
            if (gui_nav(NAV_ALT2) && ft.kind == DT_SLOT && p->inv.slots[ft.index].item && V.dd.state == DD_IDLE) {
                gui_nav_consume(NAV_ALT2);
                InvOp op;
                memset(&op, 0, sizeof(op));
                op.type = IOP_QUICK_MOVE;
                op.a = (u8)ft.index;
                send(op);
            }
            if (ft.kind == DT_SLOT && V.dd.state == DD_IDLE && p->inv.slots[ft.index].item) V.selected = ft.index;
        }
    }

    // Footer: key hints + the last rejection.
    GuiRect foot = { c.x, panel.y + panel.h - pad * 0.5f - gui_s(20), c.w, gui_s(18) };
    if (V.toast[0] && gui_time() - V.toast_t < 2.5) {
        float ta = 1.0f - saturate((float)(gui_time() - V.toast_t - 2.0) / 0.5f);
        GuiText t = { gui_s(TH->t_small + 1), GW_SEMIBOLD, gui_rgba_mul(TH->danger, ta), GUI_LEFT, 0, false, false, 0 };
        gd_text(&t, foot.x, foot.y - gui_s(2), V.toast);
    }
    screens_footer_hint(foot, gui_nav_mode() ? "A  pick up / place    X  menu    Y  quick move    B  back"
                                             : "Drag  move    Right click  menu    Shift+click  quick move    Double click  equip    Tab  close");
    draw_context(p, db, equipped);
    gd_alpha_pop();
    draw_ghost(p, db, container);
    return false;
}
