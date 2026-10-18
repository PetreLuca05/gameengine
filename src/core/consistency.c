// Consistency set: see consistency.h.
#include "consistency.h"
#include "cvar.h"
#include "log.h"
#include "platform.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONS_MAX 128

typedef struct ConsEntry { char name[CVAR_NAME_MAX]; char value[CVAR_STR_MAX]; char note[160]; } ConsEntry;

static struct {
    ConsEntry e[CONS_MAX];
    int n;
    u32 hash;
} C;

static int cmp_entry(const void *a, const void *b) { return strcmp(((const ConsEntry *)a)->name, ((const ConsEntry *)b)->name); }

static u32 fnv(u32 h, const char *s) {
    for (; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h;
}

bool consistency_load_text(const char *json, char *err, int n) {
    C.n = 0;
    C.hash = 0;
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) { snprintf(err, (size_t)n, "consistency.json: parse error"); return false; }
    const cJSON *vals = cJSON_GetObjectItemCaseSensitive(root, "values"), *it;
    bool ok = cJSON_IsObject(vals);
    if (!ok) snprintf(err, (size_t)n, "consistency.json: missing \"values\"");
    if (ok) cJSON_ArrayForEach(it, vals) {
        if (C.n >= CONS_MAX) { snprintf(err, (size_t)n, "consistency.json: more than %d values", CONS_MAX); ok = false; break; }
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(it, "value"), *why = cJSON_GetObjectItemCaseSensitive(it, "why");
        ConsEntry *e = &C.e[C.n];
        dg_strcpy(e->name, sizeof(e->name), it->string);
        if (cJSON_IsNumber(v)) snprintf(e->value, sizeof(e->value), "%g", v->valuedouble);
        else if (cJSON_IsString(v)) dg_strcpy(e->value, sizeof(e->value), v->valuestring);
        else if (cJSON_IsBool(v)) dg_strcpy(e->value, sizeof(e->value), cJSON_IsTrue(v) ? "1" : "0");
        else { snprintf(err, (size_t)n, "consistency.json: %s needs a \"value\"", it->string); ok = false; break; }
        if (!cJSON_IsString(why) || strlen(why->valuestring) < 8) { snprintf(err, (size_t)n, "consistency.json: %s needs a \"why\"", it->string); ok = false; break; }
        dg_strcpy(e->note, sizeof(e->note), why->valuestring);
        C.n++;
    }
    cJSON_Delete(root);
    if (!ok) { C.n = 0; return false; }
    qsort(C.e, (size_t)C.n, sizeof(C.e[0]), cmp_entry);
    for (int i = 1; i < C.n; i++)
        if (!strcmp(C.e[i].name, C.e[i - 1].name)) { snprintf(err, (size_t)n, "consistency.json: %s twice", C.e[i].name); C.n = 0; return false; }
    u32 h = 2166136261u;
    for (int i = 0; i < C.n; i++) {
        h = fnv(h, C.e[i].name);
        h = fnv(h, "=");
        h = fnv(h, C.e[i].value);
        h = fnv(h, ";");
    }
    C.hash = h;
    return true;
}

bool consistency_load(const char *path) {
    char *text = platform_read_file(path, NULL);
    if (!text) { log_error("consistency: cannot read %s (no shared values: joins will be rejected)", path); C.n = 0; C.hash = 0; return false; }
    char err[256];
    bool ok = consistency_load_text(text, err, sizeof(err));
    free(text);
    if (!ok) log_error("%s", err);
    return ok;
}

int consistency_count(void) { return C.n; }
const char *consistency_name(int i) { return i >= 0 && i < C.n ? C.e[i].name : ""; }
const char *consistency_value(int i) { return i >= 0 && i < C.n ? C.e[i].value : ""; }
const char *consistency_note(int i) { return i >= 0 && i < C.n ? C.e[i].note : ""; }
int consistency_find(const char *cvar) {
    for (int i = 0; i < C.n; i++) if (!strcmp(C.e[i].name, cvar)) return i;
    return -1;
}
u32 consistency_hash(void) { return C.hash; }

bool consistency_join_ok(u32 theirs, char *why, int n) {
    if (theirs == C.hash) return true;
    snprintf(why, (size_t)n, "consistency set mismatch (host %08x, you %08x): assets/render/consistency.json differs", C.hash, theirs);
    return false;
}

int consistency_apply(void) {
    int bad = 0;
    CvarSource src = cvar_source_push(CVAR_SRC_HOST);
    for (int i = 0; i < C.n; i++) {
        Cvar *cv = cvar_find(C.e[i].name);
        if (!cv) { log_error("consistency: %s is not a registered cvar", C.e[i].name); bad++; continue; }
        if (!(cv->flags & CVAR_LOCKED)) { log_error("consistency: %s is listed but not CVAR_LOCKED (docs/SETTINGS_AUDIT.md)", cv->name); bad++; continue; }
        cvar_set_cv(cv, C.e[i].value);
        dg_strcpy(cv->default_str, sizeof(cv->default_str), cv->str);   // reset returns to the shared value
    }
    cvar_source_pop(src);
    return bad;
}

int consistency_diff(char *report, int size) {
    int d = 0;
    if (report && size > 0) report[0] = 0;
    for (int i = 0; i < C.n; i++) {
        Cvar *cv = cvar_find(C.e[i].name);
        if (!cv) continue;
        char want[CVAR_STR_MAX];
        dg_strcpy(want, sizeof(want), C.e[i].value);
        // Compare the parsed values (the set may write 1.0 where the cvar prints 1).
        bool same = cv->type == CVAR_STRING ? !strcmp(cv->str, want) : (cv->type == CVAR_FLOAT ? (float)atof(want) == cv->f : atoi(want) == cv->i);
        if (same) continue;
        d++;
        if (report && size > 0) {
            size_t len = strlen(report);
            if (len + 64 < (size_t)size) snprintf(report + len, (size_t)size - len, "%s%s=%s (set %s)", len ? ", " : "", cv->name, cv->str, want);
        }
    }
    return d;
}
