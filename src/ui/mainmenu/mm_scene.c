// Main menu background (docs/MENU_LOBBY.md "Main menu shell"): a pale blue backdrop and the player's
// character alone on it (chars_preview: the inventory's studio render) holding the SMG in the
// hip-fire hold, centred, the same on every tab (a drag on the Player / Cosmetics tabs turns it);
// Cosmetics > Items closes on the gun. The local dev room world still runs under it (a dev scene, src/game/scene.h:
// the lobby hosts from it) but is covered; the selected map's season / time / weather still go to it.
#include "mm_internal.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "game/scene.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include "world/season.h"
#include "gameplay/characters.h"
#include "gameplay/items.h"
#include "gameplay/inventory/inv_tree.h"
#include "render/char_preview_view.h"

#define WHITE UI_RGBA(255, 255, 255, 255)   // lint:ok image tint (no colour change)
#define CHAR_PREVIEW_MENU (CHAR_PREVIEW_HUD + MAX_PLAYERS)   // its own preview target, after the HUD cards
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
    "mainmenu", "main menu background world (covered by the menu's backdrop + character render)",
    scene_enter_fn, scene_leave_fn, scene_camera_fn, NULL, true, true,
};

void mm_scene_register(void) { scene_register(&k_scene); }

#define MM_TURN (10.0f * DG_DEG2RAD)   // the character's turn to its left (every tab)
#define MM_SIDE (90.0f * DG_DEG2RAD)   // Cosmetics > Items: facing the screen's right (the gun's right side to the camera; char_preview squares it)

// The whole screen, the character centred, the same framing on every tab; Cosmetics > Items turns it
// to face the left and closes the camera on the SMG (half the screen wide, centred). globe: the
// backdrop only (the Maps tab).
void mm_background_draw(bool globe) {
    MainMenu *M = mm();
    float W = gui_w(), H = gui_h();
    gd_rect_grad(gui_rect(0, 0, W, H), UI_RGBA(214, 230, 242, 255), UI_RGBA(178, 203, 224, 255));
    Game *g = game();
    World *w = game_world();
    Player *p = game_local_player();
    if (globe || !g || !w || !p || W < 8 || H < 8) return;
    float dt = gui_dt();
    bool items = M->tab == MT_COSMETICS && M->cos_items;
    M->items_t += ((items ? 1.0f : 0.0f) - M->items_t) * saturate(dt * 5.0f);
    float e = M->items_t * M->items_t * (3.0f - 2.0f * M->items_t);
    if (!gui_mouse_down(0)) M->orbit_yaw *= 1.0f - saturate(dt * 3.0f);   // a drag's turn eases back
    CharPreviewIn in;
    memset(&in, 0, sizeof(in));
    in.target = CHAR_PREVIEW_MENU;
    // The render size (the image is stretched over the screen). Text pages (Settings, Patch notes,
    // Credits): a tenth of it into its own target - stretched, it reads as a blur behind the text.
    bool soft = M->tab == MT_SETTINGS || M->tab == MT_NEWS || M->tab == MT_CREDITS;
    float k = soft ? 0.1f : fminf(1.0f, 2560.0f / W);
    in.w = (int)lrintf(W * k);
    in.h = (int)lrintf(H * k);
    if (soft) in.target = CHAR_PREVIEW_MENU + 1;
    in.yaw = lerpf(MM_TURN, MM_SIDE, e) + M->orbit_yaw;
    in.hold = 1;   // the hip-fire hold, as standing still in game
    in.item_focus = e;
    in.item_fill = 0.55f;   // the longest bounds side; the gun itself then spans about half the screen
    in.held_item = w->items ? item_find(w->items, "smg") : 0;
    static const int worn[CWORN_COUNT] = { EQ_HEADWEAR, EQ_BODY_ARMOR, EQ_BACKPACK };
    for (int n = 0; n < CWORN_COUNT; n++) {
        const InvItem *it = inv_equipped_c(&p->inv.t, worn[n]);
        in.worn[n] = it ? it->s.item : 0;
    }
    TextureId tex = chars_preview(w, g->local_slot, &in);
    if (tex) gd_image(gui_rect(0, 0, W, H), tex, v4(0, 0, 1, 1), WHITE);
}

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
    mm_party_sync();
}

// Play's mode: Story selects a chapter map (Chapter select), Trials a development map (the party's).
void mm_set_play_mode(int mode) {
    MainMenu *M = mm();
    M->play_mode = mode;
    M->play_page = PP_MAIN;
    const MapInfo *cur = lobby_map_info(M->map);
    bool chapter = cur && cur->chapter[0];
    if ((mode == PM_STORY) == chapter && cur) return;
    const MapList *ml = lobby_maps();
    const char *want = NULL;
    if (mode == PM_TRIALS && M->trials_map[0] && lobby_map_info(M->trials_map)) want = M->trials_map;
    for (int i = 0; i < ml->count && !want; i++) if ((ml->maps[i].chapter[0] != 0) == (mode == PM_STORY)) want = ml->maps[i].id;
    if (want) mm_select_map(want);
}

// The host's party follows the menu's selection: the map and its start look (season, time, weather).
// A chapter map stays out of it (chapters start through Story).
void mm_party_sync(void) {
    MainMenu *M = mm();
    LobbyState st = lobby_state();
    const MapInfo *mi = lobby_map_info(M->map);
    if (mi && !mi->chapter[0]) dg_strcpy(M->trials_map, sizeof(M->trials_map), M->map);
    if (!mi || mi->chapter[0]) return;
    if (!lobby_is_host() || (st != LS_LOBBY && st != LS_COUNTDOWN)) return;
    LobbySettings s = lobby_get()->settings;
    dg_strcpy(s.map, sizeof(s.map), M->map);
    s.season = (u8)DG_CLAMP(M->season, 0, SEASON_COUNT - 1);
    s.start_hour = (u8)DG_CLAMP(M->hour, 0, 23);
    dg_strcpy(s.weather, sizeof(s.weather), M->weather);
    s.dynamic_weather = M->dynamic_weather;
    lobby_set_settings(&s);
}
