#include "health.h"
#include "weapons.h"
#include "characters.h"
#include "kill_stats.h"
#include <stdio.h>

void health_init(Health *h, float hp, float armor) {
    memset(h, 0, sizeof(*h));
    h->hp = h->max_hp = hp;
    h->armor = armor;
    h->max_armor = 100.0f;
    for (int i = 0; i < DMG_COUNT; i++) h->resist[i] = 1.0f;
}

// The item doing the damage when it isn't the attacker's held one (thrown grenades): the kill feed icon.
static u16 s_source_item;
void health_set_source_item(u16 item) { s_source_item = item; }

float apply_damage(World *w, EntityId target, float amount, DamageType type, EntityId attacker, vec3 point, vec3 dir, bool headshot) {
    return apply_damage_part(w, target, amount, type, attacker, point, dir, headshot ? BP_HEAD : -1, 0.0f);
}

float apply_damage_part(World *w, EntityId target, float amount, DamageType type, EntityId attacker, vec3 point, vec3 dir, int part, float part_mult) {
    bool headshot = part == BP_HEAD;
    if (!w->authority || !ent_valid(w, target)) return 0;
    int i = (int)ENT_INDEX(target);
    if (!(w->comps[i] & C_HEALTH) || (w->flags[i] & EF_DEAD)) return 0;
    Health *h = &w->health[i];
    float dmg = amount * h->resist[type];
    if (dmg <= 0) return 0;
    // Armor soaks a share of the damage until it runs out.
    if (h->armor > 0 && type != DMG_FALL) {
        float soak = fminf(h->armor, dmg * ARMOR_ABSORB);
        h->armor -= soak;
        dmg -= soak;
    }
    float before = h->hp;
    h->hp = fmaxf(0.0f, h->hp - dmg);
    h->last_damage_time = w->time;
    float dealt = before - h->hp;
    bool killed = h->hp <= 0.0f;

    int victim_player = world_player_by_ent(w, target);
    int attacker_player = world_player_by_ent(w, attacker);
    if (victim_player >= 0 && dealt > 0) weapon_flinch(w, victim_player, dealt);

    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_DAMAGE_NUMBER;
    ev.target = EVENT_ALL;
    ev.except = EVENT_ALL;
    ev.ent = target;
    ev.pos = point;
    ev.value = amount * h->resist[type];
    ev.id = (u32)type | (headshot ? 0x100u : 0u) | ((u32)(part >= 0 ? part + 1 : 0) << 16);
    ev.dir = v3(part_mult, 0, 0);   // dev damage numbers: "part x mult"
    ev_push(w, &ev);

    if (attacker_player >= 0) {
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_HIT_CONFIRM;
        ev.target = (u8)attacker_player;
        ev.except = EVENT_ALL;
        ev.value = dealt;
        ev.id = (killed ? 1u : 0u) | (headshot ? 2u : 0u);
        ev.ent = target;
        ev_push(w, &ev);
    }
    if (victim_player >= 0) {
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_HURT;
        ev.target = (u8)victim_player;
        ev.except = EVENT_ALL;
        ev.value = dmg;
        ev.dir = dir;
        ev.pos = point;
        ev_push(w, &ev);
    }

    if (killed) {
        w->flags[i] |= EF_DEAD;
        h->respawn_timer = victim_player >= 0 ? PLAYER_RESPAWN_TIME : 3.0f;
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_KILL;
        ev.target = EVENT_ALL;
        ev.except = EVENT_ALL;
        ev.ent = target;
        ev.ent2 = attacker;
        const char *vname = victim_player >= 0 ? w->players[victim_player].name : ent_type_name((EntityType)w->type[i]);
        const char *aname = attacker_player >= 0 ? w->players[attacker_player].name : "the world";
        snprintf(ev.text, sizeof(ev.text), "%s killed %s (%s)", aname, vname, damage_type_name(type));
        // Structured for the kill counter / feed (docs/UI_WHEELS_HUD.md).
        ev.id = (headshot ? KILLF_HEADSHOT : 0u) | (victim_player >= 0 ? KILLF_VICTIM_PLAYER : 0u) |
                (w->type[i] == ET_ZOMBIE ? KILLF_VICTIM_ZOMBIE : 0u) | (attacker_player >= 0 ? KILLF_KILLER_PLAYER : 0u) |
                (attacker_player >= 0 && attacker_player == victim_player ? KILLF_SELF : 0u);
        ev.value = (float)type;
        KillEventData kd;
        memset(&kd, 0, sizeof(kd));
        dg_strcpy(kd.killer, sizeof(kd.killer), attacker_player >= 0 ? aname : (ent_valid(w, attacker) ? ent_type_name((EntityType)w->type[ENT_INDEX(attacker)]) : ""));
        dg_strcpy(kd.victim, sizeof(kd.victim), vname);
        if (victim_player < 0 && kd.victim[0] >= 'a' && kd.victim[0] <= 'z') kd.victim[0] = (char)(kd.victim[0] - 32);   // "Zombie"
        if (attacker_player < 0 && kd.killer[0] >= 'a' && kd.killer[0] <= 'z') kd.killer[0] = (char)(kd.killer[0] - 32);
        kd.weapon = s_source_item ? s_source_item : (ent_valid(w, attacker) ? w->item[ENT_INDEX(attacker)] : 0);
        // Only weapons and throwables show as the weapon (a medkit in hand is not what killed it).
        if (kd.weapon && item_def(w->items, kd.weapon)->kind != ITEM_WEAPON && item_def(w->items, kd.weapon)->kind != ITEM_THROWABLE) kd.weapon = 0;
        memcpy(ev.data, &kd, sizeof(kd));
        ev_push(w, &ev);
        if (victim_player >= 0) {
            Player *p = &w->players[victim_player];
            p->sim.move.mode = MOVE_DEAD;
            p->respawn_timer = PLAYER_RESPAWN_TIME;
            p->deaths++;
        }
        if (attacker_player >= 0 && attacker_player != victim_player && victim_player >= 0) w->players[attacker_player].kills++;
    }
    chars_on_damage(w, target, dealt, part, dir, point, killed, (int)type);
    return dealt;
}

void heal(World *w, EntityId target, float hp, float armor) {
    if (!ent_valid(w, target)) return;
    Health *h = &w->health[ENT_INDEX(target)];
    h->hp = fminf(h->max_hp, h->hp + hp);
    h->armor = fminf(h->max_armor, h->armor + armor);
}

void health_tick_dummy(World *w, EntityId e, float dt) {
    int i = (int)ENT_INDEX(e);
    Health *h = &w->health[i];
    if (w->flags[i] & EF_DEAD) {
        // Topple over, then stand back up with full health.
        w->anim[i] = approachf(w->anim[i], 1.0f, dt * 3.0f);
        h->respawn_timer -= dt;
        if (h->respawn_timer <= 0) {
            w->flags[i] &= ~(u32)EF_DEAD;
            h->hp = h->max_hp;
        }
        return;
    }
    w->anim[i] = approachf(w->anim[i], 0.0f, dt * 2.0f);
    if (h->hp < h->max_hp && w->time - h->last_damage_time > h->regen_delay) h->hp = fminf(h->max_hp, h->hp + h->max_hp * dt);
}
