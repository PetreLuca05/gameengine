// Consistency set (docs/SETTINGS_AUDIT.md): the values every player shares. assets/render/
// consistency.json lists the CVAR_LOCKED cvars that decide what a player can see (foliage density,
// LOD / impostor ranges, shadow distance, fog, exposure, lens effects, budgets ...) with one value
// and a reason each. At startup the set is applied (as the host's values, so the locks let it
// through), every entry is checked (it must name a registered CVAR_LOCKED cvar), and its hash goes
// into the build / content hash of the lobby and into MSG_HELLO: a client with another set is
// rejected. CVAR_RULE entries may be changed by the host's server rules for the session (replicated),
// never by a client.
#ifndef DG_CONSISTENCY_H
#define DG_CONSISTENCY_H

#include "base.h"

#define CONSISTENCY_PATH "assets/render/consistency.json"

bool consistency_load_text(const char *json, char *err, int err_size);
bool consistency_load(const char *path);          // logs errors; false: no set (hash 0)
int consistency_count(void);
const char *consistency_name(int i);
const char *consistency_value(int i);
const char *consistency_note(int i);
int consistency_find(const char *cvar);           // -1: not in the set
// FNV-1a over the sorted "name=value;" pairs (independent of the file's order and formatting).
u32 consistency_hash(void);
// The host's join check (MSG_HELLO): false when the client's set differs (reason into why).
bool consistency_join_ok(u32 theirs, char *why, int why_size);
// Apply every value (CVAR_SRC_HOST; the value also becomes the cvar's default). Returns the number
// of bad entries (unknown cvar, not CVAR_LOCKED), each logged.
int consistency_apply(void);
// Self-check: entries whose cvar differs from the set now (a host rule or a dev-build change), into
// report ("name=value (set v)", comma separated). 0 = every value as listed.
int consistency_diff(char *report, int size);

#endif
