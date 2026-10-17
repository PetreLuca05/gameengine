// Throwables (src/gameplay/throwables.h).
#include "throwables.h"
#include "health.h"
#include "effects.h"
#include "weapons.h"
#include "aim.h"
#include "characters.h"
#include "zombies/horde.h"
#include "render/render.h"
#include <math.h>
#include <string.h>

static void consume(World *w, int slot, InvSlot *s) {
    if (!w->authority) return;
    s->count--;
    if (s->count == 0) memset(s, 0, sizeof(*s));
    w->players[slot].inv.revision++;
}

static void explode(World *w, vec3 at, const ItemDef *d, EntityId owner, u16 item) {
    float radius = fmaxf(0.5f, d->thr.radius);
    health_set_source_item(item);
    ENT_FOREACH(w, k) {
        if (!(w->comps[k] & C_HEALTH) || (w->flags[k] & EF_DEAD)) continue;
        EntityId e = ENT_MAKE(k, w->gen[k]);
        vec3 c = w->xf[k].pos;
        if (w->type[k] == ET_PLAYER || w->type[k] == ET_ZOMBIE) c.y += 1.0f;   // feet -> chest
        vec3 to = v3_sub(c, at);
        float dist = v3_len(to);
        if (dist > radius) continue;
        // Cover: anything solid in between (other than the target) blocks it.
        RayHit rh;
        if (dist > 0.3f && phys_raycast(w->phys, at, v3_scale(to, 0.98f), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh) &&
            (EntityId)rh.user != e && rh.distance < dist - 0.35f) continue;
        float f = 1.0f - powf(dist / radius, 1.4f);
        vec3 dir = dist > 1e-3f ? v3_scale(to, 1.0f / dist) : v3(0, 1, 0);
        apply_damage(w, e, d->thr.damage * f, DMG_EXPLOSIVE, owner, c, dir, false);
    }
    health_set_source_item(0);
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_EXPLOSION;
    ev.target = EVENT_ALL;
    ev.except = EVENT_ALL;
    ev.pos = at;
    ev.value = radius;
    ev.id = item;
    ev_push(w, &ev);
    ev_sound(w, d->thr.kind == THROW_MOLOTOV ? "molotov" : "explosion", at, 1.8f, EVENT_ALL);
    horde_noise(at, radius * 8.0f);
}

static void spawn_thrown(World *w, int slot, const PlayerSim *sim, u16 item, const ItemDef *d, float charge, float fuse_left) {
    const PlayerMove *m = &sim->move;
    vec3 eye = player_eye_pos(m);
    // Aim model (docs/AIM.md): from the throwing hand, converging on the camera ray's aim point (the
    // same muzzle-inside-geometry recovery as a gun), then a little loft over that line.
    AimQuery q = weapon_aim_query(w, slot, m, NULL, dir_from_angles(m->yaw, m->pitch), 40.0f, 0.25f, 0);
    q.hand = true;
    vec3 palm;
    if (chars_palm_world(w, w->players[slot].ent, 1, &palm)) {
        q.have_claim = true;
        q.claim = aim_world_to_cam(q.eye, q.yaw, q.pitch, palm);
    }
    AimSolve as;
    weapon_aim_solve(w, slot, &q, &as);
    float cy = atan2f(-as.dir.x, -as.dir.z), cp = asinf(clampf(as.dir.y, -1.0f, 1.0f));
    vec3 dir = dir_from_angles(cy, clampf(cp + 8.0f * DG_DEG2RAD, -1.5f, 1.5f));
    float speed = lerpf(d->thr.speed_min, d->thr.speed_max, saturate(charge));
    EntityId e = ent_spawn(w, ET_PROJECTILE);
    int i = (int)ENT_INDEX(e);
    w->vis[i].shape = d->shape;
    w->vis[i].half = d->size;
    w->vis[i].color = rgba8(d->color.x, d->color.y, d->color.z, 1);
    w->comps[i] |= C_VISUAL | C_PROJECTILE;
    w->item[i] = item;
    w->anim[i] = 0;
    Projectile *pr = &w->proj[i];
    memset(pr, 0, sizeof(*pr));
    pr->vel = v3_add(v3_scale(dir, speed), v3_scale(v3_flat(m->vel), 0.6f));
    pr->owner = w->players[slot].ent;
    pr->weapon = -1;
    pr->item = item;
    pr->life = 120.0f;
    pr->fuse = d->thr.kind == THROW_GRENADE ? fuse_left : 0.0f;
    if (d->thr.kind == THROW_FLARE) { pr->burn = d->thr.light_time; w->anim[i] = 1.0f; }
    ent_set_pos(w, e, as.origin, quat_yaw(m->yaw), true);
    ev_sound(w, "swing", eye, 0.6f, EVENT_ALL);
}

void throwables_held_tick(World *w, int slot, PlayerSim *sim, const ItemDef *def, InvSlot *s, bool attack, bool busy, float dt, bool first) {
    WeaponState *ws = &sim->wpn;
    float fuse = def->thr.kind == THROW_GRENADE ? def->thr.fuse : 0.0f;
    if (attack && !busy && ws->equip_t > 0.2f) {
        if (ws->use_t < -50.0f) return;   // thrown / went off: wait for the release
        if (ws->use_t < 0) {
            ws->use_t = 0;
            if (first && w->authority) ev_sound(w, "pin", sim->move.pos, 0.7f, EVENT_ALL);
        }
        ws->use_t += dt;
        if (fuse > 0 && ws->use_t >= fuse) {
            // Cooked too long: it goes off in the hand.
            if (w->authority) {
                const PlayerMove *m = &sim->move;
                explode(w, v3_madd(player_eye_pos(m), dir_from_angles(m->yaw, m->pitch), 0.4f), def, w->players[slot].ent, s->item);
                consume(w, slot, s);
            }
            ws->use_t = -100.0f;
        }
        return;
    }
    if (ws->use_t >= 0) {
        float charge = ws->use_t / fmaxf(0.05f, def->thr.charge_time);
        if (w->authority && s->item) {
            spawn_thrown(w, slot, sim, s->item, def, charge, fmaxf(0.05f, fuse - ws->use_t));
            consume(w, slot, s);
        }
        ws->use_t = -1.0f;
    } else if (!attack) {
        ws->use_t = -1.0f;
    }
}

static void fire_damage(World *w, int i, const ItemDef *d, float dt) {
    vec3 at = w->xf[i].pos;
    health_set_source_item(w->item[i]);
    float radius = fmaxf(0.5f, d->thr.radius);
    ENT_FOREACH(w, k) {
        if (!(w->comps[k] & C_HEALTH) || (w->flags[k] & EF_DEAD)) continue;
        vec3 c = w->xf[k].pos;
        if (v3_dist(v3(c.x, at.y, c.z), at) > radius || fabsf(c.y - at.y) > 2.0f) continue;
        // damage = the burst's damage spread over the burn (a few seconds standing in it kills).
        apply_damage(w, ENT_MAKE(k, w->gen[k]), d->thr.damage * 0.25f * dt, DMG_GENERIC, w->proj[i].owner, c, v3(0, 1, 0), false);
    }
    health_set_source_item(0);
}

void throwable_projectile_tick(World *w, int i, float dt) {
    EntityId e = ENT_MAKE(i, w->gen[i]);
    Projectile *pr = &w->proj[i];
    const ItemDef *d = item_def(w->items, pr->item);
    pr->life -= dt;
    if (pr->life <= 0) { ent_destroy(w, e); return; }
    if (!pr->resting) {
        pr->vel.y -= 9.81f * dt;
        vec3 from = w->xf[i].pos;
        vec3 step = v3_scale(pr->vel, dt);
        int owner = world_player_by_ent(w, pr->owner);
        BodyId self = owner >= 0 && w->players[owner].character ? phys_character_inner_body(w->players[owner].character) : PHYS_INVALID_BODY;
        RayHit rh;
        if (v3_len(step) > 1e-5f && phys_raycast(w->phys, from, step, PL_MASK_WORLD, self, &rh)) {
            if (d->thr.kind == THROW_MOLOTOV) {
                // Bursts: the impact damage, then a fire for a few seconds.
                vec3 at = v3_madd(rh.point, rh.normal, 0.05f);
                explode(w, at, d, pr->owner, pr->item);
                pr->resting = true;
                pr->vel = v3(0, 0, 0);
                pr->burn = 6.0f;
                w->anim[i] = 1.0f;
                w->vis[i].shape = VIS_NONE;
                w->xf[i].pos = at;
                return;
            }
            // Bounce: reflect, keep a share of the speed, rest when slow on a floor.
            float vn = v3_dot(pr->vel, rh.normal);
            vec3 refl = v3_sub(pr->vel, v3_scale(rh.normal, 2.0f * vn));
            float speed = v3_len(pr->vel);
            pr->vel = v3_scale(refl, d->thr.bounce);
            pr->vel = v3_madd(pr->vel, rh.normal, 0.05f);
            w->xf[i].pos = v3_madd(rh.point, rh.normal, 0.03f + d->size.y);
            if (speed > 2.5f) ev_sound(w, "impact_hard", rh.point, fminf(1.0f, speed / 12.0f), EVENT_ALL);
            if (v3_len(pr->vel) < 1.2f && rh.normal.y > 0.6f) { pr->resting = true; pr->vel = v3(0, 0, 0); }
        } else {
            w->xf[i].pos = v3_add(from, step);
            float sp = v3_len(pr->vel);
            if (sp > 0.5f) w->xf[i].rot = quat_mul(w->xf[i].rot, quat_axis_angle(v3(1, 0, 0), -sp * dt * 2.0f));   // tumbling
        }
    }
    if (d->thr.kind == THROW_GRENADE) {
        pr->fuse -= dt;
        if (pr->fuse <= 0) {
            explode(w, v3_add(w->xf[i].pos, v3(0, 0.1f, 0)), d, pr->owner, pr->item);
            ent_destroy(w, e);
        }
        return;
    }
    if (pr->burn > 0) {
        pr->burn -= dt;
        if (d->thr.kind == THROW_MOLOTOV) fire_damage(w, i, d, dt);
        if (pr->burn <= 0) {
            w->anim[i] = 0;
            if (d->thr.kind == THROW_MOLOTOV) { ent_destroy(w, e); return; }
            pr->life = fminf(pr->life, 20.0f);   // a spent flare lies there a while
        }
    }
}

void throwables_event(const GameEvent *ev) {
    // Debris bursts in every direction around the blast and a flash.
    for (int k = 0; k < 7; k++) {
        float a = (float)k * 0.897f;
        vec3 n = v3_norm(v3(cosf(a), 0.6f + 0.1f * (float)(k % 3), sinf(a)));
        fx_impact(v3_add(ev->pos, v3_scale(n, 0.2f)), n, 0);
    }
    fx_muzzle(ev->pos, v3(0, 1, 0));
}

void throwables_render(World *w, int i) {
    if (!(w->anim[i] > 0.0f) || !w->item[i]) return;
    const ItemDef *d = item_def(w->items, w->item[i]);
    if (d->kind != ITEM_THROWABLE) return;
    float t = (float)w->time * 13.0f + (float)i * 1.7f;
    float flicker = 0.8f + 0.12f * sinf(t) + 0.08f * sinf(t * 2.3f + 1.1f);
    PointLight l;
    memset(&l, 0, sizeof(l));
    l.pos = v3_add(w->xf[i].pos, v3(0, 0.25f, 0));
    if (d->thr.kind == THROW_FLARE) {
        l.radius = 16.0f;
        l.color = d->thr.light_color;
        l.intensity = 9.0f * flicker;
    } else {
        l.radius = 8.0f;
        l.color = v3(1.0f, 0.5f, 0.15f);
        l.intensity = 12.0f * flicker;
    }
    l.flags = LIGHT_NO_SHADOW;
    render_add_light(&l);
}

bool throwables_ring(const World *w, int slot, const PlayerSim *sim, float *duration, float *remaining, int *mode, const char **label) {
    const InvSlot *s = weapon_equipped(w, slot);
    if (!s || !s->item || sim->wpn.use_t < 0) return false;
    const ItemDef *d = item_def(w->items, s->item);
    if (d->kind != ITEM_THROWABLE) return false;
    if (d->thr.kind == THROW_GRENADE && d->thr.fuse > 0) {
        *duration = d->thr.fuse;
        *remaining = fmaxf(0.0f, d->thr.fuse - sim->wpn.use_t);
        *mode = 0;
        *label = "Cooking";
    } else {
        *duration = fmaxf(0.05f, d->thr.charge_time);
        *remaining = fmaxf(0.0f, *duration - sim->wpn.use_t);
        *mode = 1;
        *label = "Throw";
    }
    return true;
}
