// Player statistics: events, derived values, history, host deltas (docs/STATS.md). Pure.
#include "stats.h"
#include <math.h>
#include <string.h>
#include <time.h>

static StatsState g_st;
#define S (&g_st)
#define L (&g_st.life)
#define C (&g_st.cur)

StatsState *stats_get(void) { return &g_st; }

void stats_init_profile(PlayerStats *ps, const char *profile) {
    memset(ps, 0, sizeof(*ps));
    ps->version = STATS_VERSION;
    ps->longest_kill.weapon = -1;
    dg_strcpy(ps->profile, sizeof(ps->profile), profile ? profile : "local");
}

void stats_reset(void) {
    char profile[40];
    dg_strcpy(profile, sizeof(profile), L->profile[0] ? L->profile : "local");
    i64 now = S->now_override;
    i32 utc = S->utc_offset_sec;
    memset(&g_st, 0, sizeof(g_st));
    stats_init_profile(L, profile);
    S->now_override = now;
    S->utc_offset_sec = utc;
}

i64 stats_now(void) { return S->now_override ? S->now_override : (i64)time(NULL); }

i32 stats_day_of(i64 t) {
    i64 s = t + S->utc_offset_sec;
    return (i32)(s >= 0 ? s / 86400 : -((-s + 86399) / 86400));
}

// Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm).
void stats_day_ymd(i32 day, int *y, int *m, int *d) {
    i64 z = (i64)day + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153;
    int dd = (int)(doy - (153 * mp + 2) / 5 + 1);
    int mm = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (mm <= 2));
    *m = mm;
    *d = dd;
}

int stats_weekday(i32 day) { return (int)(((i64)day % 7 + 7 + 3) % 7); }   // 1970-01-01 was a Thursday

// ------------------------------------------------------------------ weapons

int stats_weapon_find(const PlayerStats *ps, const char *id) {
    if (!id || !id[0]) return -1;
    for (int i = 0; i < ps->weapon_count; i++)
        if (!strcmp(ps->weapon_ids[i], id)) return i;
    return -1;
}

int stats_weapon_slot(PlayerStats *ps, const char *id) {
    int i = stats_weapon_find(ps, id);
    if (i >= 0 || !id || !id[0] || ps->weapon_count >= STATS_WEAPON_COUNT) return i;
    i = ps->weapon_count++;
    dg_strcpy(ps->weapon_ids[i], STATS_WEAPON_ID, id);
    ps->revision++;
    return i;
}

static const float k_dist_edges[STATS_KILL_DIST_BUCKETS + 1] = { 0, 5, 10, 15, 20, 30, 40, 50, 75, 100, 150, 200, 1e9f };
const float *stats_kill_dist_edges(void) { return k_dist_edges; }

int stats_kill_dist_bucket(float dist_m) {
    for (int i = STATS_KILL_DIST_BUCKETS - 1; i > 0; i--)
        if (dist_m >= k_dist_edges[i]) return i;
    return 0;
}

static bool wslot_ok(int w) { return w >= 0 && w < STATS_WEAPON_COUNT; }

// ------------------------------------------------------------------ events

int stats_current_wave(void) {
    int b = C->waves ? C->wave : (int)(C->rec.duration_sec / (float)STATS_SEGMENT_SEC);
    return DG_CLAMP(b, 0, STATS_MAX_WAVES - 1);
}

void stats_on_match_start(const char *map) {
    if (C->active) stats_on_match_end(MATCH_ABANDON);
    memset(C, 0, sizeof(*C));
    C->active = true;
    C->rec.timestamp = stats_now();
    C->rec.segment_sec = STATS_SEGMENT_SEC;
    dg_strcpy(C->rec.map, sizeof(C->rec.map), map ? map : "");
}

void stats_on_match_end(MatchResult result) {
    if (!C->active) return;
    MatchRecord *r = &C->rec;
    C->active = false;
    if (r->duration_sec < STATS_MIN_MATCH_SEC && !r->kills && !r->deaths && !r->shots_fired && !r->downs) return;
    r->result = (u8)result;
    if (!C->step_events) {
        r->steps = (u32)(r->distance_m / STATS_STRIDE_M);
        L->steps += r->steps;
    }
    r->waves_reached = C->waves ? (u16)(C->wave + 1) : 0;
    stats_push_match(L, r);
    L->matches_played++;
    if (result == MATCH_WIN) L->matches_won++;
    if (r->waves_reached > L->highest_wave) L->highest_wave = r->waves_reached;
    L->revision++;
}

void stats_on_shots_fired(int weapon, u32 count) {
    if (!C->active || !count) return;
    L->revision++;
    C->rec.shots_fired += count;
    if (!wslot_ok(weapon)) return;
    L->weapons[weapon].shots_fired += count;
    C->rec.shots_per_weapon[weapon] = (u16)DG_MIN(65535u, (u32)C->rec.shots_per_weapon[weapon] + count);
}

void stats_on_shot_fired(int weapon) { stats_on_shots_fired(weapon, 1); }

static void add_hits(int weapon, u32 hits, u32 head_hits, double damage) {
    if (!C->active) return;
    L->revision++;
    L->damage_dealt += damage;
    C->rec.damage_dealt += (float)damage;
    C->rec.shots_hit += hits;
    if (!wslot_ok(weapon)) return;
    WeaponStats *w = &L->weapons[weapon];
    w->shots_hit += hits;
    w->headshot_hits += head_hits;
    w->damage_dealt += damage;
    C->rec.hits_per_weapon[weapon] = (u16)DG_MIN(65535u, (u32)C->rec.hits_per_weapon[weapon] + hits);
}

void stats_on_hit(int weapon, float damage, bool headshot) { add_hits(weapon, 1, headshot ? 1 : 0, damage); }

void stats_on_kill(int weapon, float dist_m, bool headshot) {
    if (!C->active) return;
    MatchRecord *r = &C->rec;
    L->revision++;
    L->kills++;
    r->kills++;
    if (headshot) { L->headshots++; r->headshots++; }
    int b = stats_current_wave();
    if (r->kills_per_wave[b] < 65535) r->kills_per_wave[b]++;
    if (dist_m > r->longest_kill_m) r->longest_kill_m = dist_m;
    if (wslot_ok(weapon)) {
        WeaponStats *w = &L->weapons[weapon];
        w->kills++;
        if (headshot) w->headshots++;
        if (dist_m > w->longest_kill_m) w->longest_kill_m = dist_m;
        w->kill_dist[stats_kill_dist_bucket(dist_m)]++;
        if (r->kills_per_weapon[weapon] < 65535) r->kills_per_weapon[weapon]++;
    }
    if (dist_m > L->longest_kill.dist_m) {
        L->longest_kill.dist_m = dist_m;
        L->longest_kill.weapon = (i16)(wslot_ok(weapon) ? weapon : -1);
        dg_strcpy(L->longest_kill.map, sizeof(L->longest_kill.map), r->map);
        L->longest_kill.timestamp = stats_now();
    }
    // Killstreak: kills since the last death / down (best updated live).
    C->streak++;
    if ((u32)C->streak > r->best_streak) r->best_streak = (u16)DG_MIN(C->streak, 65535);
    if ((u32)C->streak > L->best_killstreak) L->best_killstreak = (u32)C->streak;
}

void stats_on_death(void) {
    if (!C->active) return;
    L->revision++;
    L->deaths++;
    C->rec.deaths++;
    C->streak = 0;
}

void stats_on_down(void) {
    if (!C->active) return;
    L->revision++;
    L->times_downed++;
    if (C->rec.downs < 65535) C->rec.downs++;
    int b = stats_current_wave();
    if (C->rec.downs_per_wave[b] < 255) C->rec.downs_per_wave[b]++;
    C->streak = 0;
}

void stats_on_revive_given(void) {
    if (!C->active) return;
    L->revision++;
    L->revives_given++;
    if (C->rec.revives < 65535) C->rec.revives++;
    int b = stats_current_wave();
    if (C->rec.revives_per_wave[b] < 255) C->rec.revives_per_wave[b]++;
}

void stats_on_damage_taken(float amount) {
    if (!C->active || amount <= 0) return;
    L->revision++;
    L->damage_taken += amount;
    C->rec.damage_taken += amount;
}

void stats_on_jump(void) {
    if (!C->active) return;
    L->jumps++;
    if (C->rec.jumps < 65535) C->rec.jumps++;
}

void stats_on_step(void) {
    if (!C->active) return;
    L->steps++;
    C->rec.steps++;
    C->step_events++;
}

void stats_on_wave_complete(int wave) {
    if (!C->active) return;
    // The first report switches the timeline from time segments to waves.
    if (!C->waves) {
        C->waves = true;
        C->rec.segment_sec = 0;
        memset(C->rec.kills_per_wave, 0, sizeof(C->rec.kills_per_wave));
        memset(C->rec.downs_per_wave, 0, sizeof(C->rec.downs_per_wave));
        memset(C->rec.revives_per_wave, 0, sizeof(C->rec.revives_per_wave));
    }
    C->wave = DG_CLAMP(wave, 0, STATS_MAX_WAVES - 1);   // wave N (1-based) survived: N + 1 is playing (index N)
    if ((u32)wave > L->highest_wave) L->highest_wave = (u32)wave;
    L->revision++;
}

void stats_tick(float dt, const StatsTickIn *in) {
    if (!C->active || !in || !in->in_match || dt <= 0) return;
    C->rec.duration_sec += dt;
    L->time_played_sec += dt;
    if (in->alive && wslot_ok(in->weapon)) L->weapons[in->weapon].time_equipped_sec += dt;
    // Grounded horizontal distance; jumps over 25 m/s (teleports, respawns) are not walking.
    if (in->alive && in->grounded) {
        if (C->have_pos) {
            float dx = in->x - C->last_x, dz = in->z - C->last_z;
            float d = sqrtf(dx * dx + dz * dz);
            if (d <= 25.0f * dt + 0.02f) {
                L->total_distance_m += d;
                C->rec.distance_m += d;
            }
        }
        C->have_pos = true;
        C->last_x = in->x;
        C->last_z = in->z;
    } else C->have_pos = false;
}

// ------------------------------------------------------------------ derived

float stats_ratio(double num, double den) { return den > 0 ? (float)(num / den) : 0.0f; }
float stats_weapon_accuracy(const WeaponStats *w) { return fminf(1.0f, stats_ratio(w->shots_hit, w->shots_fired)); }
float stats_kd(const PlayerStats *ps) { return (float)ps->kills / (float)DG_MAX(ps->deaths, 1u); }
float stats_headshot_pct(const PlayerStats *ps) { return stats_ratio(ps->headshots, ps->kills); }
float stats_win_rate(const PlayerStats *ps) { return stats_ratio(ps->matches_won, ps->matches_played); }

u32 stats_total_shots(const PlayerStats *ps) {
    u32 n = 0;
    for (int i = 0; i < STATS_WEAPON_COUNT; i++) n += ps->weapons[i].shots_fired;
    return n;
}

u32 stats_total_hits(const PlayerStats *ps) {
    u32 n = 0;
    for (int i = 0; i < STATS_WEAPON_COUNT; i++) n += ps->weapons[i].shots_hit;
    return n;
}

float stats_overall_accuracy(const PlayerStats *ps) { return fminf(1.0f, stats_ratio(stats_total_hits(ps), stats_total_shots(ps))); }

int stats_favorite_weapon(const PlayerStats *ps) {
    int best = -1;
    for (int i = 0; i < STATS_WEAPON_COUNT; i++) {
        const WeaponStats *w = &ps->weapons[i];
        if (!w->kills && w->time_equipped_sec <= 0) continue;
        if (best < 0 || w->kills > ps->weapons[best].kills ||
            (w->kills == ps->weapons[best].kills && w->time_equipped_sec > ps->weapons[best].time_equipped_sec)) best = i;
    }
    return best;
}

float stats_match_accuracy(const MatchRecord *m) { return fminf(1.0f, stats_ratio(m->shots_hit, m->shots_fired)); }
float stats_match_kd(const MatchRecord *m) { return (float)m->kills / (float)DG_MAX(m->deaths, 1u); }

// ------------------------------------------------------------------ history

const MatchRecord *stats_match_at(const PlayerStats *ps, int i) {
    if (i < 0 || i >= ps->match_count) return NULL;
    int start = (ps->match_head - ps->match_count + STATS_MAX_MATCHES) % STATS_MAX_MATCHES;
    return &ps->matches[(start + i) % STATS_MAX_MATCHES];
}

const DailyRecord *stats_day_at(const PlayerStats *ps, int i) {
    if (i < 0 || i >= ps->day_count) return NULL;
    int start = (ps->day_head - ps->day_count + STATS_MAX_DAYS) % STATS_MAX_DAYS;
    return &ps->days[(start + i) % STATS_MAX_DAYS];
}

const DailyRecord *stats_day_find(const PlayerStats *ps, i32 day) {
    for (int i = ps->day_count - 1; i >= 0; i--) {
        const DailyRecord *d = stats_day_at(ps, i);
        if (d->day == day) return d;
        if (d->day < day) break;
    }
    return NULL;
}

void stats_push_match(PlayerStats *ps, const MatchRecord *m) {
    ps->matches[ps->match_head] = *m;
    ps->match_head = (ps->match_head + 1) % STATS_MAX_MATCHES;
    if (ps->match_count < STATS_MAX_MATCHES) ps->match_count++;
    // The day roll-up (the newest day is usually the match's).
    i32 day = stats_day_of(m->timestamp);
    DailyRecord *d = (DailyRecord *)stats_day_find(ps, day);
    if (!d) {
        d = &ps->days[ps->day_head];
        memset(d, 0, sizeof(*d));
        d->day = day;
        ps->day_head = (ps->day_head + 1) % STATS_MAX_DAYS;
        if (ps->day_count < STATS_MAX_DAYS) ps->day_count++;
    }
    d->matches++;
    d->kills += m->kills;
    d->deaths += m->deaths;
    d->time_sec += m->duration_sec;
    d->shots_fired += m->shots_fired;
    d->shots_hit += m->shots_hit;
    ps->revision++;
}

void stats_add_match_totals(PlayerStats *ps, const MatchRecord *m) {
    ps->kills += m->kills;
    ps->deaths += m->deaths;
    ps->headshots += m->headshots;
    ps->damage_dealt += m->damage_dealt;
    ps->damage_taken += m->damage_taken;
    ps->revives_given += m->revives;
    ps->times_downed += m->downs;
    ps->time_played_sec += m->duration_sec;
    ps->total_distance_m += m->distance_m;
    ps->steps += m->steps;
    ps->jumps += m->jumps;
    ps->matches_played++;
    if (m->result == MATCH_WIN) ps->matches_won++;
    if (m->waves_reached > ps->highest_wave) ps->highest_wave = m->waves_reached;
    if (m->best_streak > ps->best_killstreak) ps->best_killstreak = m->best_streak;
    ps->revision++;
}

// ------------------------------------------------------------------ host deltas

bool stats_delta_empty(const StatsDelta *d) { return !d->wmask && !d->n && d->damage_other <= 0 && d->damage_taken <= 0; }

bool stats_delta_full(const StatsDelta *d) {
    if (d->n >= STATS_DELTA_EVENTS) return true;
    for (int i = 0; i < STATS_WEAPON_COUNT; i++)
        if ((d->wmask & (1u << i)) && (d->w[i].shots > 60000 || d->w[i].hits > 60000)) return true;
    return false;
}

static StatsDeltaWeapon *dw(StatsDelta *d, int w) {
    if (!wslot_ok(w)) return NULL;
    d->wmask |= 1u << w;
    return &d->w[w];
}

void stats_delta_shot(StatsDelta *d, int wire_weapon, u32 count) {
    StatsDeltaWeapon *x = dw(d, wire_weapon);
    if (x) x->shots = (u16)DG_MIN(65535u, x->shots + count);
}

void stats_delta_hit(StatsDelta *d, int wire_weapon, float damage, bool headshot) {
    StatsDeltaWeapon *x = dw(d, wire_weapon);
    if (!x) { d->damage_other += damage; return; }
    if (x->hits < 65535) x->hits++;
    if (headshot && x->head_hits < 65535) x->head_hits++;
    x->damage += damage;
}

void stats_delta_kill(StatsDelta *d, int wire_weapon, float dist_m, bool headshot) {
    if (d->n >= STATS_DELTA_EVENTS) return;
    StatsDeltaEvent *e = &d->ev[d->n++];
    e->kind = (u8)(headshot ? SDK_HEADSHOT_KILL : SDK_KILL);
    e->weapon = (u8)(wslot_ok(wire_weapon) ? wire_weapon : 255);
    e->value = (u16)DG_CLAMP((int)lrintf(dist_m * 10.0f), 0, 65535);
}

void stats_delta_event(StatsDelta *d, StatsDeltaKind kind) {
    if (d->n >= STATS_DELTA_EVENTS) return;
    StatsDeltaEvent *e = &d->ev[d->n++];
    e->kind = (u8)kind;
    e->weapon = 255;
    e->value = 0;
}

void stats_delta_taken(StatsDelta *d, float amount) { if (amount > 0) d->damage_taken += amount; }

void stats_apply_delta(const StatsDelta *d, const i8 *wire_to_slot) {
    for (int i = 0; i < STATS_WEAPON_COUNT; i++) {
        if (!(d->wmask & (1u << i))) continue;
        int slot = wire_to_slot ? wire_to_slot[i] : i;
        stats_on_shots_fired(slot, d->w[i].shots);
        add_hits(slot, d->w[i].hits, d->w[i].head_hits, d->w[i].damage);
    }
    if (d->damage_other > 0) add_hits(-1, 0, 0, d->damage_other);
    for (int k = 0; k < d->n && k < STATS_DELTA_EVENTS; k++) {
        const StatsDeltaEvent *e = &d->ev[k];
        int slot = e->weapon < STATS_WEAPON_COUNT ? (wire_to_slot ? wire_to_slot[e->weapon] : e->weapon) : -1;
        switch (e->kind) {
        case SDK_KILL: stats_on_kill(slot, (float)e->value * 0.1f, false); break;
        case SDK_HEADSHOT_KILL: stats_on_kill(slot, (float)e->value * 0.1f, true); break;
        case SDK_DEATH: stats_on_death(); break;
        case SDK_DOWN: stats_on_down(); break;
        case SDK_REVIVE: stats_on_revive_given(); break;
        default: break;
        }
    }
    stats_on_damage_taken(d->damage_taken);
}

#define PUT(v) do { if (n + (int)sizeof(v) > cap) return -1; memcpy(out + n, &(v), sizeof(v)); n += (int)sizeof(v); } while (0)
#define GET(v) do { if (n + (int)sizeof(v) > size) return false; memcpy(&(v), in + n, sizeof(v)); n += (int)sizeof(v); } while (0)

int stats_delta_write(const StatsDelta *d, u8 *out, int cap) {
    int n = 0;
    PUT(d->wmask);
    for (int i = 0; i < STATS_WEAPON_COUNT; i++) {
        if (!(d->wmask & (1u << i))) continue;
        PUT(d->w[i].shots);
        PUT(d->w[i].hits);
        PUT(d->w[i].head_hits);
        PUT(d->w[i].damage);
    }
    PUT(d->damage_other);
    PUT(d->damage_taken);
    u8 cnt = (u8)DG_CLAMP(d->n, 0, STATS_DELTA_EVENTS);
    PUT(cnt);
    for (int k = 0; k < cnt; k++) {
        PUT(d->ev[k].kind);
        PUT(d->ev[k].weapon);
        PUT(d->ev[k].value);
    }
    return n;
}

bool stats_delta_read(StatsDelta *d, const u8 *in, int size) {
    memset(d, 0, sizeof(*d));
    int n = 0;
    GET(d->wmask);
    for (int i = 0; i < STATS_WEAPON_COUNT; i++) {
        if (!(d->wmask & (1u << i))) continue;
        GET(d->w[i].shots);
        GET(d->w[i].hits);
        GET(d->w[i].head_hits);
        GET(d->w[i].damage);
    }
    GET(d->damage_other);
    GET(d->damage_taken);
    u8 cnt = 0;
    GET(cnt);
    if (cnt > STATS_DELTA_EVENTS) return false;
    for (int k = 0; k < cnt; k++) {
        GET(d->ev[k].kind);
        GET(d->ev[k].weapon);
        GET(d->ev[k].value);
    }
    d->n = cnt;
    return isfinite(d->damage_other) && isfinite(d->damage_taken);
}

void stats_public_from(StatsPublic *p, const PlayerStats *ps, const char *name) {
    memset(p, 0, sizeof(*p));
    dg_strcpy(p->name, sizeof(p->name), name ? name : "");
    int fav = stats_favorite_weapon(ps);
    if (fav >= 0) dg_strcpy(p->favorite, sizeof(p->favorite), ps->weapon_ids[fav]);
    p->kills = ps->kills;
    p->deaths = ps->deaths;
    p->headshots = ps->headshots;
    p->shots_fired = stats_total_shots(ps);
    p->shots_hit = stats_total_hits(ps);
    p->revives_given = ps->revives_given;
    p->times_downed = ps->times_downed;
    p->matches_played = ps->matches_played;
    p->matches_won = ps->matches_won;
    p->highest_wave = ps->highest_wave;
    p->best_killstreak = ps->best_killstreak;
    p->time_played_sec = (float)ps->time_played_sec;
    p->distance_m = (float)ps->total_distance_m;
    p->longest_kill_m = ps->longest_kill.dist_m;
    p->damage_dealt = (float)ps->damage_dealt;
}

// Fixed layout (u32 / float / char only: identical on every platform).
int stats_public_write(const StatsPublic *p, u8 *out, int cap) {
    int n = 0;
    PUT(*p);
    return n;
}

bool stats_public_read(StatsPublic *p, const u8 *in, int size) {
    int n = 0;
    GET(*p);
    p->name[sizeof(p->name) - 1] = 0;
    p->favorite[sizeof(p->favorite) - 1] = 0;
    return isfinite(p->time_played_sec) && isfinite(p->distance_m) && isfinite(p->longest_kill_m) && isfinite(p->damage_dealt);
}
