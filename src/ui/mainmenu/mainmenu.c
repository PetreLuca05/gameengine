// Main menu shell (src/ui/mainmenu/mainmenu.h, docs/MENU_LOBBY.md "Screen map").
//
//   splash (any key skips) -> MAIN MENU
//     top tab bar: Play · Player · Cosmetics · Maps · Settings · Credits   (Q / E, LB / RB, 1-6, mouse)
//                  power icon -> CONFIRM "Quit to desktop?"
//     content: the active tab (assets/ui/screens/mainmenu_<tab>.json); in a lobby the Play tab is
//              the lobby screen and Maps / Credits are locked; LOADING: the loading screen alone
//     footer: avatar · Steam name · status · version · relay ping        news / patch notes
#include "mm_internal.h"
#include "ui/menus.h"
#include "ui/screens/screens.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "game/scene.h"
#include "audio/audio.h"
#include "assets/assets.h"
#include "world/season.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MainMenu g_mm;
MainMenu *mm(void) { return &g_mm; }
#define M (&g_mm)

static const char *const k_tab_names[MT_COUNT] = { "Play", "Player", "Cosmetics", "Maps", "Settings", "Credits" };

// ------------------------------------------------------------------ layouts

bool mm_layout_load(MmLayout *l, const char *name) {
    dg_strcpy(l->file, sizeof(l->file), name);
    char path[512];
    snprintf(path, sizeof(path), "%sassets/ui/screens/%s.json", platform_base_dir(), name);
    char *text = platform_read_file(path, NULL);
    l->loaded = false;
    l->count = 0;
    if (!text) { log_error("ui: %s missing", path); return false; }
    char err[160];
    l->loaded = gui_layout_parse(text, l->nodes, MM_LAYOUT_NODES, &l->count, err, sizeof(err));
    free(text);
    if (!l->loaded) log_error("ui: %s.json: %s", name, err);
    return l->loaded;
}

GuiRect mm_layout_solve(MmLayout *l, GuiRect r) {
    if (!l->loaded || !l->count) return r;
    LayoutParams lp = gui_layout_params();
    bool safe = l->nodes[0].safe;
    if (!safe) { lp.w = r.w; lp.h = r.h; lp.safe_frac = 0; lp.max_aspect = 0; }
    gui_layout_solve(l->nodes, l->count, &lp);
    if (!safe)
        for (int i = 0; i < l->count; i++) { l->nodes[i].rect.x += r.x; l->nodes[i].rect.y += r.y; }
    if (gui_debug())
        for (int i = 0; i < l->count; i++) gui_debug_rect(l->nodes[i].rect, UI_RGBA(255, 120, 0, 160), l->nodes[i].id);
    return l->nodes[0].rect;
}

GuiRect mm_node(const MmLayout *l, const char *id) {
    int i = gui_layout_find(l->nodes, l->count, id);
    return i >= 0 ? l->nodes[i].rect : gui_rect(0, 0, 0, 0);
}

const LayoutNode *mm_node_info(const MmLayout *l, const char *id) {
    int i = gui_layout_find(l->nodes, l->count, id);
    return i >= 0 ? &l->nodes[i] : NULL;
}

static void load_layouts(void) {
    mm_layout_load(&M->shell, "mainmenu");
    mm_layout_load(&M->play, "mainmenu_play");
    mm_layout_load(&M->browser, "mainmenu_browser");
    mm_layout_load(&M->player, "mainmenu_player");
    mm_layout_load(&M->cosmetics, "mainmenu_cosmetics");
    mm_layout_load(&M->maps, "mainmenu_maps");
    mm_layout_load(&M->settings, "mainmenu_settings");
    mm_layout_load(&M->credits, "mainmenu_credits");
    mm_layout_load(&M->lobby, "mainmenu_lobby");
    mm_layout_load(&M->loading, "mainmenu_loading");
    mm_layout_load(&M->chat, "mainmenu_chat");
}

// ------------------------------------------------------------------ shared drawing

#define MAX_TEX 32
static struct { char path[96]; TextureId tex; bool tried; } g_tex[MAX_TEX];

TextureId mm_texture(const char *path) {
    if (!path || !path[0]) return 0;
    for (int i = 0; i < MAX_TEX; i++) if (g_tex[i].tried && !strcmp(g_tex[i].path, path)) return g_tex[i].tex;
    for (int i = 0; i < MAX_TEX; i++) {
        if (g_tex[i].tried) continue;
        char buf[512];
        g_tex[i].tried = true;
        dg_strcpy(g_tex[i].path, sizeof(g_tex[i].path), path);
        const char *full = platform_data_path(path, buf, sizeof(buf));
        g_tex[i].tex = platform_file_exists(full) ? texture_load_file(full, true) : 0;
        return g_tex[i].tex;
    }
    return 0;
}

void mm_panel(GuiRect r) {
    gw_panel(r, true);
    gui_block_rect(r);
}

void mm_title(GuiRect r, const char *text) {
    GuiText t = { gui_s(TH->t_label + 1), GW_SEMIBOLD, TH->text_faint, GUI_LEFT, TH->tracking_caps * 1.6f, true, true, 0 };
    gd_text(&t, r.x, r.y + r.h - gd_line_height(t.size) - gui_s(TH->gap * 0.75f), text);
}

void mm_kv(GuiCursor *c, const char *key, const char *value) {
    GuiRect r = gc_row(c, TOK("mm_row_h"));
    gw_label(r, key, gui_s(TH->t_body - 1), GW_REGULAR, TH->text_dim, GUI_LEFT);
    gw_label(r, value, gui_s(TH->t_body - 1), GW_MEDIUM, TH->text, GUI_RIGHT);
    gd_rect(gui_rect(r.x, r.y + r.h - 1, r.w, 1), TH->panel_header);
}

void mm_thumb(GuiRect r, const char *path) {
    TextureId t = mm_texture(path);
    float rad = gui_s(TH->radius);
    if (t) gd_image(r, t, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, 255));
    else gd_round(r, rad, TH->slot_bg);
    gd_round_outline(r, rad, 1.0f, TH->panel_border);
}

// Key hints: centred in the footer (between the status and the patch notes).
void mm_footer_hint(GuiRect r, const char *text) {
    (void)r;
    GuiRect f = mm_node(&M->shell, "footer_mid");
    GuiText t = { gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_CENTER, 0, false, true, f.w };
    gd_text(&t, f.x + f.w * 0.5f, f.y + (f.h - gd_line_height(t.size)) * 0.5f, text);
}

u32 mm_name_color(int member_id) { return TH->chat_name[(member_id < 0 ? 0 : member_id) % 8]; }

// ------------------------------------------------------------------ footer

static void draw_status(GuiRect r) {
    float av = TOK("mm_avatar");
    GuiRect a = { r.x, r.y + (r.h - av) * 0.5f, av, av };
    bool steam = lobby_steam_running();
    const char *name = lobby_local_name();
    // Steam avatar (Steam downloads it on demand: retried every 2 s until it arrives).
    if (steam && !M->avatar && gui_time() - M->avatar_try > 2.0) {
        M->avatar_try = gui_time();
        static u8 px[64 * 64 * 4];
        int w = 0, h = 0;
        if (lobby_avatar_rgba(lobby_local_steam_id(), &w, &h, px, (int)sizeof(px))) M->avatar = render_texture_create(w, h, px, true, false);
    }
    gw_avatar(a, M->avatar, name, steam ? TH->info : TH->type_misc);
    float x = a.x + av + TOK("mm_gap") * 1.5f;
    GuiText nt = { gui_s(TH->t_body), GW_SEMIBOLD, TH->text, GUI_LEFT, 0, false, true, r.w - av - TOK("mm_gap") * 2 };
    float lh = gd_line_height(nt.size);
    float y = r.y + r.h * 0.5f - lh;
    gd_text(&nt, x, y, name);
    char line[160];
    u32 dot;
    const char *status;
#ifdef DG_HAVE_STEAM
    status = steam ? "Online · Steam" : "Offline · Steam not running · LAN only";
#else
    status = "LAN build (no Steam)";
#endif
    dot = steam ? TH->ok : TH->text_faint;
    float ping = lobby_relay_ping_ms();
    if (steam && ping >= 0) snprintf(line, sizeof(line), "%s  ·  relay %.0f ms  ·  v%s", status, ping, DG_VERSION);
    else snprintf(line, sizeof(line), "%s  ·  v%s", status, DG_VERSION);
    float dy = y + lh + gui_s(TH->gap * 0.5f);
    float dr = gui_s(TH->radius);
    gd_circle(x + dr, dy + gd_line_height(gui_s(TH->t_small)) * 0.5f, dr, dot);
    GuiText st = { gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, true, r.w - av - dr * 4 };
    gd_text(&st, x + dr * 3, dy, line);
}

static void draw_news(GuiRect r) {
    static char news[1024];
    static bool loaded;
    if (!loaded) {
        loaded = true;
        char buf[512];
        char *t = platform_read_file(platform_data_path("assets/ui/news.txt", buf, sizeof(buf)), NULL);
        dg_strcpy(news, sizeof(news), t ? t : "");
        free(t);
    }
    if (!news[0]) return;
    GuiText h = { gui_s(TH->t_label), GW_SEMIBOLD, TH->text_faint, GUI_RIGHT, TH->tracking_caps * 1.4f, true, true, 0 };
    gd_text(&h, r.x + r.w, r.y, "Patch notes");
    GuiText b = { gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, true, 0 };
    float y = r.y + gd_line_height(h.size) + gui_s(TH->gap * 0.5f);
    gd_clip_push(gui_rect(r.x, y, r.w, r.y + r.h - y));
    gd_text_wrap(&b, r.x, y, r.w, news, true);
    gd_clip_pop();
}

// ------------------------------------------------------------------ splash / notices

static bool draw_splash(void) {
    float t = (float)(gui_time() - M->boot_time);
    float dur = gui_tok("mm_splash_time");
    if (dur <= 0) dur = 1.6f;
    bool skip = gui_mouse_pressed(0) || gui_nav(NAV_ACCEPT) || gui_nav(NAV_BACK) || gui_nav(NAV_MENU);
    if (skip || t > dur + 0.4f) {
        M->splash = false;
        gui_nav_consume(NAV_ACCEPT);
        gui_nav_consume(NAV_BACK);
        gui_nav_consume(NAV_MENU);
        gui_anim_restart(gui_id("mm_open"));
        return false;
    }
    float fade = t > dur ? 1.0f - (t - dur) / 0.4f : 1.0f;
    gd_set_layer(GL_TOP);
    gd_rect(gui_rect(0, 0, gui_w(), gui_h()), gui_rgba_mul(UI_RGBA(6, 7, 8, 255), fade));
    float a = saturate(t / 0.5f) * fade;
    gd_alpha_push(a);
    GuiText logo = { TOK("mm_splash_logo"), GW_SEMIBOLD, TH->text, GUI_CENTER, 0.18f, true, true, 0 };
    float y = gui_h() * 0.5f - gd_line_height(logo.size) * 0.6f;
    float w = gd_text(&logo, gui_w() * 0.5f, y, "Deadground");
    float bw = w * 0.12f * gui_ease_out(saturate(t / 0.9f));
    gd_rect(gui_rect(gui_w() * 0.5f - bw * 0.5f, y + gd_line_height(logo.size) + TOK("mm_gap"), bw, gui_s(TH->accent_bar)), TH->accent);
    gd_alpha_pop();
    gd_set_layer(GL_BASE);
    return true;
}

static void draw_notice(GuiRect content) {
    if (!M->notice[0]) return;
    float age = (float)(gui_time() - M->notice_time);
    if (age > 8.0f) { M->notice[0] = 0; return; }
    float a = saturate(age / TH->modal_in) * saturate((8.0f - age) / 0.5f);
    GuiText t = { gui_s(TH->t_body), GW_MEDIUM, TH->text, GUI_LEFT, 0, false, false, 0 };
    float w = fminf(content.w * 0.6f, gd_text_width(&t, M->notice) + TOK("mm_gap_lg") * 2);
    GuiRect r = { content.x + (content.w - w) * 0.5f, content.y, w, TOK("mm_button_h") + TOK("mm_gap") };
    gd_alpha_push(a);
    gw_panel(r, true);
    gd_rect(gui_rect(r.x, r.y, gui_s(TH->accent_bar), r.h), TH->warning);
    t.max_w = r.w - TOK("mm_gap_lg") * 2;
    gd_text(&t, r.x + TOK("mm_gap_lg"), r.y + (r.h - gd_line_height(t.size)) * 0.5f, M->notice);
    gd_alpha_pop();
}

// A closed lobby (kicked, rejected, host left): the reason and OK.
static bool draw_closed(void) {
    if (lobby_state() != LS_CLOSED) return false;
    GuiRect r = screens_dialog_rect();
    gd_rect(gui_rect(0, 0, gui_w(), gui_h()), TH->scrim);
    mm_panel(r);
    float pad = gui_s(TH->pad + 4);
    GuiText t = { gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT, 0, false, false, r.w - pad * 2 };
    gd_text(&t, r.x + pad, r.y + pad, "Session ended");
    GuiText b = { gui_s(TH->t_body), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, false, 0 };
    gd_text_wrap(&b, r.x + pad, r.y + pad + TOK("mm_title_h"), r.w - pad * 2, lobby_closed_reason()[0] ? lobby_closed_reason() : "The lobby closed.", true);
    float bw = TOK("lb_button_w"), bh = gui_s(TH->button_h + 4);
    GuiRect ok = { r.x + r.w - pad - bw, r.y + r.h - pad - bh, bw, bh };
    if (!gui_focus()) gui_set_focus(gui_id("mm_closed_ok"));
    gui_focus_scope(r);
    if (gw_button(gui_id("mm_closed_ok"), ok, "OK", GB_PRIMARY, true) || gui_nav(NAV_BACK)) {
        gui_nav_consume(NAV_BACK);
        lobby_ack_closed();
    }
    return true;
}

// ------------------------------------------------------------------ tabs

static bool tab_allowed(int tab, const char **why) {
    LobbyState st = lobby_state();
    bool in_lobby = st == LS_CREATING || st == LS_JOINING || st == LS_LOBBY || st == LS_COUNTDOWN || st == LS_MIGRATING;
    if (in_lobby && tab == MT_MAPS) { if (why) *why = "In a lobby: the host picks the map in the lobby"; return false; }
    if (in_lobby && tab == MT_CREDITS) { if (why) *why = "Leave the lobby first"; return false; }
    if (why) *why = NULL;
    return true;
}

static void set_tab(int tab) {
    if (tab == M->tab || !tab_allowed(tab, NULL)) return;
    M->tab = tab;
    M->play_page = PP_MAIN;
    gui_set_focus(0);
    gui_anim_restart(gui_id("mm_tab_in"));
    M->env_dirty = true;   // the Player tab's studio light <-> the map's look
}

static void draw_tabbar(GuiRect bar) {
    GuiRect tabs = mm_node(&M->shell, "tabs"), power = mm_node(&M->shell, "power");
    const char *labels[MT_COUNT];
    bool en[MT_COUNT];
    const char *why[MT_COUNT];
    bool in_lobby = lobby_active();
    for (int i = 0; i < MT_COUNT; i++) {
        labels[i] = k_tab_names[i];
        en[i] = tab_allowed(i, &why[i]);
    }
    if (in_lobby) labels[MT_PLAY] = "Lobby";
    int t = gw_tabbar(gui_id("mm_tabbar"), tabs, labels, en, why, MT_COUNT, M->tab, false);
    if (t != M->tab) set_tab(t);
    // Power: quit to desktop (confirm). Centred vertically in its column.
    float ps = TOK("mm_power");
    GuiRect pr = { power.x + power.w - ps, bar.y + (bar.h - ps) * 0.5f, ps, ps };
    if (gw_power(gui_id("mm_power"), pr, "Quit to desktop")) screens_confirm(CONFIRM_QUIT, "Quit to desktop?", in_lobby ? "You will leave the lobby." : "Your settings and profile are saved.", 0);
}

static void tab_keys(void) {
    // 1-6, Q / E, LB / RB (the settings tab's own LB / RB go first: it consumes them).
    for (int i = 0; i < MT_COUNT; i++) if (gui_key_pressed(SDL_SCANCODE_1 + i)) { set_tab(i); gui_sound(TH->snd_tab); }
    int dir = gui_nav(NAV_TAB_NEXT) ? 1 : (gui_nav(NAV_TAB_PREV) ? -1 : 0);
    if (!dir) return;
    gui_nav_consume(NAV_TAB_NEXT);
    gui_nav_consume(NAV_TAB_PREV);
    int t = M->tab;
    for (int k = 0; k < MT_COUNT; k++) {
        t = (t + dir + MT_COUNT) % MT_COUNT;
        if (tab_allowed(t, NULL)) break;
    }
    set_tab(t);
    gui_sound(TH->snd_tab);
}

bool mainmenu_draw(bool top) {
    if (!M->active) return false;
    if (M->env_dirty) mm_scene_apply_environment();
    M->preview_rect = gui_rect(0, 0, 0, 0);
    gui_push_input(top && !M->splash);
    LobbyState st = lobby_state();
    if (st == LS_LOADING) {
        mm_loading_draw();
        gui_pop_input();
        if (M->splash) draw_splash();
        return false;
    }
    // Readability: darken the left and the bottom like the pause menu.
    float a = gui_ease_out(gui_anim_t(gui_id("mm_open")) / fmaxf(0.01f, TH->menu_in * 2));
    gd_rect_hgrad(gui_rect(0, 0, gui_w() * 0.6f, gui_h()), gui_rgba_mul(UI_RGBA(0, 0, 0, 190), a), UI_RGBA(0, 0, 0, 0));
    gd_rect_grad(gui_rect(0, gui_h() * 0.55f, gui_w(), gui_h() * 0.45f), UI_RGBA(0, 0, 0, 0), gui_rgba_mul(UI_RGBA(0, 0, 0, 170), a));
    gd_rect_grad(gui_rect(0, 0, gui_w(), gui_h() * 0.16f), gui_rgba_mul(UI_RGBA(0, 0, 0, 140), a), UI_RGBA(0, 0, 0, 0));
    mm_layout_solve(&M->shell, gui_rect(0, 0, gui_w(), gui_h()));
    GuiRect bar = mm_node(&M->shell, "tabbar"), content = mm_node(&M->shell, "content");
    if (!tab_allowed(M->tab, NULL)) M->tab = MT_PLAY;
    bool closed = st == LS_CLOSED;
    gui_push_input(!closed);
    gd_alpha_push(a);
    float tin = gui_ease_out(gui_anim_t(gui_id("mm_tab_in")) / TH->modal_in);
    gd_alpha_push(tin);
    switch (M->tab) {
    case MT_PLAY:
        if (lobby_active()) mm_lobby_draw(content);
        else mm_play_draw(content);
        break;
    case MT_PLAYER: mm_player_draw(content); break;
    case MT_COSMETICS: mm_cosmetics_draw(content); break;
    case MT_MAPS: mm_maps_draw(content); break;
    case MT_SETTINGS: {
        mm_layout_solve(&M->settings, content);
        settings_draw(mm_node(&M->settings, "settings"));
        break;
    }
    case MT_CREDITS: mm_credits_draw(content); break;
    default: break;
    }
    gd_alpha_pop();
    draw_tabbar(bar);   // after the content: the keyboard / pad focus starts in the page, not on the power icon
    draw_status(mm_node(&M->shell, "status"));
    draw_news(mm_node(&M->shell, "news"));
    draw_notice(content);
    gd_alpha_pop();
    if (top && !M->splash && !closed) {
        tab_keys();
        // Back: browser -> Play, another tab -> Play, Play -> the quit prompt (not while in a lobby:
        // the lobby screen's Leave asks).
        if (gui_nav(NAV_BACK)) {
            gui_nav_consume(NAV_BACK);
            if (M->tab == MT_PLAY && M->play_page == PP_BROWSER) M->play_page = PP_MAIN;
            else if (M->tab != MT_PLAY) set_tab(MT_PLAY);
            else if (!lobby_active()) screens_confirm(CONFIRM_QUIT, "Quit to desktop?", "Your settings and profile are saved.", 0);
        }
    }
    gui_pop_input();
    if (closed) { gui_push_input(top); draw_closed(); gui_pop_input(); }
    gui_pop_input();
    if (M->splash) draw_splash();
    return false;
}

void mainmenu_tick(float dt) {
    if (!M->active) return;
    mm_play_tick(dt);
}

// ------------------------------------------------------------------ lifecycle

bool mainmenu_enabled(void) { return M->enabled; }
bool mainmenu_active(void) { return M->active && screens_has(SCR_MAINMENU); }
const char *mainmenu_selected_map(void) { return M->map; }

void mainmenu_lobby_settings_changed(void) {
    // The lobby's map / season / time drive the background (everyone sees the host's choice).
    const Lobby *L = lobby_get();
    if (!lobby_active()) return;
    bool diff = strcmp(M->map, L->settings.map) || M->season != L->settings.season || M->hour != L->settings.start_hour || strcmp(M->weather, L->settings.weather);
    if (!diff) return;
    dg_strcpy(M->map, sizeof(M->map), L->settings.map);
    M->season = L->settings.season;
    M->hour = L->settings.start_hour;
    dg_strcpy(M->weather, sizeof(M->weather), L->settings.weather);
    M->dynamic_weather = L->settings.dynamic_weather;
    M->env_dirty = true;
}

void mainmenu_enter(const char *message) {
    if (!M->inited) return;
    // The background world: a local dev room nobody can join (the lobby hosts from it when asked).
    Game *g = game();
    bool have_bg = g && g->world && g->mode == GM_HOST && !strcmp(g->map, "devroom") && net_role() != NET_CLIENT && lobby_state() != LS_IN_GAME;
    menus_close_all();
    if (!have_bg) game_host_ex("devroom", GH_NO_AUTOHOST);
    mm_scene_enter();
    screens_push(SCR_MAINMENU);
    gui_anim_restart(gui_id("mm_open"));
    gui_anim_restart(gui_id("mm_tab_in"));
    M->active = true;
    M->tab = MT_PLAY;
    M->play_page = PP_MAIN;
    if (message && message[0]) {
        dg_strcpy(M->notice, sizeof(M->notice), message);
        M->notice_time = gui_time();
    }
    M->env_dirty = true;
    // Menu music: assets/sounds/menu.(wav|ogg) when it exists, else the ambience.
    char buf[512];
    bool has_menu = platform_file_exists(platform_data_path("assets/sounds/menu.wav", buf, sizeof(buf))) ||
                    platform_file_exists(platform_data_path("assets/sounds/menu.ogg", buf, sizeof(buf)));
    audio_play_music(has_menu ? "menu" : "ambient");
}

void mainmenu_session_started(void) {
    M->active = false;
    mainmenu_leave_scene();
    menus_close_all();
    audio_play_music("ambient");
}

void mainmenu_boot(void) {
    M->splash = true;
    M->boot_time = gui_time();
    mainmenu_enter(NULL);
}

static void cmd_mainmenu(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (lobby_active()) lobby_leave("main menu");
    M->enabled = true;
    mainmenu_enter(NULL);
}

static void cmd_mm_tab(int argc, const char **argv) {
    if (argc < 2) { log_info("mm_tab <play|player|cosmetics|maps|settings|credits|browser>"); return; }
    if (!strcmp(argv[1], "browser")) { set_tab(MT_PLAY); M->play_page = PP_BROWSER; return; }
    for (int i = 0; i < MT_COUNT; i++) if (!SDL_strcasecmp(argv[1], k_tab_names[i])) set_tab(i);
}

static void cmd_mm_map(int argc, const char **argv) {
    if (argc < 2) { log_info("mm_map <map id> [season] [hour] [weather]: select (Maps tab)"); return; }
    mm_select_map(argv[1]);
    if (argc > 2) { int s = season_from_id(argv[2]); if (s >= 0) M->season = s; }
    if (argc > 3) M->hour = DG_CLAMP(atoi(argv[3]), 0, 23);
    if (argc > 4) dg_strcpy(M->weather, sizeof(M->weather), strcmp(argv[4], "random") ? argv[4] : "");
    M->env_dirty = true;
}

// mm_dump: the menu's state as one log line (scripted tests grep it).
static void cmd_mm_dump(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    LobbyState st = lobby_state();
    log_info("mm_dump: active %d tab %s page %s lobby %s splash %d map %s season %d hour %d weather '%s' top %s preview %.0fx%.0f",
             mainmenu_active() ? 1 : 0, k_tab_names[M->tab], M->play_page == PP_BROWSER ? "browser" : "main", lobby_state_name(st), M->splash ? 1 : 0,
             M->map, M->season, M->hour, M->weather, screen_name(screens_top()), M->preview_rect.w, M->preview_rect.h);
}

void mainmenu_init(void) {
    memset(M, 0, sizeof(*M));
    M->show_dev_maps = true;   // every map and scene is listed (the toggle hides the test ones)
    M->inited = true;
    // The flow is on for players; --test runs (scripts, screenshots) boot straight into the dev room
    // unless they ask for it (--menu); --nomenu skips splash + menu for a normal run too (straight into
    // +map / +scene, or the boot map), with the player's config.
    bool test = false, menu = false, nomenu = false;
    for (int i = 1; i < g_engine.argc; i++) {
        if (!strcmp(g_engine.argv[i], "--test")) test = true;
        if (!strcmp(g_engine.argv[i], "--menu")) menu = true;
        if (!strcmp(g_engine.argv[i], "--nomenu")) nomenu = true;
    }
    M->enabled = menu || (!test && !nomenu);
    M->orbit_yaw = 0.35f;
    M->orbit_pitch = -0.05f;
    M->orbit_zoom = 1.0f;
    M->preview_tod = true;
    M->cos_selected = -1;
    M->cos_slot = 1;
    M->dynamic_weather = true;
    load_layouts();
    mm_scene_register();
    const Profile *p = lobby_profile();
    const char *map = p->last_map[0] ? p->last_map : "devroom";
    mm_select_map(lobby_map_info(map) ? map : (lobby_maps()->count ? lobby_maps()->maps[0].id : "devroom"));
    M->season = p->lobby.season;
    M->hour = p->lobby.start_hour;
    dg_strcpy(M->weather, sizeof(M->weather), p->lobby.weather);
    M->dynamic_weather = p->lobby.dynamic_weather;
    cmd_register("mainmenu", cmd_mainmenu, "open the main menu (leaves a lobby / game)");
    cmd_register("mm_tab", cmd_mm_tab, "mm_tab <play|player|cosmetics|maps|settings|credits|browser>: main menu tab");
    cmd_register("mm_map", cmd_mm_map, "mm_map <id> [season] [hour] [weather|random]: select a map in the main menu");
    cmd_register("mm_dump", cmd_mm_dump, "log the main menu state (scripted tests)");
}

void mainmenu_reload_layouts(void) { load_layouts(); }
