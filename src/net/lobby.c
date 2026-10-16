// Lobby core (src/net/lobby.h, docs/MENU_LOBBY.md). Pure.
#include "lobby.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ state machine

static const char *const k_state_names[LS_COUNT] = { "idle", "creating", "joining", "lobby", "countdown", "loading", "in_game", "migrating", "closed" };
static const char *const k_event_names[LE_COUNT] = { "host", "join", "transport_up", "transport_fail", "snapshot", "rejected", "kicked", "leave",
                                                     "start", "force_start", "countdown_cancel", "countdown_done", "load_done", "host_lost", "migrated", "ack" };

const char *lobby_state_name(int s) { return s >= 0 && s < LS_COUNT ? k_state_names[s] : "?"; }
const char *lobby_event_name(int e) { return e >= 0 && e < LE_COUNT ? k_event_names[e] : "?"; }

// One row per state: the state each event leads to (-1 = ignored). The table is the whole machine
// (docs/MENU_LOBBY.md "Lobby state machine"); tests/test_lobby.c walks every cell.
#define X -1
static const signed char k_fsm[LS_COUNT][LE_COUNT] = {
    //            HOST         JOIN        UP         FAIL       SNAP       REJ        KICK       LEAVE    START       FORCE         CANCEL     CDONE       LOADED      HOSTLOST      MIGRATED   ACK
    [LS_IDLE]      = { LS_CREATING, LS_JOINING, X,       X,         X,         X,         X,         X,       X,          X,            X,         X,          X,          X,            X,         X },
    [LS_CREATING]  = { X,           X,          LS_LOBBY, LS_CLOSED, X,        X,         X,         LS_IDLE, X,          X,            X,         X,          X,          X,            X,         X },
    [LS_JOINING]   = { X,           X,          X,       LS_CLOSED, LS_LOBBY,  LS_CLOSED, LS_CLOSED, LS_IDLE, LS_LOADING, X,            X,         X,          X,          LS_CLOSED,    X,         X },
    [LS_LOBBY]     = { X,           X,          X,       LS_CLOSED, X,         X,         LS_CLOSED, LS_IDLE, LS_LOADING, LS_COUNTDOWN, X,         X,          X,          LS_MIGRATING, X,         X },
    [LS_COUNTDOWN] = { X,           X,          X,       LS_CLOSED, X,         X,         LS_CLOSED, LS_IDLE, LS_LOADING, X,            LS_LOBBY,  LS_LOADING, X,          LS_MIGRATING, X,         X },
    [LS_LOADING]   = { X,           X,          X,       LS_CLOSED, X,         X,         LS_CLOSED, LS_IDLE, X,          X,            X,         X,          LS_IN_GAME, LS_CLOSED,    X,         X },
    [LS_IN_GAME]   = { X,           X,          X,       LS_CLOSED, X,         X,         LS_CLOSED, LS_IDLE, X,          X,            X,         X,          X,          LS_CLOSED,    X,         X },
    [LS_MIGRATING] = { X,           X,          X,       LS_CLOSED, LS_LOBBY,  LS_CLOSED, LS_CLOSED, LS_IDLE, X,          X,            X,         X,          X,          LS_CLOSED,    LS_LOBBY,  X },
    [LS_CLOSED]    = { X,           X,          X,       X,         X,         X,         X,         LS_IDLE, X,          X,            X,         X,          X,          X,            X,         LS_IDLE },
};
#undef X

LobbyState lobby_fsm_next(LobbyState s, LobbyEvent e) {
    if ((int)s < 0 || s >= LS_COUNT || (int)e < 0 || e >= LE_COUNT) return s;
    int n = k_fsm[s][e];
    return n < 0 ? s : (LobbyState)n;
}

bool lobby_fsm_accepts(LobbyState s, LobbyEvent e) {
    if ((int)s < 0 || s >= LS_COUNT || (int)e < 0 || e >= LE_COUNT) return false;
    return k_fsm[s][e] >= 0;
}

bool lobby_state_in_session(LobbyState s) { return s >= LS_CREATING && s <= LS_MIGRATING; }

// ------------------------------------------------------------------ model

static const char *const k_privacy[LP_COUNT] = { "Public", "Friends only", "Invite only" };
static const char *const k_difficulty[LD_COUNT] = { "Easy", "Normal", "Hard", "Nightmare" };
const char *lobby_privacy_name(int p) { return p >= 0 && p < LP_COUNT ? k_privacy[p] : "?"; }
const char *lobby_difficulty_name(int d) { return d >= 0 && d < LD_COUNT ? k_difficulty[d] : "?"; }

void lobby_settings_default(LobbySettings *s) {
    memset(s, 0, sizeof(*s));
    dg_strcpy(s->map, sizeof(s->map), "devroom");
    s->difficulty = LD_NORMAL;
    s->max_players = 4;
    s->privacy = LP_FRIENDS;
    s->start_hour = 9;
    s->season = 1;            // summer
    s->dynamic_weather = true;
    s->late_join = true;
}

void lobby_init(Lobby *L) {
    memset(L, 0, sizeof(*L));
    L->local_id = LOBBY_NONE;
    L->countdown = -1.0f;
    lobby_settings_default(&L->settings);
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) L->members[i].peer = -1;
}

LobbyMember *lobby_member(Lobby *L, u8 id) {
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (L->members[i].active && L->members[i].id == id) return &L->members[i];
    return NULL;
}

const LobbyMember *lobby_member_c(const Lobby *L, u8 id) { return lobby_member((Lobby *)L, id); }

LobbyMember *lobby_member_by_peer(Lobby *L, int peer) {
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (L->members[i].active && !L->members[i].host && L->members[i].peer == peer) return &L->members[i];
    return NULL;
}

static bool ieq_prefix(const char *a, const char *prefix, bool whole) {
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)prefix[i])) return false;
    return !whole || a[n] == 0;
}

LobbyMember *lobby_member_by_name(Lobby *L, const char *name) {
    if (!name || !name[0]) return NULL;
    LobbyMember *pre = NULL;
    int pre_count = 0;
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) {
        LobbyMember *m = &L->members[i];
        if (!m->active) continue;
        if (ieq_prefix(m->name, name, true)) return m;
        if (ieq_prefix(m->name, name, false)) { pre = m; pre_count++; }
    }
    return pre_count == 1 ? pre : NULL;
}

LobbyMember *lobby_host_member(Lobby *L) {
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (L->members[i].active && L->members[i].host) return &L->members[i];
    return NULL;
}

LobbyMember *lobby_local(Lobby *L) { return L->local_id == LOBBY_NONE ? NULL : lobby_member(L, L->local_id); }

int lobby_count(const Lobby *L) {
    int n = 0;
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (L->members[i].active) n++;
    return n;
}

bool lobby_all_ready(const Lobby *L) {
    int n = 0;
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) {
        if (!L->members[i].active) continue;
        if (!L->members[i].ready) return false;
        n++;
    }
    return n > 0;
}

bool lobby_is_banned(const Lobby *L, const char *ident) {
    if (!ident || !ident[0]) return false;
    for (int i = 0; i < L->ban_count; i++) if (!strcmp(L->bans[i], ident)) return true;
    return false;
}

// ------------------------------------------------------------------ chat

void lobby_chat_push(Lobby *L, const LobbyChatLine *line) {
    int at = (L->chat_head + L->chat_count) % LOBBY_CHAT_LINES;
    if (L->chat_count == LOBBY_CHAT_LINES) { L->chat_head = (L->chat_head + 1) % LOBBY_CHAT_LINES; at = (L->chat_head + L->chat_count - 1) % LOBBY_CHAT_LINES; }
    else L->chat_count++;
    L->chat[at] = *line;
}

void lobby_chat_system(Lobby *L, double now, int kind, const char *fmt, ...) {
    LobbyChatLine line;
    memset(&line, 0, sizeof(line));
    line.time = now;
    line.from = LOBBY_SYSTEM;
    line.kind = (u8)kind;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line.text, sizeof(line.text), fmt, ap);
    va_end(ap);
    lobby_chat_push(L, &line);
}

const LobbyChatLine *lobby_chat_at(const Lobby *L, int i) {
    if (i < 0 || i >= L->chat_count) return NULL;
    return &L->chat[(L->chat_head + i) % LOBBY_CHAT_LINES];
}

int lobby_sanitize(char *text) {
    int n = 0;
    for (const char *p = text; *p && n < LOBBY_CHAT_MAX; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\t') c = ' ';
        if (c < 32 || c > 126) continue;
        text[n++] = (char)c;
    }
    text[n] = 0;
    int start = 0;
    while (text[start] == ' ') start++;
    if (start) { memmove(text, text + start, (size_t)(n - start + 1)); n -= start; }
    while (n > 0 && text[n - 1] == ' ') text[--n] = 0;
    return n;
}

// Stub list: the real one would come from a data file / a platform filter (Steam text filtering).
static const char *const k_profanity[] = { "fuck", "shit", "cunt", "bitch", "asshole", "bastard" };

int lobby_filter_profanity(char *text) {
    int hits = 0;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && isalnum((unsigned char)text[i - 1])) continue;   // whole words only
        for (int k = 0; k < ARRAY_COUNT(k_profanity); k++) {
            size_t n = strlen(k_profanity[k]);
            if (i + n > len || !ieq_prefix(text + i, k_profanity[k], false)) continue;
            if (i + n < len && isalnum((unsigned char)text[i + n])) continue;
            memset(text + i, '*', n);
            hits++;
            i += n - 1;
            break;
        }
    }
    return hits;
}

bool lobby_rate_allow(LobbyRate *r, double now) {
    if (r->last <= 0 && r->tokens <= 0) r->tokens = LOBBY_CHAT_BURST;   // first message
    else r->tokens = DG_MIN((double)LOBBY_CHAT_BURST, r->tokens + (now - r->last) / LOBBY_CHAT_REFILL);
    r->last = now;
    if (r->tokens < 1.0) return false;
    r->tokens -= 1.0;
    return true;
}

// ------------------------------------------------------------------ host

static int free_slot(const Lobby *L) {
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (!L->members[i].active) return i;
    return -1;
}

static LobbyMember *add_member(Lobby *L, const char *name, u64 steam_id, const char *ident, const char *address, int peer) {
    int s = free_slot(L);
    if (s < 0) return NULL;
    LobbyMember *m = &L->members[s];
    memset(m, 0, sizeof(*m));
    m->active = true;
    m->id = L->next_id++;
    if (L->next_id == LOBBY_NONE) L->next_id = 0;   // 255 joins in one session: wrap (LOBBY_NONE is reserved)
    m->join_order = L->next_join++;
    dg_strcpy(m->name, sizeof(m->name), name && name[0] ? name : "Player");
    m->steam_id = steam_id;
    dg_strcpy(m->ident, sizeof(m->ident), ident ? ident : "");
    dg_strcpy(m->address, sizeof(m->address), address ? address : "");
    m->peer = peer;
    memset(&L->rate[s], 0, sizeof(L->rate[s]));
    return m;
}

void lobby_host_begin(Lobby *L, const char *name, u64 steam_id, const char *ident, const char *address, const LobbySettings *s) {
    LobbyState st = L->state;
    lobby_init(L);
    L->state = st;
    L->is_host = true;
    if (s) L->settings = *s;
    L->settings.max_players = (u8)DG_CLAMP(L->settings.max_players, 1, LOBBY_MAX_MEMBERS);
    LobbyMember *m = add_member(L, name, steam_id, ident, address, -1);
    m->host = true;
    L->local_id = m->id;
    L->revision = 1;
}

u8 lobby_host_add(Lobby *L, const char *name, u64 steam_id, const char *ident, const char *address, int peer, double now, char *reason, int reason_size) {
    const char *why = NULL;
    if (lobby_is_banned(L, ident)) why = "You were kicked from this lobby";
    else if (lobby_count(L) >= L->settings.max_players) why = "Lobby full";
    else if ((L->state == LS_LOADING || L->state == LS_IN_GAME) && !L->settings.late_join) why = "Game in progress (late join disabled)";
    else if (L->state != LS_LOBBY && L->state != LS_COUNTDOWN && L->state != LS_LOADING && L->state != LS_IN_GAME) why = "Lobby not open";
    LobbyMember *m = why ? NULL : add_member(L, name, steam_id, ident, address, peer);
    if (!m && !why) why = "Lobby full";
    if (why) { if (reason) dg_strcpy(reason, (size_t)reason_size, why); return LOBBY_NONE; }
    // Two members may share a display name (LAN defaults): number the newcomer.
    for (int i = 0, k = 2; i < LOBBY_MAX_MEMBERS; i++) {
        LobbyMember *o = &L->members[i];
        if (o == m || !o->active || strcmp(o->name, m->name)) continue;
        char nn[LOBBY_NAME_MAX];
        snprintf(nn, sizeof(nn), "%.26s (%d)", name && name[0] ? name : "Player", k++);
        dg_strcpy(m->name, sizeof(m->name), nn);
        i = -1;   // re-check against everyone
    }
    L->revision++;
    lobby_chat_system(L, now, LC_SYSTEM, "%s joined", m->name);
    return m->id;
}

void lobby_host_remove(Lobby *L, u8 id, double now, const char *why) {
    LobbyMember *m = lobby_member(L, id);
    if (!m) return;
    lobby_chat_system(L, now, LC_SYSTEM, "%s %s", m->name, why && why[0] ? why : "left");
    memset(m, 0, sizeof(*m));
    m->peer = -1;
    L->revision++;
}

static bool settings_valid(const LobbySettings *s) {
    return s->map[0] && s->difficulty < LD_COUNT && s->max_players >= 1 && s->max_players <= LOBBY_MAX_MEMBERS && s->privacy < LP_COUNT && s->start_hour < 24 &&
           s->season < 4;
}

static void deny(LobbyOutcome *o, int result, const char *msg) {
    o->result = result;
    dg_strcpy(o->error, sizeof(o->error), msg);
}

LobbyOutcome lobby_host_intent(Lobby *L, u8 from, const LobbyIntent *in, double now, LobbyMapCheck map_ok, void *user) {
    LobbyOutcome o;
    memset(&o, 0, sizeof(o));
    o.kicked = o.promoted = LOBBY_NONE;
    LobbyMember *m = lobby_member(L, from);
    if (!m) { deny(&o, LR_INVALID, "Not in the lobby"); return o; }
    bool pre_game = L->state == LS_LOBBY || L->state == LS_COUNTDOWN;
    switch (in->type) {
    case LI_READY: {
        if (!pre_game) { deny(&o, LR_INVALID, "The game already started"); break; }
        bool v = in->a != 0;
        if (m->ready == v) { o.result = LR_IGNORED; break; }
        m->ready = v;
        o.changed = true;
        lobby_chat_system(L, now, LC_SYSTEM, "%s is %s", m->name, v ? "ready" : "not ready");
        o.chat = true;
        // Everyone ready during a forced countdown: start at once.
        if (L->state == LS_COUNTDOWN && lobby_all_ready(L)) o.start = true;
        break;
    }
    case LI_CHAT: {
        char text[LOBBY_CHAT_MAX + 1];
        dg_strcpy(text, sizeof(text), in->text);
        if (!lobby_sanitize(text)) { o.result = LR_IGNORED; break; }
        int slot = (int)(m - L->members);
        if (!lobby_rate_allow(&L->rate[slot], now)) { deny(&o, LR_RATE_LIMITED, "You are sending messages too fast"); break; }
        lobby_filter_profanity(text);
        LobbyChatLine line;
        memset(&line, 0, sizeof(line));
        line.time = now;
        line.from = m->id;
        line.kind = LC_PLAYER;
        line.color = m->id;
        dg_strcpy(line.name, sizeof(line.name), m->name);
        dg_strcpy(line.text, sizeof(line.text), text);
        lobby_chat_push(L, &line);
        o.chat = true;
        break;
    }
    case LI_MAP:
        if (!m->host) { deny(&o, LR_DENIED, "Only the host can change the map"); break; }
        if (!pre_game) { deny(&o, LR_INVALID, "The game already started"); break; }
        if (!in->text[0] || (map_ok && !map_ok(in->text, user))) { deny(&o, LR_INVALID, "Unknown map"); break; }
        if (!strcmp(L->settings.map, in->text)) { o.result = LR_IGNORED; break; }
        dg_strcpy(L->settings.map, sizeof(L->settings.map), in->text);
        o.changed = true;
        lobby_chat_system(L, now, LC_SYSTEM, "Map: %s", in->text);
        o.chat = true;
        break;
    case LI_SETTINGS: {
        if (!m->host) { deny(&o, LR_DENIED, "Only the host can change the settings"); break; }
        if (!pre_game) { deny(&o, LR_INVALID, "The game already started"); break; }
        LobbySettings s = in->settings;
        if (!settings_valid(&s) || (map_ok && !map_ok(s.map, user))) { deny(&o, LR_INVALID, "Invalid settings"); break; }
        if (s.max_players < lobby_count(L)) s.max_players = (u8)lobby_count(L);   // never below the players already in
        if (!memcmp(&s, &L->settings, sizeof(s))) { o.result = LR_IGNORED; break; }
        bool map_changed = strcmp(s.map, L->settings.map) != 0;
        L->settings = s;
        o.changed = true;
        if (map_changed) { lobby_chat_system(L, now, LC_SYSTEM, "Map: %s", s.map); o.chat = true; }
        break;
    }
    case LI_KICK: {
        if (!m->host) { deny(&o, LR_DENIED, "Only the host can kick"); break; }
        LobbyMember *t = lobby_member(L, in->a);
        if (!t || t == m) { deny(&o, LR_INVALID, "No such player"); break; }
        if (t->ident[0] && L->ban_count < LOBBY_MAX_BANS && !lobby_is_banned(L, t->ident)) dg_strcpy(L->bans[L->ban_count++], sizeof(L->bans[0]), t->ident);
        o.kicked = t->id;
        lobby_host_remove(L, t->id, now, "was kicked");
        o.changed = o.chat = true;
        break;
    }
    case LI_PROMOTE: {
        if (!m->host) { deny(&o, LR_DENIED, "Only the host can promote"); break; }
        if (!pre_game) { deny(&o, LR_INVALID, "Host changes only in the lobby"); break; }
        LobbyMember *t = lobby_member(L, in->a);
        if (!t || t == m) { deny(&o, LR_INVALID, "No such player"); break; }
        o.promoted = t->id;   // the glue migrates: everyone reconnects to the new host
        lobby_chat_system(L, now, LC_SYSTEM, "%s is the new host", t->name);
        o.chat = true;
        break;
    }
    case LI_START:
        if (!m->host) { deny(&o, LR_DENIED, "Only the host can start"); break; }
        if (L->state == LS_COUNTDOWN) { if (lobby_all_ready(L)) o.start = true; else o.result = LR_IGNORED; break; }
        if (L->state != LS_LOBBY) { deny(&o, LR_INVALID, "Not in the lobby"); break; }
        if (lobby_all_ready(L)) { o.start = true; break; }
        if (!in->a) { deny(&o, LR_INVALID, "Not everyone is ready"); break; }
        L->countdown = LOBBY_COUNTDOWN;
        o.countdown = o.changed = true;
        lobby_chat_system(L, now, LC_SYSTEM, "Starting in %.0f seconds", (double)LOBBY_COUNTDOWN);
        o.chat = true;
        break;
    case LI_CANCEL:
        if (!m->host) { deny(&o, LR_DENIED, "Only the host can cancel"); break; }
        if (L->state != LS_COUNTDOWN) { o.result = LR_IGNORED; break; }
        L->countdown = -1.0f;
        o.changed = true;
        lobby_chat_system(L, now, LC_SYSTEM, "Start cancelled");
        o.chat = true;
        break;
    default: deny(&o, LR_INVALID, "Unknown request"); break;
    }
    if (o.changed) L->revision++;
    return o;
}

bool lobby_tick(Lobby *L, float dt) {
    if (L->state != LS_COUNTDOWN || L->countdown < 0) return false;
    L->countdown -= dt;
    if (L->countdown > 0) return false;
    L->countdown = -1.0f;
    return true;
}

// ------------------------------------------------------------------ helpers

static const char *skip_ws(const char *p) { while (*p == ' ') p++; return p; }

bool lobby_parse_chat(Lobby *L, const char *text, LobbyIntent *out, char *err, int err_size) {
    memset(out, 0, sizeof(*out));
    if (err && err_size) err[0] = 0;
    const char *t = skip_ws(text);
    if (t[0] != '/') {
        out->type = LI_CHAT;
        dg_strcpy(out->text, sizeof(out->text), text);
        return true;
    }
    char cmd[16] = "";
    int n = 0;
    for (const char *p = t + 1; *p && *p != ' ' && n < (int)sizeof(cmd) - 1; p++) cmd[n++] = (char)tolower((unsigned char)*p);
    cmd[n] = 0;
    const char *arg = skip_ws(t + 1 + strlen(cmd));
    if (!strcmp(cmd, "ready")) { out->type = LI_READY; out->a = 1; return true; }
    if (!strcmp(cmd, "unready")) { out->type = LI_READY; out->a = 0; return true; }
    if (!strcmp(cmd, "start")) { out->type = LI_START; out->a = !strcmp(arg, "force") ? 1 : 0; return true; }
    if (!strcmp(cmd, "cancel")) { out->type = LI_CANCEL; return true; }
    if (!strcmp(cmd, "map")) {
        if (!arg[0]) { if (err) dg_strcpy(err, (size_t)err_size, "Usage: /map <id>"); return false; }
        out->type = LI_MAP;
        dg_strcpy(out->text, sizeof(out->text), arg);
        return true;
    }
    if (!strcmp(cmd, "kick") || !strcmp(cmd, "promote")) {
        LobbyMember *m = lobby_member_by_name(L, arg);
        if (!m) { if (err) snprintf(err, (size_t)err_size, "No player named '%s'", arg); return false; }
        out->type = !strcmp(cmd, "kick") ? LI_KICK : LI_PROMOTE;
        out->a = m->id;
        return true;
    }
    if (err) snprintf(err, (size_t)err_size, "Unknown command /%s (/ready /unready /map /kick /promote /start /cancel)", cmd);
    return false;
}

u32 lobby_version_hash(const char *version, u32 protocol, u32 content_hash) {
    u32 h = dg_hash_str(version ? version : "");
    h = dg_hash_u32(h ^ (protocol * 2654435761u));
    h = dg_hash_u32(h ^ content_hash);
    return h ? h : 1u;
}

bool lobby_check_version(u32 host_hash, const char *host_version, u32 client_hash, const char *client_version, char *reason, int reason_size) {
    if (host_hash == client_hash) return true;
    if (reason) {
        if (strcmp(host_version ? host_version : "", client_version ? client_version : ""))
            snprintf(reason, (size_t)reason_size, "Version mismatch: host %s, you %s", host_version ? host_version : "?", client_version ? client_version : "?");
        else snprintf(reason, (size_t)reason_size, "Game data mismatch (host %08x, you %08x): verify your game files", host_hash, client_hash);
    }
    return false;
}

u8 lobby_successor(const Lobby *L, u8 leaving) {
    const LobbyMember *best = NULL;
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) {
        const LobbyMember *m = &L->members[i];
        if (!m->active || m->id == leaving) continue;
        if (!best || m->join_order < best->join_order) best = m;
    }
    return best ? best->id : LOBBY_NONE;
}

static void kv(LobbyKV *out, int max, int *n, const char *k, const char *fmt, ...) {
    if (*n >= max) return;
    dg_strcpy(out[*n].key, sizeof(out[*n].key), k);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out[*n].value, sizeof(out[*n].value), fmt, ap);
    va_end(ap);
    (*n)++;
}

int lobby_metadata(const Lobby *L, const char *host_name, u32 version_hash, LobbyKV *out, int max) {
    int n = 0;
    kv(out, max, &n, "game", "deadground");
    kv(out, max, &n, "name", "%s", host_name ? host_name : "");
    kv(out, max, &n, "map", "%s", L->settings.map);
    kv(out, max, &n, "players", "%d", lobby_count(L));
    kv(out, max, &n, "max", "%d", L->settings.max_players);
    kv(out, max, &n, "version", "%08x", version_hash);
    kv(out, max, &n, "mode", "coop");
    kv(out, max, &n, "state", "%s", L->state == LS_IN_GAME || L->state == LS_LOADING ? "in_game" : "lobby");
    kv(out, max, &n, "privacy", "%d", L->settings.privacy);
    kv(out, max, &n, "difficulty", "%d", L->settings.difficulty);
    kv(out, max, &n, "late_join", "%d", L->settings.late_join ? 1 : 0);
    kv(out, max, &n, "season", "%d", L->settings.season);
    return n;
}

void lobby_rich_presence(const Lobby *L, const char *map_name, char *out, int size) {
    const char *map = map_name && map_name[0] ? map_name : L->settings.map;
    if (L->state == LS_IN_GAME || L->state == LS_LOADING) snprintf(out, (size_t)size, "In game - %s", map);
    else if (lobby_state_in_session(L->state)) snprintf(out, (size_t)size, "In lobby - %d/%d - %s", lobby_count(L), L->settings.max_players, map);
    else snprintf(out, (size_t)size, "In the main menu");
}

// ------------------------------------------------------------------ wire

void lobby_write_settings(Writer *w, const LobbySettings *s) {
    w_str(w, s->map);
    w_u8(w, s->difficulty);
    w_u8(w, s->max_players);
    w_u8(w, s->privacy);
    w_u8(w, s->start_hour);
    w_u8(w, s->season);
    w_str(w, s->weather);
    w_u8(w, (u8)((s->pvp ? 1 : 0) | (s->late_join ? 2 : 0) | (s->dynamic_weather ? 4 : 0)));
}

void lobby_read_settings(Reader *r, LobbySettings *s) {
    memset(s, 0, sizeof(*s));
    r_str(r, s->map, sizeof(s->map));
    s->difficulty = r_u8(r);
    s->max_players = r_u8(r);
    s->privacy = r_u8(r);
    s->start_hour = r_u8(r);
    s->season = r_u8(r);
    r_str(r, s->weather, sizeof(s->weather));
    u8 f = r_u8(r);
    s->pvp = (f & 1) != 0;
    s->late_join = (f & 2) != 0;
    s->dynamic_weather = (f & 4) != 0;
}

void lobby_write_snapshot(Writer *w, const Lobby *L, u8 to_id) {
    w_u8(w, LOP_SNAPSHOT);
    w_u32(w, L->revision);
    w_u8(w, to_id);
    w_u8(w, (u8)L->state);
    w_f32(w, L->countdown);
    lobby_write_settings(w, &L->settings);
    w_u8(w, (u8)lobby_count(L));
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) {
        const LobbyMember *m = &L->members[i];
        if (!m->active) continue;
        w_u8(w, m->id);
        w_u8(w, (u8)((m->ready ? 1 : 0) | (m->host ? 2 : 0) | (m->talking ? 4 : 0)));
        w_u32(w, m->join_order);
        w_str(w, m->name);
        w_u32(w, (u32)(m->steam_id & 0xFFFFFFFFu));
        w_u32(w, (u32)(m->steam_id >> 32));
        w_str(w, m->address);
        w_u16(w, m->ping_ms);
    }
}

bool lobby_read_snapshot(Reader *r, Lobby *L) {
    u32 rev = r_u32(r);
    u8 you = r_u8(r);
    u8 host_state = r_u8(r);
    float cd = r_f32(r);
    LobbySettings s;
    lobby_read_settings(r, &s);
    int n = r_u8(r);
    if (r->overflow || n > LOBBY_MAX_MEMBERS || host_state >= LS_COUNT) return false;
    LobbyMember mem[LOBBY_MAX_MEMBERS];
    memset(mem, 0, sizeof(mem));
    for (int i = 0; i < n; i++) {
        LobbyMember *m = &mem[i];
        m->active = true;
        m->peer = -1;
        m->id = r_u8(r);
        u8 f = r_u8(r);
        m->ready = (f & 1) != 0;
        m->host = (f & 2) != 0;
        m->talking = (f & 4) != 0;
        m->join_order = r_u32(r);
        r_str(r, m->name, sizeof(m->name));
        u32 lo = r_u32(r), hi = r_u32(r);
        m->steam_id = (u64)lo | ((u64)hi << 32);
        r_str(r, m->address, sizeof(m->address));
        m->ping_ms = r_u16(r);
    }
    if (r->overflow) return false;
    for (int i = n; i < LOBBY_MAX_MEMBERS; i++) mem[i].peer = -1;
    memcpy(L->members, mem, sizeof(mem));
    L->settings = s;
    L->revision = rev;
    L->local_id = you;
    L->countdown = host_state == LS_COUNTDOWN ? cd : -1.0f;
    // The host's pre-game phase drives ours (countdown begins / is cancelled).
    if (L->state == LS_LOBBY && host_state == LS_COUNTDOWN) L->state = lobby_fsm_next(L->state, LE_FORCE_START);
    else if (L->state == LS_COUNTDOWN && host_state == LS_LOBBY) L->state = lobby_fsm_next(L->state, LE_COUNTDOWN_CANCEL);
    return true;
}

void lobby_write_chat(Writer *w, const LobbyChatLine *line) {
    w_u8(w, LOP_CHAT);
    w_u8(w, line->from);
    w_u8(w, line->kind);
    w_u8(w, line->color);
    w_str(w, line->name);
    u16 n = (u16)DG_MIN(strlen(line->text), (size_t)LOBBY_CHAT_MAX);
    w_u16(w, n);
    w_bytes(w, line->text, n);
}

bool lobby_read_chat(Reader *r, LobbyChatLine *line) {
    memset(line, 0, sizeof(*line));
    line->from = r_u8(r);
    line->kind = r_u8(r);
    line->color = r_u8(r);
    r_str(r, line->name, sizeof(line->name));
    u16 n = r_u16(r);
    if (n > LOBBY_CHAT_MAX) { r->overflow = true; return false; }
    r_bytes(r, line->text, n);
    line->text[n] = 0;
    return !r->overflow;
}

void lobby_write_intent(Writer *w, const LobbyIntent *in) {
    w_u8(w, LOP_INTENT);
    w_u8(w, in->type);
    w_u8(w, in->a);
    u16 n = (u16)DG_MIN(strlen(in->text), (size_t)LOBBY_CHAT_MAX);
    w_u16(w, n);
    w_bytes(w, in->text, n);
    if (in->type == LI_SETTINGS) lobby_write_settings(w, &in->settings);
}

bool lobby_read_intent(Reader *r, LobbyIntent *in) {
    memset(in, 0, sizeof(*in));
    in->type = r_u8(r);
    in->a = r_u8(r);
    u16 n = r_u16(r);
    if (n > LOBBY_CHAT_MAX) { r->overflow = true; return false; }   // over the cap: refused, not cut
    r_bytes(r, in->text, n);
    in->text[n] = 0;
    if (in->type == LI_SETTINGS) lobby_read_settings(r, &in->settings);
    return !r->overflow;
}
