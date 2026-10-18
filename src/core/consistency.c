#include "consistency.h"
#include "cvar.h"
#include "log.h"
#include "platform.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 128

typedef struct Entry {
    char name[CVAR_NAME_MAX];
    char value[CVAR_STR_MAX];
    char why[200];
} Entry;

static struct {
    Entry e[MAX_ENTRIES];
    int n;
} C;

static void errf(char *err, int n, const char *fmt, const char *a) {
    if (err && n > 0) snprintf(err, (size_t)n, fmt, a);
}

bool consistency_load_text(const char *json, char *err, int n) {
    C.n = 0;
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) { errf(err, n, "consistency: JSON parse error%s", ""); return false; }
    const cJSON *vals = cJSON_GetObjectItemCaseSensitive(root, "values"), *it;
    bool ok = cJSON_IsObject(vals);
    if (!ok) errf(err, n, "consistency: missing \"values\"%s", "");
    if (ok) cJSON_ArrayForEach(it, vals) {
        if (C.n >= MAX_ENTRIES) { errf(err, n, "consistency: more than %s entries", "128"); ok = false; break; }
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(it, "value"), *w = cJSON_GetObjectItemCaseSensitive(it, "why");
        if (!cJSON_IsObject(it) || !v || !cJSON_IsString(w) || !w->valuestring[0]) { errf(err, n, "consistency: '%s' needs a \"value\" and a \"why\"", it->string); ok = false; break; }
        Entry *e = &C.e[C.n++];
        dg_strcpy(e->name, sizeof(e->name), it->string);
        if (cJSON_IsString(v)) dg_strcpy(e->value, sizeof(e->value), v->valuestring);
        else if (cJSON_IsBool(v)) dg_strcpy(e->value, sizeof(e->value), cJSON_IsTrue(v) ? "1" : "0");
        else if (cJSON_IsNumber(v)) snprintf(e->value, sizeof(e->value), "%g", v->valuedouble);
        else { errf(err, n, "consistency: '%s' value must be a number, bool or string", it->string); ok = false; break; }
        dg_strcpy(e->why, sizeof(e->why), w->valuestring);
        for (int k = 0; k < C.n - 1; k++)
            if (!strcmp(C.e[k].name, e->name)) { errf(err, n, "consistency: '%s' listed twice", e->name); ok = false; }
    }
    cJSON_Delete(root);
    if (!ok) C.n = 0;
    return ok;
}

bool consistency_load(const char *path) {
    char *text = platform_read_file(path, NULL);
    if (!text) { log_error("consistency: cannot read %s", path); return false; }
    char err[200] = "";
    bool ok = consistency_load_text(text, err, sizeof(err));
    free(text);
    if (!ok) log_error("%s (%s)", err, path);
    return ok;
}

int consistency_apply(char *err, int n) {
    int bad = 0;
    if (err && n > 0) err[0] = 0;
    CvarSource src = cvar_source_push(CVAR_SRC_HOST);
    for (int i = 0; i < C.n; i++) {
        Entry *e = &C.e[i];
        Cvar *cv = cvar_find(e->name);
        if (!cv || !(cv->flags & CVAR_LOCKED)) {
            bad++;
            if (err && n > 0 && !err[0]) snprintf(err, (size_t)n, "consistency: '%s' %s", e->name, cv ? "is not CVAR_LOCKED" : "is not a registered cvar");
            continue;
        }
        cvar_set_cv(cv, e->value);
        dg_strcpy(e->value, sizeof(e->value), cv->str);   // normalized (%g, clamped)
        dg_strcpy(cv->default_str, sizeof(cv->default_str), cv->str);
    }
    cvar_source_pop(src);
    return bad;
}

int consistency_count(void) { return C.n; }
const char *consistency_name(int i) { return i >= 0 && i < C.n ? C.e[i].name : ""; }
const char *consistency_value(int i) { return i >= 0 && i < C.n ? C.e[i].value : ""; }
const char *consistency_why(int i) { return i >= 0 && i < C.n ? C.e[i].why : ""; }
const char *consistency_find(const char *cvar) {
    for (int i = 0; i < C.n; i++) if (!strcmp(C.e[i].name, cvar)) return C.e[i].value;
    return NULL;
}

static int cmp_entry(const void *a, const void *b) { return strcmp((*(const Entry *const *)a)->name, (*(const Entry *const *)b)->name); }

static u32 fnv(u32 h, const char *s) {
    for (; *s; s++) { h ^= (u8)*s; h *= 16777619u; }
    return h;
}

static u32 hash_set(bool effective) {
    const Entry *list[MAX_ENTRIES];
    for (int i = 0; i < C.n; i++) list[i] = &C.e[i];
    qsort(list, (size_t)C.n, sizeof(list[0]), cmp_entry);
    u32 h = 2166136261u;
    for (int i = 0; i < C.n; i++) {
        const char *v = list[i]->value;
        if (effective) {
            Cvar *cv = cvar_find(list[i]->name);
            if (cv && (cv->flags & CVAR_RULE)) continue;   // the host decides these for everyone
            if (cv) v = cv->str;
        }
        h = fnv(h, list[i]->name);
        h = fnv(h, "=");
        h = fnv(h, v);
        h = fnv(h, ";");
    }
    return h;
}

u32 consistency_hash(void) { return hash_set(false); }
u32 consistency_hash_effective(void) { return hash_set(true); }

int consistency_check(char *report, int size) {
    int bad = 0;
    if (report && size > 0) report[0] = 0;
    for (int i = 0; i < C.n; i++) {
        Cvar *cv = cvar_find(C.e[i].name);
        if (!cv || (cv->flags & CVAR_RULE) || !strcmp(cv->str, C.e[i].value)) continue;
        bad++;
        if (report && size > 0) {
            size_t len = strlen(report);
            if (len + 80 < (size_t)size) snprintf(report + len, (size_t)size - len, "%s%s=%s (locked %s)", len ? ", " : "", cv->name, cv->str, C.e[i].value);
        }
    }
    return bad;
}

void consistency_init(void) {
    if (!consistency_load(CONSISTENCY_PATH)) return;
    char err[200];
    int bad = consistency_apply(err, sizeof(err));
    if (bad) log_error("%s (%d bad entr%s in %s)", err, bad, bad == 1 ? "y" : "ies", CONSISTENCY_PATH);
    log_info("consistency: %d locked values, hash %08x, release locks %s", C.n, consistency_hash(), cvar_release_locks() ? "on" : "off (dev build)");
}
