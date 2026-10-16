#include "loot.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

bool loot_parse(LootTable *t, const char *json, char *err, size_t err_size) {
    memset(t, 0, sizeof(*t));
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) { snprintf(err, err_size, "loot: JSON parse error"); return false; }
    const cJSON *tiers = cJSON_GetObjectItemCaseSensitive(root, "tiers"), *tj;
    cJSON_ArrayForEach(tj, tiers) {
        if (t->n >= LOOT_MAX_TIERS) break;
        LootTier *tr = &t->tiers[t->n];
        memset(tr, 0, sizeof(*tr));
        dg_strcpy(tr->name, sizeof(tr->name), tj->string ? tj->string : "?");
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(tj, "rolls"), *e = cJSON_GetObjectItemCaseSensitive(tj, "empty");
        tr->rolls_min = cJSON_IsArray(r) ? cJSON_GetArrayItem(r, 0)->valueint : 1;
        tr->rolls_max = cJSON_IsArray(r) && cJSON_GetArraySize(r) > 1 ? cJSON_GetArrayItem(r, 1)->valueint : tr->rolls_min;
        tr->empty = cJSON_IsNumber(e) ? (float)e->valuedouble : 0.0f;
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(tj, "items")) {
            // ["item", weight, min, max]
            if (tr->n >= LOOT_MAX_ENTRIES || !cJSON_IsArray(it) || !cJSON_IsString(cJSON_GetArrayItem(it, 0))) continue;
            LootEntry *le = &tr->e[tr->n++];
            dg_strcpy(le->item, sizeof(le->item), cJSON_GetArrayItem(it, 0)->valuestring);
            int sz = cJSON_GetArraySize(it);
            le->weight = sz > 1 ? (float)cJSON_GetArrayItem(it, 1)->valuedouble : 1.0f;
            le->min = sz > 2 ? cJSON_GetArrayItem(it, 2)->valueint : 1;
            le->max = sz > 3 ? cJSON_GetArrayItem(it, 3)->valueint : le->min;
            if (le->max < le->min) le->max = le->min;
            tr->total += le->weight;
        }
        if (tr->rolls_max < tr->rolls_min) tr->rolls_max = tr->rolls_min;
        t->n++;
    }
    cJSON_Delete(root);
    if (!t->n) { snprintf(err, err_size, "loot: no tiers"); return false; }
    return true;
}

const LootTier *loot_tier(const LootTable *t, const char *name) {
    for (int i = 0; i < t->n; i++) if (name && !strcmp(t->tiers[i].name, name)) return &t->tiers[i];
    return NULL;
}

int loot_roll(const LootTable *t, const char *tier, u32 seed, LootItem *out, int max) {
    const LootTier *tr = loot_tier(t, tier);
    if (!tr) tr = t->n ? &t->tiers[0] : NULL;
    if (!tr || tr->total <= 0 || max <= 0) return 0;
    Rng r = rng_make((u64)seed * 0x9E3779B97F4A7C15ull + 12345u);
    if (rng_float(&r) < tr->empty) return 0;
    int rolls = tr->rolls_min + (int)(rng_next(&r) % (u32)(tr->rolls_max - tr->rolls_min + 1));
    int n = 0;
    for (int k = 0; k < rolls; k++) {
        float pick = rng_float(&r) * tr->total;
        const LootEntry *le = &tr->e[tr->n - 1];
        for (int i = 0; i < tr->n; i++) {
            if (pick < tr->e[i].weight) { le = &tr->e[i]; break; }
            pick -= tr->e[i].weight;
        }
        int count = le->min + (int)(rng_next(&r) % (u32)(le->max - le->min + 1));
        int j = 0;
        while (j < n && strcmp(out[j].item, le->item)) j++;
        if (j < n) { out[j].count += count; continue; }
        if (n >= max) break;
        out[n].item = le->item;
        out[n].count = count;
        n++;
    }
    return n;
}
