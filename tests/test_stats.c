// Player statistics (docs/STATS.md): derived values (accuracy, K/D, headshot %, favorite weapon,
// win rate), the killstreak (reset on death and down), match records + the day roll-up (ring
// buffers), the host delta wire format and its in-order replay, public stats, save / load round
// trip, version migration (the old profile.json stubs, unknown / missing keys), stats_fake, and the
// chart math (nice ticks, moving average, hover lookups, calendar cells, number formatting). Pure.
#include "gameplay/stats/stats.h"
#include "ui/ui_chart.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))
#define T0 1790000000   // a fixed clock (2026-09)

static PlayerStats *L(void) { return &stats_get()->life; }

static void fresh(void) {
    stats_get()->now_override = T0;
    stats_get()->utc_offset_sec = 0;
    stats_reset();
}

// ------------------------------------------------------------------ derived values

static void test_derived(void) {
    fresh();
    PlayerStats *ps = L();
    int rifle = stats_weapon_slot(ps, "rifle"), pistol = stats_weapon_slot(ps, "pistol");
    CHECK(rifle == 0 && pistol == 1 && stats_weapon_slot(ps, "rifle") == 0, "weapon registry: stable slots (%d %d)", rifle, pistol);
    CHECK(stats_weapon_slot(ps, "") == -1 && stats_weapon_find(ps, "bat") == -1, "empty / unknown ids");
    // Nothing recorded: every ratio is 0, no favorite.
    CHECK(stats_kd(ps) == 0 && stats_overall_accuracy(ps) == 0 && stats_headshot_pct(ps) == 0 && stats_win_rate(ps) == 0, "zero stats -> zero ratios");
    CHECK(stats_favorite_weapon(ps) == -1, "no favorite weapon yet");
    stats_on_match_start("devroom");
    stats_on_shots_fired(rifle, 10);
    for (int i = 0; i < 4; i++) stats_on_hit(rifle, 25.0f, i == 0);
    stats_on_shots_fired(pistol, 10);
    stats_on_hit(pistol, 20.0f, false);
    stats_on_kill(rifle, 42.0f, true);
    stats_on_kill(rifle, 12.0f, false);
    stats_on_kill(pistol, 5.0f, false);
    CHECK(NEAR(stats_weapon_accuracy(&ps->weapons[rifle]), 0.4, 1e-6), "rifle accuracy 4 / 10 (%.3f)", stats_weapon_accuracy(&ps->weapons[rifle]));
    CHECK(NEAR(stats_overall_accuracy(ps), 0.25, 1e-6), "overall accuracy 5 / 20 (%.3f)", stats_overall_accuracy(ps));
    CHECK(stats_kd(ps) == 3.0f, "K/D with no deaths = kills (%.2f)", stats_kd(ps));
    stats_on_death();
    stats_on_death();
    CHECK(NEAR(stats_kd(ps), 1.5, 1e-6), "K/D 3 / 2 (%.2f)", stats_kd(ps));
    CHECK(NEAR(stats_headshot_pct(ps), 1.0 / 3.0, 1e-6), "headshot %% of kills (%.3f)", stats_headshot_pct(ps));
    CHECK(stats_favorite_weapon(ps) == rifle, "favorite = most kills (%d)", stats_favorite_weapon(ps));
    CHECK(NEAR(ps->damage_dealt, 120.0, 1e-6) && ps->weapons[rifle].headshot_hits == 1, "damage %.1f, headshot hits %u", ps->damage_dealt, ps->weapons[rifle].headshot_hits);
    CHECK(NEAR(ps->longest_kill.dist_m, 42.0, 1e-4) && ps->longest_kill.weapon == rifle && !strcmp(ps->longest_kill.map, "devroom") && ps->longest_kill.timestamp == T0,
          "longest kill: 42 m with the rifle on devroom");
    CHECK(ps->weapons[rifle].kill_dist[stats_kill_dist_bucket(42.0f)] == 1 && stats_kill_dist_bucket(42.0f) == 6 && stats_kill_dist_bucket(500.0f) == STATS_KILL_DIST_BUCKETS - 1 &&
          stats_kill_dist_bucket(0.0f) == 0, "kill distance buckets");
    // Accuracy never goes over 100 % (penetration: two hits from one bullet).
    WeaponStats w = { 0 };
    w.shots_fired = 2;
    w.shots_hit = 3;
    CHECK(stats_weapon_accuracy(&w) == 1.0f, "accuracy clamped to 1");
    stats_on_match_end(MATCH_WIN);
    CHECK(ps->matches_played == 1 && ps->matches_won == 1 && NEAR(stats_win_rate(ps), 1.0, 1e-6), "a won match");
    // Outside a match nothing counts.
    stats_on_kill(rifle, 1, false);
    stats_on_shot_fired(rifle);
    CHECK(ps->kills == 3 && ps->weapons[rifle].shots_fired == 10, "events outside a match are ignored");
}

// ------------------------------------------------------------------ killstreak

static void test_killstreak(void) {
    fresh();
    stats_on_match_start("devroom");
    MatchStats *c = &stats_get()->cur;
    for (int i = 0; i < 5; i++) stats_on_kill(-1, 10, false);
    CHECK(c->streak == 5 && L()->best_killstreak == 5 && c->rec.best_streak == 5, "5 kills: streak 5, best updated live (%d / %u)", c->streak, L()->best_killstreak);
    stats_on_down();
    CHECK(c->streak == 0 && L()->best_killstreak == 5, "a down resets the streak, keeps the best");
    for (int i = 0; i < 3; i++) stats_on_kill(-1, 10, false);
    stats_on_death();
    CHECK(c->streak == 0 && L()->best_killstreak == 5 && c->rec.best_streak == 5, "a death resets it; 3 < 5 keeps the best");
    for (int i = 0; i < 7; i++) stats_on_kill(-1, 10, false);
    CHECK(L()->best_killstreak == 7 && c->rec.best_streak == 7, "a longer streak becomes the best (%u)", L()->best_killstreak);
    stats_on_revive_given();
    CHECK(c->streak == 7, "a revive does not end the streak");
    CHECK(c->rec.downs == 1 && c->rec.deaths == 1 && c->rec.revives == 1 && L()->times_downed == 1, "downs / deaths / revives recorded");
}

// ------------------------------------------------------------------ match lifecycle, rings, days

static void test_matches(void) {
    fresh();
    PlayerStats *ps = L();
    // A short empty match (menu background, a quick map change) is not recorded.
    stats_on_match_start("devroom");
    StatsTickIn in = { true, true, true, 0, 0, -1 };
    stats_tick(5.0f, &in);
    stats_on_match_end(MATCH_ABANDON);
    CHECK(ps->match_count == 0 && ps->matches_played == 0, "an empty 5 s match is dropped");
    // Time, distance (grounded, teleports ignored), equipped time, steps from distance.
    int rifle = stats_weapon_slot(ps, "rifle");
    stats_on_match_start("vegetation");
    in.weapon = rifle;
    for (int i = 0; i <= 60; i++) { in.x = (float)i * 0.1f; stats_tick(1.0f / 60.0f, &in); }   // 6 m in 1 s
    in.x = 500;
    stats_tick(1.0f / 60.0f, &in);   // teleport
    in.grounded = false;
    in.x = 510;
    stats_tick(1.0f / 60.0f, &in);   // in the air: not walking
    stats_on_jump();
    stats_tick(30.0f, &in);
    stats_on_wave_complete(1);
    stats_on_kill(rifle, 3, false);
    stats_on_down();
    stats_on_wave_complete(2);
    stats_on_kill(rifle, 3, false);
    stats_on_kill(rifle, 3, false);
    stats_on_revive_given();
    stats_on_match_end(MATCH_LOSS);
    const MatchRecord *m = stats_match_at(ps, 0);
    CHECK(ps->match_count == 1 && m && !strcmp(m->map, "vegetation") && m->result == MATCH_LOSS, "the match recorded");
    CHECK(m && NEAR(m->distance_m, 6.0, 0.01) && NEAR(ps->total_distance_m, 6.0, 0.01), "grounded distance 6 m, the teleport and the air ignored (%.3f)", m ? m->distance_m : -1);
    CHECK(m && m->steps == 8 && ps->steps == 8, "steps from the distance when no footstep events (%u)", m ? m->steps : 0);
    CHECK(m && m->jumps == 1 && ps->jumps == 1, "jumps");
    CHECK(m && NEAR(m->duration_sec, 1.0 + 3.0 / 60.0 + 30.0, 0.01) && NEAR(ps->weapons[rifle].time_equipped_sec, m->duration_sec, 0.01), "duration %.2f s = time equipped", m ? m->duration_sec : 0);
    CHECK(m && m->segment_sec == 0 && m->waves_reached == 3 && ps->highest_wave == 3, "waves reported: the per-wave arrays are waves (reached %u)", m ? m->waves_reached : 0);
    CHECK(m && m->kills_per_wave[1] == 1 && m->kills_per_wave[2] == 2 && m->downs_per_wave[1] == 1 && m->revives_per_wave[2] == 1, "kills / downs / revives per wave");
    CHECK(m && m->kills_per_weapon[rifle] == 3, "kills per weapon");
    // Footsteps, when they arrive, count instead of the stride.
    stats_on_match_start("devroom");
    in.grounded = true;
    in.x = 0;
    stats_tick(0.016f, &in);
    in.x = 1.2f;
    stats_tick(0.016f, &in);
    for (int i = 0; i < 3; i++) stats_on_step();
    stats_tick(20.0f, &in);
    stats_on_match_end(MATCH_ABANDON);
    CHECK(stats_match_at(ps, 1)->steps == 3, "footstep events win over the stride (%u)", stats_match_at(ps, 1)->steps);
    // No wave system: time segments.
    stats_on_match_start("devroom");
    stats_tick((float)STATS_SEGMENT_SEC * 2.5f, &in);
    stats_on_kill(-1, 2, false);
    CHECK(stats_current_wave() == 2 && stats_get()->cur.rec.kills_per_wave[2] == 1 && stats_get()->cur.rec.segment_sec == STATS_SEGMENT_SEC, "segments of %d s", STATS_SEGMENT_SEC);
    stats_on_match_end(MATCH_ABANDON);
    // The ring keeps the last STATS_MAX_MATCHES, oldest first.
    for (int i = 0; i < STATS_MAX_MATCHES + 7; i++) {
        stats_get()->now_override = T0 + 3600 * (i + 1);
        stats_on_match_start("devroom");
        for (int k = 0; k < i % 5; k++) stats_on_kill(-1, 1, false);
        stats_tick(20.0f, &in);
        stats_on_match_end(MATCH_WIN);
    }
    CHECK(ps->match_count == STATS_MAX_MATCHES, "ring full (%d)", ps->match_count);
    CHECK(stats_match_at(ps, STATS_MAX_MATCHES - 1)->timestamp == T0 + 3600 * (STATS_MAX_MATCHES + 7), "newest last");
    CHECK(stats_match_at(ps, 0)->timestamp < stats_match_at(ps, 1)->timestamp, "oldest first");
    CHECK(ps->matches_played == 3 + STATS_MAX_MATCHES + 7, "lifetime matches keep counting (%u)", ps->matches_played);
    // The days: one roll-up per calendar day.
    int total = 0;
    for (int i = 0; i < ps->day_count; i++) total += stats_day_at(ps, i)->matches;
    CHECK(total == (int)ps->matches_played && ps->day_count >= 4 && ps->day_count <= 7, "day roll-up: %d days, %d matches", ps->day_count, total);
    for (int i = 1; i < ps->day_count; i++) CHECK(stats_day_at(ps, i)->day > stats_day_at(ps, i - 1)->day, "days ascending");
    const DailyRecord *d = stats_day_find(ps, stats_day_of(T0 + 3600 * 30));
    CHECK(d && d->day == stats_day_of(T0 + 3600 * 30), "stats_day_find");
}

static void test_calendar(void) {
    int y, m, d;
    stats_day_ymd(0, &y, &m, &d);
    CHECK(y == 1970 && m == 1 && d == 1 && stats_weekday(0) == 3, "1970-01-01 Thursday");
    stats_day_ymd(20718, &y, &m, &d);   // 2026-09-22
    CHECK(y == 2026 && m == 9 && d == 22 && stats_weekday(20718) == 1, "2026-09-22 Tuesday (%d-%d-%d wd %d)", y, m, d, stats_weekday(20718));
    stats_day_ymd(19782, &y, &m, &d);   // 2024-02-29
    CHECK(y == 2024 && m == 2 && d == 29, "leap day");
    stats_get()->utc_offset_sec = 3 * 3600;
    CHECK(stats_day_of(20718LL * 86400 + 22 * 3600) == 20719, "UTC+3: 22:00 UTC is the next local day");
    stats_get()->utc_offset_sec = 0;
}

// ------------------------------------------------------------------ host deltas

static void test_delta(void) {
    fresh();
    StatsDelta d;
    memset(&d, 0, sizeof(d));
    CHECK(stats_delta_empty(&d), "empty delta");
    stats_delta_shot(&d, 2, 30);
    stats_delta_hit(&d, 2, 40.0f, true);
    stats_delta_hit(&d, 2, 30.0f, false);
    stats_delta_hit(&d, -1, 90.0f, false);   // a grenade
    stats_delta_kill(&d, 2, 33.3f, true);
    stats_delta_kill(&d, 2, 4.0f, false);
    stats_delta_event(&d, SDK_DOWN);
    stats_delta_kill(&d, 2, 10.0f, false);
    stats_delta_event(&d, SDK_REVIVE);
    stats_delta_taken(&d, 55.0f);
    u8 buf[1024];
    int n = stats_delta_write(&d, buf, sizeof(buf));
    CHECK(n > 0 && n < 64, "small on the wire (%d bytes)", n);
    StatsDelta r;
    CHECK(stats_delta_read(&r, buf, n), "reads back");
    CHECK(r.wmask == (1u << 2) && r.w[2].shots == 30 && r.w[2].hits == 2 && r.w[2].head_hits == 1 && NEAR(r.w[2].damage, 70, 1e-4) && NEAR(r.damage_other, 90, 1e-4) &&
          r.n == 5 && NEAR(r.damage_taken, 55, 1e-4), "round trip");
    CHECK(!stats_delta_read(&r, buf, n - 1), "a truncated delta is rejected");
    // Applied in order on the client: the down ends the streak between the kills.
    i8 map[STATS_WEAPON_COUNT];
    memset(map, -1, sizeof(map));
    map[2] = (i8)stats_weapon_slot(L(), "rifle");
    stats_on_match_start("devroom");
    stats_apply_delta(&d, map);
    const WeaponStats *w = &L()->weapons[map[2]];
    CHECK(L()->kills == 3 && L()->headshots == 1 && w->kills == 3 && w->shots_fired == 30 && w->shots_hit == 2, "kills / shots / hits applied");
    CHECK(NEAR(L()->damage_dealt, 160, 1e-3) && NEAR(L()->damage_taken, 55, 1e-3) && L()->times_downed == 1 && L()->revives_given == 1, "damage / down / revive applied");
    CHECK(L()->best_killstreak == 2 && stats_get()->cur.streak == 1, "streak replayed in order (best %u, now %d)", L()->best_killstreak, stats_get()->cur.streak);
    CHECK(NEAR(w->longest_kill_m, 33.3, 0.05), "kill distance in decimetres (%.2f)", w->longest_kill_m);
    // Full log: the host flushes.
    memset(&d, 0, sizeof(d));
    for (int i = 0; i < STATS_DELTA_EVENTS; i++) stats_delta_kill(&d, 1, 1, false);
    CHECK(stats_delta_full(&d) && d.n == STATS_DELTA_EVENTS, "a full event log asks for a flush");
    // Public stats.
    StatsPublic p, q;
    stats_public_from(&p, L(), "Alex");
    n = stats_public_write(&p, buf, sizeof(buf));
    CHECK(n == (int)sizeof(StatsPublic) && stats_public_read(&q, buf, n) && !strcmp(q.name, "Alex") && q.kills == 3 && !strcmp(q.favorite, "rifle") && q.shots_fired == 30,
          "public stats round trip");
}

// ------------------------------------------------------------------ persistence

static void test_save_load(void) {
    fresh();
    const char *maps[2] = { "devroom", "vegetation" };
    const char *weapons[4] = { "rifle", "pistol", "shotgun", "sniper" };
    stats_fake(L(), 140, 7, T0, maps, 2, weapons, 4, 0);
    PlayerStats *ps = L();
    CHECK(ps->matches_played == 140 && ps->match_count == STATS_MAX_MATCHES, "stats_fake: 140 matches, the ring keeps 100");
    CHECK(ps->kills > 0 && stats_overall_accuracy(ps) > 0.05f && stats_overall_accuracy(ps) < 0.95f && ps->weapon_count == 4, "plausible totals (kills %u, acc %.2f)", ps->kills,
          stats_overall_accuracy(ps));
    CHECK(stats_match_at(ps, 0)->timestamp >= T0 - 366 * 86400 && stats_match_at(ps, 99)->timestamp < T0, "dates over the last year");
    // Correlation: the better-accuracy half of the matches has more kills on average.
    double ka = 0, kb = 0;
    int na = 0, nb = 0;
    for (int i = 0; i < ps->match_count; i++) {
        const MatchRecord *m = stats_match_at(ps, i);
        if (stats_match_accuracy(m) >= stats_overall_accuracy(ps)) { ka += m->kills; na++; }
        else { kb += m->kills; nb++; }
    }
    CHECK(na && nb && ka / na > kb / nb, "accuracy correlates with kills (%.1f vs %.1f)", na ? ka / na : 0, nb ? kb / nb : 0);
    // A match in progress is saved too.
    stats_get()->now_override = T0 + 100;
    stats_on_match_start("wateryard");
    stats_on_kill(0, 12, true);
    char *json = stats_to_json(ps, &stats_get()->cur, false);
    CHECK(json != NULL, "serialised");
    PlayerStats *back = (PlayerStats *)calloc(1, sizeof(PlayerStats));
    MatchStats cur;
    char err[128] = "";
    CHECK(stats_from_json(back, &cur, json, err, sizeof(err)), "parsed back (%s)", err);
    CHECK(back->kills == ps->kills && back->deaths == ps->deaths && back->matches_played == ps->matches_played && back->steps == ps->steps &&
          NEAR(back->damage_dealt, ps->damage_dealt, 1e-6 * ps->damage_dealt + 1e-3) && NEAR(back->time_played_sec, ps->time_played_sec, 1e-3), "lifetime totals");
    CHECK(back->weapon_count == ps->weapon_count && !strcmp(back->weapon_ids[3], "sniper") && back->weapons[1].kills == ps->weapons[1].kills &&
          memcmp(back->weapons[0].kill_dist, ps->weapons[0].kill_dist, sizeof(ps->weapons[0].kill_dist)) == 0, "weapons + registry");
    CHECK(back->match_count == ps->match_count && back->day_count == ps->day_count, "history sizes (%d / %d, %d / %d)", back->match_count, ps->match_count, back->day_count, ps->day_count);
    bool same = true;
    for (int i = 0; i < ps->match_count && same; i++) same = memcmp(stats_match_at(back, i), stats_match_at(ps, i), sizeof(MatchRecord)) == 0;
    CHECK(same, "every match record identical");
    same = true;
    for (int i = 0; i < ps->day_count && same; i++) same = memcmp(stats_day_at(back, i), stats_day_at(ps, i), sizeof(DailyRecord)) == 0;
    CHECK(same, "every day identical");
    CHECK(back->longest_kill.dist_m == ps->longest_kill.dist_m && !strcmp(back->longest_kill.map, ps->longest_kill.map), "longest kill");
    CHECK(cur.active && cur.rec.kills == 1 && !strcmp(cur.rec.map, "wateryard") && cur.streak == 1, "the match in progress");
    // Byte-identical when written again.
    char *json2 = stats_to_json(back, &cur, false);
    CHECK(json2 && !strcmp(json, json2) && stats_crc32(json, strlen(json)) == stats_crc32(json2, strlen(json2)), "stable output");
    free(json);
    free(json2);
    free(back);
    CHECK(stats_crc32("123456789", 9) == 0xCBF43926u, "CRC-32 check value");
}

static void test_migration(void) {
    fresh();
    // Version 0: the old profile.json and its stat stubs.
    const char *profile = "{\"version\":1,\"lan_name\":\"Survivor\",\"last_map\":\"devroom\",\"stats\":{\"kills\":41,\"deaths\":9,\"matches\":12,\"playtime_s\":5400.5}}";
    PlayerStats *ps = (PlayerStats *)calloc(1, sizeof(PlayerStats));
    MatchStats cur;
    CHECK(stats_from_json(ps, &cur, profile, NULL, 0), "profile.json reads");
    CHECK(ps->version == STATS_VERSION && ps->kills == 41 && ps->deaths == 9 && ps->matches_played == 12 && NEAR(ps->time_played_sec, 5400.5, 1e-6),
          "v0 stubs carried over (kills %u, deaths %u, matches %u)", ps->kills, ps->deaths, ps->matches_played);
    CHECK(ps->longest_kill.weapon == -1 && ps->match_count == 0 && !cur.active, "v0: no weapons / history");
    // Missing keys stay zero, unknown keys (a newer build's) are skipped, arrays shorter / longer are fine.
    const char *newer = "{\"version\":99,\"profile\":\"local\",\"kills\":5,\"future_field\":123,\"weapons\":[{\"id\":\"rifle\",\"kills\":5,\"kill_dist\":[1,2],\"plasma\":7}],"
                        "\"matches\":[{\"t\":1790000000,\"map\":\"devroom\",\"kills\":5,\"kills_per_wave\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,"
                        "29,30,31,32,33,34,35,36,37,38,39,40,41,42,43],\"new_array\":[1]}]}";
    char err[128] = "";
    CHECK(stats_from_json(ps, &cur, newer, err, sizeof(err)), "a newer file loads (%s)", err);
    CHECK(err[0] != 0, "with a warning about the version");
    CHECK(ps->kills == 5 && ps->deaths == 0 && ps->weapon_count == 1 && ps->weapons[0].kills == 5 && ps->weapons[0].kill_dist[1] == 2 && ps->weapons[0].kill_dist[2] == 0, "known keys read, missing zero");
    const MatchRecord *m = stats_match_at(ps, 0);
    CHECK(m && m->kills_per_wave[STATS_MAX_WAVES - 1] == STATS_MAX_WAVES && m->downs == 0, "a longer array is cut at the struct's size");
    CHECK(ps->version == STATS_VERSION, "brought to the current version");
    // Garbage.
    CHECK(!stats_from_json(ps, &cur, "not json", err, sizeof(err)) && ps->kills == 0, "garbage: false, fresh stats");
    free(ps);
}

// stats_fake with the game's weapon set over many seeds: every value plausible (a clamp around a
// random draw once let a negative accuracy through: DG_CLAMP evaluates its argument more than once).
static void test_fake_bounds(void) {
    const char *maps[3] = { "devroom", "vegetation", "wateryard" };
    const char *w[10] = { "pistol", "rifle", "shotgun", "crossbow", "bat", "knife", "smg", "lmg", "dmr", "sniper" };
    PlayerStats *ps = (PlayerStats *)calloc(1, sizeof(PlayerStats));
    int bad = 0;
    for (u32 seed = 1; seed <= 40; seed++) {
        stats_init_profile(ps, "fake");
        stats_fake(ps, 120, seed, T0 + (i64)seed * 7919, maps, 3, w, 10, 3600);
        for (int i = 0; i < ps->match_count; i++) {
            const MatchRecord *m = stats_match_at(ps, i);
            bad += m->kills > 2000 || m->shots_hit > m->shots_fired || m->headshots > m->kills || m->waves_reached < 1 || m->duration_sec <= 0;
            for (int k = 0; k < STATS_WEAPON_COUNT; k++) bad += m->hits_per_weapon[k] > m->shots_per_weapon[k];
        }
        for (int k = 0; k < ps->weapon_count; k++) bad += ps->weapons[k].shots_hit > ps->weapons[k].shots_fired || ps->weapons[k].kills > 100000;
        bad += ps->matches_played != 120;
    }
    CHECK(bad == 0, "stats_fake: every match and weapon plausible over 40 seeds (%d bad)", bad);
    free(ps);
}

// ------------------------------------------------------------------ chart math

static void test_ticks(void) {
    ChartTicks t = chart_nice_ticks(0, 97, 6, true);
    CHECK(t.lo == 0 && t.hi == 100 && t.step == 20 && t.count == 6, "0..97 -> 0..100 step 20 (%g..%g step %g, %d)", t.lo, t.hi, t.step, t.count);
    t = chart_nice_ticks(0, 1, 5, true);
    CHECK(t.lo == 0 && NEAR(t.hi, 1, 1e-9) && (NEAR(t.step, 0.5, 1e-9) || NEAR(t.step, 0.2, 1e-9)), "0..1 step 0.2 or 0.5 (%g)", t.step);
    t = chart_nice_ticks(13, 47, 5, false);
    CHECK(t.lo <= 13 && t.hi >= 47 && NEAR(fmod(t.step / pow(10, floor(log10(t.step))), 1.0), 0, 1e-9) && t.count <= 5, "13..47 covered by %g..%g step %g", t.lo, t.hi, t.step);
    t = chart_nice_ticks(5, 5, 5, true);
    CHECK(t.lo == 0 && t.hi >= 5 && t.count >= 2, "flat 5 with zero: 0..%g", t.hi);
    t = chart_nice_ticks(0, 0, 5, true);
    CHECK(t.lo == 0 && t.hi > 0, "all zero: a unit range (%g)", t.hi);
    t = chart_nice_ticks(-3.2, 7.9, 6, true);
    CHECK(t.lo <= -3.2 && t.hi >= 7.9 && t.count <= 6, "negative range %g..%g", t.lo, t.hi);
    t = chart_nice_ticks(0, 123456, 6, true);
    CHECK(t.step == 50000 && t.hi == 150000, "large: step %g", t.step);
    for (int k = 2; k <= 10; k++) {
        double s = chart_nice_step(1234.5, k);
        double f = s / pow(10, floor(log10(s)));
        CHECK(NEAR(f, 1, 1e-9) || NEAR(f, 2, 1e-9) || NEAR(f, 5, 1e-9), "nice step 1 / 2 / 5 x 10^n (%g)", s);
    }
}

static void test_moving_average(void) {
    float in[6] = { 1, 2, 3, 4, 5, 6 }, out[6];
    chart_moving_average(in, 6, 3, out);
    CHECK(out[0] == 1 && out[1] == 1.5f && out[2] == 2 && out[5] == 5, "window 3 (%g %g %g %g)", out[0], out[1], out[2], out[5]);
    chart_moving_average(in, 6, 3, in);   // in place
    CHECK(in[5] == 5 && in[2] == 2 && in[0] == 1, "in place");
    float g[4] = { 2, NAN, 4, NAN };
    chart_moving_average(g, 4, 2, out);
    CHECK(out[0] == 2 && out[1] == 2 && out[2] == 4 && out[3] == 4, "gaps skipped");
    float one[1] = { 7 };
    chart_moving_average(one, 1, 1, out);
    CHECK(out[0] == 7, "window 1 = the data");
}

static void test_chart_math(void) {
    CHECK(chart_nearest_index(50, 0, 100, 3) == 1 && chart_nearest_index(0, 0, 100, 3) == 0 && chart_nearest_index(100, 0, 100, 3) == 2 && chart_nearest_index(200, 0, 100, 3) == -1,
          "nearest index");
    CHECK(chart_nearest_index(10, 0, 100, 1) == 0, "one point");
    float px[3] = { 0, 10, 20 }, py[3] = { 0, 0, 0 };
    CHECK(chart_nearest_point(px, py, 3, 11, 1, 25) == 1 && chart_nearest_point(px, py, 3, 50, 50, 25) == -1, "nearest point");
    float v[3] = { 1, 0, 3 };
    CHECK(chart_donut_segment(v, 3, 0.1f) == 0 && chart_donut_segment(v, 3, 3.0f) == 2 && chart_donut_segment(v, 3, 6.2f) == 2, "donut segments (zero skipped)");
    float z[2] = { 0, 0 };
    CHECK(chart_donut_segment(z, 2, 1) == -1, "empty donut");
    CHECK(chart_bucket(0, 0, 10, 5) == 0 && chart_bucket(9.99f, 0, 10, 5) == 4 && chart_bucket(50, 0, 10, 5) == 4 && chart_bucket(-1, 0, 10, 5) == 0, "buckets");
    int col, row;
    int cols = chart_calendar_cell(364, 365, 2, &col, &row);   // the last day a Wednesday
    CHECK(row == 2 && col == cols - 1 && cols == 53, "calendar: the last day in the last column (%d cols, %d,%d)", cols, col, row);
    chart_calendar_cell(0, 365, 2, &col, &row);
    CHECK(col == 0 && row == (2 - 364 % 7 + 7) % 7, "the first day in column 0 on its weekday (%d)", row);
    chart_calendar_cell(363, 365, 2, &col, &row);
    CHECK(row == 1, "the day before: Tuesday");
    char b[48];
    CHECK(!strcmp(chart_fmt_int(b, sizeof(b), 1234), "1,234") && !strcmp(chart_fmt_int(b, sizeof(b), -1234567), "-1,234,567") && !strcmp(chart_fmt_int(b, sizeof(b), 12), "12"),
          "thousands (%s)", b);
    CHECK(!strcmp(chart_fmt_pct(b, sizeof(b), 0.1234, 1), "12.3 %"), "percent (%s)", b);
    CHECK(!strcmp(chart_fmt_num(b, sizeof(b), 12345.678, 2), "12,345.68"), "decimals (%s)", b);
    CHECK(!strcmp(chart_fmt_dist(b, sizeof(b), 1234, false), "1.2 km") && !strcmp(chart_fmt_dist(b, sizeof(b), 850, false), "850 m"), "metric (%s)", b);
    CHECK(!strcmp(chart_fmt_dist(b, sizeof(b), 100, true), "328 ft") && !strcmp(chart_fmt_dist(b, sizeof(b), 1931, true), "1.2 mi"), "imperial (%s)", b);
    CHECK(!strcmp(chart_fmt_duration(b, sizeof(b), 4980), "1h 23m") && !strcmp(chart_fmt_duration(b, sizeof(b), 125), "2m 05s") && !strcmp(chart_fmt_duration(b, sizeof(b), 9), "9s"),
          "durations (%s)", b);
    CHECK(!strcmp(chart_fmt_compact(b, sizeof(b), 1500), "1.5k") && !strcmp(chart_fmt_compact(b, sizeof(b), 950), "950") && !strcmp(chart_fmt_compact(b, sizeof(b), 0.25), "0.25") &&
          !strcmp(chart_fmt_compact(b, sizeof(b), 2500000), "2.5M"), "compact (%s)", b);
}

int main(void) {
    test_derived();
    test_killstreak();
    test_matches();
    test_calendar();
    test_delta();
    test_save_load();
    test_migration();
    test_fake_bounds();
    test_ticks();
    test_moving_average();
    test_chart_math();
    printf("test_stats: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
