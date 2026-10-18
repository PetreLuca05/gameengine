// Phone pairing tokens, sessions and rate limiting (see hws_auth.h).
#include "hws_auth.h"

#define RATE_BURST 20.0f
#define RATE_PER_S 10.0f
#define FAIL_LIMIT 5
#define FAIL_WINDOW_MS 60000u
#define BAN_MS 60000u

static bool key_eq(const char *a, const char *b) {
    // Constant time over the fixed key length; any length mismatch fails.
    size_t la = strlen(a), lb = strlen(b);
    u8 diff = (u8)(la != HWS_KEY_CHARS) | (u8)(lb != HWS_KEY_CHARS);
    for (size_t i = 0; i < HWS_KEY_CHARS; i++) diff |= (u8)((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
    return diff == 0;
}

void hws_auth_init(HwsAuth *a, u64 token_ttl_ms) {
    memset(a, 0, sizeof(*a));
    a->token_ttl_ms = token_ttl_ms ? token_ttl_ms : 120000u;
}

void hws_key_from_bytes(const u8 rnd[16], char out[HWS_KEY_CHARS + 1]) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int o = 0;
    u32 acc = 0;
    int bits = 0;
    for (int i = 0; i < 16; i++) {
        acc = acc << 8 | rnd[i];
        bits += 8;
        while (bits >= 6) {
            out[o++] = T[(acc >> (bits - 6)) & 63];
            bits -= 6;
        }
    }
    if (bits > 0) out[o++] = T[(acc << (6 - bits)) & 63];
    out[o] = 0;   // 16 bytes = 128 bits -> 21 full + 1 partial = 22 chars
}

const char *hws_auth_issue(HwsAuth *a, int owner, const u8 rnd[16], u64 now_ms) {
    if (owner < 0 || owner >= HWS_MAX_OWNERS) return NULL;
    HwsPairing *p = &a->owner[owner];
    hws_key_from_bytes(rnd, p->token);
    p->pending = true;
    p->expires_ms = now_ms + a->token_ttl_ms;
    return p->token;
}

HwsAuthResult hws_auth_pair(HwsAuth *a, const char *token, u64 now_ms, const u8 rnd[16], int *owner, char session_out[HWS_KEY_CHARS + 1]) {
    for (int i = 0; i < HWS_MAX_OWNERS; i++) {
        HwsPairing *p = &a->owner[i];
        if (!p->pending || !key_eq(p->token, token)) continue;
        if (now_ms >= p->expires_ms) {
            p->pending = false;   // expired tokens are dropped on first use
            memset(p->token, 0, sizeof(p->token));
            return HWS_AUTH_EXPIRED;
        }
        p->pending = false;
        memset(p->token, 0, sizeof(p->token));
        hws_key_from_bytes(rnd, p->session);
        p->paired = true;
        p->generation++;
        p->session_expires_ms = a->session_ttl_ms ? now_ms + a->session_ttl_ms : 0;
        if (owner) *owner = i;
        if (session_out) memcpy(session_out, p->session, HWS_KEY_CHARS + 1);
        return HWS_AUTH_OK;
    }
    return HWS_AUTH_BAD;
}

int hws_auth_session(HwsAuth *a, const char *session, u64 now_ms, bool touch) {
    if (!session) return -1;
    for (int i = 0; i < HWS_MAX_OWNERS; i++) {
        HwsPairing *p = &a->owner[i];
        if (!p->paired || !key_eq(p->session, session)) continue;
        if (p->session_expires_ms && now_ms >= p->session_expires_ms) {
            // Expired: the session is gone (the page has to scan a new code).
            p->paired = false;
            memset(p->session, 0, sizeof(p->session));
            p->generation++;
            return -1;
        }
        if (touch && p->session_expires_ms && a->session_ttl_ms) p->session_expires_ms = now_ms + a->session_ttl_ms;
        return i;
    }
    return -1;
}

bool hws_auth_restore(HwsAuth *a, int owner, const char *session, u64 now_ms, u64 ttl_ms) {
    if (owner < 0 || owner >= HWS_MAX_OWNERS || !session || strlen(session) != HWS_KEY_CHARS) return false;
    for (int i = 0; i < HWS_KEY_CHARS; i++) {
        char c = session[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    HwsPairing *p = &a->owner[owner];
    memcpy(p->session, session, HWS_KEY_CHARS + 1);
    p->paired = true;
    p->generation++;
    p->session_expires_ms = ttl_ms ? now_ms + ttl_ms : 0;
    return true;
}

void hws_auth_unpair(HwsAuth *a, int owner) {
    if (owner < 0 || owner >= HWS_MAX_OWNERS) return;
    HwsPairing *p = &a->owner[owner];
    u32 gen = p->generation + 1;
    memset(p, 0, sizeof(*p));
    p->generation = gen;
}

float hws_auth_token_remaining(const HwsAuth *a, int owner, u64 now_ms) {
    if (owner < 0 || owner >= HWS_MAX_OWNERS || !a->owner[owner].pending || now_ms >= a->owner[owner].expires_ms) return 0.0f;
    return (float)(a->owner[owner].expires_ms - now_ms) / 1000.0f;
}

static HwsRateSlot *rate_slot(HwsAuth *a, u32 ip, u64 now_ms, bool create) {
    HwsRateSlot *oldest = &a->rate[0];
    for (int i = 0; i < HWS_RATE_SLOTS; i++) {
        HwsRateSlot *s = &a->rate[i];
        if (s->last_ms && s->ip == ip) return s;
        if (!s->last_ms || s->last_ms < oldest->last_ms) oldest = s;
    }
    if (!create) return NULL;
    // Never evict a banned address for a new one (an attacker could rotate it out otherwise):
    // fall back to the least recently seen unbanned slot.
    if (oldest->last_ms && oldest->ban_until_ms > now_ms) {
        for (int i = 0; i < HWS_RATE_SLOTS; i++)
            if (a->rate[i].ban_until_ms <= now_ms && (oldest->ban_until_ms > now_ms || a->rate[i].last_ms < oldest->last_ms)) oldest = &a->rate[i];
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->ip = ip;
    oldest->bucket = RATE_BURST;
    oldest->last_ms = now_ms ? now_ms : 1;
    return oldest;
}

bool hws_rate_allow(HwsAuth *a, u32 ip, u64 now_ms) {
    HwsRateSlot *s = rate_slot(a, ip, now_ms, true);
    if (s->ban_until_ms > now_ms) return false;
    float dt = (float)(now_ms - (s->last_ms < now_ms ? s->last_ms : now_ms)) / 1000.0f;
    s->last_ms = now_ms ? now_ms : 1;
    s->bucket = s->bucket + dt * RATE_PER_S;
    if (s->bucket > RATE_BURST) s->bucket = RATE_BURST;
    if (s->bucket < 1.0f) return false;
    s->bucket -= 1.0f;
    return true;
}

void hws_rate_fail(HwsAuth *a, u32 ip, u64 now_ms) {
    HwsRateSlot *s = rate_slot(a, ip, now_ms, true);
    if (now_ms - s->fail_window_ms > FAIL_WINDOW_MS || s->fail_window_ms == 0) {
        s->fail_window_ms = now_ms ? now_ms : 1;
        s->fails = 0;
    }
    if (++s->fails >= FAIL_LIMIT) {
        s->ban_until_ms = now_ms + BAN_MS;
        s->fails = 0;
        s->fail_window_ms = 0;
    }
}

bool hws_rate_banned(const HwsAuth *a, u32 ip, u64 now_ms) {
    for (int i = 0; i < HWS_RATE_SLOTS; i++)
        if (a->rate[i].last_ms && a->rate[i].ip == ip) return a->rate[i].ban_until_ms > now_ms;
    return false;
}
