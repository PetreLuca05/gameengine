// Phone pairing authentication (pure: time and randomness are passed in, unit tested).
//
//   pairing token  one-time, 128-bit, base64url, expires (default 2 min). Encoded in the QR URL.
//                  Consumed by the first WebSocket that presents it.
//   session key    128-bit, issued when a token is consumed; lets the page reconnect (reload,
//                  iOS re-asks for motion permission on every load) and download its media until
//                  the player unpairs or pairs another phone. One phone (session) per owner.
//                  Optional lifetime (session_ttl_ms, sliding: every resume renews it); the game
//                  persists the key and restores it on the next launch (hws_auth_restore).
//   rate limit     per source IPv4: connection token bucket (burst 20, 10 / s) and failed-auth
//                  counter (5 failures within 60 s -> 60 s ban).
// Comparisons are constant time.
#ifndef DG_HWS_AUTH_H
#define DG_HWS_AUTH_H

#include "core/base.h"

#define HWS_KEY_CHARS 22          // base64url of 16 bytes, no padding
#define HWS_MAX_OWNERS 4          // local players that can pair a phone
#define HWS_RATE_SLOTS 64

typedef struct HwsPairing {
    bool pending;                 // a token is waiting to be scanned
    char token[HWS_KEY_CHARS + 1];
    u64 expires_ms;
    bool paired;                  // a session exists
    char session[HWS_KEY_CHARS + 1];
    u32 generation;               // bumps on every new session (the server drops older sockets)
    u64 session_expires_ms;       // 0 = no expiry
} HwsPairing;

typedef struct HwsRateSlot { u32 ip; float bucket; u64 last_ms; int fails; u64 fail_window_ms, ban_until_ms; } HwsRateSlot;

typedef struct HwsAuth {
    HwsPairing owner[HWS_MAX_OWNERS];
    u64 token_ttl_ms;
    u64 session_ttl_ms;           // 0 = sessions never expire
    HwsRateSlot rate[HWS_RATE_SLOTS];
} HwsAuth;

typedef enum HwsAuthResult { HWS_AUTH_OK, HWS_AUTH_BAD, HWS_AUTH_EXPIRED, HWS_AUTH_BANNED } HwsAuthResult;

void hws_auth_init(HwsAuth *a, u64 token_ttl_ms);
void hws_key_from_bytes(const u8 rnd[16], char out[HWS_KEY_CHARS + 1]);   // base64url, no padding
// New pairing token for an owner (replaces a pending one; an existing session stays valid until
// the new token is consumed). Returns the token.
const char *hws_auth_issue(HwsAuth *a, int owner, const u8 rnd[16], u64 now_ms);
// Consumes a pairing token: on success *owner receives the owner, a new session key is created
// from rnd (the previous phone's session is revoked) and copied to session_out.
HwsAuthResult hws_auth_pair(HwsAuth *a, const char *token, u64 now_ms, const u8 rnd[16], int *owner, char session_out[HWS_KEY_CHARS + 1]);
// Owner of a valid (unexpired) session key, or -1. touch: renew its lifetime (a resume).
int hws_auth_session(HwsAuth *a, const char *session, u64 now_ms, bool touch);
// A stored session (the previous run's paired phone): valid for ttl_ms (0 = no expiry). Replaces
// the owner's session; false for a malformed key.
bool hws_auth_restore(HwsAuth *a, int owner, const char *session, u64 now_ms, u64 ttl_ms);
void hws_auth_unpair(HwsAuth *a, int owner);   // revokes the session and any pending token
float hws_auth_token_remaining(const HwsAuth *a, int owner, u64 now_ms);   // seconds, 0 = none

// Rate limiting (per IPv4, host order). allow: one connection attempt (false = throttled or banned).
bool hws_rate_allow(HwsAuth *a, u32 ip, u64 now_ms);
void hws_rate_fail(HwsAuth *a, u32 ip, u64 now_ms);   // a failed token / session check
bool hws_rate_banned(const HwsAuth *a, u32 ip, u64 now_ms);

#endif
