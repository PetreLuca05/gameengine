// Kill feed (docs/UI_WHEELS_HUD.md "Kill feed"), top right: 5 entries, 6 s, fading. The kill count
// is in the inventory's header (ui/inventory/inv_screen.c).
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

// The item's icon as a silhouette in `color` (the kill feed draws them white), else the fallback glyph.
void hud_item_icon(u16 item, const char *fallback, float cx, float cy, float size, u32 color) {
    TextureId tex;
    vec4 uv;
    if (item && icons_get(item, &tex, &uv)) {
        gd_image_mask(gui_rect(cx - size * 0.5f, cy - size * 0.5f, size, size), tex, uv, color);
        return;
    }
    hud_icon(fallback, cx, cy, size * 0.8f, color);
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
        bool head = (e->flags & KILLF_HEADSHOT) != 0, coll = (e->flags & KILLF_COLLATERAL) != 0;
        float pad = gui_s(10), sp = gui_s(8);
        float w = pad * 2 + kw + (kw > 0 ? sp : 0) + is + (coll ? sp + is : 0) + (head ? sp + is * 0.8f : 0) + sp + vw;
        GuiRect r = { right - w + (1.0f - in) * gui_s(40), y, w, rh };
        gd_alpha_push(a * in);
        gd_round(r, gui_s(TH->radius), TH->hud_panel);
        if (e->mine || e->me_victim) gd_rect(gui_rect(r.x + r.w - gui_s(3), r.y, gui_s(3), r.h), e->mine ? TH->select : TH->danger);
        float x = r.x + pad, ty = r.y + (rh - gd_line_height(kt.size)) * 0.5f, cy = r.y + rh * 0.5f;
        if (kw > 0) { gd_text(&kt, x, ty, e->killer); x += kw + sp; }
        hud_item_icon(e->weapon, (e->flags & KILLF_KILLER_PLAYER) ? "skull" : "zombie", x + is * 0.5f, cy, is, TH->select);   // white
        x += is;
        if (coll) { x += sp; hud_icon("collateral", x + is * 0.5f, cy, is, TH->select); x += is; }
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
    if (hud_cvars()->killfeed->i) y = draw_feed(g, safe, y);
    return y;
}
