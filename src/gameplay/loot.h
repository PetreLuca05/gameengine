// Loot tables (data/loot.json, docs/ZOMBIE_MAP.md "Loot"): named tiers ("household", "medical",
// "police", "industrial", "rare", ...) of weighted item entries with count ranges; a container rolls a
// tier with a seed (deterministic: the same map gives the same loot). Pure: item ids are strings, the
// caller resolves them against its ItemDB.
#ifndef DG_LOOT_H
#define DG_LOOT_H

#include "core/base.h"

#define LOOT_MAX_TIERS 32
#define LOOT_MAX_ENTRIES 32

typedef struct LootEntry { char item[24]; int min, max; float weight; } LootEntry;
typedef struct LootTier {
    char name[16];
    int rolls_min, rolls_max;    // item rolls per container
    float empty;                 // chance that a container of this tier is empty
    LootEntry e[LOOT_MAX_ENTRIES];
    int n;
    float total;
} LootTier;
typedef struct LootTable { LootTier tiers[LOOT_MAX_TIERS]; int n; } LootTable;
typedef struct LootItem { const char *item; int count; } LootItem;

bool loot_parse(LootTable *t, const char *json, char *err, size_t err_size);
const LootTier *loot_tier(const LootTable *t, const char *name);
// Rolls a container of `tier` (falls back to the first tier when unknown); duplicate items merge.
int loot_roll(const LootTable *t, const char *tier, u32 seed, LootItem *out, int max);

#endif
