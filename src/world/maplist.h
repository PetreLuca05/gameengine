// Map catalogue (docs/MENU_LOBBY.md "Adding a map"): assets/maps/*.json, one file per playable map
// (name, thumbnail, size, biome, recommended players, time-of-day / weather defaults, description,
// the level it loads). The main menu's Maps tab, the lobby map selector and the background scene
// read it. Pure apart from the directory listing (tests/test_lobby.c validates every file).
#ifndef DG_MAPLIST_H
#define DG_MAPLIST_H

#include "core/base.h"

#define MAPLIST_MAX 32

typedef struct MapInfo {
    char id[32];              // file stem: the lobby / network id
    char name[48];
    char level[48];           // what game_host loads: data/levels/<level>.json or a procedural map
    char thumbnail[96];       // png path (assets/maps/thumbs/<id>.png)
    char biome[24];
    char description[256];
    char weather[24];         // default start weather preset ("" = the season's pick)
    int season;               // default season (Season: 0 spring .. 3 winter)
    float hour;               // default start time (0..24)
    float size_m;             // playable extent (m, one side); 0 = unbounded
    int players_min, players_max;   // recommended
    bool dev;                 // developer / test map (filtered out unless "show dev maps")
    char scene[32];           // optional dev scene opened once the level is up ("handpose"; "" = none)
    int order;                // list order
} MapInfo;

typedef struct MapList {
    MapInfo maps[MAPLIST_MAX];
    int count;
} MapList;

// Parses one map file. id: the file stem. false + err when a required field is missing / invalid.
// file_exists (optional): checks the thumbnail path.
typedef bool (*MapFileExists)(const char *path, void *user);
bool maplist_parse(const char *json, const char *id, MapInfo *out, MapFileExists file_exists, void *user, char *err, int err_size);
// Loads every assets/maps/*.json (sorted by order, then name). Invalid files are logged and skipped.
int maplist_load(MapList *ml, const char *dir);
const MapInfo *maplist_find(const MapList *ml, const char *id);
int maplist_index(const MapList *ml, const char *id);
// "2 km x 2 km", "120 m x 120 m", "Unbounded"
void maplist_size_text(const MapInfo *m, char *out, int size);
// Hash of every map's id + level (part of the lobby version hash: same map catalogue on both sides).
u32 maplist_hash(const MapList *ml);

#endif
