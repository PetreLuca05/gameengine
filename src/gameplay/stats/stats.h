// Player statistics (docs/STATS.md): lifetime totals per profile (Steam id / local), per-weapon
// totals, the last STATS_MAX_MATCHES match records (the per-match charts' source), a per-day
// roll-up for the last year, and the match in progress. Plain zero-initialised structs; pure (no
// engine): the game glue (src/game/game_stats.c) feeds the events, saves and replicates.
//
// Weapons are kept by the stats' own registry (`weapon_ids`): slot i is the weapon named
// weapon_ids[i] in this file, whatever the item database's order is (a weapon removed from the
// game keeps its numbers). The glue maps the item database's weapon indices to these slots.
//
// Events are cheap (no allocation, a few adds) and apply to the active stats (stats_get()).
// Combat events of a client come from the host (stats_apply_delta, stats_net.c); movement and time
// are tracked by every machine for its own player.
#ifndef DG_STATS_H
#define DG_STATS_H

#include "core/base.h"

#define STATS_VERSION 1
#define STATS_WEAPON_COUNT 32       // >= MAX_WEAPON_DEFS (the glue checks)
#define STATS_WEAPON_ID 24
#define STATS_MAP_ID 24
#define STATS_MAX_MATCHES 100       // match ring buffer
#define STATS_MAX_DAYS 365          // daily roll-up ring buffer
#define STATS_MAX_WAVES 40          // per-wave timeline buckets
#define STATS_KILL_DIST_BUCKETS 12  // kill distance histogram per weapon (stats_kill_dist_edges)
#define STATS_SEGMENT_SEC 120       // no wave system reporting: the timeline in 2-minute segments
#define STATS_STRIDE_M 0.75f        // steps from distance when no footstep events arrived
#define STATS_MIN_MATCH_SEC 15.0f   // shorter matches with nothing in them are not recorded

typedef enum MatchResult { MATCH_ABANDON, MATCH_WIN, MATCH_LOSS, MATCH_RESULT_COUNT } MatchResult;

typedef struct WeaponStats {
    u32 shots_fired, shots_hit;     // projectiles (a shotgun shell counts its pellets), melee swings
    u32 kills, headshots;           // headshots: headshot kills
    double damage_dealt;
    double time_equipped_sec;
    float longest_kill_m;
    u32 headshot_hits;
    u32 kill_dist[STATS_KILL_DIST_BUCKETS];
} WeaponStats;

typedef struct LongestKill {
    float dist_m;
    i16 weapon;                     // stats weapon slot, -1 none
    char map[STATS_MAP_ID];
    i64 timestamp;                  // unix seconds
} LongestKill;

typedef struct MatchRecord {
    i64 timestamp;                  // unix seconds at the start
    char map[STATS_MAP_ID];
    u8 result;                      // MatchResult
    u16 segment_sec;                // 0: the per-wave arrays are waves; else segments of this many seconds
    float duration_sec;
    u16 waves_reached;
    u32 kills, deaths, headshots, shots_fired, shots_hit;
    float damage_dealt, damage_taken;
    u16 revives, downs;
    float distance_m;
    u16 best_streak;
    u16 kills_per_weapon[STATS_WEAPON_COUNT];
    u16 kills_per_wave[STATS_MAX_WAVES];
    u8 downs_per_wave[STATS_MAX_WAVES];
    // appended (STATS_VERSION 1)
    u8 revives_per_wave[STATS_MAX_WAVES];
    u16 shots_per_weapon[STATS_WEAPON_COUNT];
    u16 hits_per_weapon[STATS_WEAPON_COUNT];
    float longest_kill_m;
    u32 steps;
    u16 jumps;
} MatchRecord;

typedef struct DailyRecord {
    i32 day;                        // days since 1970-01-01 (local calendar)
    u16 matches;
    u32 kills, deaths;
    float time_sec;
    u32 shots_fired, shots_hit;
} DailyRecord;

typedef struct PlayerStats {
    u32 version;
    char profile[40];               // Steam id or "local"
    // combat
    u32 kills, deaths, headshots;
    double damage_dealt, damage_taken;
    u32 best_killstreak;
    LongestKill longest_kill;
    // team / survival
    u32 revives_given, times_downed;
    double time_played_sec;
    // movement
    double total_distance_m;
    u64 steps;
    u32 jumps;
    // matches
    u32 matches_played, matches_won, highest_wave;
    // weapons (by stats slot)
    char weapon_ids[STATS_WEAPON_COUNT][STATS_WEAPON_ID];
    int weapon_count;
    WeaponStats weapons[STATS_WEAPON_COUNT];
    // history: ring buffers (stats_match_at / stats_day_at read them oldest first)
    MatchRecord matches[STATS_MAX_MATCHES];
    int match_head, match_count;    // head: the next write
    DailyRecord days[STATS_MAX_DAYS];
    int day_head, day_count;
    u32 revision;                   // bumped on every change the tab shows (chart rebuilds)
} PlayerStats;

// The match in progress.
typedef struct MatchStats {
    bool active;
    MatchRecord rec;
    int streak;                     // kills since the last death / down
    int wave;                       // the wave being played (0-based)
    bool waves;                     // a wave system reports waves (else time segments)
    u32 step_events;                // footsteps seen (else steps come from the distance)
    bool have_pos;                  // movement: the last grounded position
    float last_x, last_z;
} MatchStats;

// What stats_tick needs from the local player.
typedef struct StatsTickIn {
    bool in_match;                  // false: menus, loading, the main menu's background world
    bool alive;                     // not dead (downed counts as alive)
    bool grounded;                  // on foot on the ground (no vehicle, swim, ladder, noclip)
    float x, z;                     // feet position
    int weapon;                     // stats slot of the equipped weapon, -1 none
} StatsTickIn;

typedef struct StatsState {
    PlayerStats life;
    MatchStats cur;
    i64 now_override;               // tests / stats_fake: the clock (0 = time())
    i32 utc_offset_sec;             // local calendar (days)
} StatsState;

// ---- the active stats
StatsState *stats_get(void);
void stats_reset(void);                         // zero, version set (a new profile)
void stats_init_profile(PlayerStats *ps, const char *profile);
i64 stats_now(void);
i32 stats_day_of(i64 unix_time);                // local calendar day number
void stats_day_ymd(i32 day, int *y, int *m, int *d);
int stats_weekday(i32 day);                     // 0 Monday .. 6 Sunday

// Weapon registry: the slot of a weapon id (added when new; -1 when full / empty id).
int stats_weapon_slot(PlayerStats *ps, const char *weapon_id);
int stats_weapon_find(const PlayerStats *ps, const char *weapon_id);

// ---- events (the active stats; ignored outside a match)
void stats_on_match_start(const char *map);
void stats_on_match_end(MatchResult result);    // pushes the record, totals, the day; ends the match
void stats_on_shot_fired(int weapon);           // stats slot, -1 none
void stats_on_shots_fired(int weapon, u32 count);
void stats_on_hit(int weapon, float damage, bool headshot);
void stats_on_kill(int weapon, float dist_m, bool headshot);
void stats_on_death(void);
void stats_on_down(void);
void stats_on_revive_given(void);
void stats_on_damage_taken(float amount);
void stats_on_jump(void);
void stats_on_step(void);                       // a footstep event of the local player
void stats_on_wave_complete(int wave);          // 1-based wave just survived: the next wave starts
void stats_tick(float dt, const StatsTickIn *in);
int stats_current_wave(void);                   // timeline bucket (wave or segment), 0-based

// ---- derived
float stats_ratio(double num, double den);      // num / den, 0 when den is 0
float stats_weapon_accuracy(const WeaponStats *w);   // 0..1
float stats_kd(const PlayerStats *ps);          // kills / max(deaths, 1)
float stats_headshot_pct(const PlayerStats *ps);     // 0..1 of kills
float stats_overall_accuracy(const PlayerStats *ps); // 0..1, over every weapon
int stats_favorite_weapon(const PlayerStats *ps);    // most kills (ties: most time equipped), -1 none
float stats_win_rate(const PlayerStats *ps);         // 0..1
u32 stats_total_shots(const PlayerStats *ps);
u32 stats_total_hits(const PlayerStats *ps);
float stats_match_accuracy(const MatchRecord *m);
float stats_match_kd(const MatchRecord *m);
int stats_kill_dist_bucket(float dist_m);
const float *stats_kill_dist_edges(void);        // STATS_KILL_DIST_BUCKETS + 1 lower edges (m; the last is open)

// ---- history (oldest first)
const MatchRecord *stats_match_at(const PlayerStats *ps, int i);   // 0 .. match_count - 1
const DailyRecord *stats_day_at(const PlayerStats *ps, int i);     // 0 .. day_count - 1
const DailyRecord *stats_day_find(const PlayerStats *ps, i32 day);
// Pure pieces of stats_on_match_end (stats_fake builds histories with them).
void stats_push_match(PlayerStats *ps, const MatchRecord *m);       // ring + day roll-up (no totals)
void stats_add_match_totals(PlayerStats *ps, const MatchRecord *m);

// ---- host -> client combat deltas (stats_net.c): the host accumulates a player's combat events;
// the client applies them in order. Weapons on the wire are item database weapon indices
// (identical on both ends); wire_to_slot maps them to the client's stats slots.
#define STATS_DELTA_EVENTS 64
typedef enum StatsDeltaKind { SDK_KILL = 1, SDK_HEADSHOT_KILL, SDK_DEATH, SDK_DOWN, SDK_REVIVE } StatsDeltaKind;
typedef struct StatsDeltaWeapon { u16 shots, hits, head_hits; float damage; } StatsDeltaWeapon;
typedef struct StatsDeltaEvent { u8 kind, weapon; u16 value; } StatsDeltaEvent;   // weapon 255 none; value: kill distance in dm
typedef struct StatsDelta {
    u32 wmask;                      // weapons with counters
    StatsDeltaWeapon w[STATS_WEAPON_COUNT];
    float damage_other;             // damage without a weapon (throwables)
    float damage_taken;
    int n;
    StatsDeltaEvent ev[STATS_DELTA_EVENTS];
} StatsDelta;

bool stats_delta_empty(const StatsDelta *d);
bool stats_delta_full(const StatsDelta *d);     // flush now (event log full / counters near their limit)
void stats_delta_shot(StatsDelta *d, int wire_weapon, u32 count);
void stats_delta_hit(StatsDelta *d, int wire_weapon, float damage, bool headshot);
void stats_delta_kill(StatsDelta *d, int wire_weapon, float dist_m, bool headshot);
void stats_delta_event(StatsDelta *d, StatsDeltaKind kind);
void stats_delta_taken(StatsDelta *d, float amount);
void stats_apply_delta(const StatsDelta *d, const i8 *wire_to_slot);   // to the active stats
// Wire format (small: only the weapons with counters, 4 bytes per event). Returns bytes / success.
int stats_delta_write(const StatsDelta *d, u8 *out, int cap);
bool stats_delta_read(StatsDelta *d, const u8 *in, int size);

// Public stats: what other players see in the player selector / compare view.
typedef struct StatsPublic {
    char name[32];
    char favorite[STATS_WEAPON_ID];
    u32 kills, deaths, headshots, shots_fired, shots_hit;
    u32 revives_given, times_downed, matches_played, matches_won, highest_wave, best_killstreak;
    float time_played_sec, distance_m, longest_kill_m, damage_dealt;
} StatsPublic;
void stats_public_from(StatsPublic *p, const PlayerStats *ps, const char *name);
int stats_public_write(const StatsPublic *p, u8 *out, int cap);
bool stats_public_read(StatsPublic *p, const u8 *in, int size);

// ---- persistence (stats_save.c): JSON in the project's save style, every field from a table, so
// appending a field is one table row; keys a file lacks stay zero, unknown keys are skipped.
// Version 0 is the old profile.json (no "profile" key, the stat stubs under "stats": kills, deaths,
// matches, playtime_s): loading it carries the stubs over, stats_migrate brings it up to date.
char *stats_to_json(const PlayerStats *ps, const MatchStats *cur, bool pretty);   // malloc'd (free())
bool stats_from_json(PlayerStats *ps, MatchStats *cur, const char *json, char *err, int err_size);
void stats_migrate(PlayerStats *ps, int from_version);
u32 stats_crc32(const void *data, size_t size);

// ---- dev data (stats_fake.c): n plausible matches spread over the last year (correlated: better
// accuracy, more kills; varied maps / weapons / dates). Appended to ps with the totals and days.
void stats_fake(PlayerStats *ps, int n, u32 seed, i64 now, const char *const *maps, int map_count,
                const char *const *weapons, int weapon_count, i32 utc_offset_sec);

#endif
