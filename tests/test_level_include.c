// Level includes (src/world/level_include.c, docs/ZOMBIE_MAP.md "Level includes"): the merge rules
// (arrays appended, objects merged with the including level winning, values filled only where missing),
// errors (missing level, cycles), the footprint pads; and on the shipped data (run from the exe folder):
// devroom_vegetation holds every dev room element, the generated ground is flat at y = 0 (the room
// floor slab: the nature ground) and flat grass under every brush, entity and footprint area, with no trees.
#include "world/level_include.h"
#include "world/mapgen.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

// ------------------------------------------------------------------ in-memory levels

static const char *k_mem[][2] = {
    { "base", "{\"name\": \"Base\", \"weather\": \"rain\", \"env\": {\"fog\": [1, 1, 1], \"clouds\": 0.9}, \"brushes\": [{\"type\": \"box\", \"pos\": [10, 0, 10], \"size\": [4, 2, 2]}],"
              " \"labels\": [{\"pos\": [10, 1, 10], \"text\": \"b\"}], \"include\": \"leaf\"}" },
    { "leaf", "{\"name\": \"Leaf\", \"brushes\": [{\"type\": \"box\", \"pos\": [-20, 0, 0], \"size\": [1, 1, 1]}], \"vehicles\": [{\"def\": \"car\", \"pos\": [0, 0, 40]}]}" },
    { "loop_a", "{\"include\": [\"loop_b\"]}" },
    { "loop_b", "{\"include\": [\"loop_a\"]}" },
};

static cJSON *read_mem(const char *level, void *user) {
    (void)user;
    for (size_t i = 0; i < sizeof(k_mem) / sizeof(k_mem[0]); i++)
        if (!strcmp(k_mem[i][0], level)) return cJSON_Parse(k_mem[i][1]);
    return NULL;
}

static int arr_n(const cJSON *o, const char *k) { return cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, k)); }

static void test_merge(void) {
    cJSON *r = cJSON_Parse("{\"name\": \"Top\", \"env\": {\"fog\": [0, 0, 0]}, \"brushes\": [{\"type\": \"box\"}], \"include\": [\"base\"]}");
    char err[256];
    CHECK(level_include_resolve(r, read_mem, NULL, err, sizeof(err)), "resolve: %s", err);
    CHECK(!cJSON_GetObjectItemCaseSensitive(r, "include"), "the include key is consumed");
    CHECK(arr_n(r, "brushes") == 3, "brushes appended through two levels (%d)", arr_n(r, "brushes"));
    CHECK(arr_n(r, "labels") == 1 && arr_n(r, "vehicles") == 1, "missing arrays copied");
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(r, "name")->valuestring, "Top"), "the including level's name wins");
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(r, "weather")->valuestring, "rain"), "missing values filled");
    const cJSON *env = cJSON_GetObjectItemCaseSensitive(r, "env");
    CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(env, "fog"), 0)->valuedouble == 0.0, "objects: the including level's keys win");
    CHECK(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(env, "clouds")), "objects: missing keys merged in");
    // own entries first
    CHECK(!cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(r, "brushes"), 0), "pos"), "own brushes stay first");
    cJSON_Delete(r);

    r = cJSON_Parse("{\"include\": [\"nope\"]}");
    CHECK(!level_include_resolve(r, read_mem, NULL, err, sizeof(err)) && strstr(err, "nope"), "a missing level fails (%s)", err);
    cJSON_Delete(r);
    r = cJSON_Parse("{\"include\": [\"loop_a\"]}");
    CHECK(!level_include_resolve(r, read_mem, NULL, err, sizeof(err)), "a cycle fails");
    cJSON_Delete(r);
    r = cJSON_Parse("{\"include\": [{\"level\": \"leaf\", \"ground\": {}}]}");
    CHECK(!level_include_resolve(r, read_mem, NULL, err, sizeof(err)), "ground without a world block fails");
    cJSON_Delete(r);
}

static bool in_pads(const cJSON *pads, float x, float z, float *level) {
    const cJSON *p;
    bool in = false;
    cJSON_ArrayForEach(p, pads) {
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(p, "pos"), *size = cJSON_GetObjectItemCaseSensitive(p, "size");
        float cx = (float)cJSON_GetArrayItem(pos, 0)->valuedouble, cz = (float)cJSON_GetArrayItem(pos, 1)->valuedouble;
        float hx = (float)cJSON_GetArrayItem(size, 0)->valuedouble * 0.5f, hz = (float)cJSON_GetArrayItem(size, 1)->valuedouble * 0.5f;
        if (fabsf(x - cx) <= hx && fabsf(z - cz) <= hz) {
            in = true;
            if (level) *level = (float)cJSON_GetObjectItemCaseSensitive(p, "level")->valuedouble;
        }
    }
    return in;
}

static void test_footprint(void) {
    cJSON *lv = cJSON_Parse("{\"brushes\": [{\"type\": \"box\", \"pos\": [100, 0, 0], \"size\": [10, 3, 2], \"yaw\": 90}],"
                            " \"entities\": [{\"type\": \"light\", \"pos\": [-50, 3, 20]}], \"footprint_areas\": [[0, 200, 40, 210]], \"room\": {\"size\": [20, 4, 20]}}");
    cJSON *pads = cJSON_CreateArray();
    LevelGround g;
    level_ground_defaults(&g);
    g.level = 2.0f;
    int n = level_footprint_pads(lv, &g, pads);
    CHECK(n == cJSON_GetArraySize(pads) && n >= 4, "pads emitted (%d)", n);
    float lvl = 0;
    CHECK(in_pads(pads, 100, 4.9f, &lvl) && lvl == 2.0f, "a yawed box is covered along its rotated length");
    CHECK(in_pads(pads, 100 + 1 + g.margin - 0.1f, 0, NULL), "the margin is covered");
    CHECK(!in_pads(pads, 100 + 1 + g.margin + g.cell + 0.1f, 0, NULL), "beyond the margin is free");
    CHECK(in_pads(pads, -50, 20, NULL) && in_pads(pads, 20, 205, NULL), "entities and footprint areas covered");
    CHECK(!in_pads(pads, 50, 100, NULL), "the gaps between stay free");
    CHECK(in_pads(pads, 0, 0, &lvl) && fabsf(lvl - (2.0f - g.room_sink)) < 1e-4f, "the room's pad comes last, sunk under its floor (%.3f)", lvl);
    int paints = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, pads) paints += !strcmp(cJSON_GetObjectItemCaseSensitive(f, "type")->valuestring, "paint");
    CHECK(paints == 0, "no hard material: no paint (%d)", paints);
    cJSON_Delete(pads);
    cJSON_Delete(lv);
}

// "hard": the ground under structures standing on it painted, rotated with them, grown by the margin.
static void test_hard_paint(void) {
    cJSON *lv = cJSON_Parse("{\"brushes\": [{\"type\": \"box\", \"pos\": [100, 0, 0], \"size\": [10, 3, 2], \"yaw\": 90},"
                            " {\"type\": \"box\", \"pos\": [0, 0.9, 0], \"size\": [1, 1, 1]},"            // raised: a shelf item
                            " {\"type\": \"box\", \"pos\": [0, 0, 50], \"size\": [3, 0.05, 3]},"          // a marker plate
                            " {\"type\": \"stairs\", \"pos\": [-40, 0, 0], \"count\": 10, \"run\": 0.3, \"width\": 2}]}");
    cJSON *out = cJSON_CreateArray();
    LevelGround g;
    level_ground_defaults(&g);
    snprintf(g.hard_material, sizeof(g.hard_material), "rock");
    g.hard_min_size = 0.0f;   // exact rectangles here; the minimum below
    level_footprint_pads(lv, &g, out);
    int n = 0;
    float cx = 0, cz = 0, w = 0, d = 0, yaw = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, out) {
        if (strcmp(cJSON_GetObjectItemCaseSensitive(f, "type")->valuestring, "paint")) continue;
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(f, "material")->valuestring, "rock"), "paint material");
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(f, "pos"), *s = cJSON_GetObjectItemCaseSensitive(f, "size"), *y = cJSON_GetObjectItemCaseSensitive(f, "yaw");
        if (n == 0) {
            cx = (float)cJSON_GetArrayItem(p, 0)->valuedouble, cz = (float)cJSON_GetArrayItem(p, 1)->valuedouble;
            w = (float)cJSON_GetArrayItem(s, 0)->valuedouble, d = (float)cJSON_GetArrayItem(s, 1)->valuedouble;
            yaw = cJSON_IsNumber(y) ? (float)y->valuedouble : 0;
        } else {   // the stairs: from pos towards -Z, 3 m long
            CHECK(fabsf((float)cJSON_GetArrayItem(p, 0)->valuedouble + 40) < 1e-3f && fabsf((float)cJSON_GetArrayItem(p, 1)->valuedouble + 1.5f) < 1e-3f, "stairs paint centred behind their foot");
            CHECK(fabsf((float)cJSON_GetArrayItem(s, 1)->valuedouble - (3.0f + 2 * g.hard_margin)) < 1e-3f, "stairs paint length");
        }
        n++;
    }
    CHECK(n == 2, "a paint per structure on the ground, none for the raised box or the plate (%d)", n);
    CHECK(cx == 100 && cz == 0 && yaw == 90 && fabsf(w - (10 + 2 * g.hard_margin)) < 1e-3f && fabsf(d - (2 + 2 * g.hard_margin)) < 1e-3f, "the wall's paint: its rectangle + margin, rotated (%.1f x %.1f, yaw %.0f)", w, d, yaw);
    // the generator paints the wall's rotated rectangle: along Z at x = 100 (yaw 90 turns its length)
    static MapGen mg;
    mapgen_init(&mg, 1);
    char err[128];
    cJSON *world = cJSON_CreateObject();
    cJSON_AddNumberToObject(world, "half_size", 300);
    cJSON_AddItemToObject(world, "features", cJSON_Duplicate(out, true));
    CHECK(mapgen_parse(&mg, world, err, sizeof(err)), "world: %s", err);
    mapgen_finalize(&mg);
    u8 m;
    mapgen_height(&mg, 100.0f, 6.0f, &m);
    CHECK(m == MGM_ROCK, "inside the wall's length + margin: rock (%s)", mapgen_material_name(m));
    mapgen_height(&mg, 100.0f + 1.0f + g.hard_margin + 0.5f, 0.0f, &m);
    CHECK(m != MGM_ROCK, "beyond the margin across the wall: not rock (%s)", mapgen_material_name(m));
    mapgen_height(&mg, 0.0f, 50.0f, &m);
    CHECK(m != MGM_ROCK, "the marker plate: not rock (%s)", mapgen_material_name(m));
    mapgen_free(&mg);
    cJSON_Delete(world);
    cJSON_Delete(out);
    // min_size: a thin wall's paint is at least that wide, so the terrain blend map keeps it
    out = cJSON_CreateArray();
    g.hard_min_size = 6.0f;
    level_footprint_pads(lv, &g, out);
    bool wide = false;
    cJSON_ArrayForEach(f, out) {
        if (strcmp(cJSON_GetObjectItemCaseSensitive(f, "type")->valuestring, "paint")) continue;
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(f, "size");
        wide = cJSON_GetArrayItem(s, 0)->valuedouble >= 6.0 - 1e-3 && cJSON_GetArrayItem(s, 1)->valuedouble >= 6.0 - 1e-3;
        break;
    }
    CHECK(wide, "min_size widens the wall's paint");
    cJSON_Delete(out);
    cJSON_Delete(lv);
}

// ------------------------------------------------------------------ shipped data

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    size_t got = fread(s, 1, (size_t)n, f);
    s[got] = 0;
    fclose(f);
    return s;
}

static cJSON *read_data(const char *level, void *user) {
    (void)user;
    char path[256];
    snprintf(path, sizeof(path), "data/levels/%s.json", level);
    char *t = read_file(path);
    cJSON *r = t ? cJSON_Parse(t) : NULL;
    free(t);
    return r;
}

static void check_ground_mat(const MapGen *g, const MgRaster *r, float x, float z, const char *what, int want) {
    u8 mat;
    float h = mapgen_height(g, x, z, &mat);
    float d = fmaxf(fabsf(x), fabsf(z));   // the hall's floor box: 61 m square, its pad 8 cm low, 0.6 m blend
    (void)d;   // devroom_vegetation: no floor slab, no sunk room pad: the nature ground at y = 0 everywhere
    CHECK(fabsf(h) < 0.005f, "%s (%.1f, %.1f): ground %.3f", what, x, z, h);
    // the pads are grass; the hard structures' ground (and a margin around it) rock
    CHECK(want >= 0 ? mat == want : (mat == MGM_GRASS || mat == MGM_ROCK), "%s (%.1f, %.1f): material %s", what, x, z, mapgen_material_name(mat));
    CHECK(mapgen_tree_density(g, r, x, z, NULL, NULL) == 0.0f, "%s (%.1f, %.1f): trees", what, x, z);
}

static void check_ground(const MapGen *g, const MgRaster *r, float x, float z, const char *what) { check_ground_mat(g, r, x, z, what, -1); }

static void test_devroom_vegetation(void) {
    cJSON *dv = read_data("devroom_vegetation", NULL), *dr = read_data("devroom", NULL);
    if (!dv || !dr) { printf("data/levels not found (run from the exe folder): shipped-data checks skipped\n"); cJSON_Delete(dv); cJSON_Delete(dr); return; }
    char err[256];
    CHECK(level_include_resolve(dv, read_data, NULL, err, sizeof(err)), "devroom_vegetation: %s", err);
    static const char *keys[] = { "brushes", "entities", "labels", "spawns", "vehicles", "fuel_pumps", "vehicle_spikes", "water", "puddles", "post_volumes" };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
        CHECK(arr_n(dv, keys[k]) == arr_n(dr, keys[k]) && arr_n(dr, keys[k]) > 0, "%s: %d of the dev room's %d", keys[k], arr_n(dv, keys[k]), arr_n(dr, keys[k]));
    CHECK(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(dv, "room")) && cJSON_GetObjectItemCaseSensitive(dv, "weather"), "room and weather come along");
    const cJSON *world = cJSON_GetObjectItemCaseSensitive(dv, "world");
    static MapGen g;
    mapgen_init(&g, (u32)cJSON_GetObjectItemCaseSensitive(world, "seed")->valueint);
    CHECK(mapgen_parse(&g, world, err, sizeof(err)), "world: %s", err);
    mapgen_finalize(&g);
    static MgRaster r;
    mapgen_raster_alloc(&r, g.raster_half, 4.0f);
    mapgen_raster_rows(&g, &r, 0, r.n);
    mapgen_raster_slopes(&g, &r, 0, r.n);
    // every element's anchor, inside the playable area
    const cJSON *e;
    int n = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(dr, "brushes")) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "pos");
        float x = (float)cJSON_GetArrayItem(p, 0)->valuedouble, z = (float)cJSON_GetArrayItem(p, 2)->valuedouble;
        CHECK(fabsf(x) < g.half_size - 20 && fabsf(z) < g.half_size - 20, "brush at (%.0f, %.0f) inside the map", x, z);
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "type"), *s = cJSON_GetObjectItemCaseSensitive(e, "size"), *c = cJSON_GetObjectItemCaseSensitive(e, "collide");
        bool hard_box = (!cJSON_IsString(t) || !strcmp(t->valuestring, "box")) && cJSON_GetArrayItem(p, 1)->valuedouble <= 0.3 && cJSON_GetArraySize(s) >= 3 &&
                        cJSON_GetArrayItem(s, 1)->valuedouble >= 0.15 && !cJSON_IsFalse(c);
        check_ground_mat(&g, &r, x, z, "brush", hard_box ? MGM_ROCK : -1);   // a wall / block standing on the ground: rock under it
        n++;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(dr, "entities")) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "pos");
        check_ground(&g, &r, (float)cJSON_GetArrayItem(p, 0)->valuedouble, (float)cJSON_GetArrayItem(p, 2)->valuedouble, "entity");
        n++;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(dr, "footprint_areas")) {
        float x0 = (float)cJSON_GetArrayItem(e, 0)->valuedouble, z0 = (float)cJSON_GetArrayItem(e, 1)->valuedouble;
        float x1 = (float)cJSON_GetArrayItem(e, 2)->valuedouble, z1 = (float)cJSON_GetArrayItem(e, 3)->valuedouble;
        for (int i = 0; i <= 8; i++)
            for (int j = 0; j <= 8; j++) check_ground(&g, &r, x0 + (x1 - x0) * (float)i / 8.0f, z0 + (z1 - z0) * (float)j / 8.0f, "area");
        n++;
    }
    // named spots of CLAUDE.md's dev room list
    static const float spots[][2] = { { 0, 50 }, { -50, 22 }, { -140, 0 }, { -50, -11 }, { 104, 40 }, { 128, 40 }, { 32, -8 }, { 32, -40 }, { 80, -82 }, { 0, 24 } };
    for (size_t i = 0; i < sizeof(spots) / sizeof(spots[0]); i++) check_ground(&g, &r, spots[i][0], spots[i][1], "spot");
    // the vegetation is still there: trees in the forests, the pond holds water
    float trees = 0;
    for (int i = -40; i <= 40; i += 4) trees += mapgen_tree_density(&g, &r, -60.0f + (float)i, -330.0f, NULL, NULL);
    CHECK(trees > 0.0f, "the pine forest has trees");
    // The audio range's open field (docs/AUDIO_PROCEDURAL.md): nothing within 450 m of (-700, 300).
    for (int dz = -440; dz <= 440; dz += 40)
        for (int dx = -440; dx <= 440; dx += 40) {
            if (dx * dx + dz * dz > 440 * 440) continue;
            float x = -700.0f + (float)dx, z = 300.0f + (float)dz;
            u8 m;
            float h = mapgen_height(&g, x, z, &m);
            CHECK(fabsf(h) < 0.01f, "open field (%.0f, %.0f): ground %.3f", x, z, h);
            CHECK(mapgen_tree_density(&g, &r, x, z, NULL, NULL) == 0.0f, "open field (%.0f, %.0f): trees", x, z);
        }
    CHECK(mapgen_water_level(&g, 0, -205) > -100.0f && mapgen_height(&g, 0, -205, NULL) < mapgen_water_level(&g, 0, -205), "the pond holds water");
    printf("devroom_vegetation: %d dev room anchors on flat, clear ground\n", n);
    mapgen_raster_free(&r);
    mapgen_free(&g);
    cJSON_Delete(dv);
    cJSON_Delete(dr);
}

int main(void) {
    test_merge();
    test_footprint();
    test_hard_paint();
    test_devroom_vegetation();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
