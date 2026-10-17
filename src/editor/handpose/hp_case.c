// Simulated cases of the hand pose editor (docs/HANDPOSE.md "Cases").
#include "hp_case.h"
#include "hp_system.h"
#include "anim/anim_library.h"
#include "core/engine.h"
#include "core/log.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "gameplay/characters.h"
#include "gameplay/entities.h"
#include "gameplay/items.h"
#include "gameplay/weapons.h"
#include "physics/physics.h"
#include "player/hands.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static HpCaseState C = { .speed = 1.0f, .loop = true };
static struct {
    bool saved;
    vec3 pos;
    float yaw, pitch;
    int ci;
} S;

const HpCaseState *hp_case_state(void) { return &C; }

static HandPoseDB *db(void) { return g_engine.anim ? &g_engine.anim->handposes : NULL; }
static const HandPoseCase *cur(void) {
    HandPoseDB *d = db();
    int i = d ? handpose_case_find_hash(d, C.case_hash) : -1;
    return i >= 0 ? &d->c[i] : NULL;
}

static int local_slot(void) {
    Game *g = game();
    return g ? g->local_slot : -1;
}

const char *hp_case_item(void) {
    const HandPoseCase *c = C.active ? cur() : NULL;
    return c && (c->kind == HCK_ITEM || c->kind == HCK_RELOAD) ? c->item : "";
}

int hp_case_hold_state(void) {
    const HandPoseCase *c = C.active ? cur() : NULL;
    return c && c->hold_state[0] ? holdpoint_state_from_name(c->hold_state) : -1;
}

static void set_player(World *w, int slot, vec3 pos, float yaw) {
    Player *p = &w->players[slot];
    p->sim.move.pos = pos;
    p->sim.move.vel = v3(0, 0, 0);
    p->sim.move.yaw = yaw;
    if (p->character) phys_character_set_pos(p->character, pos);
}

// The ledge grab, as the movement code sets it when it starts hanging (src/player/movement.c).
static void hang_from(World *w, int slot, vec3 wall_point, vec3 normal, float top_y) {
    PlayerMove *m = &w->players[slot].sim.move;
    m->mode = MOVE_HANG;
    m->hang_pos = m->pos;
    m->hang_normal = normal;
    m->ledge_point = v3(wall_point.x, top_y, wall_point.z);
    m->hang_time = 1.0f;
    m->vel = v3(0, 0, 0);
    vec3 tangent = v3_norm(v3_cross(v3(0, 1, 0), normal));
    vec3 edge = v3_madd(v3(wall_point.x, top_y + 0.02f, wall_point.z), normal, -0.06f);
    m->hand_anchor[0] = v3_madd(edge, tangent, -0.24f);
    m->hand_anchor[1] = v3_madd(edge, tangent, 0.24f);
    vec3 face_right = v3_cross(v3_neg(normal), v3(0, 1, 0));
    if (v3_dot(v3_sub(m->hand_anchor[0], edge), face_right) > 0) {
        vec3 t = m->hand_anchor[0];
        m->hand_anchor[0] = m->hand_anchor[1];
        m->hand_anchor[1] = t;
    }
    m->hand_anchor_valid = 1;
}

static EntityId case_spawn_prop(World *w, const HandPoseCase *c, vec3 feet, float yaw) {
    vec3 fwd = v3(-sinf(yaw), 0, -cosf(yaw)), right = v3(cosf(yaw), 0, -sinf(yaw));
    float d = c->distance > 0.1f ? c->distance : 0.75f;
    const char *p = c->prop;
    if (!strcmp(p, "door")) {
        // The knob in front of the right hand at the use distance (knob: 0.4 m from the panel centre).
        vec3 base = v3_add(v3_madd(feet, fwd, d), v3_scale(right, -0.25f));
        EntityId e = spawn_door(w, base, yaw, 0.98f, 2.1f, false, 0, 0xFF5A7A9A);
        if (e && strstr(c->bind, "/open")) {
            // The open variant: the panel swung away (closing pushes it).
            int i = (int)ENT_INDEX(e);
            w->door[i].progress = w->door[i].target = 1.0f;
            w->flags[i] |= EF_OPEN;
        }
        return e;
    }
    if (!strcmp(p, "lever")) return spawn_lever(w, v3_madd(feet, fwd, d), yaw, "");
    if (!strcmp(p, "container")) return spawn_container(w, v3_madd(feet, fwd, d + 0.2f), yaw);
    if (!strcmp(p, "switch")) return spawn_switch(w, v3_add(v3_madd(feet, fwd, d), v3(0, 1.35f, 0)), yaw, 0);
    if (!strcmp(p, "bench")) return spawn_bench(w, v3_add(v3_madd(feet, fwd, d + 0.35f), v3(0, 0.9f, 0)), yaw);
    if (!strcmp(p, "pickup")) {
        u16 it = item_find(w->items, "medkit");
        return spawn_pickup(w, v3_add(v3_madd(feet, fwd, d), v3(0, 0.05f, 0)), it ? it : 1, 1, 0, 0);
    }
    if (!strcmp(p, "ledge")) {
        // A wall the player hangs from: top 2.05 m above the feet, its face a body radius away.
        float dw = 0.36f, top = feet.y + 2.05f;
        vec3 ctr = v3_add(v3_madd(feet, fwd, dw + 0.3f), v3(0, (top - feet.y) * 0.5f, 0));
        EntityId e = spawn_marker(w, ctr, quat_yaw(yaw), VIS_BOX, v3(1.2f, (top - feet.y) * 0.5f, 0.3f), 0xFF707070, false);
        hang_from(w, local_slot(), v3_madd(feet, fwd, dw), v3_neg(fwd), top);
        return e;
    }
    if (!strcmp(p, "ladder")) return spawn_marker(w, v3_add(v3_madd(feet, fwd, d), v3(0, 1.5f, 0)), quat_yaw(yaw), VIS_BOX, v3(0.25f, 1.5f, 0.03f), 0xFF406080, false);
    // Any other interactable (fuel pump, vehicle...): a stand-in box with the interaction volume.
    return spawn_marker(w, v3_add(v3_madd(feet, fwd, d + 0.2f), v3(0, 0.6f, 0)), quat_yaw(yaw), VIS_BOX, v3(0.3f, 0.6f, 0.25f), 0xFF505050, false);
}

bool hp_case_enter(World *w, int ci, char *why, int why_size) {
    HandPoseDB *d = db();
    int slot = local_slot();
    if (!d || ci < 0 || ci >= d->case_count || !w || slot < 0 || !w->players[slot].active) { snprintf(why, (size_t)why_size, "needs a local host player"); return false; }
    if (!w->authority) { snprintf(why, (size_t)why_size, "simulated cases need the host"); return false; }
    hp_case_leave(w);
    const HandPoseCase *c = &d->c[ci];
    Player *p = &w->players[slot];
    if (p->sim.move.mode != MOVE_WALK) { snprintf(why, (size_t)why_size, "stand on the ground first (mode %d)", p->sim.move.mode); return false; }
    S.saved = true;
    S.pos = p->sim.move.pos;
    S.yaw = p->sim.move.yaw;
    S.pitch = p->sim.move.pitch;
    memset(&C, 0, sizeof(C));
    C.speed = 1.0f;
    C.loop = true;
    C.active = true;
    C.case_hash = c->hash;
    C.kind = c->kind;
    C.t = c->contact;   // open on the contact frame
    vec3 feet = p->sim.move.pos;
    float yaw = p->sim.move.yaw;
    p->sim.move.pitch = -0.15f;
    switch (c->kind) {
    case HCK_INTERACT:
    case HCK_MANTLE:
    case HCK_STUB:
        C.prop = case_spawn_prop(w, c, feet, yaw);
        break;
    case HCK_VEHICLE: {
        const char *vd = c->vehicle[0] ? c->vehicle : "car";
        vec3 fwd = v3(-sinf(yaw), 0, -cosf(yaw));
        C.vehicle = vehicle_spawn(w, vd, v3_add(v3_madd(feet, fwd, 3.0f), v3(0, 0.3f, 0)), yaw);
        if (!C.vehicle || !vehicles_editor_seat(w, C.vehicle, slot, c->seat)) {
            snprintf(why, (size_t)why_size, "cannot seat the player in a %s (seat %d)", vd, c->seat);
            hp_case_leave(w);
            return false;
        }
        break;
    }
    case HCK_RELOAD:
        C.view_only = true;
        C.t = 0;
        break;
    default: break;
    }
    snprintf(C.status, sizeof(C.status), "case %s: %s", c->id, handpose_case_kind_name(c->kind));
    log_info("hpe: entered case %s (%s, bind %s)", c->id, handpose_case_kind_name(c->kind), c->bind);
    return true;
}

void hp_case_leave(World *w) {
    if (!C.active) return;
    int slot = local_slot();
    hands_case_drive(NULL);
    handpose_case_preview(0, 0);
    if (w && slot >= 0 && w->players[slot].active) {
        Player *p = &w->players[slot];
        if (p->sim.move.mode == MOVE_SEATED) vehicles_editor_unseat(w, slot);
        if (p->sim.move.mode == MOVE_HANG) { p->sim.move.mode = MOVE_WALK; p->sim.move.hand_anchor_valid = 0; }
        if (S.saved) { set_player(w, slot, S.pos, S.yaw); p->sim.move.pitch = S.pitch; }
    }
    if (w && ent_valid(w, C.prop)) ent_destroy(w, C.prop);
    if (w && ent_valid(w, C.vehicle)) ent_destroy(w, C.vehicle);
    S.saved = false;
    memset(&C, 0, sizeof(C));
    C.speed = 1.0f;
    C.loop = true;
}

void hp_case_set_time(float t) {
    const HandPoseCase *c = cur();
    C.t = c ? clampf(t, 0.0f, c->duration) : 0.0f;
}
void hp_case_play(bool on) { C.playing = on; }
void hp_case_set_loop(bool on) { C.loop = on; }
void hp_case_set_speed(float s) { C.speed = clampf(s, 0.05f, 4.0f); }

void hp_case_frame(World *w, float dt) {
    if (!C.active) return;
    const HandPoseCase *c = cur();
    int slot = local_slot();
    if (!c || !w || slot < 0) return;
    Player *p = &w->players[slot];
    if (C.kind == HCK_RELOAD) {
        // The clip owns the hands: the timeline follows the real reload (play starts one).
        const WeaponDef *wd = weapon_equipped_def(w, slot);
        WeaponState *ws = &p->sim.wpn;
        if (C.playing && wd && !ws->reloading) {
            ws->reloading = 1;
            ws->reload_t = wd->reload_time;
        }
        if (wd && ws->reloading) C.t = (wd->reload_time - ws->reload_t) / fmaxf(wd->reload_time, 1e-3f) * c->duration;
        else if (C.playing) { C.playing = C.loop; C.t = 0; }
        return;
    }
    if (C.playing) {
        C.t += dt * C.speed;
        if (C.t > c->duration) {
            if (C.loop) C.t = fmodf(C.t, fmaxf(c->duration, 1e-3f));
            else { C.t = c->duration; C.playing = false; }
        }
    }
    switch (C.kind) {
    case HCK_INTERACT: {
        HandsCaseDrive d = { ent_valid(w, C.prop), C.prop, C.case_hash, C.t };
        hands_case_drive(&d);
        handpose_case_preview(C.case_hash, C.t);
        break;
    }
    case HCK_VEHICLE:
    case HCK_MANTLE:
        handpose_case_preview(C.case_hash, C.t);
        break;
    default: break;
    }
}

bool hp_case_rig_frames(World *w, HpRigCtx *ctx) {
    const HandPoseCase *c = C.active ? cur() : NULL;
    int slot = local_slot();
    if (!c || !w || slot < 0) return false;
    Player *p = &w->players[slot];
    ctx->is_case = true;
    ctx->anchor_applied = !handpose_case_anchor_in_target(c);
    switch (c->kind) {
    case HCK_INTERACT: {
        if (!ent_valid(w, C.prop)) return false;
        for (int h = 0; h < 2; h++) {
            vec3 tp;
            quat tr;
            if (!handpose_interact_target(w, C.prop, slot, h, &tp, &tr)) return false;
            ctx->hand_base[h] = xf_make(tp, tr);
        }
        ctx->root_parent = ctx->hand_base[1];
        return true;
    }
    case HCK_VEHICLE: {
        vec3 hp[2];
        quat hr[2];
        bool use[2];
        bool driver = !g_vehicle_pose.seat_driver || g_vehicle_pose.seat_driver(w, slot);
        if (!g_vehicle_pose.seat_hands) return false;
        if (!g_vehicle_pose.seat_hands(w, slot, hp, hr, use)) {
            // Passenger without keys yet: the lap frame for both hands.
            Xform sf;
            if (driver || !vehicles_seat_frame(w, slot, &sf)) return false;
            hp[0] = hp[1] = sf.t;
            hr[0] = hr[1] = sf.r;
        }
        for (int h = 0; h < 2; h++) ctx->hand_base[h] = xf_make(hp[h], hr[h]);
        if (driver) {
            if (!vehicles_steer_base(w, slot, &ctx->root_parent)) return false;
        } else if (!vehicles_seat_frame(w, slot, &ctx->root_parent)) return false;
        return true;
    }
    case HCK_MANTLE: {
        const PlayerMove *m = &p->sim.move;
        if (!m->hand_anchor_valid) return false;
        quat r = quat_yaw(m->yaw);
        CharacterInst *ch = char_get(w, p->ent);
        if (ch) r = quat_yaw(ch->rt.proc.render_body_yaw);
        for (int h = 0; h < 2; h++) ctx->hand_base[h] = xf_make(m->hand_anchor[h], r);
        ctx->root_parent = xf_make(v3_scale(v3_add(m->hand_anchor[0], m->hand_anchor[1]), 0.5f), r);
        return true;
    }
    case HCK_STUB: {
        if (!ent_valid(w, C.prop)) return false;
        int i = (int)ENT_INDEX(C.prop);
        Xform b = xf_make(v3_add(w->xf[i].pos, v3(0, -0.1f, 0)), quat_mul(w->xf[i].rot, quat_yaw(DG_PI)));
        ctx->hand_base[0] = ctx->hand_base[1] = ctx->root_parent = b;
        return true;
    }
    default: return false;
    }
}
