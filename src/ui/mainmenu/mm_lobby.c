// Lobby parts (docs/MENU_LOBBY.md "Party"): the player rows (avatar / name, ready, host, ping, mic stub,
// phone icon, kick / promote for the host) for the Play tab's party panel and the pause menu's Lobby
// page; the chat panel (the pause menu's Lobby page and the in-game chat, Enter).
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

// framed: its own panel and "Players n / max" title (the pause page); else bare rows in r.
static void draw_players(GuiRect r, bool actions, bool framed) {
    const Lobby *L = lobby_get();
    bool host = lobby_is_host();
    if (framed) mm_panel(r);
    GuiRect in = framed ? gui_rect_inset(r, gui_s(TH->pad)) : r;
    GuiCursor c = gc_make(in, TOK("mm_gap_sm"));
    char title[48];
    snprintf(title, sizeof(title), "Players  %d / %d", lobby_count(L), L->settings.max_players);
    if (framed) gw_section(gc_row(&c, TOK("mm_row_h")), title);
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
        if (m->phone) {   // a paired real phone (docs/PHONE.md)
            GuiRect pr = { rx - ic, row.y + (row.h - ic) * 0.5f, ic, ic };
            mm_phone_glyph(pr, TH->ok, true);
            if (gui_rect_contains(pr, gui_mouse().x, gui_mouse().y)) gui_tooltip(gui_idi("mm_member_phone", m->id), "Playing with a real phone");
            rx -= ic + TOK("mm_gap");
        }
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

void mm_lobby_players(GuiRect r) { draw_players(r, true, false); }

// ------------------------------------------------------------------ pause > Lobby, in-game chat

bool mainmenu_lobby_page_draw(GuiRect r) {
    char sub[64];
    const Lobby *L = lobby_get();
    snprintf(sub, sizeof(sub), "%d / %d players", lobby_count(L), L->settings.max_players);
    GuiRect c = screens_modal_frame(r, "Lobby", sub);
    // The lock (private / public): the host changes it in game too.
    {
        bool locked = L->settings.privacy != LP_PUBLIC;
        float ls = TOK("mm_row_h");
        GuiRect lr = { c.x, c.y, ls, ls };
        if (mm_lock_button(lr, locked, lobby_is_host())) mm_toggle_privacy();
        gw_label(gui_rect(lr.x + ls + TOK("mm_gap"), lr.y, c.w * 0.5f, ls), locked ? "Private: invite only" : "Public: anyone can join", gui_s(TH->t_small + 1), GW_MEDIUM, locked ? TH->accent : TH->text_dim, GUI_LEFT);
        c.y += ls + TOK("mm_gap");
        c.h -= ls + TOK("mm_gap");
    }
    GuiRect a, b;
    gui_split_h(c, 0.48f, TOK("mm_gap_lg"), &a, &b);
    a.h -= TOK("mm_button_h") + TOK("mm_gap");
    draw_players(a, true, true);
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
