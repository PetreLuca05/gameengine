// Inventory screen (docs/INVENTORY.md "Screen"), full screen over the game:
//   left:   the player's grids stacked (pockets, rig, backpack, secure container: the quick-move
//           order), each a panel with its name, weight and used / total cells; the column scrolls;
//   centre: the player, live (the game camera turns to face the body: inv_showcase), with the
//           equipment slots around it and the discard zone below; no backdrop behind the player;
//   right:  the hotbar (six large tiles); with a world container open the hotbar compacts and the
//           loot grid (crate, body, trunk) fills the rest of the column.
// No tabs. Inspect windows, nested container windows and the split dialog float over it.
// Mouse: drag between any grids / slots / tiles (R rotates, the footprint shows green / red),
// Ctrl+click quick move, Alt+click equip, Shift+drag split, double click open / inspect, right
// click menu, drag out / onto Discard drops. Pad: a cell cursor (d-pad / stick), LB / RB cycle the
// regions, A pick up / place, X rotate (menu when idle), Y quick move, B cancel / close.
// Every change is an intent (gameplay/inventory/inv_intent.h) predicted locally and confirmed by
// the host; the drag-and-drop state machine and the layout are pure (inv_dd.c, inv_layout.c).
#include "inv_screen.h"
#include "ui/screens/screens.h"
#include "ui/icons.h"
#include "ui/item_view.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/cvar.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "gameplay/attachments.h"
#include "gameplay/weapons.h"
#include "gameplay/entities.h"
#include "gameplay/revive/revive.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TH (gui_theme())
#define WHITE UI_RGBA(255, 255, 255, 255)   // lint:ok image tint (no colour change)
#define PREVIEW_W 768
#define PREVIEW_H 512
#define MAX_HITS 640
#define MAX_WINS 4
#define MAX_PAD 640

typedef enum HitKind { HK_BLOCK, HK_GRID, HK_EQUIP, HK_HOT, HK_ATTACH, HK_DISCARD } HitKind;
typedef struct Hit {
    u8 kind;
    GuiRect r, clip;          // clip: the visible part (scrolled panels, windows)
    InvTarget t;              // EQUIP / HOT / ATTACH: the target; GRID: side / parent / loc
    float cell;
} Hit;

typedef struct Win { bool open; u8 side; u16 uid; vec2 pos; bool dragging; vec2 grab; } Win;

static struct {
    InvDrag dd;
    InvTarget hover;
    InvTarget sel;             // selected item (CELL / EQUIP: side + uid)
    struct { bool open; InvTarget at; vec2 pos; double t; } ctx;
    struct { bool open; InvIntent in; float count; int max; } split;
    InvIntent pending_discard;
    char toast[96];
    double toast_t;
    InvTarget shake_at;
    double shake_t;
    Hit hits[MAX_HITS];
    int hit_n;
    GuiScroll left_scroll, loot_scroll;
    Win win[MAX_WINS];
    struct {
        bool open, dragging, rotating;
        u8 side;
        u16 uid;
        vec2 pos, grab, rot_from;
        TextureId tex;
        ItemViewCam cam;
        u16 cam_item;
        double last_touch;
    } insp;
    int filter[INV_UI_MAX_PANELS + 1];   // per panel: 0 all, 1 weapons, 2 ammo, 3 meds, 4 gear (dims the rest)
    // pad cursor
    InvTarget pad_t[MAX_PAD];
    GuiRect pad_r[MAX_PAD];
    u8 pad_region[MAX_PAD];
    int pad_n, pad_cur;
    bool alt_scripted;
    InvLayout L;
    Cvar *showcase, *order;
} V;

static Game *g_(void) { return game(); }
static const ItemDB *items(void) { Game *g = g_(); return g && g->world ? g->world->items : NULL; }

InvTree *inv_screen_tree(int side) {
    Player *p = game_local_player();
    if (side == INV_SIDE_SELF) return p ? &p->inv.t : NULL;
    return game_loot_tree();
}

static InvView view(void) {
    InvView v;
    Player *p = game_local_player();
    v.db = items();
    v.tree[INV_SIDE_SELF] = p ? &p->inv.t : NULL;
    v.tree[INV_SIDE_LOOT] = game_loot_tree();
    v.quick = p ? p->inv.quick : NULL;
    return v;
}

static const InvItem *item_of(int side, u16 uid) {
    InvTree *t = inv_screen_tree(side);
    return t && uid ? inv_get_c(t, uid) : NULL;
}

void inv_screen_toast(const char *msg) {
    dg_strcpy(V.toast, sizeof(V.toast), msg);
    V.toast_t = gui_time();
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

// The slot view of a payload (the HUD hotbar and the attachment chips use the slot widget).
void inventory_slot_view(const InvSlot *s, const ItemDB *db, int mag_override, GuiSlotView *v) {
    memset(v, 0, sizeof(*v));
    v->ammo_frac = -1;
    v->empty = !s->item;
    if (!s->item) return;
    const ItemDef *d = item_def(db, s->item);
    v->tint = type_color(d);
    v->name = revive_item_name(db, s->item, s->ammo);   // DNA samples carry the player's name
    v->count = s->count;
    if (!inv_cells_get(s->item, &v->icon, &v->uv)) icons_get(s->item, &v->icon, &v->uv);
    const WeaponDef *wd = item_weapon(db, s->item);
    if (wd && wd->mag_size > 0 && wd->kind != WPN_MELEE) {
        WeaponDef eff;
        attach_weapon_effective(db, wd, s->att, &eff);
        int ammo = mag_override >= 0 ? mag_override : s->ammo;
        v->ammo = ammo;
        v->ammo_frac = (float)ammo / (float)DG_MAX(1, eff.mag_size);
    }
}

// ------------------------------------------------------------------ intents

// A dry run on copies: what the intent would do (drop highlights, rejections before sending).
static int dry_run(const InvIntent *in) {
    static Inventory inv;
    static InvTree loot;
    Player *p = game_local_player();
    const ItemDB *db = items();
    if (!p || !db) return IIR_INVALID;
    inv = p->inv;
    InvTree *lt = game_loot_tree();
    if (lt) loot = *lt;
    InvCtx c = inv_ctx(&inv, db, lt ? &loot : NULL);
    return inv_intent_apply(&c, in, NULL);
}

static void shake(InvTarget at) {
    V.shake_at = at;
    V.shake_t = gui_time();
}

static float shake_amount(InvTarget at) {
    float t = (float)(gui_time() - V.shake_t);
    if (t > TH->shake || V.shake_at.kind != at.kind || V.shake_at.uid != at.uid || V.shake_at.index != at.index || V.shake_at.parent != at.parent) return 0;
    return 1.0f - t / fmaxf(0.05f, TH->shake);
}

int inv_screen_send(InvIntent in) {
    int r = game_inv_intent(&in);
    if (r != IIR_OK && r != IIR_NOOP) {
        inv_screen_toast(inv_result_text(r));
        gui_sound(TH->snd_error);
        return r;
    }
    if (r == IIR_OK) {
        if (in.type == II_DISCARD) gui_sound(TH->snd_drop);
        else if (in.type == II_MOUNT || in.type == II_UNMOUNT || in.type == II_EQUIP || in.type == II_BIND) gui_sound(TH->snd_equip);
        else gui_sound(TH->snd_click);
    }
    return r;
}

void inventory_confirm_drop(bool yes) {
    if (yes && V.pending_discard.type) inv_screen_send(V.pending_discard);
    memset(&V.pending_discard, 0, sizeof(V.pending_discard));
}

static void open_window(int side, u16 uid) {
    const InvItem *it = item_of(side, uid);
    if (!it || !item_container(items(), it->s.item)) return;
    for (int i = 0; i < MAX_WINS; i++) if (V.win[i].open && V.win[i].side == side && V.win[i].uid == uid) return;
    int slot = 0;
    for (int i = 0; i < MAX_WINS; i++) if (!V.win[i].open) { slot = i; break; }
    Win *w = &V.win[slot];
    w->open = true;
    w->side = (u8)side;
    w->uid = uid;
    w->pos = v2(V.L.center.x + V.L.center.w * 0.2f + (float)slot * gui_s(24), V.L.center.y + gui_s(40) + (float)slot * gui_s(24));
    w->dragging = false;
    gui_sound(TH->snd_click);
}

static void open_inspect(int side, u16 uid) {
    V.insp.open = true;
    V.insp.side = (u8)side;
    V.insp.uid = uid;
    V.insp.pos = v2(V.L.center.x + V.L.center.w * 0.08f, V.L.center.y + gui_s(24));
    gui_anim_restart(gui_id("inv_insp_in"));
}

static void apply(const InvDDOut *o) {
    switch (o->action) {
    case IDA_INTENT: {
        int r = dry_run(&o->in);
        if (r != IIR_OK && r != IIR_NOOP) {
            shake(o->at);
            inv_screen_toast(inv_result_text(r));
            gui_sound(TH->snd_error);
            break;
        }
        inv_screen_send(o->in);
        break;
    }
    case IDA_SPLIT: {
        const InvItem *it = item_of(o->in.src_side, o->in.src);
        if (!it) break;
        V.split.open = true;
        V.split.in = o->in;
        V.split.max = it->s.count - 1;
        V.split.count = (float)o->in.count;
        gui_set_focus(0);
        break;
    }
    case IDA_CONFIRM: {
        const InvItem *it = item_of(o->in.src_side, o->in.src);
        if (!it) break;
        V.pending_discard = o->in;
        char title[96];
        snprintf(title, sizeof(title), "Discard %s?", item_def(items(), it->s.item)->name);
        screens_confirm(CONFIRM_DROP, title, item_container(items(), it->s.item) ? "It and everything in it will be left on the ground." : "It will be left on the ground in front of you.", 0);
        break;
    }
    case IDA_SELECT:
        if (o->at.uid) { V.sel = o->at; gui_sound(TH->snd_click); }
        break;
    case IDA_CONTEXT:
        V.ctx.open = true;
        V.ctx.at = o->at;
        V.ctx.pos = gui_mouse();
        V.ctx.t = gui_time();
        V.sel = o->at;
        gui_set_focus(0);
        break;
    case IDA_OPEN: {
        int side = o->at.kind == IT_HOT ? INV_SIDE_SELF : o->at.side;
        const InvItem *it = item_of(side, o->at.uid);
        if (!it) break;
        if (item_container(items(), it->s.item) && !(side == INV_SIDE_SELF && !it->parent && it->loc >= INV_LOC_EQUIP)) open_window(side, it->uid);
        else open_inspect(side, it->uid);
        break;
    }
    case IDA_REJECT:
        shake(o->at);
        inv_screen_toast(idd_reason_text(o->reason));
        gui_sound(TH->snd_error);
        break;
    case IDA_PICKED: gui_sound(TH->snd_click); break;
    default: break;
    }
}

// ------------------------------------------------------------------ drawing helpers

static void hit_add(int kind, GuiRect r, GuiRect clip, InvTarget t, float cell) {
    if (V.hit_n >= MAX_HITS) return;
    Hit *h = &V.hits[V.hit_n++];
    h->kind = (u8)kind;
    h->r = r;
    h->clip = clip;
    h->t = t;
    h->cell = cell;
}

static void pad_add(InvTarget t, GuiRect r, int region) {
    if (V.pad_n >= MAX_PAD) return;
    V.pad_t[V.pad_n] = t;
    V.pad_r[V.pad_n] = r;
    V.pad_region[V.pad_n++] = (u8)region;
}

static GuiRect fit_aspect(GuiRect r, float aspect) {
    float w = r.w, h = r.w / aspect;
    if (h > r.h) { h = r.h; w = h * aspect; }
    return gui_rect(r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h);
}

void inv_draw_item_image(GuiRect r, const ItemDB *db, const InvSlot *s, bool rotated, float alpha) {
    const ItemDef *d = item_def(db, s->item);
    TextureId tex;
    vec4 uv;
    gd_alpha_push(alpha);
    if (inv_cells_get(s->item, &tex, &uv)) {
        float a = (float)DG_MAX(1, d->grid_w) / (float)DG_MAX(1, d->grid_h);
        GuiRect f = fit_aspect(r, rotated ? 1.0f / a : a);
        if (rotated) gd_image_rot90(f, tex, uv, WHITE);
        else gd_image(f, tex, uv, WHITE);
    } else if (icons_get(s->item, &tex, &uv)) {
        gd_image(fit_aspect(r, 1.0f), tex, uv, WHITE);
    } else {
        GuiText t = { fminf(r.h * 0.4f, gui_s(18)), GW_SEMIBOLD, TH->text_dim, GUI_CENTER, 0, true, false, r.w };
        gd_text(&t, r.x + r.w * 0.5f, r.y + r.h * 0.5f - t.size * 0.6f, d->name);
    }
    gd_alpha_pop();
}

static bool filtered_out(int filter, const ItemDef *d) {
    switch (filter) {
    case 1: return d->kind != ITEM_WEAPON && d->kind != ITEM_THROWABLE && d->kind != ITEM_ATTACHMENT;
    case 2: return d->kind != ITEM_AMMO;
    case 3: return d->kind != ITEM_CONSUMABLE;
    case 4: return d->equip_slot == EQ_NONE && d->container < 0;
    default: return false;
    }
}

static bool carrying(int side, u16 uid) { return V.dd.state == IDD_DRAGGING && V.dd.side == side && V.dd.uid == uid && V.dd.src.kind != IT_HOT; }

static int bound_key(const Player *p, u16 uid) {
    for (int k = 0; k < INV_HOTBAR; k++) if (p->inv.quick[k] == uid) return k + 1;
    return 0;
}

// One item tile inside a grid (or an equipment slot).
static void draw_item_tile(GuiRect r, const InvItem *it, int side, bool dim, InvTarget me) {
    const ItemDB *db = items();
    const ItemDef *d = item_def(db, it->s.item);
    Player *p = game_local_player();
    float rad = gui_s(TH->radius);
    float sx = shake_amount(me);
    if (sx > 0) r.x += sinf(sx * 40.0f) * sx * gui_s(5);
    bool sel = V.sel.uid == it->uid && (V.sel.kind == IT_HOT ? INV_SIDE_SELF : V.sel.side) == side;
    u32 bg = gui_rgba_lerp(TH->slot_bg, type_color(d), 0.16f);
    if (V.hover.uid == it->uid && V.hover.side == side && V.dd.state == IDD_IDLE) bg = gui_rgba_lerp(bg, TH->slot_hi, 0.6f);
    gd_alpha_push(carrying(side, it->uid) ? 0.35f : (dim ? 0.3f : 1.0f));
    gd_round_border(gui_rect_inset(r, 1), rad, sel ? fmaxf(1.0f, gui_s(1.5f)) : 1.0f, bg, sel ? TH->select : TH->slot_border);
    inv_draw_item_image(gui_rect_inset(r, fminf(r.w, r.h) * 0.08f), db, &it->s, (it->flags & INVF_ROT) != 0, 1.0f);
    float fs = fmaxf(gui_s(TH->t_tiny), fminf(gui_s(13), V.L.cell * 0.22f));
    GuiText name = { fs, GW_MEDIUM, TH->text_dim, GUI_LEFT, 0.02f, true, true, r.w - gui_s(8) };
    gd_text(&name, r.x + gui_s(4), r.y + gui_s(2), revive_item_name(db, it->s.item, it->s.ammo));
    char b[24];
    const WeaponDef *wd = item_weapon(db, it->s.item);
    if (it->s.count > 1) {
        snprintf(b, sizeof(b), "%d", it->s.count);
        GuiText t = { fs, GW_SEMIBOLD, TH->text, GUI_RIGHT, 0, false, true, 0 };
        gd_text(&t, r.x + r.w - gui_s(4), r.y + r.h - gd_line_height(fs) - gui_s(2), b);
    } else if (wd && wd->kind != WPN_MELEE && wd->mag_size > 0) {
        snprintf(b, sizeof(b), "%d", it->s.ammo);
        GuiText t = { fs, GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, true, 0 };
        gd_text(&t, r.x + r.w - gui_s(4), r.y + r.h - gd_line_height(fs) - gui_s(2), b);
    } else if (d->use_type == INV_USE_CHARGES && d->charges > 1) {
        snprintf(b, sizeof(b), "%d/%d", d->charges - it->s.ammo, d->charges);
        GuiText t = { fs, GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, true, 0 };
        gd_text(&t, r.x + r.w - gui_s(4), r.y + r.h - gd_line_height(fs) - gui_s(2), b);
    }
    if (d->durability) {   // durability: a thin bar along the bottom
        float f = (float)it->dur / (float)d->durability;
        GuiRect bar = { r.x + gui_s(4), r.y + r.h - gui_s(4), (r.w - gui_s(8)) * saturate(f), gui_s(2) };
        gd_rect(bar, f < 0.3f ? TH->danger : TH->text_faint);
    }
    const InvContainerDef *cd = item_container(db, it->s.item);
    if (cd && side < INV_SIDES) {   // a container: how full
        InvTree *t = inv_screen_tree(side);
        int used = t ? inv_cells_used(t, db, it->uid) : 0;
        snprintf(b, sizeof(b), "%d/%d", used, cd->capacity);
        GuiText t2 = { fs * 0.9f, GW_MEDIUM, TH->text_faint, GUI_LEFT, 0, false, true, 0 };
        gd_text(&t2, r.x + gui_s(4), r.y + r.h - gd_line_height(t2.size) - gui_s(2), b);
    }
    int key = side == INV_SIDE_SELF && p ? bound_key(p, it->uid) : 0;
    if (key) {
        snprintf(b, sizeof(b), "%d", key);
        float k = fs * 1.3f;
        GuiRect kr = { r.x + r.w - k - gui_s(3), r.y + gui_s(3), k, k };
        gd_round(kr, gui_s(3), gui_rgba_mul(TH->accent, 0.85f));
        gw_label(kr, b, fs, GW_SEMIBOLD, TH->text_dark, GUI_CENTER);
    }
    gd_alpha_pop();
}

static const char *short_slot_name(int slot) {
    static const char *n[EQ_COUNT] = { "Primary", "Secondary", "Holster", "Melee", "Headwear", "Earpiece", "Face cover", "Eyewear",
                                       "Body armor", "Tactical rig", "Backpack", "Armband", "Secure" };
    return slot >= 0 && slot < EQ_COUNT ? n[slot] : "?";
}

static void tooltip_for(GuiId id, int side, const InvItem *it) {
    const ItemDB *db = items();
    const ItemDef *d = item_def(db, it->s.item);
    int w, h;
    inv_item_size(db, it, &w, &h);
    char tip[320];
    int n = snprintf(tip, sizeof(tip), "%s\n%s  \xC2\xB7  %dx%d  \xC2\xB7  %.2f kg", revive_item_name(db, it->s.item, it->s.ammo), item_kind_name(d->kind), w, h,
                     inv_item_weight(inv_screen_tree(side), db, it));
    if (d->max_stack > 1) n += snprintf(tip + n, sizeof(tip) - (size_t)n, "\nStack %d / %d", it->s.count, d->max_stack);
    if (d->durability) n += snprintf(tip + n, sizeof(tip) - (size_t)n, "\nDurability %d / %d", it->dur, d->durability);
    if (d->equip_slot != EQ_NONE) n += snprintf(tip + n, sizeof(tip) - (size_t)n, "\nWorn: %s", short_slot_name(d->equip_slot));
    gui_tooltip(id, tip);
}

// The carried item's footprint over the hovered grid: green when the drop would apply, red when not.
static void draw_footprint(const Hit *h) {
    if (V.dd.state != IDD_DRAGGING || V.hover.kind != IT_CELL || V.hover.side != h->t.side || V.hover.parent != h->t.parent || V.hover.loc != h->t.loc) return;
    InvView v = view();
    int x, y, w, hh;
    if (!idd_footprint(&V.dd, V.hover, &v, &x, &y, &w, &hh)) return;
    InvDDOut o = idd_drop_on(&V.dd, V.hover, &v);
    bool ok = (o.action == IDA_INTENT || o.action == IDA_SPLIT || o.action == IDA_CONFIRM) && dry_run(&o.in) == IIR_OK;
    GuiRect fr = { h->r.x + (float)x * h->cell, h->r.y + (float)y * h->cell, (float)w * h->cell, (float)hh * h->cell };
    u32 c = ok ? TH->ok : TH->danger;
    gd_round_border(fr, gui_s(TH->radius), fmaxf(1.0f, gui_s(1.5f)), gui_rgba_mul(c, 0.22f), gui_rgba_mul(c, 0.9f));
}

// A container's compartments at origin (cells), its items, hit / pad registration.
static void draw_grid(GuiRect origin, GuiRect clip, int side, u16 parent, const InvContainerDef *cd, int filter, int region) {
    InvTree *t = inv_screen_tree(side);
    const ItemDB *db = items();
    if (!t || !cd) return;
    float cell = V.L.cell;
    for (int c = 0; c < cd->comp_count; c++) {
        const InvCompDef *k = &cd->comp[c];
        GuiRect gr = { origin.x + (float)k->at_x * cell, origin.y + (float)k->at_y * cell, (float)k->w * cell, (float)k->h * cell };
        if (cd->comp_count > 1 && c > 0) gr.x += (float)k->at_x * gui_s(3);   // pouches apart
        gd_round(gr, gui_s(TH->radius), gui_rgba_mul(TH->slot_bg, 0.9f));
        for (int y = 0; y < k->h; y++)
            for (int x = 0; x < k->w; x++) {
                GuiRect cr = { gr.x + (float)x * cell, gr.y + (float)y * cell, cell, cell };
                gd_round_outline(gui_rect_inset(cr, 1), gui_s(2), 1, gui_rgba_mul(TH->slot_border, 0.55f));
                InvTarget pt = { IT_CELL, (u8)side, (u8)c, 0, parent, 0, (i16)x, (i16)y };
                const InvItem *under = inv_at(t, db, parent, c, x, y);
                pt.uid = under ? under->uid : 0;
                if (gui_rect_contains(clip, cr.x + cell * 0.5f, cr.y + cell * 0.5f)) pad_add(pt, cr, region);
            }
        InvTarget gt = { IT_CELL, (u8)side, (u8)c, 0, parent, 0, 0, 0 };
        hit_add(HK_GRID, gr, clip, gt, cell);
        gd_clip_push(clip);
        for (int i = 0; i < INV_MAX_ITEMS; i++) {
            const InvItem *it = &t->items[i];
            if (!inv_alive(it) || it->parent != parent || it->loc != c) continue;
            int w, h;
            inv_item_size(db, it, &w, &h);
            GuiRect ir = { gr.x + (float)it->x * cell, gr.y + (float)it->y * cell, (float)w * cell, (float)h * cell };
            InvTarget me = { IT_CELL, (u8)side, (u8)c, 0, parent, it->uid, it->x, it->y };
            draw_item_tile(ir, it, side, filtered_out(filter, item_def(db, it->s.item)), me);
            if (V.hover.kind == IT_CELL && V.hover.uid == it->uid && V.hover.side == side && V.dd.state == IDD_IDLE && !V.ctx.open)
                tooltip_for(gui_idi("inv_item", side * 65536 + it->uid), side, it);
        }
        // The editor's fit map: every cell where its item fits.
        u16 fit = inv_editor_fit_item();
        if (fit) {
            InvItem probe;
            memset(&probe, 0, sizeof(probe));
            probe.s.item = fit;
            probe.s.count = 1;
            for (int y = 0; y < k->h; y++)
                for (int x = 0; x < k->w; x++) {
                    InvPlace pl = { parent, (u8)c, (u8)x, (u8)y, 0 };
                    if (inv_check_place(t, db, &probe, pl, 0, NULL) == IIR_OK)
                        gd_circle(gr.x + ((float)x + 0.5f) * cell, gr.y + ((float)y + 0.5f) * cell, cell * 0.12f, gui_rgba_mul(TH->ok, 0.9f));
                }
        }
        gd_clip_pop();
        draw_footprint(&V.hits[V.hit_n - 1]);
    }
}

static void panel_title(GuiRect r, const char *name, float kg, int used, int cap, int *filter, GuiId fid) {
    GuiText t = { gui_s(TH->t_label), GW_SEMIBOLD, TH->text, GUI_LEFT, TH->tracking_caps, true, false, r.w * 0.38f };
    gd_text(&t, r.x, r.y + gui_s(4), name);
    char b[64];
    snprintf(b, sizeof(b), "%.1f kg   %d/%d", kg, used, cap);
    GuiText s = { gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT, 0, false, false, 0 };
    float fw = gui_s(46);
    gd_text(&s, r.x + r.w - 2 * fw - gui_s(12), r.y + gui_s(5), b);   // left of the Sort and filter chips
    if (filter) {
        static const char *names[5] = { "All", "Guns", "Ammo", "Meds", "Gear" };
        GuiRect fr = { r.x + r.w - fw, r.y + gui_s(2), fw, gui_s(20) };
        GuiInteract in = gui_interact(fid, fr, 0);
        gd_round(fr, gui_s(TH->radius), in.hot ? TH->hover : gui_rgba_mul(TH->slot_bg, 0.8f));
        gw_label(fr, names[*filter % 5], gui_s(TH->t_tiny + 1), GW_MEDIUM, *filter ? TH->accent : TH->text_faint, GUI_CENTER);
        if (in.clicked) *filter = (*filter + 1) % 5;
        if (in.right_clicked) *filter = 0;
        if (in.hovered) gui_tooltip(fid, "Filter (click to cycle)");
    }
}

static void sort_button(GuiRect r, int side, u16 parent, GuiId id) {
    GuiRect br = { r.x + r.w - gui_s(46) * 2 - gui_s(6), r.y + gui_s(2), gui_s(46), gui_s(20) };
    GuiInteract in = gui_interact(id, br, 0);
    gd_round(br, gui_s(TH->radius), in.hot ? TH->hover : gui_rgba_mul(TH->slot_bg, 0.8f));
    gw_label(br, "Sort", gui_s(TH->t_tiny + 1), GW_MEDIUM, TH->text_faint, GUI_CENTER);
    static int key;
    if (in.hovered) gui_tooltip(id, key == 0 ? "Sort by type" : key == 1 ? "Sort by weight" : "Sort by size");
    if (in.clicked) {
        InvIntent s;
        memset(&s, 0, sizeof(s));
        s.type = II_SORT;
        s.dst_side = (u8)side;
        s.dst = parent;
        s.aux = (u8)key;
        inv_screen_send(s);
        key = (key + 1) % 3;
    }
}

// ------------------------------------------------------------------ columns

typedef struct PanelSrc { const char *name; u16 parent; const InvContainerDef *cd; } PanelSrc;

// The player's grids in the quick-move order (pockets always; rig / backpack / secure when worn).
static int player_panels(PanelSrc *out) {
    Player *p = game_local_player();
    const ItemDB *db = items();
    if (!p || !db) return 0;
    InvOrder o = inv_get_order();
    int n = 0;
    bool done[IQ_COUNT] = { false };
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < (pass ? IQ_COUNT : o.n); i++) {
            int q = pass ? i : o.t[i];
            if (done[q]) continue;
            done[q] = true;
            u16 parent;
            if (!inv_quick_parent(&p->inv.t, (InvQuickTarget)q, &parent)) continue;
            const InvContainerDef *cd = inv_parent_def(&p->inv.t, db, parent);
            if (!cd) continue;
            static const char *names[IQ_COUNT] = { "Tactical rig", "Backpack", "Pockets", "Secure container" };
            out[n].name = parent ? item_def(db, inv_get(&p->inv.t, parent)->s.item)->name : names[q];
            if (q == IQ_POCKETS) out[n].name = "Pockets";
            out[n].parent = parent;
            out[n].cd = cd;
            n++;
        }
    return n;
}

static void draw_left(const PanelSrc *ps, int n) {
    InvLayout *L = &V.L;
    Player *p = game_local_player();
    const ItemDB *db = items();
    gw_panel(gui_rect(L->left.x - gui_s(8), L->left.y - gui_s(8), L->left.w + gui_s(16), L->left.h + gui_s(16)), true);
    gui_block_rect(L->left);
    gw_scroll_begin(&V.left_scroll, gui_id("inv_left_scroll"), L->left);
    float oy = gw_scroll_y(&V.left_scroll) - L->left.y;
    for (int i = 0; i < n; i++) {
        GuiRect pr = L->panel[i];
        pr.y += oy;
        float kg = inv_parent_weight(&p->inv.t, db, ps[i].parent);
        panel_title(gui_rect(pr.x + L->pad, pr.y, pr.w - 2 * L->pad, L->title_h), ps[i].name, kg, inv_cells_used(&p->inv.t, db, ps[i].parent), ps[i].cd->capacity,
                    &V.filter[i], gui_idi("inv_filter", i));
        sort_button(gui_rect(pr.x + L->pad, pr.y, pr.w - 2 * L->pad, L->title_h), INV_SIDE_SELF, ps[i].parent, gui_idi("inv_sort", i));
        GuiRect origin = { pr.x + L->pad, pr.y + L->title_h, 0, 0 };
        draw_grid(origin, L->left, INV_SIDE_SELF, ps[i].parent, ps[i].cd, V.filter[i], 0);
    }
    gw_scroll_end(&V.left_scroll, L->left_content_h + gui_s(8));
}

static void draw_equip(void) {
    InvLayout *L = &V.L;
    Player *p = game_local_player();
    const ItemDB *db = items();
    InvView v = view();
    for (int s = 0; s < EQ_COUNT; s++) {
        GuiRect r = L->equip[s];
        InvTarget me = { IT_EQUIP, INV_SIDE_SELF, 0, (i8)s, 0, 0, 0, 0 };
        const InvItem *it = inv_equipped_c(&p->inv.t, s);
        me.uid = it ? it->uid : 0;
        hit_add(HK_EQUIP, r, r, me, 0);
        pad_add(me, r, 1);
        bool hover = V.hover.kind == IT_EQUIP && V.hover.index == s;
        u32 border = TH->slot_border, bg = gui_rgba_mul(TH->slot_bg, 0.85f);
        if (V.dd.state == IDD_DRAGGING && hover) {
            InvDDOut o = idd_drop_on(&V.dd, me, &v);
            bool ok = o.action == IDA_INTENT && dry_run(&o.in) == IIR_OK;
            border = ok ? TH->ok : TH->danger;
            bg = gui_rgba_lerp(bg, border, 0.2f);
        } else if (V.dd.state == IDD_DRAGGING) {
            const InvItem *c = item_of(V.dd.side, V.dd.uid);
            if (c && V.dd.src.kind != IT_HOT && V.dd.src.kind != IT_ATTACH && inv_equip_accepts(s, item_def(db, c->s.item))) border = gui_rgba_mul(TH->accent, 0.8f);
        }
        float sh = shake_amount(me);
        if (sh > 0) r.x += sinf(sh * 40.0f) * sh * gui_s(5);
        gd_round_border(r, gui_s(TH->radius), 1, bg, border);
        GuiText lab = { gui_s(TH->t_tiny), GW_MEDIUM, TH->text_faint, GUI_LEFT, TH->tracking_caps, true, false, r.w - gui_s(8) };
        gd_text(&lab, r.x + gui_s(5), r.y + gui_s(3), short_slot_name(s));
        if (it) {
            GuiRect ir = { r.x + gui_s(4), r.y + gui_s(16), r.w - gui_s(8), r.h - gui_s(20) };
            draw_item_tile(ir, it, INV_SIDE_SELF, false, me);
            if (hover && V.dd.state == IDD_IDLE && !V.ctx.open) tooltip_for(gui_idi("inv_eq", s), INV_SIDE_SELF, it);
        }
        if (gui_nav_mode() && V.pad_cur < V.pad_n && V.pad_t[V.pad_cur].kind == IT_EQUIP && V.pad_t[V.pad_cur].index == s) gw_focus_ring(r, gui_s(TH->radius), 1.0f);
    }
    // Discard zone.
    GuiRect d = L->discard;
    InvTarget dt = { IT_DISCARD, 0, 0, 0, 0, 0, 0, 0 };
    hit_add(HK_DISCARD, d, d, dt, 0);
    bool over = V.dd.state == IDD_DRAGGING && V.hover.kind == IT_DISCARD;
    float a = V.dd.state == IDD_DRAGGING ? 1.0f : 0.45f;
    gd_alpha_push(a);
    gd_round_border(d, d.h * 0.5f, 1, over ? gui_rgba_mul(TH->danger, 0.35f) : gui_rgba_mul(TH->slot_bg, 0.7f), over ? TH->danger : TH->slot_border);
    gw_label_caps(d, "Discard", gui_s(TH->t_label), over ? TH->text : TH->text_faint, GUI_CENTER);
    gd_alpha_pop();
}

static void draw_hotbar(void) {
    InvLayout *L = &V.L;
    Game *g = g_();
    Player *p = game_local_player();
    const ItemDB *db = items();
    InvView v = view();
    gw_panel(gui_rect(L->right.x - gui_s(8), L->right.y - gui_s(8), L->right.w + gui_s(16), L->right.h + gui_s(16)), true);
    gui_block_rect(L->right);
    GuiText t = { gui_s(TH->t_label), GW_SEMIBOLD, TH->text, GUI_LEFT, TH->tracking_caps, true, false, 0 };
    gd_text(&t, L->right.x, L->right.y + gui_s(4), "Hotbar");
    for (int k = 0; k < INV_HOTBAR; k++) {
        GuiRect r = L->hot[k];
        const InvSlot *s = inv_hot_c(&p->inv, k);
        InvTarget me = { IT_HOT, INV_SIDE_SELF, 0, (i8)k, 0, s->item ? p->inv.quick[k] : 0, 0, 0 };
        hit_add(HK_HOT, r, r, me, 0);
        pad_add(me, r, 2);
        bool sel = k == g->hotbar_slot;
        bool hover = V.hover.kind == IT_HOT && V.hover.index == k;
        u32 border = sel ? TH->select : TH->slot_border, bg = gui_rgba_mul(TH->slot_bg, 0.9f);
        if (V.dd.state == IDD_DRAGGING && hover) {
            InvDDOut o = idd_drop_on(&V.dd, me, &v);
            bool ok = o.action == IDA_INTENT;
            border = ok ? TH->ok : TH->danger;
            bg = gui_rgba_lerp(bg, border, 0.2f);
        } else if (hover) bg = gui_rgba_lerp(bg, TH->slot_hi, 0.6f);
        float sh = shake_amount(me);
        if (sh > 0) r.x += sinf(sh * 40.0f) * sh * gui_s(5);
        gd_alpha_push(V.dd.state == IDD_DRAGGING && V.dd.src.kind == IT_HOT && V.dd.src.index == k ? 0.4f : 1.0f);
        gd_round_border(r, gui_s(TH->radius), sel ? fmaxf(1.0f, gui_s(1.5f)) : 1.0f, bg, border);
        char key[4];
        snprintf(key, sizeof(key), "%d", k + 1);
        GuiText kt = { gui_s(L->hot_compact ? TH->t_tiny + 1 : TH->t_body), GW_SEMIBOLD, sel ? TH->select : TH->text_faint, GUI_LEFT, 0, false, false, 0 };
        gd_text(&kt, r.x + gui_s(6), r.y + gui_s(4), key);
        if (s->item) {
            const ItemDef *d = item_def(db, s->item);
            const InvItem *it = inv_get_c(&p->inv.t, p->inv.quick[k]);
            bool rot = false;   // tiles show the item lying flat
            if (L->hot_compact) {
                inv_draw_item_image(gui_rect_inset(r, r.w * 0.16f), db, s, rot, 1.0f);
            } else {
                GuiRect ir = { r.x + gui_s(26), r.y + gui_s(6), r.w * 0.55f, r.h - gui_s(12) };
                inv_draw_item_image(ir, db, s, rot, 1.0f);
                GuiText nm = { gui_s(TH->t_small + 1), GW_SEMIBOLD, TH->text, GUI_RIGHT, 0, false, false, r.w * 0.4f };
                gd_text(&nm, r.x + r.w - gui_s(10), r.y + gui_s(8), revive_item_name(db, s->item, s->ammo));
                char b[48] = "";
                const WeaponDef *wd = item_weapon(db, s->item);
                if (wd && wd->kind != WPN_MELEE && wd->mag_size > 0) {
                    WeaponDef eff;
                    attach_weapon_effective(db, wd, s->att, &eff);
                    int mag = k == p->sim.wpn.slot ? p->sim.wpn.mag : s->ammo;
                    snprintf(b, sizeof(b), "%d / %d   \xC2\xB7   %d", mag, eff.mag_size, wd->ammo_item ? inv_count(&p->inv, wd->ammo_item) : 0);
                } else if (s->count > 1 || d->max_stack > 1) {
                    snprintf(b, sizeof(b), "x%d", inv_count(&p->inv, s->item));
                }
                GuiText at = { gui_s(TH->t_small), GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, false, 0 };
                gd_text(&at, r.x + r.w - gui_s(10), r.y + r.h - gui_s(24), b);
            }
            if (hover && V.dd.state == IDD_IDLE && !V.ctx.open && it) tooltip_for(gui_idi("inv_hot", k), INV_SIDE_SELF, it);
        } else if (!L->hot_compact) {
            gw_label(r, "Drag an item here", gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_CENTER);
        }
        gd_alpha_pop();
        if (gui_nav_mode() && V.pad_cur < V.pad_n && V.pad_t[V.pad_cur].kind == IT_HOT && V.pad_t[V.pad_cur].index == k) gw_focus_ring(r, gui_s(TH->radius), 1.0f);
    }
}

static const char *loot_name(void) {
    Game *g = g_();
    World *w = g->world;
    EntityId e = game_loot_ent();
    if (!ent_valid(w, e)) return "Container";
    int i = (int)ENT_INDEX(e);
    static char b[64];
    switch (w->type[i]) {
    case ET_CORPSE: snprintf(b, sizeof(b), "Body: %s", w->names[i][0] ? w->names[i] : "unknown"); return b;
    case ET_VEHICLE: return "Trunk";
    default: break;
    }
    InvTree *t = game_loot_tree();
    const InvContainerDef *cd = t ? inv_root_def(t, items()) : NULL;
    if (cd && !strcmp(cd->id, "cabinet")) return "Cabinet";
    return "Crate";
}

static void draw_loot(void) {
    InvLayout *L = &V.L;
    InvTree *t = game_loot_tree();
    const ItemDB *db = items();
    GuiRect r = L->loot;
    if (!t) {
        gw_label(r, "Opening\xE2\x80\xA6", gui_s(TH->t_body), GW_REGULAR, TH->text_faint, GUI_CENTER);
        return;
    }
    const InvContainerDef *cd = inv_root_def(t, db);
    GuiRect tr = { r.x, r.y, r.w, L->title_h };
    panel_title(tr, loot_name(), inv_tree_weight(t, db), inv_cells_used(t, db, 0), cd->capacity, &V.filter[INV_UI_MAX_PANELS], gui_id("inv_filter_loot"));
    GuiRect ta = { r.x, r.y + r.h - gui_s(TH->button_h + 4), gui_s(120), gui_s(TH->button_h + 4) };
    if (gw_button(gui_id("inv_take_all"), ta, "Take all", GB_PRIMARY, true)) {
        InvIntent in;
        memset(&in, 0, sizeof(in));
        in.type = II_TAKE_ALL;
        in.src_side = in.dst_side = INV_SIDE_LOOT;
        inv_screen_send(in);
    }
    sort_button(tr, INV_SIDE_LOOT, 0, gui_id("inv_sort_loot"));
    GuiRect clip = { r.x, r.y + L->title_h, r.w, r.h - L->title_h - ta.h - L->gap };
    // Tall containers (a stash, a corpse's pockets and packs open in windows) scroll.
    int cols, rows;
    inv_container_extent(cd, &cols, &rows);
    gw_scroll_begin(&V.loot_scroll, gui_id("inv_loot_scroll"), clip);
    float y = gw_scroll_y(&V.loot_scroll), y0 = y;
    if (cd->equip) {
        // A body (corpse): its equipment as tiles, the worn containers' grids, then the pockets.
        float q = 1.6f * L->cell, x = r.x;
        bool any = false;
        for (int sl = 0; sl < EQ_COUNT; sl++) {
            const InvItem *it = inv_equipped_c(t, sl);
            if (!it) continue;
            any = true;
            if (x + q > r.x + r.w) { x = r.x; y += q + L->gap; }
            GuiRect er = { x, y, q, q };
            InvTarget me = { IT_EQUIP, INV_SIDE_LOOT, 0, (i8)sl, 0, it->uid, 0, 0 };
            hit_add(HK_EQUIP, er, clip, me, 0);
            if (gui_rect_contains(clip, er.x + q * 0.5f, er.y + q * 0.5f)) pad_add(me, er, 3);
            draw_item_tile(er, it, INV_SIDE_LOOT, false, me);
            if (V.hover.kind == IT_EQUIP && V.hover.side == INV_SIDE_LOOT && V.hover.index == sl && V.dd.state == IDD_IDLE && !V.ctx.open)
                tooltip_for(gui_idi("inv_leq", sl), INV_SIDE_LOOT, it);
            x += q + L->gap;
        }
        if (any) y += q + L->gap;
        static const int worn[3] = { EQ_RIG, EQ_BACKPACK, EQ_SECURE };
        for (int k = 0; k < 3; k++) {
            const InvItem *c = inv_equipped_c(t, worn[k]);
            const InvContainerDef *ccd = c ? item_container(db, c->s.item) : NULL;
            if (!ccd) continue;
            int cc, cr;
            inv_container_extent(ccd, &cc, &cr);
            GuiText st = { gui_s(TH->t_small), GW_SEMIBOLD, TH->text_dim, GUI_LEFT, TH->tracking_caps, true, false, r.w };
            gd_text(&st, r.x, y + gui_s(2), item_def(db, c->s.item)->name);
            y += gui_s(20);
            draw_grid(gui_rect(r.x, y, 0, 0), clip, INV_SIDE_LOOT, c->uid, ccd, V.filter[INV_UI_MAX_PANELS], 3);
            y += (float)cr * L->cell + L->gap;
        }
        GuiText st = { gui_s(TH->t_small), GW_SEMIBOLD, TH->text_dim, GUI_LEFT, TH->tracking_caps, true, false, r.w };
        gd_text(&st, r.x, y + gui_s(2), "Pockets");
        y += gui_s(20);
    }
    draw_grid(gui_rect(r.x, y, 0, 0), clip, INV_SIDE_LOOT, 0, cd, V.filter[INV_UI_MAX_PANELS], 3);
    y += (float)rows * L->cell;
    gw_scroll_end(&V.loot_scroll, y - y0 + gui_s(4));
}

// ------------------------------------------------------------------ windows (nested containers, inspect)

static GuiRect window_frame(vec2 *pos, bool *dragging, vec2 *grab, float w, float h, const char *title, GuiId id, bool *close) {
    GuiRect r = { pos->x, pos->y, w, h };
    r.x = clampf(r.x, 0, gui_w() - w);
    r.y = clampf(r.y, 0, gui_h() - h);
    *pos = v2(r.x, r.y);
    int layer = gd_layer();
    gd_set_layer(GL_OVERLAY);
    gw_panel(r, true);
    gui_block_rect(r);
    hit_add(HK_BLOCK, r, r, (InvTarget){ IT_PANEL, 0, 0, 0, 0, 0, 0, 0 }, 0);
    GuiRect bar = { r.x, r.y, r.w, gui_s(30) };
    GuiInteract in = gui_interact(id, bar, 0);
    if (in.pressed && V.dd.state == IDD_IDLE) { *dragging = true; *grab = v2(gui_mouse().x - r.x, gui_mouse().y - r.y); }
    if (*dragging) {
        if (!gui_mouse_down(0)) *dragging = false;
        else *pos = v2(gui_mouse().x - grab->x, gui_mouse().y - grab->y);
    }
    GuiText t = { gui_s(TH->t_label), GW_SEMIBOLD, TH->text, GUI_LEFT, TH->tracking_caps, true, false, r.w - gui_s(50) };
    gd_text(&t, r.x + gui_s(12), r.y + gui_s(8), title);
    GuiRect x = { r.x + r.w - gui_s(28), r.y + gui_s(4), gui_s(22), gui_s(22) };
    GuiInteract xi = gui_interact(id + 1, x, 0);
    gd_round(x, gui_s(TH->radius), xi.hot ? TH->hover : 0);
    gw_label(x, "\xC3\x97", gui_s(TH->t_body), GW_MEDIUM, xi.hot ? TH->text : TH->text_faint, GUI_CENTER);
    *close = xi.clicked;
    gd_set_layer(layer);
    return gui_rect(r.x + gui_s(12), r.y + gui_s(34), r.w - gui_s(24), r.h - gui_s(44));
}

static void draw_windows(void) {
    const ItemDB *db = items();
    for (int i = 0; i < MAX_WINS; i++) {
        Win *w = &V.win[i];
        if (!w->open) continue;
        const InvItem *it = item_of(w->side, w->uid);
        const InvContainerDef *cd = it ? item_container(db, it->s.item) : NULL;
        if (!cd) { w->open = false; continue; }
        int cols, rows;
        inv_container_extent(cd, &cols, &rows);
        float cw = (float)cols * V.L.cell + gui_s(24) + (float)cols * gui_s(3), ch = (float)rows * V.L.cell + gui_s(48);
        bool close = false;
        GuiRect c = window_frame(&w->pos, &w->dragging, &w->grab, fmaxf(cw, gui_s(220)), ch, item_def(db, it->s.item)->name, gui_idi("inv_win", i * 2), &close);
        int layer = gd_layer();
        gd_set_layer(GL_OVERLAY);
        draw_grid(gui_rect(c.x, c.y, 0, 0), gui_rect(c.x - 1, c.y - 1, c.w + 2, c.h + 2), w->side, w->uid, cd, 0, 4 + i);
        gd_set_layer(layer);
        if (close) w->open = false;
    }
}

static void stat_line(GuiCursor *c, const char *label, const char *value, float delta, bool higher_better) {
    GuiRect r = gc_row(c, gui_s(19));
    gw_label(gui_rect(r.x, r.y, r.w * 0.55f, r.h), label, gui_s(TH->t_small + 1), GW_REGULAR, TH->text_dim, GUI_LEFT);
    char buf[64];
    u32 col = TH->text;
    if (fabsf(delta) >= 0.5f) {
        col = (higher_better ? delta > 0 : delta < 0) ? TH->ok : TH->danger;
        snprintf(buf, sizeof(buf), "%s  %+.0f%%", value, delta);
    } else {
        snprintf(buf, sizeof(buf), "%s", value);
    }
    gw_label(gui_rect(r.x + r.w * 0.45f, r.y, r.w * 0.55f, r.h), buf, gui_s(TH->t_small + 1), GW_MEDIUM, col, GUI_RIGHT);
}

static float pct(float now, float base) { return base != 0.0f ? 100.0f * (now - base) / fabsf(base) : 0.0f; }

static void item_stats(GuiCursor *c, const InvItem *it, int side) {
    const ItemDB *db = items();
    const InvSlot *s = &it->s;
    const ItemDef *d = item_def(db, s->item);
    const WeaponDef *wd = item_weapon(db, s->item);
    char v[64];
    int w, h, bw, bh;
    inv_item_size(db, it, &w, &h);
    inv_base_size(db, s, 0, &bw, &bh);
    if (bw != DG_MAX(1, d->grid_w) || bh != DG_MAX(1, d->grid_h) || (it->flags & INVF_FOLDED)) snprintf(v, sizeof(v), "%dx%d (base %dx%d)", w, h, d->grid_w, d->grid_h);
    else snprintf(v, sizeof(v), "%dx%d", w, h);
    stat_line(c, "Footprint", v, 0, true);
    snprintf(v, sizeof(v), "%.2f kg", inv_item_weight(inv_screen_tree(side), db, it));
    stat_line(c, "Weight", v, 0, true);
    if (wd) {
        WeaponDef eff;
        attach_weapon_effective(db, wd, s->att, &eff);
        if (wd->kind == WPN_MELEE) {
            snprintf(v, sizeof(v), "%.0f", eff.damage);
            stat_line(c, "Damage", v, 0, true);
            snprintf(v, sizeof(v), "%.1f m", eff.reach);
            stat_line(c, "Reach", v, 0, true);
            return;
        }
        snprintf(v, sizeof(v), "%.0f%s", eff.damage, eff.pellets > 1 ? " x pellets" : "");
        stat_line(c, "Damage", v, pct(eff.damage, wd->damage), true);
        snprintf(v, sizeof(v), "%.0f rpm", eff.fire_rate * 60.0f);
        stat_line(c, "Fire rate", v, pct(eff.fire_rate, wd->fire_rate), true);
        snprintf(v, sizeof(v), "%d / %d", s->ammo, eff.mag_size);
        stat_line(c, "Magazine", v, pct((float)eff.mag_size, (float)wd->mag_size), true);
        snprintf(v, sizeof(v), "%.2f", eff.recoil.v_per);
        stat_line(c, "Vertical recoil", v, pct(eff.recoil.v_per, wd->recoil.v_per), false);
        snprintf(v, sizeof(v), "%.2f s", eff.recoil.ads_time);
        stat_line(c, "Aim time", v, pct(eff.recoil.ads_time, wd->recoil.ads_time), false);
        return;
    }
    if (d->kind == ITEM_CONSUMABLE) {
        if (d->heal > 0) { snprintf(v, sizeof(v), "+%.0f", d->heal); stat_line(c, "Health", v, 0, true); }
        if (d->armor > 0) { snprintf(v, sizeof(v), "+%.0f", d->armor); stat_line(c, "Armour", v, 0, true); }
        snprintf(v, sizeof(v), "%.1f s", d->use_time);
        stat_line(c, "Use time", v, 0, true);
    }
    const InvContainerDef *cd = item_container(db, s->item);
    if (cd) {
        snprintf(v, sizeof(v), "%d cells, %d compartment%s", cd->capacity, cd->comp_count, cd->comp_count > 1 ? "s" : "");
        stat_line(c, "Holds", v, 0, true);
    }
    if (d->durability) { snprintf(v, sizeof(v), "%d / %d", it->dur, d->durability); stat_line(c, "Durability", v, 0, true); }
}

static void draw_inspect(void) {
    if (!V.insp.open) return;
    const ItemDB *db = items();
    const InvItem *it = item_of(V.insp.side, V.insp.uid);
    if (!it) { V.insp.open = false; return; }
    const ItemDef *d = item_def(db, it->s.item);
    const WeaponDef *wd = item_weapon(db, it->s.item);
    bool close = false;
    float ww = gui_s(440), wh = gui_s(wd ? 560 : 460);
    char title[80];
    snprintf(title, sizeof(title), "Inspect: %s", revive_item_name(db, it->s.item, it->s.ammo));
    GuiRect c = window_frame(&V.insp.pos, &V.insp.dragging, &V.insp.grab, ww, wh, title, gui_id("inv_insp"), &close);
    if (close) {
        V.insp.open = false;
        return;
    }
    int layer = gd_layer();
    gd_set_layer(GL_OVERLAY);
    GuiRect vr = { c.x, c.y, c.w, c.w * 0.5f };
    if (V.insp.cam_item != it->s.item) {
        item_view_default_cam(db, it->s.item, &V.insp.cam);
        V.insp.cam.zoom = wd ? 1.2f : 0.85f;
        V.insp.cam_item = it->s.item;
    }
    GuiInteract vin = gui_interact(gui_id("inv_insp_view"), vr, 0);
    if (vin.pressed && V.dd.state == IDD_IDLE) { V.insp.rotating = true; V.insp.rot_from = gui_mouse(); }
    if (V.insp.rotating) {
        if (!gui_mouse_down(0)) V.insp.rotating = false;
        else {
            vec2 m = gui_mouse();
            V.insp.cam.yaw += (m.x - V.insp.rot_from.x) * 0.01f;
            V.insp.cam.pitch = clampf(V.insp.cam.pitch + (m.y - V.insp.rot_from.y) * 0.008f, -1.3f, 1.3f);
            V.insp.rot_from = m;
            V.insp.last_touch = gui_time();
        }
    }
    if (vin.hovered && gui_wheel() != 0) {
        V.insp.cam.zoom = clampf(V.insp.cam.zoom * (gui_wheel() > 0 ? 1.12f : 1.0f / 1.12f), 0.6f, 3.0f);
        gui_wheel_consume();
        V.insp.last_touch = gui_time();
    }
    if (gui_time() - V.insp.last_touch > 2.5) V.insp.cam.yaw += gui_dt() * 0.35f;
    // Highlight the socket a dragged attachment would go into.
    int hl = -1;
    if (V.dd.state == IDD_DRAGGING && wd) {
        const InvItem *src = item_of(V.dd.side, V.dd.uid);
        int aslot;
        if (src && V.dd.src.kind != IT_ATTACH && inv_attach_check(db, &it->s, src->s.item, &aslot) == IIR_OK) hl = aslot;
    }
    static RenderUi3dPart parts[ITEM_VIEW_MAX_PARTS];
    vec3 mn, mx;
    int n = item_view_parts(db, it->s.item, it->s.att, hl, parts, ITEM_VIEW_MAX_PARTS, &mn, &mx);
    if (!V.insp.tex) V.insp.tex = render_ui3d_target_create(PREVIEW_W, PREVIEW_H);
    RenderUi3dView rv;
    memset(&rv, 0, sizeof(rv));
    rv.target = V.insp.tex;
    item_view_matrices(&V.insp.cam, mn, mx, vr.w / fmaxf(1.0f, vr.h), &rv.view, &rv.proj);
    rv.parts = parts;
    rv.part_count = n;
    render_ui3d(&rv);
    gd_image(vr, V.insp.tex, v4(0, 0, 1, 1), WHITE);
    GuiCursor cur = gc_make(gui_rect(c.x, vr.y + vr.h + gui_s(6), c.w, 10000), gui_s(2));
    // Attachment slots (drag attachments in / out; the footprint line updates live).
    if (wd && wd->kind != WPN_MELEE) {
        float gap = gui_s(6), cw = (c.w - gap * (float)(ATT_SLOTS - 1)) / (float)ATT_SLOTS;
        GuiRect row = gc_row(&cur, cw + gui_s(16));
        int col = 0;
        InvView v = view();
        for (int k = 0; k < ATT_SLOTS; k++) {
            if (!wd->att_slots[k].present) continue;
            GuiRect chip = { row.x + (cw + gap) * (float)col++, row.y, cw, cw };
            InvTarget me = { IT_ATTACH, V.insp.side, 0, (i8)k, 0, it->uid, 0, 0 };
            hit_add(HK_ATTACH, chip, chip, me, 0);
            pad_add(me, chip, 6);
            GuiSlotView sv;
            InvSlot fake;
            memset(&fake, 0, sizeof(fake));
            fake.item = item_for_attachment(db, it->s.att[k]);
            fake.count = fake.item ? 1 : 0;
            inventory_slot_view(&fake, db, -1, &sv);
            bool hover = V.hover.kind == IT_ATTACH && V.hover.index == k;
            sv.hot = hover;
            if (V.dd.state == IDD_DRAGGING && hover) {
                InvDDOut o = idd_drop_on(&V.dd, me, &v);
                sv.drop_state = o.action == IDA_INTENT && dry_run(&o.in) == IIR_OK ? 1 : 2;
            }
            sv.shake = shake_amount(me);
            if (V.dd.state == IDD_DRAGGING && V.dd.src.kind == IT_ATTACH && V.dd.src.index == k) sv.dim = true;
            gw_slot(chip, &sv);
            gw_label_caps(gui_rect(chip.x, chip.y + chip.h + gui_s(1), chip.w, gui_s(14)), attach_slot_name(k), gui_s(TH->t_tiny), hover ? TH->text : TH->text_faint, GUI_CENTER);
            if (hover && fake.item && V.dd.state == IDD_IDLE) gui_tooltip(gui_idi("inv_att", k), item_def(db, fake.item)->name);
        }
    }
    GuiText kind = { gui_s(TH->t_label), GW_SEMIBOLD, type_color(d), GUI_LEFT, TH->tracking_caps, true, false, 0 };
    GuiRect kr = gc_row(&cur, gui_s(18));
    gd_text(&kind, kr.x, kr.y, item_kind_name(d->kind));
    GuiText desc = { gui_s(TH->t_small + 1), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, false, 0 };
    float dh = gd_text_wrap(&desc, cur.area.x, cur.y, cur.area.w, d->desc, true);
    cur.y += dh + gui_s(8);
    gd_clip_push(gui_rect(c.x, cur.y, c.w, c.y + c.h - cur.y));
    item_stats(&cur, it, V.insp.side);
    gd_clip_pop();
    gd_set_layer(layer);
}

// ------------------------------------------------------------------ context menu / split dialog

static void draw_context(void) {
    if (!V.ctx.open) return;
    const ItemDB *db = items();
    InvTarget at = V.ctx.at;
    int side = at.kind == IT_HOT ? INV_SIDE_SELF : at.side;
    const InvItem *it = item_of(side, at.uid);
    if (!it) { V.ctx.open = false; return; }
    const ItemDef *d = item_def(db, it->s.item);
    const WeaponDef *wd = item_weapon(db, it->s.item);
    Player *p = game_local_player();
    typedef struct Entry { const char *label; int type; int aux; int count; } Entry;
    Entry e[14];
    int n = 0;
    bool equipped = side == INV_SIDE_SELF && !it->parent && it->loc >= INV_LOC_EQUIP;
    if (side == INV_SIDE_SELF && (d->kind == ITEM_CONSUMABLE || d->use_type != INV_USE_NONE) && !d->vehicle_use) e[n++] = (Entry){ "Use", II_USE, 0, 0 };
    if (d->equip_slot != EQ_NONE && !equipped) e[n++] = (Entry){ "Equip", II_EQUIP, 255, 0 };
    if (equipped) e[n++] = (Entry){ "Unequip", II_QUICK_MOVE, 0, 0 };
    if (item_container(db, it->s.item) && !equipped) e[n++] = (Entry){ "Open", -1, 0, 0 };
    e[n++] = (Entry){ "Inspect", -2, 0, 0 };
    if (d->fold_dw) e[n++] = (Entry){ (it->flags & INVF_FOLDED) ? "Unfold stock" : "Fold stock", II_FOLD, 0, 0 };
    if (wd && wd->kind != WPN_MELEE && it->s.ammo > 0) e[n++] = (Entry){ "Unload", II_UNLOAD, 0, 0 };
    if (wd) {
        bool any = false;
        for (int k = 0; k < ATT_SLOTS; k++) any |= it->s.att[k] != 0;
        if (any) e[n++] = (Entry){ "Remove attachments", II_UNMOUNT, 0, 0 };
    }
    if (d->rotatable && !equipped) e[n++] = (Entry){ "Rotate", II_ROTATE, 0, 0 };
    if (it->s.count > 1) e[n++] = (Entry){ "Split", II_SPLIT, 0, 0 };
    if (side == INV_SIDE_SELF && p && !bound_key(p, it->uid)) e[n++] = (Entry){ "Bind to hotbar", II_BIND, 0, 0 };
    if (side == INV_SIDE_LOOT || game_loot_tree()) e[n++] = (Entry){ side == INV_SIDE_LOOT ? "Take" : "Put in container", II_QUICK_MOVE, 0, 0 };
    e[n++] = (Entry){ it->s.count > 1 ? "Discard one" : "Discard", II_DISCARD, 0, it->s.count > 1 ? 1 : 0 };
    if (it->s.count > 1) e[n++] = (Entry){ "Discard all", II_DISCARD, 0, 0 };
    float ih = gui_s(28), w = gui_s(200);
    GuiRect r = { V.ctx.pos.x + gui_s(4), V.ctx.pos.y + gui_s(4), w, ih * (float)n + gui_s(33) };
    if (r.x + r.w > gui_w()) r.x = V.ctx.pos.x - r.w;
    if (r.y + r.h > gui_h()) r.y = gui_h() - r.h;
    int layer = gd_layer();
    gd_set_layer(GL_TOP);
    float a = gui_ease_out((float)(gui_time() - V.ctx.t) / 0.1f);
    gd_alpha_push(a);
    gd_shadow(r, gui_s(TH->radius), gui_s(16), TH->shadow);
    gd_round_border(r, gui_s(TH->radius), 1, TH->panel, TH->panel_border);
    gui_block_rect(r);
    gw_label_caps(gui_rect(r.x + gui_s(14), r.y + gui_s(6), r.w - gui_s(28), gui_s(20)), d->name, gui_s(TH->t_label), TH->text_faint, GUI_LEFT);
    for (int i = 0; i < n; i++) {
        GuiRect ir = { r.x + gui_s(5), r.y + gui_s(28) + ih * (float)i, r.w - gui_s(10), ih };
        GuiInteract in = gui_interact(gui_idi("inv_ctx", i), ir, GUI_FOCUSABLE | (i == 0 ? GUI_DEFAULT_FOCUS : 0));
        if (in.hot) gd_round(ir, gui_s(TH->radius), TH->hover);
        bool danger = e[i].type == II_DISCARD;
        gw_label(gui_rect(ir.x + gui_s(10), ir.y, ir.w - gui_s(20), ir.h), e[i].label, gui_s(TH->t_body - 1), GW_MEDIUM,
                 danger ? gui_rgba_lerp(TH->text_dim, TH->danger, 0.6f) : (in.hot ? TH->text : TH->text_dim), GUI_LEFT);
        if (!in.clicked) continue;
        V.ctx.open = false;
        InvIntent x;
        memset(&x, 0, sizeof(x));
        x.type = (u8)DG_MAX(0, e[i].type);
        x.src_side = x.dst_side = (u8)side;
        x.src = it->uid;
        x.aux = (u8)e[i].aux;
        x.count = (u16)e[i].count;
        if (e[i].type == -1) { open_window(side, it->uid); break; }
        if (e[i].type == -2) { open_inspect(side, it->uid); break; }
        if (e[i].type == II_EQUIP) x.dst_side = INV_SIDE_SELF;
        if (e[i].type == II_SPLIT) {
            V.split.open = true;
            V.split.max = it->s.count - 1;
            V.split.count = (float)(it->s.count / 2);
            x.dst = INV_AUTO;
            V.split.in = x;
            break;
        }
        if (e[i].type == II_BIND) {
            int k = 0;
            while (k < INV_HOTBAR - 1 && p->inv.quick[k] && inv_get(&p->inv.t, p->inv.quick[k])) k++;
            x.aux = (u8)k;
        }
        if (e[i].type == II_UNMOUNT) {
            for (int k = 0; k < ATT_SLOTS; k++)
                if (it->s.att[k]) { InvIntent u = x; u.aux = (u8)k; u.dst = INV_AUTO; inv_screen_send(u); }
            break;
        }
        if (e[i].type == II_DISCARD && !e[i].count) {
            InvDDOut o;
            memset(&o, 0, sizeof(o));
            o.action = DG_MAX(1, d->max_stack) <= 1 && (wd || item_container(db, it->s.item) || d->equip_slot != EQ_NONE) ? IDA_CONFIRM : IDA_INTENT;
            o.in = x;
            o.at = at;
            apply(&o);
            break;
        }
        inv_screen_send(x);
        break;
    }
    gui_focus_scope(r);
    gd_alpha_pop();
    gd_set_layer(layer);
    if (gui_nav(NAV_BACK)) { gui_nav_consume(NAV_BACK); V.ctx.open = false; }
    else if ((gui_mouse_pressed(0) || gui_mouse_pressed(1)) && !gui_rect_contains(r, gui_mouse().x, gui_mouse().y) && gui_time() - V.ctx.t > 0.05) V.ctx.open = false;
}

static void draw_split(void) {
    if (!V.split.open) return;
    GuiRect r = { gui_w() * 0.5f - gui_s(170), gui_h() * 0.5f - gui_s(80), gui_s(340), gui_s(160) };
    int layer = gd_layer();
    gd_set_layer(GL_TOP);
    gw_panel(r, true);
    gui_block_rect(r);
    gw_label_caps(gui_rect(r.x + gui_s(16), r.y + gui_s(12), r.w - gui_s(32), gui_s(20)), "Split stack", gui_s(TH->t_label), TH->text, GUI_LEFT);
    GuiRect sr = { r.x + gui_s(16), r.y + gui_s(48), r.w - gui_s(32), gui_s(30) };
    gw_slider(gui_id("inv_split_n"), sr, &V.split.count, 1, (float)DG_MAX(1, V.split.max), 1, "%.0f");
    GuiRect ok = { r.x + r.w - gui_s(16) - gui_s(110), r.y + r.h - gui_s(48), gui_s(110), gui_s(TH->button_h) };
    GuiRect no = { ok.x - gui_s(120), ok.y, gui_s(110), ok.h };
    bool accept = gw_button(gui_id("inv_split_ok"), ok, "Split", GB_PRIMARY, true) || gui_nav(NAV_ACCEPT);
    bool cancel = gw_button(gui_id("inv_split_no"), no, "Cancel", GB_SECONDARY, true) || gui_nav(NAV_BACK);
    gui_focus_scope(r);
    gd_set_layer(layer);
    if (accept) {
        gui_nav_consume(NAV_ACCEPT);
        V.split.open = false;
        InvIntent in = V.split.in;
        in.count = (u16)DG_CLAMP((int)lrintf(V.split.count), 1, DG_MAX(1, V.split.max));
        InvDDOut o;
        memset(&o, 0, sizeof(o));
        o.action = IDA_INTENT;
        o.in = in;
        apply(&o);
    } else if (cancel) {
        gui_nav_consume(NAV_BACK);
        V.split.open = false;
    }
}

// ------------------------------------------------------------------ hit testing / input

static InvTarget hit_test(vec2 m) {
    InvTarget none = { IT_OUTSIDE, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = V.hit_n - 1; i >= 0; i--) {
        const Hit *h = &V.hits[i];
        if (!gui_rect_contains(h->r, m.x, m.y) || !gui_rect_contains(h->clip, m.x, m.y)) continue;
        InvTarget t = h->t;
        switch (h->kind) {
        case HK_BLOCK: t.kind = IT_PANEL; return t;
        case HK_GRID: {
            t.x = (i16)floorf((m.x - h->r.x) / h->cell);
            t.y = (i16)floorf((m.y - h->r.y) / h->cell);
            InvTree *tr = inv_screen_tree(t.side);
            const InvItem *it = tr ? inv_at(tr, items(), t.parent, t.loc, t.x, t.y) : NULL;
            t.uid = it ? it->uid : 0;
            return t;
        }
        default: return t;
        }
    }
    InvLayout *L = &V.L;
    GuiRect body = { L->left.x, L->header.y, L->right.x + L->right.w - L->left.x, L->footer.y + L->footer.h - L->header.y };
    if (gui_rect_contains(body, m.x, m.y)) none.kind = IT_PANEL;
    return none;
}

static bool alt_down(void) { return V.alt_scripted || (SDL_GetModState() & SDL_KMOD_ALT) != 0; }

static void pad_input(void) {
    if (!gui_nav_mode() || !V.pad_n || V.ctx.open || V.split.open) return;
    if (V.pad_cur >= V.pad_n) V.pad_cur = 0;
    static const int dirs[4] = { NAV_LEFT, NAV_RIGHT, NAV_UP, NAV_DOWN };
    for (int d = 0; d < 4; d++) {
        if (!gui_nav(dirs[d])) continue;
        gui_nav_consume(dirs[d]);
        int nx = inv_nav_step(V.pad_r, V.pad_n, V.pad_cur, d);
        if (nx >= 0) V.pad_cur = nx;
    }
    // LB / RB: the first target of the previous / next region (grids, equipment, hotbar, loot, windows).
    for (int dir = -1; dir <= 1; dir += 2) {
        int nav = dir < 0 ? NAV_TAB_PREV : NAV_TAB_NEXT;
        if (!gui_nav(nav)) continue;
        gui_nav_consume(nav);
        int cur = V.pad_region[V.pad_cur];
        for (int step = 1; step <= 8; step++) {
            int want = (cur + dir * step + 8 * 8) % 8;
            int found = -1;
            for (int i = 0; i < V.pad_n && found < 0; i++) if (V.pad_region[i] == want) found = i;
            if (found >= 0) { V.pad_cur = found; break; }
        }
    }
    InvTarget ft = V.pad_t[V.pad_cur];
    InvView v = view();
    if (gui_nav(NAV_ACCEPT)) {
        gui_nav_consume(NAV_ACCEPT);
        InvDDOut o = idd_carry_accept(&V.dd, ft, &v, false);
        apply(&o);
    }
    if (gui_nav(NAV_ALT)) {
        gui_nav_consume(NAV_ALT);
        if (V.dd.carry) V.dd.rot ^= 1;
        else if (ft.uid) {
            V.ctx.open = true;
            V.ctx.at = ft;
            V.ctx.pos = v2(V.pad_r[V.pad_cur].x + V.pad_r[V.pad_cur].w, V.pad_r[V.pad_cur].y);
            V.ctx.t = gui_time();
            gui_set_focus(0);
        }
    }
    if (gui_nav(NAV_ALT2) && ft.uid && !V.dd.carry && ft.kind != IT_HOT && ft.kind != IT_ATTACH) {
        gui_nav_consume(NAV_ALT2);
        InvIntent q;
        memset(&q, 0, sizeof(q));
        q.type = II_QUICK_MOVE;
        q.src_side = ft.side;
        q.src = ft.uid;
        inv_screen_send(q);
    }
    if (gui_nav(NAV_BACK) && V.dd.carry) { gui_nav_consume(NAV_BACK); idd_reset(&V.dd); }
    if (ft.uid) V.sel = ft;
}

static void draw_ghost(void) {
    if (V.dd.state != IDD_DRAGGING) return;
    const ItemDB *db = items();
    InvSlot s;
    memset(&s, 0, sizeof(s));
    int w = 1, h = 1;
    if (V.dd.src.kind == IT_ATTACH) {
        const InvItem *wi = item_of(V.dd.side, V.dd.uid);
        if (wi) { s.item = item_for_attachment(db, wi->s.att[V.dd.src.index]); s.count = 1; }
    } else {
        const InvItem *it = item_of(V.dd.side, V.dd.uid);
        if (it) {
            s = it->s;
            int bw, bh;
            inv_base_size(db, &it->s, it->flags, &bw, &bh);
            w = V.dd.rot ? bh : bw;
            h = V.dd.rot ? bw : bh;
            if (V.dd.src.kind == IT_HOT) w = h = 1;
        }
    }
    if (!s.item) return;
    vec2 m = gui_mouse();
    if (V.dd.carry && V.pad_cur < V.pad_n) m = v2(V.pad_r[V.pad_cur].x + V.L.cell * 0.5f, V.pad_r[V.pad_cur].y + V.L.cell * 0.5f);
    int gx = 0, gy = 0;
    if (V.dd.src.kind == IT_CELL) {
        gx = V.dd.rot ? V.dd.grab_y : V.dd.grab_x;
        gy = V.dd.rot ? V.dd.grab_x : V.dd.grab_y;
    }
    float c = V.L.cell;
    GuiRect r = { m.x - ((float)gx + 0.5f) * c, m.y - ((float)gy + 0.5f) * c, (float)w * c, (float)h * c };
    int layer = gd_layer();
    gd_set_layer(GL_TOP);
    gd_shadow(r, gui_s(TH->radius), gui_s(14), TH->shadow);
    gd_round_border(r, gui_s(TH->radius), 1, gui_rgba_mul(TH->slot_hi, 0.9f), TH->select);
    inv_draw_item_image(gui_rect_inset(r, fminf(r.w, r.h) * 0.08f), db, &s, V.dd.rot != 0, 0.95f);
    if (V.dd.split && s.count > 1) {
        char b[16];
        snprintf(b, sizeof(b), "%d", s.count / 2);
        GuiText t = { gui_s(TH->t_small), GW_SEMIBOLD, TH->text, GUI_RIGHT, 0, false, true, 0 };
        gd_text(&t, r.x + r.w - gui_s(4), r.y + r.h - gui_s(18), b);
    }
    gd_set_layer(layer);
}

// ------------------------------------------------------------------ scripted pointer (tests)
// ui_inv_* commands queue pointer steps, one per frame, injected as SDL events before the screen
// reads its input (tools/tests/inv_*.cfg). Targets: c:<self|loot>:<parent uid|pockets|rig|backpack|secure>:<loc>:<x>:<y>,
// e:<equipment slot>, h:<1-6>, a:<attachment slot> (the inspect window), discard, out.

typedef struct PStep { int kind; InvTarget t; bool out; bool shift, ctrl, alt, rot; } PStep;   // kind: 0 move, 1 down, 2 up, 3 rdown, 4 rup, 5 mods, 6 dclick
static PStep g_steps[128];
static int g_step_head, g_step_tail;

static bool parse_target(const char *s, InvTarget *t, bool *out) {
    memset(t, 0, sizeof(*t));
    *out = false;
    if (!strcmp(s, "out")) { *out = true; return true; }
    if (!strcmp(s, "discard")) { t->kind = IT_DISCARD; return true; }
    char side[8], par[16];
    int loc, x, y, k;
    if (sscanf(s, "c:%7[a-z]:%15[a-z0-9]:%d:%d:%d", side, par, &loc, &x, &y) == 5) {
        t->kind = IT_CELL;
        t->side = !strcmp(side, "loot") ? INV_SIDE_LOOT : INV_SIDE_SELF;
        // The parent: a uid, or the worn container by name (pockets = the root).
        u16 parent = (u16)atoi(par);
        Player *p = game_local_player();
        InvQuickTarget q = !strcmp(par, "pockets") ? IQ_POCKETS : !strcmp(par, "rig") ? IQ_RIG : !strcmp(par, "backpack") ? IQ_BACKPACK
                         : !strcmp(par, "secure") ? IQ_SECURE : IQ_COUNT;
        if (q != IQ_COUNT && (!p || !inv_quick_parent(&p->inv.t, q, &parent))) return false;
        t->parent = parent;
        t->loc = (u8)loc;
        t->x = (i16)x;
        t->y = (i16)y;
        return true;
    }
    if (!strncmp(s, "e:", 2)) { t->kind = IT_EQUIP; t->index = (i8)equip_slot_from_name(s + 2); return t->index != (i8)EQ_NONE && t->index >= 0; }
    if (sscanf(s, "h:%d", &k) == 1) { t->kind = IT_HOT; t->index = (i8)DG_CLAMP(k - 1, 0, INV_HOTBAR - 1); return true; }
    if (!strncmp(s, "a:", 2)) { t->kind = IT_ATTACH; t->index = (i8)attach_slot_from_name(s + 2); return t->index >= 0; }
    return false;
}

static bool target_pos(const PStep *st, vec2 *p) {
    if (st->out) { *p = v2(gui_w() - 2, gui_h() - 2); return true; }
    for (int i = 0; i < V.hit_n; i++) {
        const Hit *h = &V.hits[i];
        if (st->t.kind == IT_CELL && h->kind == HK_GRID && h->t.side == st->t.side && h->t.parent == st->t.parent && h->t.loc == st->t.loc) {
            *p = v2(h->r.x + ((float)st->t.x + 0.5f) * h->cell, h->r.y + ((float)st->t.y + 0.5f) * h->cell);
            return true;
        }
        bool k = (st->t.kind == IT_EQUIP && h->kind == HK_EQUIP) || (st->t.kind == IT_HOT && h->kind == HK_HOT) || (st->t.kind == IT_ATTACH && h->kind == HK_ATTACH);
        if (k && h->t.index == st->t.index) { *p = v2(h->r.x + h->r.w * 0.5f, h->r.y + h->r.h * 0.5f); return true; }
        if (st->t.kind == IT_DISCARD && h->kind == HK_DISCARD) { *p = v2(h->r.x + h->r.w * 0.5f, h->r.y + h->r.h * 0.5f); return true; }
    }
    return false;
}

static void push_step(int kind, const char *target, bool shift, bool ctrl, bool alt, bool rot) {
    if (g_step_tail >= 128) return;
    PStep s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.shift = shift;
    s.ctrl = ctrl;
    s.alt = alt;
    s.rot = rot;
    if (target && !parse_target(target, &s.t, &s.out)) { log_warn("ui_inv: bad target '%s'", target); return; }
    g_steps[g_step_tail++] = s;
}

static bool g_rot_scripted;

static void run_step(void) {
    if (g_step_head >= g_step_tail) { g_step_head = g_step_tail = 0; return; }
    PStep *s = &g_steps[g_step_head++];
    float pd = g_engine.window ? SDL_GetWindowPixelDensity(g_engine.window) : 1.0f;
    if (pd <= 0) pd = 1;
    gui_set_mods(s->shift, s->ctrl);
    V.alt_scripted = s->alt;
    g_rot_scripted = s->rot;
    if (s->kind == 5) return;
    vec2 p;
    if (!target_pos(s, &p)) { log_warn("ui_inv: target not on screen"); return; }
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    if (s->kind == 0) {
        ev.type = SDL_EVENT_MOUSE_MOTION;
        ev.motion.x = p.x / pd;
        ev.motion.y = p.y / pd;
        ev.motion.xrel = 5;
    } else {
        ev.type = (s->kind == 1 || s->kind == 3 || s->kind == 6) ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        ev.button.button = (s->kind == 3 || s->kind == 4) ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        ev.button.x = p.x / pd;
        ev.button.y = p.y / pd;
        ev.button.clicks = s->kind == 6 ? 2 : 1;
    }
    gui_handle_event(&ev);
}

static bool has(int argc, const char **argv, const char *w) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], w)) return true;
    return false;
}

static void cmd_drag(int argc, const char **argv) {
    if (argc < 3) { log_info("usage: ui_inv_drag <from> <to> [shift] [rot]  (targets: c:self:<parent uid|pockets|rig|backpack|secure>:<loc>:<x>:<y>, e:<slot>, h:<1-6>, a:<slot>, discard, out)"); return; }
    bool shift = has(argc, argv, "shift"), rot = has(argc, argv, "rot");
    push_step(0, argv[1], shift, false, false, false);
    push_step(1, argv[1], shift, false, false, false);
    push_step(0, argv[2], shift, false, false, false);
    push_step(0, argv[2], shift, false, false, rot);
    push_step(2, argv[2], false, false, false, false);
    push_step(5, NULL, false, false, false, false);
}

static void cmd_click(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: ui_inv_click <target> [ctrl|alt|shift|right|double]"); return; }
    bool ctrl = has(argc, argv, "ctrl"), alt = has(argc, argv, "alt"), shift = has(argc, argv, "shift");
    push_step(0, argv[1], shift, ctrl, alt, false);
    if (has(argc, argv, "right")) { push_step(3, argv[1], false, false, false, false); push_step(4, argv[1], false, false, false, false); }
    else if (has(argc, argv, "double")) {
        push_step(1, argv[1], false, false, false, false);
        push_step(2, argv[1], false, false, false, false);
        push_step(6, argv[1], false, false, false, false);
        push_step(2, argv[1], false, false, false, false);
    } else {
        push_step(1, argv[1], shift, ctrl, alt, false);
        push_step(2, argv[1], shift, ctrl, alt, false);
    }
    push_step(5, NULL, false, false, false, false);
}

void inventory_register_commands(void) {
    cmd_register("ui_inv_drag", cmd_drag, "ui_inv_drag <from> <to> [shift] [rot]: scripted inventory drag (targets: c:self:<parent>:<loc>:<x>:<y>, e:<slot>, h:<1-6>, a:<slot>, discard, out)");
    cmd_register("ui_inv_click", cmd_click, "ui_inv_click <target> [ctrl|alt|shift|right|double]: scripted inventory click");
    V.showcase = cvar_reg_bool("inv_showcase", true, CVAR_ARCHIVE | CVAR_EDITABLE, "inventory: the camera turns to face the player while the screen is open (docs/INVENTORY.md)");
    V.order = cvar_reg_string("inv_quick_order", "rig,backpack,pockets", CVAR_ARCHIVE | CVAR_EDITABLE, "inventory: quick-move / pickup order of the player's containers (rig, backpack, pockets, secure)");
    inv_cells_init();
    inv_editor_init();
}

// ------------------------------------------------------------------ screen

void inventory_open(void) {
    idd_reset(&V.dd);
    V.ctx.open = V.split.open = false;
    memset(&V.sel, 0, sizeof(V.sel));
    V.toast[0] = 0;
    gui_anim_restart(gui_id("inv_open"));
}

void inventory_close(void) {
    idd_reset(&V.dd);
    V.ctx.open = V.split.open = V.insp.open = false;
    for (int i = 0; i < MAX_WINS; i++) V.win[i].open = false;
}

// Floating windows don't hold the screen open: Esc closes them first (inventory_draw consumes
// it), Tab / I closes everything.
bool inventory_wants_back(void) { return V.dd.state == IDD_IDLE && !V.dd.carry && !V.ctx.open && !V.split.open; }

bool inventory_showcase(void) { return V.showcase && V.showcase->i; }

bool inventory_draw(GuiRect area, bool as_modal) {
    Game *g = g_();
    World *w = g ? g->world : NULL;
    Player *p = game_local_player();
    const ItemDB *db = items();
    if (!g || !w || !p || !db) return true;
    (void)area;
    (void)as_modal;
    icons_tick();
    inv_cells_tick();
    if (V.order) inv_set_order(inv_order_parse(V.order->str));
    run_step();
    // Layout.
    PanelSrc ps[INV_UI_MAX_PANELS];
    int pn = player_panels(ps);
    InvLayoutIn li;
    memset(&li, 0, sizeof(li));
    LayoutParams lp = gui_layout_params();
    li.safe = gui_layout_safe_rect(&lp);
    li.scale = gui_scale();
    InvTree *lt = game_loot_tree();
    li.loot = g->container_open;
    if (lt) inv_container_extent(inv_root_def(lt, db), &li.loot_cols, &li.loot_rows);
    else { li.loot_cols = 6; li.loot_rows = 4; }
    li.panel_n = pn;
    for (int i = 0; i < pn; i++) {
        inv_container_extent(ps[i].cd, &li.panel[i].cols, &li.panel[i].rows);
        li.panel[i].cols += ps[i].cd->comp_count > 1 ? 1 : 0;   // pouch gaps
    }
    inv_layout(&li, &V.L);
    InvLayout *L = &V.L;
    V.hit_n = 0;
    V.pad_n = 0;
    float a = gui_ease_out(gui_anim_t(gui_id("inv_open")) / TH->modal_in);
    gd_alpha_push(a);
    // Header: title, total weight (informational), sync state.
    GuiText title = { gui_s(TH->t_title), GW_SEMIBOLD, TH->text, GUI_LEFT, 0.02f, true, true, 0 };
    gd_text(&title, L->header.x, L->header.y + gui_s(6), "Inventory");
    char hb[96];
    snprintf(hb, sizeof(hb), "%.1f kg carried%s", inv_tree_weight(&p->inv.t, db), game_inv_pending() ? "   \xC2\xB7   syncing\xE2\x80\xA6" : "");
    GuiText ht = { gui_s(TH->t_small + 1), GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, true, 0 };
    gd_text(&ht, L->header.x + L->header.w, L->header.y + gui_s(14), hb);
    gd_rect(gui_rect(L->header.x, L->header.y + L->header.h - 1, L->header.w, 1), gui_rgba_mul(TH->panel_border, 0.8f));
    // Columns.
    draw_left(ps, pn);
    draw_equip();
    draw_hotbar();
    if (g->container_open) draw_loot();
    draw_windows();
    draw_inspect();
    // Esc / B closes the floating windows first (the inspect window, then the newest container window).
    if (gui_nav(NAV_BACK) && V.dd.state == IDD_IDLE && !V.dd.carry && !V.ctx.open && !V.split.open) {
        int top = -1;
        for (int i = 0; i < MAX_WINS; i++) if (V.win[i].open) top = i;
        if (V.insp.open) { V.insp.open = false; gui_nav_consume(NAV_BACK); }
        else if (top >= 0) { V.win[top].open = false; gui_nav_consume(NAV_BACK); }
    }
    // Input: the hover target from this frame's rects (drawn above).
    InvDDIn in;
    memset(&in, 0, sizeof(in));
    bool en = gui_input_enabled() && !V.ctx.open && !V.split.open;
    in.mouse = gui_mouse();
    in.pressed = en && gui_mouse_pressed(0);
    in.released = gui_mouse_released(0);
    in.right_pressed = en && gui_mouse_pressed(1);
    in.double_clicked = en && gui_double_clicked();
    in.shift = gui_shift();
    in.ctrl = gui_ctrl();
    in.alt = alt_down();
    in.rotate = (en && gui_key_pressed(SDL_SCANCODE_R)) || g_rot_scripted;
    g_rot_scripted = false;
    in.threshold = gui_s(6);
    in.hover = gui_nav_mode() && V.pad_cur < V.pad_n ? V.pad_t[V.pad_cur] : hit_test(in.mouse);
    if (!gui_nav_mode()) {
        // The pad cursor follows the mouse when it is over a target.
        for (int i = 0; i < V.pad_n; i++) if (gui_rect_contains(V.pad_r[i], in.mouse.x, in.mouse.y)) { V.pad_cur = i; break; }
    }
    in.cancel = V.dd.state != IDD_IDLE && gui_nav(NAV_BACK);
    if (in.cancel) gui_nav_consume(NAV_BACK);
    V.hover = in.hover;
    if (en) {
        InvView v = view();
        InvDDOut o = idd_update(&V.dd, &in, &v);
        if (o.action) apply(&o);
        pad_input();
    }
    // Footer: key hints + the last rejection.
    if (V.toast[0] && gui_time() - V.toast_t < 2.5) {
        float ta = 1.0f - saturate((float)(gui_time() - V.toast_t - 2.0) / 0.5f);
        GuiText t = { gui_s(TH->t_small + 1), GW_SEMIBOLD, gui_rgba_mul(TH->danger, ta), GUI_LEFT, 0, false, true, 0 };
        gd_text(&t, L->footer.x, L->footer.y + gui_s(2), V.toast);
    }
    screens_footer_hint(L->footer, gui_nav_mode() ? "A  pick up / place    X  rotate / menu    Y  quick move    LB RB  panels    B  back"
                                                  : "Drag  move    R  rotate    Ctrl+click  quick move    Alt+click  equip    Shift+drag  split    Double click  open    Tab  close");
    draw_context();
    draw_split();
    gd_alpha_pop();
    inv_editor_draw();
    draw_ghost();
    return false;
}
