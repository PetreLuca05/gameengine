// Level includes and the footprint pads of included content (level_include.h, docs/ZOMBIE_MAP.md
// "Level includes").
#include "level_include.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INCLUDE_MAX_DEPTH 4

// ------------------------------------------------------------------ merge

// src into dst: arrays appended, objects merged (dst wins), other values only where dst has none.
static void merge_into(cJSON *dst, const cJSON *src) {
    for (const cJSON *it = src->child; it; it = it->next) {
        if (!it->string || !strcmp(it->string, "include")) continue;
        cJSON *d = cJSON_GetObjectItemCaseSensitive(dst, it->string);
        if (!d) {
            cJSON_AddItemToObject(dst, it->string, cJSON_Duplicate(it, true));
        } else if (cJSON_IsArray(d) && cJSON_IsArray(it)) {
            for (const cJSON *e = it->child; e; e = e->next) cJSON_AddItemToArray(d, cJSON_Duplicate(e, true));
        } else if (cJSON_IsObject(d) && cJSON_IsObject(it)) {
            merge_into(d, it);
        }
    }
}

static float num(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void parse_ground(const cJSON *o, LevelGround *g) {
    level_ground_defaults(g);
    g->level = num(o, "level", g->level);
    g->cell = fmaxf(num(o, "cell", g->cell), 0.5f);
    g->margin = fmaxf(num(o, "margin", g->margin), 0.0f);
    g->blend = fmaxf(num(o, "blend", g->blend), 0.5f);
    g->room_sink = fmaxf(num(o, "room_sink", g->room_sink), 0.0f);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "material");
    if (cJSON_IsString(m)) snprintf(g->material, sizeof(g->material), "%s", m->valuestring);
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(o, "hard");
    if (cJSON_IsObject(h)) {
        const cJSON *hm = cJSON_GetObjectItemCaseSensitive(h, "material");
        snprintf(g->hard_material, sizeof(g->hard_material), "%s", cJSON_IsString(hm) ? hm->valuestring : "rock");
        g->hard_margin = fmaxf(num(h, "margin", g->hard_margin), 0.0f);
        g->hard_min_height = fmaxf(num(h, "min_height", g->hard_min_height), 0.0f);
        g->hard_min_size = fmaxf(num(h, "min_size", g->hard_min_size), 0.0f);
    }
}

static bool resolve(cJSON *root, LevelReadFn read, void *user, int depth, char *err, size_t err_size) {
    cJSON *inc = cJSON_DetachItemFromObjectCaseSensitive(root, "include");
    if (!inc) return true;
    bool ok = true;
    if (depth >= INCLUDE_MAX_DEPTH) {
        snprintf(err, err_size, "include: nested more than %d levels deep (a cycle?)", INCLUDE_MAX_DEPTH);
        ok = false;
    }
    if (ok && !cJSON_IsArray(inc) && !cJSON_IsString(inc) && !cJSON_IsObject(inc)) {
        snprintf(err, err_size, "include: a level name, an {\"level\": ...} object or an array of them");
        ok = false;
    }
    // a single name / object works like a one-entry array
    int n = !ok ? 0 : cJSON_IsArray(inc) ? cJSON_GetArraySize(inc) : 1;
    for (int i = 0; i < n && ok; i++) {
        const cJSON *e = cJSON_IsArray(inc) ? cJSON_GetArrayItem(inc, i) : inc;
        const cJSON *lv = cJSON_IsString(e) ? e : cJSON_GetObjectItemCaseSensitive(e, "level");
        if (!cJSON_IsString(lv)) { snprintf(err, err_size, "include %d: no level name", i); ok = false; break; }
        cJSON *sub = read(lv->valuestring, user);
        if (!sub) { snprintf(err, err_size, "include: cannot load level '%s'", lv->valuestring); ok = false; break; }
        ok = resolve(sub, read, user, depth + 1, err, err_size);
        if (ok) {
            merge_into(root, sub);
            const cJSON *ground = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "ground") : NULL;
            if (cJSON_IsObject(ground)) {
                cJSON *world = cJSON_GetObjectItemCaseSensitive(root, "world");
                if (!cJSON_IsObject(world)) {
                    snprintf(err, err_size, "include '%s': \"ground\" needs a \"world\" block (an open-world level)", lv->valuestring);
                    ok = false;
                } else {
                    cJSON *features = cJSON_GetObjectItemCaseSensitive(world, "features");
                    if (!cJSON_IsArray(features)) {
                        cJSON_DeleteItemFromObjectCaseSensitive(world, "features");
                        features = cJSON_AddArrayToObject(world, "features");
                    }
                    LevelGround g;
                    parse_ground(ground, &g);
                    level_footprint_pads(sub, &g, features);
                }
            }
        }
        cJSON_Delete(sub);
    }
    cJSON_Delete(inc);
    return ok;
}

bool level_include_resolve(cJSON *root, LevelReadFn read, void *user, char *err, size_t err_size) {
    if (err_size) err[0] = 0;
    return resolve(root, read, user, 0, err, err_size);
}

// ------------------------------------------------------------------ footprint

void level_ground_defaults(LevelGround *g) {
    memset(g, 0, sizeof(*g));
    g->level = 0.0f;
    g->cell = 4.0f;
    g->margin = 6.0f;
    g->blend = 12.0f;
    g->room_sink = 0.08f;
    snprintf(g->material, sizeof(g->material), "concrete");
    g->hard_margin = 1.5f;
    g->hard_min_height = 0.15f;
    g->hard_min_size = 6.0f;
}

typedef struct FpRect { float x0, z0, x1, z1; } FpRect;
typedef struct FpList { FpRect *r; int n, cap; } FpList;

static void fp_add(FpList *l, float cx, float cz, float hx, float hz, float yaw_deg) {
    float c = fabsf(cosf(yaw_deg * 0.017453293f)), s = fabsf(sinf(yaw_deg * 0.017453293f));
    float ax = c * hx + s * hz, az = s * hx + c * hz;   // the rotated rectangle's bounds
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->r = (FpRect *)realloc(l->r, sizeof(FpRect) * (size_t)l->cap);
    }
    l->r[l->n++] = (FpRect){ cx - ax, cz - az, cx + ax, cz + az };
}

// [x, y, z] (or [x, z] when two_d) of key k; false without one.
static bool xz(const cJSON *o, const char *k, bool two_d, float *x, float *z) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    int n = cJSON_GetArraySize(v);
    if (!cJSON_IsArray(v) || n < 2) return false;
    *x = (float)cJSON_GetArrayItem(v, 0)->valuedouble;
    *z = (float)cJSON_GetArrayItem(v, two_d || n < 3 ? 1 : 2)->valuedouble;
    return true;
}

static float arr_f(const cJSON *o, const char *k, int i, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return (float)v->valuedouble;
    const cJSON *e = cJSON_IsArray(v) ? cJSON_GetArrayItem(v, i) : NULL;
    return cJSON_IsNumber(e) ? (float)e->valuedouble : def;
}

// Brushes (level.c build_brush): the box / board / wall exactly, ramps / stairs / tunnels as the
// square around their start that holds them in any direction.
static void fp_brush(FpList *l, const cJSON *b) {
    float x, z;
    if (!xz(b, "pos", false, &x, &z)) x = z = 0;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(b, "type");
    const char *type = cJSON_IsString(t) ? t->valuestring : "box";
    float yaw = num(b, "yaw", 0);
    if (!strcmp(type, "box")) {
        fp_add(l, x, z, arr_f(b, "size", 0, 1) * 0.5f, arr_f(b, "size", 2, 1) * 0.5f, yaw);
    } else if (!strcmp(type, "moa_target")) {
        fp_add(l, x, z, num(b, "size", 1) * 0.5f, 0.1f, yaw);
    } else if (!strcmp(type, "doorway")) {
        fp_add(l, x, z, num(b, "length", 6) * 0.5f, num(b, "thickness", 0.25f) * 0.5f, yaw);
    } else if (!strcmp(type, "tunnel")) {
        fp_add(l, x, z, num(b, "inner_width", 1.4f) * 0.5f + 0.25f, num(b, "length", 6) * 0.5f, yaw);
    } else {
        float r = num(b, "width", 2) * 0.5f + num(b, "platform", 0);
        if (!strcmp(type, "ramp")) r += num(b, "length", 4);
        else r += num(b, "count", 8) * num(b, "run", 0.3f);
        fp_add(l, x, z, r, r, 0);
    }
}

static void fp_entity(FpList *l, const cJSON *e) {
    float x, z;
    if (!xz(e, "pos", false, &x, &z)) return;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "type");
    const char *type = cJSON_IsString(t) ? t->valuestring : "";
    float hx = 1.0f, hz = 1.0f;
    if (!strcmp(type, "prop")) {
        hx = hz = fmaxf(arr_f(e, "size", 0, 0.8f), arr_f(e, "size", 2, 0.8f)) * 0.5f;
    } else if (!strcmp(type, "vgeo")) {
        hx = arr_f(e, "grid", 0, 1) * num(e, "spacing", 4) * 0.5f + 1.0f;
        hz = arr_f(e, "grid", 1, 1) * num(e, "spacing", 4) * 0.5f + 1.0f;
    } else if (!strcmp(type, "zombie_area")) {
        hx = num(e, "cols", 5) * arr_f(e, "spacing", 0, 2) * 0.5f + num(e, "radius", 1.8f);
        hz = num(e, "rows", 4) * arr_f(e, "spacing", 2, 3) * 0.5f + num(e, "radius", 1.8f);
    } else if (!strcmp(type, "gate")) {
        hx = hz = num(e, "width", 3) * 0.5f;
    }
    fp_add(l, x, z, hx, hz, 0);
}

static void fp_collect(FpList *l, const cJSON *lv) {
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(lv, "room"), *it;
    if (cJSON_IsObject(room)) fp_add(l, 0, 0, arr_f(room, "size", 0, 60) * 0.5f, arr_f(room, "size", 2, 60) * 0.5f, 0);   // the room, not its "floor_size"
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(lv, "brushes")) fp_brush(l, it);
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(lv, "entities")) fp_entity(l, it);
    float x, z;
    static const struct { const char *key; float r; } points[] = { { "labels", 0.5f }, { "spawns", 1.0f }, { "vehicles", 3.0f }, { "fuel_pumps", 1.5f } };
    for (size_t k = 0; k < sizeof(points) / sizeof(points[0]); k++)
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(lv, points[k].key)) if (xz(it, "pos", false, &x, &z)) fp_add(l, x, z, points[k].r, points[k].r, 0);
    static const char *areas2d[] = { "vehicle_spikes", "puddles" };   // pos [x, z], size [w, d]
    for (size_t k = 0; k < 2; k++)
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(lv, areas2d[k])) if (xz(it, "pos", true, &x, &z)) fp_add(l, x, z, arr_f(it, "size", 0, 1) * 0.5f, arr_f(it, "size", 1, 1) * 0.5f, 0);
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(lv, "water")) {
        const cJSON *box = cJSON_GetObjectItemCaseSensitive(it, "box"), *ell = cJSON_GetObjectItemCaseSensitive(it, "ellipse"), *poly = cJSON_GetObjectItemCaseSensitive(it, "polygon");
        if (cJSON_IsObject(box) && xz(box, "center", true, &x, &z)) fp_add(l, x, z, arr_f(box, "half", 0, 1), arr_f(box, "half", 1, 1), 0);
        if (cJSON_IsObject(ell) && xz(ell, "center", true, &x, &z)) {
            float r = fmaxf(arr_f(ell, "radius", 0, 1), arr_f(ell, "radius", 1, 1));
            fp_add(l, x, z, r, r, 0);
        }
        const cJSON *p;
        cJSON_ArrayForEach(p, cJSON_IsArray(poly) ? poly : NULL) if (cJSON_GetArraySize(p) >= 2) fp_add(l, (float)cJSON_GetArrayItem(p, 0)->valuedouble, (float)cJSON_GetArrayItem(p, 1)->valuedouble, 0.5f, 0.5f, 0);
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(lv, "post_volumes")) {
        float x1, z1;
        if (xz(it, "min", false, &x, &z) && xz(it, "max", false, &x1, &z1)) fp_add(l, (x + x1) * 0.5f, (z + z1) * 0.5f, fabsf(x1 - x) * 0.5f, fabsf(z1 - z) * 0.5f, 0);
    }
    // "footprint_areas": [[x0, z0, x1, z1], ...]: ground the content needs without brushes on it (firing
    // lanes, the foliage test yard).
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(lv, "footprint_areas")) {
        if (cJSON_GetArraySize(it) < 4) continue;
        float a = (float)cJSON_GetArrayItem(it, 0)->valuedouble, b = (float)cJSON_GetArrayItem(it, 1)->valuedouble;
        float c = (float)cJSON_GetArrayItem(it, 2)->valuedouble, d = (float)cJSON_GetArrayItem(it, 3)->valuedouble;
        fp_add(l, (a + c) * 0.5f, (b + d) * 0.5f, fabsf(c - a) * 0.5f, fabsf(d - b) * 0.5f, 0);
    }
}

// ------------------------------------------------------------------ hard structures

// The ground a structure brush stands on as a rotated rectangle (centre, half extents, yaw in degrees;
// level.c build_brush's geometry); false for what is no structure on the ground: raised boxes (shelf
// items on tables, beams), boxes lower than min_height (marker plates, floor strips), non-colliding
// boxes, sighting targets.
static bool hard_rect(const cJSON *b, float min_height, float *cx, float *cz, float *hx, float *hz, float *yaw) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(b, "type");
    const char *type = cJSON_IsString(t) ? t->valuestring : "box";
    const cJSON *pos = cJSON_GetObjectItemCaseSensitive(b, "pos");
    float x = arr_f(b, "pos", 0, 0), y = cJSON_GetArraySize(pos) >= 3 ? arr_f(b, "pos", 1, 0) : 0.0f, z = arr_f(b, "pos", cJSON_GetArraySize(pos) >= 3 ? 2 : 1, 0);
    if (y > 0.3f) return false;   // pos is the brush's base: it does not stand on the ground
    *yaw = num(b, "yaw", 0);
    float lz = 0.0f;   // the rectangle's centre along the brush's local Z
    if (!strcmp(type, "box")) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(b, "collide");
        if (arr_f(b, "size", 1, 1) < min_height || cJSON_IsFalse(c)) return false;
        *hx = arr_f(b, "size", 0, 1) * 0.5f;
        *hz = arr_f(b, "size", 2, 1) * 0.5f;
    } else if (!strcmp(type, "ramp")) {   // centred on pos along local Z, the platform behind (-Z)
        float plat = num(b, "platform", 0);
        *hx = num(b, "width", 2) * 0.5f;
        *hz = num(b, "length", 4) * 0.5f + plat * 0.5f;
        lz = -plat * 0.5f;
    } else if (!strcmp(type, "stairs")) {   // from pos towards local -Z, then the platform
        float len = num(b, "count", 8) * num(b, "run", 0.3f) + num(b, "platform", 0);
        *hx = num(b, "width", 2) * 0.5f;
        *hz = len * 0.5f;
        lz = -len * 0.5f;
    } else if (!strcmp(type, "doorway")) {
        *hx = num(b, "length", 6) * 0.5f;
        *hz = num(b, "thickness", 0.25f) * 0.5f;
    } else if (!strcmp(type, "tunnel")) {
        *hx = num(b, "inner_width", 1.4f) * 0.5f + 0.25f;
        *hz = num(b, "length", 6) * 0.5f;
    } else {
        return false;
    }
    float r = *yaw * 0.017453293f;   // local (0, lz) through quat_yaw: x += lz sin, z += lz cos
    *cx = x + lz * sinf(r);
    *cz = z + lz * cosf(r);
    return true;
}

static void add_paint(cJSON *features, float cx, float cz, float w, float d, float yaw, const char *material) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "paint");
    const float pos[2] = { cx, cz }, size[2] = { w, d };
    cJSON_AddItemToObject(p, "pos", cJSON_CreateFloatArray(pos, 2));
    cJSON_AddItemToObject(p, "size", cJSON_CreateFloatArray(size, 2));
    if (yaw != 0.0f) cJSON_AddNumberToObject(p, "yaw", yaw);
    cJSON_AddStringToObject(p, "material", material);
    cJSON_AddItemToArray(features, p);
}

static int hard_paints(const cJSON *lv, const LevelGround *g, cJSON *features) {
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(lv, "brushes")) {
        float cx, cz, hx, hz, yaw;
        if (!hard_rect(it, g->hard_min_height, &cx, &cz, &hx, &hz, &yaw)) continue;
        add_paint(features, cx, cz, fmaxf(2.0f * (hx + g->hard_margin), g->hard_min_size), fmaxf(2.0f * (hz + g->hard_margin), g->hard_min_size), yaw, g->hard_material);
        n++;
    }
    return n;
}

static void add_pad(cJSON *features, float cx, float cz, float w, float d, float level, float blend, const char *material) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "pad");
    const float pos[2] = { cx, cz }, size[2] = { w, d };
    cJSON_AddItemToObject(p, "pos", cJSON_CreateFloatArray(pos, 2));
    cJSON_AddItemToObject(p, "size", cJSON_CreateFloatArray(size, 2));
    cJSON_AddNumberToObject(p, "level", level);
    cJSON_AddNumberToObject(p, "blend", blend);
    if (material[0]) cJSON_AddStringToObject(p, "material", material);
    cJSON_AddItemToArray(features, p);
}

int level_footprint_pads(const cJSON *lv, const LevelGround *g, cJSON *features) {
    FpList l = { 0 };
    fp_collect(&l, lv);
    if (!l.n) return 0;
    int pads = 0;
    // Rasterize the grown rectangles on a cell grid, then cover the marked cells with rectangles
    // (greedy: widest run, then as many rows as it spans).
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (int i = 0; i < l.n; i++) {
        x0 = fminf(x0, l.r[i].x0 - g->margin);
        z0 = fminf(z0, l.r[i].z0 - g->margin);
        x1 = fmaxf(x1, l.r[i].x1 + g->margin);
        z1 = fmaxf(z1, l.r[i].z1 + g->margin);
    }
    const float c = g->cell;
    x0 = floorf(x0 / c) * c;
    z0 = floorf(z0 / c) * c;
    int nx = (int)ceilf((x1 - x0) / c), nz = (int)ceilf((z1 - z0) / c);
    if (nx <= 0 || nz <= 0 || (double)nx * nz > 64e6) {
        free(l.r);
        return g->hard_material[0] ? hard_paints(lv, g, features) : 0;
    }
    unsigned char *m = (unsigned char *)calloc((size_t)nx * (size_t)nz, 1);
    for (int i = 0; i < l.n; i++) {
        int a = (int)floorf((l.r[i].x0 - g->margin - x0) / c), b = (int)ceilf((l.r[i].x1 + g->margin - x0) / c);
        int e = (int)floorf((l.r[i].z0 - g->margin - z0) / c), f = (int)ceilf((l.r[i].z1 + g->margin - z0) / c);
        for (int z = e < 0 ? 0 : e; z < f && z < nz; z++)
            for (int x = a < 0 ? 0 : a; x < b && x < nx; x++) m[(size_t)z * nx + x] = 1;
    }
    for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++) {
            if (m[(size_t)z * nx + x] != 1) continue;
            int w = 1, h = 1;
            while (x + w < nx && m[(size_t)z * nx + x + w] == 1) w++;
            for (bool full = true; z + h < nz && full; ) {
                for (int k = 0; k < w && full; k++) full = m[(size_t)(z + h) * nx + x + k] == 1;
                if (full) h++;
            }
            for (int r = 0; r < h; r++) memset(m + (size_t)(z + r) * nx + x, 2, (size_t)w);
            add_pad(features, x0 + ((float)x + (float)w * 0.5f) * c, z0 + ((float)z + (float)h * 0.5f) * c, (float)w * c, (float)h * c, g->level, g->blend, g->material);
            pads++;
        }
    free(m);
    free(l.r);
    // The room's floor box (level.c build_room) lies on the ground: the terrain under it sinks a little
    // so the two do not fight. Last, so it wins over the pads around it.
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(lv, "room");
    if (cJSON_IsObject(room) && g->room_sink > 0.0f) {
        float t = num(room, "wall", 0.5f);
        add_pad(features, 0, 0, arr_f(room, "size", 0, 60) + 2.0f * t, arr_f(room, "size", 2, 60) + 2.0f * t, g->level - g->room_sink, 0.6f, g->material);
        pads++;
    }
    // the hard structures' ground (paint wins over the pads whatever the order)
    if (g->hard_material[0]) pads += hard_paints(lv, g, features);
    return pads;
}
