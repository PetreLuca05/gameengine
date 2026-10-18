// Open-world generator unit tests (src/world/mapgen*.c, src/gameplay/loot.c, docs/ZOMBIE_MAP.md),
// on an open-world level (run from the exe folder, where data/ is mirrored; default
// data/levels/vegetation.json: determinism only). On the Zombieland map (branch zombieland,
// `test_mapgen data/levels/zombieland.json`) also the map's own checks:
// determinism of the raster; roads flatten the terrain to their profile and respect the class grade;
// bridges where roads cross the river, decks above the water; the river flows downhill; water levels
// (sea, lake, river, dry land); the ten POIs of the spec with enterable buildings; building lots clear
// of the carriageways; no trees on roads or in building footprints; player spawns in the suburbs on dry
// land; the map edge is mountains or sea; loot tables resolve to real items and every container's
// tier exists; loot rolls are deterministic.
#include "world/mapgen.h"
#include "world/mapgen_poi.h"
#include "gameplay/loot.h"
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

typedef struct Map { MapGen g; MapPois p; MgRaster r; } Map;

static bool build(Map *m, const cJSON *root, float spacing) {
    const cJSON *world = cJSON_GetObjectItemCaseSensitive(root, "world");
    const cJSON *seed = cJSON_GetObjectItemCaseSensitive(world, "seed");
    mapgen_init(&m->g, cJSON_IsNumber(seed) ? (u32)seed->valueint : 1);
    char err[256];
    if (!mapgen_parse(&m->g, world, err, sizeof(err))) { printf("parse: %s\n", err); return false; }
    if (!mappoi_plan(&m->p, &m->g, cJSON_GetObjectItemCaseSensitive(root, "pois"), err, sizeof(err))) { printf("pois: %s\n", err); return false; }
    mapgen_finalize(&m->g);
    mapgen_raster_alloc(&m->r, m->g.raster_half, spacing);
    mapgen_raster_rows(&m->g, &m->r, 0, m->r.n);
    mapgen_raster_slopes(&m->g, &m->r, 0, m->r.n);
    mappoi_build(&m->p, &m->g, &m->r);
    return true;
}

static void destroy(Map *m) {
    mapgen_raster_free(&m->r);
    mappoi_free(&m->p);
    mapgen_free(&m->g);
}

static u64 raster_hash(const MgRaster *r) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < (size_t)r->n * r->n; i++) {
        u32 v;
        memcpy(&v, &r->h[i], 4);
        h = (h ^ v) * 1099511628211ull;
        h = (h ^ r->mat[i]) * 1099511628211ull;
    }
    return h;
}

static u64 fnv_bytes(u64 h, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const u8 *)p)[i]) * 1099511628211ull;
    return h;
}

// The POI build (lots, primitives, entities: numeric fields only, no pointers / padding): equal on
// every compiler and platform (a Mac host and a PC client build the same map; docs/PLATFORM_MACOS.md
// "Determinism": no RNG draws in unsequenced argument lists).
static u64 poi_hash(const MapPois *p) {
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < p->lot_count; i++) {
        const MpLot *l = &p->lots[i];
        u32 k[5] = { l->kind, l->poi, l->storeys, l->flags, l->variant };
        h = fnv_bytes(h, k, sizeof(k));
        h = fnv_bytes(h, &l->pos, sizeof(vec2));
        h = fnv_bytes(h, &l->size, sizeof(vec2));
        h = fnv_bytes(h, &l->yaw, sizeof(float));
        h = fnv_bytes(h, &l->seed, sizeof(u32));
    }
    for (int i = 0; i < p->prim_count; i++) {
        const MpPrim *q = &p->prims[i];
        u32 k[4] = { q->color, q->flags, q->shape, q->poi };
        h = fnv_bytes(h, &q->pos, sizeof(vec3));
        h = fnv_bytes(h, &q->half, sizeof(vec3));
        h = fnv_bytes(h, &q->rot, sizeof(quat));
        h = fnv_bytes(h, k, sizeof(k));
    }
    for (int i = 0; i < p->ent_count; i++) {
        const MpEnt *e = &p->ents[i];
        float f[5] = { e->yaw, e->w, e->h, e->radius, e->intensity };
        h = fnv_bytes(h, &e->kind, 1);
        h = fnv_bytes(h, &e->pos, sizeof(vec3));
        h = fnv_bytes(h, f, sizeof(f));
        h = fnv_bytes(h, &e->count, sizeof(int));
    }
    return h;
}

static bool is_building(const char *kind) {
    static const char *const k[] = { "apartment", "shop", "office", "clinic", "police", "supermarket", "house", "hall", "warehouse", "admin", "workshop",
                                     "farmhouse", "barn", "hangar", "terminal", "harbor_office", "cabin", "plant" };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) if (!strcmp(kind, k[i])) return true;
    return false;
}

static void test_terrain_roads(Map *m) {
    const MapGen *g = &m->g;
    // roads: terrain = profile on the carriageway (except over water), grade limits
    int off = 0, steep = 0, samples = 0;
    float worst = 0;
    static const float grade[MGR_CLASS_COUNT] = { 0.08f, 0.10f, 0.12f, 0.16f, 0.30f, 0.01f };
    for (int r = 0; r < g->road_count; r++) {
        const MgRoad *rd = &g->roads[r];
        for (int i = 0; i < rd->count; i++) {
            const MgRoadSample *s = &g->rs[rd->first + i];
            if (s->bridge) continue;
            float h = mapgen_height(g, s->p.x, s->p.y, NULL);
            samples++;
            float d = fabsf(h - s->h);
            // junction areas: another road can own the height within its own width
            float rdst;
            int other = mapgen_nearest_road(g, s->p.x, s->p.y, 0.0f, &rdst);
            bool junction = other >= 0 && g->rs[other].road != r;
            if (d > 0.08f && !junction) {
                off++;
                worst = fmaxf(worst, d);
                if (getenv("MAPGEN_VERBOSE") && off < 40) printf("  off: %s (%.0f, %.0f) terrain %.2f profile %.2f\n", rd->name, s->p.x, s->p.y, h, s->h);
            }
            if (i > 0 && !g->rs[rd->first + i - 1].bridge && rd->cls == MGR_MAIN) {
                float dh = fabsf(s->h - g->rs[rd->first + i - 1].h);
                if (dh > grade[rd->cls] * MG_ROAD_STEP * 1.6f + 0.02f) steep++;
            }
        }
    }
    CHECK(samples > 5000, "road samples %d", samples);
    CHECK(off < samples / 100, "%d of %d road samples off their profile (worst %.2f m)", off, samples, worst);
    CHECK(steep == 0, "%d main road samples steeper than the grade", steep);
    printf("roads: %d roads, %d samples, %d off-profile (worst %.2f m)\n", g->road_count, samples, off, worst);
    // bridges: three crossings in this layout, decks above the water
    CHECK(g->bridge_count >= 3, "bridges %d", g->bridge_count);
    for (int b = 0; b < g->bridge_count; b++) {
        const MgBridge *br = &g->bridges[b];
        vec2 mid = v2_scale(v2_add(br->a, br->b), 0.5f);
        float wl = mapgen_water_level(g, mid.x, mid.y);
        CHECK(wl > -1e8f && br->h >= wl + 2.0f, "bridge %d deck %.1f over water %.1f", b, br->h, wl);
        CHECK(v2_len(v2_sub(br->b, br->a)) < 60.0f, "bridge %d is %.0f m long", b, v2_len(v2_sub(br->b, br->a)));
    }
    // the river flows downhill and ends at the sea level
    for (int r = 0; r < g->river_count; r++) {
        const MgRiver *rv = &g->rivers[r];
        int up = 0;
        for (int i = 1; i < rv->count; i++) up += g->vs[rv->first + i].water > g->vs[rv->first + i - 1].water + 1e-4f;
        CHECK(up == 0, "river %d: %d samples flow uphill", r, up);
        CHECK(fabsf(g->vs[rv->first + rv->count - 1].water - rv->end_level) < 0.6f, "river %d ends at %.2f", r, g->vs[rv->first + rv->count - 1].water);
    }
    // water: sea, lake, river, dry land
    CHECK(mapgen_water_level(g, 600, 950) == 0.0f, "sea south of the port: %.1f", mapgen_water_level(g, 600, 950));
    CHECK(fabsf(mapgen_water_level(g, -100, -600) - 12.5f) < 1e-3f, "lake level %.2f", mapgen_water_level(g, -100, -600));
    CHECK(mapgen_raster_height(&m->r, -100, -600) < 12.5f - 3.0f, "lake bed %.1f", mapgen_raster_height(&m->r, -100, -600));
    CHECK(mapgen_water_level(g, 0, -100) < -1e8f, "dry city centre");
    CHECK(mapgen_water_level(g, 318, -62) > 0.0f, "river at (318, -62)");
    // edge: west / north mountains, south-east sea
    int low = 0;
    for (int k = -18; k <= 18; k++) {
        float z = (float)k * 50.0f;
        if (z < 380 && mapgen_raster_height(&m->r, -990, z) < 60.0f) low++;          // west rim
        if (mapgen_raster_height(&m->r, (float)k * 50.0f, -990) < 55.0f) low++;      // north rim
    }
    CHECK(low == 0, "%d low points on the west / north edge", low);
    CHECK(mapgen_raster_height(&m->r, 990, 600) < 0.0f && mapgen_raster_height(&m->r, 0, 990) < 0.0f, "south-east edge is sea");
}

static void test_pois(Map *m) {
    const MapPois *p = &m->p;
    const char *ids[10] = { "city", "suburbs", "camp", "lake", "factory", "farms", "airfield", "forest", "port", "quarry" };
    // spec table (X east, Z north) -> engine (x, -z)
    const float spec[10][2] = { { 0, 100 }, { -550, 100 }, { -700, 650 }, { -100, 600 }, { 600, 650 }, { 600, 0 }, { -650, -550 }, { -100, -600 }, { 600, -650 }, { -700, -850 } };
    CHECK(p->poi_count == 10, "POIs %d", p->poi_count);
    for (int i = 0; i < 10; i++) {
        int found = -1;
        for (int k = 0; k < p->poi_count; k++) if (!strcmp(p->pois[k].id, ids[i])) found = k;
        CHECK(found >= 0, "POI %s missing", ids[i]);
        if (found < 0) continue;
        const MpPoi *q = &p->pois[found];
        float d = v2_len(v2_sub(q->center, v2(spec[i][0], -spec[i][1])));
        CHECK(d < 80.0f, "POI %s %.0f m from the spec position", ids[i], d);
        CHECK(q->enterable >= 2, "POI %s: %d enterable buildings", ids[i], q->enterable);
        CHECK(mappoi_find(p, q->center.x, q->center.y) == found, "POI %s region lookup", ids[i]);
    }
    CHECK(p->enterable_count >= 60, "enterable buildings %d", p->enterable_count);
    // density order of the spec: city highest, wild areas lowest
    float city = 0, forest = 1e9f;
    for (int k = 0; k < p->poi_count; k++) {
        if (!strcmp(p->pois[k].id, "city")) city = p->pois[k].zombie_density;
        if (!strcmp(p->pois[k].id, "forest") || !strcmp(p->pois[k].id, "camp")) forest = fminf(forest, p->pois[k].zombie_density);
    }
    for (int k = 0; k < p->poi_count; k++) CHECK(p->pois[k].zombie_density <= city, "%s denser than the city", p->pois[k].id);
    CHECK(forest < city * 0.5f, "forest / camp density %.1f vs city %.1f", forest, city);
    // building lots stay off the carriageways; no trees on roads or inside footprints
    int on_road = 0, buildings = 0;
    for (int i = 0; i < p->lot_count; i++) {
        const MpLot *l = &p->lots[i];
        if (!is_building(mappoi_lot_name(l->kind))) continue;
        buildings++;
        float c = cosf(l->yaw), s = sinf(l->yaw);
        for (int k = 0; k < 5; k++) {
            float lx = k == 4 ? 0 : (k & 1 ? 0.45f : -0.45f) * l->size.x, lz = k == 4 ? 0 : (k & 2 ? 0.45f : -0.45f) * l->size.y;
            float x = l->pos.x + lx * c + lz * s, z = l->pos.y - lx * s + lz * c;
            float d;
            int rs = mapgen_nearest_road(&m->g, x, z, 0.5f, &d);
            if (rs >= 0 && d < -0.5f) {
                on_road++;
                printf("  %s at (%.0f, %.0f) on road '%s'\n", mappoi_lot_name(l->kind), l->pos.x, l->pos.y, m->g.roads[m->g.rs[rs].road].name);
                break;
            }
            CHECK(mapgen_tree_density(&m->g, &m->r, x, z, NULL, NULL) == 0.0f, "tree density inside %s at (%.0f, %.0f)", mappoi_lot_name(l->kind), x, z);
        }
    }
    CHECK(buildings > 300, "buildings %d", buildings);
    CHECK(on_road == 0, "%d buildings on a carriageway", on_road);
    int tree_on_road = 0;
    for (int i = 0; i < m->g.rs_count; i += 7)
        if (mapgen_tree_density(&m->g, &m->r, m->g.rs[i].p.x, m->g.rs[i].p.y, NULL, NULL) > 0) tree_on_road++;
    CHECK(tree_on_road == 0, "%d road samples with trees", tree_on_road);
    // forests are dense, fields are not
    CHECK(mapgen_tree_density(&m->g, &m->r, -100, 560, NULL, NULL) > 0.01f || mapgen_tree_density(&m->g, &m->r, -60, 640, NULL, NULL) > 0.01f, "forest density");
    CHECK(mapgen_tree_density(&m->g, &m->r, 600, 40, NULL, NULL) == 0.0f || mapgen_raster_mat(&m->r, 600, 40) == MGM_GRASS, "no trees in the fields");
    // entities: spawns in the suburbs on dry land, containers / zombie spots / doors / vehicles
    int spawns = 0, cont = 0, spots = 0, doors = 0, veh = 0, locked = 0;
    for (int i = 0; i < p->ent_count; i++) {
        const MpEnt *e = &p->ents[i];
        if (e->kind == MPE_SPAWN) {
            spawns++;
            CHECK(mappoi_find(p, e->pos.x, e->pos.z) >= 0 && !strcmp(p->pois[mappoi_find(p, e->pos.x, e->pos.z)].kind, "suburbs"), "spawn outside the suburbs");
            CHECK(mapgen_water_level(&m->g, e->pos.x, e->pos.z) < e->pos.y - 1.0f, "spawn in water");
        }
        cont += e->kind == MPE_CONTAINER;
        spots += e->kind == MPE_ZOMBIE_SPOT;
        doors += e->kind == MPE_DOOR;
        locked += e->kind == MPE_DOOR && e->locked && e->item[0];
        veh += e->kind == MPE_VEHICLE;
    }
    CHECK(spawns >= 4, "spawns %d", spawns);
    CHECK(cont > 200 && cont < 1500, "containers %d", cont);
    CHECK(spots > 100, "zombie spots %d", spots);
    CHECK(locked >= 3, "locked doors with keys %d", locked);
    CHECK(veh >= 3, "vehicles %d", veh);
    printf("pois: %d POIs, %d lots (%d buildings, %d enterable), %d prims, %d containers, %d zombie spots, %d doors, %d vehicles, %d spawns\n", p->poi_count, p->lot_count,
           buildings, p->enterable_count, p->prim_count, cont, spots, doors, veh, spawns);
}

static void test_loot(const Map *m) {
    char *lt = read_file("data/loot.json");
    char *it = read_file("data/items.json");
    CHECK(lt && it, "data/loot.json, data/items.json readable (run from the exe folder)");
    if (!lt || !it) return;
    LootTable t;
    char err[128];
    CHECK(loot_parse(&t, lt, err, sizeof(err)), "loot: %s", err);
    cJSON *items = cJSON_Parse(it);
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(items, "items"), *e;
    // every entry is a real item
    for (int i = 0; i < t.n; i++)
        for (int k = 0; k < t.tiers[i].n; k++) {
            bool found = false;
            cJSON_ArrayForEach(e, arr) if (!strcmp(cJSON_GetObjectItemCaseSensitive(e, "id")->valuestring, t.tiers[i].e[k].item)) found = true;
            CHECK(found, "loot tier %s: unknown item %s", t.tiers[i].name, t.tiers[i].e[k].item);
        }
    // every container tier exists
    int missing = 0;
    for (int i = 0; i < m->p.ent_count; i++)
        if (m->p.ents[i].kind == MPE_CONTAINER && !loot_tier(&t, m->p.ents[i].tier)) { missing++; if (missing < 4) printf("  tier '%s' missing\n", m->p.ents[i].tier); }
    CHECK(missing == 0, "%d containers with an unknown tier", missing);
    // deterministic rolls, non-empty on average, merged duplicates
    LootItem a[12], b[12];
    int filled = 0, total = 0;
    for (u32 s = 0; s < 400; s++) {
        int na = loot_roll(&t, "medical", s, a, 12), nb = loot_roll(&t, "medical", s, b, 12);
        CHECK(na == nb, "roll %u not deterministic", s);
        for (int k = 0; k < na && k < nb; k++) CHECK(!strcmp(a[k].item, b[k].item) && a[k].count == b[k].count, "roll %u item %d differs", s, k);
        for (int k = 0; k < na; k++)
            for (int q = k + 1; q < na; q++) CHECK(strcmp(a[k].item, a[q].item), "roll %u: duplicate %s", s, a[k].item);
        filled += na > 0;
        total += na;
    }
    CHECK(filled > 300, "medical containers filled %d / 400", filled);
    printf("loot: %d tiers, medical: %d / 400 filled, %.1f items each\n", t.n, filled, (float)total / (float)(filled ? filled : 1));
    cJSON_Delete(items);
    free(lt);
    free(it);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "data/levels/vegetation.json";
    char *text = read_file(path);
    if (!text) { printf("cannot read %s (run from the exe folder)\n", path); return 1; }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) { printf("JSON parse error\n"); return 1; }
    static Map a, b;
    if (!build(&a, root, 2.0f) || !build(&b, root, 2.0f)) return 1;
    CHECK(raster_hash(&a.r) == raster_hash(&b.r), "raster not deterministic");
    CHECK(a.p.prim_count == b.p.prim_count && a.p.ent_count == b.p.ent_count, "POI build not deterministic");
    CHECK(poi_hash(&a.p) == poi_hash(&b.p), "POI build not deterministic");
    printf("raster hash %016llx, POI hash %016llx (%d lots, %d prims, %d ents)\n", (unsigned long long)raster_hash(&a.r), (unsigned long long)poi_hash(&a.p),
           a.p.lot_count, a.p.prim_count, a.p.ent_count);
    destroy(&b);
    // The terrain / road / POI / loot checks are written against the Zombieland map's layout.
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(nm) && !strcmp(nm->valuestring, "Zombieland")) {
        test_terrain_roads(&a);
        test_pois(&a);
        test_loot(&a);
    } else {
        CHECK(a.p.prim_count >= 0 && raster_hash(&a.r) != 0, "%s: raster built", path);
    }
    // Cross-platform golden of the shipped open-world map (Linux x86_64; CI: Windows MSVC, macOS arm64 /
    // x86_64): every peer generates the terrain itself (docs/PLATFORM_MACOS.md "Determinism").
    if (!strcmp(path, "data/levels/vegetation.json")) {
        const u64 golden = 0xA690DD81FE64036Cull;
        CHECK(raster_hash(&a.r) == golden, "vegetation raster %016llx differs from the golden %016llx", (unsigned long long)raster_hash(&a.r), (unsigned long long)golden);
    }
    destroy(&a);
    cJSON_Delete(root);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
