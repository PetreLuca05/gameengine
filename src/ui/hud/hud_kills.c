// Kill counter + kill feed (docs/UI_WHEELS_HUD.md "Kill counter"), top right: the total with a
// skull, the zombies / players split (on hover, after a kill, or always), a pulse on every kill and
// the streak, a headshot tick; the feed (5 entries, 6 s, fading) underneath.
#include "hud_internal.h"
#include "hud_icons.h"
#include "ui/icons.h"
#include "core/cvar.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void hud_caps(float x, float y, float size, u32 c, int align, const char *s) {
    GuiText t = { size, GW_SEMIBOLD, c, align, TH->tracking_caps * 1.3f, true, true, 0 };
    gd_text(&t, x, y, s);
}

void hud_item_icon(u16 item, const char *fallback, float cx, float cy, float size, u32 color) {
    TextureId tex;
    vec4 uv;
    if (item && icons_get(item, &tex, &uv)) {
        gd_image(gui_rect(cx - size * 0.5f, cy - size * 0.5f, size, size), tex, uv, gui_rgba_mul(UI_RGBA(255, 255, 255, 255), (float)(color >> 24) / 255.0f));
        return;
    }
    hud_icon(fallback, cx, cy, size * 0.8f, color);
}

static float draw_counter(Game *g, GuiRect safe) {
    const KillStats *k = &g->kill_stats;
    float w = gui_s(gui_tok("killcount_w")), h = gui_s(gui_tok("killcount_h"));
    GuiRect r = { safe.x + safe.w - w, safe.y, w, h };
    float pulse = k->pulse;
    // Split: hovered (cursor free), shortly after a kill, or always (hud_killcount_split 1).
    vec2 m = gui_mouse();
    bool hover = m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h;
    bool recent = k->last_kill > 0 && g->hud_time - k->last_kill < 2.5f;
    float split = gui_tween(gui_id("hud_kills_split"), hover || recent || hud_cvars()->killcount_split->i ? 1.0f : 0.0f, 0.15f);
    gd_shadow(r, h * 0.5f, gui_s(20), UI_RGBA(0, 0, 0, 60));
    gd_round(r, h * 0.5f, TH->hud_panel);
    if (pulse > 0.01f) gd_round_outline(r, h * 0.5f, fmaxf(1.0f, gui_s(1.5f)), gui_rgba_mul(TH->select, pulse * 0.9f));
    float pad = gui_s(14), icon = h * 0.52f;
    float cy = r.y + h * 0.5f;
    hud_icon("skull", r.x + pad + icon * 0.5f, cy, icon * (1.0f + pulse * 0.18f), gui_rgba_lerp(TH->hud_text, TH->select, pulse));
    char b[24];
    snprintf(b, sizeof(b), "%d", kill_stats_total(k));
    GuiText num = { gui_s(TH->t_large * 0.85f) * (1.0f + pulse * 0.12f), GW_SEMIBOLD, gui_rgba_lerp(TH->hud_text, TH->select, pulse), GUI_LEFT, 0, false, true, 0 };
    float nx = r.x + pad + icon + gui_s(10);
    float nw = gd_text(&num, nx, cy - gd_line_height(num.size) * 0.5f, b);
    // Right side: "Kills", or the split, sliding over.
    float rx = r.x + r.w - pad;
    if (split < 0.99f) {
        gd_alpha_push(1.0f - split);
        hud_caps(rx, cy - gd_line_height(gui_s(TH->t_label)) * 0.5f, gui_s(TH->t_label), TH->text_dim, GUI_RIGHT, "Kills");
        gd_alpha_pop();
    }
    if (split > 0.01f) {
        gd_alpha_push(split);
        char zb[16], pb[16];
        snprintf(zb, sizeof(zb), "%d", k->zombies);
        snprintf(pb, sizeof(pb), "%d", k->players);
        GuiText st = { gui_s(TH->t_small), GW_SEMIBOLD, TH->hud_text, GUI_RIGHT, 0, false, true, 0 };
        float ih = gui_s(14), y0 = cy - gui_s(15), y1 = cy + gui_s(1);
        float tw = gd_text(&st, rx, y0, zb);
        hud_icon("zombie", rx - tw - gui_s(4) - ih * 0.5f, y0 + gd_line_height(st.size) * 0.5f, ih, TH->text_dim);
        tw = gd_text(&st, rx, y1, pb);
        hud_icon("player", rx - tw - gui_s(4) - ih * 0.5f, y1 + gd_line_height(st.size) * 0.5f, ih, TH->text_dim);
        gd_alpha_pop();
    }
    // Headshot tick next to the number.
    if (k->head_tick > 0.01f) hud_icon("headshot", nx + nw + gui_s(12), cy, gui_s(16), gui_rgba_mul(TH->select, k->head_tick));
    float bottom = r.y + h;
    // Streak pill under the counter.
    float sa = gui_tween(gui_id("hud_kills_streak"), k->streak >= 2 ? 1.0f : 0.0f, 0.12f);
    if (sa > 0.01f) {
        char s[32];
        snprintf(s, sizeof(s), "Streak x%d", DG_MAX(k->streak, 2));
        GuiText t = { gui_s(TH->t_label), GW_SEMIBOLD, TH->text_dark, GUI_CENTER, TH->tracking_caps * 1.3f, true, false, 0 };
        float tw = gd_text_width(&t, s) + gui_s(18), th = gui_s(20);
        GuiRect pr = { r.x + r.w - tw, bottom + gui_s(6), tw, th };
        gd_alpha_push(sa);
        gd_round(pr, th * 0.5f, gui_rgba_lerp(TH->accent, TH->select, pulse));
        gd_text(&t, pr.x + tw * 0.5f, pr.y + (th - gd_line_height(t.size)) * 0.5f, s);
        gd_alpha_pop();
        bottom = pr.y + th;
    }
    return bottom;
}

static float draw_feed(Game *g, GuiRect safe, float y) {
    const KillFeed *f = &g->kill_feed;
    float rh = gui_s(gui_tok("killfeed_row_h")), gap = gui_s(gui_tok("killfeed_gap")), is = gui_s(gui_tok("killfeed_icon"));
    float right = safe.x + safe.w;
    for (int i = 0; i < KILLFEED_MAX; i++) {
        const KillFeedEntry *e = &f->e[i];
        float a = killfeed_alpha(e);
        if (a <= 0.0f) continue;
        // Slide in from the right over the first 0.2 s.
        float in = saturate((KILLFEED_TIME - e->time) / 0.2f);
        GuiText kt = { gui_s(TH->t_small + 1), GW_SEMIBOLD, e->mine ? TH->select : TH->hud_text, GUI_LEFT, 0, false, true, gui_s(150) };
        GuiText vt = { gui_s(TH->t_small + 1), GW_MEDIUM, e->me_victim ? TH->danger : TH->hud_text, GUI_LEFT, 0, false, true, gui_s(150) };
        float kw = e->killer[0] ? fminf(gd_text_width(&kt, e->killer), gui_s(150)) : 0;
        float vw = fminf(gd_text_width(&vt, e->victim), gui_s(150));
        bool head = (e->flags & KILLF_HEADSHOT) != 0;
        float pad = gui_s(10), sp = gui_s(8);
        float w = pad * 2 + kw + (kw > 0 ? sp : 0) + is + (head ? sp + is * 0.8f : 0) + sp + vw;
        GuiRect r = { right - w + (1.0f - in) * gui_s(40), y, w, rh };
        gd_alpha_push(a * in);
        gd_round(r, gui_s(TH->radius), TH->hud_panel);
        if (e->mine || e->me_victim) gd_rect(gui_rect(r.x + r.w - gui_s(3), r.y, gui_s(3), r.h), e->mine ? TH->select : TH->danger);
        float x = r.x + pad, ty = r.y + (rh - gd_line_height(kt.size)) * 0.5f, cy = r.y + rh * 0.5f;
        if (kw > 0) { gd_text(&kt, x, ty, e->killer); x += kw + sp; }
        hud_item_icon(e->weapon, (e->flags & KILLF_KILLER_PLAYER) ? "skull" : "zombie", x + is * 0.5f, cy, is, TH->text_dim);
        x += is;
        if (head) { x += sp; hud_icon("headshot", x + is * 0.4f, cy, is * 0.8f, TH->select); x += is * 0.8f; }
        x += sp;
        gd_text(&vt, x, ty, e->victim);
        gd_alpha_pop();
        y += (rh + gap) * fminf(1.0f, a * 2.0f);   // rows close up as they fade
    }
    return y;
}

float hud_kills_draw(Game *g, GuiRect safe) {
    float y = safe.y;
    if (hud_cvars()->killcount->i) y = draw_counter(g, safe) + gui_s(10);
    if (hud_cvars()->killfeed->i) y = draw_feed(g, safe, y);
    return y;
}
