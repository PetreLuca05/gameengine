// Ping markers in the world (docs/UI_WHEELS_HUD.md "Pings"): an icon in the type's colour over the
// spot with the label, distance and who pinged it; off screen (or behind) it sits on the screen
// edge with an arrow towards it. Your own pings get a white ring.
#include "hud_internal.h"
#include "hud_icons.h"
#include "render/render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void hud_pings_draw(Game *g, GuiRect safe) {
    if (!hud_cvars()->ping_markers->i) return;
    World *w = g->world;
    Player *lp = game_local_player();
    if (!w || !lp) return;
    const PingTypes *pt = game_ping_types();
    float k = hud_cvars()->scale->f;
    float is = gui_s(gui_tok("ping_icon")) * k, margin = gui_s(gui_tok("ping_edge_margin"));
    float cx = gui_w() * 0.5f, cy = gui_h() * 0.5f;
    vec3 fwd = dir_from_angles(g->view.yaw, g->view.pitch);
    vec3 right = right_from_yaw(g->view.yaw);
    gd_alpha_push(hud_cvars()->opacity->f);
    for (int i = 0; i < PING_MAX; i++) {
        const Ping *p = &g->pings.p[i];
        if (!p->active) continue;
        const PingTypeDef *t = &pt->t[p->type];
        u32 col = TH->ping[t->color];
        float a = ping_alpha(p, g->hud_time);
        float dist = v3_dist(p->pos, g->view.eye);
        vec2 sp;
        vec3 to = v3_sub(p->pos, g->view.eye);
        bool front = v3_dot(to, fwd) > 0.05f;
        bool on = front && render_world_to_screen(p->pos, &sp);
        bool edge = !on || sp.x < safe.x + margin || sp.x > safe.x + safe.w - margin || sp.y < safe.y + margin || sp.y > safe.y + safe.h - margin;
        float ang = 0;
        if (edge) {
            // Direction on screen: right / up components of the offset in view space.
            float rx = v3_dot(to, right), uy = to.y - fwd.y * v3_dot(to, fwd);
            if (!front && fabsf(rx) < 0.01f) rx = 0.01f;
            ang = atan2f(rx, uy);
            float ex = (safe.w * 0.5f - margin), ey = (safe.h * 0.5f - margin);
            float sx = sinf(ang), sy = -cosf(ang);
            float s = fminf(ex / fmaxf(fabsf(sx), 1e-4f), ey / fmaxf(fabsf(sy), 1e-4f));
            sp = v2(cx + sx * s, cy + sy * s);
        }
        // Pulse in, then settle; a soft bob keeps it alive.
        float age = g->hud_time - p->born;
        float pop = 1.0f + 0.35f * expf(-age * 7.0f);
        float sz = is * pop;
        gd_alpha_push(a);
        gd_circle(sp.x, sp.y, sz * 0.62f, UI_RGBA(0, 0, 0, 130));
        gd_ring(sp.x, sp.y, sz * 0.62f, fmaxf(1.5f, gui_s(2.0f)), 1.0f, p->owner == g->local_slot ? TH->select : col);
        hud_icon(t->icon, sp.x, sp.y, sz * 0.7f, col);
        if (edge) {
            float sx = sinf(ang), sy = -cosf(ang), px = -sy, py = sx;
            float tx = sp.x + sx * sz * 0.95f, ty = sp.y + sy * sz * 0.95f;
            float bx = sp.x + sx * sz * 0.7f, by = sp.y + sy * sz * 0.7f;
            gd_line(bx + px * sz * 0.22f, by + py * sz * 0.22f, tx, ty, fmaxf(1.5f, gui_s(2.0f)), col);
            gd_line(bx - px * sz * 0.22f, by - py * sz * 0.22f, tx, ty, fmaxf(1.5f, gui_s(2.0f)), col);
        } else {
            // Label + distance under it, the owner under that (others' pings).
            char b[64];
            snprintf(b, sizeof(b), "%s  %.0f m", t->label, dist);
            hud_caps(sp.x, sp.y + sz * 0.7f + gui_s(2), gui_s(TH->t_tiny + 1), col, GUI_CENTER, b);
            if (p->owner != g->local_slot && p->owner < MAX_PLAYERS && w->players[p->owner].name[0]) {
                GuiText nt = { gui_s(TH->t_tiny), GW_MEDIUM, TH->text_dim, GUI_CENTER, 0, false, true, 0 };
                gd_text(&nt, sp.x, sp.y + sz * 0.7f + gui_s(18), w->players[p->owner].name);
            }
        }
        gd_alpha_pop();
    }
    gd_alpha_pop();
}
