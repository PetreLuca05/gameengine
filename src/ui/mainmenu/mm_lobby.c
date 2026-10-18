// Lobby screen (docs/MENU_LOBBY.md "Lobby"): players (avatar / name, ready, host crown, ping, mic
// stub, kick / promote for the host) | map + game settings (host edits, everyone else sees them) |
// chat; Ready, Start (host: everyone ready, or a 10 s countdown), Invite friends, Leave. The same chat
// backend serves the pause menu's Lobby page and the in-game chat (Enter).
#include "mm_internal.h"
#include "ui/menus.h"
#include "ui/screens/screens.h"
#include "world/season.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static char g_input[LOBBY_CHAT_MAX + 1];
static GuiScroll g_chat_scroll;
static bool g_invite_open;

static bool in_lobby_phase(void) { LobbyState s = lobby_state(); return s == LS_LOBBY || s == LS_COUNTDOWN; }

// ------------------------------------------------------------------ chat

// Lines carry their local receive time (platform clock): the wall clock then = now - age.
static void clock_text(double when, char *out, int size) {
    SDL_Time now;
    SDL_DateTime dt;
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now - (SDL_Time)((platform_time() - when) * 1e9), &dt, true))
        snprintf(out, (size_t)size, "%02d:%02d", dt.hour, dt.minute);
    else out[0] = 0;
}

bool mm_chat_panel(GuiRect r, bool input, bool keep_editing, float fade_after) {
    const Lobby *L = lobby_get();
    float ih = input ? TOK("lb_input_h") : 0;
    GuiRect log = { r.x, r.y, r.w, r.h - ih - (input ? TOK("mm_gap") : 0) };
    GuiRect view = gui_rect_inset(log, TOK("mm_gap"));
    view.w -= TOK("mm_gap");
    float line_h = gd_line_height(gui_s(TH->t_body - 1)) + gui_s(TH->gap * 0.25f);
    // Measure (wrapped) heights, newest at the bottom.
    GuiText tt = { gui_s(TH->t_body - 1), GW_REGULAR, TH->text, GUI_LEFT, 0, false, fade_after > 0, 0 };
    float total = 0;
    float hts[LOBBY_CHAT_LINES];
    char lines[LOBBY_CHAT_LINES][LOBBY_CHAT_MAX + 48];
    int count = L->chat_count;
    double now = platform_time();
    for (int i = 0; i < count; i++) {
        const LobbyChatLine *l = lobby_chat_at(L, i);
        char ts[8];
        clock_text(l->time, ts, sizeof(ts));
        if (l->from == LOBBY_SYSTEM) snprintf(lines[i], sizeof(lines[i]), "%s  %s", ts, l->text);
        else snprintf(lines[i], sizeof(lines[i]), "%s  %s: %s", ts, l->name, l->text);
        hts[i] = gd_text_wrap(&tt, 0, 0, view.w, lines[i], false) + gui_s(TH->gap * 0.25f);
        total += hts[i];
    }
    bool at_bottom = g_chat_scroll.target >= g_chat_scroll.content_h - view.h - line_h;
    gw_scroll_begin(&g_chat_scroll, gui_id("mm_chat_scroll"), view);
    float y = gw_scroll_y(&g_chat_scroll) + fmaxf(0.0f, view.h - total);
    for (int i = 0; i < count; i++) {
        const LobbyChatLine *l = lobby_chat_at(L, i);
        float a = 1.0f;
        if (fade_after > 0) a = saturate((float)(fade_after + 1.0 - (now - l->time)));
        if (a <= 0.01f) { y += hts[i]; continue; }
        gd_alpha_push(a);
        if (l->from == LOBBY_SYSTEM) {
            tt.color = l->kind == LC_ERROR ? TH->warning : TH->text_faint;
            gd_text_wrap(&tt, view.x, y, view.w, lines[i], true);
        } else {
            // The whole line wraps as measured; the time and the name are drawn over its start in
            // their colours (they are the line's first words, same font and size).
            tt.color = TH->text;
            gd_text_wrap(&tt, view.x, y, view.w, lines[i], true);
            char ts[8], pre[16];
            clock_text(l->time, ts, sizeof(ts));
            snprintf(pre, sizeof(pre), "%s  ", ts);
            GuiText p = tt;
            p.color = TH->text_faint;
            float x = view.x + gd_text(&p, view.x, y, ts) + gd_text_width(&p, pre) - gd_text_width(&p, ts);
            p.color = mm_name_color(l->color);
            char nm[LOBBY_NAME_MAX + 2];
            snprintf(nm, sizeof(nm), "%s:", l->name);
            gd_text(&p, x, y, nm);
        }
        gd_alpha_pop();
        y += hts[i];
    }
    gw_scroll_end(&g_chat_scroll, total);
    if (at_bottom) g_chat_scroll.target = fmaxf(0.0f, total - view.h);   // follow new lines
    if (!input) return false;
    GuiRect ir = { r.x, r.y + r.h - ih, r.w, ih };
    GuiId fid = gui_id("mm_chat_input");
    bool sent = false;
    if (gw_textfield(fid, ir, g_input, (int)sizeof(g_input), "Say something (Enter)  ·  /ready /map /kick")) {
        if (g_input[0]) { lobby_chat(g_input); sent = true; }
        g_input[0] = 0;
        g_chat_scroll.target = 1e9f;
        if (keep_editing && sent) gw_textfield_activate(fid);   // lobby: keep typing
    }
    return sent;
}

// ------------------------------------------------------------------ players

static void draw_players(GuiRect r, bool actions) {
    const Lobby *L = lobby_get();
    bool host = lobby_is_host();
    mm_panel(r);
    GuiRect in = gui_rect_inset(r, gui_s(TH->pad));
    GuiCursor c = gc_make(in, TOK("mm_gap_sm"));
    char title[48];
    snprintf(title, sizeof(title), "Players  %d / %d", lobby_count(L), L->settings.max_players);
    gw_section(gc_row(&c, TOK("mm_row_h")), title);
    float ph = TOK("lb_player_h"), av = TOK("mm_avatar_sm");
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) {
        const LobbyMember *m = &L->members[i];
        if (!m->active) continue;
        GuiRect row = gc_row(&c, ph);
        GuiId id = gui_idi("mm_member", m->id);
        GuiInteract hi = gui_interact(id, row, GUI_FOCUSABLE | GUI_NO_HOVER_SOUND);
        bool me = m->id == L->local_id;
        if (hi.hot) gd_round(row, gui_s(TH->radius), TH->hover);
        if (hi.focused) gw_focus_ring(row, gui_s(TH->radius), 1.0f);
        gw_avatar(gui_rect(row.x + TOK("mm_gap"), row.y + (row.h - av) * 0.5f, av, av), 0, m->name, mm_name_color(m->id));
        float x = row.x + TOK("mm_gap") * 2 + av;
        GuiText nt = { gui_s(TH->t_body), me ? GW_SEMIBOLD : GW_MEDIUM, mm_name_color(m->id), GUI_LEFT, 0, false, false, row.w * 0.45f };
        float nw = gd_text(&nt, x, row.y + (row.h - gd_line_height(nt.size)) * 0.5f, m->name);
        x += nw + TOK("mm_gap");
        if (m->host) { gw_label_caps(gui_rect(x, row.y, row.w, row.h), "Host", gui_s(TH->t_tiny), TH->accent, GUI_LEFT); }
        // Right side: ready, ping, mic (stub).
        float rx = row.x + row.w - TOK("mm_gap");
        char ping[16];
        snprintf(ping, sizeof(ping), m->host && !me ? "host" : "%d ms", m->ping_ms);
        if (me && L->is_host) snprintf(ping, sizeof(ping), "-");
        GuiText pt = { gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT, 0, false, false, 0 };
        rx -= gd_text(&pt, rx, row.y + (row.h - gd_line_height(pt.size)) * 0.5f, ping) + TOK("mm_gap");
        float ic = TOK("lb_icon");
        gd_ring(rx - ic * 0.5f, row.y + row.h * 0.5f, ic * 0.35f, fmaxf(1.0f, gui_s(1.5f)), 1.0f, m->talking ? TH->ok : TH->text_faint);   // mic (stub)
        rx -= ic + TOK("mm_gap");
        GuiText rt = { gui_s(TH->t_label), GW_SEMIBOLD, m->ready ? TH->ok : TH->text_faint, GUI_RIGHT, TH->tracking_caps, true, false, 0 };
        gd_text(&rt, rx, row.y + (row.h - gd_line_height(rt.size)) * 0.5f, m->ready ? "Ready" : "Not ready");
        // Host actions on the others: kick, promote (on hover / focus).
        if (actions && host && !me && (hi.hot || gui_focus() == gui_idi("mm_kick", m->id) || gui_focus() == gui_idi("mm_promote", m->id))) {
            float bw = TOK("mm_button_w") * 0.62f, bh = row.h - TOK("mm_gap");
            GuiRect kb = { row.x + row.w - bw * 2 - TOK("mm_gap") * 2, row.y + TOK("mm_gap") * 0.5f, bw, bh };
            GuiRect pb = { kb.x + bw + TOK("mm_gap"), kb.y, bw, bh };
            gd_round(gui_rect(kb.x - TOK("mm_gap"), row.y, row.x + row.w - kb.x + TOK("mm_gap"), row.h), gui_s(TH->radius), TH->panel_hi);
            if (gw_button(gui_idi("mm_kick", m->id), kb, "Kick", GB_DANGER, true)) lobby_kick(m->id);
            if (gw_button(gui_idi("mm_promote", m->id), pb, "Make host", GB_GHOST, in_lobby_phase())) lobby_promote(m->id);
        }
    }
    if (L->state == LS_JOINING || L->state == LS_CREATING) gw_label(gc_row(&c, ph), L->state == LS_JOINING ? "Connecting..." : "Creating the lobby...", gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT);
    if (L->state == LS_MIGRATING) gw_label(gc_row(&c, ph), "The host left: moving to the new host...", gui_s(TH->t_body - 1), GW_REGULAR, TH->warning, GUI_LEFT);
}

// ------------------------------------------------------------------ centre: map + settings

static void draw_center(GuiRect r) {
    const Lobby *L = lobby_get();
    bool host = lobby_is_host() && in_lobby_phase();
    mm_panel(r);
    GuiRect in = gui_rect_inset(r, gui_s(TH->pad));
    GuiCursor c = gc_make(in, TOK("mm_gap_sm"));
    const MapInfo *mi = lobby_map_info(L->settings.map);
    // Map: thumbnail beside name + selector.
    float th = fminf(in.w * 0.42f / 1.7778f, TOK("mm_map_row_h") * 2);
    GuiRect top = gc_row(&c, th);
    mm_thumb(gui_rect(top.x, top.y, th * 1.7778f, th), mi ? mi->thumbnail : "");
    GuiRect mr = { top.x + th * 1.7778f + TOK("mm_gap_lg"), top.y, top.w - th * 1.7778f - TOK("mm_gap_lg"), th };
    gw_label(gui_rect(mr.x, mr.y, mr.w, TOK("mm_title_h")), mi ? mi->name : L->settings.map, gui_s(TH->t_title - 2), GW_SEMIBOLD, TH->text, GUI_LEFT);
    if (mi) {
        char sub[96], size[32];
        maplist_size_text(mi, size, sizeof(size));
        snprintf(sub, sizeof(sub), "%s  ·  %s", mi->biome, size);
        gw_label(gui_rect(mr.x, mr.y + TOK("mm_title_h"), mr.w, TOK("mm_row_h")), sub, gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT);
    }
    GuiRect sel = { mr.x, mr.y + mr.h - TOK("mm_button_h"), mr.w, TOK("mm_button_h") };
    const MapList *ml = lobby_maps();
    if (host) {
        const char *names[MAPLIST_MAX];
        int cur = 0;
        for (int i = 0; i < ml->count; i++) { names[i] = ml->maps[i].name; if (!strcmp(ml->maps[i].id, L->settings.map)) cur = i; }
        if (gw_dropdown(gui_id("lb_map"), sel, names, ml->count, &cur)) {
            // The map's own defaults (season, time, weather) come with it.
            LobbySettings s = L->settings;
            const MapInfo *m = &ml->maps[cur];
            dg_strcpy(s.map, sizeof(s.map), m->id);
            s.season = (u8)m->season;
            s.start_hour = (u8)m->hour;
            dg_strcpy(s.weather, sizeof(s.weather), m->weather);
            lobby_set_settings(&s);
        }
    } else if (gw_button(gui_id("lb_vote"), sel, "Vote for this map", GB_GHOST, in_lobby_phase())) {
        char v[64];
        snprintf(v, sizeof(v), "votes for %s", mi ? mi->name : L->settings.map);   // vote stub: a chat line
        lobby_chat(v);
    }
    c.y += TOK("mm_gap");
    gw_section(gc_row(&c, TOK("mm_row_h")), host ? "Game settings" : "Game settings (host)");
    LobbySettings s = L->settings;
    bool changed = false;
    gw_disabled(!host);
    const char *diff[LD_COUNT];
    for (int i = 0; i < LD_COUNT; i++) diff[i] = lobby_difficulty_name(i);
    int d = s.difficulty;
    GuiRect rr = gw_setting_row(&c, "Difficulty", NULL, gui_id("lb_diff"));
    if (gw_dropdown(gui_id("lb_diff_dd"), rr, diff, LD_COUNT, &d)) { s.difficulty = (u8)d; changed = true; }
    float mp = (float)s.max_players;
    rr = gw_setting_row(&c, "Max players", NULL, gui_id("lb_max"));
    if (gw_slider(gui_id("lb_max_s"), rr, &mp, 1, LOBBY_MAX_MEMBERS, 1, "%.0f")) { s.max_players = (u8)mp; changed = true; }
    const char *priv[LP_COUNT];
    bool pen[LP_COUNT];
    const char *pnote[LP_COUNT];
    bool lan = lobby_transport() == TRANSPORT_LAN;
    for (int i = 0; i < LP_COUNT; i++) { priv[i] = lobby_privacy_name(i); pen[i] = !(lan && i == LP_FRIENDS); pnote[i] = pen[i] ? "" : "Steam only"; }
    int p = s.privacy;
    rr = gw_setting_row(&c, "Privacy", "Public: in the browser. Friends only: Steam friends. Invite only: hidden, invites / direct IP.", gui_id("lb_priv"));
    if (gw_dropdown_ex(gui_id("lb_priv_dd"), rr, priv, LP_COUNT, &p, pen, pnote)) { s.privacy = (u8)p; changed = true; }
    bool late = s.late_join, pvp = s.pvp;
    rr = gw_setting_row(&c, "Join a running game", "Players may join after the start (they load in and spawn).", gui_id("lb_late"));
    if (gw_toggle(gui_id("lb_late_t"), rr, &late)) { s.late_join = late; changed = true; }
    rr = gw_setting_row(&c, "Player damage (PvP)", "Stub: not wired into damage yet.", gui_id("lb_pvp"));
    if (gw_toggle(gui_id("lb_pvp_t"), rr, &pvp)) { s.pvp = pvp; changed = true; }
    // Server rule (docs/SETTINGS_AUDIT.md): the same foliage density for every player in the session.
    float fp = (float)(s.foliage_pct ? s.foliage_pct : 100);
    rr = gw_setting_row(&c, "Foliage density", "A server rule: grass and plant density for every player in this game (the same for everyone, whatever their graphics settings).", gui_id("lb_foliage"));
    if (gw_slider(gui_id("lb_foliage_s"), rr, &fp, 50, 100, 5, "%.0f%%")) { s.foliage_pct = (u8)fp; changed = true; }
    gw_disabled(false);
    // Season / start time / start weather / dynamic weather (the Maps tab's rows).
    if (mm_environment_rows(&c, &s, host)) changed = true;
    if (changed && host) lobby_set_settings(&s);
    if (L->state == LS_COUNTDOWN && L->countdown >= 0) {
        char cd[48];
        snprintf(cd, sizeof(cd), "Starting in %d", (int)ceilf(L->countdown));
        GuiText t = { gui_s(TH->t_large), GW_SEMIBOLD, TH->accent, GUI_CENTER, 0.04f, true, true, 0 };
        gd_text(&t, r.x + r.w * 0.5f, r.y + r.h - gd_line_height(t.size) - gui_s(TH->pad), cd);
    }
}

// ------------------------------------------------------------------ invite popup

static void draw_invites(GuiRect anchor) {
    if (!g_invite_open) return;
    FriendEntry fr[64];
    int n = lobby_friends(fr, 64);
    float w = TOK("lb_chat_w"), rh = TOK("lb_player_h");
    float h = fminf((float)(n + 2) * rh, gui_h() * 0.5f);
    GuiRect r = { anchor.x, anchor.y - h - TOK("mm_gap"), w, h };
    int layer = gd_layer();
    gd_set_layer(GL_OVERLAY);
    mm_panel(r);
    GuiRect in = gui_rect_inset(r, TOK("mm_gap"));
    GuiCursor c = gc_make(in, TOK("mm_gap_sm"));
    gw_section(gc_row(&c, TOK("mm_row_h")), "Invite friends");
    if (!n) gw_label(gc_row(&c, rh), "No friends to show", gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT);
    for (int i = 0; i < n && c.y + rh <= in.y + in.h; i++) {
        GuiRect row = gc_row(&c, rh - TOK("mm_gap_sm"));
        u32 dot = fr[i].in_game ? TH->accent : (fr[i].state ? TH->ok : TH->text_faint);
        gd_circle(row.x + TOK("mm_gap"), row.y + row.h * 0.5f, gui_s(TH->radius), dot);
        gw_label(gui_rect(row.x + TOK("mm_gap") * 2.5f, row.y, row.w * 0.6f, row.h), fr[i].name, gui_s(TH->t_body - 1), GW_MEDIUM, fr[i].state ? TH->text : TH->text_faint, GUI_LEFT);
        GuiRect b = { row.x + row.w - TOK("mm_button_w"), row.y, TOK("mm_button_w"), row.h };
        if (gw_button(gui_idi("lb_inv", (int)(fr[i].id & 0x7FFFFFFF)), b, "Invite", GB_SECONDARY, fr[i].state != 0)) lobby_invite_friend(fr[i].id);
    }
    gui_focus_scope(r);
    if (gui_nav(NAV_BACK)) { gui_nav_consume(NAV_BACK); g_invite_open = false; }
    if (gui_mouse_pressed(0) && !gui_rect_contains(r, gui_mouse().x, gui_mouse().y) && !gui_rect_contains(anchor, gui_mouse().x, gui_mouse().y)) g_invite_open = false;
    gd_set_layer(layer);
}

// ------------------------------------------------------------------ screen

void mm_lobby_draw(GuiRect content) {
    const Lobby *L = lobby_get();
    MmLayout *Ly = &mm()->lobby;
    mm_layout_solve(Ly, content);
    bool host = lobby_is_host();
    draw_players(mm_node(Ly, "players"), true);
    draw_center(mm_node(Ly, "center"));
    GuiRect chat = mm_node(Ly, "chat");
    mm_panel(chat);
    mm_chat_panel(gui_rect_inset(chat, TOK("mm_gap")), true, true, 0);
    // Action bar.
    const LobbyMember *me = lobby_local((Lobby *)L);
    bool ready = me && me->ready;
    bool phase = in_lobby_phase();
    if (gw_button(gui_id("lb_ready"), mm_node(Ly, "ready"), ready ? "Unready" : "Ready", ready ? GB_SECONDARY : GB_PRIMARY, phase && me)) lobby_set_ready(!ready);
    if (host) {
        bool all = lobby_all_ready(L);
        if (L->state == LS_COUNTDOWN) {
            if (gw_button(gui_id("lb_start"), mm_node(Ly, "start"), all ? "Start now" : "Cancel start", all ? GB_PRIMARY : GB_DANGER, true)) { if (all) lobby_start(false); else lobby_cancel_start(); }
        } else {
            GuiId sid = gui_id("lb_start");
            if (gw_button(sid, mm_node(Ly, "start"), all ? "Start" : "Start in 10 s", all ? GB_PRIMARY : GB_SECONDARY, phase)) lobby_start(!all);
            if (!all && gui_rect_contains(mm_node(Ly, "start"), gui_mouse().x, gui_mouse().y)) gui_tooltip(sid, "Not everyone is ready: starts after a 10 second countdown (cancel any time).");
        }
    } else {
        gw_label(mm_node(Ly, "start"), "Waiting for the host", gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_CENTER);
    }
    GuiRect inv = mm_node(Ly, "invite");
    bool steam = lobby_transport() == TRANSPORT_STEAM && lobby_steam_running();
    GuiId iid = gui_id("lb_invite");
    if (gw_button(iid, inv, "Invite friends", GB_SECONDARY, steam)) {
        // The in-game list when Steam gave us friends; the overlay otherwise (or as well, Shift).
        if (gui_shift()) lobby_invite_overlay();
        else g_invite_open = !g_invite_open;
    }
    if (!steam && gui_rect_contains(inv, gui_mouse().x, gui_mouse().y)) gui_tooltip(iid, "LAN lobby: friends join from Play > Join > LAN or by your IP address.");
    // Status line.
    int ready_n = 0, n = 0;
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (L->members[i].active) { n++; if (L->members[i].ready) ready_n++; }
    char st[96];
    if (L->state == LS_COUNTDOWN) snprintf(st, sizeof(st), "Starting in %d s  ·  %d / %d ready", (int)ceilf(fmaxf(0.0f, L->countdown)), ready_n, n);
    else snprintf(st, sizeof(st), "%d / %d ready  ·  %s  ·  %s", ready_n, n, lobby_transport() == TRANSPORT_STEAM ? "Steam" : "LAN", lobby_privacy_name(L->settings.privacy));
    gw_label(mm_node(Ly, "status"), st, gui_s(TH->t_small), GW_REGULAR, TH->text_dim, GUI_CENTER);
    if (gw_button(gui_id("lb_leave"), mm_node(Ly, "leave"), "Leave", GB_DANGER, true)) lobby_leave("left the lobby");
    draw_invites(inv);
}

// ------------------------------------------------------------------ pause > Lobby, in-game chat

bool mainmenu_lobby_page_draw(GuiRect r) {
    char sub[64];
    const Lobby *L = lobby_get();
    snprintf(sub, sizeof(sub), "%d / %d players", lobby_count(L), L->settings.max_players);
    GuiRect c = screens_modal_frame(r, "Lobby", sub);
    GuiRect a, b;
    gui_split_h(c, 0.48f, TOK("mm_gap_lg"), &a, &b);
    a.h -= TOK("mm_button_h") + TOK("mm_gap");
    draw_players(a, true);
    GuiRect inv = { a.x, a.y + a.h + TOK("mm_gap"), a.w, TOK("mm_button_h") };
    bool steam = lobby_transport() == TRANSPORT_STEAM && lobby_steam_running();
    if (gw_button(gui_id("pl_invite"), inv, "Invite friends", GB_SECONDARY, steam)) lobby_invite_overlay();
    mm_chat_panel(b, true, true, 0);
    screens_footer_hint(gui_rect(c.x, c.y + c.h - gui_s(18), c.w, gui_s(18)), "Esc / B  back");
    return false;
}

void mainmenu_chat_open(void) {
    if (!screens_has(SCR_CHAT)) screens_push(SCR_CHAT);
    gw_textfield_activate(gui_id("mm_chat_input"));
    gui_nav_consume(NAV_ACCEPT);   // the Enter that opened the line is not a send
}

bool mainmenu_chat_draw(bool top) {
    MmLayout *Ly = &mm()->chat;
    mm_layout_solve(Ly, gui_rect(0, 0, gui_w(), gui_h()));
    GuiRect r = mm_node(Ly, "chat");
    gd_rect_grad(gui_rect(r.x, r.y, r.w, r.h), UI_RGBA(0, 0, 0, 0), gui_rgba_mul(TH->hud_panel, 1.0f));
    gui_push_input(top);
    bool sent = mm_chat_panel(r, true, false, 0);
    bool editing = gw_textfield_editing(gui_id("mm_chat_input"));
    gui_pop_input();
    // Enter sends and closes; Esc (the field stops editing) closes.
    return top && (sent || !editing);
}

void mainmenu_chat_feed_draw(void) {
    if (!lobby_active() || lobby_get()->chat_count == 0) return;
    MmLayout *Ly = &mm()->chat;
    mm_layout_solve(Ly, gui_rect(0, 0, gui_w(), gui_h()));
    GuiRect r = mm_node(Ly, "log");
    gui_push_input(false);
    mm_chat_panel(r, false, false, gui_tok("chat_fade"));
    gui_pop_input();
}
