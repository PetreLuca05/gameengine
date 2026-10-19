// Kill counter + kill feed model (docs/UI_WHEELS_HUD.md "Kill counter"). Every machine builds its
// own from the broadcast EV_KILL events (killer / victim / headshot / weapon), so nothing extra is
// replicated: a player joining mid-match starts at zero. Pure, inline (tests/test_hud.c).
#ifndef DG_KILL_STATS_H
#define DG_KILL_STATS_H

#include "core/base.h"
#include <string.h>

#define KILLFEED_MAX 5
#define KILLFEED_TIME 6.0f      // seconds an entry stays (fades over the last second)
#define KILL_STREAK_GAP 4.0f    // seconds between kills that keep a streak going

// EV_KILL id bits (the event's text keeps the old one-line message).
// KILLF_COLLATERAL: the bullet had already killed someone (through them: one shot, several kills).
enum { KILLF_HEADSHOT = 1u << 0, KILLF_VICTIM_PLAYER = 1u << 1, KILLF_VICTIM_ZOMBIE = 1u << 2, KILLF_KILLER_PLAYER = 1u << 3, KILLF_SELF = 1u << 4,
       KILLF_COLLATERAL = 1u << 5 };

typedef struct KillStats {
    int zombies, players, headshots;
    int streak, best_streak;
    float last_kill;          // time of the last kill (streak window)
    float pulse;              // 1 on a kill, decays (presentation)
    float head_tick;          // 1 on a headshot kill, decays
} KillStats;

typedef struct KillFeedEntry {
    char killer[32], victim[32];
    u16 weapon;               // item id (icon), 0 = none
    u32 flags;                // KILLF_*
    bool mine;                // the local player is the killer (highlighted)
    bool me_victim;
    float time;               // seconds left
} KillFeedEntry;

typedef struct KillFeed { KillFeedEntry e[KILLFEED_MAX]; } KillFeed;

DG_INLINE int kill_stats_total(const KillStats *k) { return k->zombies + k->players; }

// A kill by the local player at time now.
DG_INLINE void kill_stats_add(KillStats *k, u32 flags, float now) {
    if (flags & KILLF_VICTIM_PLAYER) k->players++;
    else k->zombies++;
    if (flags & KILLF_HEADSHOT) { k->headshots++; k->head_tick = 1.0f; }
    k->streak = (k->streak > 0 && now - k->last_kill <= KILL_STREAK_GAP) ? k->streak + 1 : 1;
    if (k->streak > k->best_streak) k->best_streak = k->streak;
    k->last_kill = now;
    k->pulse = 1.0f;
}

DG_INLINE void kill_stats_tick(KillStats *k, float now, float dt) {
    if (k->streak > 0 && now - k->last_kill > KILL_STREAK_GAP) k->streak = 0;
    k->pulse = k->pulse > 0 ? (k->pulse - dt / 0.6f > 0 ? k->pulse - dt / 0.6f : 0) : 0;
    k->head_tick = k->head_tick > 0 ? (k->head_tick - dt / 1.2f > 0 ? k->head_tick - dt / 1.2f : 0) : 0;
}

DG_INLINE void kill_stats_reset(KillStats *k) { memset(k, 0, sizeof(*k)); }

// Newest first; the oldest drops off when full.
DG_INLINE void killfeed_push(KillFeed *f, const char *killer, const char *victim, u16 weapon, u32 flags, bool mine, bool me_victim) {
    memmove(&f->e[1], &f->e[0], sizeof(KillFeedEntry) * (KILLFEED_MAX - 1));
    KillFeedEntry *e = &f->e[0];
    memset(e, 0, sizeof(*e));
    dg_strcpy(e->killer, sizeof(e->killer), killer ? killer : "");
    dg_strcpy(e->victim, sizeof(e->victim), victim ? victim : "");
    e->weapon = weapon;
    e->flags = flags;
    e->mine = mine;
    e->me_victim = me_victim;
    e->time = KILLFEED_TIME;
}

DG_INLINE void killfeed_tick(KillFeed *f, float dt) {
    for (int i = 0; i < KILLFEED_MAX; i++) if (f->e[i].time > 0) f->e[i].time -= dt;
}

DG_INLINE int killfeed_count(const KillFeed *f) {
    int n = 0;
    for (int i = 0; i < KILLFEED_MAX; i++) n += f->e[i].time > 0;
    return n;
}

// Entry alpha: fades over the last second.
DG_INLINE float killfeed_alpha(const KillFeedEntry *e) { return e->time <= 0 ? 0.0f : (e->time >= 1.0f ? 1.0f : e->time); }

#endif
