// stats_fake: plausible match histories for developing / screenshotting the Stats tab without
// playing (docs/STATS.md "Tooling"). Random but correlated: a skill that drifts upward over the
// year drives accuracy, kills, waves, wins and fewer downs; weapons have their own profiles
// (pellets, range, accuracy, kills per hit). Pure.
#include "stats.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct FakeWeapon { float shots, acc, kill_per_hit, range, pref; bool melee; } FakeWeapon;

static FakeWeapon profile_of(const char *id) {
    FakeWeapon f = { 220, 0.0f, 0.30f, 45, 1.0f, false };
    if (strstr(id, "shotgun")) { f.shots = 70 * 8; f.acc = -0.12f; f.kill_per_hit = 0.07f; f.range = 14; f.pref = 0.8f; }
    else if (strstr(id, "sniper")) { f.shots = 45; f.acc = 0.18f; f.kill_per_hit = 0.85f; f.range = 160; f.pref = 0.6f; }
    else if (strstr(id, "dmr")) { f.shots = 90; f.acc = 0.12f; f.kill_per_hit = 0.55f; f.range = 90; f.pref = 0.7f; }
    else if (strstr(id, "lmg")) { f.shots = 420; f.acc = -0.08f; f.kill_per_hit = 0.25f; f.range = 55; f.pref = 0.7f; }
    else if (strstr(id, "smg")) { f.shots = 330; f.acc = -0.04f; f.kill_per_hit = 0.24f; f.range = 25; f.pref = 1.1f; }
    else if (strstr(id, "rifle")) { f.shots = 260; f.acc = 0.02f; f.kill_per_hit = 0.30f; f.range = 60; f.pref = 1.6f; }
    else if (strstr(id, "pistol")) { f.shots = 80; f.acc = 0.0f; f.kill_per_hit = 0.30f; f.range = 18; f.pref = 0.9f; }
    else if (strstr(id, "crossbow")) { f.shots = 30; f.acc = 0.10f; f.kill_per_hit = 0.9f; f.range = 45; f.pref = 0.35f; }
    else if (strstr(id, "bat") || strstr(id, "knife") || strstr(id, "machete") || strstr(id, "axe")) { f.shots = 25; f.acc = 0.35f; f.kill_per_hit = 0.45f; f.range = 2.2f; f.pref = 0.4f; f.melee = true; }
    return f;
}

static float gauss(Rng *r) {
    float u = fmaxf(1e-6f, rng_float(r)), v = rng_float(r);
    return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

static int cmp_i64(const void *a, const void *b) {
    i64 x = *(const i64 *)a, y = *(const i64 *)b;
    return x < y ? -1 : (x > y);
}

static int pick_weapon(Rng *r, const FakeWeapon *wp, int n, int avoid) {
    float total = 0;
    for (int i = 0; i < n; i++) if (i != avoid) total += wp[i].pref;
    float x = rng_float(r) * total;
    for (int i = 0; i < n; i++) {
        if (i == avoid) continue;
        if ((x -= wp[i].pref) <= 0) return i;
    }
    return n - 1 == avoid ? 0 : n - 1;
}

void stats_fake(PlayerStats *ps, int n, u32 seed, i64 now, const char *const *maps, int map_count,
                const char *const *weapons, int weapon_count, i32 utc_offset_sec) {
    if (n <= 0 || weapon_count <= 0) return;
    StatsState *st = stats_get();
    i32 keep_utc = st->utc_offset_sec;
    st->utc_offset_sec = utc_offset_sec;
    Rng r = rng_make(seed ? seed : 1);
    int slot[STATS_WEAPON_COUNT];
    FakeWeapon wp[STATS_WEAPON_COUNT];
    int nw = DG_MIN(weapon_count, STATS_WEAPON_COUNT);
    for (int i = 0; i < nw; i++) {
        slot[i] = stats_weapon_slot(ps, weapons[i]);
        wp[i] = profile_of(weapons[i]);
    }
    // Start times over the last year: evening sessions, more of them recently.
    i64 *t = (i64 *)malloc(sizeof(i64) * (size_t)n);
    if (!t) return;
    for (int i = 0; i < n; i++) {
        float age = powf(rng_float(&r), 1.35f);   // skewed towards now
        i64 day = (i64)(age * 364.0f);
        i64 secs = (i64)(17 + rng_float(&r) * 6.5f) * 3600 + (i64)(rng_float(&r) * 3600.0f);
        t[i] = (now - (now + utc_offset_sec) % 86400) - day * 86400 + secs - utc_offset_sec;
        if (t[i] > now - 600) t[i] = now - 600 - (i64)(rng_float(&r) * 7200.0f);
    }
    qsort(t, (size_t)n, sizeof(i64), cmp_i64);
    for (int i = 0; i < n; i++) {
        float progress = n > 1 ? (float)i / (float)(n - 1) : 1.0f;
        // One draw per statement: DG_CLAMP / DG_MIN evaluate their arguments more than once.
        float skill = 0.32f + 0.36f * progress + gauss(&r) * 0.13f;
        skill = DG_CLAMP(skill, 0.05f, 0.97f);
        MatchRecord m;
        memset(&m, 0, sizeof(m));
        m.timestamp = t[i];
        dg_strcpy(m.map, sizeof(m.map), map_count > 0 ? maps[rng_next(&r) % (u32)map_count] : "devroom");
        // Waves and the result.
        int waves = (int)lrintf(4.0f + skill * 19.0f + gauss(&r) * 3.0f);
        waves = DG_CLAMP(waves, 1, STATS_MAX_WAVES);
        bool abandon = rng_float(&r) < 0.06f;
        m.result = (u8)(abandon ? MATCH_ABANDON : (waves >= 15 || rng_float(&r) < skill * 0.25f) ? MATCH_WIN : MATCH_LOSS);
        if (abandon) waves = DG_MAX(1, waves / 2);
        m.waves_reached = (u16)waves;
        m.segment_sec = 0;
        m.duration_sec = (float)waves * (70.0f + rng_float(&r) * 50.0f) + 60.0f;
        // Weapons: a primary for most of the match, a secondary for the rest.
        int prim = pick_weapon(&r, wp, nw, -1);
        int sec = nw > 1 ? pick_weapon(&r, wp, nw, prim) : prim;
        int used[2] = { prim, sec };
        float share[2] = { 0.68f + rng_float(&r) * 0.2f, 0 };
        share[1] = 1.0f - share[0];
        float len_scale = m.duration_sec / 1500.0f;
        for (int k = 0; k < (prim == sec ? 1 : 2); k++) {
            int w = used[k], s = slot[w];
            const FakeWeapon *f = &wp[w];
            float sh = prim == sec ? 1.0f : share[k];
            u32 shots = (u32)fmaxf(0.0f, f->shots * sh * len_scale * (0.7f + rng_float(&r) * 0.6f));
            float acc = 0.10f + 0.42f * skill + f->acc + gauss(&r) * 0.04f;
            acc = DG_CLAMP(acc, 0.03f, 0.92f);
            u32 hits = (u32)lrintf((float)shots * acc);
            u32 kills = (u32)lrintf((float)hits * f->kill_per_hit * (0.8f + 0.5f * skill));
            float head_frac = 0.08f + 0.38f * skill + gauss(&r) * 0.05f;
            u32 heads = (u32)lrintf((float)kills * DG_CLAMP(head_frac, 0.0f, 0.8f));
            float dmg = (float)hits * (f->melee ? 55.0f : 28.0f) * (0.85f + rng_float(&r) * 0.3f);
            if (s < 0) continue;
            WeaponStats *ws = &ps->weapons[s];
            ws->shots_fired += shots;
            ws->shots_hit += hits;
            ws->kills += kills;
            ws->headshots += heads;
            ws->headshot_hits += heads + hits / 9;
            ws->damage_dealt += dmg;
            ws->time_equipped_sec += m.duration_sec * sh;
            m.shots_fired += shots;
            m.shots_hit += hits;
            m.kills += kills;
            m.headshots += heads;
            m.damage_dealt += dmg;
            m.shots_per_weapon[s] = (u16)DG_MIN(65535u, m.shots_per_weapon[s] + shots);
            m.hits_per_weapon[s] = (u16)DG_MIN(65535u, m.hits_per_weapon[s] + hits);
            m.kills_per_weapon[s] = (u16)DG_MIN(65535u, m.kills_per_weapon[s] + kills);
            for (u32 kk = 0; kk < kills; kk++) {
                // Kill distance: mostly close, a tail out to the weapon's range.
                float d = f->melee ? 0.8f + rng_float(&r) * 1.6f : fmaxf(1.0f, -logf(fmaxf(1e-4f, rng_float(&r))) * f->range * 0.45f);
                d = fminf(d, f->range * 2.6f);
                ws->kill_dist[stats_kill_dist_bucket(d)]++;
                if (d > ws->longest_kill_m) ws->longest_kill_m = d;
                if (d > m.longest_kill_m) m.longest_kill_m = d;
                if (d > ps->longest_kill.dist_m) {
                    ps->longest_kill.dist_m = d;
                    ps->longest_kill.weapon = (i16)s;
                    dg_strcpy(ps->longest_kill.map, sizeof(ps->longest_kill.map), m.map);
                    ps->longest_kill.timestamp = m.timestamp;
                }
            }
        }
        // Kills over the waves (later waves have more zombies), downs and revives.
        float wsum = 0;
        for (int w = 0; w < waves; w++) wsum += 1.0f + (float)w * 0.25f;
        u32 left = m.kills;
        for (int w = 0; w < waves && w < STATS_MAX_WAVES; w++) {
            u32 k = w == waves - 1 ? left : (u32)lrintf((float)m.kills * (1.0f + (float)w * 0.25f) / wsum * (0.75f + rng_float(&r) * 0.5f));
            k = DG_MIN(k, left);
            m.kills_per_wave[w] = (u16)DG_MIN(65535u, k);
            left -= k;
        }
        int downs = (int)lrintf(fmaxf(0.0f, (1.0f - skill) * 3.2f + gauss(&r) * 0.9f));
        for (int d = 0; d < downs; d++) {
            int w = (int)(powf(rng_float(&r), 0.6f) * (float)waves);
            w = DG_CLAMP(w, 0, STATS_MAX_WAVES - 1);
            if (m.downs_per_wave[w] < 255) m.downs_per_wave[w]++;
        }
        m.downs = (u16)downs;
        // Deaths: a down that bled out (about a third), and the wipe that loses the match.
        u32 deaths = m.result == MATCH_LOSS ? 1 : 0;
        for (int d = 0; d < downs; d++) deaths += rng_float(&r) < 0.35f;
        m.deaths = deaths;
        int revives = (int)lrintf(fmaxf(0.0f, skill * 2.6f + gauss(&r) * 0.9f));
        for (int v = 0; v < revives; v++) {
            int w = (int)(rng_float(&r) * (float)waves);
            w = DG_CLAMP(w, 0, STATS_MAX_WAVES - 1);
            if (m.revives_per_wave[w] < 255) m.revives_per_wave[w]++;
        }
        m.revives = (u16)revives;
        m.damage_taken = (float)downs * 100.0f + rng_float(&r) * 160.0f * (1.2f - skill);
        u32 streak = (u32)lrintf((float)m.kills / (float)(downs + m.deaths + 1) * (0.9f + rng_float(&r) * 0.6f));
        m.best_streak = (u16)DG_MIN(65535u, streak);
        m.distance_m = m.duration_sec * (1.0f + rng_float(&r) * 1.6f);
        m.steps = (u32)(m.distance_m / STATS_STRIDE_M);
        float jumps = m.distance_m / (25.0f + rng_float(&r) * 30.0f);
        m.jumps = (u16)DG_MIN(65535.0f, jumps);
        stats_push_match(ps, &m);
        stats_add_match_totals(ps, &m);
    }
    free(t);
    st->utc_offset_sec = keep_utc;
    ps->revision++;
}
