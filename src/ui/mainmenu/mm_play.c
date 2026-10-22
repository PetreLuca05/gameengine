// Play tab (docs/MENU_LOBBY.md "Play"): two modes. Story: the Continue card (the last checkpoint, else
// chapter 1) and Chapter select (the story maps on the globe, mm_maps.c). Trials: you are always in a
// party - the menu creates your own lobby on start (Steam when it runs, else LAN), private by default
// (the lock) - Start game (host; a guest: Ready), Join (server browser: Steam lobbies, friends' games, LAN
// discovery, direct IP), Leave party (in someone else's); the party panel (the lock, the players, the
// map, the game settings) and the development maps bar.
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
    // The shackle: a ring whose lower half the body hides (open: lifted and slid right).
    float sx = locked ? cx : cx + bw * 0.3f, sy = body.y - (locked ? 0.0f : s * 0.14f);
    gd_ring(sx, sy, sr, t, 1.0f, c);
    if (!locked) gd_rect(gui_rect(sx - sr - t, sy, t * 2.0f, body.y - sy + t), TH->panel);   // the free leg's gap
    gd_round(body, gui_s(2), c);
    gd_circle(cx, body.y + bh * 0.45f, fmaxf(1.0f, s * 0.05f), TH->panel);
    if (in.hovered) gui_tooltip(id, locked ? "Private: only invited players (or by IP) can join. Click to make it public." : "Public: listed for everyone. Click to make it private.");
    return enabled && in.clicked;
}

// Invite friends: the Steam friends list (a direct Steam lobby invite: no overlay needed), opening under
// the Invite players button. Shift+click opens the Steam overlay's dialog instead.
static bool g_invite_open;

static void draw_invites(GuiRect anchor) {
    if (!g_invite_open) return;
    FriendEntry fr[64];
    int n = lobby_friends(fr, 64);
    float rh = TOK("lb_player_h");
    float top = anchor.y + anchor.h + TOK("mm_gap");
    float h = fminf((float)(n + 2) * rh, gui_h() - top - TOK("mm_gap_lg"));
    GuiRect r = { anchor.x, top, anchor.w, h };
    int layer = gd_layer();
    gd_set_layer(GL_OVERLAY);
    mm_panel(r);
    gui_block_rect(r);
    GuiRect in = gui_rect_inset(r, TOK("mm_gap"));
    GuiCursor c = gc_make(in, TOK("mm_gap_sm"));
    gw_section(gc_row(&c, TOK("mm_row_h")), "Invite friends");
    if (!n) gw_label(gc_row(&c, rh), "No Steam friends to show", gui_s(TH->t_body - 1), GW_REGULAR, TH->text_faint, GUI_LEFT);
    static double sent_at[64];
    for (int i = 0; i < n && c.y + rh <= in.y + in.h; i++) {
        GuiRect row = gc_row(&c, rh - TOK("mm_gap_sm"));
        u32 dot = fr[i].in_game ? TH->accent : (fr[i].state ? TH->ok : TH->text_faint);
        gd_circle(row.x + TOK("mm_gap"), row.y + row.h * 0.5f, gui_s(TH->radius), dot);
        gw_label(gui_rect(row.x + TOK("mm_gap") * 2.5f, row.y, row.w * 0.6f, row.h), fr[i].name, gui_s(TH->t_body - 1), GW_MEDIUM, fr[i].state ? TH->text : TH->text_faint, GUI_LEFT);
        GuiRect b = { row.x + row.w - TOK("mm_button_w"), row.y, TOK("mm_button_w"), row.h };
        bool sent = sent_at[i] > 0.0 && gui_time() - sent_at[i] < 4.0;
        if (gw_button(gui_idi("pt_inv", (int)(fr[i].id & 0x7FFFFFFF)), b, sent ? "Sent" : "Invite", GB_SECONDARY, fr[i].state != 0 && !sent)) {
            lobby_invite_friend(fr[i].id);
            sent_at[i] = gui_time();
        }
    }
    gui_focus_scope(r);
    if (gui_nav(NAV_BACK)) { gui_nav_consume(NAV_BACK); g_invite_open = false; }
    if (gui_mouse_pressed(0) && !gui_rect_contains(r, gui_mouse().x, gui_mouse().y) && !gui_rect_contains(anchor, gui_mouse().x, gui_mouse().y)) g_invite_open = false;
    gd_set_layer(layer);
}

// A crown (the party's host): three points over a band.
static void crown(float cx, float cy, float s, u32 c) {
    float w = s, h = s * 0.72f, x0 = cx - w * 0.5f, y1 = cy + h * 0.5f, y0 = cy - h * 0.5f;
    vec2 p[8] = {
        { x0, y1 }, { x0, y0 + h * 0.25f }, { x0 + w * 0.27f, y0 + h * 0.55f }, { cx, y0 },
        { x0 + w * 0.73f, y0 + h * 0.55f }, { x0 + w, y0 + h * 0.25f }, { x0 + w, y1 }, { x0, y1 },
    };
    gd_polyline(p, 8, fmaxf(1.5f, s * 0.12f), c, true);
    gd_rect(gui_rect(x0, y1 - s * 0.16f, w, s * 0.16f), c);
}

// Dashes along a rectangle's edges (the Invite players outline).
static void dotted_outline(GuiRect r, float dash, float gap, float t, u32 c) {
    for (float x = r.x; x < r.x + r.w; x += dash + gap) {
        float e = fminf(x + dash, r.x + r.w);
        gd_rect(gui_rect(x, r.y, e - x, t), c);
        gd_rect(gui_rect(x, r.y + r.h - t, e - x, t), c);
    }
    for (float y = r.y; y < r.y + r.h; y += dash + gap) {
        float e = fminf(y + dash, r.y + r.h);
        gd_rect(gui_rect(r.x, y, t, e - y), c);
        gd_rect(gui_rect(r.x + r.w - t, y, t, e - y), c);
    }
}

// The party (right, under the tab bar's divider): "Party n / max" + the lock, the players (a crown left
// of the host's icon; the host kicks on hover), Invite players +, the countdown.
static void draw_party(GuiRect r) {
    const Lobby *L = lobby_get();
    bool host = lobby_is_host() && in_lobby_phase();
    float ph = TOK("lb_player_h"), av = TOK("mm_avatar_sm"), gap = TOK("mm_gap");
    float y = r.y;
    // Header.
    GuiRect head = { r.x, y, r.w, TOK("mm_row_h") };
    char title[48];
    snprintf(title, sizeof(title), "Party  %d / %d", lobby_count(L), L->settings.max_players);
    gw_label_caps(head, title, gui_s(TH->t_label + 1), TH->text_dim, GUI_LEFT);
    bool locked = L->settings.privacy != LP_PUBLIC;
    float ls = head.h;
    gw_label(gui_rect(head.x, head.y, head.w - ls - gap, head.h), lobby_privacy_name(L->settings.privacy), gui_s(TH->t_small), GW_REGULAR, locked ? TH->accent : TH->text_dim, GUI_RIGHT);
    if (lock_button(gui_rect(head.x + head.w - ls, head.y, ls, ls), locked, host)) {
        LobbySettings s = L->settings;
        s.privacy = locked ? LP_PUBLIC : LP_INVITE;
        lobby_set_settings(&s);
        lobby_profile()->lobby.privacy = s.privacy;   // the next party starts the same
        lobby_profile_changed();
    }
    y += head.h + gap;
    // The players.
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) {
        const LobbyMember *m = &L->members[i];
        if (!m->active) continue;
        GuiRect row = { r.x, y, r.w, ph };
        y += ph + TOK("mm_gap_sm");
        GuiId id = gui_idi("pt_member", m->id);
        GuiInteract hi = gui_interact(id, row, GUI_NO_HOVER_SOUND);
        bool me = m->id == L->local_id;
        gd_backdrop(row, UI_RGBA(20, 22, 26, 150));
        if (hi.hot) gd_round(row, gui_s(TH->radius), TH->hover);
        float cs = av * 0.62f, x = row.x + gap;
        if (m->host) crown(x + cs * 0.5f, row.y + row.h * 0.5f, cs, TH->accent);
        x += cs + gap;
        gw_avatar(gui_rect(x, row.y + (row.h - av) * 0.5f, av, av), 0, m->name, mm_name_color(m->id));
        x += av + gap;
        GuiText nt = { gui_s(TH->t_body), me ? GW_SEMIBOLD : GW_MEDIUM, TH->text, GUI_LEFT, 0, false, true, row.x + row.w - x - TOK("mm_button_w") };
        gd_text(&nt, x, row.y + (row.h - gd_line_height(nt.size)) * 0.5f, m->name);
        // Right: ready (a dot) and the ping; the host kicks the others on hover.
        float rx = row.x + row.w - gap;
        if (host && !me && hi.hot) {
            GuiRect kb = { rx - TOK("mm_button_w") * 0.55f, row.y + gap * 0.5f, TOK("mm_button_w") * 0.55f, row.h - gap };
            if (gw_button(gui_idi("pt_kick", m->id), kb, "Kick", GB_DANGER, true)) lobby_kick(m->id);
            continue;
        }
        char ping[16];
        snprintf(ping, sizeof(ping), me && L->is_host ? "" : "%d ms", m->ping_ms);
        GuiText pt = { gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_RIGHT, 0, false, true, 0 };
        rx -= gd_text(&pt, rx, row.y + (row.h - gd_line_height(pt.size)) * 0.5f, ping) + gap;
        gd_circle(rx - gui_s(4), row.y + row.h * 0.5f, gui_s(4), m->ready ? TH->ok : TH->text_faint);
        if (gui_rect_contains(gui_rect(rx - gui_s(12), row.y, gui_s(14), row.h), gui_mouse().x, gui_mouse().y)) gui_tooltip(gui_idi("pt_ready", m->id), m->ready ? "Ready" : "Not ready");
    }
    if (L->state == LS_CREATING || L->state == LS_JOINING) { gw_label(gui_rect(r.x, y, r.w, ph), L->state == LS_JOINING ? "Connecting..." : "Creating the party...", gui_s(TH->t_small), GW_REGULAR, TH->text_faint, GUI_LEFT); y += ph; }
    if (!lobby_active()) { gw_label(gui_rect(r.x, y, r.w, ph), mm_lobby_offline_text(), gui_s(TH->t_small), GW_REGULAR, TH->warning, GUI_LEFT); y += ph; }
    // Invite players +: a blurred box, a dotted outline.
    GuiRect inv = { r.x, y + gap * 0.5f, r.w, ph };
    bool steam_lobby = lobby_transport() == TRANSPORT_STEAM && lobby_steam_running();
    bool can = steam_lobby && in_lobby_phase();
    GuiId iid = gui_id("pt_invite");
    GuiInteract ii = gui_interact(iid, inv, can ? GUI_FOCUSABLE : GUI_DISABLED);
    float hv = gui_tween(iid, ii.hot && can ? 1.0f : 0.0f, TH->hover_t);
    gd_backdrop(inv, UI_RGBA(20, 22, 26, 110));
    if (hv > 0.01f) gd_rect(inv, gui_rgba_mul(TH->hover, hv));
    dotted_outline(inv, gui_s(6), gui_s(4), fmaxf(1.0f, gui_s(1.2f)), gui_rgba_lerp(TH->text_faint, TH->accent, hv));
    GuiText it = { gui_s(TH->t_small + 1), GW_SEMIBOLD, can ? gui_rgba_lerp(TH->text_dim, TH->text, hv) : TH->text_faint, GUI_CENTER, TH->tracking_caps, true, false, 0 };
    gd_text(&it, inv.x + inv.w * 0.5f, inv.y + (inv.h - gd_line_height(it.size)) * 0.5f, "Invite players  +");
    if (ii.focused) gw_focus_ring(inv, gui_s(TH->radius), 1.0f);
    if (ii.clicked && can) {
        if (gui_shift()) lobby_invite_overlay();
        else g_invite_open = !g_invite_open;
    }
    if (!steam_lobby && ii.hovered) gui_tooltip(iid, "LAN party: friends join from Play > Join > LAN or by your IP address.");
    if (!can) g_invite_open = false;
    draw_invites(inv);
    if (L->state == LS_COUNTDOWN && L->countdown >= 0) {
        char cd[48];
        snprintf(cd, sizeof(cd), "Starting in %d", (int)ceilf(L->countdown));
        GuiText t = { gui_s(TH->t_title), GW_SEMIBOLD, TH->accent, GUI_RIGHT, 0.04f, true, true, 0 };
        gd_text(&t, r.x + r.w, inv.y + inv.h + gap * 2, cd);
    }
}

// Enroll: the big filled button (the divider's underline colour) at the bottom of the stack.
static bool enroll_button(GuiRect r, const char *label, bool enabled, float appear) {
    GuiId id = gui_id("mm_enroll");
    GuiInteract in = gui_interact(id, r, enabled ? GUI_FOCUSABLE : GUI_DISABLED);
    float h = gui_tween(id, in.hot && enabled ? 1.0f : 0.0f, TH->hover_t);
    gd_alpha_push(gui_ease_out(appear) * (enabled ? 1.0f : 0.45f));
    gd_round(r, gui_s(TH->radius), gui_rgba_lerp(TH->accent, UI_RGBA(255, 255, 255, 255), h * 0.15f));
    GuiText t = { gui_s(TH->t_body + 3), GW_SEMIBOLD, TH->text_dark, GUI_CENTER, TH->tracking_caps * 1.4f, true, false, 0 };
    gd_text(&t, r.x + r.w * 0.5f, r.y + (r.h - gd_line_height(t.size)) * 0.5f, label);
    gd_alpha_pop();
    if (in.focused) gw_focus_ring(r, gui_s(TH->radius), 1.0f);
    if (in.clicked && enabled) gui_sound(TH->snd_click);
    return in.clicked && enabled;
}

// A story save to continue (checked every 2 s): its chapter.
static bool story_saved(const char **chapter) {
    static double checked = -10;
    static bool saved;
    static char ch[48];
    if (gui_time() - checked > 2.0) { checked = gui_time(); saved = story_continue_available(ch, sizeof(ch)); }
    *chapter = ch;
    return saved;
}

// Story: the Continue card - a square button with the chapter's picture (the last checkpoint of the
// saved chapter). Shown only with a save; without one Story is a plain New game.
static void continue_card(GuiRect r, float appear) {
    const char *chapter;
    bool saved = story_saved(&chapter);
    const char *ch = saved ? chapter : "01_tenth_floor";
    const MapInfo *mi = NULL;
    const MapList *ml = lobby_maps();
    for (int i = 0; i < ml->count && !mi; i++) if (!strcmp(ml->maps[i].chapter, ch)) mi = &ml->maps[i];
    GuiId id = gui_id("mm_continue_card");
    GuiInteract in = gui_interact(id, r, GUI_FOCUSABLE);
    float h = gui_tween(id, in.hot || in.focused ? 1.0f : 0.0f, TH->hover_t);
    gd_alpha_push(gui_ease_out(appear));
    mm_panel(r);
    float pad = gui_s(TH->pad);
    GuiRect pic = { r.x + pad, r.y + pad, r.w - pad * 2, (r.w - pad * 2) / 1.7778f };
    mm_thumb(pic, mi ? mi->thumbnail : "");
    if (h > 0.01f) gd_round_outline(r, gui_s(TH->radius_panel), fmaxf(1.0f, gui_s(1.5f)), gui_rgba_mul(TH->accent, h));
    GuiText t = { gui_s(TH->t_title + 4), GW_SEMIBOLD, TH->text, GUI_LEFT, TH->tracking_caps, true, false, 0 };
    float y = pic.y + pic.h + pad;
    gd_text(&t, r.x + pad, y, saved ? "Continue" : "New game");
    y += gd_line_height(t.size) + gui_s(TH->gap * 0.5f);
    char sub[96];
    static char players_for[48];
    static int players = 1;
    if (strcmp(players_for, ch)) { dg_strcpy(players_for, sizeof(players_for), ch); players = story_chapter_players(ch); }
    snprintf(sub, sizeof(sub), "%s  ·  %s%s", mi ? mi->name : "Chapter 1", players > 1 ? "co-op" : "single player", saved ? "  ·  last checkpoint" : "");
    GuiText st = { gui_s(TH->t_small + 1), GW_REGULAR, TH->text_dim, GUI_LEFT, 0, false, true, r.w - pad * 2 };
    gd_text(&st, r.x + pad, y, sub);
    gd_rect(gui_rect(r.x, r.y + r.h - gui_s(TH->accent_bar), r.w * (0.25f + 0.75f * h), gui_s(TH->accent_bar)), TH->accent);
    gd_alpha_pop();
    if (in.focused) gw_focus_ring(r, gui_s(TH->radius_panel), 1.0f);
    if (in.clicked && lobby_is_host()) { gui_sound(TH->snd_click); mm_play_story(ch, saved ? "auto" : NULL); }
    if (in.hovered && !lobby_is_host()) gui_tooltip(id, "The party's host starts the chapter");
}

// A story chapter. Co-op ("players" > 1 in its JSON): for the party (the host) - its map, then the
// usual start (ready, start / the countdown); everyone in the party plays it. Single-player: alone, at
// once (the party closes: the chapter isn't reachable). Without a party: alone.
void mm_play_story(const char *chapter, const char *slot) {
    if (story_chapter_players(chapter) > 1 && lobby_active()) {
        if (lobby_is_host() && in_lobby_phase() && lobby_set_story(chapter, slot)) mm_play_start();
        return;
    }
    if (lobby_active()) lobby_leave("a single-player chapter");
    story_start(chapter, slot);
}

static void draw_main(GuiRect content) {
    MainMenu *M = mm();
    MmLayout *L = &M->play;
    const Lobby *lb = lobby_get();
    bool story = M->play_mode == PM_STORY;
    const char *saved_chapter;
    bool saved = story_saved(&saved_chapter);
    bool host = lobby_is_host(), guest = lobby_active() && !host;
    // Each mode's nodes; "Leave party" only in someone else's party.
    for (int i = 0; i < L->count; i++) {
        LayoutNode *n = &L->nodes[i];
        if (!strcmp(n->id, "card") || !strcmp(n->id, "chapters")) n->hidden = !story || !saved;
        if (!strcmp(n->id, "newgame")) n->hidden = !story || saved || guest;
        if (!strcmp(n->id, "join")) n->hidden = story;
        if (!strcmp(n->id, "start")) n->hidden = story && !guest;   // Story: the host starts with Continue / New game, a guest readies up
        if (!strcmp(n->id, "leave")) n->hidden = !guest;
    }
    mm_layout_solve(L, content);
    start_pending();
    int stack = gui_layout_find(L->nodes, L->count, "stack");
    float t = gui_anim_t(gui_id("mm_tab_in"));
    int k = 0;
    const LobbyMember *me = lobby_local((Lobby *)lb);
    {
        // Trials (the party, the development maps) | Story (the chapters, single player): centred under
        // the tab bar (gw_tabs lays out from its rect's left: the labels measured as it does).
        static const char *const modes[2] = { "Trials", "Story" };
        GuiRect mr = mm_node(L, "mode");
        GuiText tt = { gui_s(TH->t_small + 1), GW_SEMIBOLD, TH->text_dim, GUI_LEFT, TH->tracking_caps, true, false, 0 };
        float w = gd_text_width(&tt, modes[0]) + gd_text_width(&tt, modes[1]) + gui_s(26);
        mr = gui_rect(mr.x + (mr.w - w) * 0.5f, mr.y, w, mr.h);
        gd_alpha_push(gui_ease_out(saturate(t / fmaxf(0.01f, TH->menu_in))));
        int sel = gw_tabs(gui_id("mm_play_mode"), mr, modes, 2, story ? 1 : 0);
        gd_alpha_pop();
        if (sel != (story ? 1 : 0)) { mm_set_play_mode(sel == 1 ? PM_STORY : PM_TRIALS); gui_sound(TH->snd_tab); }
    }
    for (int i = 0; i < L->count; i++) {
        LayoutNode *n = &L->nodes[i];
        if (n->parent != stack || n->hidden) continue;
        float appear = saturate((t - (float)k * TH->menu_stagger) / fmaxf(0.01f, TH->menu_in));
        k++;
        if (!strcmp(n->id, "card")) { continue_card(n->rect, appear); continue; }
        if (!strcmp(n->id, "start")) {
            // Enroll (the host): the Trials maps, where Start starts the party's game (Cancel start
            // during the countdown); a guest readies up.
            if (host) {
                bool cd = lb->state == LS_COUNTDOWN;
                if (enroll_button(n->rect, cd ? "Cancel start" : "Enroll", in_lobby_phase(), appear)) {
                    if (cd) lobby_cancel_start();
                    else { M->play_page = PP_TRIALS; gui_anim_restart(gui_id("mm_tab_in")); gui_set_focus(0); }
                }
            } else if (enroll_button(n->rect, me && me->ready ? "Unready" : "Ready", in_lobby_phase() && me, appear)) {
                lobby_set_ready(!(me && me->ready));
            }
            continue;
        }
        if (!n->action[0]) continue;
        bool enabled = true;
        const char *tip = NULL, *label = n->label;
        GuiId id = gui_id(n->id);
        if (gw_menu_item(id, n->rect, label, false, appear, enabled)) {
            if (!strcmp(n->action, "join")) { M->play_page = PP_BROWSER; P.source = steam() ? BS_STEAM : BS_LAN; refresh(P.source); gui_set_focus(0); }
            else if (!strcmp(n->action, "leave")) lobby_leave("left the party");   // the menu makes a new party of your own
            else if (!strcmp(n->action, "newgame")) mm_play_story("01_tenth_floor", NULL);
            else if (!strcmp(n->action, "chapters")) { M->play_page = PP_CHAPTERS; gui_anim_restart(gui_id("mm_tab_in")); gui_set_focus(0); }
        }
        if (!enabled && tip && gui_rect_contains(n->rect, gui_mouse().x, gui_mouse().y)) gui_tooltip(id, tip);
        if (!gui_focus() && !strcmp(n->action, "start") && gui_nav_mode()) gui_set_focus(id);
    }
    draw_party(mm_node(L, "party"));
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
    else if (mm()->play_page == PP_CHAPTERS) mm_maps_draw(content, false);
    else if (mm()->play_page == PP_TRIALS) mm_maps_draw(content, true);
    else draw_main(content);
}
