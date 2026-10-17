// Main menu background (docs/MENU_LOBBY.md "Main menu shell"): a dev scene (src/game/scene.h) on a
// local world - the player's character in the dev room, drawn whole by the normal pipeline. The
// camera drifts slowly in front of the character; on the Player and Cosmetics tabs it frames the
// character inside the preview rect and the mouse orbits / zooms it. The selected map's season,
// time and weather light it (frozen time, fixed weather).
#include "mm_internal.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "game/scene.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include "world/season.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static vec3 player_focus(World *w, float *facing) {
    Player *p = game_local_player();
    Game *g = game();
    *facing = g ? g->view.yaw : 0.0f;
    if (!p) return v3(0, 1.3f, 0);
    (void)w;
    return v3_add(p->sim.move.pos, v3(0, 1.15f, 0));
}

static void look_at(RenderCamera *cam, vec3 eye, vec3 target) {
    vec3 d = v3_norm(v3_sub(target, eye));
    cam->pos = eye;
    cam->yaw = atan2f(-d.x, -d.z);
    cam->pitch = asinf(clampf(d.y, -1.0f, 1.0f));
    cam->roll = 0;
}

static bool scene_camera_fn(World *w, RenderCamera *cam) {
    MainMenu *M = mm();
    float facing;
    vec3 focus = player_focus(w, &facing);
    vec3 fwd = forward_from_yaw(facing);
    float t = (float)gui_time();
    bool framed = M->preview_rect.w > 0;
    float yaw, pitch, dist;
    if (framed) {
        yaw = M->orbit_yaw;
        pitch = M->orbit_pitch;
        dist = 2.6f / fmaxf(0.4f, M->orbit_zoom);
    } else {
        // Slow drift: a gentle sway around a three-quarter front view.
        yaw = 0.42f + 0.22f * sinf(t * 0.045f);
        pitch = -0.06f + 0.03f * sinf(t * 0.031f + 1.0f);
        dist = 3.4f + 0.25f * sinf(t * 0.027f);
    }
    // Orbit about the character: yaw 0 = straight in front of it.
    float base = facing + DG_PI;   // from the front
    vec3 dir = dir_from_angles(base + yaw, pitch);
    vec3 eye = v3_madd(focus, dir, -dist);
    (void)fwd;
    // Put the character where the UI wants it: the preview rect's centre, else right of centre
    // (the menu is on the left). Shift the aim point sideways by the matching view offset.
    float sw = gui_w() > 0 ? gui_w() : 1.0f, sh = gui_h() > 0 ? gui_h() : 1.0f;
    float fx = framed ? (M->preview_rect.x + M->preview_rect.w * 0.5f) / sw : 0.64f;
    float fy = framed ? (M->preview_rect.y + M->preview_rect.h * 0.5f) / sh : 0.5f;
    float vfov = cam->fov_deg > 1 ? cam->fov_deg : 70.0f;
    float half_v = tanf(vfov * 0.5f * DG_DEG2RAD), half_h = half_v * sw / sh;
    vec3 right = right_from_yaw(base + yaw);
    vec3 target = v3_madd(focus, right, -(fx * 2.0f - 1.0f) * half_h * dist);
    target.y += (fy * 2.0f - 1.0f) * half_v * dist;
    eye = v3_add(eye, v3_sub(target, focus));
    look_at(cam, eye, target);
    return true;
}

static bool scene_enter_fn(void) { return game_world() != NULL; }
static void scene_leave_fn(void) {}

static const GameScene k_scene = {
    "mainmenu", "main menu background (the player in the dev room, slow camera drift)",
    scene_enter_fn, scene_leave_fn, scene_camera_fn, NULL, true, true,
};

void mm_scene_register(void) { scene_register(&k_scene); }

void mm_scene_enter(void) {
    if (!scene_active() || strcmp(scene_active()->name, "mainmenu")) scene_enter("mainmenu");
    mm()->env_dirty = true;
}

void mainmenu_leave_scene(void) {
    if (scene_active() && !strcmp(scene_active()->name, "mainmenu")) scene_leave();
}

// The background takes the selection's look: the season's calendar day and the start time (frozen),
// the start weather (or the season's typical weather), fixed. The Player tab without "Preview in
// map lighting" shows a neutral summer noon instead.
void mm_scene_apply_environment(void) {
    MainMenu *M = mm();
    World *w = game_world();
    if (!w || !w->authority || lobby_state() == LS_LOADING || lobby_state() == LS_IN_GAME) return;
    LobbySettings s;
    lobby_settings_default(&s);
    s.season = (u8)DG_CLAMP(M->season, 0, SEASON_COUNT - 1);
    s.start_hour = (u8)DG_CLAMP(M->hour, 0, 23);
    dg_strcpy(s.weather, sizeof(s.weather), M->weather);
    if (!s.weather[0]) {
        // The season's most typical preset (the highest weight): a stable background, not a random pick.
        const SeasonDef *sd = &((const SeasonTable *)weather_seasons())->s[s.season];
        int best = 0;
        for (int i = 1; i < sd->weather_count; i++) if (sd->weight[i] > sd->weight[best]) best = i;
        if (sd->weather_count) dg_strcpy(s.weather, sizeof(s.weather), sd->weather[best]);
    }
    bool studio = (M->tab == MT_PLAYER || M->tab == MT_COSMETICS) && !M->preview_tod;
    if (studio) { s.season = SEASON_SUMMER; s.start_hour = 12; dg_strcpy(s.weather, sizeof(s.weather), "little_cloudy"); }
    s.dynamic_weather = false;
    lobby_apply_environment(w, &s);
    tod_set_day_length(&w->tod, (double)w->tick, 0.0f);   // frozen: the look stays as selected
    M->env_dirty = false;
}

void mm_select_map(const char *id) {
    MainMenu *M = mm();
    const MapInfo *mi = lobby_map_info(id);
    if (!mi) return;
    bool changed = strcmp(M->map, id) != 0;
    dg_strcpy(M->map, sizeof(M->map), id);
    if (changed) {
        // The map's defaults: its season, start time and weather.
        M->season = mi->season;
        M->hour = (int)mi->hour;
        dg_strcpy(M->weather, sizeof(M->weather), mi->weather);
    }
    M->env_dirty = true;
    Profile *p = lobby_profile();
    dg_strcpy(p->last_map, sizeof(p->last_map), id);
}
