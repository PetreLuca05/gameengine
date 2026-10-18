#include "weapons.h"
#include "player/sway.h"
#include "attachments.h"
#include "health.h"
#include "effects.h"
#include "flesh_fx.h"
#include "throwables.h"
#include "ballistics/bal_world.h"
#include "ballistics/bal_fx.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "render/debug_draw.h"
#include "render/render.h"
#include "player/hands.h"
#include "gameplay/characters.h"
#include "gameplay/aim.h"
#include "game/game.h"
#include "vfx/vfx_library.h"
#include <math.h>
#include <stdlib.h>

static WeaponCvars WC;

static void cmd_infinite_ammo(int argc, const char **argv) {
    World *w = game_world();
    if (w && !w->authority) { log_info("infinite_ammo: host only"); return; }
    bool on = argc > 1 ? atoi(argv[1]) != 0 : !WC.infinite_ammo->i;
    cvar_set_cv(WC.infinite_ammo, on ? "1" : "0");
    log_info("infinite ammo %s", on ? "ON" : "off");
}

static AimStats g_aim_stats;
const AimStats *weapons_aim_stats(void) { return &g_aim_stats; }
void weapons_aim_stats_reset(void) { memset(&g_aim_stats, 0, sizeof(g_aim_stats)); }

static void cmd_aim_stats(int argc, const char **argv) {
    if (argc > 1 && !strcmp(argv[1], "reset")) { weapons_aim_stats_reset(); log_info("aim_stats: reset"); return; }
    const AimStats *a = &g_aim_stats;
    log_info("aim_stats: %u shots, %u muzzle claims checked, %u clamped (worst %.1f mm), %u aim points checked, %u mismatched (worst %.2f m), "
             "%u muzzles inside geometry, %u trigger pulls blocked, %u camera claims checked, %u clamped (worst %.1f mm)",
             a->shots, a->checked, a->clamped, a->max_error * 1000.0f, a->p_checked, a->p_mismatch, a->max_p_error, a->recovered, a->blocked, a->eye_checked,
             a->eye_clamped, a->max_eye_error * 1000.0f);
}

static void cmd_aim_report(int argc, const char **argv) {
    (void)argc; (void)argv;
    World *w = game_world();
    Game *g = game();
    int slot = game_view_slot();
    if (!w || !g || slot < 0) { log_info("aim_report: no player"); return; }
    AimSolve a;
    if (!weapon_aim_preview(w, slot, player_eye_pos(&w->players[slot].sim.move), g->view.yaw, g->view.pitch, game_view_cmd(), &a)) { log_info("aim_report: no gun"); return; }
    vec2 px = v2(0, 0);
    bool on = render_world_to_screen(a.q, &px);
    log_info("aim_report: class %s P %.3f %.3f %.3f (%s, %.2f m) muzzle %.3f %.3f %.3f origin %.3f %.3f %.3f%s Q %.3f %.3f %.3f (%.3f m) "
             "dir %.4f %.4f %.4f Q screen %.1f %.1f%s render %dx%d",
             aim_class_name(a.cls), a.p.x, a.p.y, a.p.z, a.p_hit ? "hit" : "far", v3_dist(a.eye, a.p), a.muzzle.x, a.muzzle.y, a.muzzle.z,
             a.origin.x, a.origin.y, a.origin.z, a.recovered ? " (recovered)" : "", a.q.x, a.q.y, a.q.z, a.q_dist, a.dir.x, a.dir.y, a.dir.z,
             px.x, px.y, on ? "" : " (behind)", render_width(), render_height());
    log_info("aim_report: eye %.3f %.3f %.3f render camera %.3f %.3f %.3f (%.1f mm)", a.eye.x, a.eye.y, a.eye.z, g->view.eye.x, g->view.eye.y, g->view.eye.z,
             v3_dist(a.eye, g->view.eye) * 1000.0f);
}

void weapons_register_cvars(void) {
    WC.recoil_scale = cvar_reg_float("wpn_recoil_scale", 1.0f, 0.0f, 5.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "aim recoil multiplier (gameplay, host)");
    WC.spread_scale = cvar_reg_float("wpn_spread_scale", 1.0f, 0.0f, 5.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "bullet spread cone multiplier (gameplay, host)");
    WC.visual_scale = cvar_reg_float("wpn_visual_scale", 1.0f, 0.5f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "camera + viewmodel kick multiplier (cosmetic)");
    WC.sway_scale = cvar_reg_float("wpn_sway_scale", 1.0f, 0.0f, 3.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "weapon sway: aim sway (moves the view + bullets), the item's look lag / idle sway / breathing / sprint bob");
    WC.kick_scale = cvar_reg_float("wpn_kick_scale", 1.0f, 0.0f, 3.0f, CVAR_LOCKED, "visual recoil kick of the held item on its hold point (aim recoil is separate)");
    WC.ads_sway = cvar_reg_float("wpn_ads_sway", 0.0f, 0.0f, 10.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "aim sway that moves the reticle off the screen centre while aimed (x the weapon's idle sway; 0 = the reticle stays centred, the gun sways about it)");
    WC.sway_ads_scale = cvar_reg_float("wpn_sway_ads_scale", 1.0f, 0.0f, 3.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "ADS weapon sway: x the weapons' ADS sway multipliers (quick tuning)");
    WC.sway_cosmetic = cvar_reg_float("cl_weapon_sway", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "weapon sway (cosmetic: breathing, look lag, bob, landing of the held weapon)");
    WC.ads_bob = cvar_reg_float("wpn_ads_bob", 0.0f, 0.0f, 3.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "aim bob that moves the reticle while walking aimed (bullets follow it; 0 = the gun bobs about the reticle instead)");
    WC.bob_scale = cvar_reg_float("wpn_bob_scale", 1.0f, 0.0f, 3.0f, CVAR_LOCKED, "item bob while walking / running (x the weapon sway bob; sprinting always bobs)");
    WC.recoil_debug = cvar_reg_int("wpn_recoil_debug", 0, 0, 2, CVAR_LOCKED, "recoil overlay: 1 = panel + cone, 2 = also log every shot");
    // Replicated so clients' predicted magazines agree with the host.
    WC.infinite_ammo = cvar_reg_bool("sv_infinite_ammo", false, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "shots don't use ammo (host, replicated)");
    cmd_register("infinite_ammo", cmd_infinite_ammo, "infinite_ammo [0|1]: toggle infinite ammo (host)");
    // Aim model (docs/AIM.md).
    WC.block_barrel = cvar_reg_bool("block_fire_when_barrel_blocked", false, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "the trigger does nothing while the barrel is against a wall (obstruction within the weapon's obstruct_near; 0 = the bullet hits the wall point blank)");
    WC.muzzle_tolerance = cvar_reg_float("wpn_muzzle_tolerance", 0.3f, 0.02f, 2.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "host: how far (m) a client's claimed muzzle may be from the host's copy of its weapon pose before it is clamped (anti-cheat)");
    WC.aim_debug = cvar_reg_int("aim_debug", 0, 0, 2, CVAR_LOCKED, "aim model overlay: 1 = camera ray, aim point P, muzzle -> P bullet line, obstruction Q, class, crosshair target; 2 = also log every shot");
    cmd_register("aim_stats", cmd_aim_stats, "aim_stats [reset]: host checks of the shooters' muzzle claims and aim points (docs/AIM.md)");
    cmd_register("aim_report", cmd_aim_report, "aim_report: the local player's aim this frame (P, muzzle, Q, class) for tests");
}

const WeaponCvars *weapons_cvars(void) { return &WC; }

#define MAX_SHOT_HOOKS 8
static struct { ShotHook fn; void *user; } g_hooks[MAX_SHOT_HOOKS];
static int g_hook_count;

void weapons_add_shot_hook(ShotHook fn, void *user) {
    for (int i = 0; i < g_hook_count; i++)
        if (g_hooks[i].fn == fn && g_hooks[i].user == user) return;
    if (g_hook_count < MAX_SHOT_HOOKS) { g_hooks[g_hook_count].fn = fn; g_hooks[g_hook_count].user = user; g_hook_count++; }
}

static float cv_f(struct Cvar *c, float def) { return c ? c->f : def; }

const char *fire_mode_name(FireMode m) {
    return m == FM_AUTO ? "AUTO" : (m == FM_BURST ? "BURST" : "SINGLE");
}

// The built-in phone as an equipped slot (per player: callers may keep the pointer for a tick).
static InvSlot *phone_slot(const World *w, int slot) {
    static InvSlot ps[MAX_PLAYERS];
    InvSlot *s = &ps[DG_CLAMP(slot, 0, MAX_PLAYERS - 1)];
    memset(s, 0, sizeof(*s));
    s->item = w->items->builtin_phone;
    s->count = s->item ? 1 : 0;
    return s;
}

const InvSlot *weapon_equipped(const World *w, int slot) {
    const Player *p = &w->players[slot];
    if (p->sim.wpn.slot == WPN_SLOT_PHONE) return w->items->builtin_phone ? phone_slot(w, slot) : NULL;
    const InvSlot *s = inv_hot_c(&p->inv, DG_CLAMP((int)p->sim.wpn.slot, 0, INV_HOTBAR - 1));
    return s->item ? s : NULL;
}

// The equipped weapon with its attachments applied (per player slot storage: valid until the next
// call for the same slot).
const WeaponDef *weapon_equipped_def(const World *w, int slot) {
    static WeaponDef eff[MAX_PLAYERS];
    const InvSlot *s = weapon_equipped(w, slot);
    const WeaponDef *base = s ? item_weapon(w->items, s->item) : NULL;
    if (!base || slot < 0 || slot >= MAX_PLAYERS) return base;
    attach_weapon_effective(w->items, base, s->att, &eff[slot]);
    return &eff[slot];
}

// Stance / movement / ADS state the recoil and spread math depends on (all from PlayerSim, so
// host and predicting client agree).
static RecoilMods make_mods(const PlayerSim *sim, float scale) {
    RecoilMods m;
    memset(&m, 0, sizeof(m));
    m.ads_t = sim->wpn.ads_t;
    m.move01 = saturate((v3_len(v3_flat(sim->move.vel)) - 0.3f) / 5.0f);
    m.crouched = sim->move.crouched != 0;
    m.prone = false;   // no prone stance yet (data only)
    m.airborne = !sim->move.on_ground && (sim->move.mode == MOVE_WALK || sim->move.mode == MOVE_HANG);   // hanging one-handed: the air accuracy
    m.fire_mode = (FireMode)sim->wpn.fire_mode;
    m.extra = 1.0f;
    m.scale = scale;
    return m;
}

static WeaponVehicleHooks g_vh;
void weapons_set_vehicle_hooks(const WeaponVehicleHooks *h) { g_vh = *h; }
const WeaponVehicleHooks *weapons_vehicle_hooks(void) { return &g_vh; }

float weapon_current_spread(const World *w, int slot) {
    const WeaponDef *d = weapon_equipped_def(w, slot);
    const Player *p = &w->players[slot];
    if (!d || d->kind == WPN_MELEE) return 0;
    RecoilMods m = make_mods(&p->sim, cv_f(WC.spread_scale, 1));
    float sp = recoil_spread(&d->recoil, &p->sim.wpn.rc, &m) + chars_limb_spread(w, p->ent);
    if (p->sim.move.mode == MOVE_SEATED && g_vh.spread_mult) sp *= g_vh.spread_mult((World *)w, slot);
    return sp;
}

vec2 weapon_sway_deg(const WeaponDef *d, const WeaponState *ws, float t) {
    if (!d || d->kind == WPN_MELEE) return v2(0, 0);
    const RecoilParams *rp = &d->recoil;
    float amp = sway_global()->aim_sway * rp->sway_optic * saturate(ws->ads_t) * ws->sway_k * cv_f(WC.sway_scale, 1.0f);
    // Slow figure-eight, roughly breathing pace.
    float x = sinf(t * 0.83f) * 0.7f + sinf(t * 1.97f + 1.1f) * 0.3f;
    float y = sinf(t * 1.31f + 0.4f) * 0.6f + sinf(t * 2.63f + 2.0f) * 0.4f;
    return v2(x * amp, y * amp * 0.8f);
}

vec2 weapon_aim_offset_deg(const WeaponDef *d, const WeaponState *ws, const PlayerMove *m, float t) {
    if (!d || d->kind == WPN_MELEE) return v2(0, 0);
    // Off by default: the weapon sways about the reticle (hold point), the reticle stays centred.
    vec2 o = v2_scale(weapon_sway_deg(d, ws, t), cv_f(WC.ads_sway, 0.0f));
    if (m) {
        // Walk bob of the sight: a figure-eight at the step rate, growing with speed.
        float speed = sqrtf(m->vel.x * m->vel.x + m->vel.z * m->vel.z);
        float k = saturate(speed / 4.5f) * saturate(ws->ads_t) * cv_f(WC.ads_bob, 1.0f) * (m->on_ground ? 1.0f : 0.3f);
        float ph = t * (5.0f + fminf(speed, 6.0f) * 1.15f);
        o.x += cosf(ph) * 0.14f * k;
        o.y += (fabsf(sinf(ph)) - 0.5f) * 0.2f * k;
    }
    return o;
}

void weapon_flinch(World *w, int slot, float damage) {
    Player *p = &w->players[slot];
    const WeaponDef *d = weapon_equipped_def(w, slot);
    if (!d || d->kind == WPN_MELEE || damage <= 0) return;
    RecoilState *rc = &p->sim.wpn.rc;
    rc->bloom = fminf(fmaxf(rc->bloom, d->recoil.bloom_max), rc->bloom + damage * 0.04f);
}

static FireMode default_mode(u8 bits) {
    if (bits & FIRE_AUTO) return FM_AUTO;
    if (bits & FIRE_BURST) return FM_BURST;
    return FM_SINGLE;
}

static FireMode next_mode(FireMode cur, u8 bits) {
    // Cycle AUTO -> BURST -> SINGLE -> AUTO over the available modes.
    static const FireMode order[3] = { FM_AUTO, FM_BURST, FM_SINGLE };
    int at = 0;
    for (int i = 0; i < 3; i++)
        if (order[i] == cur) at = i;
    for (int k = 1; k <= 3; k++) {
        FireMode m = order[(at + k) % 3];
        if (bits & (1u << m)) return m;
    }
    return cur;
}

BodyId weapon_shooter_ignore_body(World *w, int shooter) {
    Player *sp = &w->players[shooter];
    BodyId self = sp->character ? phys_character_inner_body(sp->character) : PHYS_INVALID_BODY;
    if (sp->sim.move.mode == MOVE_SEATED && g_vh.ignore_body) self = g_vh.ignore_body(w, shooter);   // not the own vehicle
    return self;
}

bool weapon_trace_legacy_hitboxes(World *w, int shooter, vec3 origin, vec3 dir, float range, u32 view_tick256, HitResult *r) {
    Player *sp = &w->players[shooter];
    float best = r->hit ? r->dist : range;
    u32 view_tick = view_tick256 >> 8;
    bool found = false;
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_HITBOX) || (w->flags[i] & EF_DEAD)) continue;
        EntityId e = ENT_MAKE(i, w->gen[i]);
        if (e == sp->ent || char_get_index(w, i)) continue;   // characters use per-part hitboxes
        vec3 pos = w->xf[i].pos;
        if (w->authority && view_tick) world_lagcomp_get(w, e, view_tick, &pos);
        const Hitbox *hb = &w->hitbox[i];
        // Dummies are positioned at their capsule center; players at their feet.
        vec3 feet = w->type[i] == ET_DUMMY ? v3_sub(pos, v3(0, 0.9f, 0)) : pos;
        float height = hb->height;
        if (w->type[i] == ET_PLAYER && (w->flags[i] & EF_CROUCHED)) height = PLAYER_CROUCH_HEIGHT;
        vec3 head_c = v3_add(feet, v3(0, w->type[i] == ET_PLAYER ? height - 0.18f : hb->head_height, 0));
        // Head sphere first (as a tiny capsule).
        float th = ray_capsule(origin, dir, head_c, v3_add(head_c, v3(0, 0.01f, 0)), hb->head_radius);
        float tb = ray_capsule(origin, dir, v3_add(feet, v3(0, hb->radius, 0)), v3_add(feet, v3(0, height - hb->radius - 0.25f, 0)), hb->radius);
        float t = -1;
        bool head = false;
        if (th > 0 && (tb < 0 || th <= tb + 0.05f)) { t = th; head = true; }
        else if (tb > 0) t = tb;
        if (t > 0 && t < best) {
            best = t;
            found = true;
            r->hit = true;
            r->dist = t;
            r->point = v3_madd(origin, dir, t);
            r->normal = v3_neg(dir);
            r->ent = e;
            r->hitbox = true;
            r->character = false;
            r->headshot = head;
            r->part = -1;
            r->tire = -1;
            r->body = PHYS_INVALID_BODY;
            r->material = SURF_FLESH;
        }
    }
    return found;
}

// Lag-compensated hitscan: world geometry via Jolt, hitbox capsules rewound to view_tick.
static HitResult trace_shot(World *w, int shooter, vec3 origin, vec3 dir, float range, u32 view_tick256) {
    HitResult r;
    memset(&r, 0, sizeof(r));
    r.tire = -1;
    Player *sp = &w->players[shooter];
    BodyId self = weapon_shooter_ignore_body(w, shooter);
    RayHit rh;
    if (phys_raycast(w->phys, origin, v3_scale(dir, range), PL_MASK_WORLD, self, &rh)) {
        r.hit = true;
        r.dist = rh.distance;
        r.point = rh.point;
        r.normal = rh.normal;
        r.ent = (EntityId)rh.user;
        r.body = rh.body;
        r.material = rh.material;
    }
    float best = r.hit ? r.dist : range;
    u32 view_tick = view_tick256 >> 8;
    r.part = -1;
    // Tires (not physics shapes: the wheel volume is tested analytically).
    if (g_vh.ray_tires) {
        EntityId veh = ENT_NONE;
        float tt = 0;
        int wheel = g_vh.ray_tires(w, origin, dir, best, w->authority ? view_tick : 0, &veh, &tt);
        if (wheel >= 0 && tt < best) {
            best = tt;
            r.hit = true;
            r.dist = tt;
            r.point = v3_madd(origin, dir, tt);
            r.normal = v3_neg(dir);
            r.ent = veh;
            r.body = PHYS_INVALID_BODY;
            r.material = SURF_CLOTH;
            r.tire = wheel;
        }
    }
    if (weapon_trace_legacy_hitboxes(w, shooter, origin, dir, range, view_tick256, &r)) best = r.dist;
    // Per-bone hitboxes on characters: the host rewinds them to the shooter's view tick.
    EntityId ce;
    HitboxRayHit hh;
    if (chars_raycast(w, sp->ent, origin, dir, best, w->authority ? view_tick256 : 0, &ce, &hh)) {
        r.hit = true;
        r.dist = hh.t;
        r.point = hh.point;
        r.normal = v3_neg(dir);
        r.ent = ce;
        r.hitbox = true;
        r.character = true;
        r.part = hh.part;
        r.headshot = hh.part == BP_HEAD;
        r.body = PHYS_INVALID_BODY;
        r.material = SURF_FLESH;
    }
    return r;
}

EntityId weapon_trace_target(World *w, int shooter, vec3 origin, vec3 dir, float range) {
    HitResult h = trace_shot(w, shooter, origin, dir, range, 0);
    return (h.hit && h.hitbox) ? h.ent : ENT_NONE;
}

static void apply_hit(World *w, int shooter, const WeaponDef *d, const HitResult *h, vec3 dir, float damage_scale) {
    Player *sp = &w->players[shooter];
    if (h->character) {
        // final = base x part_mult[character][part] x weapon_part_mult[weapon][part] x range falloff
        CharacterInst *c = char_get(w, h->ent);
        float fall = damage_falloff(h->dist, d->falloff_start, d->falloff_end, d->falloff_min);
        float base = d->damage * damage_scale;
        float dmg = damage_resolve(base, c ? c->def->part_mult : NULL, d->part_mult, h->part, fall);
        float mult = base > 0 ? dmg / base : 0;
        apply_damage_part(w, h->ent, dmg, d->damage_type, sp->ent, h->point, dir, h->part, mult);
        if (w->type[ENT_INDEX(h->ent)] == ET_DUMMY)
            log_info("dummy hit: %s x%.2f (char %.2f, weapon %.2f, falloff %.2f) = %.1f damage", body_part_name(h->part), mult,
                     c ? c->def->part_mult[h->part] : 1.0f, d->part_mult[h->part], fall, dmg);
        return;
    }
    if (h->hitbox) {
        float dmg = d->damage * damage_scale * (h->headshot ? d->headshot_mult : 1.0f);
        apply_damage(w, h->ent, dmg, d->damage_type, sp->ent, h->point, dir, h->headshot);
        return;
    }
    if (h->tire >= 0) {
        if (g_vh.tire_hit) g_vh.tire_hit(w, h->ent, h->tire, h->point, dir);
        return;
    }
    if (h->body != PHYS_INVALID_BODY) phys_add_impulse(w->phys, h->body, v3_scale(dir, d->impulse * damage_scale), h->point);
    if (ent_valid(w, h->ent) && (w->comps[ENT_INDEX(h->ent)] & C_HEALTH))
        apply_damage(w, h->ent, d->damage * damage_scale, d->damage_type, sp->ent, h->point, dir, false);
}

static u32 impact_flags(const HitResult *h) {
    u32 f = 0;
    if (h->headshot) f |= FX_IMPACT_HEAD;
    if (h->hitbox) f |= FX_IMPACT_FLESH;
    else if (!h->ent) f |= FX_IMPACT_DECAL;   // static level geometry takes decals; props / doors don't
    return f;
}

// Dealt damage for the flesh effects (the same resolution as apply_hit, without side effects).
static float shot_damage(World *w, const WeaponDef *d, const HitResult *h) {
    CharacterInst *c = h->character ? char_get(w, h->ent) : NULL;
    float fall = damage_falloff(h->dist, d->falloff_start, d->falloff_end, d->falloff_min);
    return c ? damage_resolve(d->damage, c->def->part_mult, d->part_mult, h->part, fall) : d->damage;
}

static void flesh_predict(World *w, const HitResult *h, vec3 dir, const WeaponDef *d) {
    FleshHit fh;
    memset(&fh, 0, sizeof(fh));
    fh.target = h->ent;
    fh.part = h->part;
    fh.pos = h->point;
    fh.normal = h->normal;
    fh.dir = dir;
    fh.caliber = d->vfx_caliber;
    fh.dist = h->dist;
    fh.damage = shot_damage(w, d, h);
    fh.flags = FLESH_ENTRY_ONLY | (h->headshot ? FLESH_HEAD : 0u);
    flesh_fx_hit(w, &fh);
}

static void push_shot_events(World *w, int shooter, vec3 from, const HitResult *h, vec3 end, const WeaponDef *d, bool flash, bool killed) {
    int caliber = d->vfx_caliber;
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_TRACER;
    ev.target = EVENT_ALL;
    ev.except = (u8)shooter;
    ev.pos = from;
    ev.dir = end;
    // First pellet of a shot carries the muzzle flash (exactly one flash per shot for observers).
    ev.ent = w->players[shooter].ent;
    ev.id = flash ? (1u | ((u32)d->vfx_muzzle_type << 4) | ((u32)d->vfx_muzzle_device << 8) | ((u32)caliber << 12)) : 0u;
    ev_push(w, &ev);
    if (h->hit) {
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_IMPACT;
        ev.target = EVENT_ALL;
        ev.except = (u8)shooter;
        ev.pos = h->point;
        ev.dir = h->normal;
        // Shooter receives it too: it skips it when it predicted the impact (cl_predict_impacts).
        ev.except = EVENT_ALL;
        ev.ent2 = (u32)shooter + 1u;
        ev.id = fx_impact_pack(h->material, caliber, impact_flags(h));
        vec3 inc = v3_norm(v3_sub(end, from));
        memcpy(ev.data, &inc, sizeof(inc));
        if (h->character) {
            // Flesh effects on every machine: target, part, damage, kill (src/gameplay/flesh_fx.c).
            ev.ent = h->ent;
            ev.value = shot_damage(w, d, h);
            flesh_pack_event(ev.data, inc, h->part, killed, false, h->dist);
        }
        ev_push(w, &ev);
    }
}

void weapon_apply_hit(World *w, int shooter, const WeaponDef *d, const HitResult *h, vec3 dir, float damage_scale) {
    apply_hit(w, shooter, d, h, dir, damage_scale);
}

void weapon_push_impact(World *w, int shooter, const WeaponDef *d, const HitResult *h, vec3 dir, bool killed, u32 extra) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_IMPACT;
    ev.target = EVENT_ALL;
    ev.except = EVENT_ALL;
    ev.pos = h->point;
    ev.dir = h->normal;
    ev.ent2 = (u32)shooter + 1u;   // the shooter skips it when it predicted the impact
    ev.id = fx_impact_pack(h->material, d->vfx_caliber, impact_flags(h)) | (extra & 0xFFFF0000u);
    memcpy(ev.data, &dir, sizeof(dir));
    if (h->character) {
        ev.ent = h->ent;
        ev.value = shot_damage(w, d, h);
        flesh_pack_event(ev.data, dir, h->part, killed, false, h->dist);
    }
    ev_push(w, &ev);
}

void weapon_local_impact(World *w, const WeaponDef *d, const HitResult *h, vec3 dir, u32 extra) {
    if (!fx_predict_impacts()) return;
    if (h->character) flesh_predict(w, h, dir, d);
    else fx_impact_ex(h->point, h->normal, dir, h->material, d->vfx_caliber, impact_flags(h) | (extra & 0xFFFF0000u));
}

// ------------------------------------------------------------------ aim model (docs/AIM.md)

#define AIM_REACH 1.5f          // a muzzle is never farther than this from the eye (hard cap on claims)
#define AIM_EYE_REACH 0.6f      // the first-person camera is never farther than this from the simulated eye
#define AIM_EYE_TOLERANCE 0.12f // host: a camera claim within this of its own pose's camera is used as is
#define AIM_HOST_MAX_AGE 4      // host weapon poses older than this (ticks, LOD / culled) are not used to check claims
#define AIM_RECOVER_MARGIN 0.02f

// Without a claim or a pose: a barrel in front of the right side of the eye (camera space).
static vec3 fallback_muzzle_cam(bool hand) { return hand ? v3(0.12f, -0.12f, -0.45f) : v3(0.14f, -0.1f, -0.55f); }

AimQuery weapon_aim_query(World *w, int slot, const PlayerMove *m, const UserCmd *cmd, vec3 cam_dir, float max_range, float obstruct_near, u32 view_tick256) {
    (void)w;
    (void)slot;
    AimQuery q;
    memset(&q, 0, sizeof(q));
    q.eye = player_eye_pos(m);
    q.yaw = m->yaw;
    q.pitch = m->pitch;
    q.cam_dir = cam_dir;
    q.have_claim = cmd && aim_claim_valid(cmd->muzzle_cam);
    if (q.have_claim) q.claim = aim_claim_unpack(cmd->muzzle_cam);
    q.have_eye_claim = cmd && aim_claim_valid(cmd->eye_cam);
    if (q.have_eye_claim) q.eye_claim = aim_claim_unpack(cmd->eye_cam);
    q.max_range = fmaxf(1.0f, max_range);
    q.obstruct_near = obstruct_near;
    q.view_tick256 = view_tick256;
    return q;
}

static bool is_friendly(World *w, int shooter, EntityId e) {
    return ent_valid(w, e) && w->type[ENT_INDEX(e)] == ET_PLAYER && e != w->players[shooter].ent;
}

void weapon_aim_solve(World *w, int slot, const AimQuery *q, AimSolve *out) {
    memset(out, 0, sizeof(*out));
    out->cam_dir = q->cam_dir;
    BodyId self = weapon_shooter_ignore_body(w, slot);
    RayHit rh;
    // Camera ray origin: the first-person camera the player looks through (the head's camera socket),
    // not the capsule eye, which sits 10 cm (standing) to 30 cm (crouched) off it: the shooter's claim,
    // checked like the muzzle against this machine's copy of the pose; without either, the capsule eye.
    {
        vec3 host_eye = v3(0, 0, 0);
        int eage = 0;
        bool have_host = chars_eye_cam(w, w->players[slot].ent, &host_eye, &eage) && eage <= AIM_HOST_MAX_AGE;
        bool check = have_host && q->have_eye_claim && w->authority;
        AimValidate ev = aim_validate_muzzle(q->eye_claim, q->have_eye_claim, host_eye, check, AIM_EYE_TOLERANCE, AIM_EYE_REACH);
        vec3 ecam = q->have_eye_claim ? ev.cam : (have_host ? host_eye : v3(0, 0, 0));
        out->eye_error = check ? ev.error : 0.0f;
        out->eye_clamped = check && ev.clamped;
        out->eye = aim_cam_to_world(q->eye, q->yaw, q->pitch, ecam);
        // Never through a wall: the render camera is kept out of geometry the same way (camera_fp_collide).
        vec3 to_e = v3_sub(out->eye, q->eye);
        float le = v3_len(to_e);
        if (le > 1e-4f && phys_raycast(w->phys, q->eye, to_e, PL_MASK_WORLD, self, &rh)) out->eye = aim_recover_origin(q->eye, out->eye, rh.distance / le, AIM_RECOVER_MARGIN);
    }
    // Muzzle: the shooter's claim, checked against this machine's copy of its weapon pose. The host's
    // copy is authoritative; a client predicting itself has the very pose it claimed.
    vec3 host_cam = v3(0, 0, 0);
    int age = 0;
    bool have_host = !q->hand && chars_muzzle_cam(w, w->players[slot].ent, &host_cam, &age) && age <= AIM_HOST_MAX_AGE;
    bool check = have_host && q->have_claim && w->authority;
    AimValidate v = aim_validate_muzzle(q->claim, q->have_claim, host_cam, check, WC.muzzle_tolerance ? WC.muzzle_tolerance->f : 0.3f, AIM_REACH);
    vec3 cam = (q->have_claim || have_host) ? v.cam : fallback_muzzle_cam(q->hand);
    out->claim_error = check ? v.error : 0.0f;
    out->claim_clamped = check && v.clamped;
    out->muzzle = aim_cam_to_world(q->eye, q->yaw, q->pitch, cam);
    // Muzzle inside geometry (mantle, vehicle entry, barrel poked through a thin wall): back along the
    // eye -> muzzle line to the last free point, so a bullet never starts inside or behind a surface.
    out->origin = out->muzzle;
    vec3 to_m = v3_sub(out->muzzle, q->eye);
    float lm = v3_len(to_m);
    if (lm > 1e-4f && phys_raycast(w->phys, q->eye, to_m, PL_MASK_WORLD, self, &rh)) {
        out->origin = aim_recover_origin(q->eye, out->muzzle, rh.distance / lm, AIM_RECOVER_MARGIN);
        out->recovered = true;
    }
    // Aim point: the camera ray (what the reticle means).
    HitResult hp = trace_shot(w, slot, out->eye, q->cam_dir, q->max_range, q->view_tick256);
    out->p_hit = hp.hit;
    out->p = hp.hit ? hp.point : v3_madd(out->eye, q->cam_dir, q->max_range);
    // Convergence: from the barrel to P.
    out->dir = aim_converge(out->origin, out->p, q->cam_dir);
    // Obstruction: the bullet's own line, short of P (P is a surface hit itself).
    float len = v3_dist(out->origin, out->p);
    out->q = out->p;
    out->q_dist = len;
    out->q_ent = hp.ent;
    if (out->recovered) {
        // The muzzle is in the wall: the shot lands on it at point blank.
        out->q = out->origin;
        out->q_dist = 0.0f;
        out->q_ent = (EntityId)rh.user;
        out->cls = AIM_BARREL;
        return;
    }
    float probe = len - 0.03f;
    if (probe > 0.0f) {
        HitResult ho = trace_shot(w, slot, out->origin, out->dir, probe, q->view_tick256);
        if (ho.hit && ho.dist < probe) {
            out->q = ho.point;
            out->q_dist = ho.dist;
            out->q_ent = ho.ent;
            out->cls = (int)aim_classify(ho.dist, len, q->obstruct_near, is_friendly(w, slot, ho.ent));
        }
    }
}

bool weapon_aim_preview(World *w, int slot, vec3 eye, float yaw, float pitch, const UserCmd *cmd, AimSolve *out) {
    if (!w || slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) return false;
    const WeaponDef *d = weapon_equipped_def(w, slot);
    if (!d || d->kind == WPN_MELEE) return false;
    Player *p = &w->players[slot];
    Game *g = game();
    vec2 sway = weapon_aim_offset_deg(d, &p->sim.wpn, &p->sim.move, weapon_sway_time(cmd ? cmd->seq : 0) + (g ? g->alpha * SIM_DT : 0.0f));
    PlayerMove m = p->sim.move;
    m.yaw = yaw;
    m.pitch = pitch;
    vec3 dir = dir_from_angles(yaw - sway.x * DG_DEG2RAD, pitch + sway.y * DG_DEG2RAD);
    AimQuery q = weapon_aim_query(w, slot, &m, cmd, dir, d->aim_max_range, d->obstruct_near, 0);
    q.eye = eye;
    weapon_aim_solve(w, slot, &q, out);
    return true;
}

static u16 g_aim_claim;
u16 weapons_take_aim_claim(void) {
    u16 v = g_aim_claim;
    g_aim_claim = 0;
    return v;
}

// aim_debug: the whole model of one shot / frame.
static void aim_debug_draw(const AimSolve *a, float life) {
    dbg_line(a->eye, a->p, DBG_WHITE, life, true);                      // camera ray
    dbg_cross(a->p, 0.05f, a->p_hit ? DBG_GREEN : DBG_BLUE, life);         // P
    dbg_line(a->origin, a->q, DBG_YELLOW, life, true);                   // the bullet's line (to where it lands)
    if (a->cls != AIM_CLEAR) dbg_line(a->q, a->p, DBG_RED, life, true);  // the part it never reaches
    dbg_cross(a->muzzle, 0.02f, DBG_ORANGE, life);
    if (a->recovered) dbg_cross(a->origin, 0.03f, DBG_MAGENTA, life);
    if (a->cls != AIM_CLEAR) {
        dbg_cross(a->q, 0.04f, a->cls == AIM_FRIENDLY ? DBG_BLUE : DBG_RED, life);
        dbg_text(a->q, aim_class_name(a->cls), DBG_YELLOW, 0.03f, life);
    }
}

void weapons_aim_debug_frame(World *w) {
    if (!WC.aim_debug || !WC.aim_debug->i || !w) return;
    Game *g = game();
    int slot = game_view_slot();
    if (!g || slot < 0) return;
    AimSolve a;
    if (!weapon_aim_preview(w, slot, player_eye_pos(&w->players[slot].sim.move), g->view.yaw, g->view.pitch, game_view_cmd(), &a)) return;
    aim_debug_draw(&a, 0.0f);
}

static LocalFlashFn g_local_flash;
void weapons_set_local_flash(LocalFlashFn fn) { g_local_flash = fn; }

static WeaponDebugStats g_dbg_stats;
const WeaponDebugStats *weapons_debug_stats(void) { return &g_dbg_stats; }
void weapons_debug_reset(void) { memset(&g_dbg_stats, 0, sizeof(g_dbg_stats)); }

// Verifies a bullet left inside the advertised cone (what the crosshair shows).
static float angle_deg(vec3 a, vec3 b) { return atan2f(v3_len(v3_cross(a, b)), v3_dot(a, b)) * DG_RAD2DEG; }

static void debug_check_shot(int slot, vec3 aim, vec3 sdir, float cone, const RecoilState *rc, bool log_it) {
    float off = angle_deg(aim, sdir);
    float half = cone * 0.5f;
    bool inside = off <= half + 1e-3f;
    g_dbg_stats.shots++;
    if (!inside) g_dbg_stats.outside++;
    if (half > 1e-4f) g_dbg_stats.max_ratio = fmaxf(g_dbg_stats.max_ratio, off / half);
    if (log_it)
        log_info("shot p%d #%u idx %u cone %.3f off %.3f (%.0f%% of half) acc %.2f/%.2f bloom %.2f %s", slot, g_dbg_stats.shots,
                 rc->shot_index, cone, off, half > 0 ? off / half * 100.0f : 0.0f, rc->acc_pitch, rc->acc_yaw, rc->bloom,
                 inside ? "inside" : "OUTSIDE");
}

static u32 dbg_check_color(vec3 aim, vec3 sdir, float cone) {
    float off = angle_deg(aim, sdir);
    return off <= cone * 0.5f + 1e-3f ? DBG_GREEN : DBG_RED;
}

// Host: the shooter's claims against this machine's reconstruction (aim_stats; discrepancies logged).
static void aim_host_check(World *w, int slot, const UserCmd *cmd, const AimSolve *a, bool local_view) {
    AimStats *st = &g_aim_stats;
    st->shots++;
    if (a->recovered) st->recovered++;
    if (local_view) return;   // the host's own player: its claim is its own pose
    if (a->claim_error > 0.0f) {
        st->checked++;
        st->max_error = fmaxf(st->max_error, a->claim_error);
        if (a->claim_clamped) {
            st->clamped++;
            log_warn("aim: p%d muzzle claim %.0f mm from the host's weapon pose (tolerance %.0f mm): clamped", slot, a->claim_error * 1000.0f,
                     WC.muzzle_tolerance->f * 1000.0f);
        }
    }
    if (a->eye_error > 0.0f) {
        st->eye_checked++;
        st->max_eye_error = fmaxf(st->max_eye_error, a->eye_error);
        if (a->eye_clamped) {
            st->eye_clamped++;
            log_warn("aim: p%d camera claim %.0f mm from the host's pose (tolerance %.0f mm): clamped", slot, a->eye_error * 1000.0f, AIM_EYE_TOLERANCE * 1000.0f);
        }
    }
    if (cmd->aim_dist) {
        // The client's predicted aim point vs the host's (lag-compensated) one.
        float cl = (float)cmd->aim_dist * 0.01f, hd = v3_dist(a->eye, a->p);
        float err = fabsf(cl - hd);
        st->p_checked++;
        st->max_p_error = fmaxf(st->max_p_error, err);
        if (err > fmaxf(0.5f, hd * 0.05f)) {
            st->p_mismatch++;
            log_debug("aim: p%d aim point %.2f m on the client, %.2f m on the host", slot, cl, hd);
        }
    }
}

static void fire(World *w, int slot, PlayerSim *sim, const WeaponDef *d, const UserCmd *cmd, bool first, bool local_view,
                 vec3 aim, float spread, u32 seed, int shot_in_mag, AimSolve *as) {
    PlayerMove *m = &sim->move;
    vec3 eye = player_eye_pos(m);
    Rng rng = rng_make(seed);
    if (!w->authority && !(local_view && first)) {
        // Prediction replays / spectated players: nothing is spawned or drawn here (EV_SHOT does it).
        memset(as, 0, sizeof(*as));
        as->eye = as->muzzle = as->origin = as->q = as->p = eye;
        as->dir = as->cam_dir = aim;
        return;
    }
    // Aim model (docs/AIM.md): the bullet leaves the muzzle and converges on the camera ray's aim point.
    AimQuery aq = weapon_aim_query(w, slot, m, cmd, aim, d->aim_max_range, d->obstruct_near, w->authority ? cmd->view_tick : 0);
    weapon_aim_solve(w, slot, &aq, as);
    vec3 muzzle = as->muzzle, origin = as->origin, dir = as->dir;
    if (w->authority) aim_host_check(w, slot, cmd, as, local_view);
    if (local_view && first) {
        if (!w->authority && !g_aim_claim) g_aim_claim = (u16)DG_CLAMP((int)lrintf(v3_dist(as->eye, as->p) * 100.0f), 1, 65535);
        if (WC.aim_debug && WC.aim_debug->i) {
            aim_debug_draw(as, 4.0f);
            if (WC.aim_debug->i >= 2)
                log_info("aim: shot class %s P %.2f m Q %.2f m%s claim err %.1f mm", aim_class_name(as->cls), v3_dist(as->eye, as->p), as->q_dist,
                         as->recovered ? " (muzzle inside geometry)" : "", as->claim_error * 1000.0f);
        }
    }
    int dbg = WC.recoil_debug ? WC.recoil_debug->i : 0;
    bool ballistic = bal_world_enabled() && bal_weapon_cartridge(d) >= 0;
    if (local_view && first) {
        // The flash on the drawn gun (the first-person item may use its own FOV); the bullet from the muzzle.
        vec3 flash = muzzle, mp, mf;
        if (hands_muzzle(&mp, &mf) && v3_dist(mp, muzzle) < 0.5f) flash = v3_madd(mp, mf, 0.015f);
        if (ballistic) {
            bal_fx_muzzle_weapon(slot, d, flash, dir, true);   // composed flash + the shooter's report (docs/BALLISTICS.md)
            bal_shooter_report(d, eye);
        } else {
            // The flash is parented to the gun's final transform of this tick (after the fire animation's
            // kick, docs/FIRE_ANIM.md): the game spawns it once the first-person item is placed. The
            // sound's transient is this tick, the same tick as the gun's peak displacement.
            if (g_local_flash) g_local_flash(dir, d->vfx_muzzle_type, d->vfx_muzzle_device);
            else fx_muzzle_ex(flash, dir, d->vfx_muzzle_type, d->vfx_muzzle_device, true);
            fx_sound(d->sound_fire, eye, 1.0f);
        }
    }
    if (d->kind == WPN_PROJECTILE) {
        if (w->authority) {
            EntityId e = ent_spawn(w, ET_PROJECTILE);
            int i = (int)ENT_INDEX(e);
            w->vis[i].shape = VIS_BOX;
            w->vis[i].half = v3(0.012f, 0.012f, 0.22f);
            w->vis[i].color = rgba8(0.5f, 0.4f, 0.3f, 1);
            w->comps[i] |= C_VISUAL | C_PROJECTILE;
            vec3 pdir = recoil_cone_dir(dir, spread, &rng);
            w->proj[i].vel = v3_scale(pdir, d->projectile_speed);
            w->proj[i].owner = w->players[slot].ent;
            w->proj[i].weapon = (int)(d - w->items->weapons);
            w->proj[i].life = 8.0f;
            float yaw = atan2f(-pdir.x, -pdir.z), pitch = asinf(clampf(pdir.y, -1, 1));
            ent_set_pos(w, e, origin, quat_euler(yaw, pitch, 0), true);
            ev_sound(w, d->sound_fire, muzzle, 1.0f, slot);
        }
        return;
    }
    // Guns with a cartridge fire simulated bullets (gameplay/ballistics): the host's are
    // authoritative, the shooter predicts its own, everyone else replays the EV_SHOT record.
    if (ballistic) {
        const InvSlot *eq = weapon_equipped(w, slot);
        const WeaponDef *base = eq ? item_weapon(w->items, eq->item) : NULL;
        if (base) {
            BalFireIn bi;
            memset(&bi, 0, sizeof(bi));
            bi.slot = slot;
            bi.def = d;
            bi.weapon_index = (int)(base - w->items->weapons);
            bi.att = eq->att;
            bi.eye = as->eye;
            bi.origin = origin;
            bi.aim = dir;
            bi.spread = spread;
            bi.seed = seed;
            bi.view_tick256 = cmd->view_tick;
            bi.shot_in_mag = shot_in_mag;
            bi.mag_size = d->mag_size;
            bi.mag_seed = sim->wpn.rc.mag_seed;
            bi.shot_counter = (u16)sim->wpn.shots;
            bi.local_view = local_view;
            bi.first = first;
            if (first && local_view && dbg) debug_check_shot(slot, dir, recoil_cone_dir(dir, spread, &rng), spread, &sim->wpn.rc, dbg >= 2);
            bal_fire(w, &bi);
            return;   // the report reaches the others with EV_SHOT
        }
    }
    int pellets = DG_MAX(1, d->pellets);
    for (int p = 0; p < pellets; p++) {
        vec3 sdir = recoil_cone_dir(dir, spread, &rng);
        if (first && local_view) {
            debug_check_shot(slot, dir, sdir, spread, &sim->wpn.rc, dbg >= 2);
            if (dbg) {
                HitResult dh = trace_shot(w, slot, origin, sdir, d->range, 0);
                if (dh.hit) dbg_cross(dh.point, 0.03f, dbg_check_color(dir, sdir, spread), 10.0f);
            }
        }
        if (w->authority) {
            HitResult h = trace_shot(w, slot, origin, sdir, d->range, cmd->view_tick);
            vec3 end = h.hit ? h.point : v3_madd(origin, sdir, d->range);
            bool was_alive = h.hit && h.character && ent_valid(w, h.ent) && !(w->flags[ENT_INDEX(h.ent)] & EF_DEAD);
            if (h.hit) apply_hit(w, slot, d, &h, sdir, 1.0f);
            bool killed = was_alive && (w->flags[ENT_INDEX(h.ent)] & EF_DEAD);
            push_shot_events(w, slot, origin, &h, end, d, p == 0, killed);
            if (local_view && first) {
                fx_tracer(origin, end);
                if (h.hit && fx_predict_impacts()) {
                    if (h.character) flesh_predict(w, &h, sdir, d);
                    else fx_impact_ex(h.point, h.normal, sdir, h.material, d->vfx_caliber, impact_flags(&h));
                }
            }
        } else if (local_view && first) {
            // Predicted cosmetic trace against the replica world.
            HitResult h = trace_shot(w, slot, origin, sdir, d->range, 0);
            vec3 end = h.hit ? h.point : v3_madd(origin, sdir, d->range);
            fx_tracer(origin, end);
            if (h.hit && fx_predict_impacts()) {
                if (h.character) flesh_predict(w, &h, sdir, d);
                else fx_impact_ex(h.point, h.normal, sdir, h.material, d->vfx_caliber, impact_flags(&h));
            }
            // Unconfirmed reaction on what we hit (the host's hit event confirms / corrects it).
            if (h.character) chars_predict_hit(w, h.ent, h.part, sdir);
        }
    }
    if (w->authority) ev_sound(w, d->sound_fire, muzzle, 1.0f, slot);
}

static void melee_step(World *w, int slot, PlayerSim *sim, const WeaponDef *d, bool first, bool local_view) {
    WeaponState *ws = &sim->wpn;
    if (ws->swing_hit_done) return;
    float u = (ws->swing_t - d->active_start) / fmaxf(1e-3f, d->active_end - d->active_start);
    if (u < 0 || u > 1) return;
    PlayerMove *m = &sim->move;
    vec3 eye = player_eye_pos(m);
    // Sweep right-to-left across the view, slightly downward.
    float arc = d->swing_arc * DG_DEG2RAD;
    float yaw = m->yaw - arc * 0.5f + arc * u;
    float pitch = m->pitch + lerpf(0.15f, -0.2f, u);
    vec3 dir = dir_from_angles(yaw, pitch);
    vec3 tip = v3_madd(eye, dir, d->reach);
    if (first && g_engine.ptuning->debug_melee->i) dbg_line(eye, tip, DBG_ORANGE, 0.5f, true);

    Player *p = &w->players[slot];
    BodyId self = p->character ? phys_character_inner_body(p->character) : PHYS_INVALID_BODY;
    // Hitboxes: segment eye->tip against capsules (characters: per-part hitboxes).
    float best = d->reach + 1;
    EntityId hit_ent = ENT_NONE;
    vec3 hit_point = tip;
    bool head = false;
    int hit_part = -1;
    {
        EntityId ce;
        HitboxRayHit hh;
        if (chars_sweep(w, p->ent, eye, tip, d->sweep_radius, &ce, &hh) && hh.t <= d->reach) {
            best = hh.t;
            hit_ent = ce;
            hit_point = hh.point;
            hit_part = hh.part;
            head = hh.part == BP_HEAD;
        }
    }
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_HITBOX) || (w->flags[i] & EF_DEAD)) continue;
        EntityId e = ENT_MAKE(i, w->gen[i]);
        if (e == p->ent || char_get_index(w, i)) continue;
        vec3 feet = w->type[i] == ET_DUMMY ? v3_sub(w->xf[i].pos, v3(0, 0.9f, 0)) : w->xf[i].pos;
        float r = w->hitbox[i].radius + d->sweep_radius;
        vec3 a = v3_add(feet, v3(0, 0.3f, 0)), b = v3_add(feet, v3(0, w->hitbox[i].height - 0.2f, 0));
        float t = ray_capsule(eye, dir, a, b, r);
        if (t > 0 && t < best && t <= d->reach) {
            best = t;
            hit_ent = e;
            hit_point = v3_madd(eye, dir, t);
            head = hit_point.y > feet.y + w->hitbox[i].head_height - 0.12f;
            hit_part = -1;
        }
    }
    ShapeHit sh;
    bool world_hit = phys_sphere_cast(w->phys, eye, v3_scale(dir, d->reach), d->sweep_radius, PL_MASK_WORLD, self, &sh);
    if (world_hit && sh.distance < best) {
        ws->swing_hit_done = 1;
        int mat = phys_body_material(w->phys, sh.body);
        u32 mflags = sh.user ? 0u : FX_IMPACT_DECAL;
        if (local_view && first) { fx_impact_ex(sh.point, sh.normal, dir, mat, VFX_CAL_MELEE, mflags); fx_sound("melee_hit_wall", sh.point, 0.9f); }
        if (w->authority) {
            phys_add_impulse(w->phys, sh.body, v3_scale(dir, d->knockback * 0.25f), sh.point);
            GameEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EV_IMPACT;
            ev.target = EVENT_ALL;
            ev.except = (u8)slot;
            ev.pos = sh.point;
            ev.dir = sh.normal;
            ev.id = fx_impact_pack(mat, VFX_CAL_MELEE, mflags);
            memcpy(ev.data, &dir, sizeof(dir));
            ev_push(w, &ev);
            if (ent_valid(w, (EntityId)sh.user) && (w->comps[ENT_INDEX(sh.user)] & C_HEALTH))
                apply_damage(w, (EntityId)sh.user, d->damage, d->damage_type, p->ent, sh.point, dir, false);
        }
        return;
    }
    if (hit_ent) {
        ws->swing_hit_done = 1;
        // Slash direction: along the swing (right to left), a little into the target.
        vec3 swing = v3_norm(v3_add(v3_scale(v3_norm(v3_sub(dir_from_angles(yaw + 0.1f, pitch - 0.035f), dir)), 0.85f), v3_scale(dir, 0.35f)));
        bool flesh = hit_part >= 0 && char_get(w, hit_ent);
        if (local_view && first) {
            if (flesh && fx_predict_impacts()) {
                FleshHit fh;
                memset(&fh, 0, sizeof(fh));
                fh.target = hit_ent;
                fh.part = hit_part;
                fh.pos = hit_point;
                fh.normal = v3_neg(dir);
                fh.dir = swing;
                fh.caliber = VFX_CAL_MELEE;
                fh.damage = d->damage;
                fh.dist = v3_dist(eye, hit_point);
                fh.flags = FLESH_ENTRY_ONLY | FLESH_MELEE | (head ? FLESH_HEAD : 0u);
                flesh_fx_hit(w, &fh);
            } else if (!flesh) {
                fx_impact_ex(hit_point, v3_neg(dir), dir, SURF_FLESH, VFX_CAL_MELEE, FX_IMPACT_FLESH | (head ? FX_IMPACT_HEAD : 0u));
            }
            fx_sound("melee_hit", hit_point, 1.0f);
        }
        if (w->authority) {
            CharacterInst *c = hit_part >= 0 ? char_get(w, hit_ent) : NULL;
            if (c) {
                float dmg = damage_resolve(d->damage, c->def->part_mult, d->part_mult, hit_part, 1.0f);
                bool was_alive = !(w->flags[ENT_INDEX(hit_ent)] & EF_DEAD);
                apply_damage_part(w, hit_ent, dmg, d->damage_type, p->ent, hit_point, dir, hit_part, d->damage > 0 ? dmg / d->damage : 0);
                GameEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = EV_IMPACT;
                ev.target = EVENT_ALL;
                ev.except = EVENT_ALL;
                ev.ent2 = (u32)slot + 1u;   // the swinger skips the entry it predicted
                ev.pos = hit_point;
                ev.dir = v3_neg(dir);
                ev.ent = hit_ent;
                ev.value = dmg;
                ev.id = fx_impact_pack(SURF_FLESH, VFX_CAL_MELEE, FX_IMPACT_FLESH | (head ? FX_IMPACT_HEAD : 0u));
                flesh_pack_event(ev.data, swing, hit_part, was_alive && (w->flags[ENT_INDEX(hit_ent)] & EF_DEAD), true, v3_dist(eye, hit_point));
                ev_push(w, &ev);
            } else {
                apply_damage(w, hit_ent, d->damage * (head ? d->headshot_mult : 1.0f), d->damage_type, p->ent, hit_point, dir, head);
            }
        } else if (local_view && first && hit_part >= 0) {
            chars_predict_hit(w, hit_ent, hit_part, dir);
        }
    }
}

static u32 shot_seed(const UserCmd *cmd, int slot, u32 shots) {
    return dg_hash_u32(cmd->seq * 0x9E3779B1u ^ dg_hash_u32((u32)(slot + 1) * 0x85EBCA77u ^ shots));
}

static void reset_recoil(WeaponState *ws, const WeaponDef *d, const UserCmd *cmd, int slot) {
    u32 seed = (d && !d->recoil.seed_per_mag) ? (dg_hash_str(d->id) | 1u) : recoil_mag_seed(cmd->seq, (u32)slot);
    recoil_reset(&ws->rc, seed);
}

// ADS blend, sprint-to-fire timer, hold breath and sway amplitude: deterministic per tick.
static void gun_feel_tick(WeaponState *ws, const PlayerSim *sim, const RecoilParams *rp, const UserCmd *cmd, float dt) {
    bool aim = (cmd->buttons & BTN_AIM) && sim->move.mode == MOVE_WALK && !ws->reloading && ws->equip_t > 0.15f;
    float rate = dt / fmaxf(0.02f, rp->ads_time);
    ws->ads_t = aim ? fminf(1.0f, ws->ads_t + rate) : fmaxf(0.0f, ws->ads_t - rate * 1.3f);
    ws->ads_on = aim ? 1 : 0;
    ws->since_sprint = sim->move.sprinting ? 0.0f : fminf(ws->since_sprint + dt, 10.0f);
    // Hold breath: sprint key while aimed steadies the sway until the capacity runs out.
    bool want_hold = ws->ads_t > 0.8f && (cmd->buttons & BTN_SPRINT) && rp->hold_breath_time > 0;
    ws->breath_holding = (u8)(want_hold && !ws->breath_exhausted);
    if (ws->breath_holding) {
        ws->breath += dt;
        if (ws->breath >= rp->hold_breath_time) { ws->breath_exhausted = 1; ws->breath_holding = 0; }
    } else {
        ws->breath = fmaxf(0.0f, ws->breath - dt * (ws->breath_exhausted ? 0.5f : 0.8f));
        if (ws->breath_exhausted && ws->breath <= 0.0f) ws->breath_exhausted = 0;
    }
    float sway_target = ws->breath_holding ? 0.12f : (ws->breath_exhausted ? 1.8f : 1.0f);
    if (ws->sway_k <= 0.0f) ws->sway_k = 1.0f;
    ws->sway_k = lerpf(ws->sway_k, sway_target, 1.0f - expf(-dt * 5.0f));
}

void weapons_tick(World *w, int slot, PlayerSim *sim, const UserCmd *cmd, u16 pressed, float dt, bool first, WeaponOutput *out) {
    memset(out, 0, sizeof(*out));
    WeaponState *ws = &sim->wpn;
    Player *p = &w->players[slot];
    bool local_view = p->local || slot == game_view_slot();   // the viewed player (local or spectated) predicts its effects
    if (sim->move.mode == MOVE_DEAD) return;

    u8 want = (u8)DG_CLAMP((int)cmd->slot, 0, INV_HOTBAR - 1);
    if ((cmd->buttons & BTN_PHONE) && w->items->builtin_phone) want = WPN_SLOT_PHONE;   // the built-in phone, out over any slot
    bool equip_changed = want != ws->slot;
    if (equip_changed) {
        ws->prev_slot = ws->slot;
        ws->slot = want;
        ws->reloading = 0;
        ws->reload_t = 0;
        ws->swing_t = -1;
        ws->use_t = -1;
        ws->equip_t = 0;
        ws->ads_t = 0;
        ws->burst_left = 0;
        ws->cooldown = 0;
    }
    ws->equip_t += dt;
    ws->recoil_kick = fmaxf(0.0f, ws->recoil_kick - dt * 6.0f);

    InvSlot *s = ws->slot == WPN_SLOT_PHONE ? phone_slot(w, slot) : inv_hot(&p->inv, ws->slot);   // the bound instance (docs/INVENTORY.md)
    const ItemDef *idef = item_def(w->items, s->item);
    const WeaponDef *d = item_weapon(w->items, s->item);
    // Attachments: the host and the predicting client apply the same modifiers (same inventory).
    static WeaponDef eff[MAX_PLAYERS];
    if (d && slot >= 0 && slot < MAX_PLAYERS) {
        attach_weapon_effective(w->items, d, s->att, &eff[slot]);
        d = &eff[slot];
    }
    bool gun = d && d->kind != WPN_MELEE;
    if (s->item != ws->item) {
        // A different item landed in the selected slot (pickup / give / inventory move).
        ws->item = s->item;
        if (!equip_changed) {
            ws->reloading = 0;
            ws->reload_t = 0;
            ws->swing_t = -1;
            ws->ads_t = 0;
            ws->burst_left = 0;
            ws->cooldown = 0;
            ws->equip_t = dt;
        }
        equip_changed = true;
    }
    if (equip_changed) {
        reset_recoil(ws, d, cmd, slot);
        ws->rc.bloom = 0;
        if (gun) ws->fire_mode = (u8)default_mode(d->recoil.fire_modes);
    }
    // How much of this tick's view change was the player fighting the recoil (needs every tick).
    static RecoilParams no_gun;
    if (!no_gun.rpm) recoil_params_default(&no_gun);
    const RecoilParams *rp = gun ? &d->recoil : &no_gun;
    recoil_observe_input(rp, &ws->rc, cmd->pitch * DG_RAD2DEG, -cmd->yaw * DG_RAD2DEG);

    // The host owns inventory; clients predict the magazine in WeaponState only, so replays
    // of unacknowledged commands never double-spend inventory.
    if (w->authority || ws->equip_t <= dt) ws->mag = s->ammo;
    i16 *mag = w->authority ? &s->ammo : &ws->mag;
    bool attack = (cmd->buttons & BTN_ATTACK) != 0;
    bool busy = sim->move.mode != MOVE_WALK;
    if (sim->move.mode == MOVE_SEATED) busy = !(g_vh.can_shoot && g_vh.can_shoot(w, slot, sim, d));   // passengers, one-handed drivers
    // Swimming: a one-handed weapon (sidearm) at the surface only; under water everything is stowed.
    if (sim->move.mode == MOVE_SWIM) busy = !(sim->move.water == WATER_SURFACE && d && d->one_hand_capable);
    // Hanging from a ledge: a sidearm in the free hand (no reload: the other hand holds on).
    if (sim->move.mode == MOVE_HANG) busy = !weapon_is_sidearm(d);

    // Throwables: hold to cook / charge, release to throw (gameplay/throwables.c).
    if (idef->kind == ITEM_THROWABLE && s->item) {
        throwables_held_tick(w, slot, sim, idef, s, attack, busy, dt, first);
        return;
    }
    // Consumables: hold attack for use_time.
    if (idef->kind == ITEM_CONSUMABLE && s->item) {
        if (attack && !busy) {
            if (ws->use_t < -50.0f) return;   // finished / interrupted: wait for the release
            if (ws->use_t < 0) ws->use_t = 0;
            ws->use_t += dt;
            // Taking damage interrupts bandaging / medkits (host; the client gets the state back).
            if (idef->interrupt_on_damage && w->authority && ws->use_t > dt && w->health[ENT_INDEX(p->ent)].last_damage_time >= w->time - dt * 0.5f) {
                ws->use_t = -100.0f;
                ev_hint(w, slot, "%s interrupted", idef->name);
                return;
            }
            if (ws->use_t >= idef->use_time) {
                if (w->authority) {
                    bool used = true;
                    if (idef->vehicle_use) used = g_vh.use_item && g_vh.use_item(w, slot, idef);   // fuel can, tire kit, spare tire
                    else {
                        heal(w, p->ent, idef->heal, idef->armor);
                        ev_hint(w, slot, "Used %s", idef->name);
                        ev_sound(w, "heal", sim->move.pos, 0.8f, EVENT_ALL);
                    }
                    if (used) {
                        inv_use_one(w->items, s);   // a charge, or one of the stack
                        p->inv.revision++;
                    }
                }
                ws->use_t = -100.0f; // wait for release
            }
        } else {
            ws->use_t = -1;
        }
        return;
    }
    ws->use_t = -1;
    if (!d) {
        // Camera items (the phone, docs/PHONE.md): always raised into the focused (camera-ready)
        // pose while equipped, replicated like ADS; no aim button. Other items never aim (a stale
        // flag from the previous weapon is cleared).
        bool aim = idef->camera && s->item && sim->move.mode == MOVE_WALK && ws->equip_t > 0.15f;
        ws->ads_t = aim ? fminf(1.0f, ws->ads_t + dt / 0.22f) : fmaxf(0.0f, ws->ads_t - dt / 0.22f);
        ws->ads_on = aim ? 1 : 0;
        return;
    }

    if (d->kind == WPN_MELEE) {
        if (ws->cooldown > 0) ws->cooldown -= dt;
        if (ws->swing_t >= 0) {
            ws->swing_t += dt;
            melee_step(w, slot, sim, d, first, local_view);
            if (ws->swing_t >= d->swing_time) ws->swing_t = -1;
        } else if ((d->automatic ? attack : (pressed & BTN_ATTACK) != 0) && ws->cooldown <= 0 && !busy && ws->equip_t > 0.2f) {
            ws->swing_t = 0;
            ws->swing_hit_done = 0;
            ws->cooldown = 1.0f / fmaxf(0.1f, d->fire_rate);
            out->swung = true;
            if (local_view && first) fx_sound(d->sound_fire, sim->move.pos, 0.8f);
            if (w->authority) {
                GameEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = EV_MUZZLE;
                ev.target = EVENT_ALL;
                ev.except = (u8)slot;
                ev.ent = p->ent;
                ev_push(w, &ev);
            }
        }
        return;
    }

    // ---- Guns
    float rscale = cv_f(WC.recoil_scale, 1), sscale = cv_f(WC.spread_scale, 1);
    if (!(rp->fire_modes & (1u << ws->fire_mode))) ws->fire_mode = (u8)default_mode(rp->fire_modes);
    if ((pressed & BTN_FIREMODE) && !ws->reloading) {
        FireMode nm = next_mode((FireMode)ws->fire_mode, rp->fire_modes);
        if (nm != ws->fire_mode) {
            ws->fire_mode = (u8)nm;
            ws->burst_left = 0;
            out->mode_changed = true;
            if (local_view && first) fx_sound("switch", sim->move.pos, 0.5f);
        }
    }
    gun_feel_tick(ws, sim, rp, cmd, dt);

    RecoilKick tick_kick = { 0, 0 };
    int reserve = d->ammo_item ? inv_count(&p->inv, d->ammo_item) : 0;
    bool wants_reload = (pressed & BTN_RELOAD) || ((pressed & BTN_ATTACK) && *mag <= 0);
    if (ws->reloading) {
        ws->reload_t -= dt;
        ws->cooldown = 0;
        if (ws->reload_t <= 0) {
            int need = d->mag_size - *mag;
            int got = w->authority ? inv_remove(&p->inv, d->ammo_item, need) : DG_MIN(need, reserve);
            *mag = (i16)(*mag + got);
            ws->reloading = 0;
            reset_recoil(ws, d, cmd, slot);   // new magazine: pattern restarts with a new seed
        }
        if (w->authority) ws->mag = s->ammo;
    } else if (wants_reload && *mag < d->mag_size && reserve > 0 && !busy && sim->move.mode != MOVE_HANG) {
        ws->reloading = 1;
        ws->reload_t = d->reload_time;
        ws->burst_left = 0;
        if (local_view && first) fx_sound(d->sound_reload, sim->move.pos, 0.7f);
    } else {
        FireMode mode = (FireMode)ws->fire_mode;
        bool ready = ws->equip_t > 0.3f && !busy && !sim->move.sprinting && ws->since_sprint >= rp->sprint_to_fire;
        float interval = 60.0f / fmaxf(1.0f, rp->rpm);
        int cap = 1;
        bool trigger;
        if (mode == FM_AUTO) {
            trigger = attack;
            cap = 8;
        } else if (mode == FM_BURST) {
            if ((pressed & BTN_ATTACK) && ws->burst_left == 0 && ws->cooldown <= dt) ws->burst_left = (u8)DG_CLAMP(rp->burst_count, 1, 10);
            trigger = ws->burst_left > 0;
            interval = 60.0f / fmaxf(1.0f, rp->burst_rpm);
            cap = ws->burst_left;
        } else {
            trigger = (pressed & BTN_ATTACK) != 0;
        }
        if (trigger && ready && *mag <= 0) {
            if ((pressed & BTN_ATTACK) && local_view && first) fx_sound("dry_fire", sim->move.pos, 0.6f);
            ws->burst_left = 0;
            trigger = false;
        }
        cap = DG_MIN(cap, (int)*mag);
        // block_fire_when_barrel_blocked (host rule, replicated): no shot while the barrel is against a wall.
        if (WC.block_barrel && WC.block_barrel->i && trigger && ready && cap > 0) {
            vec2 sw = weapon_aim_offset_deg(d, ws, &sim->move, weapon_sway_time(cmd->seq));
            vec3 adir = dir_from_angles(sim->move.yaw - sw.x * DG_DEG2RAD, sim->move.pitch + sw.y * DG_DEG2RAD);
            AimQuery bq = weapon_aim_query(w, slot, &sim->move, cmd, adir, d->aim_max_range, d->obstruct_near, 0);
            AimSolve bs;
            weapon_aim_solve(w, slot, &bq, &bs);
            if (bs.cls == AIM_BARREL) {
                ready = false;
                out->barrel_blocked = true;
                if ((pressed & BTN_ATTACK) && local_view && first) fx_sound("dry_fire", sim->move.pos, 0.4f);
                if (w->authority && first) g_aim_stats.blocked++;
            }
        }
        int n = recoil_fire_budget(&ws->cooldown, interval, dt, trigger && ready && cap > 0, DG_MAX(cap, 0));
        float cooldown_after = ws->cooldown;
        PlayerMove *m = &sim->move;
        vec3 eye = player_eye_pos(m);
        vec2 sway = weapon_aim_offset_deg(d, ws, m, weapon_sway_time(cmd->seq));
        for (int k = 0; k < n; k++) {
            // Aim of this shot: command view + recoil already applied this tick + the sight's offset.
            float yaw = m->yaw - (tick_kick.yaw + sway.x) * DG_DEG2RAD;
            float pitch = m->pitch + (tick_kick.pitch + sway.y) * DG_DEG2RAD;
            vec3 dir = dir_from_angles(yaw, pitch);
            RecoilMods sm = make_mods(sim, sscale);
            float cone = recoil_spread(rp, &ws->rc, &sm) + chars_limb_spread(w, p->ent);
            if (sim->move.mode == MOVE_SEATED && g_vh.spread_mult) cone *= g_vh.spread_mult(w, slot);
            u32 seed = shot_seed(cmd, slot, ws->shots);
            int shot_in_mag = d->mag_size - *mag;   // 0 = first round of the magazine (tracer loading)
            if (!WC.infinite_ammo->i) (*mag)--;
            if (w->authority) ws->mag = s->ammo;
            AimSolve as;
            fire(w, slot, sim, d, cmd, first, local_view, dir, cone, seed, shot_in_mag, &as);
            ws->shots++;
            RecoilMods am = make_mods(sim, rscale);
            if (ws->rc.shot_index == 0 && ws->since_sprint < rp->sprint_to_fire + 0.35f) am.extra = rp->after_sprint_kick;
            u16 pattern_index = ws->rc.shot_index;
            RecoilKick kk = recoil_shot(rp, &ws->rc, &am);
            tick_kick.pitch += kk.pitch;
            tick_kick.yaw += kk.yaw;
            ws->recoil_kick = 1.0f;
            if (mode == FM_BURST && ws->burst_left > 0) ws->burst_left--;
            out->fired = true;
            out->shots++;
            if (first) {
                ShotEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.world = w;
                ev.slot = slot;
                ev.local_view = local_view;
                ev.def = d;
                ev.eye = eye;
                ev.muzzle = as.origin;
                ev.dir = dir;
                ev.cone_deg = cone;
                ev.pattern_index = pattern_index;
                ev.shot_number = ws->shots;
                ev.mag_left = *mag;
                ev.mode = mode;
                ev.ads_t = ws->ads_t;
                ev.seed = seed;
                // Exact fire time inside this tick (the cooldown carries the remainder): shot k of n was
                // due (n - k) intervals before the cooldown that is left.
                float due = cooldown_after - (float)(n - k) * interval;
                ev.sub_tick = saturate((dt + due) / fmaxf(dt, 1e-6f));
                ev.interval = mode == FM_SINGLE ? 0.0f : interval;
                for (int h = 0; h < g_hook_count; h++) g_hooks[h].fn(&ev, g_hooks[h].user);
            }
        }
        // Burst finished: short pause before the next one.
        if (mode == FM_BURST && n > 0 && ws->burst_left == 0) ws->cooldown += 60.0f / fmaxf(1.0f, rp->rpm);
        if (mode == FM_BURST && (*mag <= 0 || !ready)) ws->burst_left = 0;
    }
    // Recovery (and bloom decay) every tick; report everything applied to the view.
    ws->rc.ads_t = saturate(ws->ads_t);   // bloom recovery: hip / ADS blend
    RecoilKick rec = recoil_tick(rp, &ws->rc, dt);
    tick_kick.pitch += rec.pitch;
    tick_kick.yaw += rec.yaw;
    recoil_set_emitted(&ws->rc, tick_kick);
    out->view_kick = v2(-tick_kick.yaw * DG_DEG2RAD, tick_kick.pitch * DG_DEG2RAD);
}

void projectiles_tick(World *w, float dt) {
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_PROJECTILE) continue;
        if (w->proj[i].item) { throwable_projectile_tick(w, i, dt); continue; }
        EntityId e = ENT_MAKE(i, w->gen[i]);
        Projectile *pr = &w->proj[i];
        pr->life -= dt;
        if (pr->life <= 0) { ent_destroy(w, e); continue; }
        const WeaponDef *d = &w->items->weapons[DG_CLAMP(pr->weapon, 0, w->items->weapon_count - 1)];
        pr->vel.y -= d->projectile_gravity * dt;
        vec3 from = w->xf[i].pos;
        vec3 step = v3_scale(pr->vel, dt);
        float len = v3_len(step);
        vec3 dir = v3_scale(step, 1.0f / fmaxf(len, 1e-5f));
        int shooter = world_player_by_ent(w, pr->owner);
        HitResult h;
        if (shooter >= 0) {
            h = trace_shot(w, shooter, from, dir, len, 0);
        } else {
            memset(&h, 0, sizeof(h));
            RayHit rh;
            if (phys_raycast(w->phys, from, step, PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) {
                h.hit = true; h.point = rh.point; h.normal = rh.normal; h.body = rh.body; h.ent = (EntityId)rh.user;
            }
        }
        if (h.hit) {
            if (shooter >= 0) apply_hit(w, shooter, d, &h, dir, 1.0f);
            GameEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EV_IMPACT;
            ev.target = EVENT_ALL;
            ev.except = EVENT_ALL;
            ev.pos = h.point;
            ev.dir = h.normal;
            ev.id = h.hitbox ? 1 : 0;
            ev_push(w, &ev);
            ev_sound(w, "bolt_hit", h.point, 1.0f, EVENT_ALL);
            ent_destroy(w, e);
            continue;
        }
        vec3 to = v3_add(from, step);
        float yaw = atan2f(-dir.x, -dir.z), pitch = asinf(clampf(dir.y, -1, 1));
        w->xf[i].pos = to;
        w->xf[i].rot = quat_euler(yaw, pitch, 0);
    }
}

bool weapon_trace(World *w, int shooter, vec3 origin, vec3 dir, float range, vec3 *point, EntityId *ent) {
    if (shooter < 0 || shooter >= MAX_PLAYERS) return false;
    HitResult h = trace_shot(w, shooter, origin, dir, range, 0);
    if (!h.hit) return false;
    *point = h.point;
    *ent = h.ent;
    return true;
}
