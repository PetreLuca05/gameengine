#include "crosshair.h"
#include "ui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "player/spectator.h"
#include "gameplay/weapons.h"
#include "gameplay/attachments.h"
#include "gui/gui_theme.h"
#include "render/render.h"
#include "cJSON.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COLORS 8

typedef struct CrossStyle {
    u32 colors[MAX_COLORS];
    char color_names[MAX_COLORS][16];
    int color_count;
    u32 enemy, friendly, outline;
    float outline_w, min_gap, max_gap, ads_fade_time, ads_dot_size, enemy_range;
    bool ads_dot, shotgun_circle;
    int circle_segments;
    float hm_time, hm_kill_time, hm_inner, hm_outer, hm_thickness, hm_kill_scale;
    float hm_alpha;                       // a hit's marker opacity, hip or aimed (a kill's is opaque)
    float hm_fade_in, hm_pop, hm_kill_pop, hm_drift, hm_kill_spacing;
    u32 hm_color, hm_head, hm_kill;
} CrossStyle;

static struct {
    CrossStyle st;
    Cvar *style, *length, *thickness, *gap, *opacity, *color, *outline, *hitmarker, *target_color;
    float ads_fade;    // 0 = shown, 1 = hidden (ADS)
    vec2 last_pos;     // where the crosshair was drawn (tests: crosshair_pos)
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
    s->ads_dot = false;   // no crosshair at all while aimed (the sights are real)
    s->ads_dot_size = 2.0f;
    s->shotgun_circle = true;
    s->circle_segments = 48;
    s->enemy_range = 150.0f;
    s->hm_time = 0.32f;
    s->hm_kill_time = 0.6f;
    s->hm_inner = 6;
    s->hm_outer = 14;
    s->hm_thickness = 2.5f;
    s->hm_kill_scale = 1.35f;
    s->hm_alpha = 0.5f;
    s->hm_fade_in = 0.04f;
    s->hm_pop = 0.35f;
    s->hm_kill_pop = 0.6f;
    s->hm_drift = 0.18f;
    s->hm_kill_spacing = 3.0f;
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
        s.hm_kill_time = num(hm, "kill_time", s.hm_kill_time);
        s.hm_inner = num(hm, "inner", s.hm_inner);
        s.hm_outer = num(hm, "outer", s.hm_outer);
        s.hm_thickness = num(hm, "thickness", s.hm_thickness);
        s.hm_kill_scale = num(hm, "kill_scale", s.hm_kill_scale);
        s.hm_alpha = saturate(num(hm, "alpha", s.hm_alpha));
        s.hm_fade_in = num(hm, "fade_in", s.hm_fade_in);
        s.hm_pop = num(hm, "pop", s.hm_pop);
        s.hm_kill_pop = num(hm, "kill_pop", s.hm_kill_pop);
        s.hm_drift = num(hm, "drift", s.hm_drift);
        s.hm_kill_spacing = num(hm, "kill_spacing", s.hm_kill_spacing);
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
    log_info("crosshair_pos: %.2f %.2f (%s) ui %.0fx%.0f", CH.last_pos.x, CH.last_pos.y, CH.last_valid ? "drawn" : "hidden", ui_width(), ui_height());
}

// Shows a hit marker as if a hit were confirmed (preview / screenshot tests).
static void cmd_hitmarker_test(int argc, const char **argv) {
    Game *g = game();
    if (!g) return;
    const char *k = argc > 1 ? argv[1] : "hit";
    g->hitmarker_kill = !strcmp(k, "kill") || !strcmp(k, "headkill");
    g->hitmarker_head = !strcmp(k, "head") || !strcmp(k, "headkill");
    g->hitmarker_time = crosshair_hitmarker_duration(g->hitmarker_kill);
}

void crosshair_init(void) {
    CH.style = cvar_reg_int("crosshair_style", 0, 0, 3, CVAR_ARCHIVE | CVAR_EDITABLE, "0 cross, 1 cross + dot, 2 dot, 3 circle");
    CH.length = cvar_reg_float("crosshair_length", 8, 1, 40, CVAR_ARCHIVE | CVAR_EDITABLE, "crosshair line length (px at 1080p)");
    CH.thickness = cvar_reg_float("crosshair_thickness", 2, 1, 8, CVAR_ARCHIVE | CVAR_EDITABLE, "crosshair line thickness (px at 1080p)");
    CH.gap = cvar_reg_float("crosshair_gap", 0, 0, 30, CVAR_ARCHIVE | CVAR_EDITABLE, "extra gap added to the spread gap (px at 1080p)");
    CH.opacity = cvar_reg_float("crosshair_opacity", 0.9f, 0.1f, 1, CVAR_ARCHIVE | CVAR_EDITABLE, "crosshair opacity");
    CH.color = cvar_reg_int("crosshair_color", 0, 0, MAX_COLORS - 1, CVAR_ARCHIVE | CVAR_EDITABLE, "crosshair color preset (assets/ui/crosshair.json)");
    CH.outline = cvar_reg_bool("crosshair_outline", true, CVAR_ARCHIVE | CVAR_EDITABLE, "dark outline around the crosshair");
    CH.hitmarker = cvar_reg_bool("crosshair_hitmarker", true, CVAR_ARCHIVE | CVAR_EDITABLE, "show hit markers");
    CH.target_color = cvar_reg_bool("crosshair_target_color", true, CVAR_ARCHIVE | CVAR_EDITABLE, "tint the crosshair over enemies / friendlies");
    cmd_register("crosshair_reload", cmd_reload, "re-read assets/ui/crosshair.json");
    cmd_register("crosshair_hitmarker_test", cmd_hitmarker_test, "crosshair_hitmarker_test [hit|head|kill|headkill]: show a hit marker (preview)");
    cmd_register("crosshair_pos", cmd_pos, "crosshair_pos: where the crosshair was drawn last frame (UI px, tests)");
    crosshair_reload();
}

float crosshair_hitmarker_duration(bool kill) {
    float t = kill ? CH.st.hm_kill_time : CH.st.hm_time;
    return t > 0 ? t : 0.25f;
}
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

// The marker's life: a quick fade in with a springy pop (starts larger, overshoots once, settles),
// a hold, then a fade out over the last half while the arms drift outwards. A hit's is see-through
// (the same hip or aimed), a kill's opaque, larger, with two parallel lines per arm, in the kill
// colour or the head colour for a headshot kill.
static void draw_hitmarker(Game *g, float cx, float cy, float s) {
    if (!CH.hitmarker->i || g->hitmarker_time <= 0) return;
    const CrossStyle *st = &CH.st;
    bool kill = g->hitmarker_kill;
    float dur = crosshair_hitmarker_duration(kill);
    float left = fminf(g->hitmarker_time, dur), el = dur - left;
    float a_in = smoothstepf(0.0f, 1.0f, saturate(el / fmaxf(st->hm_fade_in, 1e-3f)));
    float out_t = saturate(left / (dur * 0.5f));   // 1 until the last half, then down to 0
    float a = a_in * smoothstepf(0.0f, 1.0f, out_t);
    if (!kill) a *= st->hm_alpha;
    float pop = (kill ? st->hm_kill_pop : st->hm_pop) * expf(-el * 16.0f) * cosf(el * 28.0f);
    float k = (kill ? st->hm_kill_scale : 1.0f) * (1.0f + pop) * (1.0f + st->hm_drift * (1.0f - out_t));
    u32 base = kill ? (g->hitmarker_head ? st->hm_head : st->hm_kill) : (g->hitmarker_head ? st->hm_head : st->hm_color);
    u32 c = with_alpha(base, a);
    float in = st->hm_inner * s * k, out = st->hm_outer * s * k, th = st->hm_thickness * s;
    // Kill: each arm is a pair of lines, offset either side along the arm's normal.
    float off = kill ? (st->hm_kill_spacing * s + th) * 0.5f * 0.70710678f : 0.0f;
    for (int i = 0; i < 4; i++) {
        float dx = (i & 1) ? 1.0f : -1.0f, dy = (i & 2) ? 1.0f : -1.0f;
        float nx = -dy * off, ny = dx * off;   // normal to (dx, dy), length off * sqrt(2)
        for (int l = kill ? -1 : 0; l <= (kill ? 1 : 0); l += 2) {
            float ox = nx * (float)l, oy = ny * (float)l;
            ui_line(cx + dx * in + ox, cy + dy * in + oy, cx + dx * out + ox, cy + dy * out + oy, th, c);
        }
    }
}

// World point -> UI px (false: behind the camera).
static bool project_ui(vec3 p, vec2 *out) {
    vec2 px;
    if (!render_world_to_screen(p, &px)) return false;
    out->x = px.x * ui_width() / (float)DG_MAX(1, render_width());
    out->y = px.y * ui_height() / (float)DG_MAX(1, render_height());
    return true;
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

    // Where the next bullet goes (docs/AIM.md): the shots leave the camera along the view (+ the sight
    // offset), so the crosshair is that direction's projection: the screen centre, except while the
    // fire animation's camera kick turns the view (the crosshair then stays on the true aim point).
    float cx = scr_w * 0.5f, cy = scr_h * 0.5f;
    vec2 sway = gun ? weapon_aim_offset_deg(wd, &p->sim.wpn, &p->sim.move, weapon_sway_time(vcmd->seq) + g->alpha * SIM_DT) : v2(0, 0);
    vec3 aim = dir_from_angles(g->view.yaw - sway.x * DG_DEG2RAD, g->view.pitch + sway.y * DG_DEG2RAD);
    {
        vec2 px;
        if (project_ui(v3_madd(g->view.eye, aim, game_cam_mode() == 3 ? 50.0f : 1.0e4f), &px)) {
            cx = px.x;
            cy = px.y;
        }
    }

    // Target tint: other players along the aim ray turn it blue (friendly); enemies leave it as it is
    // (2026-10-20: no red tint).
    u32 base = st->colors[DG_CLAMP(CH.color->i, 0, st->color_count - 1)];
    if (CH.target_color->i) {
        EntityId t = weapon_trace_target(w, vs, g->view.eye, aim, gun ? fminf(wd->range, st->enemy_range) : 3.0f);
        if (t && ent_valid(w, t) && w->type[ENT_INDEX(t)] == ET_PLAYER) base = st->friendly;
    }
    float alpha = CH.opacity->f;
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
    float gap_draw = fminf(fmaxf(gap, st->min_gap * s) + CH.gap->f * s, st->max_gap * s);

    float vis = 1.0f - CH.ads_fade;
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
