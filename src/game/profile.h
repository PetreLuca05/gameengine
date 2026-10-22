// Player profile (docs/MENU_LOBBY.md "Data & persistence"): <pref dir>/saves/profile.json. Equipped
// cosmetics, unlocks, the last map and lobby settings, the LAN name and stat stubs. Settings stay in
// the config file (cvars). The JSON round trip is pure (tests/test_lobby.c).
#ifndef DG_PROFILE_H
#define DG_PROFILE_H

#include "core/base.h"
#include "gameplay/cosmetics.h"
#include "net/lobby.h"

#define PROFILE_VERSION 1

typedef struct Profile {
    int version;
    char lan_name[32];
    CosmeticLoadout loadout;
    CosmeticUnlocks unlocks;
    char last_map[32];
    LobbySettings lobby;          // last lobby settings (the host's defaults next time)
    // stat stubs: superseded by the player statistics (docs/STATS.md), which import them once
    u32 kills, deaths, matches;
    double playtime_s;
    char last_session[64];        // "Continue": the last session's save id ("" = none)
} Profile;

void profile_default(Profile *p);
// JSON text (malloc'd, caller frees) / parse (missing keys keep the defaults). Pure.
char *profile_to_json(const Profile *p);
bool profile_from_json(Profile *p, const char *json);

const char *profile_path(void);           // <pref dir>/saves/profile.json
bool profile_load(Profile *p);            // false: no file yet (defaults)
bool profile_save(const Profile *p);      // creates saves/; no-op while the config is disabled (--test)

#endif
