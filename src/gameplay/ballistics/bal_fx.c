// Ballistics visuals: composed muzzle flashes, tracers, bullet traces, vapour trails
// (docs/BALLISTICS.md "Muzzle flash" / "Tracers").
#include "gameplay/ballistics/bal_fx.h"
#include "gameplay/ballistics/bal_world.h"
#include "gameplay/ballistics/bal_tracer.h"
#include "gameplay/ballistics/bal_muzzle.h"
#include "gameplay/weapons.h"
#include "gameplay/attachments.h"
#include "vfx/vfx.h"
#include "vfx/vfx_library.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "physics/physics.h"
#include "game/game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>


static struct {
    BalMuzzleHeat heat[MAX_PLAYERS + 1];
    // Per pool slot: the bullet the slot's smoke trail belongs to and its effect handle.
    u32 smoke_key[BAL_POOL];
    u32 smoke_h[BAL_POOL];
    int em_head, em_tail;
    u32 lib_rev;
    Cvar *vfx_quality, *ignition, *shutter, *trace, *vapor, *timescale;
    Cvar *scale_dist, *scale_min;
    double next_trace;
    // muzzle_preview: a flash every 0.8 s in front of the camera, seen from the side, in slow motion.
    bool preview;
    int preview_cart, preview_dev;
    float preview_barrel;
    double preview_next;
    bool preview_shot;   // the preview's flash: always shown, its own size
    Rng flash_rng;       // the flash's per-shot variation (visual only, never replicated)
} F;

// The flash varies shot to shot (docs/BALLISTICS.md "Muzzle flash"): the flame and its light show on
// FLASH_CHANCE of the shots, each at its own size (1 +- FLASH_SIZE_VAR); the smoke, vapour, sparks,
// blast and dust come with every shot.
#define FLASH_CHANCE 0.7f
#define FLASH_SIZE_VAR 0.3f

static float cv(Cvar **c, const char *name, float def) {
    if (!*c) *c = cvar_find(name);
    return *c ? (*c)->f : def;
}

// ------------------------------------------------------------------ muzzle flash

static u32 spawn_part(const char *effect, vec3 pos, vec3 dir, const BalFlashParams *c, float along, bool viewmodel, float scale, float count) {
    if (c->intensity < 0.01f || scale <= 0.0f) return 0;
    VfxSpawnParams sp = vfx_spawn_params(v3_madd(pos, dir, along));
    sp.rot = vfx_rot_from_dir(dir);
    sp.scale = scale;
    sp.intensity = c->intensity;
    sp.count_mul = count > 0.0f ? count : 1.0f;
    sp.tint = v4(c->rgb.x, c->rgb.y, c->rgb.z, 1.0f);
    sp.viewmodel = viewmodel;
    sp.socket = viewmodel ? 1 : 0;
    Player *p = viewmodel ? game_local_player() : NULL;
    if (p) sp.velocity = p->sim.move.vel;
    u32 h = vfx_game_spawn(effect, &sp);
    if (viewmodel && h) vfx_game_track_local_muzzle(h, along);
    return h;
}

void bal_fx_muzzle(int slot, int cart, int device, float barrel, vec3 pos, vec3 dir, bool viewmodel) {
    const BalData *d = bal_data();
    if (cart < 0 || cart >= d->cart_count) return;
    const BalCartridge *c = &d->cart[cart];
    const BalDevice *dev = &d->dev[DG_CLAMP(device, 0, d->dev_count - 1)];
    BalMuzzleHeat *h = &F.heat[DG_CLAMP(slot, 0, MAX_PLAYERS)];
    double now = platform_time();
    bool pop = bal_muzzle_heat_shot(h, c, dev, now);
    // Near a surface: the blast kicks up dust from the ground / a wall within half a metre.
    World *w = game_world();
    bool near = false;
    RayHit rh;
    memset(&rh, 0, sizeof(rh));
    if (w && w->phys) {
        near = phys_raycast(w->phys, pos, v3_scale(dir, 0.5f), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh) ||
               phys_raycast(w->phys, pos, v3(0, -0.6f, 0), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh);
    }
    BalMuzzleIn in;
    memset(&in, 0, sizeof(in));
    in.cart = c;
    in.dev = dev;
    in.barrel = barrel;
    in.speed = bal_cartridge_mv(c, barrel) * dev->velocity_mul;
    in.heat = bal_muzzle_heat_at(h, now);
    in.can_heat = bal_muzzle_can_heat_at(h, dev, now);
    in.near_surface = near;
    in.pop = pop;
    in.exposure_gain = 1.0f;   // the renderer applies the exposure gain to every exposure-aware emitter
    BalMuzzleOut o;
    bal_muzzle_compose(&in, &o);
    const BalFlashParams *k = o.c;
    // This shot's flame: shown or not, and its size (0: none; the light goes with it).
    if (!F.flash_rng.inc) F.flash_rng = rng_make((u64)(now * 1e6));
    float fs = rng_float(&F.flash_rng) < FLASH_CHANCE ? rng_range(&F.flash_rng, 1.0f - FLASH_SIZE_VAR, 1.0f + FLASH_SIZE_VAR) : 0.0f;
    if (F.preview_shot) fs = 1.0f;
    spawn_part("muzzle_primary", pos, dir, &k[BFC_PRIMARY], 0.0f, viewmodel, k[BFC_PRIMARY].size * fs, 1);
    spawn_part("muzzle_mach", pos, dir, &k[BFC_MACH], k[BFC_MACH].dist, viewmodel, k[BFC_MACH].size * fs, 1);
    {
        int ticks = DG_CLAMP((int)lrintf(k[BFC_SECONDARY].life * 60.0f), 1, 3);
        char name[32];
        snprintf(name, sizeof(name), "muzzle_secondary_%d", ticks);
        spawn_part(name, pos, dir, &k[BFC_SECONDARY], k[BFC_SECONDARY].dist, viewmodel, k[BFC_SECONDARY].size * fs, 1);
    }
    spawn_part("muzzle_prongs", pos, dir, &k[BFC_PRONGS], k[BFC_PRONGS].dist, viewmodel, k[BFC_PRONGS].size * fs, (float)DG_MAX(dev->prongs, 3));
    if (k[BFC_VENTS].intensity > 0.01f && dev->port_count) {
        vec3 right = v3_cross(dir, v3(0, 1, 0));
        right = v3_len2(right) > 1e-6f ? v3_norm(right) : v3(1, 0, 0);
        vec3 up = v3_cross(right, dir);
        for (int i = 0; i < dev->port_count; i++) {
            vec3 pd = dev->ports[i];
            vec3 wd = v3_norm(v3_add(v3_add(v3_scale(right, pd.x), v3_scale(up, pd.y)), v3_scale(dir, -pd.z)));
            spawn_part("muzzle_vent", v3_madd(pos, dir, -0.02f), wd, &k[BFC_VENTS], 0.0f, false, k[BFC_VENTS].size * fs, 1);
        }
    }
    spawn_part("muzzle_vapor", pos, dir, &k[BFC_VAPOR], 0.0f, false, k[BFC_VAPOR].size, 1);
    {
        BalFlashParams sm = k[BFC_SMOKE];
        sm.intensity *= 2.5f;   // the effect's alpha is authored for a 0.4 puff
        spawn_part("muzzle_smoke", pos, dir, &sm, sm.dist, false, sm.size, 1);
    }
    spawn_part("muzzle_sparks", pos, dir, &k[BFC_SPARKS], 0.0f, false, 1.0f, k[BFC_SPARKS].count);
    spawn_part("muzzle_blast", pos, dir, &k[BFC_BLAST], k[BFC_BLAST].dist, false, k[BFC_BLAST].size, 1);
    {
        bool shadow = cv(&F.vfx_quality, "vfx_quality", 2) >= 3.0f;
        BalFlashParams lt = k[BFC_LIGHT];
        lt.rgb = v3(1, 1, 1);
        spawn_part(shadow ? "muzzle_light_shadow" : "muzzle_light", pos, dir, &lt, lt.dist, viewmodel, o.light_radius * fminf(fs, 1.0f), 1);
    }
    if (o.dust && near) {
        BalFlashParams du = k[BFC_BLAST];
        du.intensity = 1.0f;
        du.rgb = v3(1, 1, 1);
        spawn_part("muzzle_dust", rh.point, rh.normal, &du, 0.02f, false, fmaxf(o.dust_scale * 0.6f, 0.3f), 1);
    }
    if (o.haze) {
        BalFlashParams hz = k[BFC_SMOKE];
        hz.intensity = 1.0f;
        hz.rgb = v3(1, 1, 1);
        if (dev->kind == BAL_DEV_SUPPRESSOR) spawn_part("muzzle_can_shimmer", pos, dir, &hz, -0.08f, false, 1.0f, 1);
        else spawn_part("muzzle_haze", pos, dir, &hz, 0.05f, false, 0.1f, 1);
    }
}

void bal_fx_muzzle_weapon(int slot, const WeaponDef *d, vec3 pos, vec3 dir, bool viewmodel) {
    int ci = bal_weapon_cartridge(d);
    if (ci < 0) return;
    bal_fx_muzzle(slot, ci, bal_weapon_device(d), d->barrel_length, pos, dir, viewmodel);
}

// Old callers (VfxMuzzleType x VfxMuzzleDevice: the dev pedestal, crossbow-less guns without a
// cartridge): a representative cartridge, barrel and device per type.
static u32 compose_legacy(vec3 pos, vec3 dir, int type, int device, bool viewmodel) {
    static const char *carts[VFX_MUZZLE_TYPE_COUNT] = { "9x19_fmj", "9x19_fmj", "556_m855", "762x51_m80", "12ga_00buck", "762x51_m80" };
    static const float barrels[VFX_MUZZLE_TYPE_COUNT] = { 0.102f, 0.225f, 0.368f, 0.56f, 0.47f, 0.61f };
    static const char *devs[4] = { "bare", "flash_hider", "compensator", "suppressor" };
    int t = DG_CLAMP(type, 0, VFX_MUZZLE_TYPE_COUNT - 1);
    const BalData *d = bal_data();
    int ci = bal_cartridge_find(d, carts[t]);
    if (ci < 0) return 0;
    Player *p = viewmodel ? game_local_player() : NULL;
    int slot = p ? (int)(p - game_world()->players) : MAX_PLAYERS;
    bal_fx_muzzle(slot, ci, bal_device_find(d, devs[DG_CLAMP(device, 0, 3)]), barrels[t], pos, dir, viewmodel);
    return 1;
}

static void cmd_muzzle_preview(int argc, const char **argv) {
    Cvar *ts = cvar_find("vfx_timescale");
    if (argc < 2 || !strcmp(argv[1], "off")) {
        F.preview = false;
        if (ts) cvar_set_float(ts, 1.0f);
        log_info("muzzle_preview: off");
        return;
    }
    World *w = game_world();
    const WeaponDef *d = NULL;
    for (int i = 0; w && i < w->items->weapon_count; i++)
        if (!strcmp(w->items->weapons[i].id, argv[1])) d = &w->items->weapons[i];
    int ci = d ? bal_weapon_cartridge(d) : -1;
    if (ci < 0) { log_info("muzzle_preview: '%s' is not a gun with a cartridge", argv[1]); return; }
    F.preview = true;
    F.preview_cart = ci;
    F.preview_barrel = d->barrel_length;
    F.preview_dev = bal_device_find(bal_data(), argc > 2 ? argv[2] : d->muzzle_device_id);
    F.preview_next = 0;
    if (ts) cvar_set_float(ts, argc > 3 ? (float)atof(argv[3]) : 0.05f);
    log_info("muzzle_preview: %s (%s, %.0f mm barrel) with %s", d->id, bal_data()->cart[ci].id, d->barrel_length * 1000.0f,
             bal_data()->dev[F.preview_dev].id);
}

static void preview_tick(World *w, vec3 cam) {
    if (!F.preview) return;
    double now = platform_time();
    if (now < F.preview_next) return;
    F.preview_next = now + 0.8;
    Player *p = game_local_player();
    if (!p) return;
    vec3 fwd = dir_from_angles(p->sim.move.yaw, p->sim.move.pitch);
    vec3 right = right_from_yaw(p->sim.move.yaw);
    (void)w;
    F.preview_shot = true;
    bal_fx_muzzle(MAX_PLAYERS, F.preview_cart, F.preview_dev, F.preview_barrel, v3_madd(v3_madd(cam, fwd, 1.6f), right, -0.35f), right, false);
    F.preview_shot = false;
}

void bal_fx_init(void) {
    cmd_register("muzzle_preview", cmd_muzzle_preview, "muzzle_preview <weapon> [device] [timescale] | off: a composed muzzle flash every 0.8 s in front of the camera, seen from the side, in slow motion (vfx_timescale)");
    F.em_head = F.em_tail = -1;
    vfx_game_set_muzzle_composer(compose_legacy);
    F.trace = cvar_reg_bool("bal_bullet_trace", true, CVAR_LOCKED, "bullet trace: the heat / shock ripple along supersonic bullets, seen through magnified optics");
    F.vapor = cvar_reg_bool("bal_vapor_trails", true, CVAR_LOCKED, "vapour trails behind supersonic bullets in humid or freezing air");
}

void bal_fx_clear(void) {
    memset(F.smoke_key, 0, sizeof(F.smoke_key));
    memset(F.smoke_h, 0, sizeof(F.smoke_h));
    memset(F.heat, 0, sizeof(F.heat));
}

// ------------------------------------------------------------------ fire hook

void fire_try_ignite(World *w, vec3 pos, float energy) {
    (void)w;
    // Stub: the foliage has no burning state yet. A real fire would start here (dry grass, a
    // tracer's burning compound or a hot fragment).
    log_info("fire: ignition attempt at (%.1f %.1f %.1f), %.0f J (no foliage fire yet)", pos.x, pos.y, pos.z, energy);
    dbg_sphere(pos, 0.3f, DBG_ORANGE, 5.0f);
}

// ------------------------------------------------------------------ tracers

static u32 bullet_key(const BalLive *l) { return ((u32)l->shot_id << 16) | ((u32)l->b.pellet << 8) | (u32)l->role | 0x80000000u; }

static bool magnified_ads(World *w) {
    Player *p = game_view_player();
    if (!p || game_ads_vis() < 0.9f) return false;
    const InvSlot *s = weapon_equipped(w, (int)(p - w->players));
    if (!s) return false;
    const AttachmentDef *o = attach_optic(w->items, s->att);
    return o && o->zoom >= 3.0f;
}

// One tracer for this frame: the velocity-stretched head ending at l->b.pos and the curved tail. It
// glows (emissive, bloom) and lights nothing. Returns its look (intensity 0: dark, nothing drawn).
static BalTracerLook draw_tracer(const BalLive *l, vec3 cam, float shutter, bool arcade) {
    const BalCartridge *c = &bal_data()->cart[l->b.cart];
    BalTracerLook look = bal_tracer_look(&c->tracer, l->b.dist, l->b.time, arcade);
    if (look.intensity <= 0.0f) return look;
    // Where it is drawn: the true path, starting at the visible muzzle and blending onto the bullet
    // (which left the camera) over the first 15 m.
    float blend = 1.0f - saturate(l->b.dist / 15.0f);
    vec3 head = v3_madd(l->b.pos, l->visual_offset, blend);
    float speed = bal_bullet_speed(&l->b);
    vec3 dir = speed > 1e-3f ? v3_scale(l->b.vel, 1.0f / speed) : v3(0, 0, -1);
    float cam_d = v3_dist(head, cam);
    float k = look.intensity;
    vec3 rgb = bal_tracer_rgb(c->tracer.color);
    vec4 col = v4(rgb.x * k, rgb.y * k, rgb.z * k, 1.0f);
    // Width: 6 cm times a scale growing with the camera distance (scale 1 at bal_tracer_scale_dist),
    // never below bal_tracer_scale_min and with no upper limit: a tracer keeps its size on screen at
    // any range.
    float scale = fmaxf(cam_d / fmaxf(cv(&F.scale_dist, "bal_tracer_scale_dist", 6.0f), 0.01f), cv(&F.scale_min, "bal_tracer_scale_min", 1.0f));
    float width = 0.06f * scale;
    float streak = fminf(bal_tracer_streak(speed, shutter), l->b.dist);   // never behind the muzzle
    if (F.em_head >= 0) vfx_game_push_sprite(F.em_head, v3_madd(head, dir, -streak), v3_scale(dir, streak), width, col);
    // Tail: the last tick positions (curved by gravity / wind), fading.
    if (F.em_tail >= 0 && look.state != BAL_TRS_IGNITER) {
        vec3 prev = v3_madd(head, dir, -streak);
        for (int t = 1; t < l->trail_count; t++) {
            vec3 p = l->trail[t];
            if (v3_dist(p, l->launch) < 15.0f) break;   // the first metres: the head's blend from the muzzle
            float f = 1.0f - (float)t / (float)BAL_TRAIL;
            vec4 tc = v4(col.x * 0.45f * f, col.y * 0.45f * f, col.z * 0.45f * f, 1.0f);
            vfx_game_push_sprite(F.em_tail, p, v3_sub(prev, p), width * 0.7f, tc);
            prev = p;
        }
    }
    return look;
}

void bal_fx_frame(World *w, vec3 cam, float dt) {
    if (!w || !vfx_game_active()) return;
    preview_tick(w, cam);
    VfxLibrary *lib = vfx_game_library();
    if (F.lib_rev != lib->revision || F.em_head < 0) {
        F.lib_rev = lib->revision;
        F.em_head = vfx_game_emitter("tracer_head", 0);
        F.em_tail = vfx_game_emitter("tracer_head", 1);
    }
    bool arcade = cv(&F.ignition, "bal_tracer_ignition", 1) == 0.0f;
    // Shutter: the frame's time in the simulation's clock (slow motion shortens the streak).
    float shutter = cv(&F.shutter, "shutter_preview", 0);
    if (shutter <= 0.0f) shutter = clampf(dt, 1.0f / 240.0f, 1.0f / 20.0f) * cv(&F.timescale, "host_timescale", 1.0f);
    bool scope = F.trace && F.trace->i && magnified_ads(w);
    bool vapor = false;
    if (F.vapor && F.vapor->i) {
        WeatherLocal wl = weather_at(w, cam);
        float humidity = saturate(0.4f + 0.5f * wl.fog + 0.4f * wl.rain);
        vapor = humidity > 0.8f || wl.temperature < 0.0f;
    }
    double now = platform_time();
    bool trace_tick = now >= F.next_trace;
    if (trace_tick) F.next_trace = now + 1.0 / 30.0;
    int count;
    const BalLive *pool = bal_world_bullets(&count);
    for (int i = 0; i < count; i++) {
        const BalLive *l = &pool[i];
        u32 key = bullet_key(l);
        bool alive = !(l->b.flags & BF_DEAD);
        if (F.smoke_key[i] != key || !alive) {
            if (F.smoke_h[i]) vfx_sys_stop(vfx_game_system(), F.smoke_h[i]);
            F.smoke_h[i] = 0;
            F.smoke_key[i] = alive ? key : 0;
        }
        if (!alive) continue;
        // The bullet itself (the optic's shock ripple follows the true path).
        vec3 head = l->b.pos;
        float speed = bal_bullet_speed(&l->b);
        vec3 dir = speed > 1e-3f ? v3_scale(l->b.vel, 1.0f / speed) : v3(0, 0, -1);
        float cam_d = v3_dist(head, cam);
        // Through a magnified optic: the supersonic shock / heat ripple of any bullet.
        if (scope && trace_tick && (l->b.flags & BF_SUPERSONIC_EVER) && cam_d < 600.0f && l->b.dist > 5.0f) {
            VfxSpawnParams sp = vfx_spawn_params(head);
            sp.velocity = v3_scale(dir, 0.5f);
            vfx_game_spawn("bullet_trace", &sp);
        }
        if (vapor && trace_tick && speed > 340.0f && cam_d < 200.0f && l->b.dist > 3.0f) {
            VfxSpawnParams sp = vfx_spawn_params(head);
            vfx_game_spawn("bullet_vapor", &sp);
        }
        if (!(l->b.flags & BF_TRACER)) continue;
        BalTracerLook look = draw_tracer(l, cam, shutter, arcade);
        if (look.intensity <= 0.0f) {
            if (F.smoke_h[i]) { vfx_sys_stop(vfx_game_system(), F.smoke_h[i]); F.smoke_h[i] = 0; }
            continue;
        }
        // Smoke trail of the burning compound.
        if (look.state == BAL_TRS_BURNING) {
            if (!F.smoke_h[i]) {
                VfxSpawnParams sp = vfx_spawn_params(head);
                sp.rot = vfx_rot_from_dir(dir);
                sp.socket = 1;
                F.smoke_h[i] = vfx_game_spawn("tracer_smoke", &sp);
            } else {
                vfx_sys_set_transform(vfx_game_system(), F.smoke_h[i], head, vfx_rot_from_dir(dir), l->b.vel);
            }
        }
    }
    // Tracers that stopped since the last frame: their last streak, up to the impact (hits closer
    // than one tick's flight are otherwise never drawn).
    const BalLive *ended = bal_world_ended_tracers(&count);
    for (int i = 0; i < count; i++) draw_tracer(&ended[i], cam, shutter, arcade);
}
