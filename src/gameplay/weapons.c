#include "weapons.h"
#include "health.h"
#include "effects.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "render/debug_draw.h"
#include <math.h>

const InvSlot *weapon_equipped(const World *w, int slot) {
    const Player *p = &w->players[slot];
    int s = DG_CLAMP((int)p->sim.wpn.slot, 0, INV_HOTBAR - 1);
    return p->inv.slots[s].item ? &p->inv.slots[s] : NULL;
}

const WeaponDef *weapon_equipped_def(const World *w, int slot) {
    const InvSlot *s = weapon_equipped(w, slot);
    return s ? item_weapon(w->items, s->item) : NULL;
}

static float base_spread(const WeaponDef *d, const PlayerMove *m, bool ads) {
    float speed = v3_len(v3_flat(m->vel));
    float move = saturate(speed / 6.0f);
    float s = ads ? d->spread_ads : d->spread_base;
    s += d->spread_move * move * (ads ? 0.5f : 1.0f);
    if (!m->on_ground) s += d->spread_move;
    if (m->crouched) s *= 0.7f;
    return s;
}

float weapon_current_spread(const World *w, int slot) {
    const WeaponDef *d = weapon_equipped_def(w, slot);
    const Player *p = &w->players[slot];
    if (!d || d->kind == WPN_MELEE) return 0;
    return base_spread(d, &p->sim.move, (p->last_cmd.buttons & BTN_AIM) != 0) + p->sim.wpn.spread;
}

// Uniform random direction inside a cone of half-angle deg around dir.
static vec3 spread_dir(vec3 dir, float deg, Rng *rng) {
    if (deg <= 0.0f) return dir;
    vec3 t = fabsf(dir.y) < 0.95f ? v3_norm(v3_cross(dir, v3(0, 1, 0))) : v3(1, 0, 0);
    vec3 b = v3_cross(t, dir);
    float a = rng_float(rng) * 2.0f * DG_PI;
    float r = sqrtf(rng_float(rng)) * tanf(deg * DG_DEG2RAD);
    return v3_norm(v3_add(dir, v3_add(v3_scale(t, cosf(a) * r), v3_scale(b, sinf(a) * r))));
}

typedef struct HitResult {
    bool hit;
    float dist;
    vec3 point, normal;
    EntityId ent;
    BodyId body;
    bool hitbox;
    bool headshot;
} HitResult;

// Lag-compensated hitscan: world geometry via Jolt, hitbox capsules rewound to view_tick.
static HitResult trace_shot(World *w, int shooter, vec3 origin, vec3 dir, float range, u32 view_tick256) {
    HitResult r;
    memset(&r, 0, sizeof(r));
    Player *sp = &w->players[shooter];
    BodyId self = sp->character ? phys_character_inner_body(sp->character) : PHYS_INVALID_BODY;
    RayHit rh;
    if (phys_raycast(w->phys, origin, v3_scale(dir, range), PL_MASK_WORLD, self, &rh)) {
        r.hit = true;
        r.dist = rh.distance;
        r.point = rh.point;
        r.normal = rh.normal;
        r.ent = (EntityId)rh.user;
        r.body = rh.body;
    }
    float best = r.hit ? r.dist : range;
    u32 view_tick = view_tick256 >> 8;
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_HITBOX) || (w->flags[i] & EF_DEAD)) continue;
        EntityId e = ENT_MAKE(i, w->gen[i]);
        if (e == sp->ent) continue;
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
            r.hit = true;
            r.dist = t;
            r.point = v3_madd(origin, dir, t);
            r.normal = v3_neg(dir);
            r.ent = e;
            r.hitbox = true;
            r.headshot = head;
            r.body = PHYS_INVALID_BODY;
        }
    }
    return r;
}

static void apply_hit(World *w, int shooter, const WeaponDef *d, const HitResult *h, vec3 dir, float damage_scale) {
    Player *sp = &w->players[shooter];
    if (h->hitbox) {
        float dmg = d->damage * damage_scale * (h->headshot ? d->headshot_mult : 1.0f);
        apply_damage(w, h->ent, dmg, d->damage_type, sp->ent, h->point, dir, h->headshot);
        return;
    }
    if (h->body != PHYS_INVALID_BODY) phys_add_impulse(w->phys, h->body, v3_scale(dir, d->impulse * damage_scale), h->point);
    if (ent_valid(w, h->ent) && (w->comps[ENT_INDEX(h->ent)] & C_HEALTH))
        apply_damage(w, h->ent, d->damage * damage_scale, d->damage_type, sp->ent, h->point, dir, false);
}

static void push_shot_events(World *w, int shooter, vec3 from, const HitResult *h, vec3 end) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_TRACER;
    ev.target = EVENT_ALL;
    ev.except = (u8)shooter;
    ev.pos = from;
    ev.dir = end;
    ev_push(w, &ev);
    if (h->hit) {
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_IMPACT;
        ev.target = EVENT_ALL;
        ev.except = (u8)shooter;
        ev.pos = h->point;
        ev.dir = h->normal;
        ev.id = h->hitbox ? 1 : 0;
        ev_push(w, &ev);
    }
}

static vec3 muzzle_world(const PlayerMove *m, const WeaponDef *d) {
    // Approximate muzzle in front of the right side of the eye (viewmodel aligned).
    vec3 eye = player_eye_pos(m);
    vec3 f = dir_from_angles(m->yaw, m->pitch);
    vec3 r = right_from_yaw(m->yaw);
    vec3 u = v3_cross(r, f);
    return v3_add(eye, v3_add(v3_scale(f, 0.45f - d->muzzle_offset.z * 0.5f), v3_add(v3_scale(r, 0.14f), v3_scale(u, -0.1f))));
}

static void fire(World *w, int slot, PlayerSim *sim, const WeaponDef *d, const UserCmd *cmd, bool first, bool local_view) {
    PlayerMove *m = &sim->move;
    bool ads = (cmd->buttons & BTN_AIM) != 0;
    vec3 eye = player_eye_pos(m);
    vec3 dir = dir_from_angles(m->yaw, m->pitch);
    float spread = base_spread(d, m, ads) + sim->wpn.spread;
    Rng rng = rng_make(((u64)cmd->seq << 8) ^ (u64)(slot + 1) * 0x9E3779B97F4A7C15ULL ^ sim->wpn.shots);
    vec3 muzzle = muzzle_world(m, d);
    if (local_view && first) {
        fx_muzzle(muzzle, dir);
        fx_sound(d->sound_fire, eye, 1.0f);
    }
    if (d->kind == WPN_PROJECTILE) {
        if (w->authority) {
            EntityId e = ent_spawn(w, ET_PROJECTILE);
            int i = (int)ENT_INDEX(e);
            w->vis[i].shape = VIS_BOX;
            w->vis[i].half = v3(0.012f, 0.012f, 0.22f);
            w->vis[i].color = rgba8(0.5f, 0.4f, 0.3f, 1);
            w->comps[i] |= C_VISUAL | C_PROJECTILE;
            vec3 pdir = spread_dir(dir, spread, &rng);
            w->proj[i].vel = v3_scale(pdir, d->projectile_speed);
            w->proj[i].owner = w->players[slot].ent;
            w->proj[i].weapon = (int)(d - w->items->weapons);
            w->proj[i].life = 8.0f;
            float yaw = atan2f(-pdir.x, -pdir.z), pitch = asinf(clampf(pdir.y, -1, 1));
            ent_set_pos(w, e, v3_madd(eye, pdir, 0.5f), quat_euler(yaw, pitch, 0), true);
            ev_sound(w, d->sound_fire, eye, 1.0f, slot);
        }
        return;
    }
    int pellets = DG_MAX(1, d->pellets);
    for (int p = 0; p < pellets; p++) {
        vec3 sdir = spread_dir(dir, spread, &rng);
        if (w->authority) {
            HitResult h = trace_shot(w, slot, eye, sdir, d->range, cmd->view_tick);
            vec3 end = h.hit ? h.point : v3_madd(eye, sdir, d->range);
            if (h.hit) apply_hit(w, slot, d, &h, sdir, 1.0f);
            push_shot_events(w, slot, muzzle, &h, end);
            if (local_view && first) {
                fx_tracer(muzzle, end);
                if (h.hit) fx_impact(h.point, h.normal, h.hitbox ? 1 : 0);
            }
        } else if (local_view && first) {
            // Predicted cosmetic trace against the replica world.
            HitResult h = trace_shot(w, slot, eye, sdir, d->range, 0);
            vec3 end = h.hit ? h.point : v3_madd(eye, sdir, d->range);
            fx_tracer(muzzle, end);
            if (h.hit) fx_impact(h.point, h.normal, h.hitbox ? 1 : 0);
        }
    }
    if (w->authority) ev_sound(w, d->sound_fire, eye, 1.0f, slot);
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
    // Hitboxes: segment eye->tip against capsules.
    float best = d->reach + 1;
    EntityId hit_ent = ENT_NONE;
    vec3 hit_point = tip;
    bool head = false;
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_HITBOX) || (w->flags[i] & EF_DEAD)) continue;
        EntityId e = ENT_MAKE(i, w->gen[i]);
        if (e == p->ent) continue;
        vec3 feet = w->type[i] == ET_DUMMY ? v3_sub(w->xf[i].pos, v3(0, 0.9f, 0)) : w->xf[i].pos;
        float r = w->hitbox[i].radius + d->sweep_radius;
        vec3 a = v3_add(feet, v3(0, 0.3f, 0)), b = v3_add(feet, v3(0, w->hitbox[i].height - 0.2f, 0));
        float t = ray_capsule(eye, dir, a, b, r);
        if (t > 0 && t < best && t <= d->reach) {
            best = t;
            hit_ent = e;
            hit_point = v3_madd(eye, dir, t);
            head = hit_point.y > feet.y + w->hitbox[i].head_height - 0.12f;
        }
    }
    ShapeHit sh;
    bool world_hit = phys_sphere_cast(w->phys, eye, v3_scale(dir, d->reach), d->sweep_radius, PL_MASK_WORLD, self, &sh);
    if (world_hit && sh.distance < best) {
        ws->swing_hit_done = 1;
        if (local_view && first) { fx_impact(sh.point, sh.normal, 0); fx_sound("melee_hit_wall", sh.point, 0.9f); }
        if (w->authority) {
            phys_add_impulse(w->phys, sh.body, v3_scale(dir, d->knockback * 0.25f), sh.point);
            GameEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EV_IMPACT;
            ev.target = EVENT_ALL;
            ev.except = (u8)slot;
            ev.pos = sh.point;
            ev.dir = sh.normal;
            ev_push(w, &ev);
            if (ent_valid(w, (EntityId)sh.user) && (w->comps[ENT_INDEX(sh.user)] & C_HEALTH))
                apply_damage(w, (EntityId)sh.user, d->damage, d->damage_type, p->ent, sh.point, dir, false);
        }
        return;
    }
    if (hit_ent) {
        ws->swing_hit_done = 1;
        if (local_view && first) { fx_impact(hit_point, v3_neg(dir), 1); fx_sound("melee_hit", hit_point, 1.0f); }
        if (w->authority) apply_damage(w, hit_ent, d->damage * (head ? d->headshot_mult : 1.0f), d->damage_type, p->ent, hit_point, dir, head);
    }
}

void weapons_tick(World *w, int slot, PlayerSim *sim, const UserCmd *cmd, u16 pressed, float dt, bool first, WeaponOutput *out) {
    memset(out, 0, sizeof(*out));
    WeaponState *ws = &sim->wpn;
    Player *p = &w->players[slot];
    bool local_view = p->local;
    if (sim->move.mode == MOVE_DEAD) return;

    u8 want = (u8)DG_CLAMP((int)cmd->slot, 0, INV_HOTBAR - 1);
    if (want != ws->slot) {
        ws->prev_slot = ws->slot;
        ws->slot = want;
        ws->reloading = 0;
        ws->reload_t = 0;
        ws->swing_t = -1;
        ws->use_t = -1;
        ws->equip_t = 0;
        ws->spread = 0;
    }
    ws->equip_t += dt;
    if (ws->cooldown > 0) ws->cooldown -= dt;
    ws->recoil_kick = fmaxf(0.0f, ws->recoil_kick - dt * 6.0f);

    InvSlot *s = &p->inv.slots[ws->slot];
    const ItemDef *idef = item_def(w->items, s->item);
    const WeaponDef *d = item_weapon(w->items, s->item);
    bool attack = (cmd->buttons & BTN_ATTACK) != 0;
    bool busy = sim->move.mode != MOVE_WALK;

    // Consumables: hold attack for use_time.
    if (idef->kind == ITEM_CONSUMABLE && s->item) {
        if (attack && !busy) {
            if (ws->use_t < 0) ws->use_t = 0;
            ws->use_t += dt;
            if (ws->use_t >= idef->use_time) {
                if (w->authority) {
                    heal(w, p->ent, idef->heal, idef->armor);
                    ev_hint(w, slot, "Used %s", idef->name);
                    ev_sound(w, "heal", sim->move.pos, 0.8f, EVENT_ALL);
                    s->count--;
                    if (s->count == 0) memset(s, 0, sizeof(*s));
                    p->inv.revision++;
                }
                ws->use_t = -100.0f; // wait for release
            }
        } else {
            ws->use_t = -1;
        }
        return;
    }
    ws->use_t = -1;
    if (!d) return;

    if (d->kind == WPN_MELEE) {
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

    // Guns
    ws->spread = fmaxf(0.0f, ws->spread - d->spread_recover * dt);
    int reserve = d->ammo_item ? inv_count(&p->inv, d->ammo_item) : 0;
    if (ws->reloading) {
        ws->reload_t -= dt;
        if (ws->reload_t <= 0) {
            int need = d->mag_size - s->ammo;
            int got = inv_remove(&p->inv, d->ammo_item, need);
            s->ammo = (i16)(s->ammo + got);
            ws->reloading = 0;
        }
        return;
    }
    bool wants_reload = (pressed & BTN_RELOAD) || ((pressed & BTN_ATTACK) && s->ammo <= 0);
    if (wants_reload && s->ammo < d->mag_size && reserve > 0 && !busy) {
        ws->reloading = 1;
        ws->reload_t = d->reload_time;
        if (local_view && first) fx_sound(d->sound_reload, sim->move.pos, 0.7f);
        return;
    }
    bool trigger = d->automatic ? attack : (pressed & BTN_ATTACK) != 0;
    bool can_fire = ws->cooldown <= 0 && ws->equip_t > 0.3f && !busy && !sim->move.sprinting;
    if (trigger && can_fire) {
        if (s->ammo <= 0) {
            if ((pressed & BTN_ATTACK) && local_view && first) fx_sound("dry_fire", sim->move.pos, 0.6f);
            return;
        }
        s->ammo--;
        ws->cooldown = 1.0f / fmaxf(0.1f, d->fire_rate);
        fire(w, slot, sim, d, cmd, first, local_view);
        ws->shots++;
        ws->spread = fminf(d->spread_max, ws->spread + d->spread_per_shot);
        ws->recoil_kick = 1.0f;
        out->fired = true;
        // Recoil: mostly up, random-ish sideways (deterministic from shot count).
        Rng rr = rng_make(ws->shots * 7919u + (u64)slot);
        float side = rng_range(&rr, -1.0f, 1.0f);
        float ads_mult = (cmd->buttons & BTN_AIM) ? 0.6f : 1.0f;
        out->view_kick = v2(side * d->recoil_yaw * DG_DEG2RAD * ads_mult, d->recoil_pitch * DG_DEG2RAD * ads_mult);
    }
}

void projectiles_tick(World *w, float dt) {
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_PROJECTILE) continue;
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
