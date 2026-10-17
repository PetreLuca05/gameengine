// Console registry metadata (src/core/cmd_registry.h). Pure.
#include "cmd_registry.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void list_from_json(CmdList *l, const cJSON *arr) {
    memset(l, 0, sizeof(*l));
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (cJSON_IsString(e) && l->count < CMDREG_MAX_LIST) dg_strcpy(l->items[l->count++], CMDREG_NAME, e->valuestring);
    }
}

bool cmdreg_parse(CmdRegistryCfg *cfg, const char *json, char *err, int err_size) {
    if (err && err_size) err[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "JSON syntax error"); return false; }
    CmdRegistryCfg c;
    memset(&c, 0, sizeof(c));
    const char *bad = NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "categories")) {
        if (c.cat_count >= CMDREG_MAX_CATS) { bad = "too many categories"; break; }
        CmdCategory *k = &c.cats[c.cat_count];
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (!cJSON_IsString(v)) { bad = "a category has no \"id\""; break; }
        dg_strcpy(k->id, sizeof(k->id), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(e, "label");
        dg_strcpy(k->label, sizeof(k->label), cJSON_IsString(v) ? v->valuestring : k->id);
        const cJSON *p;
        cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(e, "prefixes")) {
            if (cJSON_IsString(p) && k->prefix_count < CMDREG_MAX_PREFIXES) dg_strcpy(k->prefixes[k->prefix_count++], sizeof(k->prefixes[0]), p->valuestring);
        }
        c.cat_count++;
    }
    if (!bad && !c.cat_count) bad = "no categories";
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(root, "names");
    cJSON_ArrayForEach(e, names) {
        if (bad) break;
        if (c.name_count >= CMDREG_MAX_NAMES) { bad = "too many \"names\""; break; }
        int k = cJSON_IsString(e) ? cmdreg_category_by_id(&c, e->valuestring) : -1;
        if (k < 0) { bad = "\"names\" uses an unknown category"; break; }
        dg_strcpy(c.names[c.name_count], CMDREG_NAME, e->string);
        c.name_cat[c.name_count++] = k;
    }
    list_from_json(&c.destructive, cJSON_GetObjectItemCaseSensitive(root, "destructive"));
    list_from_json(&c.favorites, cJSON_GetObjectItemCaseSensitive(root, "favorites"));
    cJSON_Delete(root);
    if (bad) { if (err) snprintf(err, (size_t)err_size, "%s", bad); return false; }
    *cfg = c;
    return true;
}

int cmdreg_category_by_id(const CmdRegistryCfg *cfg, const char *id) {
    for (int i = 0; i < cfg->cat_count; i++) if (!strcmp(cfg->cats[i].id, id)) return i;
    return -1;
}

int cmdreg_category(const CmdRegistryCfg *cfg, const char *name) {
    for (int i = 0; i < cfg->name_count; i++) if (!strcmp(cfg->names[i], name)) return cfg->name_cat[i];
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < cfg->cat_count; i++) {
        for (int k = 0; k < cfg->cats[i].prefix_count; k++) {
            const char *p = cfg->cats[i].prefixes[k];
            size_t n = strlen(p);
            if (n > best_len && !strncmp(name, p, n)) { best = i; best_len = n; }
        }
    }
    return best;
}

bool cmdreg_destructive(const CmdRegistryCfg *cfg, const char *name) { return cmdlist_has(&cfg->destructive, name); }

static bool contains_ci(const char *hay, const char *needle, int n) {
    if (!hay) return false;
    for (const char *h = hay; *h; h++) {
        int k = 0;
        while (k < n && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == n) return true;
    }
    return false;
}

bool cmdreg_match(const char *query, const char *name, const char *desc) {
    const char *q = query;
    while (q && *q) {
        while (*q == ' ') q++;
        int n = 0;
        while (q[n] && q[n] != ' ') n++;
        if (n && !contains_ci(name, q, n) && !contains_ci(desc, q, n)) return false;
        q += n;
    }
    return true;
}

void cmdlist_parse(CmdList *l, const char *text) {
    memset(l, 0, sizeof(*l));
    const char *s = text ? text : "";
    while (*s && l->count < CMDREG_MAX_LIST) {
        while (*s == ' ') s++;
        int n = 0;
        while (s[n] && s[n] != ' ') n++;
        if (n > 0 && n < CMDREG_NAME) {
            char w[CMDREG_NAME];
            memcpy(w, s, (size_t)n);
            w[n] = 0;
            if (!cmdlist_has(l, w)) dg_strcpy(l->items[l->count++], CMDREG_NAME, w);
        }
        s += n;
    }
}

void cmdlist_format(const CmdList *l, char *out, int size) {
    if (size <= 0) return;
    out[0] = 0;
    int used = 0;
    for (int i = 0; i < l->count; i++) {
        int n = snprintf(out + used, (size_t)(size - used), "%s%s", i ? " " : "", l->items[i]);
        if (n < 0 || used + n >= size) { out[used] = 0; break; }   // whole names only
        used += n;
    }
}

bool cmdlist_has(const CmdList *l, const char *name) {
    for (int i = 0; i < l->count; i++) if (!strcmp(l->items[i], name)) return true;
    return false;
}

bool cmdlist_toggle(CmdList *l, const char *name) {
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->items[i], name)) continue;
        memmove(&l->items[i], &l->items[i + 1], sizeof(l->items[0]) * (size_t)(l->count - i - 1));
        l->count--;
        return false;
    }
    if (l->count >= CMDREG_MAX_LIST) return false;
    dg_strcpy(l->items[l->count++], CMDREG_NAME, name);
    return true;
}

void cmdlist_touch(CmdList *l, const char *name, int max) {
    if (max > CMDREG_MAX_LIST) max = CMDREG_MAX_LIST;
    int at = -1;
    for (int i = 0; i < l->count; i++) if (!strcmp(l->items[i], name)) at = i;
    if (at < 0) at = l->count < max ? l->count++ : max - 1;
    memmove(&l->items[1], &l->items[0], sizeof(l->items[0]) * (size_t)at);
    dg_strcpy(l->items[0], CMDREG_NAME, name);
    if (l->count > max) l->count = max;
}
