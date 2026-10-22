// Player statistics, game side (docs/STATS.md): the event hooks the simulation calls, the match
// lifecycle, host-authoritative combat stats (a client's arrive as deltas every 30 s and at the
// match end), public stats on request, persistence (<pref>/saves/stats_<profile>.json, atomic,
// autosaved at the match end and every 60 s in a match, on a job), the Steam mirror, commands.
// The data model is pure: src/gameplay/stats/stats.h.
#ifndef DG_GAME_STATS_H
#define DG_GAME_STATS_H

#include "core/base.h"
#include "gameplay/stats/stats.h"

struct World;

void game_stats_init(void);                 // cvars + commands (game_init)
void game_stats_shutdown(void);             // ends the match, saves synchronously
void game_stats_frame(float dt);            // time / distance, flushes, autosave, Steam, public stats

// Match lifecycle.
void game_stats_level_loaded(const char *map);    // a new world: the previous match ends (abandoned), a new one starts
void game_stats_level_leaving(void);              // before a world goes away: host deltas flushed, the match ended (abandoned)
void game_stats_match_end(MatchResult result);    // a decided match (story chapter complete, a wave mode's win / loss)
void game_stats_wave_complete(int wave);          // a wave system's wave survived (1-based)

// Host hooks (authoritative combat stats; no-ops on clients / replicas). item: the item that did it
// (0 = none / not a weapon).
void stats_sv_shots(struct World *w, int slot, u16 item, int count);
void stats_sv_damage(struct World *w, int attacker_slot, int victim_slot, u32 target, u16 item, float dealt, bool headshot, bool killed, bool kill_counts);
void stats_sv_down(struct World *w, int slot);
void stats_sv_revive(struct World *w, int reviver_slot, int revived_slot);

// Local hooks (this machine's player only).
void stats_cl_jump(void);
void stats_cl_step(void);

// Net (MSG_STATS, net.c): from_slot >= 0 on the host (the sender's slot), -1 on a client.
void game_stats_net_receive(int from_slot, const u8 *data, int size);

// UI (ui_stats_tab.c)
typedef struct StatsPlayerRow { int slot; char name[32]; bool local; } StatsPlayerRow;
int game_stats_players(StatsPlayerRow *out, int max);         // connected players (the local one first)
const StatsPublic *game_stats_public(int slot);                // NULL until received (requests it)
const char *game_stats_weapon_name(const PlayerStats *ps, int stats_slot);   // the item's display name
const char *game_stats_map_name(const char *map_id);
bool game_stats_imperial(void);
void game_stats_set_imperial(bool on);
bool game_stats_export(char *path_out, int size);              // pretty JSON; the path written
void game_stats_reset_all(void);                               // lifetime + history (saved)
bool game_stats_loaded(void);

#endif
