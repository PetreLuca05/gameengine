#include "recoil_debug.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "game/game.h"
#include "gameplay/weapons.h"
#include "render/debug_draw.h"
#include "physics/physics.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

void recoil_debug_init(void) {}

#define PANEL_BG UI_RGBA(8, 10, 12, 190)
#define TXT UI_RGBA(225, 228, 220, 255)
#define DIM UI_RGBA(150, 155, 150, 255)

static float line(float x, float y, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_text(x, y, FONT_SMALL, TXT, UI_ALIGN_LEFT, buf);
    return y + 16.0f * fmaxf(1.0f, ui_height() / 1080.0f);
}

void recoil_debug_draw(vec3 cam_pos) {
    const WeaponCvars *wc = weapons_cvars();
    if (!wc->recoil_debug || !wc->recoil_debug->i) return;
    Game *g = game();
    World *w = game_world();
    Player *p = game_local_player();
    if (!g || !w || !p) return;
    const WeaponDef *d = weapon_equipped_def(w, g->local_slot);
    if (!d || d->kind == WPN_MELEE) return;
    const RecoilParams *rp = &d->recoil;
    const WeaponState *ws = &p->sim.wpn;
    const RecoilState *rc = &ws->rc;
    float cone = lerpf(g->spread_prev, g->spread_cur, g->alpha);

    // World: the cone at the distance the aim ray hits.
    vec2 sway = weapon_sway_deg(d, ws, weapon_sway_time(g->last_cmd.seq) + g->alpha * SIM_DT);
    vec3 aim = dir_from_angles(g->view.yaw - sway.x * DG_DEG2RAD, g->view.pitch + sway.y * DG_DEG2RAD);
    RayHit rh;
    BodyId self = p->character ? phys_character_inner_body(p->character) : PHYS_INVALID_BODY;
    float dist = 30.0f;
    if (phys_raycast(w->phys, g->view.eye, v3_scale(aim, 200.0f), PL_MASK_WORLD, self, &rh)) dist = rh.distance;
    vec3 c = v3_madd(g->view.eye, aim, dist * 0.995f);
    float r = tanf(cone * 0.5f * DG_DEG2RAD) * dist;
    dbg_circle(c, v3_neg(aim), r, DBG_CYAN, 0);
    dbg_cross(c, 0.04f, DBG_YELLOW, 0);
    (void)cam_pos;

    // Panel.
    float s = fmaxf(1.0f, ui_height() / 1080.0f);
    float x = 16 * s, y = ui_height() * 0.30f, pw = 300 * s;
    ui_rect(x - 8 * s, y - 8 * s, pw, 330 * s, PANEL_BG);
    y = line(x, y, "RECOIL  %s  [%s]  %.0f rpm", d->id, fire_mode_name((FireMode)ws->fire_mode), rp->rpm);
    y = line(x, y, "mag %d  seed %08x  shot %u", ws->mag, rc->mag_seed, rc->shot_index);
    y = line(x, y, "acc     pitch %+6.2f  yaw %+6.2f deg", rc->acc_pitch, rc->acc_yaw);
    y = line(x, y, "counter pitch %6.2f  yaw %6.2f deg", rc->countered_pitch, rc->countered_yaw);
    y = line(x, y, "recover %s pool %+5.2f / %+5.2f", rc->recovering ? "ON " : "off", rc->pool_pitch, rc->pool_yaw);
    y = line(x, y, "cone %.3f deg (bloom %.2f)  @%.1f m = %.1f cm", cone, rc->bloom, dist, r * 200.0f);
    y = line(x, y, "ads %.2f  sway x%.2f  breath %.1f/%.1f%s", ws->ads_t, ws->sway_k, ws->breath, rp->hold_breath_time,
             ws->breath_holding ? " HOLD" : (ws->breath_exhausted ? " OUT" : ""));
    const WeaponDebugStats *st = weapons_debug_stats();
    y = line(x, y, "bullets checked %u  outside %u  max %.0f%%", st->shots, st->outside, st->max_ratio * 100.0f);
    y = line(x, y, "scale recoil %.2f spread %.2f visual %.2f", wc->recoil_scale->f, wc->spread_scale->f, wc->visual_scale->f);

    // Pattern preview: the first 30 shots of this magazine (no recovery, no countering).
    float bx = x, by = y + 6 * s, bw = 280 * s, bh = 150 * s;
    ui_rect_outline(bx, by, bw, bh, 1, DIM);
    vec2 pts[30];
    int n = recoil_preview(rp, rc->mag_seed, pts, 30);
    float maxv = 1.0f, maxh = 0.5f;
    for (int i = 0; i < n; i++) { maxv = fmaxf(maxv, pts[i].y); maxh = fmaxf(maxh, fabsf(pts[i].x)); }
    float k = fminf((bh - 12 * s) / maxv, (bw * 0.5f - 8 * s) / maxh);
    float ox = bx + bw * 0.5f, oy = by + bh - 6 * s;
    ui_line(ox, by + 2, ox, by + bh - 2, 1, UI_RGBA(80, 80, 80, 200));
    float px = ox, py = oy;
    for (int i = 0; i < n; i++) {
        float qx = ox + pts[i].x * k, qy = oy - pts[i].y * k;
        ui_line(px, py, qx, qy, 1.5f * s, UI_RGBA(90, 200, 255, 200));
        u32 dc = i < rc->shot_index ? UI_RGBA(255, 80, 60, 255) : UI_RGBA(230, 230, 230, 220);
        ui_rect(qx - 2 * s, qy - 2 * s, 4 * s, 4 * s, dc);
        px = qx;
        py = qy;
    }
    // Live accumulated (uncountered) offset.
    float lx = ox + (rc->acc_yaw - signf(rc->acc_yaw) * rc->countered_yaw) * k, ly = oy - (rc->acc_pitch - rc->countered_pitch) * k;
    ui_rect(lx - 3 * s, ly - 3 * s, 6 * s, 6 * s, UI_RGBA(255, 220, 40, 255));
    ui_text(bx + 4 * s, by + 2 * s, FONT_SMALL, DIM, UI_ALIGN_LEFT, "pattern (30 shots), yellow = current");
}
