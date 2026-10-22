// Play tab (docs/MENU_LOBBY.md): Host game, Join (server browser: Steam lobbies, friends' games, LAN
// discovery, direct IP), Continue (stub). Steam-only parts are greyed with the reason
// when Steam isn't running (offline / LAN mode).
#include "mm_internal.h"
#include "ui/screens/screens.h"
#include "core/cvar.h"
#include "core/platform.h"
#include "world/season.h"
#include "game/game_internal.h"
#include "game/game_story.h"
#include <stdio.h>
#include <string.h>

#define MAX_ROWS 96

static struct {
    int host_kind;                // 0 Steam, 1 LAN
    int source;                   // BrowserSource
    BrowserFilter filter;
    int filter_map, filter_players, filter_region;
    BrowserEntry rows[MAX_ROWS];
    int row_count, selected;
    double refreshed[BS_COUNT];
    GuiScroll scroll;
    char direct[64];
} P = { .host_kind = 0, .source = BS_LAN, .filter_region = 1, .selected = -1, .direct = "127.0.0.1:27015" };

static bool steam(void) { return lobby_steam_running(); }

static void host_game(void) {
    TransportKind k = steam() && P.host_kind == 0 ? TRANSPORT_STEAM : TRANSPORT_LAN;
    lobby_host(k);
}

static void refresh(int source) {
    BrowserFilter f = P.filter;
    lobby_browser_refresh(source, &f);
    P.refreshed[source] = gui_time();
    P.selected = -1;
}

// ------------------------------------------------------------------ main page

static void draw_side(GuiRect r) {
    mm_panel(r);
    float pad = gui_s(TH->pad);
    GuiRect in = gui_rect_inset(r, pad);
    GuiCursor c = gc_make(in, TOK("mm_gap_sm"));
    const MapInfo *mi = lobby_map_info(mm()->map);
    gw_section(gc_row(&c, TOK("mm_row_h")), "Selected map");
    if (mi) {
        GuiRect th = gc_row(&c, in.w / 1.7778f);
        mm_thumb(th, mi->thumbnail);
        gw_label(gc_row(&c, TOK("mm_title_h")), mi->name, gui_s(TH->t_title - 4), GW_SEMIBOLD, TH->text, GUI_LEFT);
        const SeasonTable *st = weather_seasons();
        char v[64];
        snprintf(v, sizeof(v), "%s  ·  %02d:00  ·  %s", st->s[DG_CLAMP(mm()->season, 0, SEASON_COUNT - 1)].name, mm()->hour, mm()->weather[0] ? mm()->weather : "season weather");
        mm_kv(&c, "Start", v);
        snprintf(v, sizeof(v), "%d - %d", mi->players_min, mi->players_max);
        mm_kv(&c, "Players", v);
    }
    c.y += TOK("mm_gap");
    // Friends playing (Steam) / LAN games nearby.
    if (steam()) {
        FriendEntry fr[8];
        int n = lobby_friends(fr, 8), shown = 0;
        gw_section(gc_row(&c, TOK("mm_row_h")), "Friends");
        for (int i = 0; i < n && shown < 4; i++) {
            if (!fr[i].state) continue;
            char v[64];
            snprintf(v, sizeof(v), "%s", fr[i].in_game ? (fr[i].lobby ? "In a lobby" : "Playing") : "Online");
            mm_kv(&c, fr[i].name, v);
            shown++;
        }
        if (!shown) gw_label(gc_row(&c, TOK("mm_row_h")), "No friends online", gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT);
    } else {
        gw_section(gc_row(&c, TOK("mm_row_h")), "Offline");
        gd_text_wrap(&(GuiText){ gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT, 0, false, false, 0 }, c.area.x, c.y, c.area.w,
                     "Steam isn't running: hosting and joining work on the local network (LAN) and by direct IP. Steam lobbies, invites and friends need Steam.", true);
    }
}

static void draw_main(GuiRect content) {
    MmLayout *L = &mm()->play;
    mm_layout_solve(L, content);
    int stack = gui_layout_find(L->nodes, L->count, "stack");
    float t = gui_anim_t(gui_id("mm_tab_in"));
    int k = 0;
    const Profile *prof = lobby_profile();
    for (int i = 0; i < L->count; i++) {
        LayoutNode *n = &L->nodes[i];
        if (n->parent != stack) continue;
        float appear = saturate((t - (float)k * TH->menu_stagger) / fmaxf(0.01f, TH->menu_in));
        k++;
        if (!strcmp(n->id, "title")) { gd_alpha_push(gui_ease_out(appear)); mm_title(n->rect, n->label); gd_alpha_pop(); continue; }
        if (!strcmp(n->id, "transport")) {
            // Where "Host game" hosts: Steam (friends can join through Steam) or the LAN.
            static const char *const items[] = { "Host on Steam", "Host on LAN" };
            static const bool en_on[] = { true, true }, en_off[] = { false, true };
            static const char *const notes_off[] = { "Steam not running", "" };
            int sel = steam() ? P.host_kind : 1;
            gd_alpha_push(gui_ease_out(appear));
            if (gw_dropdown_ex(gui_id("mm_host_kind"), n->rect, items, 2, &sel, steam() ? en_on : en_off, steam() ? NULL : notes_off)) P.host_kind = sel;
            gd_alpha_pop();
            continue;
        }
        if (!n->action[0]) continue;
        bool enabled = true;
        const char *tip = NULL;
        // Continue: the last story checkpoint when there is one (docs/STORY.md "Saves"), else the last session.
        static double story_checked = -10;
        static bool story_save;
        static char story_chapter[48];
        if (gui_time() - story_checked > 2.0) { story_checked = gui_time(); story_save = story_continue_available(story_chapter, sizeof(story_chapter)); }
        if (!strcmp(n->action, "continue")) { enabled = story_save || prof->last_session[0] != 0; tip = "No saved session yet"; }
        GuiId id = gui_id(n->id);
        if (gw_menu_item(id, n->rect, n->label, false, appear, enabled)) {
            if (!strcmp(n->action, "host")) host_game();
            else if (!strcmp(n->action, "join")) { mm()->play_page = PP_BROWSER; P.source = steam() ? BS_STEAM : BS_LAN; refresh(P.source); gui_set_focus(0); }
            else if (!strcmp(n->action, "continue") && story_save) story_start(story_chapter, "auto");
            else if (!strcmp(n->action, "continue")) lobby_host(steam() && P.host_kind == 0 ? TRANSPORT_STEAM : TRANSPORT_LAN);
            else if (!strcmp(n->action, "story")) story_start("01_tenth_floor", NULL);
        }
        if (!enabled && tip && gui_rect_contains(n->rect, gui_mouse().x, gui_mouse().y)) gui_tooltip(id, tip);
        if (!gui_focus() && !strcmp(n->action, "host") && gui_nav_mode()) gui_set_focus(id);
    }
    draw_side(mm_node(L, "side"));
}

// ------------------------------------------------------------------ browser

static void draw_filters(MmLayout *L) {
    const MapList *ml = lobby_maps();
    const char *maps[MAPLIST_MAX + 1];
    maps[0] = "Any map";
    for (int i = 0; i < ml->count; i++) maps[i + 1] = ml->maps[i].name;
    if (gw_dropdown(gui_id("mm_f_map"), mm_node(L, "f_map"), maps, ml->count + 1, &P.filter_map))
        dg_strcpy(P.filter.map, sizeof(P.filter.map), P.filter_map ? ml->maps[P.filter_map - 1].id : "");
    static const char *const players[] = { "Any players", "Not full", "Not empty" };
    if (gw_dropdown(gui_id("mm_f_players"), mm_node(L, "f_players"), players, 3, &P.filter_players)) P.filter.players = P.filter_players;
    static const char *const regions[] = { "Region: close", "Region: default", "Region: far", "Worldwide" };
    gw_disabled(P.source != BS_STEAM);
    if (gw_dropdown(gui_id("mm_f_region"), mm_node(L, "f_region"), regions, 4, &P.filter_region)) { P.filter.region = P.filter_region; refresh(P.source); }
    gw_disabled(false);
    GuiRect fr = mm_node(L, "f_friends");
    GuiRect a, b;
    gui_split_h(fr, 0.5f, TOK("mm_gap"), &a, &b);
    gw_disabled(!steam());
    gw_toggle(gui_id("mm_f_friends"), a, &P.filter.friends_only);
    gw_disabled(false);
    gw_label(gui_rect(a.x + gui_s(TH->button_h * 3), a.y, a.w, a.h), "Friends only", gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT);
    gw_toggle(gui_id("mm_f_running"), b, &P.filter.show_running);
    gw_label(gui_rect(b.x + gui_s(TH->button_h * 3), b.y, b.w, b.h), "In progress", gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_LEFT);
    bool busy = lobby_browser_busy(P.source);
    if (gw_button(gui_id("mm_f_refresh"), mm_node(L, "f_refresh"), busy ? "Searching" : "Refresh", GB_SECONDARY, !busy && P.source != BS_DIRECT)) refresh(P.source);
}

static void draw_list(GuiRect r) {
    mm_panel(r);
    GuiRect view = gui_rect_inset(r, TOK("mm_gap"));
    view.w -= TOK("mm_gap");
    P.row_count = lobby_browser_entries(P.source, &P.filter, P.rows, MAX_ROWS);
    gw_scroll_begin(&P.scroll, gui_id("mm_browser_scroll"), view);
    float y = gw_scroll_y(&P.scroll), rh = TOK("mm_list_row_h");
    for (int i = 0; i < P.row_count; i++) {
        const BrowserEntry *e = &P.rows[i];
        GuiRect row = { view.x, y + (float)i * rh, view.w, rh - TOK("mm_gap_sm") };
        GuiId id = gui_idi("mm_row", i);
        GuiInteract in = gui_interact(id, row, GUI_FOCUSABLE);
        bool sel = P.selected == i;
        if (sel) gd_round(row, gui_s(TH->radius), TH->press);
        else if (in.hot) gd_round(row, gui_s(TH->radius), TH->hover);
        if (sel) gd_rect(gui_rect(row.x, row.y + row.h * 0.2f, gui_s(TH->accent_bar), row.h * 0.6f), TH->accent);
        if (in.focused) gw_focus_ring(row, gui_s(TH->radius), 1.0f);
        if (in.clicked) P.selected = i;
        if (in.double_clicked && e->version_ok) lobby_join(e->kind, e->address);
        gd_alpha_push(e->version_ok ? 1.0f : 0.45f);
        float pad = TOK("mm_gap") * 1.5f;
        gw_label(gui_rect(row.x + pad, row.y, row.w * 0.5f, row.h * 0.55f), e->name, gui_s(TH->t_body), GW_MEDIUM, TH->text, GUI_LEFT);
        const MapInfo *mi = lobby_map_info(e->map);
        char sub[160];
        snprintf(sub, sizeof(sub), "%s%s%s", mi ? mi->name : (e->map[0] ? e->map : "?"), e->in_game ? "  ·  in progress" : "", e->friend_host ? "  ·  friend" : "");
        if (!e->version_ok) snprintf(sub, sizeof(sub), "Different game version");
        gw_label(gui_rect(row.x + pad, row.y + row.h * 0.45f, row.w * 0.6f, row.h * 0.5f), sub, gui_s(TH->t_small), GW_REGULAR, e->version_ok ? TH->text_faint : TH->warning, GUI_LEFT);
        char right[48];
        if (e->ping_ms >= 0) snprintf(right, sizeof(right), "%d/%d    %.0f ms", e->players, e->max_players, e->ping_ms);
        else snprintf(right, sizeof(right), "%d/%d", e->players, e->max_players);
        gw_label(gui_rect(row.x, row.y, row.w - pad, row.h), right, gui_s(TH->t_body - 1), GW_MEDIUM, TH->text_dim, GUI_RIGHT);
        gd_alpha_pop();
    }
    if (!P.row_count) {
        const char *msg = lobby_browser_busy(P.source) ? "Searching..." :
                          P.source == BS_LAN ? "No LAN games found. Hosts answer on UDP port 27016." :
                          P.source == BS_FRIENDS ? "None of your friends is in a Deadground lobby." : "No lobbies match the filters.";
        gw_label(gui_rect(view.x + TOK("mm_gap"), y, view.w, TOK("mm_list_row_h")), msg, gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT);
    }
    gw_scroll_end(&P.scroll, (float)P.row_count * rh);
    if (P.selected >= P.row_count) P.selected = -1;
}

static void draw_direct(GuiRect r) {
    mm_panel(r);
    GuiRect in = gui_rect_inset(r, gui_s(TH->pad));
    GuiCursor c = gc_make(in, TOK("mm_gap"));
    gw_section(gc_row(&c, TOK("mm_row_h")), "Direct connect");
    gd_text_wrap(&(GuiText){ gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT, 0, false, false, 0 }, c.area.x, c.y, c.area.w,
                 "The host's address on the LAN or the internet (ip:port; the host forwards UDP 27015). A SteamID64 connects over Steam.", true);
    c.y += TOK("mm_title_h");
    GuiRect row = gc_row(&c, TOK("mm_button_h")), a, b;
    gui_split_h(row, 0.7f, TOK("mm_gap"), &a, &b);
    bool enter = gw_textfield(gui_id("mm_direct"), a, P.direct, (int)sizeof(P.direct), "ip:port or SteamID64");
    if ((gw_button(gui_id("mm_direct_go"), b, "Connect", GB_PRIMARY, P.direct[0] != 0) || enter) && P.direct[0]) {
        bool sid = !strchr(P.direct, '.') && !strchr(P.direct, ':') && strlen(P.direct) >= 15;
        lobby_join(sid ? TRANSPORT_STEAM : TRANSPORT_LAN, P.direct);
    }
}

static void draw_detail(MmLayout *L) {
    GuiRect d = mm_node(L, "detail");
    const BrowserEntry *e = P.selected >= 0 && P.selected < P.row_count ? &P.rows[P.selected] : NULL;
    const MapInfo *mi = e ? lobby_map_info(e->map) : NULL;
    mm_thumb(mm_node(L, "d_thumb"), mi ? mi->thumbnail : "");
    GuiRect info = mm_node(L, "d_info");
    GuiCursor c = gc_make(info, TOK("mm_gap_sm"));
    if (e) {
        gw_label(gc_row(&c, TOK("mm_title_h")), e->name, gui_s(TH->t_title - 4), GW_SEMIBOLD, TH->text, GUI_LEFT);
        char v[64];
        mm_kv(&c, "Map", mi ? mi->name : e->map);
        snprintf(v, sizeof(v), "%d / %d", e->players, e->max_players);
        mm_kv(&c, "Players", v);
        if (e->season >= 0 && e->season < SEASON_COUNT) mm_kv(&c, "Season", weather_seasons()->s[e->season].name);
        mm_kv(&c, "State", e->in_game ? "In progress" : "In the lobby");
        mm_kv(&c, "Network", e->kind == TRANSPORT_STEAM ? "Steam" : "LAN");
        if (!e->version_ok) gw_label(gc_row(&c, TOK("mm_row_h")), "Different game version: update to join", gui_s(TH->t_small), GW_MEDIUM, TH->warning, GUI_LEFT);
    } else {
        gw_label(gc_row(&c, TOK("mm_row_h")), "Select a game", gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT);
    }
    (void)d;
    if (gw_button(gui_id("mm_join"), mm_node(L, "d_join"), "Join", GB_PRIMARY, e && e->version_ok)) lobby_join(e->kind, e->address);
    if (gw_button(gui_id("mm_browser_back"), mm_node(L, "d_back"), "Back", GB_SECONDARY, true)) mm()->play_page = PP_MAIN;
}

static void draw_browser(GuiRect content) {
    MmLayout *L = &mm()->browser;
    mm_layout_solve(L, content);
    static const char *const sources[BS_COUNT] = { "Steam lobbies", "Friends", "LAN", "Direct IP" };
    bool en[BS_COUNT] = { steam(), steam(), true, true };
    static const char *const why[BS_COUNT] = { "Steam isn't running", "Steam isn't running", NULL, NULL };
    if (!en[P.source]) P.source = BS_LAN;
    int s = gw_tabbar(gui_id("mm_sources"), mm_node(L, "sources"), sources, en, why, BS_COUNT, P.source, true);
    if (s != P.source) { P.source = s; if (s != BS_DIRECT) refresh(s); }
    // LAN: listen again every few seconds while the page is open.
    if (P.source == BS_LAN && gui_time() - P.refreshed[BS_LAN] > 5.0) refresh(BS_LAN);
    if (P.source == BS_DIRECT) {
        GuiRect f = mm_node(L, "filters"), l = mm_node(L, "list");
        draw_direct(gui_rect(f.x, f.y, f.w, l.y + l.h - f.y));
    } else {
        draw_filters(L);
        draw_list(mm_node(L, "list"));
    }
    draw_detail(L);
    mm_footer_hint(gui_rect(content.x, content.y + content.h, content.w, TOK("mm_row_h")), "Enter / A  join     Esc / B  back");
}

void mm_play_draw(GuiRect content) {
    if (mm()->play_page == PP_BROWSER) draw_browser(content);
    else draw_main(content);
}
