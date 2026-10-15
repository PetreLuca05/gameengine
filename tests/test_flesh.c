// Flesh impact tests (pure, no GPU / physics): shipped rules, exit direction and thickness per
// body part through the hit capsules, the exit rules per caliber, splatter rays landing behind
// the target (stretched by angle, drips on walls), per-character skinned decal budgets, gore off
// (no blood decals) and the per-character blood / surface / armour blocks.
// Run from the build output directory.
#include "vfx/vfx_flesh.h"
#include "vfx/vfx_library.h"
#include "vfx/vfx_sim.h"
#include "assets/character.h"
#include "core/engine.h"
#include "core/platform.h"
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

static FleshRules g_rules;

static void test_rules(void) {
    size_t n = 0;
    char *text = platform_read_file("assets/vfx/flesh_rules.json", &n);
    char err[128] = { 0 };
    CHECK(text && flesh_rules_parse(&g_rules, text, err, sizeof(err)), "flesh_rules.json parses (%s)", err);
    free(text);
    CHECK(g_rules.cal[VFX_CAL_RIFLE].exit_chance >= 1.0f, "rifles always exit");
    CHECK(g_rules.cal[VFX_CAL_PISTOL].exit_chance > 0 && g_rules.cal[VFX_CAL_PISTOL].exit_chance < 0.5f, "pistols rarely");
    CHECK(g_rules.cal[VFX_CAL_SHOTGUN].max_dist > 0, "shotgun: no exit at range");
    CHECK(g_rules.cal[VFX_CAL_MELEE].exit_chance == 0, "melee never exits");
    CHECK(g_rules.skinned_max <= VFX_SKINNED_DECALS_PER_CHAR, "skinned budget <= %d", VFX_SKINNED_DECALS_PER_CHAR);
    // Scaling: farther = smaller, more damage = bigger, rifles bigger than pistols.
    CHECK(flesh_hit_scale(&g_rules, VFX_CAL_RIFLE, 35, 40) < flesh_hit_scale(&g_rules, VFX_CAL_RIFLE, 35, 5), "distance shrinks");
    CHECK(flesh_hit_scale(&g_rules, VFX_CAL_RIFLE, 80, 5) > flesh_hit_scale(&g_rules, VFX_CAL_RIFLE, 20, 5), "damage grows");
    CHECK(flesh_hit_scale(&g_rules, VFX_CAL_RIFLE, 35, 5) > flesh_hit_scale(&g_rules, VFX_CAL_PISTOL, 35, 5), "caliber");
}

typedef struct PartCap { int part; float r, len; } PartCap;

static void test_exit(void) {
    // Capsules sized like the shipped hitboxes (radius, segment length): head, chest, stomach,
    // upper arm, forearm, thigh, shin. Shots through the middle, at angles and off-centre.
    const PartCap caps[] = { { BP_HEAD, 0.10f, 0.10f }, { BP_CHEST, 0.15f, 0.22f }, { BP_STOMACH, 0.14f, 0.18f }, { BP_UPPER_ARM_L, 0.05f, 0.26f },
                             { BP_LOWER_ARM_R, 0.04f, 0.24f }, { BP_UPPER_LEG_L, 0.075f, 0.38f }, { BP_LOWER_LEG_R, 0.055f, 0.38f } };
    for (int i = 0; i < (int)(sizeof(caps) / sizeof(caps[0])); i++) {
        const PartCap *c = &caps[i];
        vec3 a = v3(0, 1.0f, 0), b = v3(0, 1.0f + c->len, 0);
        for (int k = 0; k < 5; k++) {
            float ang = (float)k * 0.25f;                      // 0 .. 1 rad around the vertical axis
            vec3 dir = v3_norm(v3(sinf(ang), -0.1f * (float)k, -cosf(ang)));
            float off = (float)(k % 3 - 1) * c->r * 0.4f;      // off-centre shots (shorter chords)
            vec3 from = v3(off, 1.0f + c->len * 0.5f, 3.0f);
            from = v3_madd(from, dir, -0.0f);
            // Entry: the ray's first capsule hit.
            vec3 rd = v3_norm(v3_sub(v3(off, 1.0f + c->len * 0.5f, 0), from));
            if (k) rd = dir, from = v3_madd(v3(off, 1.0f + c->len * 0.5f, 0), dir, -3.0f);
            float t = ray_capsule(from, rd, a, b, c->r);
            CHECK(t > 0, "%s shot %d hits", body_part_name(c->part), k);
            if (t <= 0) continue;
            vec3 entry = v3_madd(from, rd, t), ex;
            float th;
            bool ok = flesh_capsule_exit(a, b, c->r, entry, rd, &ex, &th);
            CHECK(ok, "%s shot %d: exit found", body_part_name(c->part), k);
            if (!ok) continue;
            // Along the incoming direction, on the far side, on the surface, never thicker than
            // the capsule allows.
            vec3 d = v3_sub(ex, entry);
            CHECK(v3_dot(d, rd) > 0 && fabsf(v3_len(d) - th) < 1e-3f, "%s shot %d: exit along the shot (%.3f vs %.3f)", body_part_name(c->part), k, v3_len(d), th);
            CHECK(v3_len(v3_sub(v3_norm(d), rd)) < 1e-3f, "%s shot %d: exit direction = incoming", body_part_name(c->part), k);
            CHECK(th <= 2.0f * c->r + c->len + 1e-3f && th > 0.01f, "%s shot %d: thickness %.3f in (0, %.3f]", body_part_name(c->part), k, th, 2 * c->r + c->len);
            vec3 ab = v3_sub(b, a);
            float s = clampf(v3_dot(v3_sub(ex, a), ab) / v3_len2(ab), 0, 1);
            CHECK(fabsf(v3_dist(ex, v3_madd(a, ab, s)) - c->r) < 2e-3f, "%s shot %d: exit on the surface", body_part_name(c->part), k);
            if (k == 0) CHECK(fabsf(th - 2.0f * sqrtf(c->r * c->r - off * off)) < 2e-3f, "%s: straight chord %.3f", body_part_name(c->part), th);
        }
    }
    // Rules: a rifle through the chest exits, a pistol through it does not, a shotgun pellet at
    // 10 m never does, melee never; a rifle through a forearm always.
    const float chest = 0.30f, arm = 0.08f;
    CHECK(flesh_should_exit(&g_rules, VFX_CAL_RIFLE, BP_CHEST, chest, 20, 1), "rifle chest exits");
    CHECK(flesh_should_exit(&g_rules, VFX_CAL_RIFLE, BP_HEAD, 0.2f, 20, 2), "rifle head exits");
    CHECK(!flesh_should_exit(&g_rules, VFX_CAL_PISTOL, BP_CHEST, chest, 5, 3), "pistol chest stops");
    CHECK(flesh_should_exit(&g_rules, VFX_CAL_RIFLE, BP_LOWER_ARM_L, arm, 20, 4), "rifle forearm exits");
    int sg_far = 0, sg_near = 0, pistol_arm = 0;
    for (u32 s = 1; s <= 400; s++) {
        sg_far += flesh_should_exit(&g_rules, VFX_CAL_SHOTGUN, BP_UPPER_ARM_L, arm, 10, s);
        sg_near += flesh_should_exit(&g_rules, VFX_CAL_SHOTGUN, BP_UPPER_ARM_L, arm, 3, s);
        pistol_arm += flesh_should_exit(&g_rules, VFX_CAL_PISTOL, BP_UPPER_ARM_L, arm, 5, s);
        CHECK(!flesh_should_exit(&g_rules, VFX_CAL_MELEE, BP_CHEST, 0.05f, 1, s), "melee never exits");
        CHECK(flesh_should_exit(&g_rules, VFX_CAL_SHOTGUN, BP_UPPER_ARM_L, arm, 3, s) == flesh_should_exit(&g_rules, VFX_CAL_SHOTGUN, BP_UPPER_ARM_L, arm, 3, s),
              "deterministic per seed");
    }
    CHECK(sg_far == 0, "shotgun at 10 m never exits (%d)", sg_far);
    CHECK(sg_near > 0 && sg_near < 400, "shotgun close: sometimes (%d / 400)", sg_near);
    CHECK(pistol_arm > 40 && pistol_arm < 260, "pistol through an arm: rarely-ish (%d / 400)", pistol_arm);
    printf("  exit: shotgun near %d/400, pistol arm %d/400\n", sg_near, pistol_arm);
}

// A room: floor y = 0, back wall z = -4, side wall x = 2.5 (rays fly toward -z).
static bool room_ray(void *ctx, vec3 from, vec3 delta, vec3 *hit, vec3 *normal) {
    (void)ctx;
    float best = 2.0f;
    vec3 n = v3(0, 0, 0);
    if (delta.y < 0) { float t = -from.y / delta.y; if (t > 0 && t < best) { best = t; n = v3(0, 1, 0); } }
    if (delta.z < 0) { float t = (-4.0f - from.z) / delta.z; if (t > 0 && t < best) { best = t; n = v3(0, 0, 1); } }
    if (delta.x > 0) { float t = (2.5f - from.x) / delta.x; if (t > 0 && t < best) { best = t; n = v3(-1, 0, 0); } }
    if (best > 1.0f) return false;
    *hit = v3_madd(from, delta, best);
    *normal = n;
    return true;
}

static void test_splatter(void) {
    // Shooter at z = +10, target at the origin (chest 1.3 m), shooting -z: every splat lies
    // behind the target, farther than it from the shooter.
    vec3 shooter = v3(0, 1.5f, 10);
    vec3 exitp = v3(0, 1.3f, -0.15f);
    vec3 dir = v3_norm(v3_sub(exitp, shooter));
    FleshSplat sp[16];
    int total = 0, hits = 0, drips = 0, stretched = 0, walls = 0;
    for (u32 seed = 1; seed <= 50; seed++) {
        int n = flesh_plan_splatter(&g_rules, exitp, dir, 1.0f, 8, 5.0f, 14.0f, room_ray, NULL, seed, sp, 16);
        CHECK(n == 8, "8 rays planned (%d)", n);
        for (int i = 0; i < n; i++) {
            total++;
            if (!sp[i].hit) continue;
            hits++;
            CHECK(v3_dot(v3_sub(sp[i].pos, exitp), dir) > 0, "splat %d behind the exit", i);
            CHECK(v3_dist(sp[i].pos, shooter) > v3_dist(exitp, shooter), "splat farther from the shooter than the target");
            CHECK(sp[i].size > 0 && sp[i].alpha > 0 && sp[i].alpha <= 1, "splat size / alpha");
            CHECK(sp[i].aspect >= 1.0f && sp[i].aspect <= g_rules.splat_stretch_max + 1e-4f, "aspect in range (%.2f)", sp[i].aspect);
            if (v3_len2(sp[i].tangent) > 0) CHECK(fabsf(v3_dot(sp[i].tangent, sp[i].normal)) < 1e-3f, "tangent on the surface");
            if (sp[i].aspect > 1.5f) stretched++;
            if (fabsf(sp[i].normal.y) < 0.4f) walls++;
            if (sp[i].drip) { drips++; CHECK(fabsf(sp[i].normal.y) < 0.4f && sp[i].drip_len > 0, "drips only on vertical surfaces"); }
        }
    }
    CHECK(hits > total / 2, "most rays land (%d / %d)", hits, total);
    CHECK(walls > 0 && drips > 0, "the back wall gets splats (%d) and drips (%d)", walls, drips);
    CHECK(stretched > 0, "grazing splats stretch (%d)", stretched);
    printf("  splatter: %d/%d rays hit, %d on walls, %d drips, %d stretched\n", hits, total, walls, drips, stretched);
}

static void test_budgets(void) {
    VfxLibrary *lib = (VfxLibrary *)calloc(1, sizeof(VfxLibrary));
    vfx_lib_add_effect_json(lib, "{ \"name\": \"wound\", \"decal\": { \"texture\": \"x\", \"lifetime\": 1000, \"pool\": \"blood\", \"roughness\": 0.2 }, \"emitters\": [] }", "w");
    vfx_lib_add_effect_json(lib, "{ \"name\": \"hole\", \"decal\": { \"texture\": \"x\", \"lifetime\": 1000 }, \"emitters\": [] }", "h");
    vfx_lib_resolve(lib);
    CHECK(lib->effects[0].decal.pool == VFX_DECAL_POOL_BLOOD && fabsf(lib->effects[0].decal.roughness - 0.2f) < 1e-6f, "decal pool / roughness parsed");
    VfxSystem *s = (VfxSystem *)calloc(1, sizeof(VfxSystem));
    vfx_sys_init(s, lib);
    s->cfg.blood_decal_max = 200;
    s->cfg.decal_max = 64;
    // 50 hits/s on two characters for 4 s: never more than 16 skinned decals each.
    int worst_a = 0, worst_b = 0;
    for (int i = 0; i < 200; i++) {
        VfxDecalSpawn sp;
        memset(&sp, 0, sizeof(sp));
        sp.pos = v3(0, 1, 0);
        sp.normal = v3(0, 0, 1);
        sp.size = 0.04f;
        sp.kind = VFX_DECAL_SKINNED;
        sp.attach = (i & 1) ? 7u : 9u;
        sp.bone = 3;
        vfx_sys_add_decal_ex(s, &lib->effects[0].decal, &sp, (u32)i + 1u);
        if (i % 3 == 0) vfx_sys_add_decal(s, &lib->effects[1].decal, v3((float)i, 0, 0), v3(0, 1, 0), 1, (u32)i);
        int a = vfx_sys_decal_count(s, VFX_DECAL_POOL_BLOOD, 7u), b = vfx_sys_decal_count(s, VFX_DECAL_POOL_BLOOD, 9u);
        if (a > worst_a) worst_a = a;
        if (b > worst_b) worst_b = b;
        if (i % 3 == 0) vfx_sys_tick(s, 1.0f / 50.0f);
    }
    CHECK(worst_a <= VFX_SKINNED_DECALS_PER_CHAR && worst_b <= VFX_SKINNED_DECALS_PER_CHAR, "per-character skinned budget (%d, %d)", worst_a, worst_b);
    CHECK(vfx_sys_decal_count(s, VFX_DECAL_POOL_BLOOD, 7u) == VFX_SKINNED_DECALS_PER_CHAR, "character full at the budget");
    CHECK(vfx_sys_decal_count(s, VFX_DECAL_POOL_IMPACT, 0) <= 64, "impact pool separate (%d)", vfx_sys_decal_count(s, VFX_DECAL_POOL_IMPACT, 0));
    // Blood pool budget for static splats.
    for (int i = 0; i < 400; i++) {
        VfxDecalSpawn sp;
        memset(&sp, 0, sizeof(sp));
        sp.pos = v3((float)i * 0.1f, 0, 0);
        sp.normal = v3(0, 1, 0);
        sp.size = 0.3f;
        vfx_sys_add_decal_ex(s, &lib->effects[0].decal, &sp, (u32)i);
    }
    CHECK(vfx_sys_decal_count(s, VFX_DECAL_POOL_BLOOD, 0) <= 200, "blood pool budget (%d)", vfx_sys_decal_count(s, VFX_DECAL_POOL_BLOOD, 0));
    vfx_sys_kill_decals(s, 7u);
    CHECK(vfx_sys_decal_count(s, VFX_DECAL_POOL_BLOOD, 7u) == 0, "a revived character loses its wounds");
    // Gore off: the blood budget is 0, nothing is placed (impact holes still are).
    VfxSystem *g = (VfxSystem *)calloc(1, sizeof(VfxSystem));
    vfx_sys_init(g, lib);
    g->cfg.blood_decal_max = 0;
    int placed = 0;
    for (int i = 0; i < 100; i++) {
        VfxDecalSpawn sp;
        memset(&sp, 0, sizeof(sp));
        sp.pos = v3(0, 1, 0);
        sp.normal = v3(0, 0, 1);
        sp.kind = i & 1 ? VFX_DECAL_SKINNED : VFX_DECAL_STATIC;
        sp.attach = 5;
        placed += vfx_sys_add_decal_ex(g, &lib->effects[0].decal, &sp, (u32)i) >= 0;
        VfxSpawnParams p = vfx_spawn_params(v3(0, 0, 0));
        p.decal_ok = true;
        vfx_sys_spawn(g, 1, &p);
    }
    CHECK(placed == 0 && vfx_sys_decal_count(g, VFX_DECAL_POOL_BLOOD, 0) == 0, "gore off: no blood decals (%d)", placed);
    CHECK(vfx_sys_decal_count(g, VFX_DECAL_POOL_IMPACT, 0) > 0, "gore off: impact decals unaffected");
    free(g);
    free(s);
    free(lib);
}

static void test_characters(void) {
    const char *names[] = { "player", "zombie_placeholder", "zombie_runner", "zombie_brute", "zombie_helmet", "target_dummy" };
    CharacterDef *d = (CharacterDef *)calloc(1, sizeof(CharacterDef));
    float human_dark = 0;
    for (int i = 0; i < 6; i++) {
        char path[128];
        snprintf(path, sizeof(path), "assets/characters/%s.json", names[i]);
        size_t n = 0;
        char *text = platform_read_file(path, &n);
        char err[128] = { 0 };
        CHECK(text && character_def_parse(d, text, path, err, sizeof(err)), "%s parses", names[i]);
        free(text);
        CHECK(d->blood.amount > 0 && d->blood.color.x > d->blood.color.y * 3, "%s: blood is red (not neon: %.2f)", names[i], d->blood.color.x);
        CHECK(d->blood.color.x < 0.45f, "%s: dark blood", names[i]);
        if (i == 0) {
            human_dark = d->blood.darkness;
            CHECK(d->part_surface[BP_CHEST] == CSURF_CLOTH && d->part_surface[BP_HEAD] == CSURF_FLESH && d->part_surface[BP_HAND_L] == CSURF_FLESH, "player surfaces");
        }
        if (d->kind == CK_ZOMBIE) CHECK(d->blood.darkness > human_dark && d->blood.mist < 1.0f, "%s: darker, thicker blood", names[i]);
        if (i == 4) CHECK(d->armor_hits[BP_HEAD] == 2 && d->part_surface[BP_HEAD] == CSURF_ARMOR, "helmet: 2 hits on the head");
        else CHECK(d->armor_hits[BP_HEAD] == 0, "%s: no helmet", names[i]);
    }
    free(d);
}

static void test_shipped_effects(void) {
    VfxLibrary *lib = (VfxLibrary *)calloc(1, sizeof(VfxLibrary));
    bool ok = vfx_lib_load(lib, "assets/vfx");
    CHECK(ok, "assets/vfx loads");
    const char *fx[] = { "flesh_entry", "flesh_entry_cloth", "flesh_entry_mist", "flesh_exit", "flesh_exit_head", "flesh_mist", "flesh_kill", "melee_flesh",
                         "flesh_bone", "flesh_helmet", "flesh_helmet_break", "gore_dust", "flesh_bleed" };
    for (int i = 0; i < (int)(sizeof(fx) / sizeof(fx[0])); i++) CHECK(vfx_lib_find_effect(lib, fx[i]) >= 0, "effect %s", fx[i]);
    const char *dec[] = { "blood_splatter", "blood_drip_splat", "blood_drip", "blood_pool", "wound_entry", "wound_exit", "wound_slash", "blood_stain", "blood_stain_cloth", "blood_splat" };
    for (int i = 0; i < (int)(sizeof(dec) / sizeof(dec[0])); i++) {
        int e = vfx_lib_find_effect(lib, dec[i]);
        CHECK(e >= 0 && lib->effects[e].decal.enabled, "decal effect %s", dec[i]);
        if (e < 0) continue;
        const VfxDecalDef *d = &lib->effects[e].decal;
        CHECK(d->pool == VFX_DECAL_POOL_BLOOD, "%s in the blood pool", dec[i]);
        CHECK(d->roughness >= 0.05f && d->roughness <= 0.5f && d->dry_time > 0, "%s: wet, dries (%.2f)", dec[i], d->roughness);
        CHECK(d->texture_id >= 0, "%s texture", dec[i]);
    }
    // Every sub-emitter resolves.
    for (int i = 0; i < lib->emitter_count; i++)
        CHECK(!lib->emitters[i].update.sub_effect[0] || lib->emitters[i].update.sub_effect_id >= 0, "sub effect %s", lib->emitters[i].update.sub_effect);
    // Blood particles are dark, never neon.
    for (int i = 0; i < (int)(sizeof(fx) / sizeof(fx[0])); i++) {
        int e = vfx_lib_find_effect(lib, fx[i]);
        if (e < 0 || strstr(fx[i], "helmet") || strstr(fx[i], "bone") || strstr(fx[i], "dust")) continue;
        for (int k = 0; k < lib->effects[e].emitter_count; k++) {
            const VfxEmitterDef *em = &lib->emitters[lib->effects[e].emitters[k]];
            if (!strcmp(em->name, "fibers")) continue;
            CHECK(em->spawn.color_a.x <= 0.35f && em->spawn.color_a.y < 0.06f, "%s/%s: dark blood colour", fx[i], em->name);
        }
    }
    free(lib);
}

int main(void) {
    printf("flesh tests\n");
    test_rules();
    test_exit();
    test_splatter();
    test_budgets();
    test_characters();
    test_shipped_effects();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
