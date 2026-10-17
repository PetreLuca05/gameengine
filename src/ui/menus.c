// Screen stack (docs/UI.md "Screen map"):
//
//   in game ── Esc / Start ──> PAUSE (button stack bottom left, logo + version bottom right)
//     PAUSE ── Settings ─────> SETTINGS   (centred modal: tabs, scroll; the stack stays visible)
//     PAUSE ── Multiplayer ──> MULTIPLAYER
//     PAUSE ── Inventory ────> INVENTORY  (also Tab / I / pad Back straight from the game)
//     PAUSE ── Disconnect / Quit ──> CONFIRM
//     SETTINGS ── resolution / mode change ──> CONFIRM (keep? 15 s countdown, reverts)
//     INVENTORY ── drop outside (non-stackable) ──> CONFIRM
//     world: crate ──> INVENTORY + crate panel, weapon bench ──> BENCH
//   Esc / B pops the top screen; popping the last one resumes. Only the top screen gets input.
//
// Single player (or a host without clients) pauses the simulation; in multiplayer the menu opens
// without pausing. The HUD hides while a screen is open.
#include "menus.h"
#include "ui.h"
#include "console.h"
#include "icons.h"
#include "screens/screens.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "input/input.h"
#include "net/net.h"
#include "app/video.h"
#include "vfx_editor.h"
#include "holdpoint_editor.h"
#include "game/scene.h"
#include "mainmenu/mainmenu.h"
#include "net/lobby_net.h"
#include "pause/commands_tab.h"
#include "hud/hud_internal.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STACK 8

typedef struct Menus {
    int stack[MAX_STACK];
    int depth;
    LayoutNode nodes[GUI_LAYOUT_MAX_NODES];
    int node_count;
    double open_time;
    int confirm_kind, confirm_arg;
    char confirm_title[64], confirm_text[256];
    bool inited;
    GuiId pause_focus;      // the stack entry that opened the current sub-menu (focus on return)
} Menus;

#define M (g_engine.menus)
#define TH (gui_theme())

static const char *k_names[SCR_COUNT] = { "none", "pause", "settings", "multiplayer", "inventory", "bench", "confirm", "mainmenu", "chat", "lobby", "commands" };
const char *screen_name(int id) { return id >= 0 && id < SCR_COUNT ? k_names[id] : "?"; }

static bool load_layout(void) {
    char path[512];
    snprintf(path, sizeof(path), "%sassets/ui/screens/pause.json", platform_base_dir());
    size_t size = 0;
    char *text = platform_read_file(path, &size);
    if (!text) { log_error("ui: %s missing", path); return false; }
    char err[160];
    int n = 0;
    bool ok = gui_layout_parse(text, M->nodes, GUI_LAYOUT_MAX_NODES, &n, err, sizeof(err));
    free(text);
    if (!ok) { log_error("ui: pause.json: %s", err); return false; }
    M->node_count = n;
    return true;
}

static void update_mouse_mode(void) {
    bool free_cursor = menus_wants_mouse() || console_is_open();
    input_set_game_focus(!free_cursor && g_engine.has_focus);
}

int screens_top(void) { return M->depth ? M->stack[M->depth - 1] : SCR_NONE; }

bool screens_has(int id) {
    for (int i = 0; i < M->depth; i++) if (M->stack[i] == id) return true;
    return false;
}

static void on_close(int id) {
    Game *g = game();
    if (id == SCR_INVENTORY) {
        inventory_close();
        if (g && g->container_open) { game_rpc(RPC_CONTAINER_CLOSE, 0, 0, 0); g->container_open = false; }
        if (g) g->inventory_open = false;
    }
    if (id == SCR_BENCH && g) g->bench_open = false;
    if (id == SCR_SETTINGS) config_save(video_config_path());
}

void screens_push(int id) {
    if (M->depth >= MAX_STACK || screens_top() == id) return;
    M->stack[M->depth++] = id;
    gui_anim_restart(gui_idi("screen", id));
    if (id == SCR_PAUSE) gui_anim_restart(gui_id("pause_open"));
    if (id == SCR_INVENTORY) { inventory_open(); if (game()) game()->inventory_open = true; }
    gui_set_focus(0);
    gui_sound(TH->snd_open);
    update_mouse_mode();
}

void screens_pop(void) {
    if (!M->depth) return;
    int id = M->stack[--M->depth];
    on_close(id);
    gui_set_focus(screens_top() == SCR_PAUSE ? M->pause_focus : 0);
    gui_sound(TH->snd_back);
    update_mouse_mode();
}

void menus_close_all(void) {
    while (M->depth) {
        int id = M->stack[--M->depth];
        on_close(id);
    }
    Game *g = game();
    if (g) {
        g->menu_open = false;
        if (g->container_open) game_rpc(RPC_CONTAINER_CLOSE, 0, 0, 0);
        g->inventory_open = g->container_open = g->bench_open = false;
    }
    config_save(video_config_path());
    update_mouse_mode();
}

void screens_confirm(int kind, const char *title, const char *text, int arg) {
    M->confirm_kind = kind;
    M->confirm_arg = arg;
    dg_strcpy(M->confirm_title, sizeof(M->confirm_title), title);
    dg_strcpy(M->confirm_text, sizeof(M->confirm_text), text);
    screens_push(SCR_CONFIRM);
}

static GuiRect node_rect(const char *id) {
    int i = gui_layout_find(M->nodes, M->node_count, id);
    return i >= 0 ? M->nodes[i].rect : gui_rect(gui_w() * 0.2f, gui_h() * 0.15f, gui_w() * 0.6f, gui_h() * 0.7f);
}

GuiRect screens_modal_rect(void) { return node_rect("modal"); }
GuiRect screens_dialog_rect(void) { return node_rect("dialog"); }

// ------------------------------------------------------------------ conditions / layout

static bool online(void) { return net_role() != NET_OFF; }

static bool cond_true(const char *c) {
    if (!c[0]) return true;
    bool neg = c[0] == '!';
    if (neg) c++;
    bool v = true;
    if (!strcmp(c, "steam")) v = net_steam_available();
    else if (!strcmp(c, "online")) v = online();
    else if (!strcmp(c, "world")) v = game_world() != NULL;
    else if (!strcmp(c, "lobby")) v = lobby_active();                       // a lobby session (pause > Lobby)
    else if (!strcmp(c, "menu")) v = mainmenu_enabled();                    // the main menu flow (Leave to main menu)
    else if (!strcmp(c, "legacy_online")) v = online() && !mainmenu_enabled();   // console sessions without the menu
    else if (!strcmp(c, "commands")) v = commands_tab_available();          // developer / con_enable
    return neg ? !v : v;
}

static void solve_layout(void) {
    for (int i = 0; i < M->node_count; i++) M->nodes[i].hidden = !cond_true(M->nodes[i].cond);
    LayoutParams lp = gui_layout_params();
    gui_layout_solve(M->nodes, M->node_count, &lp);
    if (gui_debug())
        for (int i = 0; i < M->node_count; i++)
            if (!M->nodes[i].hidden) gui_debug_rect(M->nodes[i].rect, UI_RGBA(255, 120, 0, 160), M->nodes[i].id);
}

bool menus_can_pause(void) { return net_role() == NET_OFF || (net_role() == NET_HOST && net_host_client_count() == 0); }

// ------------------------------------------------------------------ shared drawing

GuiRect screens_modal_frame(GuiRect r, const char *title, const char *subtitle) {
    gw_panel(r, true);
    gui_block_rect(r);
    float pad = gui_s(TH->pad + 6);
    GuiText t = { gui_s(TH->t_title), GW_SEMIBOLD, TH->text, GUI_LEFT, 0.02f, true, false, 0 };
    gd_text(&t, r.x + pad, r.y + gui_s(18), title);
    if (subtitle && subtitle[0]) {
        GuiText s = { gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT, 0, false, false, r.w * 0.5f };
        gd_text(&s, r.x + r.w - pad, r.y + gui_s(26), subtitle);
    }
    return gui_rect(r.x + pad, r.y + gui_s(64), r.w - pad * 2, r.h - gui_s(64) - pad * 0.75f);
}

void screens_footer_hint(GuiRect r, const char *text) {
    GuiText t = { gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT, 0, false, false, 0 };
    gd_text(&t, r.x + r.w, r.y, text);
}

static void draw_brand(void) {
    GuiRect r = node_rect("brand");
    float a = gui_ease_out(gui_anim_t(gui_id("pause_open")) / 0.3f);
    gd_alpha_push(a);
    GuiText logo = { gui_s(40), GW_SEMIBOLD, TH->text, GUI_RIGHT, 0.16f, true, true, 0 };
    float y = r.y + r.h - gui_s(84);
    float w = gd_text(&logo, r.x + r.w, y, "Deadground");
    gd_rect(gui_rect(r.x + r.w - w, y + gui_s(50), gui_s(46), gui_s(3)), TH->accent);
    char line[160];
    snprintf(line, sizeof(line), "v%s", DG_VERSION);
    GuiText ver = { gui_s(TH->t_small), GW_MEDIUM, TH->text_faint, GUI_RIGHT, 0.04f, false, true, 0 };
    gd_text(&ver, r.x + r.w, y + gui_s(58), line);
    if (online()) {
        NetStats st = net_stats();
        Game *g = game();
        if (net_role() == NET_CLIENT) snprintf(line, sizeof(line), "%s  ·  ping %.0f ms  ·  loss %.1f%%", st.transport, st.ping_ms, st.loss_pct);
        else snprintf(line, sizeof(line), "Hosting via %s  ·  %d player%s", st.transport, net_host_client_count() + 1, net_host_client_count() ? "s" : "");
        (void)g;
        GuiText pl = { gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_RIGHT, 0, false, true, 0 };
        gd_text(&pl, r.x + r.w, y + gui_s(78), line);
    }
    gd_alpha_pop();
}

static int sub_of(void) {
    // The pause entry highlighted while its sub-menu is open.
    for (int i = M->depth - 1; i >= 1; i--) {
        int s = M->stack[i];
        if (s == SCR_SETTINGS || s == SCR_MULTIPLAYER || s == SCR_INVENTORY || s == SCR_LOBBY || s == SCR_COMMANDS) return s;
    }
    return SCR_NONE;
}

static void do_action(const char *action) {
    if (!strcmp(action, "resume")) menus_close_all();
    else if (!strcmp(action, "inventory")) { if (screens_top() != SCR_INVENTORY) { while (screens_top() != SCR_PAUSE && M->depth) screens_pop(); screens_push(SCR_INVENTORY); } }
    else if (!strcmp(action, "settings")) { while (screens_top() != SCR_PAUSE && M->depth) screens_pop(); settings_open(-1); screens_push(SCR_SETTINGS); }
    else if (!strcmp(action, "multiplayer")) { while (screens_top() != SCR_PAUSE && M->depth) screens_pop(); net_lan_refresh(); screens_push(SCR_MULTIPLAYER); }
    else if (!strcmp(action, "invite")) net_steam_invite();
    else if (!strcmp(action, "lobby")) { while (screens_top() != SCR_PAUSE && M->depth) screens_pop(); screens_push(SCR_LOBBY); }
    else if (!strcmp(action, "commands")) { while (screens_top() != SCR_PAUSE && M->depth) screens_pop(); commands_tab_open(); screens_push(SCR_COMMANDS); }
    else if (!strcmp(action, "copypos")) {
        // The compass coordinates, as a setpos line (docs/UI_WHEELS_HUD.md "Compass").
        if (hud_copy_coords()) { gui_sound(TH->snd_click); log_info("position copied to the clipboard: %s", hud_coords_text()); }
        else gui_sound(TH->snd_error);
    }
    else if (!strcmp(action, "mainmenu")) {
        bool host = lobby_is_host() && lobby_count(lobby_get()) > 1;
        screens_confirm(CONFIRM_MAINMENU, "Leave to main menu?", host ? "You are the host: the game ends for everyone." : "You will leave the game.", 0);
    }
    else if (!strcmp(action, "leave")) {
        Game *g = game();
        bool client = g && g->mode == GM_CLIENT;
        screens_confirm(CONFIRM_LEAVE, client ? "Leave the game?" : "Stop hosting?",
                        client ? "You will return to the local dev room." : "Connected players will be disconnected.", 0);
    } else if (!strcmp(action, "quit")) screens_confirm(CONFIRM_QUIT, "Quit to desktop?", "Unsaved progress will be lost.", 0);
}

static void draw_pause(bool top) {
    // Dim the scene a little behind the stack (a gradient from the left).
    float a = gui_ease_out(gui_anim_t(gui_id("pause_open")) / TH->menu_in);
    gd_rect_hgrad(gui_rect(0, 0, gui_w() * 0.55f, gui_h()), gui_rgba_mul(UI_RGBA(0, 0, 0, 200), a), UI_RGBA(0, 0, 0, 0));
    gd_rect_grad(gui_rect(0, gui_h() * 0.45f, gui_w(), gui_h() * 0.55f), UI_RGBA(0, 0, 0, 0), gui_rgba_mul(UI_RGBA(0, 0, 0, 150), a));
    int stack = gui_layout_find(M->nodes, M->node_count, "stack");
    int sub = sub_of();
    float t = gui_anim_t(gui_id("pause_open"));
    int k = 0;
    gui_push_input(top || sub != SCR_NONE);   // the stack stays usable next to a sub-menu
    for (int i = 0; i < M->node_count; i++) {
        LayoutNode *n = &M->nodes[i];
        if (n->parent != stack || n->hidden) continue;
        float appear = saturate((t - (float)k * TH->menu_stagger) / fmaxf(0.01f, TH->menu_in));
        k++;
        if (!n->action[0]) {
            if (n->label[0]) {
                gd_alpha_push(gui_ease_out(appear));
                GuiText tt = { gui_s(TH->t_label + 1), GW_SEMIBOLD, TH->text_faint, GUI_LEFT, TH->tracking_caps * 1.6f, true, true, 0 };
                gd_text(&tt, n->rect.x + gui_s(14), n->rect.y + n->rect.h - gd_line_height(tt.size) - gui_s(6), n->label);
                gd_alpha_pop();
            }
            continue;
        }
        const char *label = n->label;
        if (!strcmp(n->action, "leave")) {
            Game *g = game();
            label = g && g->mode == GM_CLIENT ? "Leave game" : "Stop hosting";
        }
        bool selected = (!strcmp(n->action, "settings") && sub == SCR_SETTINGS) || (!strcmp(n->action, "multiplayer") && sub == SCR_MULTIPLAYER) ||
                        (!strcmp(n->action, "inventory") && sub == SCR_INVENTORY) || (!strcmp(n->action, "lobby") && sub == SCR_LOBBY) ||
                        (!strcmp(n->action, "commands") && sub == SCR_COMMANDS);
        bool enabled = !(!strcmp(n->action, "inventory") && !game_local_player());
        u32 flags_default = !strcmp(n->action, "resume");
        (void)flags_default;
        if (gw_menu_item(gui_id(n->id), n->rect, label, selected, appear, enabled)) {
            M->pause_focus = gui_id(n->id);
            do_action(n->action);
        }
    }
    gui_pop_input();
    draw_brand();
}

static void draw_confirm(bool top) {
    GuiRect r = screens_dialog_rect();
    float a = gui_ease_out(gui_anim_t(gui_idi("screen", SCR_CONFIRM)) / TH->modal_in);
    gd_rect(gui_rect(0, 0, gui_w(), gui_h()), gui_rgba_mul(TH->scrim, a));
    r.y += (1.0f - a) * gui_s(12);
    gd_alpha_push(a);
    gui_push_input(top);
    gw_panel(r, true);
    gui_block_rect(r);
    float pad = gui_s(TH->pad + 4);
    GuiText t = { gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT, 0, false, false, r.w - pad * 2 };
    gd_text(&t, r.x + pad, r.y + pad, M->confirm_title);
    char text[288];
    dg_strcpy(text, sizeof(text), M->confirm_text);
    if (M->confirm_kind == CONFIRM_DISPLAY) {
        snprintf(text, sizeof(text), "%s Reverting in %d s.", M->confirm_text, (int)ceilf(settings_confirm_left()));
    }
    GuiText b = { gui_s(TH->t_body), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, false, 0 };
    gd_text_wrap(&b, r.x + pad, r.y + pad + gui_s(40), r.w - pad * 2, text, true);
    float bw = gui_s(M->confirm_kind == CONFIRM_FIDELITY ? 200 : 150), bh = gui_s(TH->button_h + 4);
    GuiRect yes = { r.x + r.w - pad - bw, r.y + r.h - pad - bh, bw, bh };
    GuiRect no = { yes.x - bw - gui_s(10), yes.y, bw, bh };
    const char *yes_l = "Confirm", *no_l = "Cancel";
    int yes_kind = GB_PRIMARY;
    switch (M->confirm_kind) {
    case CONFIRM_QUIT: yes_l = "Quit"; yes_kind = GB_DANGER; break;
    case CONFIRM_LEAVE: yes_l = "Leave"; yes_kind = GB_DANGER; break;
    case CONFIRM_MAINMENU: yes_l = "Leave"; yes_kind = GB_DANGER; break;
    case CONFIRM_DROP: yes_l = "Drop"; break;
    case CONFIRM_DISPLAY: yes_l = "Keep"; no_l = "Revert"; break;
    case CONFIRM_RESET_BINDS: yes_l = "Reset"; yes_kind = GB_DANGER; break;
    case CONFIRM_FIDELITY: yes_l = "Apply recommended"; no_l = "Keep current"; break;
    case CONFIRM_COMMAND: yes_l = "Run"; yes_kind = GB_DANGER; break;
    }
    bool y = gw_button(gui_id("confirm_yes"), yes, yes_l, yes_kind, true);
    bool n = gw_button(gui_id("confirm_no"), no, no_l, GB_SECONDARY, true);
    if (top && !gui_focus()) gui_set_focus(gui_id("confirm_no"));
    gui_focus_scope(r);
    bool back = top && gui_nav(NAV_BACK);
    if (back) gui_nav_consume(NAV_BACK);
    gui_pop_input();
    gd_alpha_pop();
    if (!top) return;
    if (y || n || back) {
        int kind = M->confirm_kind;
        bool yes_pressed = y;
        screens_pop();
        switch (kind) {
        case CONFIRM_QUIT: if (yes_pressed) g_engine.quit = true; break;   // main.c: settings + profile saved, lobby left, Steam shut down
        case CONFIRM_MAINMENU:
            if (yes_pressed) { lobby_leave("left to the main menu"); mainmenu_enter(NULL); }
            break;
        case CONFIRM_LEAVE:
            if (yes_pressed) { menus_close_all(); game_disconnect_to_local("left the session"); }
            break;
        case CONFIRM_DROP: inventory_confirm_drop(yes_pressed); break;
        case CONFIRM_DISPLAY: settings_confirm_result(yes_pressed); break;
        case CONFIRM_RESET_BINDS: if (yes_pressed) input_reset_bindings(); break;
        case CONFIRM_FIDELITY: if (yes_pressed) game_fidelity_apply_recommended(); break;
        case CONFIRM_COMMAND: commands_tab_confirm_result(yes_pressed); break;
        }
    }
}

// Called by the settings tick when the countdown runs out with the dialog still open.
void screens_confirm_expired(void) {
    if (screens_top() == SCR_CONFIRM && M->confirm_kind == CONFIRM_DISPLAY) {
        M->depth--;
        settings_confirm_result(false);
    }
}

static void draw_modal(int id, bool top) {
    GuiRect r = screens_modal_rect();
    float a = gui_ease_out(gui_anim_t(gui_idi("screen", id)) / TH->modal_in);
    r.y += (1.0f - a) * gui_s(14);
    gd_alpha_push(a);
    gui_push_input(top);
    if (top) gui_focus_scope(r);
    bool close = false;
    switch (id) {
    case SCR_SETTINGS: close = settings_draw(r); break;
    case SCR_MULTIPLAYER: close = multiplayer_draw(r); break;
    case SCR_BENCH: close = bench_draw(screens_dialog_rect().w > 0 ? r : r); break;
    case SCR_LOBBY: close = mainmenu_lobby_page_draw(r) || !lobby_active(); break;
    case SCR_COMMANDS: close = commands_tab_draw(r); break;
    default: break;
    }
    gui_pop_input();
    gd_alpha_pop();
    if (close && top) screens_pop();
}

// ------------------------------------------------------------------ public API

bool menus_wants_mouse(void) {
    Game *g = game();
    return (M && M->depth > 0) || (g && (g->inventory_open || g->container_open || g->bench_open)) || vfx_editor_open() || holdpoint_editor_open() ||
           daynight_editor_open() || flesh_range_open() || scene_wants_mouse();
}

bool menus_open(void) { return M && M->depth > 0; }
bool menus_dev_tools_open(void) { return vfx_editor_open() || holdpoint_editor_open() || daynight_editor_open() || flesh_range_open() || scene_wants_mouse(); }
void menus_frame_end(void) { if (M && M->inited) gui_frame_end(); }
bool menus_paused(void) { return M && M->depth > 0 && M->stack[0] == SCR_PAUSE && menus_can_pause(); }

void menus_toggle_pause(void) {
    if (!M->depth) { screens_push(SCR_PAUSE); return; }
    // Esc with screens open: handled as "back" by the screens (gui NAV_BACK) - nothing here.
}

void menus_toggle_inventory(void) {
    Game *g = game();
    if (!g || !g->world) return;
    if (screens_top() == SCR_INVENTORY) { screens_pop(); return; }
    if (M->depth) return;
    screens_push(SCR_INVENTORY);
}

void menus_tick(float dt) {
    gui_frame_begin(dt);
    Game *g = game();
    // Gamepad Start: pause / resume. Tab / I / pad Back (or the inventory binding): inventory.
    if (gui_nav(NAV_MENU)) {
        gui_nav_consume(NAV_MENU);
        if (M->depth && M->stack[0] == SCR_MAINMENU) { /* the main menu stays: Start does nothing there */ }
        else if (!M->depth) screens_push(SCR_PAUSE);
        else menus_close_all();
    }
    // Enter: the chat line, in a multiplayer game (the lobby's chat backend).
    if (!M->depth && !console_is_open() && lobby_active() && game_world() && gui_key_pressed(SDL_SCANCODE_RETURN)) mainmenu_chat_open();
    mainmenu_tick(dt);
    bool inv_key = gui_nav(NAV_VIEW) || (!M->depth && !console_is_open() && input_consume_press(ACT_INVENTORY));
    if (inv_key && !console_is_open() && (!M->depth || screens_top() == SCR_INVENTORY) && inventory_wants_back()) {
        gui_nav_consume(NAV_VIEW);
        menus_toggle_inventory();
    }
    // World-driven screens.
    if (g && g->container_open && !screens_has(SCR_INVENTORY)) screens_push(SCR_INVENTORY);
    if (g && g->bench_open && !screens_has(SCR_BENCH)) screens_push(SCR_BENCH);
    if (g && !g->world && screens_has(SCR_INVENTORY)) menus_close_all();
    settings_tick(dt);
    if (g) {
        g->menu_open = M->depth > 0;
        g->paused = menus_paused();
    }
    update_mouse_mode();
}

void menus_draw(void) {
    if (!M->depth) { mainmenu_chat_feed_draw(); vfx_editor_draw(); holdpoint_editor_draw(); return; }
    solve_layout();
    int top = screens_top();
    for (int i = 0; i < M->depth; i++) {
        int id = M->stack[i];
        bool is_top = i == M->depth - 1;
        gd_set_layer(GL_BASE);
        switch (id) {
        case SCR_PAUSE: draw_pause(is_top); break;
        case SCR_INVENTORY: {
            bool as_modal = i > 0;
            gui_push_input(is_top);
            bool close = inventory_draw(as_modal ? screens_modal_rect() : gui_rect(0, 0, gui_w(), gui_h()), as_modal);
            gui_pop_input();
            if (close && is_top) screens_pop();
            break;
        }
        case SCR_CONFIRM: draw_confirm(is_top); break;
        case SCR_MAINMENU: mainmenu_draw(is_top); break;
        case SCR_CHAT:
            if (mainmenu_chat_draw(is_top) && is_top) screens_pop();
            break;
        default: draw_modal(id, is_top); break;
        }
    }
    gd_set_layer(GL_BASE);
    // Back (Esc / B) not taken by a widget: pop the top screen.
    if (gui_nav(NAV_BACK) && M->depth && screens_top() == top && top != SCR_MAINMENU) {
        gui_nav_consume(NAV_BACK);
        if (top != SCR_INVENTORY || inventory_wants_back()) screens_pop();
    }
    vfx_editor_draw();
    holdpoint_editor_draw();
}

// ------------------------------------------------------------------ commands

static void cmd_menu(int argc, const char **argv) {
    const char *page = argc > 1 ? argv[1] : "pause";
    if (!strcmp(page, "close")) { menus_close_all(); return; }
    if (!M->depth || M->stack[0] != SCR_PAUSE) { menus_close_all(); screens_push(SCR_PAUSE); }
    if (!strcmp(page, "settings") || !strcmp(page, "graphics")) do_action("settings");
    else if (!strcmp(page, "controls")) { do_action("settings"); settings_open(1); }
    else if (!strcmp(page, "crosshair")) { do_action("settings"); settings_open(2); }
    else if (!strcmp(page, "audio")) { do_action("settings"); settings_open(3); }
    else if (!strcmp(page, "skin") || !strcmp(page, "character")) { do_action("settings"); settings_open(4); }
    else if (!strcmp(page, "phone")) { do_action("settings"); settings_open(6); }
    else if (!strcmp(page, "multiplayer")) do_action("multiplayer");
    else if (!strcmp(page, "inventory")) do_action("inventory");
    else if (!strcmp(page, "quit")) do_action("quit");
    else if (!strcmp(page, "leave")) do_action("leave");
}

// ui_screen <name>: open a screen directly (tests, screenshots).
static void cmd_ui_screen(int argc, const char **argv) {
    if (argc < 2) {
        char line[160] = "";
        for (int i = 0; i < M->depth; i++) { strncat(line, " ", sizeof(line) - strlen(line) - 1); strncat(line, screen_name(M->stack[i]), sizeof(line) - strlen(line) - 1); }
        log_info("ui_screen <pause|settings[:tab]|multiplayer|inventory|bench|commands|copypos|confirm_quit|confirm_leave|close>  stack:%s", line[0] ? line : " (none)");
        return;
    }
    const char *n = argv[1];
    if (!strcmp(n, "close")) { menus_close_all(); return; }
    if (!strcmp(n, "inventory") && (!M->depth)) { screens_push(SCR_INVENTORY); return; }
    if (!strcmp(n, "bench")) { Game *g = game(); if (g) g->bench_open = true; screens_push(SCR_BENCH); return; }
    if (!strncmp(n, "settings", 8)) {
        if (!M->depth || M->stack[0] != SCR_PAUSE) { menus_close_all(); screens_push(SCR_PAUSE); }
        do_action("settings");
        if (n[8] == ':') settings_open(atoi(n + 9));
        return;
    }
    if (!strcmp(n, "commands") || !strcmp(n, "copypos")) {
        if (!M->depth || M->stack[0] != SCR_PAUSE) { menus_close_all(); screens_push(SCR_PAUSE); }
        do_action(n);
        return;
    }
    if (!strcmp(n, "confirm_quit")) { if (!M->depth) screens_push(SCR_PAUSE); do_action("quit"); return; }
    if (!strcmp(n, "confirm_leave")) { if (!M->depth) screens_push(SCR_PAUSE); do_action("leave"); return; }
    const char *args[2] = { "menu", n };
    cmd_menu(2, args);
}

// ui_nav <up|down|left|right|accept|back|tab_next|tab_prev|alt|alt2|menu|view>: synthetic
// navigation input (scripted keyboard / gamepad tests).
static void cmd_ui_nav(int argc, const char **argv) {
    if (argc < 2) return;
    static const struct { const char *n; SDL_Scancode k; int pad; } map[] = {
        { "up", SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP }, { "down", SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN },
        { "left", SDL_SCANCODE_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT }, { "right", SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
        { "accept", SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_SOUTH }, { "back", SDL_SCANCODE_ESCAPE, SDL_GAMEPAD_BUTTON_EAST },
        { "tab_next", SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER }, { "tab_prev", SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
        { "alt", SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_WEST }, { "alt2", SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_NORTH },
        { "menu", SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_START }, { "view", SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_BACK },
    };
    bool pad = argc > 2 && !strcmp(argv[2], "pad");
    for (int i = 0; i < (int)ARRAY_COUNT(map); i++) {
        if (strcmp(argv[1], map[i].n)) continue;
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        if (!pad && map[i].k != SDL_SCANCODE_UNKNOWN) {
            ev.type = SDL_EVENT_KEY_DOWN;
            ev.key.scancode = map[i].k;
            if (map[i].k == SDL_SCANCODE_ESCAPE && !M->depth) { menus_toggle_pause(); return; }
        } else {
            ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            ev.gbutton.button = (Uint8)map[i].pad;
        }
        gui_handle_event(&ev);
        return;
    }
    log_warn("ui_nav: unknown '%s'", argv[1]);
}

// ui_key <SDL key name>: a synthetic key press ("1".."9", "Return", "Q", "E" ...; scripted tests).
static void cmd_ui_key(int argc, const char **argv) {
    if (argc < 2) return;
    SDL_Scancode sc = SDL_GetScancodeFromName(argv[1]);
    if (sc == SDL_SCANCODE_UNKNOWN) { log_warn("ui_key: unknown key '%s'", argv[1]); return; }
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.scancode = sc;
    gui_handle_event(&ev);
}

// ui_mouse <x> <y> [down|up|click|rclick|dclick|wheel <n>] (fractions 0..1 of the window when <= 1)
static void cmd_ui_mouse(int argc, const char **argv) {
    if (argc < 3) return;
    float x = (float)atof(argv[1]), y = (float)atof(argv[2]);
    if (x <= 1.0f && y <= 1.0f) { x *= gui_w(); y *= gui_h(); }
    float pd = g_engine.window ? SDL_GetWindowPixelDensity(g_engine.window) : 1.0f;
    if (pd <= 0) pd = 1;
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.x = x / pd;
    ev.motion.y = y / pd;
    ev.motion.xrel = 4;
    gui_handle_event(&ev);
    const char *what = argc > 3 ? argv[3] : "";
    bool right = !strcmp(what, "rclick") || !strcmp(what, "rdown") || !strcmp(what, "rup");
    if (!strcmp(what, "wheel")) {
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_MOUSE_WHEEL;
        ev.wheel.y = argc > 4 ? (float)atof(argv[4]) : -1.0f;
        gui_handle_event(&ev);
        return;
    }
    bool down = !strcmp(what, "down") || !strcmp(what, "rdown") || !strcmp(what, "click") || !strcmp(what, "rclick") || !strcmp(what, "dclick");
    bool up = !strcmp(what, "up") || !strcmp(what, "rup") || !strcmp(what, "click") || !strcmp(what, "rclick") || !strcmp(what, "dclick");
    if (down) {
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        ev.button.button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        ev.button.x = x / pd;
        ev.button.y = y / pd;
        ev.button.clicks = !strcmp(what, "dclick") ? 2 : 1;
        gui_handle_event(&ev);
    }
    if (up) {
        // Released next frame (a press and release in one frame is still a click).
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
        ev.button.button = right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        ev.button.x = x / pd;
        ev.button.y = y / pd;
        gui_handle_event(&ev);
    }
}

static void cmd_ui_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    gui_reload();
    load_layout();
    mainmenu_reload_layouts();
    icons_reload();
    log_info("ui: reloaded theme, fonts, screen layouts and icons");
}

static void cmd_inventory(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    menus_toggle_inventory();
}

void menus_init(void) {
    M = (Menus *)calloc(1, sizeof(Menus));
    gui_theme_load("assets/ui/theme.json");
    gui_init();
    if (!gd_init()) log_error("ui: draw layer init failed (fonts)");
    load_layout();
    icons_init();
    gd_register_commands();
    inventory_register_commands();
    commands_tab_register();
    cmd_register("menu", cmd_menu, "open a menu: pause | settings | controls | crosshair | audio | character | phone | multiplayer | inventory | close");
    cmd_register("inventory", cmd_inventory, "toggle the inventory screen");
    cmd_register("ui_screen", cmd_ui_screen, "ui_screen <name>: open a UI screen (no args: list + current stack)");
    cmd_register("ui_reload", cmd_ui_reload, "reload the UI theme, fonts, screen layouts and icons");
    cmd_register("ui_nav", cmd_ui_nav, "ui_nav <up|down|left|right|accept|back|tab_next|tab_prev|alt|alt2|menu|view> [pad]: synthetic UI navigation");
    cmd_register("ui_mouse", cmd_ui_mouse, "ui_mouse <x> <y> [down|up|click|rclick|dclick|wheel n]: synthetic UI mouse (0..1 = window fraction)");
    cmd_register("ui_key", cmd_ui_key, "ui_key <key name>: synthetic key press (scripted tests)");
    M->inited = true;
}

bool menus_handle_event(const SDL_Event *ev) {
    if (!M || !M->inited) return false;
    // Key capture (rebinding) owns the keyboard / pad: no navigation, no back.
    if (input_capturing() && (ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) return false;
    return gui_handle_event(ev);
}
