// Ping system core (docs/UI_WHEELS_HUD.md "Pings"): ping types (assets/ui/wheels/ping.json), the
// list of live pings, the per-player cap and the host's rate limiter. Pure (tests/test_hud.c).
//
// Flow: a client asks the host (RPC_PING: type + position); the host rate-limits it, gives it a
// uid and broadcasts EV_PING. Every machine applies the same event to its own PingList, so the
// cap (3 per player, the oldest replaced) and the lifetimes agree without more traffic. A tap on
// one of your own pings removes it (EV_PING with a negative lifetime).
#ifndef DG_PING_H
#define DG_PING_H

#include "core/base.h"
#include "core/dgmath.h"

#define PING_TYPES 8
#define PING_PER_PLAYER 3
#define PING_MAX_PLAYERS 8
#define PING_MAX (PING_MAX_PLAYERS * PING_PER_PLAYER)
#define PING_RATE_WINDOW 3.0f   // seconds
#define PING_RATE_COUNT 4       // pings per window
#define PING_RATE_MIN_GAP 0.25f // seconds between two pings

typedef enum PingType { PING_GENERIC, PING_ENEMY, PING_LOOT, PING_DANGER, PING_GO, PING_HELP, PING_VEHICLE, PING_DEFEND } PingType;

typedef struct PingTypeDef {
    char id[16];
    char label[32];
    char icon[24];
    char chat[48];          // "spotted an enemy" (chat line: "<name> spotted an enemy")
    char sound[24];         // audio hook (assets/sounds/<sound>.wav)
    float lifetime;         // seconds
    int color;              // theme ping colour index
} PingTypeDef;

typedef struct PingTypes { PingTypeDef t[PING_TYPES]; } PingTypes;

void ping_types_defaults(PingTypes *pt);
// Reads the "slots" of the ping wheel file (same order as PingType, by id). Unknown ids fail.
bool ping_types_parse(PingTypes *pt, const char *json, char *err, int err_size);
int ping_type_from_id(const char *id);
const char *ping_type_id(int type);

typedef struct Ping {
    bool active;
    u32 uid;
    u8 owner;               // player slot
    u8 type;
    vec3 pos;
    float born, expires;    // world time
} Ping;

typedef struct PingList { Ping p[PING_MAX]; } PingList;

// Adds (or replaces the same uid); evicts the owner's oldest when it already has PING_PER_PLAYER.
Ping *ping_add(PingList *l, u32 uid, int owner, int type, vec3 pos, float now, float lifetime);
bool ping_remove(PingList *l, u32 uid);
void ping_expire(PingList *l, float now);
int ping_count_owner(const PingList *l, int owner);
int ping_count(const PingList *l);
// The owner's ping within radius of pos (tap again on your ping removes it), or NULL.
const Ping *ping_find_near(const PingList *l, int owner, vec3 pos, float radius);
// 0..1 fade at the end of the lifetime (last 1.5 s) and in over the first 0.2 s.
float ping_alpha(const Ping *p, float now);

typedef struct PingLimiter { float t[PING_MAX_PLAYERS][PING_RATE_COUNT]; int n[PING_MAX_PLAYERS]; } PingLimiter;
// True (and records it) when the player may ping at time now.
bool ping_rate_ok(PingLimiter *lim, int player, float now);
void ping_rate_reset(PingLimiter *lim, int player);

// Contextual ping (tap): what the crosshair is on. hit_kind: 0 nothing / world, 1 enemy (zombie,
// hostile), 2 loot (pickup, container), 3 vehicle.
DG_INLINE int ping_contextual_type(int hit_kind) {
    switch (hit_kind) {
    case 1: return PING_ENEMY;
    case 2: return PING_LOOT;
    case 3: return PING_VEHICLE;
    default: return PING_GENERIC;
    }
}

// RPC_PING wire packing: a = type | (y in cm + 32768) << 8 (16 bits), b = x cm, c = z cm.
void ping_pack(int type, vec3 pos, int *a, int *b, int *c);
void ping_unpack(int a, int b, int c, int *type, vec3 *pos);

#endif
