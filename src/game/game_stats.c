// Player statistics, game side (game_stats.h, docs/STATS.md).
//
//   host:   combat events of every player (apply_damage_ex, fire, revive) -> the local player's stats
//           directly, a remote player's into its StatsDelta -> MSG_STATS STN_DELTA every 30 s, when
//           the event log fills, on a wave and before the world goes away
//   client: STN_DELTA -> stats_apply_delta; movement / time / equipped time tracked locally
//   both:   STN_PUBLIC (a client's own public stats to the host every 30 s; the host answers
//           STN_REQUEST with the cached copy, STN_NONE when it has none)
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE   // localtime_r / gmtime_r under -std=c11
#endif
#include "game_stats.h"
#include "game_internal.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/platform.h"
#include "gameplay/items.h"
#include "gameplay/weapons.h"
#include "net/net.h"
#include "net/protocol.h"
#include "net/lobby_net.h"
#include "ui/mainmenu/mainmenu.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(DEADGROUND_STEAM) && defined(DG_HAVE_STEAM)
#include "steam_shim.h"
#endif

#define G (g_engine.game)
#define FLUSH_SEC 30.0f        // host: combat deltas to each client
#define AUTOSAVE_SEC 60.0f     // in a match
#define PUBLIC_SEC 30.0f       // client: own public stats to the host
#define PUBLIC_FRESH 10.0      // a received public copy is re-requested after this
#define WIRE_UNMAPPED (-2)

_Static_assert(MAX_WEAPON_DEFS <= STATS_WEAPON_COUNT, "STATS_WEAPON_COUNT must cover every weapon def");

typedef enum StatsNetOp { STN_DELTA = 1, STN_PUBLIC, STN_REQUEST, STN_NONE } StatsNetOp;

typedef struct SaveJob {
    char path[512];
    PlayerStats life;
    MatchStats cur;
    u32 last_crc, crc;
    bool ok, skipped;
} SaveJob;

static struct {
    bool inited, loaded;
    char profile[40];
    i8 wire_to_slot[STATS_WEAPON_COUNT];
    StatsDelta pending[MAX_PLAYERS];        // host: remote players' combat since the last flush
    float flush_t, save_t, public_t;
    bool public_sent;                       // client: since the welcome
    StatsPublic pub[MAX_PLAYERS];
    bool pub_ok[MAX_PLAYERS];
    double pub_time[MAX_PLAYERS], pub_req[MAX_PLAYERS];
    StatsPublic local_pub;
    JobCounter save_jobs;
    bool save_busy, save_again;
    u32 last_crc;
    Cvar *units;
} GS;

// ------------------------------------------------------------------ weapons

static const ItemDB *items_db(void) { return g_engine.items; }

static int wire_of_item(u16 item) {
    const ItemDB *db = items_db();
    if (!item || !db) return -1;
    const ItemDef *d = item_def(db, item);
    if (!d || d->kind != ITEM_WEAPON || d->weapon < 0 || d->weapon >= DG_MIN(db->weapon_count, STATS_WEAPON_COUNT)) return -1;
    return d->weapon;
}

static void wire_reset(void) { memset(GS.wire_to_slot, WIRE_UNMAPPED, sizeof(GS.wire_to_slot)); }

// The local stats' slot of an item database weapon (registered on first use).
static int slot_of_wire(int wire) {
    const ItemDB *db = items_db();
    if (wire < 0 || wire >= STATS_WEAPON_COUNT || !db || wire >= db->weapon_count) return -1;
    if (GS.wire_to_slot[wire] == WIRE_UNMAPPED) GS.wire_to_slot[wire] = (i8)stats_weapon_slot(&stats_get()->life, db->weapons[wire].id);
    return GS.wire_to_slot[wire];
}

const char *game_stats_weapon_name(const PlayerStats *ps, int stats_slot) {
    if (stats_slot < 0 || stats_slot >= ps->weapon_count) return "-";
    const char *id = ps->weapon_ids[stats_slot];
    const ItemDB *db = items_db();
    if (db)
        for (int i = 1; i < db->item_count; i++)
            if (db->items[i].kind == ITEM_WEAPON && db->items[i].weapon >= 0 && db->items[i].weapon < db->weapon_count && !strcmp(db->weapons[db->items[i].weapon].id, id))
                return db->items[i].name;
    return id;
}

const char *game_stats_map_name(const char *map_id) {
    const MapList *ml = lobby_maps();
    if (ml)
        for (int i = 0; i < ml->count; i++)
            if (!strcmp(ml->maps[i].id, map_id) || !strcmp(ml->maps[i].level, map_id)) return ml->maps[i].name;
    return map_id && map_id[0] ? map_id : "-";
}

// ------------------------------------------------------------------ persistence

static void profile_id(char *out, int size) {
    u64 id = lobby_steam_running() ? lobby_local_steam_id() : 0;
    if (id) snprintf(out, (size_t)size, "%llu", (unsigned long long)id);
    else dg_strcpy(out, (size_t)size, "local");
}

static void stats_path(const char *profile, char *out, int size) { snprintf(out, (size_t)size, "%ssaves/stats_%s.json", platform_pref_dir(), profile); }

// Writes text to path through path.tmp and a rename (a crash never leaves half a file).
static bool write_atomic(const char *path, const char *text) {
    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!platform_write_file(tmp, text, strlen(text))) return false;
    if (SDL_RenamePath(tmp, path)) return true;
    SDL_RemovePath(path);   // platforms that refuse to replace
    return SDL_RenamePath(tmp, path);
}

static void save_job(void *user) {
    SaveJob *j = (SaveJob *)user;
    char *text = stats_to_json(&j->life, &j->cur, false);
    if (!text) return;
    j->crc = stats_crc32(text, strlen(text));
    if (j->crc == j->last_crc) j->skipped = j->ok = true;   // nothing changed since the last write
    else j->ok = write_atomic(j->path, text);
    free(text);
}

static void save_async(void);

static void save_done(void *user) {
    SaveJob *j = (SaveJob *)user;
    if (j->ok) GS.last_crc = j->crc;
    else log_warn("stats: cannot write %s", j->path);
    free(j);
    GS.save_busy = false;
    if (GS.save_again) { GS.save_again = false; save_async(); }
}

// Autosave on a job: the stats are copied now, serialised and written off the main thread.
static void save_async(void) {
    if (!GS.loaded || !config_enabled()) return;
    if (GS.save_busy) { GS.save_again = true; return; }
    SaveJob *j = (SaveJob *)malloc(sizeof(SaveJob));
    if (!j) return;
    memset(j, 0, sizeof(*j));
    stats_path(GS.profile, j->path, sizeof(j->path));
    j->life = stats_get()->life;
    j->cur = stats_get()->cur;
    j->last_crc = GS.last_crc;
    char dir[512];
    snprintf(dir, sizeof(dir), "%ssaves", platform_pref_dir());
    SDL_CreateDirectory(dir);
    GS.save_busy = true;
    jobs_submit(save_job, save_done, j, &GS.save_jobs);
}

// The pending async save (if any) finishes first; its completion still runs from the pump.
static void save_sync(void) {
    jobs_wait(&GS.save_jobs);
    if (!GS.loaded || !config_enabled()) return;
    char path[512], dir[512];
    stats_path(GS.profile, path, sizeof(path));
    snprintf(dir, sizeof(dir), "%ssaves", platform_pref_dir());
    SDL_CreateDirectory(dir);
    char *text = stats_to_json(&stats_get()->life, &stats_get()->cur, false);
    if (!text) return;
    if (!write_atomic(path, text)) log_warn("stats: cannot write %s", path);
    else GS.last_crc = stats_crc32(text, strlen(text));
    free(text);
}

#if defined(DEADGROUND_STEAM) && defined(DG_HAVE_STEAM)
// Steam Stats mirror (the app's stats must be configured in Steamworks with these API names).
static void steam_mirror(void) {
    if (!lobby_steam_running()) return;
    const PlayerStats *ps = &stats_get()->life;
    dgs_stat_set_int("kills", (int)DG_MIN(ps->kills, 0x7FFFFFFFu));
    dgs_stat_set_int("deaths", (int)DG_MIN(ps->deaths, 0x7FFFFFFFu));
    dgs_stat_set_int("headshots", (int)DG_MIN(ps->headshots, 0x7FFFFFFFu));
    dgs_stat_set_int("matches_won", (int)DG_MIN(ps->matches_won, 0x7FFFFFFFu));
    dgs_stat_set_int("highest_wave", (int)DG_MIN(ps->highest_wave, 0x7FFFFFFFu));
    dgs_stats_store();
}
#else
static void steam_mirror(void) {}
#endif

static void load_profile(const char *profile) {
    StatsState *st = stats_get();
    bool in_match = st->cur.active;
    char map[STATS_MAP_ID];
    dg_strcpy(map, sizeof(map), st->cur.rec.map);
    memset(&st->life, 0, sizeof(st->life));
    memset(&st->cur, 0, sizeof(st->cur));
    stats_init_profile(&st->life, profile);
    wire_reset();
    dg_strcpy(GS.profile, sizeof(GS.profile), profile);
    GS.loaded = true;
    GS.last_crc = 0;
    if (!config_enabled()) { if (in_match) stats_on_match_start(map); return; }   // --test: in memory only
    char path[512];
    stats_path(profile, path, sizeof(path));
    char *text = platform_read_file(path, NULL);
    bool legacy = false;
    if (!text) {
        // No stats yet: carry over the old profile.json stat stubs (version 0).
        char ppath[512];
        snprintf(ppath, sizeof(ppath), "%ssaves/profile.json", platform_pref_dir());
        text = platform_read_file(ppath, NULL);
        legacy = text != NULL;
    }
    if (text) {
        char err[128] = "";
        MatchStats cur;
        if (!stats_from_json(&st->life, &cur, text, err, sizeof(err))) {
            log_warn("stats: %s: %s (starting fresh)", path, err);
            stats_init_profile(&st->life, profile);
        } else {
            if (err[0]) log_warn("stats: %s: %s", path, err);
            dg_strcpy(st->life.profile, sizeof(st->life.profile), profile);
            // A match still in progress in the file (the game did not shut down): abandoned.
            if (cur.active && !legacy) {
                st->cur = cur;
                stats_on_match_end(MATCH_ABANDON);
            }
            log_info("stats: loaded %s%s (%u matches, %u kills)", legacy ? "the profile's stat stubs from " : "", legacy ? "profile.json" : path, st->life.matches_played, st->life.kills);
        }
        free(text);
    }
    steam_mirror();
    if (in_match) stats_on_match_start(map);
}

static void ensure_profile(void) {
    char id[40];
    profile_id(id, sizeof(id));
    if (GS.loaded && !strcmp(id, GS.profile)) return;
    if (GS.loaded) save_sync();   // the previous profile's
    load_profile(id);
}

bool game_stats_loaded(void) { return GS.loaded; }

// ------------------------------------------------------------------ host: combat

static bool sv_on(const World *w) { return GS.inited && w && w->authority; }
static bool slot_ok(const World *w, int s) { return s >= 0 && s < MAX_PLAYERS && w->players[s].active; }

static void flush_slot(int slot) {
    StatsDelta *d = &GS.pending[slot];
    if (stats_delta_empty(d)) return;
    u8 buf[2 + 4 + STATS_WEAPON_COUNT * 10 + 9 + STATS_DELTA_EVENTS * 4 + 16];
    buf[0] = MSG_STATS;
    buf[1] = STN_DELTA;
    int n = stats_delta_write(d, buf + 2, (int)sizeof(buf) - 2);
    if (n > 0) net_send_to_slot(slot, buf, (u32)(n + 2));
    memset(d, 0, sizeof(*d));
}

static void flush_all(void) {
    for (int s = 0; s < MAX_PLAYERS; s++) flush_slot(s);
}

static StatsDelta *pending_of(const World *w, int slot) {
    if (w->players[slot].local) return NULL;
    StatsDelta *d = &GS.pending[slot];
    if (stats_delta_full(d)) flush_slot(slot);
    return d;
}

void stats_sv_shots(World *w, int slot, u16 item, int count) {
    if (!sv_on(w) || !slot_ok(w, slot) || count <= 0) return;
    int wire = wire_of_item(item);
    StatsDelta *d = pending_of(w, slot);
    if (d) stats_delta_shot(d, wire, (u32)count);
    else stats_on_shots_fired(slot_of_wire(wire), (u32)count);
}

void stats_sv_damage(World *w, int attacker, int victim, u32 target, u16 item, float dealt, bool headshot, bool killed, bool kill_counts) {
    if (!sv_on(w)) return;
    bool own = attacker >= 0 && attacker == victim;
    int wire = wire_of_item(item);
    if (slot_ok(w, attacker) && !own) {
        StatsDelta *d = pending_of(w, attacker);
        if (dealt > 0) {
            if (d) stats_delta_hit(d, wire, dealt, headshot);
            else stats_on_hit(slot_of_wire(wire), dealt, headshot);
        }
        if (killed && kill_counts) {
            vec3 a = w->players[attacker].sim.move.pos, b = ent_valid(w, target) ? w->xf[ENT_INDEX(target)].pos : a;
            float dist = v3_len(v3_sub(a, b));
            d = pending_of(w, attacker);
            if (d) stats_delta_kill(d, wire, dist, headshot);
            else stats_on_kill(slot_of_wire(wire), dist, headshot);
        }
    }
    if (slot_ok(w, victim)) {
        StatsDelta *d = pending_of(w, victim);
        if (d) stats_delta_taken(d, dealt);
        else stats_on_damage_taken(dealt);
        if (killed) {
            d = pending_of(w, victim);
            if (d) stats_delta_event(d, SDK_DEATH);
            else stats_on_death();
        }
    }
}

void stats_sv_down(World *w, int slot) {
    if (!sv_on(w) || !slot_ok(w, slot)) return;
    StatsDelta *d = pending_of(w, slot);
    if (d) stats_delta_event(d, SDK_DOWN);
    else stats_on_down();
}

void stats_sv_revive(World *w, int reviver, int revived) {
    if (!sv_on(w) || !slot_ok(w, reviver) || reviver == revived) return;
    StatsDelta *d = pending_of(w, reviver);
    if (d) stats_delta_event(d, SDK_REVIVE);
    else stats_on_revive_given();
}

void stats_cl_jump(void) { stats_on_jump(); }
void stats_cl_step(void) { stats_on_step(); }

// ------------------------------------------------------------------ match lifecycle

void game_stats_level_loaded(const char *map) {
    if (!GS.inited) return;
    ensure_profile();
    if (stats_get()->cur.active) game_stats_match_end(MATCH_ABANDON);
    memset(GS.pending, 0, sizeof(GS.pending));
    memset(GS.pub_ok, 0, sizeof(GS.pub_ok));
    GS.public_sent = false;
    GS.flush_t = GS.save_t = GS.public_t = 0;
    stats_on_match_start(map);
}

void game_stats_level_leaving(void) {
    if (!GS.inited) return;
    if (net_role() == NET_HOST) { flush_all(); net_flush(); }
    if (stats_get()->cur.active) game_stats_match_end(MATCH_ABANDON);
}

void game_stats_match_end(MatchResult result) {
    if (!GS.inited || !stats_get()->cur.active) return;
    if (net_role() == NET_HOST) flush_all();
    stats_on_match_end(result);
    steam_mirror();
    save_async();
}

void game_stats_wave_complete(int wave) {
    if (!GS.inited) return;
    if (net_role() == NET_HOST) flush_all();   // the deltas' kills belong to the wave that just ended
    stats_on_wave_complete(wave);
}

// ------------------------------------------------------------------ public stats

static void send_public(void) {
    Player *lp = game_local_player();
    stats_public_from(&GS.local_pub, &stats_get()->life, lp ? lp->name : lobby_local_name());
    u8 buf[2 + sizeof(StatsPublic)];
    buf[0] = MSG_STATS;
    buf[1] = STN_PUBLIC;
    int n = stats_public_write(&GS.local_pub, buf + 2, (int)sizeof(buf) - 2);
    if (n > 0) net_send_reliable(0, buf, (u32)(n + 2));
}

static void host_send_public(int to_slot, int about) {
    World *w = game_world();
    u8 buf[3 + sizeof(StatsPublic)];
    buf[0] = MSG_STATS;
    buf[2] = (u8)about;
    const StatsPublic *p = NULL;
    if (w && slot_ok(w, about) && w->players[about].local) {
        stats_public_from(&GS.local_pub, &stats_get()->life, w->players[about].name);
        p = &GS.local_pub;
    } else if (about >= 0 && about < MAX_PLAYERS && GS.pub_ok[about]) p = &GS.pub[about];
    if (!p) {
        buf[1] = STN_NONE;
        net_send_to_slot(to_slot, buf, 3);
        return;
    }
    buf[1] = STN_PUBLIC;
    int n = stats_public_write(p, buf + 3, (int)sizeof(buf) - 3);
    if (n > 0) net_send_to_slot(to_slot, buf, (u32)(n + 3));
}

void game_stats_net_receive(int from_slot, const u8 *data, int size) {
    if (!GS.inited || size < 1) return;
    u8 op = data[0];
    data++;
    size--;
    if (from_slot >= 0) {
        // Host: a client's public stats / a request for someone's.
        if (op == STN_PUBLIC && from_slot < MAX_PLAYERS) {
            StatsPublic p;
            if (!stats_public_read(&p, data, size)) return;
            World *w = game_world();
            if (w && slot_ok(w, from_slot)) dg_strcpy(p.name, sizeof(p.name), w->players[from_slot].name);   // the name the host knows
            GS.pub[from_slot] = p;
            GS.pub_ok[from_slot] = true;
            GS.pub_time[from_slot] = platform_time();
        } else if (op == STN_REQUEST && size >= 1 && data[0] < MAX_PLAYERS) host_send_public(from_slot, data[0]);
        return;
    }
    // Client.
    if (op == STN_DELTA) {
        StatsDelta d;
        if (!stats_delta_read(&d, data, size)) return;
        for (int i = 0; i < STATS_WEAPON_COUNT; i++) if (d.wmask & (1u << i)) slot_of_wire(i);
        stats_apply_delta(&d, GS.wire_to_slot);
        GS.public_t = PUBLIC_SEC;   // the totals changed: tell the host soon
    } else if ((op == STN_PUBLIC || op == STN_NONE) && size >= 1 && data[0] < MAX_PLAYERS) {
        int s = data[0];
        GS.pub_time[s] = platform_time();
        if (op == STN_NONE) { GS.pub_ok[s] = false; return; }
        StatsPublic p;
        if (!stats_public_read(&p, data + 1, size - 1)) return;
        GS.pub[s] = p;
        GS.pub_ok[s] = true;
    }
}

const StatsPublic *game_stats_public(int slot) {
    World *w = game_world();
    if (!w || slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) return NULL;
    if (w->players[slot].local) {
        stats_public_from(&GS.local_pub, &stats_get()->life, w->players[slot].name);
        return &GS.local_pub;
    }
    double now = platform_time();
    if (net_role() == NET_CLIENT && now - GS.pub_req[slot] > 2.0 && (!GS.pub_ok[slot] || now - GS.pub_time[slot] > PUBLIC_FRESH)) {
        GS.pub_req[slot] = now;
        u8 buf[3] = { MSG_STATS, STN_REQUEST, (u8)slot };
        net_send_reliable(0, buf, 3);
    }
    return GS.pub_ok[slot] ? &GS.pub[slot] : NULL;
}

int game_stats_players(StatsPlayerRow *out, int max) {
    World *w = game_world();
    int n = 0;
    if (!w) return 0;
    for (int pass = 0; pass < 2; pass++)
        for (int s = 0; s < MAX_PLAYERS && n < max; s++) {
            const Player *p = &w->players[s];
            if (!p->active || (pass == 0) != p->local) continue;
            out[n].slot = s;
            out[n].local = p->local;
            dg_strcpy(out[n].name, sizeof(out[n].name), p->name);
            n++;
        }
    return n;
}

// ------------------------------------------------------------------ frame

static StatsTickIn tick_input(void) {
    StatsTickIn in;
    memset(&in, 0, sizeof(in));
    in.weapon = -1;
    World *w = game_world();
    Player *lp = game_local_player();
    if (!w || !lp || !lp->active || mainmenu_active() || (G && G->paused)) return in;
    in.in_match = true;
    const PlayerMove *m = &lp->sim.move;
    in.alive = m->mode != MOVE_DEAD;
    in.grounded = m->mode == MOVE_WALK && m->on_ground;
    in.x = m->pos.x;
    in.z = m->pos.z;
    const InvSlot *eq = weapon_equipped(w, G->local_slot);
    in.weapon = eq ? slot_of_wire(wire_of_item(eq->item)) : -1;
    return in;
}

void game_stats_frame(float dt) {
    if (!GS.inited) return;
    ensure_profile();
    StatsTickIn in = tick_input();
    stats_tick(dt, &in);
    bool in_match = stats_get()->cur.active && in.in_match;
    if (net_role() == NET_HOST) {
        GS.flush_t += dt;
        if (GS.flush_t >= FLUSH_SEC) { GS.flush_t = 0; flush_all(); }
    }
    if (net_role() == NET_CLIENT && net_client_welcomed() && game_world()) {
        GS.public_t += dt;
        if (!GS.public_sent || GS.public_t >= PUBLIC_SEC) { GS.public_sent = true; GS.public_t = 0; send_public(); }
    }
    if (in_match) {
        GS.save_t += dt;
        if (GS.save_t >= AUTOSAVE_SEC) { GS.save_t = 0; save_async(); }
    }
}

// ------------------------------------------------------------------ UI helpers

bool game_stats_imperial(void) { return GS.units && GS.units->i == 1; }
void game_stats_set_imperial(bool on) { if (GS.units) cvar_set_int(GS.units, on ? 1 : 0); }

bool game_stats_export(char *path_out, int size) {
    char dirbuf[512], path[768];
    // --test runs never write into the user's folders: the build tree's (or the pref dir's) exports/.
    const char *dir = config_enabled() ? NULL : platform_writable_dir("exports/", dirbuf, sizeof(dirbuf));
    if (!dir) {
        snprintf(dirbuf, sizeof(dirbuf), "%ssaves", platform_pref_dir());
        SDL_CreateDirectory(dirbuf);
        dg_strcpy(dirbuf + strlen(dirbuf), sizeof(dirbuf) - strlen(dirbuf), "/");
        dir = dirbuf;
    }
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    snprintf(path, sizeof(path), "%sstats_export_%s_%04d%02d%02d_%02d%02d%02d.json", dir, GS.profile[0] ? GS.profile : "local", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    char *text = stats_to_json(&stats_get()->life, &stats_get()->cur, true);
    bool ok = text && platform_write_file(path, text, strlen(text));
    free(text);
    if (ok) log_info("stats: exported %s", path);
    else log_warn("stats: cannot write %s", path);
    if (path_out) dg_strcpy(path_out, (size_t)size, path);
    return ok;
}

void game_stats_reset_all(void) {
    StatsState *st = stats_get();
    bool in_match = st->cur.active;
    char map[STATS_MAP_ID];
    dg_strcpy(map, sizeof(map), st->cur.rec.map);
    stats_reset();
    dg_strcpy(st->life.profile, sizeof(st->life.profile), GS.profile[0] ? GS.profile : "local");
    wire_reset();
    if (in_match) stats_on_match_start(map);
    st->life.revision += 1000;   // every cached chart rebuilds
    save_async();
    steam_mirror();
    log_info("stats: reset");
}

// ------------------------------------------------------------------ commands

static void cmd_stats_fake(int argc, const char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 100;
    bool keep = argc > 2 && !strcmp(argv[2], "keep");
    u32 seed = argc > 3 ? (u32)strtoul(argv[3], NULL, 10) : (u32)time(NULL);
    n = DG_CLAMP(n, 1, 5000);
    ensure_profile();
    StatsState *st = stats_get();
    if (!keep) {
        bool in_match = st->cur.active;
        char map[STATS_MAP_ID];
        dg_strcpy(map, sizeof(map), st->cur.rec.map);
        stats_reset();
        dg_strcpy(st->life.profile, sizeof(st->life.profile), GS.profile);
        wire_reset();
        if (in_match) stats_on_match_start(map);
    }
    const char *maps[16];
    int nm = 0;
    const MapList *ml = lobby_maps();
    for (int i = 0; ml && i < ml->count && nm < 16; i++) maps[nm++] = ml->maps[i].id;
    if (!nm) { maps[nm++] = "devroom"; maps[nm++] = "vegetation"; maps[nm++] = "wateryard"; }
    const char *weapons[STATS_WEAPON_COUNT];
    int nw = 0;
    const ItemDB *db = items_db();
    for (int i = 0; db && i < db->weapon_count && nw < STATS_WEAPON_COUNT; i++) weapons[nw++] = db->weapons[i].id;
    if (!nw) { weapons[nw++] = "rifle"; weapons[nw++] = "pistol"; weapons[nw++] = "shotgun"; }
    stats_fake(&st->life, n, seed, (i64)time(NULL), maps, nm, weapons, nw, st->utc_offset_sec);
    save_async();
    log_info("stats_fake: %d matches (%u lifetime, %u kills, K/D %.2f, accuracy %.1f %%)", n, st->life.matches_played, st->life.kills, stats_kd(&st->life), stats_overall_accuracy(&st->life) * 100.0f);
}

static void cmd_stats_dump(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    const PlayerStats *ps = &stats_get()->life;
    const MatchStats *c = &stats_get()->cur;
    log_info("stats: profile %s v%u  kills %u deaths %u K/D %.2f headshots %u (%.1f %%) accuracy %.1f %% (%u / %u) damage %.0f / %.0f taken",
             ps->profile, ps->version, ps->kills, ps->deaths, stats_kd(ps), ps->headshots, stats_headshot_pct(ps) * 100.0f, stats_overall_accuracy(ps) * 100.0f,
             stats_total_hits(ps), stats_total_shots(ps), ps->damage_dealt, ps->damage_taken);
    log_info("stats: matches %u won %u (%.0f %%) highest wave %u best streak %u revives %u downed %u time %.0f s distance %.1f m steps %llu jumps %u",
             ps->matches_played, ps->matches_won, stats_win_rate(ps) * 100.0f, ps->highest_wave, ps->best_killstreak, ps->revives_given, ps->times_downed,
             ps->time_played_sec, ps->total_distance_m, (unsigned long long)ps->steps, ps->jumps);
    log_info("stats: longest kill %.1f m (%s, %s)  history %d matches, %d days", ps->longest_kill.dist_m,
             ps->longest_kill.weapon >= 0 ? ps->weapon_ids[ps->longest_kill.weapon] : "-", ps->longest_kill.map, ps->match_count, ps->day_count);
    for (int i = 0; i < ps->weapon_count; i++) {
        const WeaponStats *w = &ps->weapons[i];
        log_info("stats:   %-10s kills %u shots %u hits %u (%.1f %%) headshots %u damage %.0f equipped %.0f s longest %.1f m", ps->weapon_ids[i], w->kills, w->shots_fired,
                 w->shots_hit, stats_weapon_accuracy(w) * 100.0f, w->headshots, w->damage_dealt, w->time_equipped_sec, w->longest_kill_m);
    }
    if (c->active)
        log_info("stats: match '%s' %.0f s  kills %u deaths %u downs %u revives %u shots %u hits %u streak %d best %u distance %.1f m wave %d (%s)", c->rec.map,
                 c->rec.duration_sec, c->rec.kills, c->rec.deaths, c->rec.downs, c->rec.revives, c->rec.shots_fired, c->rec.shots_hit, c->streak, c->rec.best_streak,
                 c->rec.distance_m, stats_current_wave() + 1, c->waves ? "waves" : "segments");
    else log_info("stats: no match in progress");
}

static void cmd_stats_reset(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    game_stats_reset_all();
}

static void cmd_stats_export(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    game_stats_export(NULL, 0);
}

static void cmd_stats_save(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!config_enabled()) { log_info("stats_save: --test runs keep the stats in memory"); return; }
    save_sync();
    char path[512];
    stats_path(GS.profile, path, sizeof(path));
    log_info("stats: saved %s", path);
}

static void cmd_stats_match_end(int argc, const char **argv) {
    const char *r = argc > 1 ? argv[1] : "abandon";
    MatchResult res = !strcmp(r, "win") ? MATCH_WIN : !strcmp(r, "loss") ? MATCH_LOSS : MATCH_ABANDON;
    char map[STATS_MAP_ID];
    dg_strcpy(map, sizeof(map), G && G->world ? G->map : "");
    game_stats_match_end(res);
    if (G && G->world) stats_on_match_start(map);   // the world keeps running: a new match on it
    log_info("stats_match_end: %s (%u matches)", r, stats_get()->life.matches_played);
}

static void cmd_stats_wave(int argc, const char **argv) {
    int wave = argc > 1 ? atoi(argv[1]) : stats_current_wave() + 1;
    game_stats_wave_complete(wave);
    log_info("stats_wave: wave %d survived", wave);
}

static void cmd_stats_public(int argc, const char **argv) {
    int slot = argc > 1 ? atoi(argv[1]) : (G ? G->local_slot : 0);
    const StatsPublic *p = game_stats_public(slot);
    if (!p) { log_info("stats_public %d: not received yet (requested)", slot); return; }
    log_info("stats_public %d: %s kills %u deaths %u headshots %u shots %u hits %u revives %u downed %u matches %u won %u wave %u streak %u time %.0f s distance %.0f m longest %.1f m favorite %s",
             slot, p->name, p->kills, p->deaths, p->headshots, p->shots_fired, p->shots_hit, p->revives_given, p->times_downed, p->matches_played, p->matches_won, p->highest_wave,
             p->best_killstreak, p->time_played_sec, p->distance_m, p->longest_kill_m, p->favorite[0] ? p->favorite : "-");
}

static void cmd_stats_flush(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (net_role() != NET_HOST) { log_info("stats_flush: host only"); return; }
    flush_all();
    log_info("stats_flush: combat deltas sent");
}

void game_stats_init(void) {
    if (GS.inited) return;
    wire_reset();
    GS.units = cvar_reg_int("ui_units", 0, 0, 1, CVAR_ARCHIVE | CVAR_EDITABLE, "units of distances in the Stats tab and the HUD: 0 metric (m / km), 1 imperial (ft / mi)");
    cmd_register("stats_fake", cmd_stats_fake, "stats_fake [N] [keep] [seed]: N plausible fake matches over the last year (replaces the stats unless keep; Stats tab development)");
    cmd_register("stats_dump", cmd_stats_dump, "print the lifetime stats, per weapon, and the match in progress");
    cmd_register("stats_reset", cmd_stats_reset, "reset every stat of this profile (lifetime + history)");
    cmd_register("stats_export", cmd_stats_export, "write the stats as readable JSON (saves/stats_export_*.json)");
    cmd_register("stats_save", cmd_stats_save, "save the stats now (saves/stats_<profile>.json)");
    cmd_register("stats_match_end", cmd_stats_match_end, "stats_match_end [win|loss|abandon]: record the match in progress with a result (a new one starts)");
    cmd_register("stats_wave", cmd_stats_wave, "stats_wave [n]: report wave n survived (wave systems call game_stats_wave_complete)");
    cmd_register("stats_public", cmd_stats_public, "stats_public [slot]: the public stats this machine has for a player (a client asks the host for them)");
    cmd_register("stats_flush", cmd_stats_flush, "host: send the players' combat stat deltas now (else every 30 s)");
    // The local calendar for the day roll-up / heatmap: the offset from UTC now.
    time_t now = time(NULL);
    struct tm g, l;
#ifdef _WIN32
    gmtime_s(&g, &now);
    localtime_s(&l, &now);
#else
    gmtime_r(&now, &g);
    localtime_r(&now, &l);
#endif
    g.tm_isdst = l.tm_isdst;
    stats_get()->utc_offset_sec = (i32)difftime(mktime(&l), mktime(&g));
    GS.inited = true;
}

void game_stats_shutdown(void) {
    if (!GS.inited) return;
    game_stats_level_leaving();   // the clients' last deltas, the match recorded
    save_sync();
}
