#include "menus.h"
#include "crosshair.h"
#include "ui.h"
#include "console.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "input/input.h"
#include "net/net.h"
#include "app/video.h"
#include "nuklear_config.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum MenuPage { PAGE_NONE, PAGE_PAUSE, PAGE_SETTINGS, PAGE_MULTIPLAYER } MenuPage;
typedef enum SettingsTab { TAB_GRAPHICS, TAB_CONTROLS, TAB_CROSSHAIR, TAB_AUDIO } SettingsTab;

typedef struct GraphicsSettings {
    int width, height, mode, vsync, shadow, texq, aniso, aa, maxfps;
    float fov, scale, brightness, gamma;
} GraphicsSettings;

#define MAX_RES 48

typedef struct Menus {
    MenuPage page;
    SettingsTab tab;
    GraphicsSettings pending, applied_backup;
    bool pending_valid;
    float confirm_timer;     // > 0 while waiting for "keep settings?"
    int res_w[MAX_RES], res_h[MAX_RES];
    int res_count;
    char connect_addr[64];
    int connect_len;
    int bind_scroll;
} Menus;

#define M (g_engine.menus)

static Cvar *cv(const char *n) { return cvar_find(n); }

static void collect_resolutions(void) {
    M->res_count = 0;
    SDL_DisplayID d = SDL_GetDisplayForWindow(g_engine.window);
    int count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d, &count);
    for (int i = 0; i < count && M->res_count < MAX_RES; i++) {
        bool dup = false;
        for (int k = 0; k < M->res_count; k++) if (M->res_w[k] == modes[i]->w && M->res_h[k] == modes[i]->h) dup = true;
        if (dup) continue;
        M->res_w[M->res_count] = modes[i]->w;
        M->res_h[M->res_count] = modes[i]->h;
        M->res_count++;
    }
    SDL_free(modes);
    static const int fallback[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 } };
    if (!M->res_count)
        for (int i = 0; i < 4; i++) { M->res_w[i] = fallback[i][0]; M->res_h[i] = fallback[i][1]; M->res_count++; }
}

static GraphicsSettings read_graphics(void) {
    GraphicsSettings g;
    g.width = cv("r_width")->i;
    g.height = cv("r_height")->i;
    g.mode = cv("r_mode")->i;
    g.vsync = cv("r_vsync")->i;
    g.shadow = cv("r_shadow_quality")->i;
    g.texq = cv("r_texture_quality")->i;
    g.aniso = cv("r_aniso")->i;
    g.aa = cv("r_aa")->i;
    g.maxfps = cv("r_maxfps")->i;
    g.fov = cv("cl_fov")->f;
    g.scale = cv("r_render_scale")->f;
    g.brightness = cv("r_brightness")->f;
    g.gamma = cv("r_gamma")->f;
    return g;
}

static void write_graphics(const GraphicsSettings *g) {
    cvar_set_int(cv("r_width"), g->width);
    cvar_set_int(cv("r_height"), g->height);
    cvar_set_int(cv("r_mode"), g->mode);
    cvar_set_int(cv("r_vsync"), g->vsync);
    cvar_set_int(cv("r_shadow_quality"), g->shadow);
    cvar_set_int(cv("r_texture_quality"), g->texq);
    cvar_set_int(cv("r_aniso"), g->aniso);
    cvar_set_int(cv("r_aa"), g->aa);
    cvar_set_int(cv("r_maxfps"), g->maxfps);
    cvar_set_float(cv("cl_fov"), g->fov);
    cvar_set_float(cv("r_render_scale"), g->scale);
    cvar_set_float(cv("r_brightness"), g->brightness);
    cvar_set_float(cv("r_gamma"), g->gamma);
    video_apply();
}

static void update_mouse_mode(void);

static void cmd_menu(int argc, const char **argv) {
    const char *page = argc > 1 ? argv[1] : "pause";
    Game *g = game();
    if (!strcmp(page, "close")) { menus_close_all(); return; }
    M->page = PAGE_PAUSE;
    if (!strcmp(page, "settings")) { M->page = PAGE_SETTINGS; M->pending = read_graphics(); collect_resolutions(); }
    if (!strcmp(page, "controls")) { M->page = PAGE_SETTINGS; M->tab = TAB_CONTROLS; }
    if (!strcmp(page, "audio")) { M->page = PAGE_SETTINGS; M->tab = TAB_AUDIO; }
    if (!strcmp(page, "crosshair")) { M->page = PAGE_SETTINGS; M->tab = TAB_CROSSHAIR; }
    if (!strcmp(page, "multiplayer")) { M->page = PAGE_MULTIPLAYER; net_lan_refresh(); }
    if (g) g->menu_open = true;
    update_mouse_mode();
}

static void cmd_inventory(int argc, const char **argv) { menus_toggle_inventory(); }

void menus_init(void) {
    M = (Menus *)calloc(1, sizeof(Menus));
    cmd_register("menu", cmd_menu, "open a menu: pause | settings | controls | audio | multiplayer | close");
    cmd_register("inventory", cmd_inventory, "toggle the inventory screen");
    dg_strcpy(M->connect_addr, sizeof(M->connect_addr), "127.0.0.1:27015");
    M->connect_len = (int)strlen(M->connect_addr);
}

static void update_mouse_mode(void) {
    Game *g = game();
    bool free_cursor = menus_wants_mouse() || console_is_open();
    input_set_game_focus(!free_cursor && g_engine.has_focus);
    (void)g;
}

bool menus_wants_mouse(void) {
    Game *g = game();
    return (M && M->page != PAGE_NONE) || (g && (g->inventory_open || g->container_open));
}

void menus_close_all(void) {
    Game *g = game();
    M->page = PAGE_NONE;
    if (g) {
        g->menu_open = false;
        if (g->container_open) game_rpc(RPC_CONTAINER_CLOSE, 0, 0, 0);
        g->inventory_open = false;
        g->container_open = false;
    }
    config_save(video_config_path());
    update_mouse_mode();
}

void menus_toggle_pause(void) {
    Game *g = game();
    if (g && (g->inventory_open || g->container_open)) { menus_close_all(); return; }
    if (M->page != PAGE_NONE) {
        if (M->page == PAGE_PAUSE) menus_close_all();
        else M->page = PAGE_PAUSE;
    } else {
        M->page = PAGE_PAUSE;
    }
    if (g) g->menu_open = M->page != PAGE_NONE;
    update_mouse_mode();
}

void menus_toggle_inventory(void) {
    Game *g = game();
    if (!g || M->page != PAGE_NONE) return;
    if (g->container_open) { menus_close_all(); return; }
    g->inventory_open = !g->inventory_open;
    g->inv_selected = -1;
    update_mouse_mode();
}

void menus_tick(float dt) {
    if (M->confirm_timer > 0) {
        M->confirm_timer -= dt;
        if (M->confirm_timer <= 0) {
            write_graphics(&M->applied_backup);
            M->pending = M->applied_backup;
            log_info("video: settings reverted (not confirmed)");
        }
    }
    // Keep the cursor state consistent with open UI (e.g. container opened by an event).
    update_mouse_mode();
}

// ------------------------------------------------------------------ widgets

static float S(float v) { return v * ui_scale(); }

static void label_value(struct nk_context *ctx, const char *label, const char *fmt, float v) {
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, v);
    nk_layout_row_begin(ctx, NK_DYNAMIC, S(24), 2);
    nk_layout_row_push(ctx, 0.75f);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.25f);
    nk_label(ctx, buf, NK_TEXT_RIGHT);
    nk_layout_row_end(ctx);
}

static bool slider_f(struct nk_context *ctx, const char *label, float *v, float lo, float hi, float step, const char *fmt) {
    label_value(ctx, label, fmt, *v);
    nk_layout_row_dynamic(ctx, S(22), 1);
    float old = *v;
    nk_slider_float(ctx, lo, v, hi, step);
    return old != *v;
}

static void slider_cvar(struct nk_context *ctx, const char *label, const char *name, float lo, float hi, float step, const char *fmt) {
    Cvar *c = cv(name);
    if (!c) return;
    float v = c->f;
    if (slider_f(ctx, label, &v, lo, hi, step, fmt)) cvar_set_float(c, v);
}

static void check_cvar(struct nk_context *ctx, const char *label, const char *name) {
    Cvar *c = cv(name);
    if (!c) return;
    nk_layout_row_dynamic(ctx, S(24), 1);
    nk_bool v = c->i != 0;
    if (nk_checkbox_label(ctx, label, &v) != 0 || (v != 0) != (c->i != 0)) cvar_set_int(c, v ? 1 : 0);
}

static int combo_row(struct nk_context *ctx, const char *label, const char **items, int count, int selected) {
    nk_layout_row_begin(ctx, NK_DYNAMIC, S(26), 2);
    nk_layout_row_push(ctx, 0.45f);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.55f);
    int r = nk_combo(ctx, items, count, selected, (int)S(24), nk_vec2(S(240), S(260)));
    nk_layout_row_end(ctx);
    return r;
}

// ------------------------------------------------------------------ pages

static struct nk_rect centered(float w, float h) { return nk_rect((ui_width() - w) * 0.5f, (ui_height() - h) * 0.5f, w, h); }

static void draw_pause(struct nk_context *ctx) {
    Game *g = game();
    if (nk_begin(ctx, "PAUSED", centered(S(320), S(400)), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, S(40), 1);
        if (nk_button_label(ctx, "Resume")) { menus_close_all(); nk_end(ctx); return; }
        if (nk_button_label(ctx, "Settings")) { M->page = PAGE_SETTINGS; M->pending = read_graphics(); M->pending_valid = true; collect_resolutions(); }
        if (nk_button_label(ctx, "Multiplayer")) { M->page = PAGE_MULTIPLAYER; net_lan_refresh(); }
        bool steam = net_steam_available();
        if (!steam) nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, steam ? "Invite Friends (Steam)" : "Invite Friends (Steam unavailable)")) net_steam_invite();
        if (!steam) nk_widget_disable_end(ctx);
        bool can_disconnect = net_role() != NET_OFF;
        if (!can_disconnect) nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, g && g->mode == GM_CLIENT ? "Disconnect" : "Stop Hosting")) {
            menus_close_all();
            game_disconnect_to_local("left the session");
        }
        if (!can_disconnect) nk_widget_disable_end(ctx);
        if (nk_button_label(ctx, "Quit")) g_engine.quit = true;
    }
    nk_end(ctx);
}

static void draw_graphics(struct nk_context *ctx) {
    GraphicsSettings *p = &M->pending;
    // Resolution
    char labels[MAX_RES][24];
    const char *items[MAX_RES];
    int sel = 0;
    for (int i = 0; i < M->res_count; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%d x %d", M->res_w[i], M->res_h[i]);
        items[i] = labels[i];
        if (M->res_w[i] == p->width && M->res_h[i] == p->height) sel = i;
    }
    int r = combo_row(ctx, "Resolution", items, M->res_count, sel);
    p->width = M->res_w[r];
    p->height = M->res_h[r];
    static const char *modes[] = { "Windowed", "Borderless", "Fullscreen" };
    p->mode = combo_row(ctx, "Window mode", modes, 3, p->mode);
    nk_layout_row_dynamic(ctx, S(24), 1);
    nk_bool vs = p->vsync != 0;
    nk_checkbox_label(ctx, "VSync", &vs);
    p->vsync = vs ? 1 : 0;
    slider_f(ctx, "Field of view", &p->fov, 60, 110, 1, "%.0f");
    slider_f(ctx, "Render scale", &p->scale, 0.5f, 2.0f, 0.05f, "%.2fx");
    static const char *shadow[] = { "Off", "Low", "Medium", "High" };
    p->shadow = combo_row(ctx, "Shadow quality", shadow, 4, p->shadow);
    static const char *texq[] = { "Low", "Medium", "High" };
    p->texq = combo_row(ctx, "Texture quality", texq, 3, p->texq);
    static const char *aniso[] = { "Off", "2x", "4x", "8x", "16x" };
    static const int aniso_v[] = { 1, 2, 4, 8, 16 };
    int ai = 0;
    for (int i = 0; i < 5; i++) if (aniso_v[i] == p->aniso) ai = i;
    p->aniso = aniso_v[combo_row(ctx, "Anisotropic filtering", aniso, 5, ai)];
    static const char *aa[] = { "None", "FXAA", "TAA (stub: FXAA)" };
    p->aa = combo_row(ctx, "Anti-aliasing", aa, 3, p->aa);
    float fps = (float)p->maxfps;
    slider_f(ctx, "Max FPS (0 = unlimited)", &fps, 0, 300, 10, "%.0f");
    p->maxfps = (int)fps;
    slider_f(ctx, "Brightness", &p->brightness, 0.5f, 2.0f, 0.05f, "%.2f");
    slider_f(ctx, "Gamma", &p->gamma, 0.6f, 1.6f, 0.02f, "%.2f");

    nk_layout_row_dynamic(ctx, S(34), 2);
    if (nk_button_label(ctx, "Apply")) {
        GraphicsSettings cur = read_graphics();
        bool display_change = cur.width != p->width || cur.height != p->height || cur.mode != p->mode;
        M->applied_backup = cur;
        write_graphics(p);
        if (display_change) M->confirm_timer = 15.0f;
    }
    if (nk_button_label(ctx, "Revert")) M->pending = read_graphics();
}

static void draw_controls(struct nk_context *ctx) {
    slider_cvar(ctx, "Mouse sensitivity X", "cl_sensitivity", 0.1f, 10, 0.05f, "%.2f");
    slider_cvar(ctx, "Mouse sensitivity Y", "cl_sensitivity_y", 0.1f, 10, 0.05f, "%.2f");
    slider_cvar(ctx, "ADS sensitivity multiplier", "cl_ads_sensitivity", 0.1f, 1.5f, 0.05f, "%.2f");
    check_cvar(ctx, "Invert Y", "cl_invert_y");
    slider_cvar(ctx, "Gamepad sensitivity", "pad_sensitivity", 0.5f, 8, 0.1f, "%.1f");
    slider_cvar(ctx, "Gamepad deadzone", "pad_deadzone", 0, 0.5f, 0.01f, "%.2f");
    check_cvar(ctx, "Toggle crouch (instead of hold)", "cl_crouch_toggle");
    check_cvar(ctx, "Toggle sprint (instead of hold)", "cl_sprint_toggle");
    check_cvar(ctx, "Head bob", "cl_headbob");
    nk_layout_row_dynamic(ctx, S(26), 1);
    nk_label(ctx, input_capturing() ? "Press a key / button (Esc cancels)..." : "Key bindings (click to rebind):", NK_TEXT_LEFT);
    for (int a = 0; a < ACT_COUNT; a++) {
        nk_layout_row_begin(ctx, NK_DYNAMIC, S(24), 3);
        nk_layout_row_push(ctx, 0.44f);
        nk_label(ctx, action_label((Action)a), NK_TEXT_LEFT);
        for (int s = 0; s < BINDS_PER_ACTION; s++) {
            nk_layout_row_push(ctx, 0.28f);
            char name[48];
            binding_name(input_get_binding((Action)a, s), name, sizeof(name));
            if (nk_button_label(ctx, name)) input_begin_capture((Action)a, s);
        }
        nk_layout_row_end(ctx);
    }
    nk_layout_row_dynamic(ctx, S(30), 1);
    if (nk_button_label(ctx, "Reset bindings to defaults")) input_reset_bindings();
}

static void draw_crosshair_tab(struct nk_context *ctx) {
    static const char *styles[] = { "Cross", "Cross + dot", "Dot", "Circle" };
    Cvar *st = cv("crosshair_style");
    if (st) {
        int sel = combo_row(ctx, "Style", styles, 4, DG_CLAMP(st->i, 0, 3));
        if (sel != st->i) cvar_set_int(st, sel);
    }
    Cvar *col = cv("crosshair_color");
    if (col) {
        const char *names[8];
        int n = DG_MIN(crosshair_color_count(), 8);
        for (int i = 0; i < n; i++) names[i] = crosshair_color_name(i);
        int sel = combo_row(ctx, "Color", names, n, DG_CLAMP(col->i, 0, n - 1));
        if (sel != col->i) cvar_set_int(col, sel);
    }
    slider_cvar(ctx, "Line length", "crosshair_length", 1, 40, 0.5f, "%.1f");
    slider_cvar(ctx, "Line thickness", "crosshair_thickness", 1, 8, 0.5f, "%.1f");
    slider_cvar(ctx, "Extra gap", "crosshair_gap", 0, 30, 0.5f, "%.1f");
    slider_cvar(ctx, "Opacity", "crosshair_opacity", 0.1f, 1, 0.05f, "%.2f");
    check_cvar(ctx, "Outline", "crosshair_outline");
    check_cvar(ctx, "Tint over enemies / friendlies", "crosshair_target_color");
    check_cvar(ctx, "Hit markers", "crosshair_hitmarker");
    check_cvar(ctx, "Follow camera kick (pin to screen center)", "crosshair_follow_kick");
    nk_layout_row_dynamic(ctx, S(10), 1);
    slider_cvar(ctx, "Weapon kick (visual only)", "wpn_visual_scale", 0, 2, 0.05f, "%.2f");
    nk_layout_row_dynamic(ctx, S(22), 1);
    nk_label(ctx, "The gap shows the exact bullet spread cone. Palette: assets/ui/crosshair.json", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, S(30), 1);
    if (nk_button_label(ctx, "Reload crosshair style")) crosshair_reload();
}

static void draw_audio(struct nk_context *ctx) {
    slider_cvar(ctx, "Master volume", "snd_master", 0, 1, 0.01f, "%.2f");
    slider_cvar(ctx, "Music volume", "snd_music", 0, 1, 0.01f, "%.2f");
    slider_cvar(ctx, "Effects volume", "snd_sfx", 0, 1, 0.01f, "%.2f");
    slider_cvar(ctx, "Voice volume", "snd_voice", 0, 1, 0.01f, "%.2f");
}

static void draw_settings(struct nk_context *ctx) {
    if (nk_begin(ctx, "SETTINGS", centered(S(620), S(640)), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, S(32), 5);
        if (nk_button_label(ctx, M->tab == TAB_GRAPHICS ? "> Graphics" : "Graphics")) M->tab = TAB_GRAPHICS;
        if (nk_button_label(ctx, M->tab == TAB_CONTROLS ? "> Controls" : "Controls")) M->tab = TAB_CONTROLS;
        if (nk_button_label(ctx, M->tab == TAB_CROSSHAIR ? "> Crosshair" : "Crosshair")) M->tab = TAB_CROSSHAIR;
        if (nk_button_label(ctx, M->tab == TAB_AUDIO ? "> Audio" : "Audio")) M->tab = TAB_AUDIO;
        if (nk_button_label(ctx, "Back")) { config_save(video_config_path()); M->page = PAGE_PAUSE; }
        switch (M->tab) {
        case TAB_GRAPHICS: draw_graphics(ctx); break;
        case TAB_CONTROLS: draw_controls(ctx); break;
        case TAB_CROSSHAIR: draw_crosshair_tab(ctx); break;
        case TAB_AUDIO: draw_audio(ctx); break;
        }
    }
    nk_end(ctx);
    if (M->confirm_timer > 0) {
        if (nk_begin(ctx, "Confirm display settings", centered(S(380), S(150)), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
            char buf[96];
            snprintf(buf, sizeof(buf), "Keep these display settings? Reverting in %d s", (int)ceilf(M->confirm_timer));
            nk_layout_row_dynamic(ctx, S(30), 1);
            nk_label(ctx, buf, NK_TEXT_CENTERED);
            nk_layout_row_dynamic(ctx, S(36), 2);
            if (nk_button_label(ctx, "Keep")) { M->confirm_timer = 0; config_save(video_config_path()); }
            if (nk_button_label(ctx, "Revert")) { M->confirm_timer = 0; write_graphics(&M->applied_backup); M->pending = M->applied_backup; }
        }
        nk_end(ctx);
    }
}

static void draw_multiplayer(struct nk_context *ctx) {
    if (nk_begin(ctx, "MULTIPLAYER", centered(S(560), S(520)), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        NetRole role = net_role();
        nk_layout_row_dynamic(ctx, S(24), 1);
        char status[128];
        snprintf(status, sizeof(status), "Status: %s (%s)", role == NET_HOST ? "hosting" : role == NET_CLIENT ? "connected" : "offline", net_transport_name());
        nk_label(ctx, status, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, S(34), 2);
        if (nk_button_label(ctx, role == NET_HOST ? "LAN hosting active" : "Host LAN game")) {
            if (role != NET_HOST) {
                if (game()->mode != GM_HOST) game_host(game()->g_map->str);
                net_host(TRANSPORT_LAN, (u16)cv("net_port")->i);
            }
        }
        bool steam = net_steam_available();
        if (!steam) nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, steam ? "Host Steam lobby" : "Steam unavailable (LAN build)")) {
            if (game()->mode != GM_HOST) game_host(game()->g_map->str);
            net_host(TRANSPORT_STEAM, 0);
        }
        if (!steam) nk_widget_disable_end(ctx);

        nk_layout_row_dynamic(ctx, S(24), 1);
        nk_label(ctx, "Direct connect (ip:port):", NK_TEXT_LEFT);
        nk_layout_row_begin(ctx, NK_DYNAMIC, S(30), 2);
        nk_layout_row_push(ctx, 0.7f);
        nk_edit_string(ctx, NK_EDIT_FIELD, M->connect_addr, &M->connect_len, (int)sizeof(M->connect_addr) - 1, nk_filter_ascii);
        nk_layout_row_push(ctx, 0.3f);
        if (nk_button_label(ctx, "Connect")) {
            M->connect_addr[M->connect_len] = 0;
            menus_close_all();
            net_connect(TRANSPORT_LAN, M->connect_addr);
        }
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, S(30), 2);
        nk_label(ctx, "LAN games:", NK_TEXT_LEFT);
        if (nk_button_label(ctx, "Refresh")) net_lan_refresh();
        const LanServer *servers;
        int n = net_lan_servers(&servers);
        if (!n) {
            nk_layout_row_dynamic(ctx, S(24), 1);
            nk_label_colored(ctx, "No LAN games found (broadcast on port 27016)", NK_TEXT_LEFT, nk_rgb(150, 150, 150));
        }
        for (int i = 0; i < n; i++) {
            char line[160];
            snprintf(line, sizeof(line), "%s  [%s]  %d/%d  %s  %.0f ms", servers[i].name, servers[i].map, servers[i].players,
                     servers[i].max_players, servers[i].address, servers[i].ping_ms);
            nk_layout_row_begin(ctx, NK_DYNAMIC, S(28), 2);
            nk_layout_row_push(ctx, 0.78f);
            nk_label(ctx, line, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 0.22f);
            if (nk_button_label(ctx, "Join")) {
                char addr[64];
                dg_strcpy(addr, sizeof(addr), servers[i].address);
                menus_close_all();
                net_connect(TRANSPORT_LAN, addr);
                nk_layout_row_end(ctx);
                break;
            }
            nk_layout_row_end(ctx);
        }
        nk_layout_row_dynamic(ctx, S(34), 1);
        if (nk_button_label(ctx, "Back")) M->page = PAGE_PAUSE;
    }
    nk_end(ctx);
}

// ------------------------------------------------------------------ inventory / container

static void slot_button(struct nk_context *ctx, World *w, const InvSlot *s, int id, bool selected, bool *left, bool *right) {
    char label[48];
    const ItemDef *d = item_def(w->items, s->item);
    if (!s->item) snprintf(label, sizeof(label), " ");
    else if (s->count > 1) snprintf(label, sizeof(label), "%.10s x%d", d->name, s->count);
    else if (item_weapon(w->items, s->item) && item_weapon(w->items, s->item)->mag_size) snprintf(label, sizeof(label), "%.10s [%d]", d->name, s->ammo);
    else snprintf(label, sizeof(label), "%.12s", d->name);
    struct nk_rect bounds = nk_widget_bounds(ctx);
    struct nk_style_button style = ctx->style.button;
    if (s->item) {
        struct nk_color c = nk_rgba((nk_byte)(d->color.x * 120), (nk_byte)(d->color.y * 120), (nk_byte)(d->color.z * 120), 255);
        style.normal = nk_style_item_color(c);
    }
    if (selected) style.border_color = nk_rgb(230, 140, 50), style.border = 3;
    *left = nk_button_label_styled(ctx, &style, label) != 0;
    *right = nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_RIGHT, bounds) != 0;
}

static void draw_item_info(struct nk_context *ctx, World *w, const InvSlot *s) {
    if (!s || !s->item) return;
    const ItemDef *d = item_def(w->items, s->item);
    nk_layout_row_dynamic(ctx, S(22), 1);
    nk_label_colored(ctx, d->name, NK_TEXT_LEFT, nk_rgb(240, 200, 140));
    nk_layout_row_dynamic(ctx, S(22), 1);
    nk_label(ctx, d->desc, NK_TEXT_LEFT);
}

static void draw_inventory(struct nk_context *ctx, bool with_container) {
    Game *g = game();
    World *w = g->world;
    Player *p = game_local_player();
    if (!p) return;
    float bw = S(92), bh = S(46);
    float win_w = bw * INV_BACKPACK_W + S(60);
    float x = with_container ? ui_width() * 0.5f - win_w - S(10) : (ui_width() - win_w) * 0.5f;
    if (nk_begin(ctx, "INVENTORY", nk_rect(x, ui_height() * 0.15f, win_w, S(560)), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, S(20), 1);
        nk_label_colored(ctx, with_container ? "Click: put into crate   Right-click: drop" : "Click two slots to move/swap   Right-click: drop", NK_TEXT_LEFT, nk_rgb(150, 150, 150));
        nk_layout_row_dynamic(ctx, S(20), 1);
        nk_label(ctx, "Backpack", NK_TEXT_LEFT);
        for (int row = 0; row < INV_BACKPACK_H; row++) {
            nk_layout_row_static(ctx, bh, (int)bw - 4, INV_BACKPACK_W);
            for (int c = 0; c < INV_BACKPACK_W; c++) {
                int si = INV_HOTBAR + row * INV_BACKPACK_W + c;
                bool l, r;
                slot_button(ctx, w, &p->inv.slots[si], si, g->inv_selected == si, &l, &r);
                if (r && p->inv.slots[si].item) game_rpc(RPC_INV_DROP, si, 0, 0);
                else if (l) {
                    if (with_container) { if (p->inv.slots[si].item) game_rpc(RPC_CONTAINER_PUT, si, 0, 0); }
                    else if (g->inv_selected < 0) { if (p->inv.slots[si].item) g->inv_selected = si; }
                    else { game_rpc(RPC_INV_MOVE, g->inv_selected, si, 0); g->inv_selected = -1; }
                }
            }
        }
        nk_layout_row_dynamic(ctx, S(20), 1);
        nk_label(ctx, "Hotbar (1-9)", NK_TEXT_LEFT);
        for (int row = 0; row < 2; row++) {
            nk_layout_row_static(ctx, bh, (int)bw - 4, row == 0 ? 5 : 4);
            for (int c = 0; c < (row == 0 ? 5 : 4); c++) {
                int si = row * 5 + c;
                bool l, r;
                slot_button(ctx, w, &p->inv.slots[si], si, g->inv_selected == si, &l, &r);
                if (r && p->inv.slots[si].item) game_rpc(RPC_INV_DROP, si, 0, 0);
                else if (l) {
                    if (with_container) { if (p->inv.slots[si].item) game_rpc(RPC_CONTAINER_PUT, si, 0, 0); }
                    else if (g->inv_selected < 0) { if (p->inv.slots[si].item) g->inv_selected = si; }
                    else { game_rpc(RPC_INV_MOVE, g->inv_selected, si, 0); g->inv_selected = -1; }
                }
            }
        }
        draw_item_info(ctx, w, g->inv_selected >= 0 ? &p->inv.slots[g->inv_selected] : &p->inv.slots[g->hotbar_slot]);
    }
    nk_end(ctx);

    if (with_container) {
        if (nk_begin(ctx, "CRATE", nk_rect(ui_width() * 0.5f + S(10), ui_height() * 0.15f, bw * 4 + S(50), S(360)), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
            nk_layout_row_dynamic(ctx, S(20), 1);
            nk_label_colored(ctx, "Click an item to take it", NK_TEXT_LEFT, nk_rgb(150, 150, 150));
            for (int row = 0; row < 3; row++) {
                nk_layout_row_static(ctx, bh, (int)bw - 4, 4);
                for (int c = 0; c < 4; c++) {
                    int si = row * 4 + c;
                    bool l, r;
                    slot_button(ctx, w, &g->container_slots[si], 500 + si, false, &l, &r);
                    if ((l || r) && g->container_slots[si].item) game_rpc(RPC_CONTAINER_TAKE, si, 0, 0);
                }
            }
            nk_layout_row_dynamic(ctx, S(34), 2);
            if (nk_button_label(ctx, "Take all")) game_rpc(RPC_CONTAINER_TAKE, 255, 0, 0);
            if (nk_button_label(ctx, "Close")) menus_close_all();
        }
        nk_end(ctx);
    }
}

void menus_draw(void) {
    struct nk_context *ctx = ui_nk();
    Game *g = game();
    if (g && (g->inventory_open || g->container_open) && M->page == PAGE_NONE) draw_inventory(ctx, g->container_open);
    switch (M->page) {
    case PAGE_PAUSE: draw_pause(ctx); break;
    case PAGE_SETTINGS: draw_settings(ctx); break;
    case PAGE_MULTIPLAYER: draw_multiplayer(ctx); break;
    default: break;
    }
}
