// Inventory screen (docs/INVENTORY.md "Screen"), over the game: the first-person view keeps rendering
// behind it (the camera never moves) under a full-screen blur.
//   left:   the primary and secondary weapon slots, each with its attachment chips, the holster,
//           the gear row (helmet, plate carrier, backpack) and the inspect panel;
//   centre: a second render of the player character (its own instance, idle, the worn gear) on the
//           blur, head to mid-thigh; dropping a weapon or gear on it equips it; the discard zone;
//   right:  the category bar (highlights, never moves anything) and Compact, then an open world
//           container's grid above the player's grid (the worn backpack's); the column scrolls.
// Mouse: drag between any grids / slots / chips (R rotates, the footprint shows green / red),
// Ctrl+click quick move, Alt+click equip, Shift+drag split, double click inspect, right click menu,
// drag out / onto Discard drops. Pad: a cell cursor (d-pad / stick), LB / RB cycle the regions,
// A pick up / place, X rotate (menu when idle), Y quick move, B cancel / close.
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
#include "gameplay/health.h"
#include "gameplay/entities.h"
#include "gameplay/characters.h"
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
#define MAX_PAD 640

typedef enum HitKind { HK_BLOCK, HK_GRID, HK_EQUIP, HK_ATTACH, HK_CHAR, HK_DISCARD } HitKind;
typedef struct Hit {
    u8 kind;
    GuiRect r, clip;          // clip: the visible part (the scrolled column)
    InvTarget t;              // EQUIP / ATTACH: the target; GRID: side / parent / loc
    float cell;
} Hit;

// Pad regions (LB / RB cycle them).
enum { PR_SLOTS, PR_CHIPS, PR_LOOT, PR_GRID, PR_INSPECT, PR_COUNT };

static struct {
    InvDrag dd;
    InvTarget hover;
    InvTarget sel;             // selected item (CELL / EQUIP: side + uid): the inspect panel
    struct { bool open; InvTarget at; vec2 pos; double t; } ctx;
    struct { bool open; InvIntent in; float count; int max; } split;
    InvIntent pending_discard;
    char toast[96];
    double toast_t;
    InvTarget shake_at;
    double shake_t;
    Hit hits[MAX_HITS];
    int hit_n;
    GuiScroll right_scroll;
    struct {
        bool rotating;
        vec2 rot_from;
        TextureId tex;
        ItemViewCam cam;
        u16 cam_item;
        double last_touch;
    } insp;
    int cat;                   // the category bar: 0 all, else ICAT_* + 1
    float char_yaw;            // the character turned by dragging it
    bool char_drag;
    float char_from;
    // pad cursor
    InvTarget pad_t[MAX_PAD];
    GuiRect pad_r[MAX_PAD];
    u8 pad_region[MAX_PAD];
    int pad_n, pad_cur;
    bool alt_scripted;
    InvLayout L;
} V;

static Game *g_(void) { return game(); }

// Type size (px) for a design size, never below a readable floor: at 720p the theme's 12 px tiny type
// is 8 px on screen.
#define INV_MIN_TEXT_PX 10.0f
static float tpx(float design) { return fmaxf(gui_s(design), INV_MIN_TEXT_PX); }
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

// The slot view of a payload (the attachment chips use the slot widget).
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
// loot_left (optional): the entries the open container still holds afterwards (what take all leaves).
static int dry_run_left(const InvIntent *in, int *loot_left) {
    static Inventory inv;
    static InvTree loot;
    Player *p = game_local_player();
    const ItemDB *db = items();
    if (!p || !db) return IIR_INVALID;
    inv = p->inv;
    InvTree *lt = game_loot_tree();
    if (lt) loot = *lt;
    InvCtx c = inv_ctx(&inv, db, lt ? &loot : NULL);
    int r = inv_intent_apply(&c, in, NULL);
    if (loot_left) *loot_left = lt ? inv_live_count(&loot) : 0;
    return r;
}

static int dry_run(const InvIntent *in) { return dry_run_left(in, NULL); }

// c with its alpha raised to at least a (an opaque-enough backdrop).
static u32 min_alpha(u32 c, float a) {
    u32 al = (u32)(a * 255.0f + 0.5f);
    return (c >> 24) >= al ? c : (c & 0x00FFFFFFu) | (al << 24);   // lint:ok the alpha byte only
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
        else if (in.type == II_MOUNT || in.type == II_UNMOUNT || in.type == II_EQUIP || in.type == II_HOLD) gui_sound(TH->snd_equip);
        else gui_sound(TH->snd_click);
    }
    return r;
}

void inventory_confirm_drop(bool yes) {
    if (yes && V.pending_discard.type) inv_screen_send(V.pending_discard);
    memset(&V.pending_discard, 0, sizeof(V.pending_discard));
}

static void select_item(InvTarget at) {
    if (!at.uid) return;
    V.sel = at;
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
        bool worn_pack = !it->parent && it->loc == INV_LOC_EQUIP + EQ_BACKPACK && o->in.src_side == INV_SIDE_SELF;
        screens_confirm(CONFIRM_DROP, title, worn_pack ? "It will be left on the ground; what it holds moves into your sling bag." : "It will be left on the ground in front of you.", 0);
        break;
    }
    case IDA_SELECT:
        if (o->at.uid) { select_item(o->at); gui_sound(TH->snd_click); }
        break;
    case IDA_CONTEXT:
        V.ctx.open = true;
        V.ctx.at = o->at;
        V.ctx.pos = gui_mouse();
        V.ctx.t = gui_time();
        select_item(o->at);
        gui_set_focus(0);
        break;
    case IDA_OPEN: select_item(o->at); break;
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

static bool pad_focused(int kind, int side, int index) {
    if (!gui_nav_mode() || V.pad_cur >= V.pad_n) return false;
    const InvTarget *t = &V.pad_t[V.pad_cur];
    return t->kind == kind && t->side == side && t->index == index;
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

static bool carrying(int side, u16 uid) { return V.dd.state == IDD_DRAGGING && V.dd.side == side && V.dd.uid == uid && V.dd.src.kind != IT_ATTACH; }

static bool in_hands(int side, u16 uid) {
    Player *p = game_local_player();
    return side == INV_SIDE_SELF && p && uid && p->inv.hands == uid;
}

// One item tile inside a grid.
static void draw_item_tile(GuiRect r, const InvItem *it, int side, bool dim, InvTarget me) {
    const ItemDB *db = items();
    const ItemDef *d = item_def(db, it->s.item);
    float rad = gui_s(TH->radius);
    float sx = shake_amount(me);
    if (sx > 0) r.x += sinf(sx * 40.0f) * sx * gui_s(5);
    bool sel = V.sel.uid == it->uid && V.sel.side == side;
    u32 bg = gui_rgba_lerp(TH->slot_bg, type_color(d), 0.16f);
    if (V.hover.uid == it->uid && V.hover.side == side && V.dd.state == IDD_IDLE) bg = gui_rgba_lerp(bg, TH->slot_hi, 0.6f);
    gd_alpha_push(carrying(side, it->uid) ? 0.35f : (dim ? 0.25f : 1.0f));
    gd_round_border(gui_rect_inset(r, 1), rad, sel ? fmaxf(1.0f, gui_s(1.5f)) : 1.0f, bg, sel ? TH->select : TH->slot_border);
    inv_draw_item_image(gui_rect_inset(r, fminf(r.w, r.h) * 0.08f), db, &it->s, (it->flags & INVF_ROT) != 0, 1.0f);
    float fs = fmaxf(tpx(TH->t_tiny), fminf(gui_s(13), V.L.cell * 0.22f));
    bool held = in_hands(side, it->uid);
    float badge = held ? fs * 0.7f : 0.0f;   // in the hands: a dot top right; the name stops left of it
    GuiText name = { fs, GW_MEDIUM, TH->text_dim, GUI_LEFT, 0.02f, true, true, r.w - gui_s(8) - (held ? badge + gui_s(4) : 0.0f) };
    if (name.max_w > fs * 1.5f) gd_text(&name, r.x + gui_s(4), r.y + gui_s(2), revive_item_name(db, it->s.item, it->s.ammo));
    char b[24];
    const WeaponDef *wd = item_weapon(db, it->s.item);
    GuiText br = { fs, GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, true, 0 };
    float by = r.y + r.h - gd_line_height(fs) - gui_s(2);
    if (it->s.count > 1) {
        snprintf(b, sizeof(b), "%d", it->s.count);
        br.weight = GW_SEMIBOLD;
        br.color = TH->text;
        gd_text(&br, r.x + r.w - gui_s(4), by, b);
    } else if (wd && wd->kind != WPN_MELEE && wd->mag_size > 0) {
        snprintf(b, sizeof(b), "%d", it->s.ammo);
        gd_text(&br, r.x + r.w - gui_s(4), by, b);
    } else if (d->use_type == INV_USE_CHARGES && d->charges > 1) {
        snprintf(b, sizeof(b), "%d/%d", d->charges - it->s.ammo, d->charges);
        gd_text(&br, r.x + r.w - gui_s(4), by, b);
    } else if (item_container(db, it->s.item)) {   // a spare backpack: its grid
        int cc, cr;
        inv_container_extent(item_container(db, it->s.item), &cc, &cr);
        snprintf(b, sizeof(b), "%dx%d", cc, cr);
        br.color = TH->text_faint;
        gd_text(&br, r.x + r.w - gui_s(4), by, b);
    } else if (d->plates && it->s.ammo > 0) {   // a plate carrier holding plates
        snprintf(b, sizeof(b), "%d/%d", (int)ceilf((float)it->s.ammo / ARMOR_PER_PLATE), d->plates);
        gd_text(&br, r.x + r.w - gui_s(4), by, b);
    }
    if (d->durability) {   // durability: a thin bar along the bottom
        float f = (float)it->dur / (float)d->durability;
        GuiRect bar = { r.x + gui_s(4), r.y + r.h - gui_s(4), (r.w - gui_s(8)) * saturate(f), gui_s(2) };
        gd_rect(bar, f < 0.3f ? TH->danger : TH->text_faint);
    }
    if (held) gd_circle(r.x + r.w - gui_s(4) - badge * 0.5f, r.y + gui_s(4) + badge * 0.5f, badge * 0.5f, TH->select);
    gd_alpha_pop();
}

static const char *slot_name(int slot) {
    static const char *n[EQ_COUNT] = { "Primary", "Secondary", "Holster", "Backpack", "Helmet", "Body armor" };
    return slot >= 0 && slot < EQ_COUNT ? n[slot] : "?";
}

static const char *cat_name(int cat) {
    static const char *n[ICAT_COUNT] = { "Weapons", "Attachments", "Medical", "Throwables", "Tools", "Misc" };
    return cat >= 0 && cat < ICAT_COUNT ? n[cat] : "Misc";
}

static void tooltip_for(GuiId id, const InvItem *it) {
    const ItemDB *db = items();
    const ItemDef *d = item_def(db, it->s.item);
    int w, h;
    inv_item_size(db, it, &w, &h);
    char tip[320];
    int n = snprintf(tip, sizeof(tip), "%s\n%s  \xC2\xB7  %dx%d", revive_item_name(db, it->s.item, it->s.ammo), cat_name(d->category), w, h);
    if (d->max_stack > 1) n += snprintf(tip + n, sizeof(tip) - (size_t)n, "\nStack %d / %d", it->s.count, d->max_stack);
    if (d->durability) n += snprintf(tip + n, sizeof(tip) - (size_t)n, "\nDurability %d / %d", it->dur, d->durability);
    if (d->plates) n += snprintf(tip + n, sizeof(tip) - (size_t)n, "\nHolds %d plates", d->plates);
    if (d->equip_slot != EQ_NONE) n += snprintf(tip + n, sizeof(tip) - (size_t)n, "\nWorn: %s", slot_name(d->equip_slot));
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

// A tree's grid at origin (cells), its items, hit / pad registration. One rectangle: no dividers.
static void draw_grid(GuiRect origin, GuiRect clip, int side, u16 parent, const InvContainerDef *cd, int region) {
    InvTree *t = inv_screen_tree(side);
    const ItemDB *db = items();
    if (!t || !cd) return;
    float cell = V.L.cell;
    for (int c = 0; c < cd->comp_count; c++) {
        const InvCompDef *k = &cd->comp[c];
        GuiRect gr = { origin.x + (float)k->at_x * cell, origin.y + (float)k->at_y * cell, (float)k->w * cell, (float)k->h * cell };
        gd_round(gr, gui_s(TH->radius), gui_rgba_mul(TH->slot_bg, 0.9f));
        for (int y = 0; y < k->h; y++)
            for (int x = 0; x < k->w; x++) {
                GuiRect cr = { gr.x + (float)x * cell, gr.y + (float)y * cell, cell, cell };
                InvTarget pt = { IT_CELL, (u8)side, (u8)c, 0, parent, 0, (i16)x, (i16)y };
                const InvItem *under = inv_at(t, db, parent, c, x, y);
                // The cell lines only on free cells (and under the carried item): a multi-cell tile reads as one.
                if (!under || carrying(side, under->uid)) gd_round_outline(gui_rect_inset(cr, 1), gui_s(2), 1, gui_rgba_mul(TH->slot_border, 0.55f));
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
            draw_item_tile(ir, it, side, inv_cat_dims(V.cat, item_def(db, it->s.item)), me);
            if (V.hover.kind == IT_CELL && V.hover.uid == it->uid && V.hover.side == side && V.dd.state == IDD_IDLE && !V.ctx.open)
                tooltip_for(gui_idi("inv_item", side * 65536 + it->uid), it);
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

// A panel title: the name left, a note (cells used / free, the grid size) right of it.
static void panel_title(GuiRect r, const char *name, const char *note) {
    GuiText t = { tpx(TH->t_label), GW_SEMIBOLD, TH->text, GUI_LEFT, TH->tracking_caps, true, false, r.w * 0.6f };
    gd_text(&t, r.x, r.y + gui_s(4), name);
    GuiText s = { tpx(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT, 0, false, false, 0 };
    gd_text(&s, r.x + r.w, r.y + gui_s(5), note);
}

// An equipment slot's frame: the slot name, its backdrop, the drag highlight, the shake. Returns the
// content rect.
static GuiRect slot_frame(GuiRect r, int slot, InvTarget me, const InvItem *it) {
    const ItemDB *db = items();
    InvView v = view();
    bool hover = V.hover.kind == IT_EQUIP && V.hover.side == me.side && V.hover.index == slot;
    u32 border = gui_rgba_lerp(TH->panel_border, TH->text_faint, 0.35f), bg = TH->panel;
    if (V.dd.state == IDD_DRAGGING && hover) {
        InvDDOut o = idd_drop_on(&V.dd, me, &v);
        bool ok = o.action == IDA_INTENT && dry_run(&o.in) == IIR_OK;
        border = ok ? TH->ok : TH->danger;
        bg = gui_rgba_lerp(bg, border, 0.2f);
    } else if (V.dd.state == IDD_DRAGGING) {
        const InvItem *c = item_of(V.dd.side, V.dd.uid);
        if (c && V.dd.src.kind != IT_ATTACH && inv_equip_accepts(slot, item_def(db, c->s.item))) border = gui_rgba_mul(TH->accent, 0.8f);
    } else if (hover) {
        bg = gui_rgba_lerp(bg, TH->slot_hi, 0.6f);
    }
    if (it && V.sel.uid == it->uid && V.sel.side == me.side) border = TH->select;
    float sh = shake_amount(me);
    if (sh > 0) r.x += sinf(sh * 40.0f) * sh * gui_s(5);
    gd_backdrop(gui_rect_inset(r, 1), WHITE);
    gd_round_border(r, gui_s(TH->radius), 1, bg, border);
    GuiText lab = { tpx(TH->t_tiny), GW_MEDIUM, it ? TH->text_faint : TH->text_dim, GUI_LEFT, TH->tracking_caps, true, false, r.w - gui_s(8) };
    gd_text(&lab, r.x + gui_s(5), r.y + gui_s(3), slot_name(slot));
    if (pad_focused(IT_EQUIP, me.side, slot)) gw_focus_ring(r, gui_s(TH->radius), 1.0f);
    float lh = gd_line_height(lab.size) + gui_s(4);
    return gui_rect(r.x + gui_s(4), r.y + lh, r.w - gui_s(8), r.h - lh - gui_s(4));
}

// ------------------------------------------------------------------ left: slots, chips, inspect

// A weapon slot: the image at its grid aspect, the name, the magazine / reserve and (drawn) the fire mode.
static void draw_weapon_slot(int slot) {
    GuiRect r = V.L.equip[slot];
    Player *p = game_local_player();
    const ItemDB *db = items();
    const InvItem *it = inv_equipped_c(&p->inv.t, slot);
    InvTarget me = { IT_EQUIP, INV_SIDE_SELF, 0, (i8)slot, 0, it ? it->uid : 0, 0, 0 };
    hit_add(HK_EQUIP, r, r, me, 0);
    pad_add(me, r, PR_SLOTS);
    GuiRect c = slot_frame(r, slot, me, it);
    if (!it) return;
    gd_alpha_push(carrying(INV_SIDE_SELF, it->uid) ? 0.35f : 1.0f);
    float fs = tpx(TH->t_small);
    GuiRect img = { c.x, c.y, c.w * 0.62f, c.h - gd_line_height(fs) };
    inv_draw_item_image(img, db, &it->s, false, 1.0f);
    GuiText nm = { fs, GW_SEMIBOLD, TH->text, GUI_LEFT, 0, false, true, c.w * 0.6f };
    gd_text(&nm, c.x + gui_s(2), c.y + c.h - gd_line_height(fs), revive_item_name(db, it->s.item, it->s.ammo));
    const WeaponDef *wd = item_weapon(db, it->s.item);
    if (wd && wd->kind != WPN_MELEE && wd->mag_size > 0) {
        WeaponDef eff;
        attach_weapon_effective(db, wd, it->s.att, &eff);
        bool drawn = p->sim.wpn.slot == slot;
        char b[48];
        snprintf(b, sizeof(b), "%d / %d  \xC2\xB7  %d", drawn ? p->sim.wpn.mag : it->s.ammo, eff.mag_size, wd->ammo_item ? inv_count(&p->inv, wd->ammo_item) : 0);
        GuiText at = { fs, GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, false, 0 };
        gd_text(&at, c.x + c.w - gui_s(2), c.y + c.h - gd_line_height(fs), b);
        if (drawn) {
            GuiText fm = { tpx(TH->t_tiny), GW_SEMIBOLD, TH->select, GUI_RIGHT, TH->tracking_caps, true, false, 0 };
            gd_text(&fm, c.x + c.w - gui_s(2), c.y, fire_mode_name((FireMode)p->sim.wpn.fire_mode));
        }
    }
    gd_alpha_pop();
    if (V.hover.kind == IT_EQUIP && V.hover.side == INV_SIDE_SELF && V.hover.index == slot && V.dd.state == IDD_IDLE && !V.ctx.open) tooltip_for(gui_idi("inv_eq", slot), it);
}

// The attachment chips of a weapon in a row (a weapon's own slots only): mounted attachment or an
// empty outline with the slot name; drag an attachment in to mount it, drag a chip out to unmount.
static void draw_chips(GuiRect row, int side, const InvItem *w, int region) {
    const ItemDB *db = items();
    const WeaponDef *wd = w ? item_weapon(db, w->s.item) : NULL;
    if (!wd || wd->kind == WPN_MELEE) {
        gw_label(row, w ? "Takes no attachments" : "", tpx(TH->t_tiny), GW_REGULAR, TH->text_faint, GUI_LEFT);
        return;
    }
    int n = 0;
    for (int k = 0; k < ATT_SLOTS; k++) n += wd->att_slots[k].present;
    if (!n) return;
    float gap = gui_s(6), cw = fminf(row.h, (row.w - gap * (float)(ATT_SLOTS - 1)) / (float)ATT_SLOTS);
    InvView v = view();
    int col = 0;
    for (int k = 0; k < ATT_SLOTS; k++) {
        if (!wd->att_slots[k].present) continue;
        GuiRect chip = { row.x + (cw + gap) * (float)col++, row.y, cw, cw };
        InvTarget me = { IT_ATTACH, (u8)side, 0, (i8)k, 0, w->uid, 0, 0 };
        hit_add(HK_ATTACH, chip, chip, me, 0);
        pad_add(me, chip, region);
        GuiSlotView sv;
        InvSlot fake;
        memset(&fake, 0, sizeof(fake));
        fake.item = item_for_attachment(db, w->s.att[k]);
        fake.count = fake.item ? 1 : 0;
        inventory_slot_view(&fake, db, -1, &sv);
        bool hover = V.hover.kind == IT_ATTACH && V.hover.uid == w->uid && V.hover.side == side && V.hover.index == k;
        sv.hot = hover;
        if (V.dd.state == IDD_DRAGGING && hover) {
            InvDDOut o = idd_drop_on(&V.dd, me, &v);
            sv.drop_state = o.action == IDA_INTENT && dry_run(&o.in) == IIR_OK ? 1 : 2;
        } else if (V.dd.state == IDD_DRAGGING && V.dd.src.kind != IT_ATTACH) {
            const InvItem *src = item_of(V.dd.side, V.dd.uid);
            int aslot;
            if (src && inv_attach_check(db, &w->s, src->s.item, &aslot) == IIR_OK && aslot == k) sv.drop_state = 1;
        }
        sv.shake = shake_amount(me);
        sv.focus = gui_nav_mode() && V.pad_cur < V.pad_n && V.pad_t[V.pad_cur].kind == IT_ATTACH && V.pad_t[V.pad_cur].uid == w->uid && V.pad_t[V.pad_cur].index == k;
        if (V.dd.state == IDD_DRAGGING && V.dd.src.kind == IT_ATTACH && V.dd.uid == w->uid && V.dd.src.index == k) sv.dim = true;
        gw_slot(chip, &sv);
        if (!fake.item) gw_label_caps(chip, attach_slot_name(k), fminf(tpx(TH->t_tiny) * 0.85f, cw * 0.22f), TH->text_faint, GUI_CENTER);
        if (hover && V.dd.state == IDD_IDLE) gui_tooltip(gui_idi("inv_att", side * 4096 + w->uid * 8 + k), fake.item ? item_def(db, fake.item)->name : attach_slot_name(k));
    }
}

// A gear slot (helmet, plate carrier, backpack): the image, the plates / grid / durability.
static void draw_gear_slot(int slot) {
    GuiRect r = V.L.equip[slot];
    Player *p = game_local_player();
    const ItemDB *db = items();
    const InvItem *it = inv_equipped_c(&p->inv.t, slot);
    InvTarget me = { IT_EQUIP, INV_SIDE_SELF, 0, (i8)slot, 0, it ? it->uid : 0, 0, 0 };
    hit_add(HK_EQUIP, r, r, me, 0);
    pad_add(me, r, PR_SLOTS);
    GuiRect c = slot_frame(r, slot, me, it);
    float fs = tpx(TH->t_tiny);
    char b[32] = "";
    if (slot == EQ_BODY_ARMOR || (slot == EQ_HEADWEAR && !it)) {
        // The plates the player holds: 1 without a carrier, its plates with one (docs/DAMAGE.md "Armor").
        const Health *h = game_world() ? &game_world()->health[ENT_INDEX(p->ent)] : NULL;
        if (slot == EQ_BODY_ARMOR && h) snprintf(b, sizeof(b), "%d/%d plates", (int)ceilf(h->armor / ARMOR_PER_PLATE - 0.01f), (int)lrintf(h->max_armor / ARMOR_PER_PLATE));
    }
    if (it) {
        gd_alpha_push(carrying(INV_SIDE_SELF, it->uid) ? 0.35f : 1.0f);
        inv_draw_item_image(gui_rect(c.x, c.y, c.w, c.h - gd_line_height(fs)), db, &it->s, false, 1.0f);
        const ItemDef *d = item_def(db, it->s.item);
        if (slot == EQ_BACKPACK) {
            int cc, cr;
            inv_grid_size(&p->inv.t, db, &cc, &cr);
            snprintf(b, sizeof(b), "%d x %d", cc, cr);
        } else if (d->durability) {
            snprintf(b, sizeof(b), "%d / %d", it->dur, d->durability);
        }
        gd_alpha_pop();
        if (V.hover.kind == IT_EQUIP && V.hover.side == INV_SIDE_SELF && V.hover.index == slot && V.dd.state == IDD_IDLE && !V.ctx.open) tooltip_for(gui_idi("inv_eq", slot), it);
    }
    if (b[0]) {
        GuiText t = { fs, GW_MEDIUM, TH->text_dim, GUI_CENTER, 0, false, true, c.w };
        gd_text(&t, c.x + c.w * 0.5f, c.y + c.h - gd_line_height(fs), b);
    }
}

static void stat_line(GuiCursor *c, const char *label, const char *value, float delta, bool higher_better) {
    GuiRect r = gc_row(c, gui_s(19));
    gw_label(gui_rect(r.x, r.y, r.w * 0.55f, r.h), label, tpx(TH->t_small + 1), GW_REGULAR, TH->text_dim, GUI_LEFT);
    char buf[64];
    u32 col = TH->text;
    if (fabsf(delta) >= 0.5f) {
        col = (higher_better ? delta > 0 : delta < 0) ? TH->ok : TH->danger;
        snprintf(buf, sizeof(buf), "%s  %+.0f%%", value, delta);
    } else {
        snprintf(buf, sizeof(buf), "%s", value);
    }
    gw_label(gui_rect(r.x + r.w * 0.45f, r.y, r.w * 0.55f, r.h), buf, tpx(TH->t_small + 1), GW_MEDIUM, col, GUI_RIGHT);
}

static float pct(float now, float base) { return base != 0.0f ? 100.0f * (now - base) / fabsf(base) : 0.0f; }

static void item_stats(GuiCursor *c, const InvItem *it) {
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
        int cc, cr;
        inv_container_extent(cd, &cc, &cr);
        snprintf(v, sizeof(v), "%d x %d", cc, cr);
        stat_line(c, "Grid", v, 0, true);
    }
    if (d->plates) { snprintf(v, sizeof(v), "%d (without: %d)", d->plates, ARMOR_BASE_PLATES); stat_line(c, "Plates", v, 0, true); }
    if (d->durability) { snprintf(v, sizeof(v), "%d / %d", it->dur, d->durability); stat_line(c, "Durability", v, 0, true); }
}

// The inspect panel (docked under the slots): the selected item turning in 3D, its attachments,
// what it is and its stats.
static void draw_inspect(void) {
    GuiRect pr = V.L.inspect;
    if (pr.h < gui_s(60)) return;
    gw_panel(pr, true);
    gui_block_rect(pr);
    hit_add(HK_BLOCK, pr, pr, (InvTarget){ IT_PANEL, 0, 0, 0, 0, 0, 0, 0 }, 0);
    GuiRect c = gui_rect_inset(pr, V.L.pad);
    const ItemDB *db = items();
    const InvItem *it = item_of(V.sel.side, V.sel.uid);
    if (!it) {
        memset(&V.sel, 0, sizeof(V.sel));
        gw_label(c, "Select an item to inspect it", tpx(TH->t_small), GW_REGULAR, TH->text_faint, GUI_CENTER);
        return;
    }
    const ItemDef *d = item_def(db, it->s.item);
    const WeaponDef *wd = item_weapon(db, it->s.item);
    float a = gui_ease_out(gui_anim_t(gui_id("inv_insp_in")) / TH->modal_in);
    gd_alpha_push(a);
    GuiText title = { tpx(TH->t_label), GW_SEMIBOLD, TH->text, GUI_LEFT, TH->tracking_caps, true, true, c.w * 0.65f };
    gd_text(&title, c.x, c.y, revive_item_name(db, it->s.item, it->s.ammo));
    GuiText kind = { tpx(TH->t_tiny), GW_SEMIBOLD, type_color(d), GUI_RIGHT, TH->tracking_caps, true, false, 0 };
    gd_text(&kind, c.x + c.w, c.y + gui_s(1), cat_name(d->category));
    float ty = c.y + gd_line_height(title.size) + gui_s(4);
    GuiRect vr = { c.x, ty, c.w, fminf(c.w * 0.5f, (c.y + c.h - ty) * 0.45f) };
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
    // The attachment chips of any weapon (the slot column shows the primary's and secondary's too).
    if (wd && wd->kind != WPN_MELEE) {
        float ch = fminf(gui_s(40), (c.w - gui_s(24)) / (float)ATT_SLOTS);
        draw_chips(gc_row(&cur, ch), V.sel.side, it, PR_INSPECT);
    }
    GuiText desc = { tpx(TH->t_small + 1), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, false, 0 };
    gd_clip_push(gui_rect(c.x, cur.y, c.w, c.y + c.h - cur.y));
    float dh = gd_text_wrap(&desc, cur.area.x, cur.y, cur.area.w, d->desc, true);
    cur.y += dh + gui_s(8);
    item_stats(&cur, it);
    gd_clip_pop();
    gd_alpha_pop();
}

static void draw_left(void) {
    Player *p = game_local_player();
    static const int weapons[3] = { EQ_PRIMARY, EQ_SECONDARY, EQ_HOLSTER };
    for (int i = 0; i < 3; i++) {
        draw_weapon_slot(weapons[i]);
        if (i < INV_UI_WEAPON_ROWS) draw_chips(V.L.att_row[i], INV_SIDE_SELF, inv_equipped_c(&p->inv.t, weapons[i]), PR_CHIPS);
    }
    static const int gear[3] = { EQ_HEADWEAR, EQ_BODY_ARMOR, EQ_BACKPACK };
    for (int i = 0; i < 3; i++) draw_gear_slot(gear[i]);
    draw_inspect();
}

// ------------------------------------------------------------------ centre: the character

// The second render of the player (gameplay/characters.h chars_preview): the same model and
// cosmetics, its own idle pose, the worn gear, alone on a transparent background over the blur.
// Dropping a weapon or gear on it equips it; dragging it turns it.
static void draw_character(void) {
    InvLayout *L = &V.L;
    Game *g = g_();
    Player *p = game_local_player();
    GuiRect r = L->player;
    float s = gui_scale() > 0 ? gui_scale() : 1.0f;
    (void)s;
    if (g && g->world && p) {
        CharPreviewIn in;
        memset(&in, 0, sizeof(in));
        in.w = (int)lrintf(r.w);
        in.h = (int)lrintf(r.h);
        in.yaw = V.char_yaw;
        static const int worn[CWORN_COUNT] = { EQ_HEADWEAR, EQ_BODY_ARMOR, EQ_BACKPACK };
        for (int k = 0; k < CWORN_COUNT; k++) {
            const InvItem *it = inv_equipped_c(&p->inv.t, worn[k]);
            in.worn[k] = it ? it->s.item : 0;
        }
        TextureId tex = chars_preview(g->world, g->local_slot, &in);
        if (tex) gd_image(r, tex, v4(0, 0, 1, 1), WHITE);
    }
    // The drop target: the body's middle (not the empty sides of the column).
    GuiRect body = { r.x + r.w * 0.25f, r.y + r.h * 0.12f, r.w * 0.5f, r.h * 0.76f };
    InvTarget me = { IT_CHAR, INV_SIDE_SELF, 0, 0, 0, 0, 0, 0 };
    hit_add(HK_CHAR, body, body, me, 0);
    if (V.dd.state == IDD_DRAGGING && V.dd.src.kind != IT_ATTACH) {
        InvView v = view();
        InvDDOut o = idd_drop_on(&V.dd, me, &v);
        if (o.action == IDA_INTENT) {
            bool over = V.hover.kind == IT_CHAR;
            bool ok = dry_run(&o.in) == IIR_OK;
            u32 c = over ? (ok ? TH->ok : TH->danger) : TH->accent;
            gd_round_outline(body, gui_s(TH->radius_panel), over ? fmaxf(1.0f, gui_s(1.5f)) : 1.0f, gui_rgba_mul(c, over ? 0.9f : 0.35f));
            GuiText t = { tpx(TH->t_small), GW_SEMIBOLD, gui_rgba_mul(TH->text, over ? 1.0f : 0.6f), GUI_CENTER, TH->tracking_caps, true, false, 0 };
            gd_text(&t, body.x + body.w * 0.5f, body.y + body.h - gui_s(28), "Drop to equip");
        }
    }
    // Drag on the character (not carrying anything): turn it.
    GuiInteract ci = gui_interact(gui_id("inv_char"), body, 0);
    if (ci.pressed && V.dd.state == IDD_IDLE) { V.char_drag = true; V.char_from = gui_mouse().x; }
    if (V.char_drag) {
        if (!gui_mouse_down(0)) V.char_drag = false;
        else { V.char_yaw += (gui_mouse().x - V.char_from) * 0.01f; V.char_from = gui_mouse().x; }
    } else {
        V.char_yaw *= 1.0f - saturate(gui_dt() * 3.0f);   // eases back to facing the camera
    }
    // Discard zone: a solid pill (the panel colour near opaque, a danger-tinted edge).
    GuiRect d = L->discard;
    InvTarget dt = { IT_DISCARD, 0, 0, 0, 0, 0, 0, 0 };
    hit_add(HK_DISCARD, d, d, dt, 0);
    bool over = V.dd.state == IDD_DRAGGING && V.hover.kind == IT_DISCARD;
    bool dragging = V.dd.state == IDD_DRAGGING;
    u32 fill = min_alpha(over ? gui_rgba_lerp(TH->panel, TH->danger, 0.45f) : TH->panel, 0.95f);
    u32 edge = over ? TH->danger : gui_rgba_lerp(TH->panel_border, TH->danger, dragging ? 0.8f : 0.45f);
    gd_round_border(d, d.h * 0.5f, dragging ? fmaxf(1.0f, gui_s(1.5f)) : 1.0f, fill, edge);
    gw_label_caps(d, "Discard", tpx(TH->t_label), over ? TH->text : dragging ? TH->text_dim : TH->text_faint, GUI_CENTER);
}

// ------------------------------------------------------------------ right: categories, loot, grid

static void draw_cats(void) {
    GuiRect r = V.L.cats;
    float h = gui_s(24), gap = gui_s(6), x = r.x, y = r.y;
    float fs = tpx(TH->t_tiny + 1);
    GuiText t = { fs, GW_MEDIUM, 0, GUI_CENTER, TH->tracking_caps, true, false, 0 };
    GuiRect cr = gui_rect(r.x + r.w - gui_s(92), r.y, gui_s(92), h);   // Compact, top right
    for (int i = 0; i <= ICAT_COUNT; i++) {
        const char *name = i ? cat_name(i - 1) : "All";
        float w = gd_text_width(&t, name) + gui_s(20);
        if (x + w > (y == r.y ? cr.x - gap : r.x + r.w)) { x = r.x; y += h + gap * 0.5f; }
        GuiRect chip = { x, y, w, h };
        GuiInteract in = gui_interact(gui_idi("inv_cat", i), chip, 0);
        bool on = V.cat == i;
        gd_round_border(chip, h * 0.5f, 1, on ? gui_rgba_mul(TH->select, 0.9f) : (in.hot ? TH->hover : gui_rgba_mul(TH->slot_bg, 0.8f)), on ? TH->select : TH->slot_border);
        gw_label_caps(chip, name, fs, on ? TH->text_dark : (in.hot ? TH->text : TH->text_dim), GUI_CENTER);
        if (in.clicked) { V.cat = on ? 0 : i; gui_sound(TH->snd_click); }
        x += w + gap;
    }
    GuiInteract ci = gui_interact(gui_id("inv_compact"), cr, 0);
    gd_round_border(cr, gui_s(TH->radius), 1, ci.hot ? TH->hover : gui_rgba_mul(TH->slot_bg, 0.8f), TH->slot_border);
    gw_label_caps(cr, "Compact", fs, ci.hot ? TH->text : TH->text_dim, GUI_CENTER);
    if (ci.hovered) gui_tooltip(gui_id("inv_compact"), "Pack the backpack: largest items first");
    if (ci.clicked) {
        InvIntent s;
        memset(&s, 0, sizeof(s));
        s.type = II_COMPACT;
        s.dst_side = INV_SIDE_SELF;
        inv_screen_send(s);
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
    if (cd && !strcmp(cd->id, "wardrobe")) return "Wardrobe";
    if (cd && !strcmp(cd->id, "stash")) return "Stash";
    return "Crate";
}

static void cells_note(char *b, size_t n, const InvTree *t, u16 grid) {
    const ItemDB *db = items();
    int cc, cr;
    inv_grid_size(t, db, &cc, &cr);
    snprintf(b, n, "%d / %d cells", inv_cells_used(t, db, grid), cc * cr);
}

// The world container (above the player's grid): its name and Take all, a body's equipment row, its grid.
static void draw_loot(float oy) {
    InvLayout *L = &V.L;
    InvTree *t = game_loot_tree();
    const ItemDB *db = items();
    GuiRect r = L->loot;
    r.y += oy;
    gw_panel(r, true);
    GuiRect c = gui_rect(r.x + L->pad, r.y, r.w - 2 * L->pad, r.h);
    if (!t) {
        gw_label(c, "Opening\xE2\x80\xA6", tpx(TH->t_body), GW_REGULAR, TH->text_faint, GUI_CENTER);
        return;
    }
    u16 grid = inv_grid(t, db);
    char note[48] = "";
    if (grid != INV_NO_GRID) cells_note(note, sizeof(note), t, grid);
    panel_title(gui_rect(c.x, c.y, c.w, L->title_h), loot_name(), note);
    float y = c.y + L->title_h;
    if (inv_root_def(t, db)->equip) {
        // A body: what it wears as tiles (a corpse is looted, not dressed).
        float q = 1.6f * L->cell, x = c.x;
        for (int sl = 0; sl < EQ_COUNT; sl++) {
            const InvItem *it = inv_equipped_c(t, sl);
            if (!it) continue;
            if (x + q > c.x + c.w) break;
            GuiRect er = { x, y, q, q };
            InvTarget me = { IT_EQUIP, INV_SIDE_LOOT, 0, (i8)sl, 0, it->uid, 0, 0 };
            hit_add(HK_EQUIP, er, L->view, me, 0);
            if (gui_rect_contains(L->view, er.x + q * 0.5f, er.y + q * 0.5f)) pad_add(me, er, PR_LOOT);
            draw_item_tile(er, it, INV_SIDE_LOOT, inv_cat_dims(V.cat, item_def(db, it->s.item)), me);
            if (V.hover.kind == IT_EQUIP && V.hover.side == INV_SIDE_LOOT && V.hover.index == sl && V.dd.state == IDD_IDLE && !V.ctx.open)
                tooltip_for(gui_idi("inv_leq", sl), it);
            x += q + L->gap;
        }
        y += q + L->gap;
    }
    if (grid != INV_NO_GRID) draw_grid(gui_rect(c.x, y, 0, 0), L->view, INV_SIDE_LOOT, grid, inv_parent_def(t, db, grid), PR_LOOT);
    GuiRect ta = { c.x + c.w - gui_s(120), r.y + r.h - gui_s(TH->button_h + 6), gui_s(120), gui_s(TH->button_h) };
    if (gui_rect_contains(L->view, ta.x, ta.y) && gw_button(gui_id("inv_take_all"), ta, "Take all", GB_PRIMARY, true)) {
        InvIntent in;
        memset(&in, 0, sizeof(in));
        in.type = II_TAKE_ALL;
        in.src_side = in.dst_side = INV_SIDE_LOOT;
        int left = 0;
        int rr = dry_run_left(&in, &left);
        // Take all takes what fits: say so when some of it stays (no room in the backpack).
        if (inv_screen_send(in) == IIR_OK && rr == IIR_OK && left > 0) {
            char msg[64];
            if (left == 1) snprintf(msg, sizeof(msg), "No room for 1 item");
            else snprintf(msg, sizeof(msg), "No room for %d items", left);
            inv_screen_toast(msg);
        }
    }
}

// The player's grid: the worn backpack's, titled with its name and size.
static void draw_own_grid(float oy) {
    InvLayout *L = &V.L;
    Player *p = game_local_player();
    const ItemDB *db = items();
    GuiRect r = L->grid;
    r.y += oy;
    gw_panel(r, true);
    GuiRect c = gui_rect(r.x + L->pad, r.y, r.w - 2 * L->pad, r.h);
    u16 grid = inv_grid(&p->inv.t, db);
    if (grid == INV_NO_GRID) {
        gw_label(c, "No backpack", tpx(TH->t_body), GW_REGULAR, TH->text_faint, GUI_CENTER);
        return;
    }
    const InvItem *pk = inv_get_c(&p->inv.t, grid);
    char note[48];
    cells_note(note, sizeof(note), &p->inv.t, grid);
    panel_title(gui_rect(c.x, c.y, c.w, L->title_h), pk ? item_def(db, pk->s.item)->name : "Backpack", note);
    draw_grid(gui_rect(c.x, c.y + L->title_h, 0, 0), L->view, INV_SIDE_SELF, grid, inv_parent_def(&p->inv.t, db, grid), PR_GRID);
}

static void draw_right(void) {
    InvLayout *L = &V.L;
    Game *g = g_();
    draw_cats();
    gui_block_rect(L->right);
    gw_scroll_begin(&V.right_scroll, gui_id("inv_right_scroll"), L->view);
    float oy = gw_scroll_y(&V.right_scroll) - L->view.y;
    if (g->container_open) draw_loot(oy);
    draw_own_grid(oy);
    gw_scroll_end(&V.right_scroll, L->content_h + gui_s(8));
}

// ------------------------------------------------------------------ context menu / split dialog

static void draw_context(void) {
    if (!V.ctx.open) return;
    const ItemDB *db = items();
    InvTarget at = V.ctx.at;
    int side = at.side;
    const InvItem *it = item_of(side, at.uid);
    if (!it) { V.ctx.open = false; return; }
    const ItemDef *d = item_def(db, it->s.item);
    const WeaponDef *wd = item_weapon(db, it->s.item);
    typedef struct Entry { const char *label; int type; int aux; int count; } Entry;
    Entry e[16];
    int n = 0;
    bool equipped = !it->parent && it->loc >= INV_LOC_EQUIP;
    bool mine = side == INV_SIDE_SELF;
    if (mine && (d->kind == ITEM_CONSUMABLE || d->use_type != INV_USE_NONE) && !d->vehicle_use) e[n++] = (Entry){ "Use", II_USE, 0, 0 };
    if (mine && inv_holdable(d)) e[n++] = in_hands(side, it->uid) ? (Entry){ "Put away", II_HOLD, 1, 0 } : (Entry){ "Hold", II_HOLD, 0, 0 };
    if (d->equip_slot != EQ_NONE && !(mine && equipped) && !d->default_pack) e[n++] = (Entry){ "Equip", II_EQUIP, 255, 0 };
    if (mine && equipped && !d->default_pack) e[n++] = (Entry){ "Unequip", II_MOVE, 0, 0 };
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
    if ((side == INV_SIDE_LOOT || game_loot_tree()) && !d->default_pack) e[n++] = (Entry){ side == INV_SIDE_LOOT ? "Take" : "Put in container", II_QUICK_MOVE, 0, 0 };
    if (!d->default_pack) {
        e[n++] = (Entry){ it->s.count > 1 ? "Discard one" : "Discard", II_DISCARD, 0, it->s.count > 1 ? 1 : 0 };
        if (it->s.count > 1) e[n++] = (Entry){ "Discard all", II_DISCARD, 0, 0 };
    }
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
    gw_label_caps(gui_rect(r.x + gui_s(14), r.y + gui_s(6), r.w - gui_s(28), gui_s(20)), d->name, tpx(TH->t_label), TH->text_faint, GUI_LEFT);
    for (int i = 0; i < n; i++) {
        GuiRect ir = { r.x + gui_s(5), r.y + gui_s(28) + ih * (float)i, r.w - gui_s(10), ih };
        GuiInteract in = gui_interact(gui_idi("inv_ctx", i), ir, GUI_FOCUSABLE | (i == 0 ? GUI_DEFAULT_FOCUS : 0));
        if (in.hot) gd_round(ir, gui_s(TH->radius), TH->hover);
        bool danger = e[i].type == II_DISCARD;
        gw_label(gui_rect(ir.x + gui_s(10), ir.y, ir.w - gui_s(20), ir.h), e[i].label, tpx(TH->t_body - 1), GW_MEDIUM,
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
        if (e[i].type == -2) { select_item(at); break; }
        if (e[i].type == II_EQUIP) x.dst_side = INV_SIDE_SELF;
        if (e[i].type == II_MOVE) { x.dst = INV_AUTO; x.dst_side = INV_SIDE_SELF; }   // unequip: into the grid
        if (e[i].type == II_HOLD) {
            x.aux = 0;
            if (e[i].aux) x.src = 0;   // put away
            if (inv_screen_send(x) == IIR_OK && !e[i].aux) game_select(INV_SEL_HANDS);
            break;
        }
        if (e[i].type == II_SPLIT) {
            V.split.open = true;
            V.split.max = it->s.count - 1;
            V.split.count = (float)(it->s.count / 2);
            x.dst = INV_AUTO;
            V.split.in = x;
            break;
        }
        if (e[i].type == II_UNMOUNT) {
            for (int k = 0; k < ATT_SLOTS; k++)
                if (it->s.att[k]) { InvIntent u = x; u.aux = (u8)k; u.dst = INV_AUTO; inv_screen_send(u); }
            break;
        }
        if (e[i].type == II_DISCARD && !e[i].count) {
            InvDDOut o;
            memset(&o, 0, sizeof(o));
            o.action = DG_MAX(1, d->max_stack) <= 1 && (wd || d->equip_slot != EQ_NONE) ? IDA_CONFIRM : IDA_INTENT;
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
    gw_label_caps(gui_rect(r.x + gui_s(16), r.y + gui_s(12), r.w - gui_s(32), gui_s(20)), "Split stack", tpx(TH->t_label), TH->text, GUI_LEFT);
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
    // The columns and the panels: dropping on them does nothing (only outside the screen's body discards).
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
        if (nx >= 0) {
            V.pad_cur = nx;
            if (V.pad_region[nx] == PR_GRID || V.pad_region[nx] == PR_LOOT) {   // keep the cursor in view
                GuiRect r = V.pad_r[nx];
                gw_scroll_to(&V.right_scroll, r.y - gw_scroll_y(&V.right_scroll), r.h);
            }
        }
    }
    // LB / RB: the first target of the previous / next region (slots, chips, loot, grid, inspect).
    for (int dir = -1; dir <= 1; dir += 2) {
        int nav = dir < 0 ? NAV_TAB_PREV : NAV_TAB_NEXT;
        if (!gui_nav(nav)) continue;
        gui_nav_consume(nav);
        int cur = V.pad_region[V.pad_cur];
        for (int step = 1; step <= PR_COUNT; step++) {
            int want = (cur + dir * step + PR_COUNT * 4) % PR_COUNT;
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
    if (gui_nav(NAV_ALT2) && ft.uid && !V.dd.carry && ft.kind != IT_ATTACH) {
        gui_nav_consume(NAV_ALT2);
        InvIntent q;
        memset(&q, 0, sizeof(q));
        q.type = II_QUICK_MOVE;
        q.src_side = ft.side;
        q.src = ft.uid;
        inv_screen_send(q);
    }
    if (gui_nav(NAV_BACK) && V.dd.carry) { gui_nav_consume(NAV_BACK); idd_reset(&V.dd); }
    if (ft.uid && ft.kind != IT_ATTACH && (V.sel.uid != ft.uid || V.sel.side != ft.side)) select_item(ft);
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
        GuiText t = { tpx(TH->t_small), GW_SEMIBOLD, TH->text, GUI_RIGHT, 0, false, true, 0 };
        gd_text(&t, r.x + r.w - gui_s(4), r.y + r.h - gui_s(18), b);
    }
    gd_set_layer(layer);
}

// ------------------------------------------------------------------ scripted pointer (tests)
// ui_inv_* commands queue pointer steps, one per frame, injected as SDL events before the screen
// reads its input (tools/tests/ui_inventory.cfg). Targets: c:<self|loot>:<grid|parent uid>:<loc>:<x>:<y>,
// e:<equipment slot>, a1:<attachment slot> / a2:<attachment slot> (the primary's / secondary's chips),
// a:<attachment slot> (the inspect panel's), char (the character), discard, out.

typedef struct PStep { int kind; InvTarget t; bool out; bool shift, ctrl, alt, rot; u8 chip_row; } PStep;   // kind: 0 move, 1 down, 2 up, 3 rdown, 4 rup, 5 mods, 6 dclick
static PStep g_steps[128];
static int g_step_head, g_step_tail;

static bool parse_target(const char *s, InvTarget *t, bool *out, u8 *chip_row) {
    memset(t, 0, sizeof(*t));
    *out = false;
    *chip_row = 0;
    if (!strcmp(s, "out")) { *out = true; return true; }
    if (!strcmp(s, "discard")) { t->kind = IT_DISCARD; return true; }
    if (!strcmp(s, "char")) { t->kind = IT_CHAR; return true; }
    char side[8], par[16];
    int loc, x, y;
    if (sscanf(s, "c:%7[a-z]:%15[a-z0-9]:%d:%d:%d", side, par, &loc, &x, &y) == 5) {
        t->kind = IT_CELL;
        t->side = !strcmp(side, "loot") ? INV_SIDE_LOOT : INV_SIDE_SELF;
        // The parent: a uid, or "grid": that side's grid (the worn backpack, a world container's root).
        u16 parent = (u16)atoi(par);
        if (!strcmp(par, "grid")) {
            InvTree *tr = inv_screen_tree(t->side);
            if (!tr || !items()) return false;
            parent = inv_grid(tr, items());
        }
        t->parent = parent;
        t->loc = (u8)loc;
        t->x = (i16)x;
        t->y = (i16)y;
        return true;
    }
    if (!strncmp(s, "e:", 2)) { t->kind = IT_EQUIP; t->index = (i8)equip_slot_from_name(s + 2); return t->index != (i8)EQ_NONE && t->index >= 0; }
    if (!strncmp(s, "a1:", 3) || !strncmp(s, "a2:", 3)) {
        t->kind = IT_ATTACH;
        *chip_row = (u8)(s[1] - '0');
        t->index = (i8)attach_slot_from_name(s + 3);
        return t->index >= 0;
    }
    if (!strncmp(s, "a:", 2)) { t->kind = IT_ATTACH; t->index = (i8)attach_slot_from_name(s + 2); return t->index >= 0; }
    return false;
}

static bool target_pos(const PStep *st, vec2 *p) {
    if (st->out) { *p = v2(gui_w() - 2, gui_h() - 2); return true; }
    Player *pl = game_local_player();
    for (int i = 0; i < V.hit_n; i++) {
        const Hit *h = &V.hits[i];
        if (st->t.kind == IT_CELL && h->kind == HK_GRID && h->t.side == st->t.side && h->t.parent == st->t.parent && h->t.loc == st->t.loc) {
            *p = v2(h->r.x + ((float)st->t.x + 0.5f) * h->cell, h->r.y + ((float)st->t.y + 0.5f) * h->cell);
            return true;
        }
        if (st->t.kind == IT_EQUIP && h->kind == HK_EQUIP && h->t.side == INV_SIDE_SELF && h->t.index == st->t.index) { *p = v2(h->r.x + h->r.w * 0.5f, h->r.y + h->r.h * 0.5f); return true; }
        if (st->t.kind == IT_ATTACH && h->kind == HK_ATTACH && h->t.index == st->t.index) {
            // a1 / a2: the chip of the weapon in the primary / secondary slot; a: the inspect panel's (the last).
            const InvItem *w = st->chip_row && pl ? inv_equipped_c(&pl->inv.t, st->chip_row == 1 ? EQ_PRIMARY : EQ_SECONDARY) : NULL;
            if (st->chip_row && (!w || h->t.uid != w->uid || h->t.side != INV_SIDE_SELF)) continue;
            if (!st->chip_row) {
                bool later = false;
                for (int j = i + 1; j < V.hit_n; j++) later |= V.hits[j].kind == HK_ATTACH && V.hits[j].t.index == st->t.index;
                if (later) continue;
            }
            *p = v2(h->r.x + h->r.w * 0.5f, h->r.y + h->r.h * 0.5f);
            return true;
        }
        if ((st->t.kind == IT_DISCARD && h->kind == HK_DISCARD) || (st->t.kind == IT_CHAR && h->kind == HK_CHAR)) { *p = v2(h->r.x + h->r.w * 0.5f, h->r.y + h->r.h * 0.5f); return true; }
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
    if (target && !parse_target(target, &s.t, &s.out, &s.chip_row)) { log_warn("ui_inv: bad target '%s'", target); return; }
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
    if (argc < 3) { log_info("usage: ui_inv_drag <from> <to> [shift] [rot]  (targets: c:<self|loot>:<grid|parent uid>:<loc>:<x>:<y>, e:<slot>, a1:/a2:/a:<attachment slot>, char, discard, out)"); return; }
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

// ui_inv_cat <all|weapons|...>: the category bar (tests); logs which items stay lit.
static void cmd_cat(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: ui_inv_cat <all|weapons|attachments|medical|throwables|tools|misc>"); return; }
    int c = inv_category_from_name(argv[1]);
    V.cat = c >= 0 ? c + 1 : 0;
    Player *p = game_local_player();
    const ItemDB *db = items();
    if (!p || !db) return;
    int lit = 0, dim = 0;
    for (int i = 0; i < INV_MAX_ITEMS; i++) {
        const InvItem *it = &p->inv.t.items[i];
        if (!inv_alive(it) || it->loc >= INV_LOC_EQUIP) continue;
        if (inv_cat_dims(V.cat, item_def(db, it->s.item))) dim++;
        else lit++;
    }
    log_info("ui_inv_cat %s: %d lit, %d dimmed", V.cat ? argv[1] : "all", lit, dim);
}

void inventory_register_commands(void) {
    cmd_register("ui_inv_drag", cmd_drag, "ui_inv_drag <from> <to> [shift] [rot]: scripted inventory drag (targets: c:<self|loot>:<grid|uid>:<loc>:<x>:<y>, e:<slot>, a1:/a2:/a:<attachment slot>, char, discard, out)");
    cmd_register("ui_inv_click", cmd_click, "ui_inv_click <target> [ctrl|alt|shift|right|double]: scripted inventory click");
    cmd_register("ui_inv_cat", cmd_cat, "ui_inv_cat <category|all>: the inventory category bar (highlight only, tests)");
    inv_cells_init();
    inv_editor_init();
}

// ------------------------------------------------------------------ screen

void inventory_open(void) {
    idd_reset(&V.dd);
    V.ctx.open = V.split.open = false;
    memset(&V.sel, 0, sizeof(V.sel));
    V.toast[0] = 0;
    V.char_yaw = 0;
    gui_anim_restart(gui_id("inv_open"));
}

void inventory_close(void) {
    idd_reset(&V.dd);
    V.ctx.open = V.split.open = false;
    V.char_drag = false;
}

// Esc / B closes the context menu or the split dialog first; Tab / I close everything.
bool inventory_wants_back(void) { return V.dd.state == IDD_IDLE && !V.dd.carry && !V.ctx.open && !V.split.open; }

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
    run_step();
    // Layout.
    InvLayoutIn li;
    memset(&li, 0, sizeof(li));
    LayoutParams lp = gui_layout_params();
    li.safe = gui_layout_safe_rect(&lp);
    li.scale = gui_scale();
    inv_grid_size(&p->inv.t, db, &li.grid_cols, &li.grid_rows);
    InvTree *lt = game_loot_tree();
    li.loot = g->container_open;
    if (lt) {
        inv_grid_size(lt, db, &li.loot_cols, &li.loot_rows);
        li.loot_body = inv_root_def(lt, db)->equip;
    }
    inv_layout(&li, &V.L);
    InvLayout *L = &V.L;
    V.hit_n = 0;
    V.pad_n = 0;
    float a = gui_ease_out(gui_anim_t(gui_id("inv_open")) / TH->modal_in);
    gd_alpha_push(a);
    // The game keeps rendering behind (first person, the camera unmoved) under the UI blur.
    gd_backdrop(gui_rect(0, 0, gui_w(), gui_h()), WHITE);
    gd_rect(gui_rect(0, 0, gui_w(), gui_h()), TH->scrim);
    draw_character();
    // Header: title, sync state.
    GuiText title = { tpx(TH->t_title), GW_SEMIBOLD, TH->text, GUI_LEFT, 0.02f, true, true, 0 };
    gd_text(&title, L->header.x, L->header.y + gui_s(6), "Inventory");
    if (game_inv_pending()) {
        GuiText ht = { tpx(TH->t_small + 1), GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, true, 0 };
        gd_text(&ht, L->header.x + L->header.w, L->header.y + gui_s(14), "syncing\xE2\x80\xA6");
    }
    gd_rect(gui_rect(L->header.x, L->header.y + L->header.h - 1, L->header.w, 1), gui_rgba_mul(TH->panel_border, 0.8f));
    // Columns.
    draw_left();
    draw_right();
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
        GuiText t = { tpx(TH->t_small + 1), GW_SEMIBOLD, gui_rgba_mul(TH->danger, ta), GUI_LEFT, 0, false, true, 0 };
        gd_text(&t, L->footer.x, L->footer.y + gui_s(2), V.toast);
    }
    screens_footer_hint(L->footer, gui_nav_mode() ? "A  pick up / place    X  rotate / menu    Y  quick move    LB RB  regions    B  back"
                                                  : "Drag  move    R  rotate    Ctrl+click  quick move    Alt+click  equip    Shift+drag  split    Right click  menu    Tab  close");
    draw_context();
    draw_split();
    gd_alpha_pop();
    inv_editor_draw();
    draw_ghost();
    return false;
}
