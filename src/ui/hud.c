// HUD (docs/UI.md "HUD"): vitals bottom left, ammo bottom right, the hotbar (the inventory's slot
// widget) bottom centre, the interaction prompt under the crosshair, hints, kill feed top right.
// Everything sits in the UI safe area and hides while a game screen is open. The crosshair and
// hit markers are crosshair.c; the developer overlays keep the small debug font. All of it shows
// the *viewed* player: a spectated player's HUD exactly as they see it, plus the spectator bar
// (docs/SPECTATE.md "Spectator HUD").
#include "hud.h"
#include "gameplay/phone/phone.h"
#include "hud/hud_internal.h"
#include "crosshair.h"
#include "menus.h"
#include "ui.h"
#include "icons.h"
#include "gui/gui_widgets.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "game/game.h"
#include "gameplay/weapons.h"
#include "gameplay/effects.h"
#include "gameplay/attachments.h"
#include "render/render.h"
#include "vfx/vfx.h"
#include "net/net.h"
#include "input/input.h"
#include "player/spectator.h"
#include "gameplay/revive/revive.h"
#include "widgets/timed_ring.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define COL_PANEL UI_RGBA(10, 12, 14, 150)

void inventory_slot_view(const InvSlot *s, const ItemDB *db, int mag_override, GuiSlotView *v);   // scr_inventory.c

static float s_compass_bottom;
float hud_compass_bottom(void) { return s_compass_bottom; }

static GuiRect safe_rect(void) {
    LayoutParams lp = gui_layout_params();
    return gui_layout_safe_rect(&lp);
}

static void caps(float x, float y, float size, u32 c, int align, const char *s) { hud_caps(x, y, size, c, align, s); }

static void bar(GuiRect r, float frac, u32 fill, u32 back) {
    gd_round(r, r.h * 0.5f, back);
    if (frac > 0.002f) gd_round(gui_rect(r.x, r.y, fmaxf(r.h, r.w * saturate(frac)), r.h), r.h * 0.5f, fill);
}

// The player whose HUD is drawn: the local one, or the one spectated (none on the free camera).
static Player *hud_player(void) { return spectator_mode() == SPEC_FREE ? NULL : game_view_player(); }

static void draw_damage_indicators(Game *g) {
    float cx = gui_w() * 0.5f, cy = gui_h() * 0.5f;
    Player *p = hud_player();
    if (!p) return;
    for (int i = 0; i < MAX_HURT_MARKERS; i++) {
        HurtMarker *h = &g->hurt[i];
        if (h->time <= 0) continue;
        vec3 to = v3_sub(h->from, p->sim.move.pos);
        float ang = atan2f(-to.x, -to.z) - g->view.yaw;  // 0 = in front
        float a = saturate(h->time / 1.5f);
        float r = gui_s(120);
        float x = cx - sinf(ang) * r, y = cy - cosf(ang) * r;
        float tx = -sinf(ang), ty = -cosf(ang);
        float px = -ty, py = tx;
        u32 c = UI_RGBA(230, 52, 40, (int)(220 * a));
        gd_line(x + px * gui_s(18), y + py * gui_s(18), x + tx * gui_s(12), y + ty * gui_s(12), gui_s(4), c);
        gd_line(x - px * gui_s(18), y - py * gui_s(18), x + tx * gui_s(12), y + ty * gui_s(12), gui_s(4), c);
    }
    if (g->damage_flash > 0) {
        float a = g->damage_flash * 0.35f;
        float bw = gui_s(70);
        u32 c = UI_RGBA(200, 0, 0, (int)(255 * a)), z = UI_RGBA(200, 0, 0, 0);
        gd_rect_grad(gui_rect(0, 0, gui_w(), bw), c, z);
        gd_rect_grad(gui_rect(0, gui_h() - bw, gui_w(), bw), z, c);
        gd_rect_hgrad(gui_rect(0, 0, bw, gui_h()), c, z);
        gd_rect_hgrad(gui_rect(gui_w() - bw, 0, bw, gui_h()), z, c);
    }
}

static void draw_vitals(World *w, GuiRect safe) {
    Player *p = hud_player();
    if (!p) return;
    Health *h = &w->health[ENT_INDEX(p->ent)];
    float bw = gui_s(250), x = safe.x, y = safe.y + safe.h - gui_s(58);
    float hp = h->hp / fmaxf(1, h->max_hp);
    gd_shadow(gui_rect(x, y - gui_s(2), bw + gui_s(60), gui_s(44)), 0, gui_s(30), UI_RGBA(0, 0, 0, 55));
    caps(x, y - gui_s(6), gui_s(TH->t_label), TH->text_dim, GUI_LEFT, "Health");
    char b[16];
    snprintf(b, sizeof(b), "%.0f", fmaxf(0.0f, h->hp));
    GuiText num = { gui_s(26), GW_SEMIBOLD, hp > 0.3f ? TH->hud_text : TH->danger, GUI_LEFT, 0, false, true, 0 };
    gd_text(&num, x + bw + gui_s(12), y + gui_s(4), b);
    float low = hp < 0.3f ? 0.6f + 0.4f * sinf((float)gui_time() * 6.0f) : 1.0f;
    bar(gui_rect(x, y + gui_s(16), bw, gui_s(8)), hp, gui_rgba_mul(hp > 0.3f ? UI_RGBA(232, 232, 226, 235) : TH->danger, low), UI_RGBA(255, 255, 255, 30));
    if (h->max_armor > 0) {
        bar(gui_rect(x, y + gui_s(30), bw, gui_s(4)), h->armor / fmaxf(1, h->max_armor), TH->info, UI_RGBA(255, 255, 255, 20));
        if (h->armor > 0) {
            snprintf(b, sizeof(b), "%.0f", h->armor);
            GuiText an = { gui_s(TH->t_small), GW_MEDIUM, TH->info, GUI_LEFT, 0, false, true, 0 };
            gd_text(&an, x + bw + gui_s(12), y + gui_s(31), b);
        }
    }
}

static const char *hud_fire_mode_name(int m) { return m == FM_SINGLE ? "Single" : (m == FM_BURST ? "Burst" : "Auto"); }

static void draw_ammo(Game *g, World *w, GuiRect safe) {
    Player *p = hud_player();
    if (!p || p->sim.move.mode == MOVE_DEAD) return;
    const InvSlot *slot = weapon_equipped(w, game_view_slot());
    if (!slot) return;
    const ItemDef *it = item_def(w->items, slot->item);
    const WeaponDef *wd = item_weapon(w->items, slot->item);
    float x = safe.x + safe.w, y = safe.y + safe.h - gui_s(84);
    gd_shadow(gui_rect(x - gui_s(220), y + gui_s(4), gui_s(220), gui_s(72)), 0, gui_s(30), UI_RGBA(0, 0, 0, 55));
    caps(x, y, gui_s(TH->t_label), TH->text_dim, GUI_RIGHT, it->name);
    float cx = gui_w() * 0.5f, cy = gui_h() * 0.5f;
    if (wd && wd->kind != WPN_MELEE) {
        WeaponDef eff;
        attach_weapon_effective(w->items, wd, slot->att, &eff);
        int reserve = inv_count(&p->inv, wd->ammo_item);
        int mag = p->sim.wpn.mag;
        char b[32];
        snprintf(b, sizeof(b), "/ %d", reserve);
        GuiText rs = { gui_s(22), GW_MEDIUM, TH->text_dim, GUI_RIGHT, 0, false, true, 0 };
        float rw = gd_text(&rs, x, y + gui_s(30), b);
        snprintf(b, sizeof(b), "%d", mag);
        float lowf = eff.mag_size > 0 ? (float)mag / (float)eff.mag_size : 1.0f;
        GuiText mg = { gui_s(TH->t_huge), GW_SEMIBOLD, mag == 0 ? TH->danger : (lowf < 0.25f ? TH->warning : TH->hud_text), GUI_RIGHT, 0, false, true, 0 };
        gd_text(&mg, x - rw - gui_s(8), y + gui_s(14), b);
        // Magazine ticks (up to 40), then a bar.
        float ty = y + gui_s(76);
        if (eff.mag_size > 0 && eff.mag_size <= 40) {
            float tw = gui_s(3), tg = gui_s(2);
            for (int i = 0; i < eff.mag_size; i++) {
                float tx = x - (float)(i + 1) * (tw + tg);
                gd_rect(gui_rect(tx, ty, tw, gui_s(8)), i < mag ? UI_RGBA(232, 232, 226, 220) : UI_RGBA(255, 255, 255, 30));
            }
        } else if (eff.mag_size > 0) {
            bar(gui_rect(x - gui_s(160), ty + gui_s(2), gui_s(160), gui_s(4)), lowf, UI_RGBA(232, 232, 226, 220), UI_RGBA(255, 255, 255, 30));
        }
        if (wd->recoil.fire_modes && (wd->recoil.fire_modes & (wd->recoil.fire_modes - 1)))
            caps(x - gui_s(170), y, gui_s(TH->t_label), TH->accent, GUI_RIGHT, hud_fire_mode_name(p->sim.wpn.fire_mode));
        if (p->sim.wpn.reloading) {
            float frac = 1.0f - p->sim.wpn.reload_t / fmaxf(0.01f, eff.reload_time);
            gd_ring(cx, cy, gui_s(22), gui_s(2.5f), frac, gui_rgba_mul(TH->accent, 0.9f));
            caps(cx, cy + gui_s(30), gui_s(TH->t_label), TH->hud_text, GUI_CENTER, "Reloading");
        } else if (mag == 0) {
            char key[32], msg[64];
            binding_name(input_get_binding(ACT_RELOAD, 0), key, sizeof(key));
            snprintf(msg, sizeof(msg), reserve > 0 ? "%s  Reload" : "No ammo", key);
            caps(cx, cy + gui_s(40), gui_s(TH->t_small), reserve > 0 ? TH->warning : TH->danger, GUI_CENTER, msg);
        }
    } else if (it->kind == ITEM_CONSUMABLE) {
        char b[16];
        snprintf(b, sizeof(b), "%d", slot->count);
        GuiText mg = { gui_s(TH->t_huge * 0.8f), GW_SEMIBOLD, TH->hud_text, GUI_RIGHT, 0, false, true, 0 };
        gd_text(&mg, x, y + gui_s(16), b);
        // The use itself shows on the timed ring (game_wheels.c).
        if (p->sim.wpn.use_t < 0) caps(x, y + gui_s(70), gui_s(TH->t_tiny), TH->text_faint, GUI_RIGHT, "Hold fire to use");
    } else if (it->kind == ITEM_THROWABLE) {
        char b[16];
        snprintf(b, sizeof(b), "%d", slot->count);
        GuiText mg = { gui_s(TH->t_huge * 0.8f), GW_SEMIBOLD, TH->hud_text, GUI_RIGHT, 0, false, true, 0 };
        gd_text(&mg, x, y + gui_s(16), b);
        if (p->sim.wpn.use_t < 0)
            caps(x, y + gui_s(70), gui_s(TH->t_tiny), TH->text_faint, GUI_RIGHT, it->thr.kind == THROW_GRENADE ? "Hold fire to cook, release to throw" : "Hold fire, release to throw");
    } else if (wd) {
        char b[32];
        snprintf(b, sizeof(b), "%.0f dmg", wd->damage);
        caps(x, y + gui_s(30), gui_s(TH->t_small), TH->text_dim, GUI_RIGHT, b);
    }
}

static void draw_hotbar(Game *g, World *w, GuiRect safe) {
    Player *p = hud_player();
    if (!p) return;
    // The selection: the local hotbar key, or what a spectated player has equipped.
    int hot = p->local ? g->hotbar_slot : (p->sim.wpn.slot < INV_HOTBAR ? p->sim.wpn.slot : g->hotbar_slot);
    float sz = gui_s(TH->slot * 0.82f), gap = gui_s(TH->slot_gap);
    float total = INV_HOTBAR * sz + (INV_HOTBAR - 1) * gap;
    float x0 = gui_w() * 0.5f - total * 0.5f, y = safe.y + safe.h - sz;
    for (int i = 0; i < INV_HOTBAR; i++) {
        GuiRect r = { x0 + (float)i * (sz + gap), y, sz, sz };
        bool sel = i == hot;
        float lift = gui_tween(gui_idi("hud_hot", i), sel ? 1.0f : 0.0f, 0.1f);
        r.y -= lift * gui_s(6);
        GuiSlotView v;
        inventory_slot_view(&p->inv.slots[i], w->items, i == p->sim.wpn.slot ? p->sim.wpn.mag : -1, &v);
        v.hotkey = i + 1;
        v.selected = sel;
        gd_alpha_push(sel ? 1.0f : 0.78f);
        gd_round(r, gui_s(TH->radius), UI_RGBA(0, 0, 0, 70));
        gw_slot(r, &v);
        gd_alpha_pop();
    }
    // Name of the selected item, fading out after a switch.
    const InvSlot *s = &p->inv.slots[hot];
    static int last = -1;
    static double since;
    if (last != hot || !s->item) { last = hot; since = gui_time(); }
    float a = 1.0f - saturate((float)(gui_time() - since - 1.2) / 0.4f);
    if (s->item && a > 0.01f) {
        gd_alpha_push(a);
        caps(gui_w() * 0.5f, y - gui_s(34), gui_s(TH->t_small), TH->hud_text, GUI_CENTER, item_def(w->items, s->item)->name);
        gd_alpha_pop();
    }
}

static void draw_prompt(Game *g) {
    if (game_view_slot() != g->local_slot) {
        // A spectated player's hold interaction: its progress ring (the prompt text is theirs).
        if (g->hold_progress > 0.001f) gd_ring(gui_w() * 0.5f, gui_h() * 0.5f + gui_s(77), gui_s(22), gui_s(2.5f), g->hold_progress, TH->accent);
        return;
    }
    if (!g->focus_prompt[0]) return;
    float cx = gui_w() * 0.5f, y = gui_h() * 0.5f + gui_s(64);
    char key[32] = "";
    const char *text = g->focus_prompt;
    if (text[0] == '[') {
        const char *e = strchr(text, ']');
        if (e) {
            snprintf(key, sizeof(key), "%.*s", (int)(e - text - 1), text + 1);
            text = e + 1;
            while (*text == ' ') text++;
        }
    }
    GuiText t = { gui_s(TH->t_body), GW_MEDIUM, TH->hud_text, GUI_LEFT, 0, false, true, 0 };
    float tw = gd_text_width(&t, text);
    float ks = gui_s(26);
    GuiText kt = { gui_s(TH->t_small), GW_SEMIBOLD, TH->text_dark, GUI_CENTER, 0, false, false, 0 };
    float kw = key[0] ? fmaxf(ks, gd_text_width(&kt, key) + gui_s(12)) : 0;
    float total = kw + (key[0] ? gui_s(10) : 0) + tw;
    float x = cx - total * 0.5f;
    if (key[0]) {
        GuiRect kr = { x, y, kw, ks };
        gd_round(kr, gui_s(4), UI_RGBA(236, 236, 230, 235));
        gd_text(&kt, kr.x + kr.w * 0.5f, kr.y + (ks - gd_line_height(kt.size)) * 0.5f, key);
        if (g->hold_progress > 0.001f) gd_ring(kr.x + kr.w * 0.5f, kr.y + ks * 0.5f, ks * 0.85f, gui_s(2.5f), g->hold_progress, TH->accent);
        x += kw + gui_s(10);
    }
    gd_text(&t, x, y + (ks - gd_line_height(t.size)) * 0.5f, text);
}

static void draw_hints(Game *g, GuiRect safe) {
    float y = gui_h() * 0.64f;
    for (int i = 0; i < MAX_HINTS; i++) {
        Hint *h = &g->hints[i];
        if (h->time <= 0) continue;
        float a = saturate(h->time);
        GuiText t = { gui_s(TH->t_body), GW_MEDIUM, gui_rgba_mul(UI_RGBA(245, 232, 205, 255), a), GUI_CENTER, 0, false, true, 0 };
        gd_text(&t, gui_w() * 0.5f, y, h->text);
        y += gui_s(24);
    }
}

static void key_name(Action a, char *out, int size) { binding_name(input_get_binding(a, 0), out, size); }

static void draw_recovery(World *w, int slot, float y);

// Spectator bar (top centre): who is watched and how, the delay readout, the controls; the respawn
// countdown / Respawn bottom centre; the death camera countdown before spectating starts.
static void draw_spectator(GuiRect safe) {
    SpectatorHud sh;
    if (!spectator_hud(&sh)) return;
    float cx = gui_w() * 0.5f;
    char k1[32], k2[32], k3[32], msg[160];
    if (sh.mode != SPEC_OFF) {
        static const char *modes[] = { "", "Spectating - first person", "Spectating - third person", "Spectating - free camera" };
        float y = safe.y + gui_s(8);
        float pw = gui_s(440), ph = gui_s(sh.show_delay ? 104 : 86);
        GuiRect r = { cx - pw * 0.5f, y, pw, ph };
        gd_shadow(r, gui_s(TH->radius), gui_s(30), UI_RGBA(0, 0, 0, 60));
        gd_round(r, gui_s(TH->radius), TH->hud_panel);
        caps(cx, y + gui_s(10), gui_s(TH->t_label), sh.first_person_denied ? TH->warning : TH->accent, GUI_CENTER, modes[DG_CLAMP(sh.mode, 0, 3)]);
        GuiText nm = { gui_s(TH->t_body + 6), GW_SEMIBOLD, TH->hud_text, GUI_CENTER, 0, false, true, 0 };
        gd_text(&nm, cx, y + gui_s(28), sh.mode == SPEC_FREE ? "Free camera" : (sh.name[0] ? sh.name : "..."));
        GuiText ar = { gui_s(TH->t_body + 6), GW_SEMIBOLD, TH->text_dim, GUI_CENTER, 0, false, true, 0 };
        gd_text(&ar, r.x + gui_s(24), y + gui_s(28), "<");
        gd_text(&ar, r.x + r.w - gui_s(24), y + gui_s(28), ">");
        key_name(ACT_AIM, k1, sizeof(k1));
        key_name(ACT_ATTACK, k2, sizeof(k2));
        key_name(ACT_USE, k3, sizeof(k3));
        snprintf(msg, sizeof(msg), "%s Prev   %s Next   %s Mode   1-8 Player   Esc Menu", k1, k2, k3);
        caps(cx, y + gui_s(62), gui_s(TH->t_tiny), TH->text_faint, GUI_CENTER, msg);
        if (sh.show_delay) {
            // Honest floor: the target's input crosses one hop to the host and one to here.
            snprintf(msg, sizeof(msg), "Spec delay %.0f ms  |  minimum = one hop to the host + one hop to you", sh.delay_ms);
            caps(cx, y + gui_s(80), gui_s(TH->t_tiny), sh.delay_ms > 200 ? TH->warning : TH->text_dim, GUI_CENTER, msg);
        }
    } else if (sh.dead && sh.death_cam_left > 0 && (sh.waiting_respawn || sh.respawn_s > 1.5f)) {
        key_name(ACT_ATTACK, k1, sizeof(k1));
        snprintf(msg, sizeof(msg), "Spectating in %.1f s   %s Skip", sh.death_cam_left, k1);
        caps(cx, gui_h() * 0.36f + gui_s(56), gui_s(TH->t_small), TH->hud_text, GUI_CENTER, msg);
    }
    if (sh.dead) {
        float y = safe.y + safe.h - gui_s(TH->slot * 0.82f) - gui_s(70);
        Game *g = game();
        if (g && g->world) draw_recovery(g->world, g->local_slot, y - gui_s(46));
        if (sh.can_respawn) {
            key_name(ACT_RELOAD, k1, sizeof(k1));
            snprintf(msg, sizeof(msg), "%s  Respawn", k1);
            caps(cx, y, gui_s(TH->t_body), TH->ok, GUI_CENTER, msg);
        } else if (sh.respawn_s > 0) {
            snprintf(msg, sizeof(msg), "Respawn in %.0f", ceilf(sh.respawn_s));
            caps(cx, y, gui_s(TH->t_small), TH->text_dim, GUI_CENTER, msg);
            bar(gui_rect(cx - gui_s(80), y + gui_s(22), gui_s(160), gui_s(4)), 1.0f - fminf(1.0f, sh.respawn_s / 30.0f), TH->accent, UI_RGBA(255, 255, 255, 30));
        }
    }
}

// ------------------------------------------------------------------ revive loop (docs/REVIVE.md)

// Downed: the bleed-out ring and what to do (the viewed player: spectators see it too).
static void draw_downed(World *w, Player *p) {
    if (!p || !p->sim.down.downed || p->sim.move.mode == MOVE_DEAD) return;
    const DownState *d = &p->sim.down;
    float cx = gui_w() * 0.5f, cy = gui_h() * 0.72f;
    float total = (float)d->bleed_total / (float)REVIVE_HZ, left = (float)d->bleed_ticks / (float)REVIVE_HZ;
    bool helped = d->reviver != REVIVE_NOBODY;
    ui_timed_ring_at(cx, cy, 0.85f, total, left, TR_DRAIN, helped ? "Paused" : "Bleeding out", 1.0f);
    caps(cx, cy - gui_s(70), gui_s(TH->t_body), TH->danger, GUI_CENTER, "Downed");
    char msg[96];
    if (helped && d->reviver < MAX_PLAYERS) snprintf(msg, sizeof(msg), "%s is reviving you", w->players[d->reviver].name);
    else snprintf(msg, sizeof(msg), "Crawl to cover or wait for help");
    caps(cx, cy - gui_s(48), gui_s(TH->t_small), TH->hud_text, GUI_CENTER, msg);
}

// A dead player's body / sample / fabricator progress (the spectator bar, bottom).
static void draw_recovery(World *w, int slot, float y) {
    RecoveryStatus rs;
    if (!revive_recovery_status(w, slot, &rs)) return;
    float cx = gui_w() * 0.5f;
    char msg[96];
    u32 col = TH->hud_text;
    switch (rs.stage) {
    case REC_BODY: snprintf(msg, sizeof(msg), "Your body is waiting: DNA can be extracted"); col = TH->text_dim; break;
    case REC_EXTRACTED: snprintf(msg, sizeof(msg), "DNA extracted by %s", rs.by); col = TH->accent; break;
    case REC_QUEUED: snprintf(msg, sizeof(msg), "In fabricator: queue #%d", rs.queue_pos); col = TH->accent; break;
    case REC_RECONSTRUCTING: snprintf(msg, sizeof(msg), "Reconstructing... %.0f", ceilf(rs.remaining)); col = TH->ok; break;
    case REC_OPENING: snprintf(msg, sizeof(msg), "Reconstructed"); col = TH->ok; break;
    default: return;
    }
    caps(cx, y, gui_s(rs.stage == REC_RECONSTRUCTING ? TH->t_body : TH->t_small), col, GUI_CENTER, msg);
    if (rs.stage == REC_RECONSTRUCTING) bar(gui_rect(cx - gui_s(110), y + gui_s(24), gui_s(220), gui_s(4)), rs.progress, TH->ok, UI_RGBA(255, 255, 255, 30));
}

// Fabricator panels: a world-space plate over each nearby machine (state, player, progress, queue).
static void draw_fabricators(Game *g, World *w) {
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_FABRICATOR) continue;
        EntityId e = ENT_MAKE(i, w->gen[i]);
        vec3 at;
        if (!revive_fab_socket(w, e, "panel", &at, NULL)) continue;
        at.y += 0.55f;
        float dist = v3_dist(at, g->view.eye);
        if (dist > 14.0f) continue;
        vec2 sp;
        if (!render_world_to_screen(at, &sp)) continue;
        FabView fv;
        revive_fab_view(w, e, &fv);
        float k = DG_CLAMP(5.0f / fmaxf(dist, 1.0f), 0.55f, 1.2f);
        float pw = gui_s(190) * k, ph = gui_s(fv.m.qn ? 74 : 58) * k;
        GuiRect r = { sp.x - pw * 0.5f, sp.y - ph, pw, ph };
        gd_alpha_push(saturate((14.0f - dist) / 3.0f));
        gd_round(r, gui_s(TH->radius) * k, TH->hud_panel);
        caps(sp.x, r.y + gui_s(7) * k, gui_s(TH->t_tiny) * k, fv.m.powered ? TH->accent : TH->warning, GUI_CENTER, "Fabricator");
        char msg[96];
        if (!fv.m.powered) snprintf(msg, sizeof(msg), "No power");
        else if (fv.m.state == FAB_RECONSTRUCTING) snprintf(msg, sizeof(msg), "%s  %.0f s", fv.current_name, ceilf(fv.remaining));
        else if (fv.m.state == FAB_OPENING) snprintf(msg, sizeof(msg), "%s  opening", fv.current_name);
        else if (fv.m.state == FAB_COOLDOWN) snprintf(msg, sizeof(msg), "Cooling down %.0f s", ceilf(fv.remaining));
        else snprintf(msg, sizeof(msg), "Ready: insert DNA");
        GuiText t = { gui_s(TH->t_small) * k, GW_SEMIBOLD, TH->hud_text, GUI_CENTER, 0, false, true, 0 };
        gd_text(&t, sp.x, r.y + gui_s(22) * k, msg);
        if (fv.m.state == FAB_RECONSTRUCTING) bar(gui_rect(r.x + gui_s(12) * k, r.y + gui_s(44) * k, pw - gui_s(24) * k, gui_s(3) * k), fv.progress, TH->ok, UI_RGBA(255, 255, 255, 30));
        if (fv.m.qn) {
            char q[80] = "Queue:";
            for (int n = 0; n < fv.m.qn; n++) {
                const DnaSample *ds = dna_get_c(revive_dna(), fv.m.queue[n]);
                size_t l = strlen(q);
                snprintf(q + l, sizeof(q) - l, " %s", ds ? ds->name : "?");
            }
            caps(sp.x, r.y + gui_s(54) * k, gui_s(TH->t_tiny) * k, TH->text_dim, GUI_CENTER, q);
        }
        gd_alpha_pop();
    }
}

static void draw_world_bars(Game *g, World *w, float s) {
    // Floating health bars / names above characters: hidden unless cl_healthbars 1.
    static Cvar *show;
    if (!show) show = cvar_find("cl_healthbars");   // registered in game_init (before the config loads)
    if (!show || !show->i) return;
    vec3 cam = g->view.eye;
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_HEALTH)) continue;
        if (w->type[i] == ET_PLAYER && w->player_slot[i] == game_view_slot()) continue;
        vec3 top = w->type[i] == ET_DUMMY ? v3_add(w->xf[i].pos, v3(0, 1.15f, 0)) : v3_add(w->xf[i].pos, v3(0, 2.05f, 0));
        float d = v3_dist(top, cam);
        if (d > 40.0f) continue;
        vec2 sp;
        if (!render_world_to_screen(top, &sp)) continue;
        Health *h = &w->health[i];
        float bw = 70 * s * clampf(8.0f / d, 0.5f, 1.4f);
        bar(gui_rect(sp.x - bw * 0.5f, sp.y, bw, gui_s(5)), h->hp / fmaxf(1, h->max_hp), (w->flags[i] & EF_DEAD) ? UI_RGBA(90, 90, 90, 255) : TH->danger, UI_RGBA(0, 0, 0, 140));
        if (w->type[i] == ET_PLAYER) {
            GuiText t = { gui_s(TH->t_small), GW_MEDIUM, TH->hud_text, GUI_CENTER, 0, false, true, 0 };
            gd_text(&t, sp.x, sp.y - gui_s(20), w->names[i]);
        }
    }
}

static void draw_dev(Game *g, World *w, float s) {
    float y = 10 * s;
    if (g->cl_showfps->i) {
        RenderStats rs = render_stats();
        ui_textf(10 * s, y, FONT_SMALL, UI_RGBA(200, 255, 200, 230), UI_ALIGN_LEFT, "%.0f fps  %.2f ms  draws %u  tris %uk  culled %u",
                 g_engine.fps_smoothed, 1000.0 / fmax(1.0, g_engine.fps_smoothed), rs.draw_calls, rs.triangles / 1000, rs.culled);
        y += 16 * s;
    }
    if (game_cam_debug_text()[0]) {
        ui_textf(10 * s, y, FONT_SMALL, UI_RGBA(180, 230, 255, 230), UI_ALIGN_LEFT, "%s", game_cam_debug_text());
        y += 16 * s;
    }
    if (vfx_game_stats_text()[0]) {
        ui_textf(10 * s, y, FONT_SMALL, UI_RGBA(255, 220, 160, 230), UI_ALIGN_LEFT, "%s", vfx_game_stats_text());
        y += 16 * s;
    }
    if (g->dev_mode->i) {
        Player *p = game_local_player();
        if (p) {
            const PlayerMove *m = &p->sim.move;
            static const char *modes[] = { "walk", "mantle", "vault", "hang", "noclip", "dead", "seated" };
            ui_textf(10 * s, y, FONT_SMALL, UI_RGBA(220, 220, 255, 220), UI_ALIGN_LEFT,
                     "pos %.2f %.2f %.2f  speed %.2f m/s  %s%s%s  %s  map %s  %s",
                     m->pos.x, m->pos.y, m->pos.z, v3_len(v3_flat(m->vel)), modes[DG_MIN(m->mode, 6)],
                     m->on_ground ? " ground" : " air", m->crouched ? " crouch" : "", m->sprinting ? "sprint" : "",
                     g->map, g->mode == GM_HOST ? (net_role() == NET_HOST ? "host (LAN open)" : "host") : "client");
            y += 16 * s;
        }
    }
    bool show_net = g->show_netstats;
    Cvar *ns = cvar_find("net_showstats");
    if (ns && ns->i) show_net = true;
    if (show_net && net_role() != NET_OFF) {
        NetStats st = net_stats();
        ui_rect(8 * s, y, 330 * s, 100 * s, COL_PANEL);
        y += 4 * s;
        ui_textf(14 * s, y, FONT_SMALL, UI_RGBA(235, 235, 230, 255), UI_ALIGN_LEFT, "net: %s via %s, %d peer(s)", net_role() == NET_HOST ? "host" : "client", st.transport, st.peers); y += 16 * s;
        if (net_role() == NET_CLIENT)
            ui_textf(14 * s, y, FONT_SMALL, UI_RGBA(235, 235, 230, 255), UI_ALIGN_LEFT, "ping %.0f ms (cmd rtt %.0f ms)   loss %.1f%%", st.ping_ms, st.app_rtt_ms, st.loss_pct);
        else
            ui_textf(14 * s, y, FONT_SMALL, UI_RGBA(235, 235, 230, 255), UI_ALIGN_LEFT, "ping %.0f ms   loss %.1f%%", st.ping_ms, st.loss_pct);
        y += 16 * s;
        ui_textf(14 * s, y, FONT_SMALL, UI_RGBA(235, 235, 230, 255), UI_ALIGN_LEFT, "in %.1f kB/s   out %.1f kB/s", st.in_kbps, st.out_kbps); y += 16 * s;
        ui_textf(14 * s, y, FONT_SMALL, UI_RGBA(235, 235, 230, 255), UI_ALIGN_LEFT, "snapshot %.0f B @ %d Hz  interp %.0f ms", st.snapshot_bytes, st.snapshot_rate, st.interp_ms); y += 16 * s;
        ui_textf(14 * s, y, FONT_SMALL, UI_RGBA(235, 235, 230, 255), UI_ALIGN_LEFT, "prediction error %.3f m  inventory intents pending %d", st.prediction_error, game_inv_pending()); y += 16 * s;
    }
}

void hud_draw(void) {
    Game *g = game();
    if (!g || !g->world) return;
    World *w = g->world;
    float s = ui_scale();
    icons_tick();
    draw_dev(g, w, s);
    if (menus_open()) return;   // the HUD hides behind game screens
    GuiRect safe = safe_rect();
    draw_world_bars(g, w, gui_scale());
    draw_fabricators(g, w);
    fx_draw_hud();
    if (!phone_holding()) crosshair_draw((float)g_engine.frame_dt);   // the raised phone is the aim (docs/PHONE.md)
    draw_prompt(g);
    draw_damage_indicators(g);
    draw_vitals(w, safe);
    draw_ammo(g, w, safe);
    draw_hotbar(g, w, safe);
    draw_hints(g, safe);
    hud_pings_draw(g, safe);
    s_compass_bottom = hud_compass_draw(g, safe);
    gd_alpha_push(hud_cvars()->opacity->f);
    hud_kills_draw(g, safe);
    gd_alpha_pop();
    Player *p = hud_player();
    if (p && p->local && p->sim.move.mode == MOVE_DEAD && spectator_mode() == SPEC_OFF) {
        GuiText t = { gui_s(TH->t_huge * 1.2f), GW_SEMIBOLD, TH->danger, GUI_CENTER, 0.2f, true, true, 0 };
        gd_text(&t, gui_w() * 0.5f, gui_h() * 0.36f, "You died");
    }
    draw_spectator(safe);
    draw_downed(w, p);
    if (p && p->sim.move.mode == MOVE_HANG) {
        char k1[32], k2[32], msg[96];
        binding_name(input_get_binding(ACT_JUMP, 0), k1, sizeof(k1));
        binding_name(input_get_binding(ACT_CROUCH, 0), k2, sizeof(k2));
        snprintf(msg, sizeof(msg), "%s  Pull up      %s  Drop", k1, k2);
        caps(gui_w() * 0.5f, gui_h() * 0.5f + gui_s(100), gui_s(TH->t_small), TH->hud_text, GUI_CENTER, msg);
    }
}
