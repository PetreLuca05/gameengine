// VFX core tests (pure, no GPU): curves, seeded emitter determinism, flipbook meta parsing,
// atlas packing, decal budget, one flash per shot at 1200 RPM, impact effect selection for every
// surface tag, and loading of the shipped assets/vfx set. Run from the build output directory.
#include "vfx/vfx_curve.h"
#include "vfx/vfx_library.h"
#include "vfx/vfx_sim.h"
#include "vfx/vfx_atlas.h"
#include "core/engine.h"
#include "core/log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
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

static bool nearf(float a, float b, float e) { return fabsf(a - b) <= e; }

static void test_curves(void) {
    VfxLibrary *lib = (VfxLibrary *)calloc(1, sizeof(VfxLibrary));
    const char *json =
        "{ \"name\": \"c\", \"emitters\": [ { \"name\": \"e\", \"update\": {"
        "  \"size_curve\": [[0, 1], [0.5, 3], [1, 2]],"
        "  \"color_curve\": [[0, 1, 0, 0, 1], [1, 0, 0, 1, 0]],"
        "  \"velocity_curve\": { \"type\": \"bezier\", \"keys\": [[0, 0], [1, 1]] },"
        "  \"spin_curve\": 0.5 } } ] }";
    int fx = vfx_lib_add_effect_json(lib, json, "test");
    CHECK(fx == 0, "effect parsed");
    const VfxUpdateDef *u = &lib->emitters[lib->effects[fx].emitters[0]].update;
    CHECK(nearf(vfx_curve_eval1(&u->size_curve, 0.0f), 1, 1e-6f), "size t=0");
    CHECK(nearf(vfx_curve_eval1(&u->size_curve, 0.25f), 2, 1e-6f), "size t=0.25 (%f)", vfx_curve_eval1(&u->size_curve, 0.25f));
    CHECK(nearf(vfx_curve_eval1(&u->size_curve, 0.75f), 2.5f, 1e-6f), "size t=0.75");
    CHECK(nearf(vfx_curve_eval1(&u->size_curve, 2.0f), 2, 1e-6f), "size clamps past 1");
    CHECK(nearf(vfx_curve_eval1(&u->size_curve, -1.0f), 1, 1e-6f), "size clamps before 0");
    vec4 c = vfx_curve_eval(&u->color_curve, 0.5f);
    CHECK(nearf(c.x, 0.5f, 1e-6f) && nearf(c.z, 0.5f, 1e-6f) && nearf(c.w, 0.5f, 1e-6f), "color lerp");
    // Bezier with auto tangents through (0,0)-(1,1): straight line, smooth.
    CHECK(nearf(vfx_curve_eval1(&u->velocity_curve, 0.5f), 0.5f, 1e-5f), "bezier midpoint %f", vfx_curve_eval1(&u->velocity_curve, 0.5f));
    float prev = -1;
    bool mono = true;
    for (int i = 0; i <= 100; i++) { float v = vfx_curve_eval1(&u->velocity_curve, i / 100.0f); if (v < prev - 1e-6f) mono = false; prev = v; }
    CHECK(mono, "bezier monotonic");
    CHECK(nearf(vfx_curve_eval1(&u->spin_curve, 0.3f), 0.5f, 1e-6f), "constant curve");
    vec4 lut[VFX_LUT_SAMPLES];
    vfx_curve_bake(&u->size_curve, lut);
    CHECK(nearf(lut[0].x, 1, 1e-6f) && nearf(lut[VFX_LUT_SAMPLES - 1].x, 2, 1e-6f), "LUT ends");
    // Explicit Hermite tangents: [t, v, in, out].
    const char *bz = "{ \"name\": \"b\", \"emitters\": [ { \"name\": \"e\", \"update\": { \"size_curve\": { \"type\": \"bezier\", \"keys\": [[0, 0, 0, 0], [1, 1, 0, 0]] } } } ] }";
    int fb = vfx_lib_add_effect_json(lib, bz, "test2");
    const VfxCurve *sc = &lib->emitters[lib->effects[fb].emitters[0]].update.size_curve;
    CHECK(nearf(vfx_curve_eval1(sc, 0.5f), 0.5f, 1e-5f) && vfx_curve_eval1(sc, 0.1f) < 0.1f, "ease-in-out hermite (%f)", vfx_curve_eval1(sc, 0.1f));
    free(lib);
}

static const char *k_det_effect =
    "{ \"name\": \"det\", \"duration\": 12, \"emitters\": [ {"
    "  \"name\": \"sparks\", \"tier\": \"cpu\", \"deterministic\": true,"
    "  \"spawn\": { \"mode\": \"rate\", \"rate\": 120, \"duration\": 12, \"shape\": \"sphere\", \"radius\": 0.2, \"dir\": \"up\", \"angle\": 40,"
    "             \"speed\": [2, 6], \"life\": [0.5, 2.0], \"size\": [0.01, 0.03], \"spin\": [-90, 90] },"
    "  \"update\": { \"gravity\": 1, \"drag\": 0.5, \"turbulence\": 2, \"collide\": \"physics\", \"bounce\": 0.4 },"
    "  \"render\": { \"mode\": \"add\" } } ] }";

static bool floor_ray(void *ctx, vec3 from, vec3 d, vec3 *hit, vec3 *n, int *mat) {
    (void)ctx;
    if (from.y + d.y >= 0 || d.y >= 0) return false;
    float t = -from.y / d.y;
    if (t < 0 || t > 1) return false;
    *hit = v3_madd(from, d, t);
    *n = v3(0, 1, 0);
    *mat = SURF_CONCRETE;
    return true;
}

static u32 run_det(VfxLibrary *lib, u32 seed, int ticks, u32 *mid_hash) {
    VfxSystem *s = (VfxSystem *)calloc(1, sizeof(VfxSystem));
    vfx_sys_init(s, lib);
    s->raycast = floor_ray;
    s->cfg.camera = v3(0, 2, 5);
    VfxSpawnParams p = vfx_spawn_params(v3(0, 1, 0));
    p.seed = seed;
    vfx_sys_spawn(s, vfx_lib_find_effect(lib, "det"), &p);
    for (int t = 0; t < ticks; t++) {
        vfx_sys_tick(s, 1.0f / 60.0f);
        if (t == ticks / 2 && mid_hash) *mid_hash = vfx_sys_cpu_hash(s);
    }
    u32 h = vfx_sys_cpu_hash(s);
    int alive = s->stats.cpu_particles;
    free(s);
    return h ^ (u32)alive;
}

static void test_determinism(void) {
    VfxLibrary *lib = (VfxLibrary *)calloc(1, sizeof(VfxLibrary));
    vfx_lib_add_effect_json(lib, k_det_effect, "det");
    vfx_lib_resolve(lib);
    u32 m1 = 0, m2 = 0, m3 = 0;
    u32 a = run_det(lib, 0xC0FFEEu, 600, &m1);
    u32 b = run_det(lib, 0xC0FFEEu, 600, &m2);
    u32 c = run_det(lib, 0xBADF00Du, 600, &m3);
    CHECK(m1 != 2166136261u && a != (2166136261u ^ 0u), "particles alive at mid / end");
    CHECK(a == b && m1 == m2, "seeded deterministic emitter: identical particle state over 600 ticks (%08x vs %08x)", a, b);
    CHECK(a != c, "different seeds diverge");
    printf("  determinism: 600 ticks, state hash %08x (mid %08x)\n", a, m1);
    free(lib);
}

static void test_meta(void) {
    VfxTextureMeta m;
    const char *j = "{ \"file\": \"smoke.png\", \"width\": 2048, \"height\": 2048, \"grid\": [8, 8], \"frames\": 60, \"fps\": 30,"
                    " \"loop\": false, \"motion_vectors\": true, \"lighting\": \"sixway\", \"variants\": 1, \"placeholder\": true }";
    CHECK(vfx_lib_parse_texture_meta(&m, j, "smoke"), "meta parses");
    CHECK(m.cols == 8 && m.rows == 8 && m.frames == 60 && nearf(m.fps, 30, 0) && !m.loop, "grid / frames / fps / loop");
    CHECK(m.motion_vectors && m.lighting == VFX_LIGHT_SIXWAY && m.placeholder && !strcmp(m.file, "smoke.png"), "mv / lighting / placeholder / file");
    CHECK(vfx_lib_parse_texture_meta(&m, "{ }", "plain") && m.cols == 1 && m.frames == 1 && !strcmp(m.file, "plain.png"), "defaults");
    CHECK(!vfx_lib_parse_texture_meta(&m, "{ broken", "x"), "bad JSON rejected");
}

static void test_atlas(void) {
    VfxAtlasRect r[64];
    u32 st = 7;
    int n = 0;
    for (int i = 0; i < 40; i++, n++) {
        r[i].w = 64 << (int)(vfx_rand01(&st) * 4.0f);
        r[i].h = 64 << (int)(vfx_rand01(&st) * 4.0f);
    }
    int pages = vfx_atlas_pack(r, n, 2048, 2);
    CHECK(pages > 0, "packs (%d pages)", pages);
    bool inside = true, overlap = false;
    for (int i = 0; i < n; i++) {
        if (!r[i].placed || r[i].x < 0 || r[i].y < 0 || r[i].x + r[i].w > 2048 || r[i].y + r[i].h > 2048) inside = false;
        for (int k = i + 1; k < n; k++) {
            if (r[i].page != r[k].page) continue;
            if (r[i].x < r[k].x + r[k].w && r[k].x < r[i].x + r[i].w && r[i].y < r[k].y + r[k].h && r[k].y < r[i].y + r[i].h) overlap = true;
        }
    }
    CHECK(inside, "all rects inside their page");
    CHECK(!overlap, "no overlaps");
    VfxAtlasRect big = { 4096, 16, 0, 0, 0, false };
    CHECK(vfx_atlas_pack(&big, 1, 2048, 0) < 0, "oversized rect rejected");
    printf("  atlas: 40 rects in %d pages of 2048^2\n", pages);
}

static void test_decal_budget(void) {
    VfxLibrary *lib = (VfxLibrary *)calloc(1, sizeof(VfxLibrary));
    vfx_lib_add_effect_json(lib, "{ \"name\": \"hole\", \"decal\": { \"texture\": \"x\", \"lifetime\": 1000 }, \"emitters\": [] }", "hole");
    vfx_lib_resolve(lib);
    VfxSystem *s = (VfxSystem *)calloc(1, sizeof(VfxSystem));
    vfx_sys_init(s, lib);
    s->cfg.decal_max = 64;
    int worst = 0;
    for (int i = 0; i < 1000; i++) {
        VfxSpawnParams p = vfx_spawn_params(v3((float)(i % 30), 1, 0));
        p.decal_ok = true;
        vfx_sys_spawn(s, 0, &p);
        vfx_sys_tick(s, 1.0f / 60.0f);
        int live = 0;
        for (int k = 0; k < VFX_MAX_DECALS_HARD; k++) live += s->decals[k].alive;
        if (live > worst) worst = live;
    }
    CHECK(worst <= 64, "decal budget never exceeded (max %d of 64)", worst);
    CHECK(s->decal_count == 64, "pool full at the budget (%d)", s->decal_count);
    // The newest decal survives eviction (oldest recycled first).
    u32 newest = s->decal_serial;
    bool found = false;
    for (int k = 0; k < VFX_MAX_DECALS_HARD; k++) if (s->decals[k].alive && s->decals[k].serial == newest) found = true;
    CHECK(found, "newest decal kept");
    VfxSpawnParams p = vfx_spawn_params(v3(0, 0, 0));
    p.decal_ok = false;
    u32 before = s->decal_serial;
    vfx_sys_spawn(s, 0, &p);
    CHECK(s->decal_serial == before, "no decal on non-decal surfaces (characters)");
    free(s);
    free(lib);
}

static void test_flash_per_shot(VfxLibrary *lib, const char *effect_name) {
    int fx = vfx_lib_find_effect(lib, effect_name);
    CHECK(fx >= 0, "%s exists", effect_name);
    if (fx < 0) return;
    VfxSystem *s = (VfxSystem *)calloc(1, sizeof(VfxSystem));
    vfx_sys_init(s, lib);
    s->cfg.camera = v3(0, 1.6f, 0);
    // 1200 RPM = 20 shots / s = one shot every 3 ticks.
    int shots = 0, max_alive = 0, repeats = 0, core_k = -1;
    const VfxEffectDef *e = &lib->effects[fx];
    for (int k = 0; k < e->emitter_count; k++) if (!strcmp(lib->emitters[e->emitters[k]].name, "core")) core_k = k;
    CHECK(core_k >= 0, "%s has a core emitter", effect_name);
    int last_variant = -2;
    u32 records_core = 0;
    for (int t = 0; t < 600; t++) {
        s->spawn_count = 0;
        if (t % 3 == 0) {
            VfxSpawnParams p = vfx_spawn_params(v3(0, 1.5f, 0));
            p.viewmodel = true;
            vfx_sys_spawn(s, fx, &p);
            shots++;
        }
        vfx_sys_tick(s, 1.0f / 60.0f);
        int alive = 0;
        for (int i = 0; i < VFX_MAX_INSTANCES; i++) alive += s->inst[i].active && s->inst[i].effect == fx;
        if (alive > max_alive) max_alive = alive;
        for (int i = 0; i < s->spawn_count; i++) {
            const VfxGpuSpawn *g = &s->spawns[i];
            if ((int)g->info[0] != lib->effects[fx].emitters[core_k]) continue;
            records_core++;
            int v = (int)(g->info[3] & 0xFFF);
            if (v == last_variant) repeats++;
            last_variant = v;
        }
    }
    CHECK(s->stats.flashes_spawned == (u32)shots, "%s: one core flash per shot (%u flashes, %d shots)", effect_name, s->stats.flashes_spawned, shots);
    CHECK(records_core == (u32)shots, "%s: one GPU core record per shot (%u)", effect_name, records_core);
    CHECK(repeats == 0, "%s: never the same variant twice in a row (%d repeats)", effect_name, repeats);
    CHECK(max_alive <= 4, "%s: no piling (max %d live instances at 1200 RPM)", effect_name, max_alive);
    free(s);
}

static void test_assets(void) {
    VfxLibrary *lib = (VfxLibrary *)calloc(1, sizeof(VfxLibrary));
    bool ok = vfx_lib_load(lib, "assets/vfx");
    CHECK(ok && lib->load_errors == 0, "assets/vfx loads (%d effects, %d errors)", lib->effect_count, lib->load_errors);
    // Every surface tag selects its own impact effect.
    for (int m = 0; m < SURF_COUNT; m++) {
        int e = vfx_lib_impact_effect(lib, m, false);
        CHECK(e >= 0, "impact for %s", surface_name(m));
        if (e < 0) continue;
        char want[64];
        snprintf(want, sizeof(want), "impact_%s", m == SURF_DEFAULT ? "concrete" : surface_name(m));
        CHECK(!strcmp(lib->effects[e].name, want), "%s -> %s (got %s)", surface_name(m), want, lib->effects[e].name);
    }
    int hs = vfx_lib_impact_effect(lib, SURF_FLESH, true);
    CHECK(hs >= 0 && !strcmp(lib->effects[hs].name, "impact_flesh_head"), "headshot variant");
    // Every weapon type x device has a muzzle effect named for it.
    for (int t = 0; t < VFX_MUZZLE_TYPE_COUNT; t++)
        for (int d = 0; d < VFX_DEV_COUNT; d++) {
            int e = vfx_lib_muzzle_effect(lib, t, d);
            char want[64];
            snprintf(want, sizeof(want), "muzzle_%s_%s", vfx_muzzle_type_name(t), vfx_muzzle_device_name(d));
            CHECK(e >= 0 && !strcmp(lib->effects[e].name, want), "muzzle %s (got %s)", want, e >= 0 ? lib->effects[e].name : "none");
        }
    // Every texture an emitter or decal names exists.
    for (int i = 0; i < lib->emitter_count; i++) {
        const VfxEmitterDef *e = &lib->emitters[i];
        CHECK(!e->render.texture[0] || e->render.texture_id >= 0, "emitter %s texture %s", e->name, e->render.texture);
    }
    for (int i = 0; i < lib->effect_count; i++) {
        const VfxDecalDef *d = &lib->effects[i].decal;
        CHECK(!d->enabled || d->texture_id >= 0, "effect %s decal texture %s", lib->effects[i].name, d->texture);
    }
    // Round trip: save -> parse gives the same emitter count and core parameters.
    int fx = vfx_lib_find_effect(lib, "impact_concrete");
    if (fx >= 0) {
        char *txt = vfx_lib_effect_to_json(lib, fx);
        VfxLibrary *l2 = (VfxLibrary *)calloc(1, sizeof(VfxLibrary));
        int f2 = vfx_lib_add_effect_json(l2, txt, "roundtrip");
        CHECK(f2 >= 0 && l2->effects[f2].emitter_count == lib->effects[fx].emitter_count, "editor save round trip");
        if (f2 >= 0)
            for (int k = 0; k < lib->effects[fx].emitter_count; k++) {
                const VfxEmitterDef *a = &lib->emitters[lib->effects[fx].emitters[k]], *b = &l2->emitters[l2->effects[f2].emitters[k]];
                CHECK(a->spawn.count.max == b->spawn.count.max && a->render.mode == b->render.mode && a->update.drag == b->update.drag &&
                          vfx_curve_eval1(&a->update.size_curve, 0.37f) == vfx_curve_eval1(&b->update.size_curve, 0.37f),
                      "round trip emitter %s", a->name);
            }
        free(l2);
        free(txt);
    }
    for (int t = 0; t < VFX_MUZZLE_TYPE_COUNT; t++) {
        char n[64];
        snprintf(n, sizeof(n), "muzzle_%s_bare", vfx_muzzle_type_name(t));
        test_flash_per_shot(lib, n);
    }
    free(lib);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    test_curves();
    test_determinism();
    test_meta();
    test_atlas();
    test_decal_budget();
    test_assets();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
