// Console registry metadata (docs/UI_WHEELS_HUD.md "Commands tab"): categories for every command
// and cvar (assets/ui/command_categories.json: exact names, else the longest name prefix), the
// destructive commands that ask first, favorites / recent lists and the search match. Pure
// (tests/test_hud.c); tools/tests/lint_commands.py checks every registration against the same file.
#ifndef DG_CMD_REGISTRY_H
#define DG_CMD_REGISTRY_H

#include "base.h"

#define CMDREG_MAX_CATS 24
#define CMDREG_MAX_PREFIXES 24
#define CMDREG_MAX_NAMES 96
#define CMDREG_MAX_LIST 48
#define CMDREG_NAME 48

typedef struct CmdCategory {
    char id[16];
    char label[32];
    char prefixes[CMDREG_MAX_PREFIXES][24];
    int prefix_count;
} CmdCategory;

typedef struct CmdList {
    char items[CMDREG_MAX_LIST][CMDREG_NAME];
    int count;
} CmdList;

typedef struct CmdRegistryCfg {
    CmdCategory cats[CMDREG_MAX_CATS];
    int cat_count;
    char names[CMDREG_MAX_NAMES][CMDREG_NAME];
    int name_cat[CMDREG_MAX_NAMES];
    int name_count;
    CmdList destructive;
    CmdList favorites;     // defaults for a new profile
} CmdRegistryCfg;

bool cmdreg_parse(CmdRegistryCfg *cfg, const char *json, char *err, int err_size);
int cmdreg_category(const CmdRegistryCfg *cfg, const char *name);   // index, -1 = none
int cmdreg_category_by_id(const CmdRegistryCfg *cfg, const char *id);
bool cmdreg_destructive(const CmdRegistryCfg *cfg, const char *name);
// Search: every space-separated word of query appears (case-insensitive) in the name or description.
bool cmdreg_match(const char *query, const char *name, const char *desc);

// Lists (favorites, recent): space-separated in a cvar.
void cmdlist_parse(CmdList *l, const char *text);
void cmdlist_format(const CmdList *l, char *out, int size);
bool cmdlist_has(const CmdList *l, const char *name);
bool cmdlist_toggle(CmdList *l, const char *name);   // returns true if now present
void cmdlist_touch(CmdList *l, const char *name, int max);   // most recent first, max entries

#endif
