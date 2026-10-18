// Building kit tests (docs/BUILDING_KIT.md): every shipped piece in assets/kits/building passes the
// validator; each broken fixture in tests/data/kit_bad (tools/kit_gen/kit_bad_fixtures.py) is rejected with
// its own message; sockets / hulls / LODs / UV sets load; prefab expansion (nested transforms, name
// prefixes, overrides, missing prefabs, cycles).
#include "assets/kit_piece.h"
#include "world/kit_layout.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

static const char *SLOTS = "M_paint\nM_wood\nM_metal\nM_glass\nM_render\nM_concrete\nM_trim\n";

static int validate_file(const char *dir, const char *id, const KitRules *r, char *out, int size, KitPiece *keep) {
    char path[300], err[200];
    snprintf(path, sizeof(path), "%s/%s.glb", dir, id);
    KitPiece *p = calloc(1, sizeof(KitPiece));
    if (!kit_piece_load(p, path, id, err, sizeof(err))) {
        snprintf(out, (size_t)size, "load failed: %s", err);
        free(p);
        return -1;
    }
    int n = kit_piece_validate(p, r, SLOTS, out, size);
    if (keep) *keep = *p;
    else kit_piece_free(p);
    free(p);
    return n;
}

static void test_fixtures(void) {
    static const struct { const char *id; const char *category; const char *expect; } cases[] = {
        { "bad_pivot", "wall", "pivot off grid" },
        { "bad_scale_cm", "wall", "scale" },
        { "BadName", "wall", "lower case" },
        { "bad_slot", "wall", "material slot 'Material.001' unmapped" },
        { "bad_unnamed", "wall", "unknown name ('Cube')" },
        { "bad_hull", "wall", "not convex" },
        { "bad_nouv1", "wall", "no second UV set" },
        { "bad_density", "wall", "texel density" },
        { "bad_lod", "wall", "LOD1 has" },
        { "bad_socket", "door_leaf", "socket S_hinge missing" },
        { "bad_nocollision", "wall", "no collision" },
    };
    char out[4096];
    KitRules r;
    kit_rules_default(&r);
    snprintf(r.category, sizeof(r.category), "wall");
    r.collision_required = true;
    r.grid = 0.05f;
    int n = validate_file("tests/data/kit_bad", "good_wall_100", &r, out, sizeof(out), NULL);
    CHECK(n == 0, "the good fixture passes (%d):\n%s", n, out);
    for (int i = 0; i < ARRAY_COUNT(cases); i++) {
        KitRules cr = r;
        snprintf(cr.category, sizeof(cr.category), "%s", cases[i].category);
        if (!strcmp(cases[i].category, "door_leaf")) {
            snprintf(cr.required_sockets[0], 24, "hinge");
            snprintf(cr.required_sockets[1], 24, "handle");
            cr.required_socket_n = 2;
            cr.grid = 0;
        }
        n = validate_file("tests/data/kit_bad", cases[i].id, &cr, out, sizeof(out), NULL);
        CHECK(n > 0 && strstr(out, cases[i].expect), "%s rejected with '%s' (%d):\n%s", cases[i].id, cases[i].expect, n, out);
    }
}

static void test_shipped(void) {
    char *kt = read_file("assets/kits/building/kit.json");
    cJSON *kit = kt ? cJSON_Parse(kt) : NULL;
    free(kt);
    CHECK(kit != NULL, "assets/kits/building/kit.json readable (run from the build dir or the repo root)");
    if (!kit) return;
    const cJSON *pieces = cJSON_GetObjectItemCaseSensitive(kit, "pieces"), *cats = cJSON_GetObjectItemCaseSensitive(kit, "categories");
    // Material slots from the material sets the manifest's pieces may use.
    char slots[2048] = "";
    static const char *const sets[] = { "paint", "wallpaper", "wood", "tile", "concrete", "brick", "stone", "render", "glass", "metal", "trim", "fabric", "plastic", "carpet", "emissive" };
    for (int i = 0; i < ARRAY_COUNT(sets); i++) {
        char path[128];
        snprintf(path, sizeof(path), "assets/kits/building/materials/%s.json", sets[i]);
        char *t = read_file(path);
        CHECK(t != NULL, "material set %s exists", sets[i]);
        free(t);
        strcat(slots, "M_");
        strcat(slots, sets[i]);
        strcat(slots, "\n");
    }
    int count = 0, bad = 0;
    char out[4096];
    const cJSON *pe;
    cJSON_ArrayForEach(pe, pieces) {
        KitRules r;
        kit_rules_default(&r);
        const char *cat = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pe, "category"));
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(cats, cat ? cat : "");
        CHECK(c != NULL, "%s: category '%s' defined", pe->string, cat ? cat : "?");
        snprintf(r.category, sizeof(r.category), "%s", cat ? cat : "");
        r.collision_required = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "collision"));
        const cJSON *v;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(c, "max_tris"))) r.max_tris = v->valueint;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(c, "grid"))) r.grid = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(c, "grid_height"))) r.grid_height = (float)v->valuedouble;
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(c, "sockets")) snprintf(r.required_sockets[r.required_socket_n++], 24, "%s", v->valuestring);
        char path[300], err[200];
        snprintf(path, sizeof(path), "assets/kits/building/%s.glb", pe->string);
        KitPiece *p = calloc(1, sizeof(KitPiece));
        bool ok = kit_piece_load(p, path, pe->string, err, sizeof(err));
        int n = ok ? kit_piece_validate(p, &r, slots, out, sizeof(out)) : -1;
        if (n) { bad++; printf("%s", ok ? out : err); }
        // An entity hook names a socket the piece has.
        const cJSON *ent = cJSON_GetObjectItemCaseSensitive(pe, "entity");
        const char *sock = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ent, "socket"));
        if (ok && sock) CHECK(kit_piece_socket(p, sock, 0) != NULL, "%s: entity socket S_%s exists", pe->string, sock);
        kit_piece_free(p);
        free(p);
        count++;
    }
    CHECK(count >= 200 && bad == 0, "%d shipped pieces, %d with problems", count, bad);
    // Spot checks of what loads.
    KitPiece *p = calloc(1, sizeof(KitPiece));
    char err[200];
    CHECK(kit_piece_load(p, "assets/kits/building/wall_int_door_090.glb", "wall_int_door_090", err, sizeof(err)), "door wall loads: %s", err);
    CHECK(fabsf(p->mx[0] - 1.2f) < 1e-3f && fabsf(p->mx[1] - 2.7f) < 1e-3f && fabsf(p->mn[2] + 0.2f) < 1e-3f, "door wall 1.2 x 2.7 x 0.2");
    const KitSocket *s = kit_piece_socket(p, "open", 0);
    CHECK(s && fabsf(s->pos[0] - 0.6f) < 1e-4f && fabsf(s->pos[2] + 0.1f) < 1e-4f, "S_open at the opening's bottom centre");
    CHECK(p->hull_n == 3 && p->hulls[0].box, "3 box hulls (jambs + lintel), %d", p->hull_n);
    CHECK(p->lods[0].slot_n == 1 && p->lods[0].slots[0].uv1 != NULL, "one slot with UV1");
    kit_piece_free(p);
    CHECK(kit_piece_load(p, "assets/kits/building/glass_slider_leaf_075.glb", "glass_slider_leaf_075", err, sizeof(err)), "slider leaf loads");
    CHECK(p->lods[0].slot_n == 2 && kit_piece_socket(p, "hinge", 0) && kit_piece_socket(p, "handle", 0), "glass + metal slots, hinge + handle sockets");
    kit_piece_free(p);
    CHECK(kit_piece_load(p, "assets/kits/building/blinds_venetian_150.glb", "blinds_venetian_150", err, sizeof(err)) && p->lod_n == 2 && p->lods[1].tris < p->lods[0].tris,
          "venetian blinds: LOD1 drops slats (%u -> %u)", p->lods[0].tris, p->lods[1].tris);
    kit_piece_free(p);
    free(p);
    cJSON_Delete(kit);
}

// ------------------------------------------------------------------ layout

static char *loader(void *user, const char *name) {
    const char *const *db = user;
    for (int i = 0; db[i]; i += 2)
        if (!strcmp(db[i], name)) {
            size_t n = strlen(db[i + 1]);
            char *t = malloc(n + 1);
            memcpy(t, db[i + 1], n + 1);
            return t;
        }
    return NULL;
}

static const KitPlaced *find(const KitLayout *L, const char *name) {
    for (int i = 0; i < L->n; i++) {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(L->items[i].entry, "name"));
        if (n && !strcmp(n, name)) return &L->items[i];
    }
    return NULL;
}

static void test_layout(void) {
    static const char *const db[] = {
        "room", "{\"pieces\":[{\"piece\":\"wall_int_300\",\"pos\":[1,0,0],\"name\":\"w\"},"
                "{\"piece\":\"door_frame_int_090\",\"pos\":[0,0,-2],\"yaw\":90,\"name\":\"door\",\"locked\":false},"
                "{\"prefab\":\"lamp\",\"pos\":[2,2.7,-2],\"prefix\":\"l_\"}]}",
        "lamp", "{\"pieces\":[{\"piece\":\"light_downlight\",\"name\":\"a\",\"group\":3}]}",
        "loop", "{\"pieces\":[{\"prefab\":\"loop\"}]}",
        NULL,
    };
    cJSON *level = cJSON_Parse("{\"kit\":[{\"piece\":\"floor_slab_100x100\",\"pos\":[0,-0.3,0]}],"
                               "\"prefabs\":[{\"prefab\":\"room\",\"pos\":[10,30,5],\"yaw\":90,\"prefix\":\"u1_\",\"override\":{\"door\":{\"locked\":true}}},"
                               "{\"prefab\":\"room\",\"pos\":[0,30,0]},{\"prefab\":\"nope\"},{\"prefab\":\"loop\"}]}");
    KitLayout L;
    kit_layout_build(&L, level, loader, (void *)db);
    CHECK(L.n == 1 + 2 * 3 + KIT_LAYOUT_DEPTH * 0, "placed pieces: %d", L.n);
    CHECK(L.errors == 2 && strstr(L.err, "nope"), "missing prefab + cycle reported (%d): %s", L.errors, L.err);
    // u1_ room at (10, 30, 5) turned 90 deg: the wall's local (1, 0, 0) -> x' = cos90 = 0, z' = -sin90 = -1.
    const KitPlaced *w = find(&L, "u1_w");
    CHECK(w && fabsf(w->pos[0] - 10.0f) < 1e-4f && fabsf(w->pos[1] - 30.0f) < 1e-4f && fabsf(w->pos[2] - 4.0f) < 1e-4f && w->yaw == 90.0f,
          "nested transform (%.2f %.2f %.2f yaw %.0f)", w ? w->pos[0] : 0, w ? w->pos[1] : 0, w ? w->pos[2] : 0, w ? w->yaw : 0);
    const KitPlaced *d = find(&L, "u1_door"), *d0 = find(&L, "door");
    CHECK(d && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d->entry, "locked")) && d->yaw == 180.0f, "override applied, yaws add");
    CHECK(d0 && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(d0->entry, "locked")), "the other instance keeps its value");
    const KitPlaced *l = find(&L, "u1_l_a");
    // lamp at room-local (2, 2.7, -2) rotated 90: x' = 2c + (-2)s = -2, z' = -2s + (-2)c = -2 -> (8, 32.7, 3)
    CHECK(l && fabsf(l->pos[0] - 8.0f) < 1e-4f && fabsf(l->pos[1] - 32.7f) < 1e-4f && fabsf(l->pos[2] - 3.0f) < 1e-4f && l->prefab_depth == 2,
          "nested prefix + transform (%.2f %.2f %.2f)", l ? l->pos[0] : 0, l ? l->pos[1] : 0, l ? l->pos[2] : 0);
    CHECK(find(&L, "l_a") != NULL, "second instance without an outer prefix");
    kit_layout_free(&L);
    cJSON_Delete(level);
    float x, z;
    kit_rotate_xz(90, 0, -1, &x, &z);
    CHECK(fabsf(x + 1.0f) < 1e-5f && fabsf(z) < 1e-5f, "yaw 90 turns forward (-Z) to -X like quat_yaw / forward_from_yaw");
}

int main(void) {
    test_fixtures();
    test_shipped();
    test_layout();
    printf("test_kit: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
