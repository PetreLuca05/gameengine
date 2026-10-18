// Console variables + console commands + command buffer.
// Settings menus, the config file and the console all go through this one system.
#ifndef DG_CVAR_H
#define DG_CVAR_H

#include "base.h"
#include <stdio.h>

typedef enum CvarType { CVAR_BOOL, CVAR_INT, CVAR_FLOAT, CVAR_STRING } CvarType;

enum {
    CVAR_ARCHIVE    = 1 << 0, // saved to config file
    CVAR_CHEAT      = 1 << 1, // only changeable when sv_cheats 1 (host)
    CVAR_SERVER     = 1 << 2, // owned by the host; replicated to clients
    CVAR_READONLY   = 1 << 3,
    CVAR_NOTIFY     = 1 << 4, // print when changed
    // Classification (docs/SETTINGS_AUDIT.md): every registration carries exactly one of the first
    // three (tools/tests/lint_settings.py). Release builds enforce them (cvar_release_locks).
    CVAR_EDITABLE   = 1 << 5, // a player setting (menus, console, config.cfg)
    CVAR_TIER       = 1 << 6, // set by Engine Fidelity / the era only (hidden, never saved)
    CVAR_LOCKED     = 1 << 7, // one value for everyone (assets/render/consistency.json, never saved)
    CVAR_RULE       = 1 << 8, // with CVAR_LOCKED: the host's server rules may set it (replicated)
};
#define CVAR_CLASS_MASK (CVAR_EDITABLE | CVAR_TIER | CVAR_LOCKED)

// Who writes a cvar. A user write (menu, console, config, game code) of a TIER / LOCKED cvar is
// rejected (and logged) while the release locks are on; Engine Fidelity / the era write TIER
// cvars as CVAR_SRC_FIDELITY, the consistency set and the host's replicated values as
// CVAR_SRC_HOST. A LOCKED | RULE cvar takes user writes while this machine is not a client of
// someone else's server (cvar_set_rule_gate): its host sets the rules, and they replicate.
typedef enum CvarSource { CVAR_SRC_USER, CVAR_SRC_FIDELITY, CVAR_SRC_HOST } CvarSource;

#define CVAR_NAME_MAX 48
#define CVAR_STR_MAX 128

typedef struct Cvar Cvar;
typedef void (*CvarChangeFn)(Cvar *cv);

struct Cvar {
    char name[CVAR_NAME_MAX];
    char desc[CVAR_STR_MAX];
    char str[CVAR_STR_MAX];
    char default_str[CVAR_STR_MAX];
    CvarType type;
    u32 flags;
    float f;
    int i;
    float min, max;
    bool has_range;
    u32 modified;          // bumped on every change
    CvarChangeFn on_change;
    // Called on every set before the change is compared / announced: may rewrite the value in place
    // with cvar_store (Engine Fidelity bounds, src/render/fidelity). on_change sees the result.
    CvarChangeFn bound;
};

typedef void (*CmdFn)(int argc, const char **argv);

void cvar_system_init(void);
void cvar_system_shutdown(void);

Cvar *cvar_reg(const char *name, const char *default_value, CvarType type, u32 flags, const char *desc);
Cvar *cvar_reg_bool(const char *name, bool def, u32 flags, const char *desc);
Cvar *cvar_reg_int(const char *name, int def, int min, int max, u32 flags, const char *desc);
Cvar *cvar_reg_float(const char *name, float def, float min, float max, u32 flags, const char *desc);
Cvar *cvar_reg_string(const char *name, const char *def, u32 flags, const char *desc);

// Release locks: on in release builds (DG_DEV_BUILD 0), off in dev builds; --release-locks and
// the tests turn them on. cvar_source_push / pop bracket privileged writes.
bool cvar_dev_build(void);
void cvar_set_release_locks(bool on);
bool cvar_release_locks(void);
CvarSource cvar_source_push(CvarSource src);   // returns the previous source
void cvar_source_pop(CvarSource prev);
void cvar_set_rule_gate(bool (*can_set_rules)(void));
bool cvar_writable(const Cvar *cv, CvarSource src);   // what a write from src would do now
int cvar_rejected_count(void);                         // writes rejected by the locks so far
const char *cvar_class_name(u32 flags);                // "EDITABLE" / "TIER" / "LOCKED" / "LOCKED+RULE" / "?"
bool cvar_saved(const Cvar *cv);                       // written to config.cfg

Cvar *cvar_find(const char *name);
bool cvar_set(const char *name, const char *value);
void cvar_set_cv(Cvar *cv, const char *value);
void cvar_set_float(Cvar *cv, float v);
void cvar_set_int(Cvar *cv, int v);
void cvar_reset(Cvar *cv);
void cvar_store(Cvar *cv, const char *value);   // write + parse only (inside a bound callback)
int cvar_count(void);
Cvar *cvar_at(int index);

void cmd_register(const char *name, CmdFn fn, const char *desc);
bool cmd_exists(const char *name);
// The registry, in registration order (the Commands tab, the lint).
int cmd_count(void);
const char *cmd_name_at(int index);
const char *cmd_desc_at(int index);
const char *cmd_desc(const char *name);   // NULL: no such command

// Command buffer: text is split on ';' and newlines. "wait [n]" pauses n frames.
void cbuf_add(const char *text);
void cbuf_execute(void);         // call once per frame
// Scripts: a command polling a condition puts itself back at the front and waits a frame
// (e.g. lobby_wait); the rest of the script runs once it stops retrying.
void cbuf_retry(const char *line);
void cmd_execute_line(const char *line);

// Tab completion: fills up to max names starting with prefix, returns count.
int cmd_complete(const char *prefix, const char **out, int max);

// Config persistence (archive cvars). Extra writers (e.g. key binds) hook in.
typedef void (*ConfigWriterFn)(FILE *f);
void config_add_writer(ConfigWriterFn fn);
bool config_save(const char *path);          // no-op while disabled (--test)
// Settings persistence: archived cvar / bind changes mark the config dirty; it's written one
// second after the last change and on exit, never in --test runs.
void config_set_enabled(bool enabled);
bool config_enabled(void);
void config_mark_dirty(void);
void config_clear_dirty(void);
bool config_save_if_dirty(const char *path);
bool config_autosave(const char *path);
bool config_exec_file(const char *path);
// The user's config.cfg: like config_exec_file, but entries of TIER / LOCKED cvars (no longer
// saved; older configs have them) are dropped with one log line.
bool config_load_file(const char *path);
int config_load_dropped(void);   // entries the last config_load_file dropped (the file is rewritten)

#endif
