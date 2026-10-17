// Main menu + lobby (docs/MENU_LOBBY.md): the lobby state machine (every event from every state),
// host-authoritative membership (join, full, kick + rejoin blocked, late join), ready / start gating
// and the forced-start countdown, chat (rate limit, 256 character cap, slash commands, profanity
// stub), version mismatch rejection, migration successor, the wire round trips, Steam metadata /
// rich presence, map JSON validation, cosmetics equip round trip to the player character, the
// profile, seasons + the season-biased weather scheduler, and the main menu screen lint (theme
// tokens only, every layout solved at 16:9 / 21:9 / 4:3). Run from the build output directory.
#include "net/lobby.h"
#include "world/maplist.h"
#include "world/season.h"
#include "world/weather.h"
#include "gameplay/cosmetics.h"
#include "game/profile.h"
#include "ui/gui/gui_layout.h"
#include "core/engine.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

// ------------------------------------------------------------------ state machine

// The transitions restated independently of src/net/lobby.c; every other (state, event) pair must
// be ignored (the state stays).
typedef struct Tr { LobbyState s; LobbyEvent e; LobbyState to; } Tr;
static const Tr k_expected[] = {
    { LS_IDLE, LE_HOST, LS_CREATING }, { LS_IDLE, LE_JOIN, LS_JOINING },
    { LS_CREATING, LE_TRANSPORT_UP, LS_LOBBY }, { LS_CREATING, LE_TRANSPORT_FAIL, LS_CLOSED }, { LS_CREATING, LE_LEAVE, LS_IDLE },
    { LS_JOINING, LE_SNAPSHOT, LS_LOBBY }, { LS_JOINING, LE_REJECTED, LS_CLOSED }, { LS_JOINING, LE_TRANSPORT_FAIL, LS_CLOSED },
    { LS_JOINING, LE_KICKED, LS_CLOSED }, { LS_JOINING, LE_HOST_LOST, LS_CLOSED }, { LS_JOINING, LE_LEAVE, LS_IDLE }, { LS_JOINING, LE_START, LS_LOADING },
    { LS_LOBBY, LE_START, LS_LOADING }, { LS_LOBBY, LE_FORCE_START, LS_COUNTDOWN }, { LS_LOBBY, LE_HOST_LOST, LS_MIGRATING },
    { LS_LOBBY, LE_KICKED, LS_CLOSED }, { LS_LOBBY, LE_LEAVE, LS_IDLE }, { LS_LOBBY, LE_TRANSPORT_FAIL, LS_CLOSED },
    { LS_COUNTDOWN, LE_COUNTDOWN_DONE, LS_LOADING }, { LS_COUNTDOWN, LE_START, LS_LOADING }, { LS_COUNTDOWN, LE_COUNTDOWN_CANCEL, LS_LOBBY },
    { LS_COUNTDOWN, LE_HOST_LOST, LS_MIGRATING }, { LS_COUNTDOWN, LE_KICKED, LS_CLOSED }, { LS_COUNTDOWN, LE_LEAVE, LS_IDLE },
    { LS_COUNTDOWN, LE_TRANSPORT_FAIL, LS_CLOSED },
    { LS_LOADING, LE_LOAD_DONE, LS_IN_GAME }, { LS_LOADING, LE_HOST_LOST, LS_CLOSED }, { LS_LOADING, LE_KICKED, LS_CLOSED },
    { LS_LOADING, LE_LEAVE, LS_IDLE }, { LS_LOADING, LE_TRANSPORT_FAIL, LS_CLOSED },
    { LS_IN_GAME, LE_HOST_LOST, LS_CLOSED }, { LS_IN_GAME, LE_KICKED, LS_CLOSED }, { LS_IN_GAME, LE_LEAVE, LS_IDLE }, { LS_IN_GAME, LE_TRANSPORT_FAIL, LS_CLOSED },
    { LS_MIGRATING, LE_MIGRATED, LS_LOBBY }, { LS_MIGRATING, LE_SNAPSHOT, LS_LOBBY }, { LS_MIGRATING, LE_TRANSPORT_FAIL, LS_CLOSED },
    { LS_MIGRATING, LE_HOST_LOST, LS_CLOSED }, { LS_MIGRATING, LE_REJECTED, LS_CLOSED }, { LS_MIGRATING, LE_KICKED, LS_CLOSED }, { LS_MIGRATING, LE_LEAVE, LS_IDLE },
    { LS_CLOSED, LE_ACK, LS_IDLE }, { LS_CLOSED, LE_LEAVE, LS_IDLE },
};

static void test_fsm(void) {
    int cells = 0, moves = 0;
    for (int s = 0; s < LS_COUNT; s++) {
        for (int e = 0; e < LE_COUNT; e++) {
            LobbyState want = (LobbyState)s;
            bool listed = false;
            for (int i = 0; i < ARRAY_COUNT(k_expected); i++)
                if (k_expected[i].s == (LobbyState)s && k_expected[i].e == (LobbyEvent)e) { want = k_expected[i].to; listed = true; }
            LobbyState got = lobby_fsm_next((LobbyState)s, (LobbyEvent)e);
            CHECK(got == want, "fsm %s + %s -> %s (expected %s)", lobby_state_name(s), lobby_event_name(e), lobby_state_name(got), lobby_state_name(want));
            CHECK(lobby_fsm_accepts((LobbyState)s, (LobbyEvent)e) == listed, "fsm %s + %s accepted = %d", lobby_state_name(s), lobby_event_name(e), listed);
            cells++;
            if (got != (LobbyState)s) moves++;
        }
    }
    CHECK(cells == LS_COUNT * LE_COUNT, "every cell visited");
    // Every state is reachable from IDLE and every session state can get back to IDLE.
    bool reach[LS_COUNT] = { false };
    reach[LS_IDLE] = true;
    for (int it = 0; it < LS_COUNT; it++)
        for (int s = 0; s < LS_COUNT; s++) if (reach[s]) for (int e = 0; e < LE_COUNT; e++) reach[lobby_fsm_next((LobbyState)s, (LobbyEvent)e)] = true;
    for (int s = 0; s < LS_COUNT; s++) CHECK(reach[s], "state %s reachable", lobby_state_name(s));
    for (int s = 0; s < LS_COUNT; s++) {
        LobbyState x = lobby_fsm_next((LobbyState)s, LE_LEAVE);
        CHECK(s == LS_IDLE ? x == LS_IDLE : x == LS_IDLE, "leave from %s returns to idle", lobby_state_name(s));
    }
    CHECK(!lobby_state_in_session(LS_IDLE) && !lobby_state_in_session(LS_CLOSED) && lobby_state_in_session(LS_IN_GAME), "in-session states");
    CHECK(lobby_fsm_next(LS_COUNT, LE_HOST) == LS_COUNT, "out of range state ignored");
    printf("fsm: %d cells, %d transitions\n", cells, moves);
}

// ------------------------------------------------------------------ host model

static bool map_ok(const char *map, void *user) { (void)user; return !strcmp(map, "devroom") || !strcmp(map, "zombieland"); }

static Lobby *host_lobby(void) {
    static Lobby L;
    lobby_init(&L);
    L.state = LS_LOBBY;
    LobbySettings s;
    lobby_settings_default(&s);
    s.max_players = 4;
    lobby_host_begin(&L, "Host", 76561198000000001ull, "steam:1", "76561198000000001", &s);
    return &L;
}

static LobbyIntent intent(int type, int a, const char *text) {
    LobbyIntent in;
    memset(&in, 0, sizeof(in));
    in.type = (u8)type;
    in.a = (u8)a;
    if (text) dg_strcpy(in.text, sizeof(in.text), text);
    return in;
}

static void test_membership(void) {
    Lobby *L = host_lobby();
    char why[96];
    CHECK(lobby_count(L) == 1 && L->members[0].host && L->local_id == 0, "host is member 0");
    u8 a = lobby_host_add(L, "Alice", 0, "lan:10.0.0.2/Alice", "10.0.0.2:27015", 1, 1.0, why, sizeof(why));
    u8 b = lobby_host_add(L, "Bob", 0, "lan:10.0.0.3/Bob", "10.0.0.3:27015", 2, 1.0, why, sizeof(why));
    u8 c = lobby_host_add(L, "Alice", 0, "lan:10.0.0.4/Alice", "10.0.0.4:27015", 3, 1.0, why, sizeof(why));
    CHECK(a != LOBBY_NONE && b != LOBBY_NONE && c != LOBBY_NONE && lobby_count(L) == 4, "three join");
    CHECK(strcmp(lobby_member(L, c)->name, "Alice") != 0, "duplicate name numbered (%s)", lobby_member(L, c)->name);
    u8 d = lobby_host_add(L, "Dan", 0, "lan:10.0.0.5/Dan", "", 4, 1.0, why, sizeof(why));
    CHECK(d == LOBBY_NONE && strstr(why, "full"), "fifth rejected: lobby full (%s)", why);
    CHECK(lobby_member_by_peer(L, 2) == lobby_member(L, b), "member by peer");
    CHECK(lobby_member_by_name(L, "bob") == lobby_member(L, b), "member by name (case)");
    CHECK(lobby_member_by_name(L, "Ali") == NULL, "ambiguous prefix -> none");

    // Kick: host only, removed, banned, rejoin blocked.
    LobbyIntent k = intent(LI_KICK, b, NULL);
    LobbyOutcome o = lobby_host_intent(L, a, &k, 2.0, map_ok, NULL);
    CHECK(o.result == LR_DENIED && lobby_member(L, b), "member cannot kick");
    o = lobby_host_intent(L, 0, &k, 2.0, map_ok, NULL);
    CHECK(o.result == LR_OK && o.kicked == b && !lobby_member(L, b) && o.changed, "host kicks Bob");
    CHECK(lobby_is_banned(L, "lan:10.0.0.3/Bob"), "Bob banned");
    u8 again = lobby_host_add(L, "Bob", 0, "lan:10.0.0.3/Bob", "", 5, 3.0, why, sizeof(why));
    CHECK(again == LOBBY_NONE && strstr(why, "kicked"), "rejoin blocked (%s)", why);
    u8 other = lobby_host_add(L, "Bob", 0, "lan:10.0.0.9/Bob", "", 6, 3.0, why, sizeof(why));
    CHECK(other != LOBBY_NONE, "another machine may join");
    k = intent(LI_KICK, 0, NULL);
    CHECK(lobby_host_intent(L, 0, &k, 3.0, map_ok, NULL).result == LR_INVALID, "host cannot kick itself");

    // Ids are never reused within a session.
    CHECK(other != b && other > c, "fresh member id");

    // Late join: allowed in game by default, refused when disabled.
    lobby_host_remove(L, other, 4.0, "left");
    L->state = LS_IN_GAME;
    CHECK(lobby_host_add(L, "Eve", 0, "lan:e", "", 7, 5.0, why, sizeof(why)) != LOBBY_NONE, "late join allowed");
    lobby_host_remove(L, lobby_member_by_name(L, "Eve")->id, 5.0, "left");
    L->settings.late_join = false;
    CHECK(lobby_host_add(L, "Eve", 0, "lan:e", "", 7, 5.0, why, sizeof(why)) == LOBBY_NONE && strstr(why, "progress"), "late join refused (%s)", why);

    // Migration successor: the earliest joined of the others.
    Lobby *M = host_lobby();
    u8 m1 = lobby_host_add(M, "One", 0, "lan:1", "", 1, 0, why, sizeof(why));
    u8 m2 = lobby_host_add(M, "Two", 0, "lan:2", "", 2, 0, why, sizeof(why));
    CHECK(lobby_successor(M, 0) == m1, "successor = first joined");
    lobby_host_remove(M, m1, 1, "left");
    CHECK(lobby_successor(M, 0) == m2, "successor after the first left");
    lobby_host_remove(M, m2, 1, "left");
    CHECK(lobby_successor(M, 0) == LOBBY_NONE, "nobody left");
    LobbyIntent pr = intent(LI_PROMOTE, m2, NULL);
    CHECK(lobby_host_intent(M, 0, &pr, 2, map_ok, NULL).result == LR_INVALID, "promote a gone member");
}

static void test_ready_start(void) {
    Lobby *L = host_lobby();
    char why[96];
    u8 a = lobby_host_add(L, "Alice", 0, "lan:a", "", 1, 0, why, sizeof(why));
    u8 b = lobby_host_add(L, "Bob", 0, "lan:b", "", 2, 0, why, sizeof(why));
    LobbyIntent start = intent(LI_START, 0, NULL), force = intent(LI_START, 1, NULL);
    LobbyOutcome o = lobby_host_intent(L, 0, &start, 1, map_ok, NULL);
    CHECK(o.result == LR_INVALID && !o.start, "start refused: nobody ready");
    o = lobby_host_intent(L, a, &start, 1, map_ok, NULL);
    CHECK(o.result == LR_DENIED, "members cannot start");
    LobbyIntent r1 = intent(LI_READY, 1, NULL);
    lobby_host_intent(L, 0, &r1, 1, map_ok, NULL);
    lobby_host_intent(L, a, &r1, 1, map_ok, NULL);
    CHECK(lobby_host_intent(L, a, &r1, 1, map_ok, NULL).result == LR_IGNORED, "ready twice: ignored");
    CHECK(!lobby_all_ready(L), "Bob not ready");
    o = lobby_host_intent(L, 0, &start, 1, map_ok, NULL);
    CHECK(!o.start, "still refused");
    // Forced start: a 10 s countdown.
    o = lobby_host_intent(L, 0, &force, 1, map_ok, NULL);
    CHECK(o.countdown && !o.start && fabsf(L->countdown - LOBBY_COUNTDOWN) < 1e-3f, "force -> countdown");
    L->state = lobby_fsm_next(L->state, LE_FORCE_START);
    CHECK(L->state == LS_COUNTDOWN, "countdown state");
    bool done = false;
    for (int i = 0; i < 9 * 60; i++) done |= lobby_tick(L, 1.0f / 60.0f);
    CHECK(!done, "not done after 9 s");
    // Cancel, then everyone ready during a new countdown starts at once.
    LobbyIntent cancel = intent(LI_CANCEL, 0, NULL);
    o = lobby_host_intent(L, 0, &cancel, 2, map_ok, NULL);
    CHECK(o.changed && L->countdown < 0, "cancelled");
    L->state = lobby_fsm_next(L->state, LE_COUNTDOWN_CANCEL);
    CHECK(L->state == LS_LOBBY, "back to lobby");
    lobby_host_intent(L, 0, &force, 3, map_ok, NULL);
    L->state = LS_COUNTDOWN;
    o = lobby_host_intent(L, b, &r1, 3, map_ok, NULL);
    CHECK(o.start, "last ready during the countdown starts the game");
    // A full countdown runs out.
    L->countdown = LOBBY_COUNTDOWN;
    done = false;
    for (int i = 0; i < 11 * 60 && !done; i++) done = lobby_tick(L, 1.0f / 60.0f);
    CHECK(done && L->countdown < 0, "countdown runs out after 10 s");
    // All ready: start without a countdown.
    L->state = LS_LOBBY;
    o = lobby_host_intent(L, 0, &start, 4, map_ok, NULL);
    CHECK(o.start && !o.countdown, "all ready: start");
    // Ready changes refused once loading.
    L->state = LS_LOADING;
    LobbyIntent r0 = intent(LI_READY, 0, NULL);
    CHECK(lobby_host_intent(L, a, &r0, 5, map_ok, NULL).result == LR_INVALID, "no unready in game");

    // Map / settings: host only, validated.
    L->state = LS_LOBBY;
    LobbyIntent m = intent(LI_MAP, 0, "zombieland");
    CHECK(lobby_host_intent(L, a, &m, 6, map_ok, NULL).result == LR_DENIED, "members cannot change the map");
    o = lobby_host_intent(L, 0, &m, 6, map_ok, NULL);
    CHECK(o.result == LR_OK && o.changed && !strcmp(L->settings.map, "zombieland"), "host changes the map");
    m = intent(LI_MAP, 0, "atlantis");
    CHECK(lobby_host_intent(L, 0, &m, 6, map_ok, NULL).result == LR_INVALID, "unknown map refused");
    LobbyIntent st = intent(LI_SETTINGS, 0, NULL);
    st.settings = L->settings;
    st.settings.max_players = 1;
    st.settings.season = 3;
    o = lobby_host_intent(L, 0, &st, 6, map_ok, NULL);
    CHECK(o.changed && L->settings.max_players == 3 && L->settings.season == 3, "max players never below the players in (%d), season set", L->settings.max_players);
    st.settings.season = 9;
    CHECK(lobby_host_intent(L, 0, &st, 6, map_ok, NULL).result == LR_INVALID, "bad season refused");
    st.settings = L->settings;
    st.settings.start_hour = 30;
    CHECK(lobby_host_intent(L, 0, &st, 6, map_ok, NULL).result == LR_INVALID, "bad hour refused");
}

static void test_chat(void) {
    Lobby *L = host_lobby();
    char why[96];
    u8 a = lobby_host_add(L, "Alice", 0, "lan:a", "", 1, 0, why, sizeof(why));
    int before = L->chat_count;
    LobbyIntent c = intent(LI_CHAT, 0, "  hello   ");
    LobbyOutcome o = lobby_host_intent(L, a, &c, 10.0, map_ok, NULL);
    CHECK(o.chat && L->chat_count == before + 1 && !strcmp(lobby_chat_at(L, L->chat_count - 1)->text, "hello"), "chat trimmed + relayed");
    CHECK(lobby_chat_at(L, L->chat_count - 1)->from == a && !strcmp(lobby_chat_at(L, L->chat_count - 1)->name, "Alice"), "chat line names the sender");
    // Rate limit: a burst of LOBBY_CHAT_BURST, then refused until refilled.
    int ok = 1;
    for (int i = 0; i < 8; i++) if (lobby_host_intent(L, a, &c, 10.0 + i * 0.01, map_ok, NULL).result == LR_OK) ok++;
    CHECK(ok == LOBBY_CHAT_BURST, "burst of %d allowed (%d)", LOBBY_CHAT_BURST, ok);
    CHECK(lobby_host_intent(L, a, &c, 10.2, map_ok, NULL).result == LR_RATE_LIMITED, "rate limited");
    CHECK(lobby_host_intent(L, a, &c, 10.2 + LOBBY_CHAT_REFILL + 0.01, map_ok, NULL).result == LR_OK, "one more after the refill");
    CHECK(lobby_host_intent(L, 0, &c, 10.2, map_ok, NULL).result == LR_OK, "rate limits are per member");
    // Empty / whitespace: ignored. Control characters stripped.
    LobbyIntent e = intent(LI_CHAT, 0, "   ");
    CHECK(lobby_host_intent(L, 0, &e, 30, map_ok, NULL).result == LR_IGNORED, "empty message ignored");
    char t[64] = "a\tb\x01" "c\x7f";
    lobby_sanitize(t);
    CHECK(!strcmp(t, "a bc"), "sanitize (%s)", t);
    // 256 character cap: longer input cut by the sanitizer, and a longer wire message refused.
    char big[600];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK(lobby_sanitize(big) == LOBBY_CHAT_MAX, "cut to %d", LOBBY_CHAT_MAX);
    u8 buf[2048];
    Writer w = writer_make(buf, sizeof(buf));
    w_u8(&w, LOP_INTENT);
    w_u8(&w, LI_CHAT);
    w_u8(&w, 0);
    w_u16(&w, 300);
    for (int i = 0; i < 300; i++) w_u8(&w, 'y');
    Reader r = reader_make(buf, w.len);
    r_u8(&r);
    LobbyIntent in;
    CHECK(!lobby_read_intent(&r, &in), "over-long chat on the wire refused");
    // Profanity stub: whole words only.
    char p[96] = "what the FUCK, shitake mushrooms";
    int n = lobby_filter_profanity(p);
    CHECK(n == 1 && !strcmp(p, "what the ****, shitake mushrooms"), "profanity filter (%s)", p);
    // Scrollback ring keeps the newest LOBBY_CHAT_LINES.
    for (int i = 0; i < LOBBY_CHAT_LINES + 10; i++) lobby_chat_system(L, 100 + i, LC_SYSTEM, "line %d", i);
    CHECK(L->chat_count == LOBBY_CHAT_LINES && !strcmp(lobby_chat_at(L, LOBBY_CHAT_LINES - 1)->text, "line 137") && !strcmp(lobby_chat_at(L, 0)->text, "line 10"),
          "ring keeps the newest (%s .. %s)", lobby_chat_at(L, 0)->text, lobby_chat_at(L, LOBBY_CHAT_LINES - 1)->text);

    // Slash commands.
    char err[128];
    LobbyIntent s;
    CHECK(lobby_parse_chat(L, "/ready", &s, err, sizeof(err)) && s.type == LI_READY && s.a == 1, "/ready");
    CHECK(lobby_parse_chat(L, "/unready", &s, err, sizeof(err)) && s.type == LI_READY && s.a == 0, "/unready");
    CHECK(lobby_parse_chat(L, "/kick alice", &s, err, sizeof(err)) && s.type == LI_KICK && s.a == a, "/kick by name");
    CHECK(!lobby_parse_chat(L, "/kick nobody", &s, err, sizeof(err)) && strstr(err, "nobody"), "/kick unknown (%s)", err);
    CHECK(lobby_parse_chat(L, "/map zombieland", &s, err, sizeof(err)) && s.type == LI_MAP && !strcmp(s.text, "zombieland"), "/map");
    CHECK(!lobby_parse_chat(L, "/map", &s, err, sizeof(err)), "/map without id");
    CHECK(lobby_parse_chat(L, "/start force", &s, err, sizeof(err)) && s.type == LI_START && s.a == 1, "/start force");
    CHECK(!lobby_parse_chat(L, "/dance", &s, err, sizeof(err)) && strstr(err, "Unknown"), "unknown command");
    CHECK(lobby_parse_chat(L, "hi /ready", &s, err, sizeof(err)) && s.type == LI_CHAT, "not a command mid-text");
}

static void test_version(void) {
    u32 a = lobby_version_hash("0.1.0", 16, 1234), b = lobby_version_hash("0.1.0", 16, 1234);
    u32 c = lobby_version_hash("0.1.1", 16, 1234), d = lobby_version_hash("0.1.0", 16, 999), e = lobby_version_hash("0.1.0", 15, 1234);
    CHECK(a == b && a != c && a != d && a != e, "version hash covers version, protocol, content");
    char why[128];
    CHECK(lobby_check_version(a, "0.1.0", b, "0.1.0", why, sizeof(why)), "same build joins");
    CHECK(!lobby_check_version(a, "0.1.0", c, "0.1.1", why, sizeof(why)) && strstr(why, "0.1.0") && strstr(why, "0.1.1"), "version mismatch rejected (%s)", why);
    CHECK(!lobby_check_version(a, "0.1.0", d, "0.1.0", why, sizeof(why)) && strstr(why, "data"), "content mismatch rejected (%s)", why);
}

static void test_wire(void) {
    Lobby *L = host_lobby();
    char why[96];
    u8 a = lobby_host_add(L, "Alice", 76561198000000002ull, "steam:2", "76561198000000002", 1, 0, why, sizeof(why));
    lobby_member(L, a)->ready = true;
    lobby_member(L, a)->ping_ms = 42;
    dg_strcpy(L->settings.weather, sizeof(L->settings.weather), "light_rain");
    L->settings.season = 2;
    L->settings.start_hour = 21;
    L->settings.pvp = true;
    L->settings.dynamic_weather = false;
    u8 buf[4096];
    Writer w = writer_make(buf, sizeof(buf));
    lobby_write_snapshot(&w, L, a);
    CHECK(!w.overflow && w.len < 1024, "snapshot is small (%u bytes)", w.len);
    Lobby C;
    lobby_init(&C);
    C.state = LS_JOINING;
    lobby_chat_system(&C, 0, LC_SYSTEM, "local line");
    Reader r = reader_make(buf, w.len);
    CHECK(r_u8(&r) == LOP_SNAPSHOT && lobby_read_snapshot(&r, &C), "snapshot decodes");
    CHECK(C.local_id == a && lobby_count(&C) == 2 && lobby_member(&C, a)->ready && lobby_member(&C, a)->ping_ms == 42, "members replicated");
    CHECK(lobby_member(&C, a)->steam_id == 76561198000000002ull && lobby_member(&C, 0)->host, "steam id + host flag");
    CHECK(!memcmp(&C.settings, &L->settings, sizeof(LobbySettings)), "settings replicated (season, hour, weather, flags)");
    CHECK(C.chat_count == 1, "local chat kept");
    // Countdown follows the host.
    C.state = LS_LOBBY;
    L->state = LS_COUNTDOWN;
    L->countdown = 7.5f;
    w = writer_make(buf, sizeof(buf));
    lobby_write_snapshot(&w, L, a);
    r = reader_make(buf, w.len);
    r_u8(&r);
    lobby_read_snapshot(&r, &C);
    CHECK(C.state == LS_COUNTDOWN && fabsf(C.countdown - 7.5f) < 1e-4f, "countdown replicated");
    // Truncated snapshot fails cleanly.
    r = reader_make(buf, w.len / 2);
    r_u8(&r);
    Lobby D;
    lobby_init(&D);
    CHECK(!lobby_read_snapshot(&r, &D), "truncated snapshot refused");
    // Chat and intents.
    LobbyChatLine line;
    memset(&line, 0, sizeof(line));
    line.from = a;
    line.kind = LC_PLAYER;
    dg_strcpy(line.name, sizeof(line.name), "Alice");
    dg_strcpy(line.text, sizeof(line.text), "hello there");
    w = writer_make(buf, sizeof(buf));
    lobby_write_chat(&w, &line);
    r = reader_make(buf, w.len);
    LobbyChatLine got;
    CHECK(r_u8(&r) == LOP_CHAT && lobby_read_chat(&r, &got) && !strcmp(got.text, "hello there") && got.from == a, "chat round trip");
    LobbyIntent in = intent(LI_SETTINGS, 0, NULL), out;
    in.settings = L->settings;
    w = writer_make(buf, sizeof(buf));
    lobby_write_intent(&w, &in);
    r = reader_make(buf, w.len);
    CHECK(r_u8(&r) == LOP_INTENT && lobby_read_intent(&r, &out) && out.type == LI_SETTINGS && !memcmp(&out.settings, &in.settings, sizeof(LobbySettings)), "settings intent round trip");
    // Steam metadata + rich presence.
    LobbyKV kv[16];
    int n = lobby_metadata(L, "Host", 0xABCDu, kv, 16);
    bool has_map = false, has_ver = false;
    for (int i = 0; i < n; i++) {
        if (!strcmp(kv[i].key, "map") && !strcmp(kv[i].value, L->settings.map)) has_map = true;
        if (!strcmp(kv[i].key, "version") && !strcmp(kv[i].value, "0000abcd")) has_ver = true;
    }
    CHECK(has_map && has_ver, "metadata has map + version hash");
    char rp[96];
    L->state = LS_LOBBY;
    lobby_rich_presence(L, "Forest", rp, sizeof(rp));
    CHECK(!strcmp(rp, "In lobby - 2/4 - Forest"), "rich presence lobby (%s)", rp);
    L->state = LS_IN_GAME;
    lobby_rich_presence(L, "Dev Room", rp, sizeof(rp));
    CHECK(!strcmp(rp, "In game - Dev Room"), "rich presence game (%s)", rp);
}

// ------------------------------------------------------------------ data

static char *read_text(const char *rel) {
    char buf[512];
    return platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
}

static bool exists_cb(const char *path, void *user) {
    (void)user;
    char buf[512];
    return platform_file_exists(platform_data_path(path, buf, sizeof(buf)));
}

static void test_maps(void) {
    MapList ml;
    int n = maplist_load(&ml, "assets/maps");
    CHECK(n >= 3, "maps loaded (%d)", n);
    CHECK(maplist_find(&ml, "devroom") != NULL, "dev room listed");
    CHECK(maplist_find(&ml, "vegetation") != NULL, "open world listed");
    // Every file parses (nothing skipped), thumbnails exist, levels exist (or are procedural).
    char dir[512];
    platform_data_path("assets/maps", dir, sizeof(dir));
    int count = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    CHECK(count == n, "every map file valid (%d files, %d maps)", count, n);
    SDL_free(files);
    for (int i = 0; i < ml.count; i++) {
        const MapInfo *m = &ml.maps[i];
        char rel[128];
        snprintf(rel, sizeof(rel), "data/levels/%s.json", m->level);
        bool proc = !strcmp(m->level, "streaming") || !strcmp(m->level, "flat") || !strcmp(m->level, "terrain");
        CHECK(proc || exists_cb(rel, NULL), "%s: level %s exists", m->id, m->level);
        CHECK(exists_cb(m->thumbnail, NULL), "%s: thumbnail", m->id);
        CHECK(m->season >= 0 && m->season < SEASON_COUNT, "%s: season", m->id);
        if (i) CHECK(ml.maps[i - 1].order <= m->order, "sorted by order");
    }
    CHECK(maplist_hash(&ml) != 0, "catalogue hash");
    // Validation of bad files.
    MapInfo mi;
    char err[160];
    CHECK(!maplist_parse("{ \"level\": \"x\", \"thumbnail\": \"t.png\", \"biome\": \"b\", \"description\": \"d\" }", "a", &mi, NULL, NULL, err, sizeof(err)) && strstr(err, "name"), "name required (%s)", err);
    CHECK(!maplist_parse("{ \"name\": \"A\", \"thumbnail\": \"t.png\", \"biome\": \"b\", \"description\": \"d\" }", "a", &mi, NULL, NULL, err, sizeof(err)) && strstr(err, "level"), "level required");
    CHECK(!maplist_parse("{ \"name\": \"A\", \"level\": \"x\", \"thumbnail\": \"t.png\", \"biome\": \"b\", \"description\": \"d\", \"time_of_day\": 30 }", "a", &mi, NULL, NULL, err, sizeof(err)) && strstr(err, "time_of_day"), "hour range");
    CHECK(!maplist_parse("{ \"name\": \"A\", \"level\": \"x\", \"thumbnail\": \"t.png\", \"biome\": \"b\", \"description\": \"d\", \"players\": [4, 2] }", "a", &mi, NULL, NULL, err, sizeof(err)) && strstr(err, "players"), "players range");
    CHECK(!maplist_parse("{ \"name\": \"A\", \"level\": \"x\", \"thumbnail\": \"t.png\", \"biome\": \"b\", \"description\": \"d\", \"season\": \"monsoon\" }", "a", &mi, NULL, NULL, err, sizeof(err)) && strstr(err, "season"), "season name");
    CHECK(!maplist_parse("{ \"name\": \"A\", \"level\": \"x\", \"thumbnail\": \"missing.png\", \"biome\": \"b\", \"description\": \"d\" }", "a", &mi, exists_cb, NULL, err, sizeof(err)) && strstr(err, "thumbnail"), "thumbnail must exist");
    CHECK(!maplist_parse("{ nope", "a", &mi, NULL, NULL, err, sizeof(err)), "syntax error");
    CHECK(maplist_parse("{ \"name\": \"A\", \"level\": \"x\", \"thumbnail\": \"t.png\", \"biome\": \"b\", \"description\": \"d\", \"season\": \"winter\" }", "a", &mi, NULL, NULL, err, sizeof(err)) &&
          mi.season == SEASON_WINTER && mi.players_min == 1 && mi.players_max == 4, "minimal valid map + defaults");
    char sz[32];
    mi.size_m = 2000;
    maplist_size_text(&mi, sz, sizeof(sz));
    CHECK(!strcmp(sz, "2 km x 2 km"), "size text (%s)", sz);
}

// The player character as the game loads it: player.json plus the skins (body slot) from skins.json.
static bool load_player_def(CharacterDef *d) {
    char *text = read_text("assets/characters/player.json");
    if (!text) return false;
    char err[160];
    bool ok = character_def_parse(d, text, "player.json", err, sizeof(err));
    free(text);
    if (!ok) { printf("player.json: %s\n", err); return false; }
    char *sk = read_text("assets/human/skins.json");
    cJSON *root = sk ? cJSON_Parse(sk) : NULL;
    free(sk);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "skins")) {
        CharPartOption o;
        memset(&o, 0, sizeof(o));
        dg_strcpy(o.id, sizeof(o.id), cJSON_GetObjectItemCaseSensitive(e, "id")->valuestring);
        o.mesh = 0;
        character_add_option(d, CSLOT_BODY, &o);
    }
    cJSON_Delete(root);
    for (int s = 0; s < CSLOT_COUNT; s++) for (int i = 0; i < d->slots[s].option_count; i++) d->slots[s].options[i].mesh = 0;   // "loaded"
    return true;
}

static void test_cosmetics(void) {
    static CosmeticDB db;
    int n = cosmetics_load(&db, "assets/cosmetics");
    CHECK(n >= 10, "cosmetics loaded (%d)", n);
    static CharacterDef def;
    memset(&def, 0, sizeof(def));
    CHECK(load_player_def(&def), "player character parses");
    int per_slot[COS_SLOTS] = { 0 };
    for (int i = 0; i < db.count; i++) {
        const CosmeticItem *it = &db.items[i];
        per_slot[it->slot]++;
        if (it->slot == COS_SLOT_WEAPON) continue;
        CHECK(character_find_option(&def, it->slot, it->option) >= 0, "%s: player.json has option '%s' in slot %s", it->id, it->option, char_slot_name(it->slot));
    }
    CHECK(per_slot[CSLOT_BODY] && per_slot[CSLOT_HEAD] && per_slot[CSLOT_TORSO] && per_slot[CSLOT_LEGS] && per_slot[CSLOT_HANDS] && per_slot[COS_SLOT_WEAPON], "categories filled");
    // Ownership: default items owned, locked ones only through the profile.
    CosmeticUnlocks un;
    memset(&un, 0, sizeof(un));
    const CosmeticItem *hood = cosmetics_find(&db, "hood_grey");
    CHECK(hood && !cosmetics_owned(hood, &un), "locked item not owned");
    CosmeticLoadout lo;
    memset(&lo, 0, sizeof(lo));
    CHECK(!cosmetics_equip(&db, &lo, &un, "hood_grey"), "cannot equip a locked item");
    dg_strcpy(un.ids[un.count++], sizeof(un.ids[0]), "hood_grey");
    CHECK(cosmetics_owned(hood, &un) && cosmetics_equip(&db, &lo, &un, "hood_grey"), "unlocked: equips");
    CHECK(!cosmetics_equip(&db, &lo, &un, "no_such_item"), "unknown item");
    // Round trip: loadout -> part options -> selection -> loadout.
    CHECK(cosmetics_equip(&db, &lo, &un, "outfit_officer") && cosmetics_equip(&db, &lo, &un, "helmet_black") && cosmetics_equip(&db, &lo, &un, "vest_tan") &&
          cosmetics_equip(&db, &lo, &un, "pads_grey") && cosmetics_equip(&db, &lo, &un, "gloves_tan"), "equip a set");
    CHECK(cosmetics_equip(&db, &lo, &un, "helmet_olive") && !strcmp(lo.equipped[CSLOT_HEAD], "helmet_olive"), "equipping replaces the slot");
    int opt[CSLOT_COUNT];
    CHECK(cosmetics_to_parts(&db, &lo, &def, opt) == 0, "every equipped item resolves");
    u8 sel[CSLOT_COUNT];
    for (int s = 0; s < CSLOT_COUNT; s++) sel[s] = (u8)(opt[s] < 0 ? CHAR_PART_NONE : opt[s] + 1);
    CHECK(!strcmp(def.slots[CSLOT_HEAD].options[opt[CSLOT_HEAD]].id, "helmet_olive"), "head option");
    CosmeticLoadout back;
    cosmetics_from_parts(&db, &def, sel, &back);
    for (int s = 0; s < CSLOT_COUNT; s++) CHECK(!strcmp(back.equipped[s], lo.equipped[s]), "slot %s round trip (%s vs %s)", char_slot_name(s), back.equipped[s], lo.equipped[s]);
    char parts[256];
    cosmetics_parts_string(&db, &lo, parts, sizeof(parts));
    CHECK(strstr(parts, "head:helmet_olive") && strstr(parts, "torso:vest_tan") && !strstr(parts, "body:"), "cl_parts string (%s)", parts);
    CHECK(!strcmp(cosmetics_body_skin(&db, &lo), "officer"), "cl_skin from the body item");
    cosmetics_unequip(&lo, CSLOT_HEAD);
    CHECK(!lo.equipped[CSLOT_HEAD][0] && !cosmetics_is_equipped(&lo, "helmet_olive"), "unequip");
    cosmetics_to_parts(&db, &lo, &def, opt);
    CHECK(opt[CSLOT_HEAD] < 0, "unequipped slot empty");
    // Bad files.
    CosmeticDB bad;
    memset(&bad, 0, sizeof(bad));
    char err[160];
    CHECK(!cosmetics_parse(&bad, "{\"items\":[{\"id\":\"a\",\"slot\":\"nose\",\"option\":\"x\"}]}", "t", err, sizeof(err)) && strstr(err, "slot"), "bad slot (%s)", err);
    CHECK(!cosmetics_parse(&bad, "{\"items\":[{\"id\":\"a\",\"slot\":\"head\",\"option\":\"x\",\"rarity\":\"mythic\"}]}", "t", err, sizeof(err)) && strstr(err, "rarity"), "bad rarity");
    CHECK(!cosmetics_parse(&bad, "{\"items\":[{\"id\":\"a\",\"slot\":\"head\",\"option\":\"x\"},{\"id\":\"a\",\"slot\":\"head\",\"option\":\"y\"}]}", "t", err, sizeof(err)) && strstr(err, "duplicate"), "duplicate id");
    CHECK(bad.count == 0, "nothing added from a bad file");
}

static void test_profile(void) {
    Profile p, q;
    profile_default(&p);
    dg_strcpy(p.lan_name, sizeof(p.lan_name), "Lan Name");
    dg_strcpy(p.loadout.equipped[CSLOT_HEAD], sizeof(p.loadout.equipped[0]), "helmet_olive");
    dg_strcpy(p.loadout.equipped[COS_SLOT_WEAPON], sizeof(p.loadout.equipped[0]), "skin_desert");
    dg_strcpy(p.unlocks.ids[p.unlocks.count++], sizeof(p.unlocks.ids[0]), "hood_grey");
    dg_strcpy(p.last_map, sizeof(p.last_map), "zombieland");
    p.lobby.season = SEASON_WINTER;
    p.lobby.start_hour = 22;
    p.lobby.dynamic_weather = false;
    dg_strcpy(p.lobby.weather, sizeof(p.lobby.weather), "snow");
    p.kills = 12;
    p.playtime_s = 3600.5;
    char *text = profile_to_json(&p);
    CHECK(text != NULL, "profile serializes");
    profile_default(&q);
    CHECK(profile_from_json(&q, text), "profile parses");
    free(text);
    CHECK(!strcmp(q.lan_name, "Lan Name") && !strcmp(q.loadout.equipped[CSLOT_HEAD], "helmet_olive") && !strcmp(q.loadout.equipped[COS_SLOT_WEAPON], "skin_desert"), "loadout round trip");
    CHECK(q.unlocks.count == 1 && !strcmp(q.unlocks.ids[0], "hood_grey") && !strcmp(q.last_map, "zombieland"), "unlocks + last map");
    CHECK(q.lobby.season == SEASON_WINTER && q.lobby.start_hour == 22 && !q.lobby.dynamic_weather && !strcmp(q.lobby.weather, "snow"), "lobby settings round trip");
    CHECK(q.kills == 12 && fabs(q.playtime_s - 3600.5) < 1e-6, "stats");
    profile_default(&q);
    CHECK(!profile_from_json(&q, "{ broken"), "broken file refused");
    CHECK(profile_from_json(&q, "{}") && !strcmp(q.lan_name, "Survivor"), "empty file: defaults");
}

// ------------------------------------------------------------------ seasons + weather

static void test_seasons(void) {
    SeasonTable t;
    CHECK(seasons_load(&t, "assets/world/seasons.json"), "seasons.json parses");
    SeasonTable d;
    seasons_defaults(&d);
    for (int s = 0; s < SEASON_COUNT; s++) {
        CHECK(!strcmp(t.s[s].id, season_id(s)) && t.s[s].weather_count > 0, "%s defined", season_id(s));
        CHECK(t.s[s].day_of_year == d.s[s].day_of_year && fabsf(t.s[s].temp_offset - d.s[s].temp_offset) < 1e-4f && t.s[s].weather_count == d.s[s].weather_count,
              "%s: built-in defaults match the file", season_id(s));
        for (int k = 0; k < t.s[s].weather_count && k < d.s[s].weather_count; k++)
            CHECK(!strcmp(t.s[s].weather[k], d.s[s].weather[k]) && fabsf(t.s[s].weight[k] - d.s[s].weight[k]) < 1e-4f, "%s weight %s", season_id(s), t.s[s].weather[k]);
    }
    CHECK(t.s[SEASON_SUMMER].day_of_year > 150 && t.s[SEASON_SUMMER].day_of_year < 240 && (t.s[SEASON_WINTER].day_of_year < 60 || t.s[SEASON_WINTER].day_of_year > 330), "calendar days");
    CHECK(!season_allows(&t.s[SEASON_SUMMER], "snow") && season_allows(&t.s[SEASON_WINTER], "snow") && !season_allows(&t.s[SEASON_WINTER], "rain"), "allowed presets");
    char err[128];
    SeasonTable x = t;
    CHECK(!seasons_parse(&x, "{\"seasons\":[{\"id\":\"monsoon\"}]}", err, sizeof(err)), "unknown season id");
    CHECK(!seasons_parse(&x, "{\"seasons\":[{\"id\":\"winter\",\"day_of_year\":400}]}", err, sizeof(err)), "bad day");
    CHECK(!seasons_parse(&x, "{\"seasons\":[{\"id\":\"winter\",\"weather\":{\"snow\":-1}}]}", err, sizeof(err)), "negative weight");

    WeatherLib lib;
    CHECK(weather_lib_load(&lib, "assets/world/weather"), "weather presets");
    for (int s = 0; s < SEASON_COUNT; s++)
        for (int k = 0; k < t.s[s].weather_count; k++) CHECK(weather_find_preset(&lib, t.s[s].weather[k]) >= 0, "%s: preset %s exists", season_id(s), t.s[s].weather[k]);
    // Long scheduler walks per season: the weather looks like the season (autumn rains, winter snows,
    // summer is mostly fair), and disallowed presets never come.
    for (int s = 0; s < SEASON_COUNT; s++) {
        float bias[WEATHER_MAX_PRESETS];
        season_weather_bias(&t.s[s], &lib, bias);
        weather_lib_set_bias(&lib, bias, t.s[s].temp_offset);
        int cur = weather_find_preset(&lib, "clear"), rain = 0, snow = 0, fair = 0, steps = 4000, bad = 0;
        for (int i = 0; i < steps; i++) {
            cur = weather_pick_next(&lib, cur, (u32)i * 2654435761u + (u32)s * 97u);
            const WeatherPreset *p = &lib.presets[cur];
            if (bias[cur] <= 0) bad++;
            WeatherParams pp = p->p;
            weather_seasonal_params(&lib, &pp);
            float type = weather_precip_type(pp.v[WP_TEMPERATURE]);
            if (pp.v[WP_PRECIP] > 0.05f) { if (type > 0.5f) snow++; else rain++; }
            else if (pp.v[WP_CLOUD_COVER] < 0.5f) fair++;
        }
        printf("season %-6s: rain %4.1f%%  snow %4.1f%%  fair %4.1f%%\n", season_id(s), 100.0 * rain / steps, 100.0 * snow / steps, 100.0 * fair / steps);
        CHECK(bad == 0, "%s: only the season's presets (%d others)", season_id(s), bad);
        if (s == SEASON_AUTUMN) CHECK(rain > steps * 30 / 100 && snow == 0, "autumn rains (%d%%)", rain * 100 / steps);
        if (s == SEASON_WINTER) CHECK(snow > steps * 30 / 100 && rain == 0, "winter snows (%d%%), never rains", snow * 100 / steps);
        if (s == SEASON_SUMMER) CHECK(fair > steps / 2 && snow == 0 && rain < steps / 5, "summer is mostly fair (%d%%)", fair * 100 / steps);
        if (s == SEASON_SPRING) CHECK(snow == 0 && rain > steps / 10, "spring showers");
    }
    // Winter: even a rain preset falls as snow (the temperature offset).
    float bias[WEATHER_MAX_PRESETS];
    season_weather_bias(&t.s[SEASON_WINTER], &lib, bias);
    weather_lib_set_bias(&lib, bias, t.s[SEASON_WINTER].temp_offset);
    WeatherParams rp = lib.presets[weather_find_preset(&lib, "rain")].p;
    weather_seasonal_params(&lib, &rp);
    CHECK(weather_precip_type(rp.v[WP_TEMPERATURE]) > 0.99f, "winter rain is snow (%.1f C)", rp.v[WP_TEMPERATURE]);
    // No season: the plain Markov table (unchanged behaviour).
    weather_lib_set_bias(&lib, NULL, 0);
    CHECK(!lib.bias_on && lib.temp_offset == 0, "bias off");
    WeatherParams cp = lib.presets[0].p, cp2 = cp;
    weather_seasonal_params(&lib, &cp2);
    CHECK(!memcmp(&cp, &cp2, sizeof(cp)), "no offset without a season");
    // Start weather pick: deterministic, always one of the season's presets.
    for (u32 seed = 0; seed < 200; seed++) {
        const char *w = season_pick_weather(&t.s[SEASON_AUTUMN], seed);
        if (!season_allows(&t.s[SEASON_AUTUMN], w)) { CHECK(false, "start pick %s not in autumn", w); break; }
    }
    CHECK(!strcmp(season_pick_weather(&t.s[SEASON_WINTER], 7), season_pick_weather(&t.s[SEASON_WINTER], 7)), "start pick deterministic");
}

// ------------------------------------------------------------------ screen lint + layouts

static int g_tok_count;
static LayoutToken g_toks[GUI_LAYOUT_MAX_NODES * 2];

static bool load_theme_tokens(void) {
    char *text = read_text("assets/ui/theme.json");
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!root) return false;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "layout")) {
        if (!cJSON_IsNumber(e) || g_tok_count >= ARRAY_COUNT(g_toks)) continue;
        dg_strcpy(g_toks[g_tok_count].name, sizeof(g_toks[0].name), e->string);
        g_toks[g_tok_count++].value = (float)e->valuedouble;
    }
    cJSON_Delete(root);
    gui_layout_set_tokens(g_toks, g_tok_count);
    return g_tok_count > 0;
}

// Sizes in the main menu screens must be theme tokens ("@name"); ratios (w_pct, flex, aspect) may be
// numbers; colours never appear in screen JSON.
static void lint_value(const char *file, const char *node, const char *key, const cJSON *v) {
    static const char *const size_keys[] = { "x", "y", "w", "h", "min_w", "min_h", "max_w", "max_h", "gap", "pad" };
    bool is_size = false;
    for (int i = 0; i < ARRAY_COUNT(size_keys); i++) if (!strcmp(key, size_keys[i])) is_size = true;
    if (strstr(key, "color") || strstr(key, "colour") || strstr(key, "rgba") || strstr(key, "tint"))
        CHECK(false, "%s: node %s: colour key '%s' (colours come from theme.json)", file, node, key);
    if (!is_size) return;
    if (cJSON_IsArray(v)) {
        const cJSON *x;
        cJSON_ArrayForEach(x, v) lint_value(file, node, key, x);
        return;
    }
    CHECK(cJSON_IsString(v) && v->valuestring[0] == '@', "%s: node %s: '%s' is a literal size (use a theme.json layout token \"@name\")", file, node, key);
}

static void test_screens(void) {
    CHECK(load_theme_tokens(), "theme.json has layout tokens");
    char dir[512];
    platform_data_path("assets/ui/screens", dir, sizeof(dir));
    int count = 0;
    char **files = SDL_GlobDirectory(dir, "mainmenu*.json", 0, &count);
    CHECK(files && count >= 8, "main menu screens (%d)", count);
    static const LayoutParams sizes[] = { { 1920, 1080, 1, 0, 0 }, { 2560, 1080, 1, 0, 0 }, { 1024, 768, 0.711f, 0, 0 }, { 1280, 720, 0.667f, 0, 0 }, { 3840, 2160, 2, 0, 0 }, { 3440, 1440, 1.333f, 0, 0 } };
    for (int f = 0; files && f < count; f++) {
        char rel[256];
        snprintf(rel, sizeof(rel), "assets/ui/screens/%s", files[f]);
        char *text = read_text(rel);
        CHECK(text != NULL, "%s readable", rel);
        if (!text) continue;
        cJSON *root = cJSON_Parse(text);
        CHECK(root != NULL, "%s: JSON", rel);
        const cJSON *n;
        cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(root, "nodes")) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(n, "id");
            const cJSON *kv;
            cJSON_ArrayForEach(kv, n) lint_value(rel, cJSON_IsString(id) ? id->valuestring : "?", kv->string, kv);
        }
        cJSON_Delete(root);
        static LayoutNode nodes[GUI_LAYOUT_MAX_NODES];
        int nc = 0;
        char err[160];
        bool ok = gui_layout_parse(text, nodes, GUI_LAYOUT_MAX_NODES, &nc, err, sizeof(err));
        free(text);
        CHECK(ok, "%s parses: %s", rel, err);
        if (!ok) continue;
        // The shell solves against the window (safe area); the tab screens against the content
        // area: every node stays inside its root, sizes are positive, siblings in a stack don't overlap.
        bool shell = nc > 0 && nodes[0].safe;   // solved against the window (safe area), the tab screens inside the content area
        for (int k = 0; k < ARRAY_COUNT(sizes); k++) {
            LayoutParams p = sizes[k];
            if (shell) { p.safe_frac = 0.035f; p.max_aspect = 2.39f; }
            else { p.w *= 0.9f; p.h *= 0.8f; }   // roughly the shell's content area
            gui_layout_solve(nodes, nc, &p);
            GuiRect area = shell ? gui_layout_safe_rect(&p) : gui_rect(0, 0, p.w, p.h);
            for (int i = 0; i < nc; i++) {
                GuiRect r = nodes[i].rect;
                CHECK(r.w >= 0 && r.h >= 0, "%s %gx%g: node %s size", rel, p.w, p.h, nodes[i].id);
                CHECK(r.x >= area.x - 0.5f && r.y >= area.y - 0.5f && r.x + r.w <= area.x + area.w + 0.5f && r.y + r.h <= area.y + area.h + 0.5f,
                      "%s %gx%g: node %s (%.0f %.0f %.0f %.0f) outside (%.0f %.0f %.0f %.0f)", rel, p.w, p.h, nodes[i].id, r.x, r.y, r.w, r.h, area.x, area.y, area.w, area.h);
                for (int j = i + 1; j < nc; j++) {
                    if (nodes[j].parent != nodes[i].parent || nodes[i].parent < 0 || nodes[nodes[i].parent].dir == GD_OVERLAY) continue;
                    GuiRect a = nodes[i].rect, b = nodes[j].rect;
                    bool overlap = a.x + 0.5f < b.x + b.w && b.x + 0.5f < a.x + a.w && a.y + 0.5f < b.y + b.h && b.y + 0.5f < a.y + a.h;
                    CHECK(!overlap || a.w < 1 || a.h < 1 || b.w < 1 || b.h < 1, "%s %gx%g: %s overlaps %s", rel, p.w, p.h, nodes[i].id, nodes[j].id);
                }
            }
        }
    }
    SDL_free(files);
    // An unknown token is a parse error, not a silent 0.
    LayoutNode nodes[4];
    int nc;
    char err[160];
    CHECK(!gui_layout_parse("{\"nodes\":[{\"id\":\"a\",\"w\":\"@no_such_token\"}]}", nodes, 4, &nc, err, sizeof(err)) && strstr(err, "no_such_token"), "unknown token refused (%s)", err);
}

int main(void) {
    test_fsm();
    test_membership();
    test_ready_start();
    test_chat();
    test_version();
    test_wire();
    test_maps();
    test_cosmetics();
    test_profile();
    test_seasons();
    test_screens();
    printf("test_lobby: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
