// The consistency set (docs/SETTINGS_AUDIT.md): the values of the LOCKED cvars every player shares,
// from assets/render/consistency.json. Applied at startup (after every registration, before the
// config); its hash is part of the build identity (lobby_version_hash) and of the join handshake
// (MSG_HELLO), so clients with a different set can't join. Pure (cvar system + cJSON):
// tests/test_settings.c.
#ifndef DG_CONSISTENCY_H
#define DG_CONSISTENCY_H

#include "base.h"

#define CONSISTENCY_PATH "assets/render/consistency.json"

// Parse the file's text ({"values": {"<cvar>": {"value": .., "why": ".."}}}); replaces the set.
bool consistency_load_text(const char *json, char *err, int err_size);
bool consistency_load(const char *path);
// Every entry onto its cvar (also its default, so `reset` returns it). Errors: an entry naming no
// cvar or a cvar that isn't CVAR_LOCKED. Returns the number of errors.
int consistency_apply(char *err, int err_size);
int consistency_count(void);
const char *consistency_name(int i);
const char *consistency_value(int i);    // normalized by the cvar once applied
const char *consistency_why(int i);
const char *consistency_find(const char *cvar);   // the locked value, NULL = not in the set
// FNV-1a over the sorted "name=value" pairs of the file (the build identity).
u32 consistency_hash(void);
// The same over the current values of the entries that are not host rules (CVAR_RULE): what this
// machine renders with. The join handshake compares it with the host's.
u32 consistency_hash_effective(void);
// Self-check: non-rule entries whose cvar no longer holds the locked value (a dev build's console
// edits). Writes "name=value (locked v)" pairs into report. 0 = consistent.
int consistency_check(char *report, int size);
// Join handshake (MSG_HELLO): a client whose effective hash differs from the host's (or that sent
// none: missing) is rejected; why gets the MSG_REJECT reason.
bool consistency_join_ok(u32 host_hash, u32 client_hash, bool missing, char *why, int why_size);
// Startup: load + apply + self-check + log (game init).
void consistency_init(void);

#endif
