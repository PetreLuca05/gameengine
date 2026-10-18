// Lobby (docs/MENU_LOBBY.md): the pre-game session every multiplayer game goes through. Pure: no
// SDL, no transport, no game (tests/test_lobby.c). The glue to the net layer, Steam and the UI is
// src/net/lobby_net.c.
//
//   * state machine: lobby_fsm_next (IDLE, CREATING, JOINING, LOBBY, COUNTDOWN, LOADING, IN_GAME,
//     MIGRATING, CLOSED); every machine runs it for its own session;
//   * host-authoritative model: members (ready, host, ping), settings (map, difficulty, privacy,
//     start time / weather, late join), chat, kick bans. Clients send intents (lobby_host_intent
//     validates them on the host), the host broadcasts snapshots and chat lines;
//   * wire format: tiny reliable MSG_LOBBY messages (lobby_write_* / lobby_read_*);
//   * helpers: chat rate limit, slash commands, profanity filter stub, version check, migration
//     successor, Steam lobby metadata.
#ifndef DG_LOBBY_H
#define DG_LOBBY_H

#include "core/base.h"
#include "net/bitbuf.h"

#define LOBBY_MAX_MEMBERS 8          // = MAX_PLAYERS (world.h)
#define LOBBY_MAX_BANS 32
#define LOBBY_CHAT_MAX 256           // characters per message (longer input is cut)
#define LOBBY_CHAT_LINES 128         // scrollback
#define LOBBY_CHAT_BURST 4           // messages allowed back to back ...
#define LOBBY_CHAT_REFILL 1.0        // ... then one more per this many seconds
#define LOBBY_COUNTDOWN 10.0f        // forced start ("Start" before everyone is ready)
#define LOBBY_NAME_MAX 32
#define LOBBY_SYSTEM 0xFF            // chat line "from" for system messages
#define LOBBY_NONE 0xFF              // member id: none

// ------------------------------------------------------------------ state machine

typedef enum LobbyState {
    LS_IDLE,          // main menu, no session
    LS_CREATING,      // host: transport / Steam lobby being created
    LS_JOINING,       // client: connecting, hello sent, waiting for the first snapshot
    LS_LOBBY,         // in the lobby (host or member)
    LS_COUNTDOWN,     // forced start: LOBBY_COUNTDOWN seconds, then LOADING
    LS_LOADING,       // the map is loading (host first, then every client)
    LS_IN_GAME,       // playing
    LS_MIGRATING,     // the host left the lobby: the successor hosts, the others reconnect
    LS_CLOSED,        // the session ended with a reason (kicked, rejected, host left): shown, then IDLE
    LS_COUNT
} LobbyState;

typedef enum LobbyEvent {
    LE_HOST,              // "Host game"
    LE_JOIN,              // browser / direct IP / invite
    LE_TRANSPORT_UP,      // host: listening (and the Steam lobby exists)
    LE_TRANSPORT_FAIL,    // could not listen / connect, connection timed out
    LE_SNAPSHOT,          // client: the host accepted us (first lobby snapshot)
    LE_REJECTED,          // version / asset mismatch, banned, full, game running without late join
    LE_KICKED,
    LE_LEAVE,             // the local player leaves (Leave, Quit, Leave to main menu)
    LE_START,             // everyone ready and the host pressed Start (or the host's start reached us)
    LE_FORCE_START,       // the host pressed Start before everyone was ready
    LE_COUNTDOWN_CANCEL,  // the host cancelled the countdown
    LE_COUNTDOWN_DONE,
    LE_LOAD_DONE,         // the map is loaded and the local player spawned
    LE_HOST_LOST,         // the connection to the host dropped (or the host left)
    LE_MIGRATED,          // the new host is up / we reconnected to it
    LE_ACK,               // the closed message was dismissed
    LE_COUNT
} LobbyEvent;

const char *lobby_state_name(int s);
const char *lobby_event_name(int e);
// The next state; s itself when the event does not apply in s (ignored).
LobbyState lobby_fsm_next(LobbyState s, LobbyEvent e);
bool lobby_fsm_accepts(LobbyState s, LobbyEvent e);
bool lobby_state_in_session(LobbyState s);   // CREATING .. MIGRATING

// ------------------------------------------------------------------ model

typedef enum LobbyPrivacy { LP_PUBLIC, LP_FRIENDS, LP_INVITE, LP_COUNT } LobbyPrivacy;
typedef enum LobbyDifficulty { LD_EASY, LD_NORMAL, LD_HARD, LD_NIGHTMARE, LD_COUNT } LobbyDifficulty;

typedef struct LobbySettings {
    char map[32];             // assets/maps/<id>.json
    u8 difficulty;            // LobbyDifficulty
    u8 max_players;           // 1 .. LOBBY_MAX_MEMBERS
    u8 privacy;               // LobbyPrivacy (the Steam lobby type)
    u8 start_hour;            // 0 .. 23
    u8 season;                // Season (src/world/season.h): calendar day + weather bias
    bool dynamic_weather;     // the season-biased scheduler changes the weather over time
    char weather[24];         // start weather preset ("" = a weighted pick of the season's presets)
    bool pvp;                 // stub
    bool late_join;           // players may join a running game
    u8 foliage_pct;           // server rule: foliage density for everyone (LOBBY_FOLIAGE_MIN .. 100 %, docs/SETTINGS_AUDIT.md)
} LobbySettings;
#define LOBBY_FOLIAGE_MIN 50

typedef struct LobbyMember {
    bool active;
    bool ready;
    bool host;
    bool talking;             // mic icon (stub)
    bool phone;               // a real phone is paired with this member's game (cosmetic icon, docs/PHONE.md)
    u8 id;                    // stable for the session (never reused)
    u32 join_order;           // migration order
    char name[LOBBY_NAME_MAX];
    u64 steam_id;             // 0 = LAN
    char ident[64];           // kick / ban key: "steam:<id>" or "lan:<ip>"
    char address[64];         // where to reach this member if it becomes the host (migration)
    u16 ping_ms;
    int peer;                 // host only (not replicated): transport peer, -1 = the host itself
} LobbyMember;

typedef enum LobbyChatKind { LC_PLAYER, LC_SYSTEM, LC_ERROR } LobbyChatKind;

typedef struct LobbyChatLine {
    double time;              // local receive time (timestamps)
    u8 from;                  // member id, LOBBY_SYSTEM
    u8 kind;                  // LobbyChatKind
    u8 color;                 // name colour index (member id)
    char name[LOBBY_NAME_MAX];
    char text[LOBBY_CHAT_MAX + 1];
} LobbyChatLine;

typedef struct LobbyRate { double tokens; double last; } LobbyRate;

typedef struct Lobby {
    LobbyState state;
    bool is_host;
    u8 local_id;              // our member id (LOBBY_NONE until the first snapshot)
    u32 revision;             // bumped on every replicated change (host)
    LobbySettings settings;
    LobbyMember members[LOBBY_MAX_MEMBERS];
    u8 next_id;
    u32 next_join;
    float countdown;          // seconds left (COUNTDOWN), < 0 none
    char bans[LOBBY_MAX_BANS][64];
    int ban_count;
    LobbyChatLine chat[LOBBY_CHAT_LINES];   // ring
    int chat_head, chat_count;
    LobbyRate rate[LOBBY_MAX_MEMBERS];      // per member slot (host)
    char close_reason[128];
    u64 steam_lobby;          // 0 = LAN / none
} Lobby;

void lobby_init(Lobby *L);
void lobby_settings_default(LobbySettings *s);
const char *lobby_privacy_name(int p);
const char *lobby_difficulty_name(int d);

LobbyMember *lobby_member(Lobby *L, u8 id);
const LobbyMember *lobby_member_c(const Lobby *L, u8 id);
LobbyMember *lobby_member_by_peer(Lobby *L, int peer);
LobbyMember *lobby_member_by_name(Lobby *L, const char *name);   // case-insensitive, prefix if unique
LobbyMember *lobby_host_member(Lobby *L);
LobbyMember *lobby_local(Lobby *L);
int lobby_count(const Lobby *L);
bool lobby_all_ready(const Lobby *L);
bool lobby_is_banned(const Lobby *L, const char *ident);

// Chat scrollback (both sides). i = 0 is the oldest line kept.
void lobby_chat_push(Lobby *L, const LobbyChatLine *line);
void lobby_chat_system(Lobby *L, double now, int kind, const char *fmt, ...);
const LobbyChatLine *lobby_chat_at(const Lobby *L, int i);

// ------------------------------------------------------------------ host

// Starts a lobby with the local player as its host (member id 0).
void lobby_host_begin(Lobby *L, const char *name, u64 steam_id, const char *ident, const char *address, const LobbySettings *s);
// A player asks to join. Returns the new member id, or LOBBY_NONE with the reason (banned, full,
// game running without late join).
u8 lobby_host_add(Lobby *L, const char *name, u64 steam_id, const char *ident, const char *address, int peer, double now, char *reason, int reason_size);
// A member left (disconnect or Leave): system line, readiness of a running countdown re-checked.
void lobby_host_remove(Lobby *L, u8 id, double now, const char *why);

typedef enum LobbyIntentType {
    LI_READY = 1,     // a = 0 / 1
    LI_CHAT,          // text
    LI_MAP,           // text = map id
    LI_SETTINGS,      // settings
    LI_KICK,          // a = member id
    LI_PROMOTE,       // a = member id
    LI_START,         // a = 1: force (countdown) when not everyone is ready
    LI_CANCEL,        // cancel the countdown
    LI_PHONE,         // a = 0 / 1: the member's phone is connected (any state, cosmetic)
} LobbyIntentType;

typedef struct LobbyIntent {
    u8 type;
    u8 a;
    char text[LOBBY_CHAT_MAX + 1];
    LobbySettings settings;
} LobbyIntent;

typedef enum LobbyResult {
    LR_OK,
    LR_IGNORED,       // nothing changed (already ready, same map ...)
    LR_DENIED,        // host-only action from a member
    LR_INVALID,       // unknown map / member, bad value, wrong state
    LR_RATE_LIMITED,  // chat
} LobbyResult;

typedef struct LobbyOutcome {
    int result;                 // LobbyResult
    bool changed;               // replicated state changed: broadcast a snapshot
    bool chat;                  // a chat line was added (relay it: the newest line)
    u8 kicked;                  // member id to disconnect, LOBBY_NONE
    u8 promoted;                // member id that becomes the host (migration), LOBBY_NONE
    bool start;                 // load the map now
    bool countdown;             // a forced-start countdown began
    char error[96];             // for the sender (LR_DENIED / INVALID / RATE_LIMITED)
} LobbyOutcome;

// map_ok(map, user): does this map exist (the host's map list)? NULL = any non-empty id.
typedef bool (*LobbyMapCheck)(const char *map, void *user);
LobbyOutcome lobby_host_intent(Lobby *L, u8 from, const LobbyIntent *in, double now, LobbyMapCheck map_ok, void *user);
// Countdown: true when it ran out this call (start the game).
bool lobby_tick(Lobby *L, float dt);
// Token bucket: true = the message may be sent (consumes a token).
bool lobby_rate_allow(LobbyRate *r, double now);

// ------------------------------------------------------------------ helpers

// "/kick name", "/ready", "/unready", "/map id", "/start", "/cancel", "/promote name": parsed into
// an intent. Returns false for an unknown command (err set); not a command: the chat intent.
bool lobby_parse_chat(Lobby *L, const char *text, LobbyIntent *out, char *err, int err_size);
// Replaces listed words (case-insensitive, whole words) with asterisks; returns the count (stub list).
int lobby_filter_profanity(char *text);
// Printable ASCII only, trimmed, at most LOBBY_CHAT_MAX characters. Returns the new length.
int lobby_sanitize(char *text);

// Build identity: the game version + protocol + content (maps, cosmetics, assets) hash.
u32 lobby_version_hash(const char *version, u32 protocol, u32 content_hash);
// false + a readable reason when a client may not join this host.
bool lobby_check_version(u32 host_hash, const char *host_version, u32 client_hash, const char *client_version, char *reason, int reason_size);

// The member that takes over when `leaving` (the host) goes: the earliest joined of the others.
u8 lobby_successor(const Lobby *L, u8 leaving);

// Steam lobby metadata (browser filters): key / value pairs, returns the count.
typedef struct LobbyKV { char key[24]; char value[64]; } LobbyKV;
int lobby_metadata(const Lobby *L, const char *host_name, u32 version_hash, LobbyKV *out, int max);
// "In lobby - 3/4 - Forest" / "In game - Dev Room" (map_name: display name).
void lobby_rich_presence(const Lobby *L, const char *map_name, char *out, int size);

// ------------------------------------------------------------------ wire (MSG_LOBBY payload)

typedef enum LobbyOp {
    LOP_SNAPSHOT = 1,  // host -> client: full state (members, settings, countdown, your id)
    LOP_CHAT,          // host -> client: one chat line
    LOP_INTENT,        // client -> host
    LOP_START,         // host -> client: the map is loading
    LOP_KICKED,        // host -> client: reason
    LOP_MIGRATE,       // host -> client: the host is leaving; successor id + its address
    LOP_ERROR,         // host -> client: an intent was refused (text)
} LobbyOp;

void lobby_write_snapshot(Writer *w, const Lobby *L, u8 to_id);
// Client: replaces members / settings / countdown, keeps the local chat and state.
bool lobby_read_snapshot(Reader *r, Lobby *L);
void lobby_write_chat(Writer *w, const LobbyChatLine *line);
bool lobby_read_chat(Reader *r, LobbyChatLine *line);
void lobby_write_intent(Writer *w, const LobbyIntent *in);
bool lobby_read_intent(Reader *r, LobbyIntent *in);
void lobby_write_settings(Writer *w, const LobbySettings *s);
void lobby_read_settings(Reader *r, LobbySettings *s);

#endif
