// Compass (docs/UI_WHEELS_HUD.md "Compass"), top centre: a heading strip (ticks every 5 deg,
// labels every 15, N in the accent colour) fading at its ends, the heading under the centre mark,
// ping and teammate markers at their bearings (clamped to the ends when outside), and the X / Z /
// height coordinates under it (10 Hz, copied from the pause menu). assets/ui/hud_compass.json.
// Also the HUD settings cvars (Settings > HUD).
#include "hud_internal.h"
#include "hud_icons.h"
#include "compass_math.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    HudCvars cv;
    HudCompassCfg cfg;
    bool loaded;
    char coords[64];
    double coords_t;
} HC;

const HudCvars *hud_cvars(void) { return &HC.cv; }

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static bool jbool(const cJSON *o, const char *k, bool def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

void hud_compass_load(void) {
    HudCompassCfg c = { 150, 5, 15, 0.18f, 10, 800, true, true, true };
    char *json = platform_read_file("assets/ui/hud_compass.json", NULL);
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (json && !root) log_warn("hud_compass.json: JSON syntax error");
    if (root) {
        c.fov_deg = clampf(jnum(root, "fov_deg", c.fov_deg), 30, 360);
        c.tick_every = clampf(jnum(root, "tick_every", c.tick_every), 1, 45);
        c.label_every = clampf(jnum(root, "label_every", c.label_every), c.tick_every, 90);
        c.edge_fade = clampf(jnum(root, "edge_fade", c.edge_fade), 0, 0.5f);
        c.coords = jbool(root, "coords", c.coords);
        c.coords_hz = clampf(jnum(root, "coords_hz", c.coords_hz), 1, 60);
        c.marker_max_dist = jnum(root, "marker_max_dist", c.marker_max_dist);
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "markers");
        c.markers_pings = jbool(m, "pings", c.markers_pings);
        c.markers_team = jbool(m, "teammates", c.markers_team);
        cJSON_Delete(root);
    }
    free(json);
    HC.cfg = c;
    HC.loaded = true;
}

const HudCompassCfg *hud_compass_cfg(void) {
    if (!HC.loaded) hud_compass_load();
    return &HC.cfg;
}

const char *hud_coords_text(void) { return HC.coords; }

bool hud_copy_coords(void) {
    Player *p = game_local_player();
    if (!p) return false;
    char b[96];
    const PlayerMove *m = &p->sim.move;
    // The setpos form, so it pastes straight back into the console.
    snprintf(b, sizeof(b), "setpos %.2f %.2f %.2f", m->pos.x, m->pos.y, m->pos.z);
    return SDL_SetClipboardText(b);
}

static void cmd_hud_reload(int argc, const char **argv) {
    (void)argc; (void)argv;
    hud_compass_load();
    log_info("hud: reloaded assets/ui/hud_compass.json");
}
static void cmd_copypos(int argc, const char **argv) {
    (void)argc; (void)argv;
    log_info(hud_copy_coords() ? "position copied to the clipboard" : "no position to copy");
}

static void cmd_kill_stats(int argc, const char **argv) {
    (void)argc; (void)argv;
    Game *g = game();
    if (!g) return;
    const KillStats *k = &g->kill_stats;
    log_info("kill_stats: total %d zombies %d players %d headshots %d streak %d best %d", kill_stats_total(k), k->zombies, k->players, k->headshots, k->streak, k->best_streak);
    for (int i = 0; i < KILLFEED_MAX; i++) {
        const KillFeedEntry *e = &g->kill_feed.e[i];
        if (e->time > 0) log_info("kill_feed %d: %s -> %s weapon %d flags %u%s", i, e->killer, e->victim, e->weapon, e->flags, e->mine ? " (mine)" : "");
    }
}

void hud_register(void) {
    cmd_register("kill_stats", cmd_kill_stats, "print the kill counter and the kill feed");
    HC.cv.compass = cvar_reg_bool("hud_compass", true, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: compass strip (top centre)");
    HC.cv.compass_arc = cvar_reg_float("hud_compass_arc", 150.0f, 90.0f, 180.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: degrees of heading the compass strip shows (90 .. 180)");
    HC.cv.coords = cvar_reg_bool("hud_coords", true, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: X / Z / height coordinates under the compass");
    HC.cv.killfeed = cvar_reg_bool("hud_killfeed", true, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: kill feed (top right)");
    HC.cv.ping_markers = cvar_reg_bool("hud_ping_markers", true, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: ping markers in the world");
    HC.cv.ping_compass = cvar_reg_bool("hud_ping_compass", true, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: ping and teammate markers on the compass");
    HC.cv.opacity = cvar_reg_float("hud_opacity", 1.0f, 0.2f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: opacity of the compass, kill feed and markers");
    HC.cv.scale = cvar_reg_float("hud_scale", 1.0f, 0.75f, 1.5f, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: size of the compass and ping markers");
    cmd_register("hud_reload", cmd_hud_reload, "reload assets/ui/hud_compass.json");
    cmd_register("copypos", cmd_copypos, "copy your position to the clipboard (as a setpos command)");
}

// A marker on the strip: clamped to the ends (with a chevron) when outside the strip's view.
static void strip_marker(float cx, float half_w, float y, float off_deg, float half_fov, const char *icon, u32 col, float size, const char *sub) {
    bool outside = fabsf(off_deg) > half_fov;
    float x = cx + clampf(off_deg / half_fov, -1.0f, 1.0f) * half_w;
    float a = outside ? 0.7f : 1.0f;
    gd_alpha_push(a);
    gd_circle(x, y, size * 0.62f, UI_RGBA(0, 0, 0, 120));
    if (!hud_icon(icon, x, y, size * 0.85f, col)) gd_circle(x, y, size * 0.28f, col);
    if (outside) {
        float d = off_deg > 0 ? 1.0f : -1.0f, ax = x + d * size * 0.85f;
        gd_line(ax, y - size * 0.3f, ax + d * size * 0.28f, y, fmaxf(1.0f, gui_s(1.6f)), col);
        gd_line(ax, y + size * 0.3f, ax + d * size * 0.28f, y, fmaxf(1.0f, gui_s(1.6f)), col);
    }
    if (sub && sub[0]) {
        GuiText t = { gui_s(TH->t_tiny), GW_SEMIBOLD, col, GUI_CENTER, 0, false, true, 0 };
        gd_text(&t, x, y + size * 0.62f + gui_s(1), sub);
    }
    gd_alpha_pop();
}

float hud_compass_draw(Game *g, GuiRect safe) {
    const HudCompassCfg *c = hud_compass_cfg();
    const HudCvars *cv = &HC.cv;
    Player *p = game_local_player();
    if (!p) { HC.coords[0] = 0; return safe.y; }
    float k = cv->scale->f;
    float w = fminf(gui_s(gui_tok("compass_w")) * k, safe.w * 0.6f), h = gui_s(gui_tok("compass_h")) * k;
    float cx = gui_w() * 0.5f, x0 = cx - w * 0.5f, y = safe.y;
    float heading = compass_heading_deg(g->view.yaw);
    float half_fov = (cv->compass_arc ? cv->compass_arc->f : c->fov_deg) * 0.5f, half_w = w * 0.5f;
    float bottom = y;
    gd_alpha_push(cv->opacity->f);
    if (cv->compass->i) {
        // Body: fades out at both ends.
        float fw = w * 0.22f;
        u32 bg = TH->hud_panel, z = gui_rgba_mul(bg, 0.0f);
        gd_rect_hgrad(gui_rect(x0, y, fw, h), z, bg);
        gd_rect(gui_rect(x0 + fw, y, w - fw * 2, h), bg);
        gd_rect_hgrad(gui_rect(x0 + w - fw, y, fw, h), bg, z);
        // Ticks and labels.
        float step = c->tick_every;
        int first = (int)ceilf((heading - half_fov) / step), last = (int)floorf((heading + half_fov) / step);
        GuiText lt = { gui_s(TH->t_small) * k, GW_SEMIBOLD, TH->hud_text, GUI_CENTER, 0.04f, false, true, 0 };
        for (int t = first; t <= last; t++) {
            float deg = (float)t * step;
            float off = compass_offset_deg(deg, heading);
            float u = fabsf(off) / half_fov;
            float a = c->edge_fade > 0 ? 1.0f - smoothstepf(1.0f - c->edge_fade * 2.0f, 1.0f, u) : 1.0f;
            if (a <= 0.01f) continue;
            float x = cx + off / half_fov * half_w;
            int ideg = ((int)lrintf(deg) % 360 + 360) % 360;
            bool major = fmodf((float)ideg, c->label_every) < 0.01f;
            u32 col = gui_rgba_mul(major ? TH->hud_text : TH->text_dim, a);
            if (major) {
                char lb[8];
                compass_label(ideg, lb, sizeof(lb));
                bool cardinal = ideg % 45 == 0;
                lt.color = gui_rgba_mul(ideg == 0 ? TH->accent : (cardinal ? TH->hud_text : TH->text_dim), a);
                lt.size = gui_s(cardinal ? TH->t_body : TH->t_tiny + 1) * k;
                lt.weight = cardinal ? GW_SEMIBOLD : GW_MEDIUM;
                gd_text(&lt, x, y + (h - gd_line_height(lt.size)) * 0.5f - gui_s(1), lb);
                gd_line(x, y + h - gui_s(gui_tok("compass_tick_major")) * k * 0.45f, x, y + h, fmaxf(1.0f, gui_s(1.4f)), col);
            } else {
                gd_line(x, y + h - gui_s(gui_tok("compass_tick")) * k, x, y + h, 1.0f, col);
            }
        }
        // Centre mark + the heading in degrees under it.
        gd_line(cx, y - gui_s(2), cx, y + h + gui_s(4), fmaxf(1.0f, gui_s(2.0f)), TH->select);
        char hb[8];
        snprintf(hb, sizeof(hb), "%d", ((int)lrintf(heading)) % 360);
        GuiText ht = { gui_s(TH->t_tiny + 1) * k, GW_SEMIBOLD, TH->select, GUI_CENTER, 0, false, true, 0 };
        gd_text(&ht, cx, y + h + gui_s(5), hb);
        bottom = y + h + gui_s(5) + gd_line_height(ht.size);
        // Markers: pings (type colour + icon + distance), teammates (initial).
        if (cv->ping_compass->i) {
            float ms = gui_s(gui_tok("compass_marker")) * k, my = y + h * 0.5f;
            World *w = g->world;
            vec3 me = p->sim.move.pos;
            if (c->markers_team) {
                for (int s = 0; s < MAX_PLAYERS; s++) {
                    Player *o = &w->players[s];
                    if (!o->active || s == g->local_slot || !ent_valid(w, o->ent)) continue;
                    vec3 op = w->xf[ENT_INDEX(o->ent)].pos;
                    if (v3_dist(op, me) > c->marker_max_dist) continue;
                    char ini[4] = { o->name[0] ? o->name[0] : '?', 0 };
                    // Knocked down teammates stand out (docs/REVIVE.md).
                    bool down = (w->flags[ENT_INDEX(o->ent)] & EF_DOWNED) != 0;
                    strip_marker(cx, half_w, my, compass_offset_deg(compass_bearing_deg(me, op), heading), half_fov, down ? "help" : "player", down ? TH->danger : TH->info, ms * (down ? 1.0f : 0.8f), ini);
                }
                // Bodies with DNA left (the dead see their own), fabricators reconstructing.
                ENT_FOREACH(w, i) {
                    if (w->type[i] != ET_CORPSE || !(w->flags[i] & EF_DNA)) continue;
                    vec3 cp = w->xf[i].pos;
                    float d = v3_dist(cp, me);
                    if (d > c->marker_max_dist) continue;
                    char db[16];
                    snprintf(db, sizeof(db), "DNA %.0fm", d);
                    strip_marker(cx, half_w, my, compass_offset_deg(compass_bearing_deg(me, cp), heading), half_fov, "help", TH->ping[7 % 8], ms * 0.85f, db);
                }
            }
            if (c->markers_pings) {
                const PingTypes *pt = game_ping_types();
                for (int i = 0; i < PING_MAX; i++) {
                    const Ping *pg = &g->pings.p[i];
                    if (!pg->active) continue;
                    float d = v3_dist(pg->pos, me);
                    if (d > c->marker_max_dist) continue;
                    const PingTypeDef *t = &pt->t[pg->type];
                    char db[16];
                    snprintf(db, sizeof(db), "%.0fm", d);
                    gd_alpha_push(ping_alpha(pg, g->hud_time));
                    strip_marker(cx, half_w, my, compass_offset_deg(compass_bearing_deg(me, pg->pos), heading), half_fov, t->icon, TH->ping[t->color], ms, db);
                    gd_alpha_pop();
                }
            }
        }
    }
    // Coordinates (refreshed at coords_hz).
    if (cv->coords->i && c->coords) {
        if (gui_time() - HC.coords_t >= 1.0 / c->coords_hz || !HC.coords[0]) {
            HC.coords_t = gui_time();
            compass_coords_text(p->sim.move.pos, HC.coords, sizeof(HC.coords));
        }
        GuiText ct = { gui_s(TH->t_tiny) * k, GW_MEDIUM, TH->text_dim, GUI_CENTER, 0.04f, false, true, 0 };
        gd_text(&ct, cx, bottom + gui_s(2), HC.coords);
        bottom += gui_s(2) + gd_line_height(ct.size);
    } else {
        compass_coords_text(p->sim.move.pos, HC.coords, sizeof(HC.coords));
    }
    gd_alpha_pop();
    return bottom;
}
