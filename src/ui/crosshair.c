#include "crosshair.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "player/spectator.h"
#include "gameplay/weapons.h"
#include "gameplay/aim.h"
#include "gameplay/attachments.h"
#include "gui/gui_theme.h"
#include "render/render.h"
#include "cJSON.h"
#include <math.h>
#include <stdlib.h>

#define MAX_COLORS 8

typedef struct CrossStyle {
    u32 colors[MAX_COLORS];
    char color_names[MAX_COLORS][16];
    int color_count;
    u32 enemy, friendly, outline;
    float outline_w, min_gap, max_gap, ads_fade_time, ads_dot_size, enemy_range;
    bool ads_dot, shotgun_circle;
    int circle_segments;
    float hm_time, hm_inner, hm_outer, hm_thickness, hm_kill_scale;
    u32 hm_color, hm_head, hm_kill;
} CrossStyle;

static struct {
    CrossStyle st;
    Cvar *style, *length, *thickness, *gap, *opacity, *color, *outline, *follow_kick, *hitmarker, *target_color;
    Cvar *obstruct, *obstruct_lerp;
    float ads_fade;    // 0 = shown, 1 = hidden (ADS)
    // Aim model (docs/AIM.md): the crosshair's offset from the aim point to the obstruction (smoothed).
    vec2 obs_off;
    float obs_w;       // 0 = clear, 1 = obstructed (smoothed)
    int obs_cls;       // last obstruction class (kept while fading out)
    vec2 last_pos;     // where the crosshair was drawn (tests: aim_report / crosshair_pos)
    vec2 last_target;  // where it was heading that frame (Q's projection when obstructed, else P's)
    bool last_valid;
} CH;

static u32 rgba_arr(const cJSON *o, const char *k, u32 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    int c[4] = { 255, 255, 255, 255 };
    for (int i = 0; i < 4 && i < cJSON_GetArraySize(v); i++) c[i] = DG_CLAMP((int)cJSON_GetArrayItem(v, i)->valuedouble, 0, 255);
    return UI_RGBA(c[0], c[1], c[2], c[3]);
}

static float num(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static bool flag(const cJSON *o, const char *k, bool def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static void style_defaults(CrossStyle *s) {
    memset(s, 0, sizeof(*s));
    s->colors[0] = UI_RGBA(240, 240, 236, 255);
    dg_strcpy(s->color_names[0], sizeof(s->color_names[0]), "White");
    s->color_count = 1;
    s->enemy = UI_RGBA(255, 70, 55, 255);
    s->friendly = UI_RGBA(90, 170, 255, 255);
    s->outline = UI_RGBA(0, 0, 0, 170);
    s->outline_w = 1.0f;
    s->min_gap = 3.0f;
    s->max_gap = 260.0f;
    s->ads_fade_time = 0.12f;
    s->ads_dot = true;
    s->ads_dot_size = 2.0f;
    s->shotgun_circle = true;
    s->circle_segments = 48;
    s->enemy_range = 150.0f;
    s->hm_time = 0.25f;
    s->hm_inner = 6;
    s->hm_outer = 14;
    s->hm_thickness = 2.5f;
    s->hm_kill_scale = 1.35f;
    s->hm_color = UI_RGBA(255, 255, 255, 255);
    s->hm_head = UI_RGBA(255, 200, 60, 255);
    s->hm_kill = UI_RGBA(255, 60, 40, 255);
}

bool crosshair_reload(void) {
    CrossStyle s;
    style_defaults(&s);
    char buf[512];
    char *text = platform_read_file(platform_data_path("assets/ui/crosshair.json", buf, sizeof(buf)), NULL);
    if (!text) { log_warn("crosshair: assets/ui/crosshair.json missing, using defaults"); CH.st = s; return false; }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) { log_error("crosshair: JSON parse error near %.40s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?"); CH.st = s; return false; }
    const cJSON *cols = cJSON_GetObjectItemCaseSensitive(root, "colors");
    if (cJSON_IsArray(cols) && cJSON_GetArraySize(cols) > 0) {
        s.color_count = 0;
        const cJSON *c;
        cJSON_ArrayForEach(c, cols) {
            if (s.color_count >= MAX_COLORS) break;
            const cJSON *n = cJSON_GetObjectItemCaseSensitive(c, "name");
            dg_strcpy(s.color_names[s.color_count], sizeof(s.color_names[0]), cJSON_IsString(n) ? n->valuestring : "?");
            s.colors[s.color_count++] = rgba_arr(c, "rgba", UI_RGBA(255, 255, 255, 255));
        }
    }
    s.enemy = rgba_arr(root, "enemy_color", s.enemy);
    s.friendly = rgba_arr(root, "friendly_color", s.friendly);
    s.outline = rgba_arr(root, "outline_color", s.outline);
    s.outline_w = num(root, "outline_width", s.outline_w);
    s.min_gap = num(root, "min_gap", s.min_gap);
    s.max_gap = num(root, "max_gap", s.max_gap);
    s.ads_fade_time = num(root, "ads_fade_time", s.ads_fade_time);
    s.ads_dot = flag(root, "ads_dot", s.ads_dot);
    s.ads_dot_size = num(root, "ads_dot_size", s.ads_dot_size);
    s.shotgun_circle = flag(root, "shotgun_circle", s.shotgun_circle);
    s.circle_segments = DG_CLAMP((int)num(root, "circle_segments", (float)s.circle_segments), 8, 128);
    s.enemy_range = num(root, "enemy_check_range", s.enemy_range);
    const cJSON *hm = cJSON_GetObjectItemCaseSensitive(root, "hitmarker");
    if (cJSON_IsObject(hm)) {
        s.hm_time = num(hm, "time", s.hm_time);
        s.hm_inner = num(hm, "inner", s.hm_inner);
        s.hm_outer = num(hm, "outer", s.hm_outer);
        s.hm_thickness = num(hm, "thickness", s.hm_thickness);
        s.hm_kill_scale = num(hm, "kill_scale", s.hm_kill_scale);
        s.hm_color = rgba_arr(hm, "color", s.hm_color);
        s.hm_head = rgba_arr(hm, "head_color", s.hm_head);
        s.hm_kill = rgba_arr(hm, "kill_color", s.hm_kill);
    }
    cJSON_Delete(root);
    CH.st = s;
    if (CH.color && CH.color->i >= s.color_count) cvar_set_int(CH.color, 0);
    return true;
}

static void cmd_reload(int argc, const char **argv) {
    (void)argc; (void)argv;
    if (crosshair_reload()) log_info("crosshair: style reloaded (%d colors)", CH.st.color_count);
}

static void cmd_pos(int argc, const char **argv) {
    (void)argc; (void)argv;
    log_info("crosshair_pos: %.1f %.1f (%s) obstruction %s %.2f offset %.1f %.1f ui %.0fx%.0f target %.1f %.1f", CH.last_pos.x, CH.last_pos.y,
             CH.last_valid ? "drawn" : "hidden", aim_class_name(CH.obs_cls), CH.obs_w, CH.obs_off.x, CH.obs_off.y, ui_width(), ui_height(), CH.last_target.x,
             CH.last_target.y);
}

void crosshair_init(void) {
    CH.style = cvar_reg_int("crosshair_style", 0, 0, 3, CVAR_ARCHIVE, "0 cross, 1 cross + dot, 2 dot, 3 circle");
    CH.length = cvar_reg_float("crosshair_length", 8, 1, 40, CVAR_ARCHIVE, "crosshair line length (px at 1080p)");
    CH.thickness = cvar_reg_float("crosshair_thickness", 2, 1, 8, CVAR_ARCHIVE, "crosshair line thickness (px at 1080p)");
    CH.gap = cvar_reg_float("crosshair_gap", 0, 0, 30, CVAR_ARCHIVE, "extra gap added to the spread gap (px at 1080p)");
    CH.opacity = cvar_reg_float("crosshair_opacity", 0.9f, 0.1f, 1, CVAR_ARCHIVE, "crosshair opacity");
    CH.color = cvar_reg_int("crosshair_color", 0, 0, MAX_COLORS - 1, CVAR_ARCHIVE, "crosshair color preset (assets/ui/crosshair.json)");
    CH.outline = cvar_reg_bool("crosshair_outline", true, CVAR_ARCHIVE, "dark outline around the crosshair");
    CH.follow_kick = cvar_reg_bool("crosshair_follow_kick", false, CVAR_ARCHIVE, "pin the crosshair to the screen center (follows the camera kick) instead of the true aim point");
    CH.hitmarker = cvar_reg_bool("crosshair_hitmarker", true, CVAR_ARCHIVE, "show hit markers");
    CH.target_color = cvar_reg_bool("crosshair_target_color", true, CVAR_ARCHIVE, "tint the crosshair over enemies / friendlies");
    CH.obstruct = cvar_reg_bool("crosshair_obstruct", true, CVAR_ARCHIVE, "move the crosshair to where the bullet will land when something is between the muzzle and the aim point (docs/AIM.md)");
    CH.obstruct_lerp = cvar_reg_float("crosshair_obstruct_lerp", 0.1f, 0.0f, 1.0f, CVAR_ARCHIVE, "time constant (s) of the crosshair's move to and from an obstruction");
    cmd_register("crosshair_reload", cmd_reload, "re-read assets/ui/crosshair.json");
    cmd_register("crosshair_pos", cmd_pos, "crosshair_pos: where the crosshair was drawn last frame (UI px) and its obstruction state (tests)");
    crosshair_reload();
}

float crosshair_hitmarker_duration(void) { return CH.st.hm_time > 0 ? CH.st.hm_time : 0.25f; }
int crosshair_color_count(void) { return CH.st.color_count; }
const char *crosshair_color_name(int i) { return (i >= 0 && i < CH.st.color_count) ? CH.st.color_names[i] : "?"; }

static u32 with_alpha(u32 c, float a) {
    u32 al = (u32)((float)((c >> 24) & 0xFF) * saturate(a));
    return (c & 0x00FFFFFFu) | (al << 24);
}

static void rect_ol(float x, float y, float w, float h, u32 c, u32 oc, float ow) {
    if (ow > 0) ui_rect(x - ow, y - ow, w + 2 * ow, h + 2 * ow, oc);
    ui_rect(x, y, w, h, c);
}

static void circle(float cx, float cy, float r, float th, int seg, u32 c) {
    for (int i = 0; i < seg; i++) {
        float a0 = (float)i / (float)seg * 2.0f * DG_PI, a1 = (float)(i + 1) / (float)seg * 2.0f * DG_PI;
        ui_line(cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, th, c);
    }
}

static void draw_hitmarker(Game *g, float cx, float cy, float s) {
    if (!CH.hitmarker->i || g->hitmarker_time <= 0) return;
    const CrossStyle *st = &CH.st;
    float a = saturate(g->hitmarker_time / crosshair_hitmarker_duration());
    u32 base = g->hitmarker_kill ? st->hm_kill : (g->hitmarker_head ? st->hm_head : st->hm_color);
    u32 c = with_alpha(base, a);
    float k = g->hitmarker_kill ? st->hm_kill_scale : 1.0f;
    float in = st->hm_inner * s * k, out = st->hm_outer * s * k, th = st->hm_thickness * s;
    ui_line(cx - in, cy - in, cx - out, cy - out, th, c);
    ui_line(cx + in, cy - in, cx + out, cy - out, th, c);
    ui_line(cx - in, cy + in, cx - out, cy + out, th, c);
    ui_line(cx + in, cy + in, cx + out, cy + out, th, c);
}

// World point -> UI px (false: behind the camera).
static bool project_ui(vec3 p, vec2 *out) {
    vec2 px;
    if (!render_world_to_screen(p, &px)) return false;
    out->x = px.x * ui_width() / (float)DG_MAX(1, render_width());
    out->y = px.y * ui_height() / (float)DG_MAX(1, render_height());
    return true;
}

// "Barrel blocked": a barrel line ending on a hatched wall.
static void glyph_barrel(float cx, float cy, float s, u32 c) {
    float th = fmaxf(1.0f, 2.0f * s);
    ui_line(cx - 12 * s, cy, cx - 1 * s, cy, th * 1.5f, c);
    ui_line(cx + 1 * s, cy - 8 * s, cx + 1 * s, cy + 8 * s, th, c);
    for (int i = -1; i <= 1; i++) ui_line(cx + 1 * s, cy + (float)i * 5 * s, cx + 6 * s, cy + (float)i * 5 * s - 4 * s, th * 0.75f, c);
}

// "Friendly in the line of fire": a head-and-shoulders figure.
static void glyph_friendly(float cx, float cy, float s, u32 c) {
    float th = fmaxf(1.0f, 2.0f * s);
    circle(cx, cy - 4 * s, 3.5f * s, th, 16, c);
    ui_line(cx - 7 * s, cy + 8 * s, cx - 4 * s, cy + 2 * s, th, c);
    ui_line(cx - 4 * s, cy + 2 * s, cx + 4 * s, cy + 2 * s, th, c);
    ui_line(cx + 4 * s, cy + 2 * s, cx + 7 * s, cy + 8 * s, th, c);
}

// Through a magnified optic: a faint red frame around the screen edge while obstructed.
static void red_edge(float w, float h, float a, float s) {
    float e = 10.0f * s;
    u32 c = UI_RGBA(220, 40, 30, (int)(110.0f * saturate(a)));
    ui_rect(0, 0, w, e, c);
    ui_rect(0, h - e, w, e, c);
    ui_rect(0, e, e, h - 2 * e, c);
    ui_rect(w - e, e, e, h - 2 * e, c);
}

void crosshair_draw(float dt) {
    Game *g = game();
    World *w = game_world();
    // The viewed player's crosshair: the local one, or a spectated player's in first person (its
    // spread, sway and focus). Third person / free camera spectating: none.
    Player *p = game_view_player();
    int vs = game_view_slot();
    CH.last_valid = false;
    if (!g || !w || !p || p->sim.move.mode == MOVE_DEAD || g->menu_open || g->inventory_open || g->container_open) return;
    if (spectator_mode() != SPEC_OFF && !spectator_first_person()) return;
    const UserCmd *vcmd = game_view_cmd();
    const CrossStyle *st = &CH.st;
    float s = ui_height() / 1080.0f;
    float scr_w = ui_width(), scr_h = ui_height();
    const WeaponDef *wd = weapon_equipped_def(w, vs);
    bool gun = wd && wd->kind != WPN_MELEE;
    if (p->sim.move.mode == MOVE_HANG && !weapon_is_sidearm(wd)) gun = false;   // slung while hanging (only a sidearm comes out)
    bool ads = gun && p->sim.wpn.ads_t > 0.5f;

    // ADS fade (presentation-rate).
    float rate = dt / fmaxf(0.01f, st->ads_fade_time);
    CH.ads_fade = ads ? fminf(1.0f, CH.ads_fade + rate) : fmaxf(0.0f, CH.ads_fade - rate);

    // Where the next bullet goes: the view (+ sway); the camera adds the kick spring on top.
    float cx = scr_w * 0.5f, cy = scr_h * 0.5f;
    vec2 sway = gun ? weapon_aim_offset_deg(wd, &p->sim.wpn, &p->sim.move, weapon_sway_time(vcmd->seq) + g->alpha * SIM_DT) : v2(0, 0);
    vec3 aim = dir_from_angles(g->view.yaw - sway.x * DG_DEG2RAD, g->view.pitch + sway.y * DG_DEG2RAD);
    // Aim model (docs/AIM.md): the reticle is the aim point P of the camera ray; the bullet leaves the
    // muzzle towards it and lands on Q when something is in between. The viewed player's solve (a
    // spectator gets the target's from its forwarded commands: the muzzle claim and the view).
    AimSolve as;
    bool have_aim = gun && weapon_aim_preview(w, vs, player_eye_pos(&p->sim.move), g->view.yaw, g->view.pitch, vcmd, &as);
    if (!CH.follow_kick->i) {
        vec2 px;
        if (project_ui(have_aim ? as.p : v3_madd(g->view.eye, aim, 50.0f), &px)) {
            cx = px.x;
            cy = px.y;
        }
    }
    float ccx = cx, ccy = cy;   // the unobstructed crosshair (ghost while obstructed)
    bool obstructed = have_aim && CH.obstruct->i && as.cls != AIM_CLEAR;
    vec2 qpx = v2(cx, cy);
    if (obstructed && !project_ui(as.q, &qpx)) obstructed = false;
    {
        float k = aim_lerp_k(dt, CH.obstruct_lerp->f);
        vec2 want = obstructed ? v2(qpx.x - cx, qpx.y - cy) : v2(0, 0);
        // Settled on an obstruction: follow Q exactly (a Q at point blank moves a few px with the
        // weapon's idle sway every frame; easing it would leave the crosshair behind the impact).
        // The ease is for the moves to and from it.
        float dx = want.x - CH.obs_off.x, dy = want.y - CH.obs_off.y;
        if (obstructed && CH.obs_w > 0.98f && dx * dx + dy * dy < 64.0f) k = 1.0f;
        CH.obs_off.x += dx * k;
        CH.obs_off.y += dy * k;
        CH.obs_w += ((obstructed ? 1.0f : 0.0f) - CH.obs_w) * k;
        CH.last_target = obstructed ? qpx : v2(cx, cy);
        if (obstructed) CH.obs_cls = as.cls;
        if (!gun || !CH.obstruct->i) { CH.obs_off = v2(0, 0); CH.obs_w = 0.0f; }
    }
    // Through a magnified optic the reticle cannot move: the impact marker moves instead.
    bool scoped = false;
    if (ads && game_ads_vis() > 0.9f) {
        const InvSlot *es = weapon_equipped(w, vs);
        const AttachmentDef *o = es ? attach_optic(w->items, es->att) : NULL;
        scoped = o && o->zoom >= 3.0f;
    }
    if (!scoped) {
        cx += CH.obs_off.x;
        cy += CH.obs_off.y;
    }

    // Target tint: hitbox along the aim ray (enemies red, other players blue).
    u32 base = st->colors[DG_CLAMP(CH.color->i, 0, st->color_count - 1)];
    if (CH.target_color->i) {
        EntityId t = weapon_trace_target(w, vs, g->view.eye, aim, gun ? fminf(wd->range, st->enemy_range) : 3.0f);
        if (t && ent_valid(w, t)) {
            int ti = (int)ENT_INDEX(t);
            base = w->type[ti] == ET_PLAYER ? st->friendly : st->enemy;
        }
    }
    float alpha = CH.opacity->f;
    float ow_k = CH.obs_w;
    if (ow_k > 0.01f) {
        const GuiTheme *th = gui_theme();
        base = gui_rgba_lerp(base, CH.obs_cls == AIM_FRIENDLY ? th->crosshair_friendly : th->crosshair_obstructed, saturate(ow_k));
    }
    u32 col = with_alpha(base, alpha);
    u32 ocol = CH.outline->i ? with_alpha(st->outline, alpha * ((st->outline >> 24) & 0xFF) / 255.0f) : 0;
    float ow = CH.outline->i ? st->outline_w * fmaxf(1.0f, s) : 0.0f;

    if (!gun) {
        // Melee / empty hands: small dot, accent over interactables.
        float d = 4 * s;
        u32 c = (vs == g->local_slot ? g->focus : (EntityId)vcmd->use_target) ? UI_RGBA(220, 130, 45, 255) : col;
        rect_ol(cx - d * 0.5f, cy - d * 0.5f, d, d, c, ocol, ow);
        draw_hitmarker(g, cx, cy, s);
        return;
    }

    // Exact cone projection (interpolated between ticks).
    float spread = lerpf(g->spread_prev, g->spread_cur, g->alpha);
    float fov_v = (g->view.fov > 1 ? g->view.fov : 75.0f) * DG_DEG2RAD;
    float gap = tanf(spread * 0.5f * DG_DEG2RAD) * (scr_h * 0.5f) / tanf(fov_v * 0.5f);
    // Obstructed: the cone where it actually ends (radius at |Q - muzzle|, seen from the camera).
    if (obstructed || ow_k > 0.01f) {
        float gq = obstructed ? aim_spread_px(spread, as.q_dist, v3_dist(g->view.eye, as.q), fov_v, scr_h) : gap;
        gap = lerpf(gap, gq, saturate(ow_k));
    }
    float gap_draw = fminf(fmaxf(gap, st->min_gap * s) + CH.gap->f * s, st->max_gap * s);

    if (ow_k > 0.01f) {
        // The normal aim point as a faint ghost; the glyph of the obstruction under the crosshair.
        const GuiTheme *th = gui_theme();
        u32 gc = with_alpha(st->colors[DG_CLAMP(CH.color->i, 0, st->color_count - 1)], 0.35f * alpha * saturate(ow_k));
        float d = 3.0f * fmaxf(1.0f, s);
        if (!scoped) rect_ol(ccx - d * 0.5f, ccy - d * 0.5f, d, d, gc, 0, 0);
        u32 kc = with_alpha(CH.obs_cls == AIM_FRIENDLY ? th->crosshair_friendly : th->crosshair_obstructed, alpha * saturate(ow_k));
        float gy = (scoped ? ccy : cy) + gap_draw + CH.length->f * s + 16.0f * s;
        float gx = scoped ? ccx : cx;
        if (CH.obs_cls == AIM_BARREL) glyph_barrel(gx, gy, s, kc);
        else if (CH.obs_cls == AIM_FRIENDLY) glyph_friendly(gx, gy, s, kc);
        if (scoped) {
            // The reticle stays: the impact marker sits where Q projects, inside the scope view.
            float r = 6.0f * s;
            vec2 m = v2(ccx + CH.obs_off.x, ccy + CH.obs_off.y);
            ui_line(m.x - r, m.y - r, m.x + r, m.y + r, fmaxf(1.0f, 2.0f * s), kc);
            ui_line(m.x - r, m.y + r, m.x + r, m.y - r, fmaxf(1.0f, 2.0f * s), kc);
            red_edge(scr_w, scr_h, ow_k, s);
        }
    }

    // Red dots / iron sights: the moved crosshair shows over the sight while obstructed.
    float vis = fmaxf(1.0f - CH.ads_fade, scoped ? 0.0f : saturate(ow_k));
    if (vis > 0.01f) {
        u32 c = with_alpha(col, vis * ((col >> 24) & 0xFF) / 255.0f);
        u32 oc = with_alpha(ocol, vis * ((ocol >> 24) & 0xFF) / 255.0f);
        float len = CH.length->f * s, th = fmaxf(1.0f, CH.thickness->f * s);
        int style = CH.style->i;
        bool pellets = wd->pellets > 1;
        if ((pellets && st->shotgun_circle) || style == 3) {
            // Shotgun: the pellet cone itself.
            if (ow > 0) circle(cx, cy, gap_draw, th + 2 * ow, st->circle_segments, oc);
            circle(cx, cy, gap_draw, th, st->circle_segments, c);
            rect_ol(cx - th * 0.5f, cy - th * 0.5f, th, th, c, oc, ow);
        } else if (style == 2) {
            float d = th * 1.5f;
            rect_ol(cx - d * 0.5f, cy - d * 0.5f, d, d, c, oc, ow);
        } else {
            rect_ol(cx - gap_draw - len, cy - th * 0.5f, len, th, c, oc, ow);
            rect_ol(cx + gap_draw, cy - th * 0.5f, len, th, c, oc, ow);
            rect_ol(cx - th * 0.5f, cy - gap_draw - len, th, len, c, oc, ow);
            rect_ol(cx - th * 0.5f, cy + gap_draw, th, len, c, oc, ow);
            if (style == 1) rect_ol(cx - th * 0.5f, cy - th * 0.5f, th, th, c, oc, ow);
        }
    }
    if (st->ads_dot && CH.ads_fade > 0.01f) {
        // Our box guns have no real sights: a small dot marks the aim point while aiming.
        float d = st->ads_dot_size * fmaxf(1.0f, s);
        u32 c = with_alpha(col, CH.ads_fade * ((col >> 24) & 0xFF) / 255.0f);
        u32 oc = with_alpha(ocol, CH.ads_fade * ((ocol >> 24) & 0xFF) / 255.0f);
        rect_ol(cx - d * 0.5f, cy - d * 0.5f, d, d, c, oc, ow);
    }
    CH.last_pos = v2(cx, cy);
    CH.last_valid = true;
    draw_hitmarker(g, cx, cy, s);
}
