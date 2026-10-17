// Fire animation of the viewed player's weapon (docs/FIRE_ANIM.md): the pure layers live in
// src/anim/fire_anim.c; this file drives them from the shot events, steps them per tick (before
// the hold point is evaluated, so the kick lands on the shot's own tick), hands the result to the
// first-person body (gun kick, hand lag, trigger, slide) and to the camera / post layers, spawns
// the casing when the slide comes back, and owns the test tools: sway_check, fire_compare,
// fire_bench and the recorder the tuner (src/ui/fire_tuner.c) draws.
#include "game_internal.h"
#include "anim/fire_anim.h"
#include "player/sway.h"
#include "player/holdpoint.h"
#include "player/hands.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/platform.h"
#include "core/log.h"
#include "gameplay/weapons.h"
#include "gameplay/characters.h"
#include "gameplay/attachments.h"
#include "gameplay/effects.h"
#include "render/fidelity/fidelity.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define G (g_engine.game)
#define REC_N 600

static struct {
    Cvar *scale, *punch, *debug;
    FireAnimState st;
    FireAnimDef def;
    bool have_def;
    FireAnimOut out, frame;
    u32 shots;
    // recorder (tuner graphs, fire_compare)
    FireRecord rec[REC_N];
    int rec_pos, rec_count;
    u32 rec_tick;
    bool rec_shot;
    // fire_compare / sway_check runs
    int cmp_weapon, cmp_phase, cmp_wait;
    bool cmp_active;
} FA;

const FireAnimDef *game_fire_def(void) { return FA.have_def ? &FA.def : NULL; }
FireAnimState *game_fire_state(void) { return &FA.st; }

// Engine Fidelity: the perceptual punches (FOV punch, roll twitch, blur spike, chroma) are off at
// Low and below, reduced at Medium. The gun / hands / camera motion itself is gameplay feel: always on.
static float perceptual_scale(void) {
    float k = FA.punch ? FA.punch->f : 1.0f;
    if (!fid_loaded()) return k;
    int lv = fid_level();
    return lv <= FID_LOW ? 0.0f : (lv == FID_MEDIUM ? 0.6f * k : k);
}

// Muzzle flash on the gun's final transform: the shot only records it; it is spawned once this
// frame's first-person item (kick applied) is placed, at its muzzle along its barrel.
static struct { bool pending; int ticks; vec3 dir; int type, device; } g_flash;
static void local_flash(vec3 dir, int type, int device) {
    g_flash.pending = true;
    g_flash.ticks = 0;
    g_flash.dir = dir;
    g_flash.type = type;
    g_flash.device = device;
}

void game_fire_flush_flash(void) {
    if (FA.debug && FA.debug->i >= 2) {
        // Where the first-person gun actually is this frame (camera space): the muzzle and the grip.
        Xform ic = chars_fp_item_cam_raw();
        const WeaponDef *d = weapon_equipped_def(G->world, game_view_slot());
        vec3 mz = d ? xf_point(ic, d->sockets.muzzle) : v3(0, 0, 0);
        log_info("fire_anim: frame %llu muzzle %.4f %.4f %.4f grip %.4f %.4f %.4f | kick %.2f deg %.1f mm hand %.1f mm cam %.2f fov %.2f",
                 (unsigned long long)g_engine.frame_index, mz.x, mz.y, mz.z, ic.t.x, ic.t.y, ic.t.z, FA.frame.gun_rot.x, FA.frame.gun_pos.z * 1000.0f,
                 v3_len(FA.frame.hand_pos) * 1000.0f, FA.frame.cam_rot.x, FA.frame.fov_punch);
    }
    if (!g_flash.pending) return;
    g_flash.pending = false;
    vec3 mp, mf;
    if (hands_muzzle(&mp, &mf)) fx_muzzle_ex(v3_madd(mp, mf, 0.015f), mf, g_flash.type, g_flash.device, true);
    else {
        Player *p = game_view_player();
        if (p) fx_muzzle_ex(v3_madd(player_eye_pos(&p->sim.move), g_flash.dir, 0.6f), g_flash.dir, g_flash.type, g_flash.device, true);
    }
}

void game_fire_reset(void) {
    fire_anim_reset(&FA.st);
    memset(&FA.out, 0, sizeof(FA.out));
    memset(&FA.frame, 0, sizeof(FA.frame));
    chars_set_fp_fire(NULL, NULL);
}

void game_fire_on_shot(const ShotEvent *ev) {
    if (!G || ev->slot != game_view_slot() || !ev->def || ev->def->kind == WPN_MELEE) return;
    FA.def = ev->def->fire;
    FA.have_def = true;
    FireShot sh;
    memset(&sh, 0, sizeof(sh));
    sh.ads_t = ev->ads_t;
    sh.sub_tick = ev->sub_tick;
    sh.seed = ev->seed;
    sh.mag_left = ev->mag_left;
    sh.interval = ev->interval;
    sh.scale = FA.scale ? FA.scale->f : 1.0f;
    fire_anim_shot(&FA.st, &FA.def, &sh);
    FA.shots++;
    FA.rec_shot = true;
    if (FA.debug && FA.debug->i) log_info("fire_anim: shot %u sub %.2f ads %.2f mag %d", FA.shots, ev->sub_tick, ev->ads_t, ev->mag_left);
}

static void record(const FireAnimOut *o, bool held) {
    FireRecord *r = &FA.rec[FA.rec_pos];
    r->gun_pitch = o->gun_rot.x;
    r->gun_back = o->gun_pos.z;
    r->hand_pitch = o->gun_rot.x + o->hand_rot.x;
    r->cam_pitch = o->cam_rot.x;
    r->fov = o->fov_punch;
    r->shot = FA.rec_shot;
    r->held = held;
    FA.rec_shot = false;
    FA.rec_pos = (FA.rec_pos + 1) % REC_N;
    if (FA.rec_count < REC_N) FA.rec_count++;
    FA.rec_tick++;
}

int game_fire_record(FireRecord *out, int max) {
    int n = DG_MIN(max, FA.rec_count);
    for (int i = 0; i < n; i++) out[i] = FA.rec[(FA.rec_pos - n + i + REC_N * 2) % REC_N];
    return n;
}

void game_fire_tick(void) {
    // No first-person frame placed the gun (third-person camera): spawn the flash anyway.
    if (g_flash.pending && ++g_flash.ticks >= 2) game_fire_flush_flash();
    Player *p = game_view_player();
    int vs = game_view_slot();
    const WeaponDef *d = p ? weapon_equipped_def(G->world, vs) : NULL;
    if (d && d->kind != WPN_MELEE) {
        FA.def = d->fire;   // the effective definition: attachments' mass and modifiers included
        FA.have_def = true;
    }
    bool held = p && vs == G->local_slot ? (G->last_cmd.buttons & BTN_ATTACK) != 0 : true;
    if (!FA.have_def) {
        chars_set_fp_fire(NULL, NULL);
        return;
    }
    fire_anim_step(&FA.st, &FA.def, SIM_DT, held, perceptual_scale(), &FA.out);
    // Casing: out of the ejection port when the slide / bolt comes back (not on the shot).
    if (fire_anim_take_eject(&FA.st) && p && d && d->kind == WPN_HITSCAN) {
        vec3 mp, mf;
        vec3 eye = player_eye_pos(&p->sim.move);
        vec3 r = right_from_yaw(p->sim.move.yaw);
        vec3 port = hands_muzzle(&mp, &mf) ? v3_madd(mp, mf, -0.5f * fmaxf(0.2f, d->fire.length)) : v3_add(eye, v3_add(v3_scale(r, 0.12f), dir_from_angles(p->sim.move.yaw, p->sim.move.pitch)));
        fx_shell_ex(v3_madd(port, r, 0.02f), v3_add(v3_scale(r, 2.2f), v3(0, 1.4f, 0)), d->vfx_caliber);
    }
    chars_set_fp_fire(&FA.out, NULL);
    chars_set_fp_fire_snap(FA.rec_shot);
    record(&FA.out, held);
}

const FireAnimOut *game_fire_sample(float alpha) {
    // The shot's tick shows its own output at once (the kick, punch and blur land with the flash).
    fire_anim_sample(&FA.st, &FA.out, FA.rec[(FA.rec_pos + REC_N - 1) % REC_N].shot ? 1.0f : alpha, &FA.frame);
    chars_set_fp_fire(NULL, &FA.frame);
    return &FA.frame;
}

void game_fire_post(float *blur, float *chroma) {
    *blur = FA.frame.blur;
    *chroma = FA.frame.chroma;
}

// ------------------------------------------------------------------ sway_check

// Sway must be bit-identical for every weapon and attachment combination: run the hold point
// (the same evaluation the body uses) with every weapon's effective sockets and the kick
// parameters the character builds, with the same scripted input (600 ticks of idle, walk, ADS),
// and compare its global sway output. An optic scales the ADS sway by its own (global) steadiness,
// so combinations are compared per optic: the same optic sways the same on every weapon.
static void sway_run(const WeaponDef *eff, const AttachmentDef *opt, int mode, float *out9, int n) {
    HoldPointStyle style;
    holdpoint_style_defaults(&style, eff->hold_style);
    HoldPointState st;
    memset(&st, 0, sizeof(st));
    for (int i = 0; i < n; i++) {
        HoldPointInput in;
        memset(&in, 0, sizeof(in));
        in.style = &style;
        in.ovr = &eff->sockets.hp;
        in.anchor = xf_make(eff->sockets.anchor.t, quat_identity());
        in.sight = eff->sockets.sight;
        in.pivot = eff->sockets.pivot;
        in.length = eff->sockets.length;
        in.has_item = true;
        in.ads = mode == 2;
        in.dt = SIM_DT;
        in.phase_valid = true;
        in.clock = i * SIM_DT;
        in.stride = mode == 1 ? i * SIM_DT * 2.2f : 0.0f;
        in.aim_yaw = sinf(i * 0.05f) * 0.4f;
        in.aim_pitch = cosf(i * 0.031f) * 0.2f;
        in.vel_cam = mode == 1 ? v3(0, 0, -4.0f) : v3(0, 0, 0);
        in.grounded = true;
        in.cam_world = xf_identity();
        in.sway_scale = 1.0f;
        in.bob_scale = 1.0f;
        in.kick_params.set = true;
        in.kick_params.sway_set = true;
        in.kick_params.optic_sway = opt && opt->sway_scale > 0 ? opt->sway_scale : 1.0f;
        in.kick_params.breath_k = 1.0f;
        in.kick_params.stance = 1.0f;
        in.kick_params.ads_sway_mult = 1.0f;
        HoldPointOutput o;
        memset(&o, 0, sizeof(o));
        holdpoint_eval(&st, &in, &o);
        float *v = out9 + i * 9;
        v[0] = o.sway_hip_rot.x; v[1] = o.sway_hip_rot.y; v[2] = o.sway_hip_rot.z;
        v[3] = o.sway_hip_pos.x; v[4] = o.sway_hip_pos.y; v[5] = o.sway_hip_pos.z;
        v[6] = o.sway_ads_rot.x; v[7] = o.sway_ads_rot.y; v[8] = o.sway_ads_rot.z;
    }
}

static FaMetrics fire_metrics_of(const FireAnimDef *d, float *peak_back, float *cam_lag) {
    static float pitch[240], back[240], cam[240];
    FireAnimState s;
    fire_anim_reset(&s);
    FireShot sh = { 0, 0, 99, 10, 0, 1.0f };
    for (int i = 0; i < 240; i++) {
        if (i == 2) fire_anim_shot(&s, d, &sh);
        FireAnimOut o;
        fire_anim_step(&s, d, SIM_DT, false, 1.0f, &o);
        pitch[i] = o.gun_rot.x;
        back[i] = o.gun_pos.z;
        cam[i] = o.cam_rot.x;
    }
    FaMetrics m = fire_anim_metrics(pitch, 240, 0.05f);
    FaMetrics mb = fire_anim_metrics(back, 240, 0.05f);
    FaMetrics mc = fire_anim_metrics(cam, 240, 0.05f);
    *peak_back = mb.peak;
    *cam_lag = (float)(mc.peak_tick - m.peak_tick);
    return m;
}

static void cmd_sway_check(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!G) return;
    const ItemDB *db = &G->items;
    // Reference per optic (index 0 = no optic) x mode.
    static float ref[MAX_ATTACHMENTS + 1][3][REC_N * 9], cur[REC_N * 9];
    static bool have[MAX_ATTACHMENTS + 1];
    memset(have, 0, sizeof(have));
    int fails = 0, combos = 0, weapons = 0;
    for (int wi = 0; wi < db->weapon_count; wi++) {
        const WeaponDef *base = &db->weapons[wi];
        if (base->kind == WPN_MELEE) continue;
        weapons++;
        // The defaults, then every allowed attachment of every slot on its own.
        for (int variant = -1; variant <= db->attachment_count; variant++) {
            u8 att[ATT_SLOTS];
            attach_defaults(base, att);
            if (variant >= 1) {
                const AttachmentDef *ad = attach_def(db, (u8)variant);
                if (!ad || !attach_allowed(base, ad)) continue;
                att[ad->slot] = (u8)variant;
            } else if (variant == 0) {
                memset(att, 0, sizeof(att));
            }
            static WeaponDef eff;
            attach_weapon_effective(db, base, att, &eff);
            const AttachmentDef *opt = attach_optic(db, att);
            int key = att[ATT_OPTIC];
            for (int mode = 0; mode < 3; mode++) {
                float *dst = have[key] ? cur : ref[key][mode];
                sway_run(&eff, opt, mode, dst, REC_N);
                if (have[key] && memcmp(ref[key][mode], cur, sizeof(float) * REC_N * 9)) {
                    fails++;
                    log_warn("sway_check: %s (attachment variant %d) %s sway differs", base->id, variant, mode == 0 ? "idle" : mode == 1 ? "walk" : "ADS");
                }
            }
            have[key] = true;
            combos++;
        }
    }
    log_info("sway_check: %d weapons, %d weapon x attachment combinations x idle / walk / ADS over %d ticks: %s", weapons, combos, REC_N,
             fails ? "FAIL" : "PASS (bit-identical)");
    log_info("sway_check: fire motion per weapon (must differ):");
    for (int wi = 0; wi < db->weapon_count; wi++) {
        const WeaponDef *d = &db->weapons[wi];
        if (d->kind == WPN_MELEE) continue;
        float pb, cl;
        FaMetrics m = fire_metrics_of(&d->fire, &pb, &cl);
        log_info("  %-10s %-8s mass %4.1f kg  peak %5.2f deg  kick %.1f cm  settle %3d ticks  overshoot %3.0f%%  camera +%.0f ticks", d->id, d->fire.cls_name,
                 d->fire.mass, m.peak, pb * 100.0f, m.settle_tick - m.peak_tick, m.overshoot * 100.0f, cl);
    }
}

// ------------------------------------------------------------------ fire_compare / fire_bench

// fire_compare: every gun mag-dumped in turn (give, select, hold fire until empty, settle), logging
// the fire motion of each from the recorder. Runs from game_fire_frame.
static void cmd_fire_compare(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    FA.cmp_active = true;
    FA.cmp_weapon = -1;
    FA.cmp_phase = 0;
    FA.cmp_wait = 0;
    cbuf_add("infinite_ammo 0\n");
    log_info("fire_compare: mag-dumping every weapon");
}

static const WeaponDef *next_gun(int *wi) {
    const ItemDB *db = &G->items;
    for ((*wi)++; *wi < db->weapon_count; (*wi)++)
        if (db->weapons[*wi].kind == WPN_HITSCAN || db->weapons[*wi].kind == WPN_PROJECTILE) return &db->weapons[*wi];
    return NULL;
}

void game_fire_frame(void) {
    if (!FA.cmp_active || !G) return;
    if (FA.cmp_wait > 0) { FA.cmp_wait--; return; }
    char buf[128];
    switch (FA.cmp_phase) {
    case 0: {
        const WeaponDef *d = next_gun(&FA.cmp_weapon);
        if (!d) { FA.cmp_active = false; log_info("fire_compare: done"); cbuf_add("-attack\n"); return; }
        snprintf(buf, sizeof(buf), "-attack\ngive %s\nequip %s\n", d->id, d->id);
        cbuf_add(buf);
        FA.cmp_phase = 1;
        FA.cmp_wait = 45;
        FA.rec_count = 0;
        break;
    }
    case 1:
        cbuf_add("+attack\n");
        FA.cmp_phase = 2;
        FA.cmp_wait = 240;
        FA.shots = 0;
        break;
    case 2: {
        cbuf_add("-attack\n");
        FA.cmp_phase = 3;
        FA.cmp_wait = 90;
        break;
    }
    case 3: {
        const WeaponDef *d = &G->items.weapons[FA.cmp_weapon];
        static FireRecord r[REC_N];
        int n = game_fire_record(r, REC_N);
        float peak = 0, ride = 0, minv = 1e9f;
        int first = -1, last = -1;
        for (int i = 0; i < n; i++) {
            peak = fmaxf(peak, r[i].gun_pitch);
            if (r[i].shot) { if (first < 0) first = i; last = i; }
        }
        for (int i = first; i >= 0 && i <= last; i++) { ride = fmaxf(ride, r[i].gun_pitch); minv = fminf(minv, r[i].gun_pitch); }
        int settle = 0;
        for (int i = n - 1; i > last && last >= 0; i--) if (fabsf(r[i].gun_pitch) > 0.05f * peak) { settle = i - last; break; }
        log_info("fire_compare: %-10s %-8s %3u shots  peak %5.2f deg  lowest during the dump %5.2f deg (no reset)  release settle %3d ticks", d->id,
                 d->fire.cls_name, FA.shots, peak, first >= 0 ? minv : 0.0f, settle);
        FA.cmp_phase = 0;
        break;
    }
    }
}

// fire_bench: lay every gun x muzzle device out on the dev room's firing bench (the sighting lane):
// prints the fire metrics of each combination and gives the guns (muzzle devices via "attach").
static void cmd_fire_bench(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!G) return;
    const ItemDB *db = &G->items;
    log_info("fire_bench: every gun x muzzle device (hip, one shot)");
    for (int wi = 0; wi < db->weapon_count; wi++) {
        const WeaponDef *base = &db->weapons[wi];
        if (base->kind == WPN_MELEE) continue;
        for (int a = 0; a <= db->attachment_count; a++) {
            const AttachmentDef *ad = a ? attach_def(db, (u8)a) : NULL;
            if (a && (!ad || ad->slot != ATT_MUZZLE || !attach_allowed(base, ad))) continue;
            u8 att[ATT_SLOTS];
            attach_defaults(base, att);
            att[ATT_MUZZLE] = (u8)a;
            static WeaponDef eff;
            attach_weapon_effective(db, base, att, &eff);
            float pb, cl;
            FaMetrics m = fire_metrics_of(&eff.fire, &pb, &cl);
            log_info("  %-10s + %-12s mass %4.2f kg  pitch I %.4f  peak %5.2f deg  kick %.1f cm  settle %3d ticks  overshoot %3.0f%%", base->id,
                     ad ? ad->id : "bare", eff.fire.mass + (eff.fire.att_count ? 0 : 0), fire_anim_inertia(&eff.fire, FA_PITCH), m.peak, pb * 100.0f,
                     m.settle_tick - m.peak_tick, m.overshoot * 100.0f);
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "give %s\n", base->id);
        cbuf_add(buf);
    }
}

static void load_sway_profile(void) {
    SwayProfile prof;
    sway_profile_defaults(&prof);
    char buf[512];
    if (sway_profile_load(&prof, platform_data_path("assets/weapons/sway_global.json", buf, sizeof(buf)))) log_info("sway: global profile assets/weapons/sway_global.json");
    else log_warn("sway: assets/weapons/sway_global.json missing, built-in defaults");
    sway_set_global(&prof);
}

// fire_reload: the class files, the weapons' fire blocks and the global sway profile (live tuning).
static void cmd_fire_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    load_sway_profile();
    if (G && weapons_reload_defs(&G->items, "data/weapons")) log_info("fire_reload: fire classes, weapons and the sway profile reloaded");
}

void game_fire_init(void) {
    weapons_set_local_flash(local_flash);
    load_sway_profile();
    cmd_register("fire_reload", cmd_fire_reload, "reload the fire animation classes (assets/weapons/fire_poses), the weapons and the global sway profile");
    FA.scale = cvar_reg_float("wpn_fire_anim_scale", 1.0f, 0.0f, 3.0f, CVAR_ARCHIVE, "fire animation master scale (gun kick, hands, body, camera reaction; docs/FIRE_ANIM.md)");
    FA.punch = cvar_reg_float("cl_fire_punch", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "fire FOV punch / roll twitch / blur spike / chromatic tick (0 = off; also off at Low fidelity)");
    FA.debug = cvar_reg_int("wpn_fire_debug", 0, 0, 2, 0, "fire animation log: 1 every shot (sub-tick time, ADS, magazine), 2 also the first-person gun per frame");
    cmd_register("sway_check", cmd_sway_check, "assert the global sway is bit-identical for every weapon x attachment (idle, walk, ADS) and list the fire motion per weapon");
    cmd_register("fire_compare", cmd_fire_compare, "mag-dump every gun in turn and log its fire motion (peak, no reset, release settle)");
    cmd_register("fire_bench", cmd_fire_bench, "fire motion of every gun x muzzle device (mass, inertia, peak, settle) and give every gun");
    game_fire_reset();
}
