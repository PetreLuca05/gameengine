// Flesh range (dev room, north of the room): player / zombie / helmet zombie targets at 5, 15 and
// 40 m with a wall behind each group. The panel (flesh_range) fires synthetic hits at a chosen
// body part with any weapon's caliber and damage (or kills), toggles slow motion, day / night,
// gore and vfx_debug (exit chords, splatter rays, decal boxes). flesh_test checks the rules in
// the running game (exit direction, splats behind the target, per-character budgets, gore off,
// timings) and logs PASS / FAIL.
#include "game_internal.h"
#include "gameplay/flesh_fx.h"
#include "gameplay/characters.h"
#include "gameplay/health.h"
#include "gameplay/items.h"
#include "vfx/vfx.h"
#include "vfx/vfx_library.h"
#include "render/debug_draw.h"
#include "ui/ui.h"
#include "core/log.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/platform.h"
#include "nuklear_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)
#define FR_LINE_Z (-34.0f)
static const float k_group_x[3] = { -10.0f, 0.0f, 10.0f };
static const float k_dist[3] = { 5.0f, 15.0f, 40.0f };
static const float k_lane_dx[3] = { -1.6f, 0.0f, 1.6f };
static const char *const k_lane_names[3] = { "player", "zombie", "helmet" };
static const char *const k_weapons[] = { "pistol", "smg", "rifle", "shotgun", "dmr", "sniper", "lmg", "bat", "knife" };
#define FR_WEAPONS ((int)(sizeof(k_weapons) / sizeof(k_weapons[0])))
static const int k_parts[] = { BP_HEAD, BP_NECK, BP_CHEST, BP_STOMACH, BP_PELVIS, BP_UPPER_ARM_L, BP_LOWER_ARM_R, BP_HAND_L, BP_UPPER_LEG_R, BP_LOWER_LEG_L, BP_FOOT_R };
#define FR_PARTS ((int)(sizeof(k_parts) / sizeof(k_parts[0])))

typedef struct FleshRange {
    bool open;
    int weapon, part, group, lane;
    bool kill, slowmo;
    // flesh_test
    int test_state, test_frames, test_fail, test_checks;
    bool test_quit;
    int perf_hits;
    double perf_ms;
    char log[512];
} FleshRange;
static FleshRange FRG = { false, 2, 2, 1, 1, false, false, 0, 0, 0, 0, false, 0, 0, { 0 } };

static CharacterInst *range_target(World *w, int group, int lane) {
    if (!w || !w->chars) return NULL;
    vec3 want = v3(k_group_x[group] + k_lane_dx[lane], 0, FR_LINE_Z - k_dist[group]);
    CharacterInst *best = NULL;
    float bd = 1.0f;
    for (int i = 0; i < MAX_CHARACTERS; i++) {
        CharacterInst *c = w->chars->c[i];
        if (!c || !ent_valid(w, c->ent)) continue;
        float d = sqrtf((c->root.t.x - want.x) * (c->root.t.x - want.x) + (c->root.t.z - want.z) * (c->root.t.z - want.z));
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}

static const WeaponDef *weapon_by_id(World *w, const char *id) {
    if (!w || !w->items) return NULL;
    for (int i = 0; i < w->items->weapon_count; i++)
        if (!strcmp(w->items->weapons[i].id, id)) return &w->items->weapons[i];
    return NULL;
}

static const HitCapsule *part_capsule(const CharacterInst *c, int part) {
    for (int i = 0; i < c->hit.count; i++) if (c->hit.c[i].part == part) return &c->hit.c[i];
    return c->hit.count ? &c->hit.c[0] : NULL;
}

// One synthetic hit from the firing line (or a melee swing up close). Returns false if the
// target or part is missing. The shooter position is written for the tests.
static bool range_fire(World *w, CharacterInst *c, int part, const WeaponDef *wd, bool kill, float spread_deg, u32 seed, vec3 *shooter_out) {
    const HitCapsule *k = part_capsule(c, part);
    if (!k || !wd) return false;
    bool melee = wd->vfx_caliber == VFX_CAL_MELEE;
    vec3 aim = v3_scale(v3_add(k->a, k->b), 0.5f);
    float dist = melee ? 1.2f : (FR_LINE_Z - c->root.t.z);
    vec3 shooter = v3(aim.x, 1.55f, c->root.t.z + fabsf(dist));
    if (melee) shooter = v3(aim.x + 0.3f, aim.y + 0.1f, c->root.t.z + 1.2f);
    vec3 dir = v3_norm(v3_sub(aim, shooter));
    if (spread_deg > 0) {
        u32 s = seed;
        float a = flesh_rand01(&s) * 6.2831853f, r = sqrtf(flesh_rand01(&s)) * spread_deg * DG_DEG2RAD;
        vec3 right = v3_norm(v3_cross(dir, v3(0, 1, 0))), up = v3_cross(right, dir);
        dir = v3_norm(v3_add(dir, v3_add(v3_scale(right, cosf(a) * r), v3_scale(up, sinf(a) * r))));
    }
    float t = ray_capsule(shooter, dir, k->a, k->b, k->r);
    if (t <= 0) {
        // Missed the part (spread): any capsule of the character.
        HitboxRayHit hh;
        if (!hitbox_raycast(&c->hit, shooter, dir, 100.0f, &hh)) return false;
        t = hh.t;
        part = hh.part;
    }
    vec3 entry = v3_madd(shooter, dir, t);
    vec3 ab = v3_sub(k->b, k->a);
    float s = v3_len2(ab) > 1e-8f ? clampf(v3_dot(v3_sub(entry, k->a), ab) / v3_len2(ab), 0, 1) : 0;
    vec3 n = v3_sub(entry, v3_madd(k->a, ab, s));
    n = v3_len2(n) > 1e-8f ? v3_norm(n) : v3_neg(dir);
    FleshHit fh;
    memset(&fh, 0, sizeof(fh));
    fh.target = c->ent;
    fh.part = part;
    fh.pos = entry;
    fh.normal = n;
    // Melee: the swing goes right to left across the target.
    fh.dir = melee ? v3_norm(v3_add(v3(-0.85f, -0.1f, 0), v3_scale(dir, 0.35f))) : dir;
    fh.caliber = wd->vfx_caliber;
    fh.damage = damage_resolve(wd->damage, c->def->part_mult, wd->part_mult, part, 1.0f);
    fh.dist = melee ? 1.2f : t;
    fh.seed = seed;
    bool can_kill = kill && w->authority && !c->dead;
    fh.flags = (part == BP_HEAD ? FLESH_HEAD : 0u) | (melee ? FLESH_MELEE : 0u) | (can_kill ? FLESH_KILL : 0u);
    flesh_fx_hit(w, &fh);
    // Kill (host): the flesh hit first (its wounds bleed on the ragdoll), then the death.
    if (can_kill) apply_damage_part(w, c->ent, 1e6f, (DamageType)wd->damage_type, ENT_NONE, entry, dir, part, 1.0f);
    if (shooter_out) *shooter_out = shooter;
    return true;
}

static void range_fire_selected(World *w) {
    CharacterInst *c = range_target(w, FRG.group, FRG.lane);
    const WeaponDef *wd = weapon_by_id(w, k_weapons[FRG.weapon]);
    if (!c || !wd) { log_warn("flesh_range: no target / weapon"); return; }
    int pellets = DG_MAX(1, wd->pellets);
    u32 seed = flesh_hash((u32)(platform_time() * 1000.0));
    for (int p = 0; p < pellets; p++)
        range_fire(w, c, k_parts[FRG.part], wd, FRG.kill && p == pellets - 1, pellets > 1 ? 2.2f : 0.0f, flesh_hash(seed + (u32)p), NULL);
}

// flesh_shoot <group 0-2 | 5 | 15 | 40> <lane player|zombie|helmet> <part> <weapon> [kill]
static void cmd_flesh_shoot(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    if (argc < 5 || !w) { log_info("usage: flesh_shoot <5|15|40> <player|zombie|helmet> <part> <weapon> [kill]"); return; }
    int g = atoi(argv[1]);
    g = g == 5 ? 0 : (g == 15 ? 1 : (g == 40 ? 2 : DG_CLAMP(g, 0, 2)));
    int lane = 1;
    for (int i = 0; i < 3; i++) if (!strcmp(argv[2], k_lane_names[i])) lane = i;
    int part = body_part_from_name(argv[3]);
    if (part < 0) part = BP_CHEST;
    const WeaponDef *wd = weapon_by_id(w, argv[4]);
    CharacterInst *c = range_target(w, g, lane);
    if (!c || !wd) { log_warn("flesh_shoot: no target / weapon '%s'", argv[4]); return; }
    bool kill = argc > 5 && !strcmp(argv[5], "kill");
    int pellets = DG_MAX(1, wd->pellets);
    u32 seed = flesh_hash((u32)(w->tick * 2654435761u) ^ (u32)part);
    for (int p = 0; p < pellets; p++) range_fire(w, c, part, wd, kill && p == pellets - 1, pellets > 1 ? 2.2f : 0.0f, flesh_hash(seed + (u32)p), NULL);
    const FleshDebug *d = flesh_fx_last();
    log_info("flesh_shoot: %s %s %s at %.0f m: %s (thickness %.3f m), %d rays", k_lane_names[lane], body_part_name(part), argv[4], k_dist[g],
             d->exited ? "exit" : "no exit", d->thickness, d->rays);
}

static void cmd_flesh_range(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    FRG.open = !FRG.open;
}

bool flesh_range_open(void) { return FRG.open; }

// ------------------------------------------------------------------ flesh_test

#define TCHECK(cond, ...)                                                   \
    do {                                                                    \
        FRG.test_checks++;                                                  \
        if (!(cond)) {                                                      \
            FRG.test_fail++;                                                \
            char m_[256];                                                   \
            snprintf(m_, sizeof(m_), __VA_ARGS__);                          \
            log_warn("flesh_test FAIL: %s", m_);                            \
        }                                                                   \
    } while (0)

static int blood_decals(u32 attach) { return vfx_sys_decal_count(vfx_game_system(), VFX_DECAL_POOL_BLOOD, attach); }

static void test_exits(World *w) {
    // Rifle through every part of the player and the zombie at every distance: exits along the
    // shot, splats behind the target.
    const WeaponDef *rifle = weapon_by_id(w, "rifle"), *pistol = weapon_by_id(w, "pistol"), *shotgun = weapon_by_id(w, "shotgun");
    TCHECK(rifle && pistol && shotgun, "weapons present");
    if (!rifle || !pistol || !shotgun) return;
    int exits = 0, shots = 0, splats = 0;
    for (int g = 0; g < 3; g++)
        for (int lane = 0; lane < 2; lane++) {
            CharacterInst *c = range_target(w, g, lane);
            TCHECK(c != NULL, "target %s at %.0f m", k_lane_names[lane], k_dist[g]);
            if (!c) continue;
            for (int pi = 0; pi < FR_PARTS; pi++) {
                vec3 shooter;
                if (!range_fire(w, c, k_parts[pi], rifle, false, 0, (u32)(g * 100 + lane * 20 + pi + 1), &shooter)) continue;
                const FleshDebug *d = flesh_fx_last();
                shots++;
                TCHECK(d->valid, "debug record");
                TCHECK(d->exited, "rifle exits %s %s at %.0f m (thickness %.3f)", k_lane_names[lane], body_part_name(k_parts[pi]), k_dist[g], d->thickness);
                if (!d->exited) continue;
                exits++;
                vec3 e = v3_sub(d->exit, d->entry);
                TCHECK(v3_dot(e, d->dir) > 0 && d->thickness > 0.01f && d->thickness < 0.8f, "%s exit along the shot (%.3f m)", body_part_name(k_parts[pi]), d->thickness);
                float td = v3_dist(shooter, d->entry);
                for (int i = 0; i < d->rays; i++) {
                    if (!d->splats[i].hit) continue;
                    splats++;
                    TCHECK(v3_dot(v3_sub(d->splats[i].pos, d->exit), d->dir) > 0, "splat behind the exit");
                    TCHECK(v3_dist(d->splats[i].pos, shooter) > td, "splat farther than the target (%.2f vs %.2f)", v3_dist(d->splats[i].pos, shooter), td);
                }
            }
            TCHECK(blood_decals(c->ent) <= VFX_SKINNED_DECALS_PER_CHAR, "%s budget (%d)", k_lane_names[lane], blood_decals(c->ent));
        }
    TCHECK(splats > 0, "splats placed (%d)", splats);
    // Pistols mostly stop in the torso; shotgun pellets at 40 m never exit.
    CharacterInst *z = range_target(w, 1, 1), *far = range_target(w, 2, 1);
    int pistol_exits = 0, sg_exits = 0;
    for (int i = 0; z && far && i < 20; i++) {
        range_fire(w, z, BP_CHEST, pistol, false, 0, 500u + (u32)i, NULL);
        pistol_exits += flesh_fx_last()->exited;
        range_fire(w, far, BP_STOMACH, shotgun, false, 2.0f, 900u + (u32)i, NULL);
        sg_exits += flesh_fx_last()->exited;
    }
    TCHECK(pistol_exits <= 4, "pistol chest exits rarely (%d / 20)", pistol_exits);
    TCHECK(sg_exits == 0, "shotgun at 40 m never exits (%d / 20)", sg_exits);
    // Helmet: sparks for its hits, no blood, no wound on the head until it breaks.
    CharacterInst *hz = range_target(w, 0, 2);
    TCHECK(hz && hz->def->armor_hits[BP_HEAD] > 0, "helmet zombie");
    if (hz) {
        int before = blood_decals(hz->ent);
        memcpy(hz->armor_left, hz->def->armor_hits, sizeof(hz->armor_left));
        for (int i = 0; i < hz->def->armor_hits[BP_HEAD]; i++) range_fire(w, hz, BP_HEAD, rifle, false, 0, 700u + (u32)i, NULL);
        TCHECK(blood_decals(hz->ent) == before && hz->armor_left[BP_HEAD] == 0, "helmet hits: no blood (%d -> %d)", before, blood_decals(hz->ent));
        range_fire(w, hz, BP_HEAD, rifle, false, 0, 710u, NULL);
        TCHECK(blood_decals(hz->ent) > before, "broken helmet: the head bleeds");
    }
    log_info("flesh_test: %d rifle shots, %d exits, %d splats; pistol chest exits %d/20, shotgun 40 m exits %d/20", shots, exits, splats, pistol_exits, sg_exits);
}

static void test_budget_and_gore(World *w) {
    const WeaponDef *rifle = weapon_by_id(w, "rifle");
    CharacterInst *c = range_target(w, 0, 0);
    if (!rifle || !c) return;
    int worst = 0;
    for (int i = 0; i < 60; i++) {
        range_fire(w, c, k_parts[i % FR_PARTS], rifle, false, 1.5f, 1000u + (u32)i, NULL);
        int n = blood_decals(c->ent);
        if (n > worst) worst = n;
    }
    TCHECK(worst <= VFX_SKINNED_DECALS_PER_CHAR, "60 hits on one character: max %d skinned decals (<= %d)", worst, VFX_SKINNED_DECALS_PER_CHAR);
    // Gore off: every blood decal goes, and new hits place none.
    Cvar *gore = cvar_find("gore_level");
    int was = gore ? gore->i : 2;
    if (gore) cvar_set_int(gore, 0);
    vfx_game_render(v3(0, 1.6f, FR_LINE_Z));   // applies the config (blood budget 0)
    for (int i = 0; i < 20; i++) range_fire(w, c, k_parts[i % FR_PARTS], rifle, false, 0, 2000u + (u32)i, NULL);
    TCHECK(blood_decals(0) == 0, "gore off: no blood decals (%d)", blood_decals(0));
    if (gore) cvar_set_int(gore, was);
}

static void test_perf(World *w) {
    // 50 hits / s for this frame's share (called every frame for 3 s): entry and full costs.
    const WeaponDef *rifle = weapon_by_id(w, "rifle");
    if (!rifle) return;
    float dt = (float)g_engine.frame_dt;
    static float accum;
    accum += 50.0f * fmaxf(dt, 1.0f / 240.0f);
    while (accum >= 1.0f) {
        accum -= 1.0f;
        int k = FRG.perf_hits++;
        CharacterInst *c = range_target(w, k % 3, (k / 3) % 2);
        if (!c) continue;
        double t0 = platform_time();
        range_fire(w, c, k_parts[k % FR_PARTS], rifle, false, 1.0f, 3000u + (u32)k, NULL);
        FRG.perf_ms += (platform_time() - t0) * 1000.0;
    }
}

static void test_frame(void) {
    World *w = G ? G->world : NULL;
    if (!FRG.test_state || !w) return;
    FRG.test_frames++;
    switch (FRG.test_state) {
    case 1:   // wait for the characters to be posed
        if (FRG.test_frames > 30) { flesh_fx_reset_stats(); test_exits(w); test_budget_and_gore(w); FRG.test_state = 2; FRG.test_frames = 0; flesh_fx_reset_stats(); }
        break;
    case 2:
        test_perf(w);
        if (FRG.test_frames > 180) FRG.test_state = 3, FRG.test_frames = 0;   // entry / full averages over the sustained fire
        break;
    case 3: {   // a kill: burst, pool and bleeding wounds
        const WeaponDef *rifle = weapon_by_id(w, "rifle");
        CharacterInst *c = range_target(w, 1, 1);
        if (FRG.test_frames == 1 && c && rifle) {
            double t0 = platform_time();
            range_fire(w, c, BP_HEAD, rifle, true, 0, 4242u, NULL);
            log_info("flesh_test: kill hit %.3f ms", (platform_time() - t0) * 1000.0);
        }
        if (FRG.test_frames > 150) {
            const FleshStats *st = flesh_fx_stats();
            TCHECK(st->pools >= 1, "kill: a pool started (%d)", st->pools);
            TCHECK(st->bleeders >= 1, "kill: wounds bleed (%d)", st->bleeders);
            float avg = FRG.perf_hits ? (float)(FRG.perf_ms / FRG.perf_hits) : 0;
            TCHECK(FRG.perf_hits >= 140, "50 hits / s sustained (%d in 3 s)", FRG.perf_hits);
            float entry_avg = st->n_entry ? (float)(st->ms_sum_entry / st->n_entry) : 0;
            TCHECK(entry_avg < 0.03f, "entry avg %.4f ms (< 0.03)", entry_avg);
            TCHECK(st->ms_max_kill < 0.15f, "kill hit %.3f ms (< 0.15)", st->ms_max_kill);
            snprintf(FRG.log, sizeof(FRG.log), "flesh_test %s: %d checks, %d failed; %d hits at 50/s, full hit avg %.3f ms, entry avg %.4f / max %.4f ms, kill %.3f ms, %d splats %d drips %d wounds %d pools",
                     FRG.test_fail ? "FAIL" : "PASS", FRG.test_checks, FRG.test_fail, FRG.perf_hits, avg, entry_avg, st->ms_max_entry, st->ms_max_kill,
                     st->splats, st->drips, st->wounds, st->pools);
            log_info("%s", FRG.log);
            FRG.test_state = 0;
            if (FRG.test_quit) cbuf_add("quit");
        }
        break;
    }
    default: FRG.test_state = 0; break;
    }
}

static void cmd_flesh_test(int argc, const char **argv) {
    if (!G || !G->world || !G->world->authority) { log_warn("flesh_test: host only (dev room)"); return; }
    FRG.test_state = 1;
    FRG.test_frames = FRG.test_fail = FRG.test_checks = FRG.perf_hits = 0;
    FRG.perf_ms = 0;
    FRG.test_quit = argc > 1 && !strcmp(argv[1], "quit");
    log_info("flesh_test: running (about 6 s)");
}

// ------------------------------------------------------------------ panel

static void draw_panel(void) {
    if (!FRG.open) return;
    struct nk_context *ctx = ui_nk();
    World *w = G ? G->world : NULL;
    float s = ui_scale();
    if (nk_begin(ctx, "FLESH RANGE", nk_rect(20 * s, 80 * s, 420 * s, 600 * s), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE)) {
        char b[160];
        nk_layout_row_dynamic(ctx, 20 * s, 1);
        nk_label(ctx, "Target", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24 * s, 3);
        for (int i = 0; i < 3; i++) { snprintf(b, sizeof(b), "%.0f m", k_dist[i]); if (nk_option_label(ctx, b, FRG.group == i)) FRG.group = i; }
        for (int i = 0; i < 3; i++) if (nk_option_label(ctx, k_lane_names[i], FRG.lane == i)) FRG.lane = i;
        nk_layout_row_dynamic(ctx, 20 * s, 1);
        nk_label(ctx, "Body part", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 22 * s, 3);
        for (int i = 0; i < FR_PARTS; i++) if (nk_option_label(ctx, body_part_name(k_parts[i]), FRG.part == i)) FRG.part = i;
        nk_layout_row_dynamic(ctx, 20 * s, 1);
        nk_label(ctx, "Weapon", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 22 * s, 3);
        for (int i = 0; i < FR_WEAPONS; i++) if (nk_option_label(ctx, k_weapons[i], FRG.weapon == i)) FRG.weapon = i;
        nk_layout_row_dynamic(ctx, 26 * s, 2);
        if (nk_button_label(ctx, "FIRE")) range_fire_selected(w);
        FRG.kill = nk_check_label(ctx, "kill (host)", FRG.kill);
        nk_layout_row_dynamic(ctx, 24 * s, 3);
        bool slow = nk_check_label(ctx, "slow motion", FRG.slowmo);
        if (slow != FRG.slowmo) { FRG.slowmo = slow; cvar_set_float(cvar_find("vfx_timescale"), slow ? 0.15f : 1.0f); }
        if (nk_button_label(ctx, "day")) cbuf_add("time_set 12:00");
        if (nk_button_label(ctx, "night")) cbuf_add("time_set 23:30");
        Cvar *dbg = cvar_find("vfx_debug"), *gore = cvar_find("gore_level");
        nk_layout_row_dynamic(ctx, 24 * s, 2);
        if (dbg) { bool on = nk_check_label(ctx, "vfx_debug (rays, boxes)", dbg->i != 0); if (on != (dbg->i != 0)) cvar_set_int(dbg, on ? 1 : 0); }
        if (gore) {
            static const char *gl[] = { "gore off", "gore low", "gore high" };
            int g = nk_combo(ctx, gl, 3, gore->i, (int)(22 * s), nk_vec2(160 * s, 100 * s));
            if (g != gore->i) cvar_set_int(gore, g);
        }
        nk_layout_row_dynamic(ctx, 24 * s, 2);
        if (nk_button_label(ctx, "go to the firing line")) {
            snprintf(b, sizeof(b), "setpos %.1f 0.05 %.1f 0 0; lookat %.1f 1.3 %.1f", k_group_x[FRG.group], FR_LINE_Z + 0.6f, k_group_x[FRG.group] + k_lane_dx[FRG.lane],
                     FR_LINE_Z - k_dist[FRG.group]);
            cbuf_add(b);
        }
        if (nk_button_label(ctx, "give the weapon")) { snprintf(b, sizeof(b), "give %s; give %s_ammo", k_weapons[FRG.weapon], k_weapons[FRG.weapon]); cbuf_add(b); }
        const FleshStats *st = flesh_fx_stats();
        const FleshDebug *d = flesh_fx_last();
        nk_layout_row_dynamic(ctx, 18 * s, 1);
        snprintf(b, sizeof(b), "last: %s, thickness %.3f m, rays %d (%d hit)", d->exited ? "exit" : "no exit", d->thickness, st->last_rays, st->last_ray_hits);
        nk_label(ctx, b, NK_TEXT_LEFT);
        snprintf(b, sizeof(b), "entry %.4f ms (max %.4f), full %.3f, kill %.3f ms", st->ms_last_entry, st->ms_max_entry, st->ms_last_full, st->ms_max_kill);
        nk_label(ctx, b, NK_TEXT_LEFT);
        CharacterInst *c = range_target(w, FRG.group, FRG.lane);
        snprintf(b, sizeof(b), "blood decals: %d (target %d / %d), splats %d drips %d pools %d", blood_decals(0), c ? blood_decals(c->ent) : 0, VFX_SKINNED_DECALS_PER_CHAR,
                 st->splats, st->drips, st->pools);
        nk_label(ctx, b, NK_TEXT_LEFT);
        if (FRG.log[0]) nk_label_wrap(ctx, FRG.log);
        nk_layout_row_dynamic(ctx, 24 * s, 2);
        if (nk_button_label(ctx, "run flesh_test")) cbuf_add("flesh_test");
        if (nk_button_label(ctx, "Close")) FRG.open = false;
    }
    nk_end(ctx);
}

void flesh_range_draw(void) {
    test_frame();
    draw_panel();
}

void flesh_range_register(void) {
    cmd_register("flesh_range", cmd_flesh_range, "toggle the flesh range panel (dev room, north)");
    cmd_register("flesh_shoot", cmd_flesh_shoot, "flesh_shoot <5|15|40> <player|zombie|helmet> <part> <weapon> [kill]: synthetic hit");
    cmd_register("flesh_test", cmd_flesh_test, "flesh_test [quit]: flesh impact checks on the range (host)");
}
