// Play tab (docs/MENU_LOBBY.md "Party"): you are always in a party - the menu creates your own lobby on
// start (Steam when it runs, else LAN), private by default (the lock). Continue, Story, Start game
// (host; a guest: Ready), Join (server browser: Steam lobbies, friends' games, LAN discovery, direct IP),
// Leave party (in someone else's). The party panel: the lock, the players, the map, the game settings.
#include "mm_internal.h"
#include "ui/screens/screens.h"
#include "core/cvar.h"
#include "core/platform.h"
#include "world/season.h"
#include "game/game_internal.h"
#include "game/game_story.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_ROWS 96

static struct {
    int source;                   // BrowserSource
    BrowserFilter filter;
    int filter_map, filter_players, filter_region;
    BrowserEntry rows[MAX_ROWS];
    int row_count, selected;
    double refreshed[BS_COUNT];
    GuiScroll scroll;
    char direct[64];
} P = { .source = BS_LAN, .filter_region = 1, .selected = -1, .direct = "127.0.0.1:27015" };

static bool steam(void) { return lobby_steam_running(); }

static void refresh(int source) {
    BrowserFilter f = P.filter;
    lobby_browser_refresh(source, &f);
    P.refreshed[source] = gui_time();
    P.selected = -1;
}

// ------------------------------------------------------------------ main page: the party

static bool g_start_pending;   // Start pressed: ready first, then start (lobby_start wants the host ready)

static bool in_lobby_phase(void) { LobbyState s = lobby_state(); return s == LS_LOBBY || s == LS_COUNTDOWN; }

// Start the party's game (host): ready, then lobby_start - at once when everyone is ready, else the
// 10 s countdown. The Maps tab's "Host on this map" comes here too.
void mm_play_start(void) {
    if (!lobby_is_host() || !in_lobby_phase()) return;
    const LobbyMember *me = lobby_local((Lobby *)lobby_get());
    if (me && !me->ready) lobby_set_ready(true);
    g_start_pending = true;
}

static void start_pending(void) {
    if (!g_start_pending) return;
    const Lobby *L = lobby_get();
    const LobbyMember *me = lobby_local((Lobby *)L);
    if (!lobby_is_host() || !in_lobby_phase()) { g_start_pending = false; return; }
    if (!me || !me->ready) return;   // the ready intent lands first
    g_start_pending = false;
    if (L->state != LS_COUNTDOWN) lobby_start(!lobby_all_ready(L));
}

// A padlock: closed = private (friends / invite only), open = public. Returns true on a click.
static bool lock_button(GuiRect r, bool locked, bool enabled) {
    GuiId id = gui_id("mm_party_lock");
    GuiInteract in = gui_interact(id, r, enabled ? GUI_FOCUSABLE : GUI_DISABLED);
    if (in.hot && enabled) gd_round(r, gui_s(TH->radius), TH->hover);
    if (in.focused) gw_focus_ring(r, gui_s(TH->radius), 1.0f);
    u32 c = locked ? TH->accent : TH->text_dim;
    float s = fminf(r.w, r.h), cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    float bw = s * 0.56f, bh = s * 0.40f, sr = s * 0.17f, t = fmaxf(1.5f, s * 0.08f);
    GuiRect body = { cx - bw * 0.5f, cy - bh * 0.15f, bw, bh };
    // The shackle: a half ring over the body (open: lifted and swung right).
    float sx = locked ? cx : cx + bw * 0.42f, sy = body.y - (locked ? 0.0f : s * 0.10f);
    gd_ring(sx, sy, sr, t, 0.5f, c);
    gd_rect(gui_rect(sx - sr - t * 0.5f, sy, t, (locked ? 0.0f : s * 0.10f) + t), c);
    if (locked) gd_rect(gui_rect(sx + sr - t * 0.5f, sy, t, t), c);
    gd_round(body, gui_s(2), c);
    gd_circle(cx, body.y + bh * 0.45f, fmaxf(1.0f, s * 0.05f), TH->panel);
    if (in.hovered) gui_tooltip(id, locked ? "Private: only invited players (or by IP) can join. Click to make it public." : "Public: listed for everyone. Click to make it private.");
    return enabled && in.clicked;
}

// The party panel (right): the lock + privacy, the players, the map, the host's game settings.
static void draw_party(GuiRect r) {
    const Lobby *L = lobby_get();
    bool host = lobby_is_host() && in_lobby_phase();
    mm_panel(r);
    GuiRect in = gui_rect_inset(r, gui_s(TH->pad));
    GuiCursor c = gc_make(in, TOK("mm_gap_sm"));
    // Header: "Party  n / max", the lock, the privacy in words.
    GuiRect head = gc_row(&c, TOK("mm_title_h"));
    char title[48];
    snprintf(title, sizeof(title), "Party  %d / %d", lobby_count(L), L->settings.max_players);
    gw_label(head, title, gui_s(TH->t_title - 4), GW_SEMIBOLD, TH->text, GUI_LEFT);
    bool locked = L->settings.privacy != LP_PUBLIC;
    float ls = head.h;
    GuiRect lr = { head.x + head.w - ls, head.y, ls, ls };
    gw_label(gui_rect(head.x, head.y, head.w - ls - TOK("mm_gap"), head.h), lobby_privacy_name(L->settings.privacy), gui_s(TH->t_small), GW_REGULAR, locked ? TH->accent : TH->text_dim, GUI_RIGHT);
    if (lock_button(lr, locked, host)) {
        LobbySettings s = L->settings;
        s.privacy = locked ? LP_PUBLIC : LP_INVITE;
        lobby_set_settings(&s);
        lobby_profile()->lobby.privacy = s.privacy;   // the next party starts the same
        lobby_profile_changed();
    }
    // The players (the lobby screen's rows: ready, host, ping, phone; the host kicks / promotes).
    float ph = TOK("lb_player_h");
    int n = DG_MAX(1, lobby_count(L));
    GuiRect pr = gc_row(&c, (float)(n + 1) * (ph + TOK("mm_gap_sm")) + TOK("mm_row_h"));
    mm_lobby_players(pr);
    // The map and the start look (the Maps tab picks them).
    const MapInfo *mi = lobby_map_info(L->settings.map[0] ? L->settings.map : mm()->map);
    c.y += TOK("mm_gap");
    gw_section(gc_row(&c, TOK("mm_row_h")), "Map");
    if (mi) {
        GuiRect row = gc_row(&c, TOK("mm_map_row_h"));
        float th = row.h;
        mm_thumb(gui_rect(row.x, row.y, th * 1.7778f, th), mi->thumbnail);
        float x = row.x + th * 1.7778f + TOK("mm_gap");
        gw_label(gui_rect(x, row.y, row.x + row.w - x, row.h * 0.5f), mi->name, gui_s(TH->t_body), GW_SEMIBOLD, TH->text, GUI_LEFT);
        const SeasonTable *st = weather_seasons();
        char v[64];
        snprintf(v, sizeof(v), "%s  ·  %02d:00", st->s[DG_CLAMP(L->settings.season, 0, SEASON_COUNT - 1)].name, L->settings.start_hour);
        gw_label(gui_rect(x, row.y + row.h * 0.5f, row.x + row.w - x, row.h * 0.4f), v, gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT);
    }
    // Game settings: the host edits, everyone else reads.
    c.y += TOK("mm_gap");
    gw_section(gc_row(&c, TOK("mm_row_h")), host ? "Game settings" : "Game settings (host)");
    LobbySettings s = L->settings;
    bool changed = false;
    gw_disabled(!host);
    const char *diff[LD_COUNT];
    for (int i = 0; i < LD_COUNT; i++) diff[i] = lobby_difficulty_name(i);
    int d = s.difficulty;
    GuiRect rr = gw_setting_row(&c, "Difficulty", NULL, gui_id("pt_diff"));
    if (gw_dropdown(gui_id("pt_diff_dd"), rr, diff, LD_COUNT, &d)) { s.difficulty = (u8)d; changed = true; }
    float mp = (float)s.max_players;
    rr = gw_setting_row(&c, "Max players", NULL, gui_id("pt_max"));
    if (gw_slider(gui_id("pt_max_s"), rr, &mp, 1, LOBBY_MAX_MEMBERS, 1, "%.0f")) { s.max_players = (u8)mp; changed = true; }
    bool late = s.late_join;
    rr = gw_setting_row(&c, "Join a running game", "Players may join after the start (they load in and spawn).", gui_id("pt_late"));
    if (gw_toggle(gui_id("pt_late_t"), rr, &late)) { s.late_join = late; changed = true; }
    gw_disabled(false);
    if (changed && host) lobby_set_settings(&s);
    // Bottom: invite friends (Steam), the countdown.
    GuiRect inv = { in.x, in.y + in.h - TOK("mm_button_h"), in.w, TOK("mm_button_h") };
    bool steam_lobby = lobby_transport() == TRANSPORT_STEAM && lobby_steam_running();
    if (c.y + TOK("mm_gap") < inv.y) {
        GuiId iid = gui_id("pt_invite");
        if (gw_button(iid, inv, "Invite friends", GB_SECONDARY, steam_lobby && in_lobby_phase())) lobby_invite_overlay();
        if (!steam_lobby && gui_rect_contains(inv, gui_mouse().x, gui_mouse().y)) gui_tooltip(iid, "LAN party: friends join from Play > Join > LAN or by your IP address.");
    }
    if (L->state == LS_COUNTDOWN && L->countdown >= 0) {
        char cd[48];
        snprintf(cd, sizeof(cd), "Starting in %d", (int)ceilf(L->countdown));
        GuiText t = { gui_s(TH->t_title), GW_SEMIBOLD, TH->accent, GUI_CENTER, 0.04f, true, true, 0 };
        gd_text(&t, r.x + r.w * 0.5f, inv.y - gd_line_height(t.size) - TOK("mm_gap"), cd);
    }
    if (L->state == LS_CREATING || L->state == LS_JOINING) gw_label(gc_row(&c, TOK("mm_row_h")), L->state == LS_JOINING ? "Connecting..." : "Creating the party...", gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT);
    if (!lobby_active()) gw_label(gc_row(&c, TOK("mm_row_h")), mm_lobby_offline_text(), gui_s(TH->t_small), GW_REGULAR, TH->warning, GUI_LEFT);
}

static void draw_main(GuiRect content) {
    MmLayout *L = &mm()->play;
    const Lobby *lb = lobby_get();
    bool host = lobby_is_host(), guest = lobby_active() && !host;
    // "Leave party" only in someone else's party.
    for (int i = 0; i < L->count; i++) if (!strcmp(L->nodes[i].id, "leave")) L->nodes[i].hidden = !guest;
    mm_layout_solve(L, content);
    start_pending();
    int stack = gui_layout_find(L->nodes, L->count, "stack");
    float t = gui_anim_t(gui_id("mm_tab_in"));
    int k = 0;
    const Profile *prof = lobby_profile();
    const LobbyMember *me = lobby_local((Lobby *)lb);
    for (int i = 0; i < L->count; i++) {
        LayoutNode *n = &L->nodes[i];
        if (n->parent != stack || n->hidden) continue;
        float appear = saturate((t - (float)k * TH->menu_stagger) / fmaxf(0.01f, TH->menu_in));
        k++;
        if (!strcmp(n->id, "title")) { gd_alpha_push(gui_ease_out(appear)); mm_title(n->rect, n->label); gd_alpha_pop(); continue; }
        if (!n->action[0]) continue;
        bool enabled = true;
        const char *tip = NULL, *label = n->label;
        // Continue: the last story checkpoint when there is one (docs/STORY.md "Saves"), else the last session.
        static double story_checked = -10;
        static bool story_save;
        static char story_chapter[48];
        if (gui_time() - story_checked > 2.0) { story_checked = gui_time(); story_save = story_continue_available(story_chapter, sizeof(story_chapter)); }
        if (!strcmp(n->action, "continue")) { enabled = story_save || (prof->last_session[0] != 0 && host); tip = "No saved session yet"; }
        if (!strcmp(n->action, "start")) {
            // The host starts (or cancels the countdown); a guest readies up.
            if (host) {
                label = lb->state == LS_COUNTDOWN ? "Cancel start" : "Start game";
                enabled = in_lobby_phase();
                tip = "Creating the party...";
            } else {
                label = me && me->ready ? "Unready" : "Ready";
                enabled = in_lobby_phase() && me;
                tip = lobby_active() ? "Connecting..." : mm_lobby_offline_text();
            }
        }
        GuiId id = gui_id(n->id);
        if (gw_menu_item(id, n->rect, label, false, appear, enabled)) {
            if (!strcmp(n->action, "start")) {
                if (!host) lobby_set_ready(!(me && me->ready));
                else if (lb->state == LS_COUNTDOWN) lobby_cancel_start();
                else mm_play_start();
            } else if (!strcmp(n->action, "join")) { mm()->play_page = PP_BROWSER; P.source = steam() ? BS_STEAM : BS_LAN; refresh(P.source); gui_set_focus(0); }
            else if (!strcmp(n->action, "leave")) lobby_leave("left the party");   // the menu makes a new party of your own
            else if (!strcmp(n->action, "continue") && story_save) story_start(story_chapter, "auto");
            else if (!strcmp(n->action, "continue")) mm_play_start();
            else if (!strcmp(n->action, "story")) story_start("01_tenth_floor", NULL);
        }
        if (!enabled && tip && gui_rect_contains(n->rect, gui_mouse().x, gui_mouse().y)) gui_tooltip(id, tip);
        if (!gui_focus() && !strcmp(n->action, "start") && gui_nav_mode()) gui_set_focus(id);
    }
    draw_party(mm_node(L, "side"));
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
