#include "hud.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "game/game.h"
#include "gameplay/weapons.h"
#include "gameplay/effects.h"
#include "render/render.h"
#include "net/net.h"
#include "input/input.h"
#include <stdio.h>
#include <math.h>

#define COL_WHITE UI_RGBA(235, 235, 230, 255)
#define COL_DIM UI_RGBA(180, 180, 175, 200)
#define COL_PANEL UI_RGBA(10, 12, 14, 150)
#define COL_ACCENT UI_RGBA(220, 130, 45, 255)

static void bar(float x, float y, float w, float h, float frac, u32 fill, u32 back) {
    ui_rect(x, y, w, h, back);
    ui_rect(x + 2, y + 2, (w - 4) * saturate(frac), h - 4, fill);
}

static void draw_crosshair(Game *g, World *w, float s) {
    float cx = ui_width() * 0.5f, cy = ui_height() * 0.5f;
    Player *p = game_local_player();
    if (!p || p->sim.move.mode == MOVE_DEAD) return;
    const WeaponDef *wd = weapon_equipped_def(w, g->local_slot);
    bool ads = (g->last_cmd.buttons & BTN_AIM) != 0;
    if (wd && wd->kind != WPN_MELEE && !ads) {
        // Gap follows the actual spread cone projected to the screen.
        float spread = weapon_current_spread(w, g->local_slot);
        float fov = g->view.fov > 1 ? g->view.fov : 75.0f;
        float gap = tanf(spread * DG_DEG2RAD) / tanf(fov * 0.5f * DG_DEG2RAD) * (ui_height() * 0.5f);
        gap = fmaxf(gap, 4 * s);
        float len = 8 * s, th = 2 * s;
        u32 c = UI_RGBA(240, 240, 240, 220);
        ui_rect(cx - gap - len, cy - th * 0.5f, len, th, c);
        ui_rect(cx + gap, cy - th * 0.5f, len, th, c);
        ui_rect(cx - th * 0.5f, cy - gap - len, th, len, c);
        ui_rect(cx - th * 0.5f, cy + gap, th, len, c);
    } else if (!ads) {
        ui_rect(cx - 2 * s, cy - 2 * s, 4 * s, 4 * s, g->focus ? COL_ACCENT : UI_RGBA(240, 240, 240, 200));
    }
    if (g->hitmarker_time > 0) {
        float a = g->hitmarker_time / 0.25f;
        u32 c = g->hitmarker_kill ? UI_RGBA(255, 60, 40, (int)(255 * a)) : (g->hitmarker_head ? UI_RGBA(255, 200, 60, (int)(255 * a)) : UI_RGBA(255, 255, 255, (int)(255 * a)));
        float in = 6 * s, out = 14 * s;
        ui_line(cx - in, cy - in, cx - out, cy - out, 2.5f * s, c);
        ui_line(cx + in, cy - in, cx + out, cy - out, 2.5f * s, c);
        ui_line(cx - in, cy + in, cx - out, cy + out, 2.5f * s, c);
        ui_line(cx + in, cy + in, cx + out, cy + out, 2.5f * s, c);
    }
}

static void draw_damage_indicators(Game *g, float s) {
    float cx = ui_width() * 0.5f, cy = ui_height() * 0.5f;
    Player *p = game_local_player();
    if (!p) return;
    for (int i = 0; i < MAX_HURT_MARKERS; i++) {
        HurtMarker *h = &g->hurt[i];
        if (h->time <= 0) continue;
        vec3 to = v3_sub(h->from, p->sim.move.pos);
        float ang = atan2f(-to.x, -to.z) - g->view.yaw;  // 0 = in front
        float a = saturate(h->time / 1.5f);
        float r = 110 * s;
        float x = cx - sinf(ang) * r, y = cy - cosf(ang) * r;
        float tx = -sinf(ang), ty = -cosf(ang);
        float px = -ty, py = tx;
        u32 c = UI_RGBA(230, 40, 30, (int)(220 * a));
        ui_line(x + px * 18 * s, y + py * 18 * s, x + tx * 14 * s, y + ty * 14 * s, 4 * s, c);
        ui_line(x - px * 18 * s, y - py * 18 * s, x + tx * 14 * s, y + ty * 14 * s, 4 * s, c);
    }
    if (g->damage_flash > 0) {
        float a = g->damage_flash * 0.35f;
        float bw = 60 * s;
        u32 c = UI_RGBA(200, 0, 0, (int)(255 * a));
        ui_rect(0, 0, ui_width(), bw, c);
        ui_rect(0, ui_height() - bw, ui_width(), bw, c);
        ui_rect(0, bw, bw, ui_height() - 2 * bw, c);
        ui_rect(ui_width() - bw, bw, bw, ui_height() - 2 * bw, c);
    }
}

static void draw_vitals(Game *g, World *w, float s) {
    Player *p = game_local_player();
    if (!p) return;
    Health *h = &w->health[ENT_INDEX(p->ent)];
    float x = 24 * s, y = ui_height() - 70 * s, bw = 260 * s;
    ui_rect(x - 8 * s, y - 30 * s, bw + 16 * s, 82 * s, COL_PANEL);
    ui_textf(x, y - 26 * s, FONT_SMALL, COL_DIM, UI_ALIGN_LEFT, "HEALTH");
    bar(x, y - 10 * s, bw, 20 * s, h->hp / fmaxf(1, h->max_hp), h->hp > 30 ? UI_RGBA(90, 200, 90, 255) : UI_RGBA(220, 60, 50, 255), UI_RGBA(30, 30, 30, 200));
    ui_textf(x + bw - 4 * s, y - 9 * s, FONT_UI, COL_WHITE, UI_ALIGN_RIGHT, "%.0f", h->hp);
    ui_textf(x, y + 14 * s, FONT_SMALL, COL_DIM, UI_ALIGN_LEFT, "ARMOR");
    bar(x, y + 28 * s, bw, 14 * s, h->armor / fmaxf(1, h->max_armor), UI_RGBA(80, 140, 220, 255), UI_RGBA(30, 30, 30, 200));
    if (h->armor > 0) ui_textf(x + bw - 4 * s, y + 26 * s, FONT_SMALL, COL_WHITE, UI_ALIGN_RIGHT, "%.0f", h->armor);
}

static void draw_ammo(Game *g, World *w, float s) {
    Player *p = game_local_player();
    if (!p) return;
    const InvSlot *slot = weapon_equipped(w, g->local_slot);
    if (!slot) return;
    const ItemDef *it = item_def(w->items, slot->item);
    const WeaponDef *wd = item_weapon(w->items, slot->item);
    float x = ui_width() - 24 * s, y = ui_height() - 96 * s;
    ui_rect(x - 230 * s, y - 8 * s, 238 * s, 84 * s, COL_PANEL);
    ui_textf(x, y, FONT_UI, COL_DIM, UI_ALIGN_RIGHT, "%s", it->name);
    if (wd && wd->kind != WPN_MELEE) {
        int reserve = inv_count(&p->inv, wd->ammo_item);
        ui_textf(x - 70 * s, y + 18 * s, FONT_HUGE, slot->ammo > 0 ? COL_WHITE : UI_RGBA(230, 70, 50, 255), UI_ALIGN_RIGHT, "%d", slot->ammo);
        ui_textf(x, y + 34 * s, FONT_LARGE, COL_DIM, UI_ALIGN_RIGHT, "/ %d", reserve);
        if (p->sim.wpn.reloading) {
            float frac = 1.0f - p->sim.wpn.reload_t / fmaxf(0.01f, wd->reload_time);
            bar(ui_width() * 0.5f - 80 * s, ui_height() * 0.5f + 40 * s, 160 * s, 8 * s, frac, COL_ACCENT, UI_RGBA(20, 20, 20, 180));
            ui_text_shadow(ui_width() * 0.5f, ui_height() * 0.5f + 52 * s, FONT_SMALL, COL_WHITE, UI_ALIGN_CENTER, "RELOADING");
        } else if (slot->ammo == 0) {
            char key[32];
            binding_name(input_get_binding(ACT_RELOAD, 0), key, sizeof(key));
            ui_textf(ui_width() * 0.5f, ui_height() * 0.5f + 40 * s, FONT_UI, UI_RGBA(230, 90, 60, 255), UI_ALIGN_CENTER, reserve > 0 ? "[%s] Reload" : "No ammo", key);
        }
    } else if (it->kind == ITEM_CONSUMABLE) {
        ui_textf(x, y + 26 * s, FONT_LARGE, COL_WHITE, UI_ALIGN_RIGHT, "x%d", slot->count);
        if (p->sim.wpn.use_t >= 0) {
            float frac = p->sim.wpn.use_t / fmaxf(0.01f, it->use_time);
            bar(ui_width() * 0.5f - 80 * s, ui_height() * 0.5f + 40 * s, 160 * s, 8 * s, frac, UI_RGBA(90, 200, 90, 255), UI_RGBA(20, 20, 20, 180));
        } else {
            ui_text_shadow(x, y + 58 * s, FONT_SMALL, COL_DIM, UI_ALIGN_RIGHT, "hold attack to use");
        }
    } else if (wd) {
        ui_textf(x, y + 30 * s, FONT_UI, COL_DIM, UI_ALIGN_RIGHT, "melee  %.0f dmg", wd->damage);
    }
}

static void draw_hotbar(Game *g, World *w, float s) {
    Player *p = game_local_player();
    if (!p) return;
    float slot_w = 64 * s, gap = 6 * s;
    float total = INV_HOTBAR * slot_w + (INV_HOTBAR - 1) * gap;
    float x0 = (ui_width() - total) * 0.5f, y = ui_height() - slot_w - 18 * s;
    for (int i = 0; i < INV_HOTBAR; i++) {
        float x = x0 + i * (slot_w + gap);
        bool sel = i == g->hotbar_slot;
        ui_rect(x, y, slot_w, slot_w, sel ? UI_RGBA(60, 45, 25, 210) : COL_PANEL);
        if (sel) ui_rect_outline(x, y, slot_w, slot_w, 2 * s, COL_ACCENT);
        ui_textf(x + 4 * s, y + 2 * s, FONT_SMALL, COL_DIM, UI_ALIGN_LEFT, "%d", i + 1);
        const InvSlot *is = &p->inv.slots[i];
        if (!is->item) continue;
        const ItemDef *d = item_def(w->items, is->item);
        u32 c = rgba8v(d->color);
        ui_rect(x + slot_w * 0.3f, y + slot_w * 0.28f, slot_w * 0.4f, slot_w * 0.3f, c | 0xFF000000u);
        char name[16];
        snprintf(name, sizeof(name), "%.9s", d->name);
        ui_text_shadow(x + slot_w * 0.5f, y + slot_w - 18 * s, FONT_SMALL, COL_WHITE, UI_ALIGN_CENTER, name);
        if (is->count > 1) ui_textf(x + slot_w - 4 * s, y + 2 * s, FONT_SMALL, COL_WHITE, UI_ALIGN_RIGHT, "%d", is->count);
        const WeaponDef *wd = item_weapon(w->items, is->item);
        if (wd && wd->mag_size > 0) ui_textf(x + slot_w - 4 * s, y + 2 * s, FONT_SMALL, UI_RGBA(230, 200, 120, 255), UI_ALIGN_RIGHT, "%d", is->ammo);
    }
}

static void draw_prompt(Game *g, float s) {
    if (!g->focus_prompt[0]) return;
    float cx = ui_width() * 0.5f, y = ui_height() * 0.5f + 70 * s;
    float tw = font_text_width(FONT_UI, g->focus_prompt);
    ui_rect(cx - tw * 0.5f - 12 * s, y - 6 * s, tw + 24 * s, 30 * s, COL_PANEL);
    ui_text_shadow(cx, y, FONT_UI, COL_WHITE, UI_ALIGN_CENTER, g->focus_prompt);
    if (g->hold_progress > 0.001f) bar(cx - 90 * s, y + 28 * s, 180 * s, 8 * s, g->hold_progress, COL_ACCENT, UI_RGBA(20, 20, 20, 180));
}

static void draw_hints(Game *g, float s) {
    float y = ui_height() * 0.62f;
    for (int i = 0; i < MAX_HINTS; i++) {
        Hint *h = &g->hints[i];
        if (h->time <= 0) continue;
        int a = (int)(255 * saturate(h->time));
        ui_text_shadow(ui_width() * 0.5f, y, FONT_UI, UI_RGBA(255, 230, 180, a), UI_ALIGN_CENTER, h->text);
        y += 22 * s;
    }
    float ky = 20 * s;
    for (int i = 0; i < MAX_KILLFEED; i++) {
        Hint *h = &g->killfeed[i];
        if (h->time <= 0) continue;
        int a = (int)(255 * saturate(h->time));
        ui_text_shadow(ui_width() - 20 * s, ky, FONT_UI, UI_RGBA(255, 120, 100, a), UI_ALIGN_RIGHT, h->text);
        ky += 22 * s;
    }
}

static void draw_world_bars(Game *g, World *w, float s) {
    vec3 cam = g->view.eye;
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_HEALTH)) continue;
        if (w->type[i] == ET_PLAYER && w->player_slot[i] == g->local_slot) continue;
        vec3 top = w->type[i] == ET_DUMMY ? v3_add(w->xf[i].pos, v3(0, 1.15f, 0)) : v3_add(w->xf[i].pos, v3(0, 2.05f, 0));
        float d = v3_dist(top, cam);
        if (d > 40.0f) continue;
        vec2 sp;
        if (!render_world_to_screen(top, &sp)) continue;
        Health *h = &w->health[i];
        float bw = 70 * s * clampf(8.0f / d, 0.5f, 1.4f);
        bar(sp.x - bw * 0.5f, sp.y, bw, 9 * s, h->hp / fmaxf(1, h->max_hp), (w->flags[i] & EF_DEAD) ? UI_RGBA(90, 90, 90, 255) : UI_RGBA(220, 70, 50, 255), UI_RGBA(15, 15, 15, 170));
        if (w->type[i] == ET_PLAYER) ui_text_shadow(sp.x, sp.y - 18 * s, FONT_SMALL, COL_WHITE, UI_ALIGN_CENTER, w->names[i]);
        else ui_textf(sp.x, sp.y - 16 * s, FONT_SMALL, COL_DIM, UI_ALIGN_CENTER, "%.0f / %.0f", h->hp, h->max_hp);
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
    if (g->dev_mode->i) {
        Player *p = game_local_player();
        if (p) {
            const PlayerMove *m = &p->sim.move;
            static const char *modes[] = { "walk", "mantle", "vault", "hang", "noclip", "dead" };
            ui_textf(10 * s, y, FONT_SMALL, UI_RGBA(220, 220, 255, 220), UI_ALIGN_LEFT,
                     "pos %.2f %.2f %.2f  speed %.2f m/s  %s%s%s  %s  map %s  %s",
                     m->pos.x, m->pos.y, m->pos.z, v3_len(v3_flat(m->vel)), modes[DG_MIN(m->mode, 5)],
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
        ui_textf(14 * s, y, FONT_SMALL, COL_WHITE, UI_ALIGN_LEFT, "net: %s via %s, %d peer(s)", net_role() == NET_HOST ? "host" : "client", st.transport, st.peers); y += 16 * s;
        ui_textf(14 * s, y, FONT_SMALL, COL_WHITE, UI_ALIGN_LEFT, "ping %.0f ms   loss %.1f%%", st.ping_ms, st.loss_pct); y += 16 * s;
        ui_textf(14 * s, y, FONT_SMALL, COL_WHITE, UI_ALIGN_LEFT, "in %.1f kB/s   out %.1f kB/s", st.in_kbps, st.out_kbps); y += 16 * s;
        ui_textf(14 * s, y, FONT_SMALL, COL_WHITE, UI_ALIGN_LEFT, "snapshot %.0f B @ %d Hz  interp %.0f ms", st.snapshot_bytes, st.snapshot_rate, st.interp_ms); y += 16 * s;
        ui_textf(14 * s, y, FONT_SMALL, COL_WHITE, UI_ALIGN_LEFT, "prediction error %.3f m", st.prediction_error); y += 16 * s;
    }
}

void hud_draw(void) {
    Game *g = game();
    if (!g || !g->world) return;
    World *w = g->world;
    float s = ui_scale();
    draw_world_bars(g, w, s);
    fx_draw_hud();
    if (!g->menu_open) {
        draw_crosshair(g, w, s);
        draw_prompt(g, s);
    }
    draw_damage_indicators(g, s);
    draw_vitals(g, w, s);
    draw_ammo(g, w, s);
    draw_hotbar(g, w, s);
    draw_hints(g, s);
    Player *p = game_local_player();
    if (p && p->sim.move.mode == MOVE_DEAD)
        ui_text_shadow(ui_width() * 0.5f, ui_height() * 0.4f, FONT_HUGE, UI_RGBA(230, 60, 50, 255), UI_ALIGN_CENTER, "YOU DIED");
    if (p && p->sim.move.mode == MOVE_HANG) {
        char k1[32], k2[32];
        binding_name(input_get_binding(ACT_JUMP, 0), k1, sizeof(k1));
        binding_name(input_get_binding(ACT_CROUCH, 0), k2, sizeof(k2));
        ui_textf(ui_width() * 0.5f, ui_height() * 0.5f + 100 * s, FONT_UI, COL_WHITE, UI_ALIGN_CENTER, "[%s] Pull up   [%s] Drop", k1, k2);
    }
    draw_dev(g, w, s);
}
