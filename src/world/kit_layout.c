// Building kit layout: prefab expansion (docs/BUILDING_KIT.md "Prefabs"). Pure; see kit_layout.h.
#include "world/kit_layout.h"
#include "cJSON.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void kit_rotate_xz(float yaw_deg, float x, float z, float *ox, float *oz) {
    float a = yaw_deg * 0.017453292519943295f, c = cosf(a), s = sinf(a);
    // Rotation about +Y (quat_yaw): x' = x cos + z sin, z' = -x sin + z cos.
    *ox = x * c + z * s;
    *oz = -x * s + z * c;
}

static void fail(KitLayout *L, const char *fmt, ...) {
    if (!L->errors++) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(L->err, sizeof(L->err), fmt, ap);
        va_end(ap);
    }
}

static void jvec3(const cJSON *o, const char *k, float out[3]) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < 3 && cJSON_IsArray(v) && i < cJSON_GetArraySize(v); i++) out[i] = (float)cJSON_GetArrayItem(v, i)->valuedouble;
}

static float jyaw(const cJSON *o) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "yaw");
    return cJSON_IsNumber(v) ? (float)v->valuedouble : 0.0f;
}

typedef struct Frame {
    float pos[3], yaw;
    char prefix[48];
    const cJSON *overrides[KIT_LAYOUT_DEPTH];   // outermost first
    int override_n;
} Frame;

static void child_frame(const Frame *f, const cJSON *e, Frame *out) {
    float p[3], rx, rz;
    jvec3(e, "pos", p);
    kit_rotate_xz(f->yaw, p[0], p[2], &rx, &rz);
    *out = *f;
    out->pos[0] = f->pos[0] + rx;
    out->pos[1] = f->pos[1] + p[1];
    out->pos[2] = f->pos[2] + rz;
    out->yaw = f->yaw + jyaw(e);
}

static void place(KitLayout *L, const Frame *parent, const cJSON *e, int depth) {
    Frame f;
    child_frame(parent, e, &f);
    const char *piece = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "piece"));
    if (!piece || !*piece) { fail(L, "kit instance without \"piece\""); return; }
    cJSON *copy = cJSON_Duplicate(e, true);
    // Names: prefixed by the enclosing prefab instances; overrides by the (unprefixed) child name, outermost wins.
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "name"));
    if (name && *name) {
        for (int k = parent->override_n - 1; k >= 0; k--) {
            const cJSON *ov = cJSON_GetObjectItemCaseSensitive(parent->overrides[k], name);
            const cJSON *it;
            cJSON_ArrayForEach(it, ov) {
                cJSON_DeleteItemFromObjectCaseSensitive(copy, it->string);
                cJSON_AddItemToObject(copy, it->string, cJSON_Duplicate(it, true));
            }
        }
        if (parent->prefix[0]) {
            char full[96];
            snprintf(full, sizeof(full), "%s%s", parent->prefix, name);
            cJSON_DeleteItemFromObjectCaseSensitive(copy, "name");
            cJSON_AddStringToObject(copy, "name", full);
        }
    }
    cJSON_AddItemToArray(L->owned, copy);
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 256;
        L->items = realloc(L->items, sizeof(KitPlaced) * (size_t)L->cap);
    }
    KitPlaced *k = &L->items[L->n++];
    memset(k, 0, sizeof(*k));
    dg_strcpy(k->piece, sizeof(k->piece), piece);
    memcpy(k->pos, f.pos, sizeof(k->pos));
    k->yaw = f.yaw;
    k->entry = copy;
    k->prefab_depth = depth;
}

static void expand(KitLayout *L, const Frame *parent, const cJSON *list, KitPrefabLoader load, void *user, int depth);

static void use_prefab(KitLayout *L, const Frame *parent, const cJSON *e, KitPrefabLoader load, void *user, int depth) {
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "prefab"));
    if (depth >= KIT_LAYOUT_DEPTH) { fail(L, "prefab %s: nested deeper than %d (a cycle?)", name, KIT_LAYOUT_DEPTH); return; }
    char *text = load ? load(user, name) : NULL;
    cJSON *pf = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!pf) { fail(L, "prefab %s: missing or not JSON", name); return; }
    Frame f;
    child_frame(parent, e, &f);
    const char *pre = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "prefix"));
    if (pre) {
        char full[48];
        snprintf(full, sizeof(full), "%s%s", parent->prefix, pre);
        dg_strcpy(f.prefix, sizeof(f.prefix), full);
    }
    const cJSON *ov = cJSON_GetObjectItemCaseSensitive(e, "override");
    if (ov && f.override_n < KIT_LAYOUT_DEPTH) f.overrides[f.override_n++] = ov;
    expand(L, &f, cJSON_GetObjectItemCaseSensitive(pf, "pieces"), load, user, depth + 1);
    cJSON_AddItemToArray(L->owned, pf);   // overrides above point into e (the caller's), children copied already
}

static void expand(KitLayout *L, const Frame *parent, const cJSON *list, KitPrefabLoader load, void *user, int depth) {
    const cJSON *e;
    cJSON_ArrayForEach(e, list) {
        if (cJSON_GetObjectItemCaseSensitive(e, "prefab")) use_prefab(L, parent, e, load, user, depth);
        else place(L, parent, e, depth);
    }
}

void kit_layout_build(KitLayout *L, const cJSON *level, KitPrefabLoader load, void *user) {
    memset(L, 0, sizeof(*L));
    L->owned = cJSON_CreateArray();
    Frame root;
    memset(&root, 0, sizeof(root));
    expand(L, &root, cJSON_GetObjectItemCaseSensitive(level, "kit"), load, user, 0);
    expand(L, &root, cJSON_GetObjectItemCaseSensitive(level, "prefabs"), load, user, 0);
}

void kit_layout_free(KitLayout *L) {
    free(L->items);
    cJSON_Delete(L->owned);
    memset(L, 0, sizeof(*L));
}
