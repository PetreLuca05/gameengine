#include "gameplay/phone/phone.h"
#include "gameplay/attachments.h"
#include "game.h"
#include "ui/mainmenu/mainmenu.h"
#include "game_internal.h"
#include "game_water.h"
#include "editor/handpose/hp_system.h"
#include "editor/handpose/hp_editor.h"
#include "scene.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "input/input.h"
#include "world/level.h"
#include "world/chunks.h"
#include "gameplay/entities.h"
#include "gameplay/health.h"
#include "gameplay/revive/revive.h"
#include "game_revive.h"
#include "gameplay/weapons.h"
#include "gameplay/ballistics/bal_world.h"
#include "gameplay/ballistics/bal_fx.h"
#include "gameplay/characters.h"
#include "gameplay/aim.h"
#include "anim/anim_library.h"
#include "assets/anim_cache.h"
#include "ui/anim_debug.h"
#include "gameplay/flesh_fx.h"
#include "gameplay/zombies/horde.h"
#include "world/openworld.h"
#include "gameplay/effects.h"
#include "player/hands.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "net/net.h"
#include "net/spectate.h"
#include "player/spectator.h"
#include "audio/audio.h"
#include "ui/console.h"
#include "ui/crosshair.h"
#include "ui/recoil_debug.h"
#include "ui/hud/hud_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define G (g_engine.game)

Game *game(void) { return G; }
World *game_world(void) { return G ? G->world : NULL; }
bool game_is_host(void) { return G && G->mode == GM_HOST; }

Player *game_local_player(void) {
    if (!G || !G->world || G->local_slot < 0) return NULL;
    Player *p = &G->world->players[G->local_slot];
    return p->active ? p : NULL;
}

int game_view_slot(void) {
    int s = spectator_view_slot();
    return s >= 0 ? s : (G ? G->local_slot : -1);
}

Player *game_view_player(void) {
    int s = spectator_view_slot();
    if (s < 0) return game_local_player();
    Player *p = &G->world->players[s];
    return p->active ? p : NULL;
}

const UserCmd *game_view_cmd(void) {
    const UserCmd *c = spectator_view_cmd();
    return c ? c : &G->last_cmd;
}

// ------------------------------------------------------------------ players

static vec3 pick_spawn(World *w, u32 *index, float *yaw) {
    LevelStatic *ls = &w->level;
    if (w->streaming && !w->openworld) {
        *yaw = 0;
        return chunks_spawn_point(w);
    }
    int n = DG_MAX(1, ls->spawn_count);
    for (int tries = 0; tries < n; tries++) {
        SpawnPoint *s = &ls->spawns[(*index + (u32)tries) % (u32)n];
        if (!phys_capsule_overlap(w->phys, v3_add(s->pos, v3(0, 0.05f, 0)), PLAYER_RADIUS, PLAYER_HEIGHT, PL_MASK_ALL, PHYS_INVALID_BODY)) {
            *index = (*index + (u32)tries + 1) % (u32)n;
            *yaw = s->yaw;
            return s->pos;
        }
    }
    SpawnPoint *s = &ls->spawns[*index % (u32)n];
    *index = (*index + 1) % (u32)n;
    *yaw = s->yaw;
    return s->pos;
}

static CharacterParams player_char_params(void) {
    CharacterParams cp;
    cp.radius = PLAYER_RADIUS;
    cp.height_stand = PLAYER_HEIGHT;
    cp.height_crouch = PLAYER_CROUCH_HEIGHT;
    cp.max_slope_deg = player_tuning()->max_slope->f;
    cp.mass = 80.0f;
    cp.max_strength = 400.0f;
    return cp;
}

void game_create_player_character(World *w, int slot, vec3 feet) {
    Player *p = &w->players[slot];
    if (p->character) phys_character_destroy(w->phys, p->character);
    CharacterParams cp = player_char_params();
    p->character = phys_character_create(w->phys, &cp, feet, p->ent);
}

void game_respawn_player(World *w, int slot) {
    Player *p = &w->players[slot];
    float yaw = 0;
    vec3 pos = pick_spawn(w, &p->spawn_index, &yaw);
    game_respawn_player_at(w, slot, pos, yaw, 100.0f);
}

// Respawn at a given place (spawn points, a fabricator chamber: docs/REVIVE.md).
void game_respawn_player_at(World *w, int slot, vec3 pos, float yaw, float hp) {
    Player *p = &w->players[slot];
    player_move_reset(&p->sim, pos, yaw);
    memset(&p->sim.wpn, 0, sizeof(p->sim.wpn));
    p->sim.wpn.swing_t = -1;
    p->sim.wpn.use_t = -1;
    p->sim.wpn.slot = p->last_cmd.slot;
    if (p->character) {
        phys_character_set_crouched(w->phys, p->character, false);
        phys_character_set_pos(p->character, pos);
        phys_character_set_vel(p->character, v3(0, 0, 0));
    }
    int i = (int)ENT_INDEX(p->ent);
    health_init(&w->health[i], 100.0f, 0.0f);
    w->health[i].hp = fminf(hp, w->health[i].max_hp);
    w->flags[i] &= ~(u32)(EF_DEAD | EF_HIDDEN);   // hidden: joined dead (sv_join_dead)
    revive_sv_on_respawn(w, slot);
    ent_set_pos(w, p->ent, pos, quat_yaw(yaw), true);
    spectate_on_respawn(slot);
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_RESPAWN;
    ev.target = (u8)slot;
    ev.except = EVENT_ALL;
    ev.ent = p->ent;
    ev.pos = pos;
    ev.value = yaw;
    ev_push(w, &ev);
}

int game_spawn_player(World *w, int slot, const char *name, bool local, int peer) {
    Player *p = &w->players[slot];
    memset(p, 0, sizeof(*p));
    p->active = true;
    p->local = local;
    p->peer = peer;
    dg_strcpy(p->name, sizeof(p->name), name);
    p->ent = ent_spawn(w, ET_PLAYER);
    int i = (int)ENT_INDEX(p->ent);
    w->player_slot[i] = (u8)slot;
    w->vis[i].shape = VIS_CAPSULE;
    w->vis[i].half = v3(PLAYER_RADIUS, PLAYER_HEIGHT * 0.5f - PLAYER_RADIUS, PLAYER_RADIUS);
    w->vis[i].color = rgba8(0.25f, 0.4f + 0.08f * (float)slot, 0.3f, 1);
    w->comps[i] |= C_VISUAL | C_HEALTH | C_PLAYER;
    dg_strcpy(w->names[i], sizeof(w->names[0]), name);
    entity_setup(w, p->ent);
    p->spawn_index = (u32)slot;
    game_create_player_character(w, slot, v3(0, 1, 0));
    game_respawn_player(w, slot);
    log_info("player %d '%s' spawned%s", slot, name, local ? " (local)" : "");
    return slot;
}

void game_remove_player(World *w, int slot) {
    Player *p = &w->players[slot];
    if (!p->active) return;
    if (p->character) phys_character_destroy(w->phys, p->character);
    ent_destroy(w, p->ent);
    log_info("player %d '%s' left", slot, p->name);
    memset(p, 0, sizeof(*p));
    p->peer = -1;
}

// ------------------------------------------------------------------ simulation

// Validates that the user can interact with target right now (host side).
static bool interact_valid(World *w, int slot, EntityId target) {
    if (!ent_valid(w, target)) return false;
    int i = (int)ENT_INDEX(target);
    if (!(w->comps[i] & C_INTERACT) || !w->inter[i].def || !w->inter[i].enabled || (w->flags[i] & EF_HIDDEN)) return false;
    Player *p = &w->players[slot];
    vec3 eye = player_eye_pos(&p->sim.move);
    vec3 fp = ent_focus_point(w, target);
    float dist = v3_dist(eye, fp);
    if (dist > w->inter[i].range + 0.75f) return false; // slack for latency
    // Line of sight: nothing solid between the eye and the target (other than the target itself).
    RayHit rh;
    BodyId self = p->character ? phys_character_inner_body(p->character) : PHYS_INVALID_BODY;
    vec3 d = v3_sub(fp, eye);
    if (phys_raycast(w->phys, eye, v3_scale(d, 0.97f), PL_MASK_WORLD, self, &rh)) {
        if ((EntityId)rh.user != target && rh.distance < dist - 0.25f) return false;
    }
    const InteractDef *def = w->inter[i].def;
    if (def->can_use && !def->can_use(w, target, slot)) return false;
    return true;
}

static void interact_tick(World *w, int slot, const UserCmd *cmd, u16 pressed, float dt) {
    Player *p = &w->players[slot];
    if (p->sim.move.mode == MOVE_DEAD || p->sim.down.downed) return;
    EntityId target = (EntityId)cmd->use_target;
    bool use_down = (cmd->buttons & BTN_USE) != 0;
    // Timed actions (revive, extract DNA, insert into a fabricator) run in revive_sim_pre.
    bool timed = ent_valid(w, target) && (w->comps[ENT_INDEX(target)] & C_INTERACT) && w->inter[ENT_INDEX(target)].def && w->inter[ENT_INDEX(target)].def->action;
    if ((pressed & BTN_USE) && !timed && interact_valid(w, slot, target)) {
        const InteractDef *def = w->inter[ENT_INDEX(target)].def;
        if (def->hold_time > 0 && !(def == interact_def_for(ET_CONTAINER) && w->cont[ENT_INDEX(target)].opened)) {
            p->hold_target = target;
            p->hold_time = 0;
            p->hold_progress = 0;
        } else {
            def->on_use(w, target, slot);
            GameEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EV_USE_ANIM;
            ev.target = EVENT_ALL;
            ev.except = (u8)slot;
            ev.ent = p->ent;
            ev.ent2 = target;
            ev_push(w, &ev);
        }
    }
    if (p->hold_target) {
        bool still = use_down && target == p->hold_target && ent_valid(w, target);
        const InteractDef *def = ent_valid(w, p->hold_target) ? w->inter[ENT_INDEX(p->hold_target)].def : NULL;
        if (!still || !def) {
            if (def && def->on_hold) def->on_hold(w, p->hold_target, slot, -1.0f);
            p->hold_target = ENT_NONE;
            p->hold_progress = 0;
        } else {
            p->hold_time += dt;
            p->hold_progress = saturate(p->hold_time / def->hold_time);
            if (def->on_hold) def->on_hold(w, p->hold_target, slot, p->hold_progress);
            if (p->hold_progress >= 1.0f) {
                def->on_use(w, p->hold_target, slot);
                p->hold_target = ENT_NONE;
                p->hold_progress = 0;
            }
        }
    }
    // Close the container window when walking away.
    if (p->open_container && ent_valid(w, p->open_container)) {
        if (v3_dist(p->sim.move.pos, w->xf[ENT_INDEX(p->open_container)].pos) > 3.5f) {
            GameEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EV_CONTAINER_CLOSE;
            ev.target = (u8)slot;
            ev.except = EVENT_ALL;
            ev_push(w, &ev);
            p->open_container = ENT_NONE;
        }
    }
}

// One tick of a player's simulation. Shared by the host and client-side prediction.
void game_sim_player(World *w, int slot, const UserCmd *cmd, float dt, bool first) {
    Player *p = &w->players[slot];
    if (!p->active || !p->character) return;
    u16 pressed = (u16)(cmd->buttons & ~p->sim.last_buttons);
    // Noclip only when cheats are allowed.
    UserCmd c = *cmd;
    Cvar *cheats = cvar_find("sv_cheats");
    if (!cheats || !cheats->i) c.buttons &= (u16)~BTN_NOCLIP;
    revive_sim_pre(w, slot, &c, pressed, dt, first);   // timed actions, downed crawl, forced idle (docs/REVIVE.md)
    pressed = (u16)(c.buttons & ~p->sim.last_buttons);
    vehicles_player_cmd(w, slot, &c, pressed, first);   // enter / exit / seats / driver input (host)
    // A driver's right click holds the gun out at the hip: no aim-down-sights from it.
    if (p->sim.move.mode == MOVE_SEATED && p->sim.move.veh_onehand) c.buttons &= (u16)~BTN_AIM;
    if (p->sim.move.mode == MOVE_SWIM) c.buttons &= (u16)~BTN_AIM;
    player_move_tick(w, slot, &p->sim, p->character, &c, dt, first);
    WeaponOutput wo;
    weapons_tick(w, slot, &p->sim, &c, pressed, dt, first, &wo);
    if (wo.fired || wo.swung) chars_notify_attack(w, slot, wo.swung, first);
    spectator_note_kick(slot, c.seq, wo.view_kick);   // a watched player's camera carries its kick (spectator.c)
    if (first && p->local && G) {
        // Aim recoil + recovery rotate the real view (the next command carries it).
        G->view.yaw += wo.view_kick.x;
        G->view.pitch = clampf(G->view.pitch + wo.view_kick.y, -88.0f * DG_DEG2RAD, 88.0f * DG_DEG2RAD);
        if (wo.mode_changed) {
            char t[64];
            snprintf(t, sizeof(t), "Fire mode: %s", fire_mode_name((FireMode)p->sim.wpn.fire_mode));
            game_add_hint(t);
        }
    }
    if (w->authority) interact_tick(w, slot, &c, pressed, dt);
    p->last_cmd = c;

    // Mirror onto the player entity (replicated to others).
    int i = (int)ENT_INDEX(p->ent);
    const PlayerMove *m = &p->sim.move;
    w->xf[i].pos = m->pos;
    w->xf[i].rot = quat_yaw(m->yaw);
    w->anim[i] = m->pitch;
    u32 f = w->flags[i] & (EF_DEAD | EF_HIDDEN | EF_DOWNED);
    if (m->crouched) f |= EF_CROUCHED;
    if (m->sprinting) f |= EF_SPRINT;
    if (m->mode == MOVE_HANG) f |= EF_HANG;
    if (m->mode == MOVE_SEATED) f |= EF_SEATED;
    if (m->mode == MOVE_SWIM) f |= m->water == WATER_UNDER ? (EF_SWIM | EF_UNDERWATER) : EF_SWIM;
    else if (m->water == WATER_WADE && m->water_depth > 0.3f) f |= EF_WADE;
    if (m->mode == MOVE_MANTLE || m->mode == MOVE_VAULT) f |= EF_MANTLE;
    if (p->sim.wpn.reloading) f |= EF_RELOADING;
    if (c.buttons & BTN_AIM) f |= EF_ADS;
    if (p->sim.wpn.recoil_kick > 0.8f) f |= EF_FIRING;
    if ((c.buttons & BTN_FLASHLIGHT) && m->mode != MOVE_DEAD) f |= EF_FLASHLIGHT;
    w->flags[i] = f;
    const InvSlot *eq = weapon_equipped(w, slot);
    w->item[i] = eq ? eq->item : 0;
    if (eq) memcpy(w->attach[i], eq->att, sizeof(w->attach[i]));
    else memset(w->attach[i], 0, sizeof(w->attach[i]));
    u16 shown = revive_held_override(w, slot);   // the syringe / the vial during a timed action
    if (shown) { w->item[i] = shown; memset(w->attach[i], 0, sizeof(w->attach[i])); }
}

static void host_tick(World *w, const UserCmd *local_cmd) {
    world_store_prev(w);
    // Frozen world (the hand pose editor): only the players (and their animation) run.
    bool frozen = game_world_frozen();
    daynight_host_tick(w);
    weather_host_tick(w);
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (!p->active) continue;
        if (p->local) {
            game_sim_player(w, s, local_cmd, SIM_DT, true);
            p->last_processed_seq = local_cmd->seq;   // its local data acks it like a remote player's (spectators)
        } else {
            // Remote players simulate exactly once per received command (never on a guess), so
            // the host runs the same ticks the client predicted: movement, fire timing, recoil
            // and ammo stay in lockstep. A starved queue waits; a backed-up one catches up.
            int n = p->queue_count == 0 ? 0 : (p->queue_count > 3 ? 2 : 1);
            if (p->queue_count > 16) n = p->queue_count - 8;   // long stall: bound the latency
            for (int k = 0; k < n; k++) {
                UserCmd cmd = p->queue[0];
                memmove(p->queue, p->queue + 1, sizeof(UserCmd) * (size_t)(p->queue_count - 1));
                p->queue_count--;
                game_sim_player(w, s, &cmd, SIM_DT, true);
                if (cmd.seq > p->last_processed_seq) p->last_processed_seq = cmd.seq;
            }
        }
        // Death / respawn
        int i = (int)ENT_INDEX(p->ent);
        if (w->flags[i] & EF_DEAD) {
            if (p->respawn_timer > 0) p->respawn_timer -= SIM_DT;
            if (spectate_respawn_ready(w, s) && revive_sv_timer_respawn_allowed(w, s)) game_respawn_player(w, s);   // sv_respawn_time / sv_respawn_mode
        }
        // Fell out of the world.
        if (p->sim.move.pos.y < -60.0f && p->sim.move.mode != MOVE_NOCLIP) game_respawn_player(w, s);
    }
    spectate_host_tick(w);   // observers: targets still legal, auto-advance
    if (!frozen) {
        entities_tick(w, SIM_DT);
        revive_host_tick(w);   // corpses, fabricators, DNA samples (docs/REVIVE.md)
        projectiles_tick(w, SIM_DT);
        horde_host_tick(w, SIM_DT);   // horde zombies: tiers, steering, mover, attacks (before their animation)
    }
    chars_host_tick(w, SIM_DT);   // sim-only animation -> per-part hitboxes (before the lag-comp record)
    if (!frozen) bal_world_tick(w, SIM_DT);   // bullets in flight against this tick's (and rewound) hitboxes
    if (!frozen) horde_post_tick(w);
    if (w->chunks) chunks_update(w, SIM_DT);
    if (!frozen) {
        openworld_tick(w, SIM_DT);   // open-world maps: deep water, POI zombie spawner
        vehicles_host_tick(w);
        water_tick(w);   // swell wind, buoyancy impulses on props (before the step)
        phys_step(w->phys, SIM_DT);
        vehicles_host_post(w);
        vehicles_test_tick(w);
    }
    // Props moved by physics: pull their new transforms.
    ENT_FOREACH(w, i) {
        if (w->type[i] == ET_PROP) ent_sync_from_body(w, ENT_MAKE(i, w->gen[i]));
    }
    world_record_lagcomp(w);
    w->tick++;
    w->time += SIM_DT;
}

// Delivers this tick's events: locally for the host player, over the network for others.
static void dispatch_events(World *w) {
    for (int k = 0; k < w->event_count; k++) {
        const GameEvent *ev = &w->events[k];
        // The same routing as remote players get (spectators see their target's HUD feedback).
        bool to_local = G->local_slot >= 0 && spec_event_delivers(ev->target, ev->except, ev->type, G->local_slot, spectate_event_target(G->local_slot));
        if (to_local) game_handle_event(ev);
    }
    net_host_send_events(w);
    net_host_send_anim_events(w);
    w->event_count = 0;
    w->anim_event_count = 0;
}

// ------------------------------------------------------------------ commands

// ------------------------------------------------------------------ weapon feel (client presentation)

// Shot hook: client-only cosmetic layers driven by every locally predicted shot.
static void on_shot(const ShotEvent *ev, void *user) {
    (void)user;
    if (!G || ev->slot != game_view_slot()) return;   // the viewed player's shots (local, or spectated)
    // The gun-driven fire animation (docs/FIRE_ANIM.md): the gun, hands, body, camera and punches.
    // The casing leaves when the slide / bolt comes back (game_fire_tick), not on the shot.
    game_fire_on_shot(ev);
    // Bolt / pump cycle after the shot (if rounds are left).
    if (ev->def->bolt_action && ev->mag_left > 0) G->bolt_timer = fmaxf(0.25f, 60.0f / fmaxf(1.0f, ev->def->recoil.rpm) * 0.35f);
}

static void weapon_feel_tick(void) {
    Player *p = game_view_player();
    int vs = game_view_slot();
    const WeaponDef *d = p ? weapon_equipped_def(G->world, vs) : NULL;
    G->spread_prev = G->spread_cur;
    G->spread_cur = p ? weapon_current_spread(G->world, vs) : 0;
    if (G->bolt_timer > 0) {
        G->bolt_timer -= SIM_DT;
        if (G->bolt_timer <= 0 && p && d && d->bolt_action) fx_sound("bolt_cycle", player_eye_pos(&p->sim.move), 0.7f);   // the casing: game_fire_tick
    }
    // wpn_magdump: stop once the magazine is empty (or after 20 s).
    if (G->magdump_ticks > 0) {
        G->magdump_ticks++;
        int mag = p ? p->sim.wpn.mag : 0;
        if (G->magdump_ticks > 2 && mag < G->magdump_last_mag) {
            if (!G->magdump_shots) G->magdump_first_tick = G->magdump_ticks;
            G->magdump_last_tick = G->magdump_ticks;
            G->magdump_shots += G->magdump_last_mag - mag;
        }
        G->magdump_last_mag = mag;
        bool empty = p && mag <= 0 && G->magdump_ticks > 60;
        if (empty || G->magdump_ticks > 60 * 20 || !p) {
            const WeaponDebugStats *st = weapons_debug_stats();
            float secs = (float)(G->magdump_last_tick - G->magdump_first_tick) * SIM_DT;
            float rpm = (G->magdump_shots > 1 && secs > 0) ? (float)(G->magdump_shots - 1) / secs * 60.0f : 0.0f;
            log_info("magdump: %d rounds, mode %s, measured %.0f rpm, view climbed %.2f deg | %u bullets checked, %u outside the cone, "
                     "max offset %.0f%% of the cone radius -> %s",
                     G->magdump_shots, p ? fire_mode_name((FireMode)p->sim.wpn.fire_mode) : "?", rpm, p ? p->sim.wpn.rc.acc_pitch : 0.0f,
                     st->shots, st->outside, st->max_ratio * 100.0f, st->outside == 0 ? "PASS" : "FAIL");
            G->magdump_ticks = 0;
        }
    }
}

// wpn_magdump drives the trigger: held for auto, clicked for single / burst.
static void magdump_cmd(UserCmd *c) {
    if (G->magdump_ads) c->buttons |= BTN_AIM;
    if (G->magdump_ticks < 40) return;   // equip + aim settle
    Player *p = game_local_player();
    FireMode m = p ? (FireMode)p->sim.wpn.fire_mode : FM_AUTO;
    bool held = m == FM_AUTO || ((G->magdump_ticks / 3) & 1);
    if (held) c->buttons |= BTN_ATTACK;
}

static i8 axis_to_i8(float v) { return (i8)DG_CLAMP((int)lrintf(v * 127.0f), -127, 127); }

static UserCmd build_cmd(void) {
    UserCmd c;
    memset(&c, 0, sizeof(c));
    c.seq = ++G->client_seq;
    c.yaw = G->view.yaw;
    c.pitch = G->view.pitch;
    bool can_play = input_game_focus() && !G->menu_open && !G->inventory_open && !G->container_open && !G->bench_open && !console_is_open();
    // Scripted "+action" commands work even without window focus (automated tests).
    InputFrame f = input_frame();
    c.move_x = axis_to_i8(f.move.x);
    c.move_y = axis_to_i8(f.move.y);
    if (input_down(ACT_JUMP)) c.buttons |= BTN_JUMP;
    if (input_down(ACT_CROUCH)) c.buttons |= BTN_CROUCH;
    if (input_down(ACT_SPRINT)) c.buttons |= BTN_SPRINT;
    if (input_down(ACT_WALK)) c.buttons |= BTN_WALK;
    if (input_down(ACT_USE)) c.buttons |= BTN_USE;
    if (input_down(ACT_ATTACK) && (can_play || !input_game_focus())) c.buttons |= BTN_ATTACK;
    if (game_fire_loop_trigger()) c.buttons |= BTN_ATTACK;   // fire tuner loops (docs/FIRE_ANIM.md)
    if (input_down(ACT_AIM)) c.buttons |= BTN_AIM;
    if (input_down(ACT_RELOAD)) c.buttons |= BTN_RELOAD;
    // Edge presses shorter than a tick still register.
    if (input_consume_press(ACT_JUMP)) c.buttons |= BTN_JUMP;
    if (input_consume_press(ACT_USE)) c.buttons |= BTN_USE;
    if (input_consume_press(ACT_ATTACK)) c.buttons |= BTN_ATTACK;
    if (input_consume_press(ACT_RELOAD)) c.buttons |= BTN_RELOAD;
    if (input_consume_press(ACT_CROUCH)) c.buttons |= BTN_CROUCH;
    if (input_consume_press(ACT_FIREMODE)) c.buttons |= BTN_FIREMODE;
    if (G->flashlight) c.buttons |= BTN_FLASHLIGHT;
    if (G->phone_out) c.buttons |= BTN_PHONE;
    vehicles_fill_cmd(&c);
    if (G->noclip_request) { c.buttons |= BTN_NOCLIP; G->noclip_request = false; }
    wheels_filter_cmd(&c);   // E tap / hold on doors, the healing wheel's auto-use, open wheels (game_wheels.c)
    if (G->menu_open || G->inventory_open || G->container_open || G->bench_open || console_is_open() || phone_ui_blocks_game()) {
        c.move_x = c.move_y = 0;
        c.buttons &= (u16)(BTN_CROUCH | BTN_NOCLIP | BTN_FLASHLIGHT | BTN_PHONE | BTN_VEH_CAM);
    }
    if (spectator_controls_input()) {
        // Spectating: the keys drive the spectator controls, the own (dead / dev) player stands still.
        c.move_x = c.move_y = 0;
        c.buttons &= (u16)(BTN_CROUCH | BTN_NOCLIP);
    }
    if (G->magdump_ticks > 0) magdump_cmd(&c);
    water_test_cmd(&c);   // water_test swim drives the local player
    c.slot = G->hotbar_slot;
    c.use_target = G->focus;
    c.view_tick = net_client_view_tick256();
    phone_fill_cmd(&c);
    // Aim model (docs/AIM.md): where the muzzle is in the pose this player sees (the host checks it).
    {
        Player *lp = game_local_player();
        vec3 mc;
        if (lp && chars_muzzle_cam(G->world, lp->ent, &mc, NULL)) aim_claim_pack(mc, c.muzzle_cam);
        if (lp && chars_eye_cam(G->world, lp->ent, &mc, NULL)) aim_claim_pack(mc, c.eye_cam);
    }
    return c;
}

// Client-side reconciliation: restore the host's state for ack_seq and replay newer commands.
void game_cl_reconcile(const PlayerSim *server, u32 ack_seq) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!p || !p->character) return;
    if (ack_seq <= G->last_ack_seq && ack_seq != 0) return;
    G->last_ack_seq = ack_seq;
    vec3 before = p->sim.move.pos;
    WeaponState before_w = p->sim.wpn;
    p->sim = *server;
    phys_character_set_crouched(w->phys, p->character, server->move.crouched != 0);
    phys_character_set_pos(p->character, server->move.pos);
    phys_character_set_vel(p->character, server->move.vel);
    u32 first = ack_seq + 1;
    if (G->client_seq - ack_seq >= CMD_HISTORY) first = G->client_seq - CMD_HISTORY + 1;
    for (u32 s = first; s <= G->client_seq; s++) {
        const UserCmd *c = &G->cmd_history[s % CMD_HISTORY];
        if (c->seq != s) continue;
        if (s == G->client_seq) G->predicted_prev = p->sim.move;
        game_sim_player(w, G->local_slot, c, SIM_DT, false);
    }
    // Recoil is predicted too: the replay must land on the same aim state.
    const RecoilState *ra = &before_w.rc, *rb = &p->sim.wpn.rc;
    G->recoil_pred_error = fabsf(ra->acc_pitch - rb->acc_pitch) + fabsf(ra->acc_yaw - rb->acc_yaw) + fabsf(ra->bloom - rb->bloom) +
                           (ra->shot_index != rb->shot_index ? 1.0f : 0.0f) + (before_w.mag != p->sim.wpn.mag ? 1.0f : 0.0f);
    G->recoil_pred_error_max = fmaxf(G->recoil_pred_error_max, G->recoil_pred_error);
    if (G->recoil_pred_error > 0.01f)
        log_debug("recoil mispredict: ack %u seq %u: acc %.3f/%.3f -> %.3f/%.3f idx %u -> %u mag %d -> %d bloom %.2f -> %.2f seed %08x -> %08x rec %d -> %d cd %.3f -> %.3f",
                 ack_seq, G->client_seq, ra->acc_pitch, ra->acc_yaw, rb->acc_pitch, rb->acc_yaw, ra->shot_index, rb->shot_index, before_w.mag,
                 p->sim.wpn.mag, ra->bloom, rb->bloom, ra->mag_seed, rb->mag_seed, ra->recovering, rb->recovering, before_w.cooldown, p->sim.wpn.cooldown);
    vec3 after = p->sim.move.pos;
    float err = v3_dist(before, after);
    G->prediction_error = err;
    if (err < 1.0f) G->error_offset = v3_add(G->error_offset, v3_sub(before, after));
    else G->error_offset = v3(0, 0, 0);
}

static void tick(void) {
    Player *vp = game_view_player();
    G->ads_prev = vp ? vp->sim.wpn.ads_t : 0.0f;
    UserCmd cmd = build_cmd();
    G->last_cmd = cmd;
    World *w = G->world;
    if (G->mode == GM_HOST) {
        // The host's own commands go to its spectators like a remote player's (docs/SPECTATE.md).
        spectate_host_cmds(w, G->local_slot, &cmd, 1, platform_wall_ms());
        host_tick(w, &cmd);
        dispatch_events(w);
        net_host_after_tick(w);
        spectator_tick(w);
    } else if (G->mode == GM_CLIENT) {
        G->cmd_history[cmd.seq % CMD_HISTORY] = cmd;
        Player *p = game_local_player();
        if (p && p->character) {
            G->predicted_prev = p->sim.move;
            game_sim_player(w, G->local_slot, &cmd, SIM_DT, true);
            chars_local_predict(w, G->local_slot, true);
        }
        // Sent after the prediction: the command carries its shot's predicted aim point (docs/AIM.md).
        G->cmd_history[cmd.seq % CMD_HISTORY].aim_dist = weapons_take_aim_claim();
        net_client_send_cmds(G->cmd_history, CMD_HISTORY, cmd.seq);
        spectator_tick(w);   // the watched player: relayed commands through the same path
        g_chars_force_phys_step = vehicles_client_tick(w, &cmd);
        chars_client_tick(w, net_client_render_tick(), SIM_DT);
        bal_world_tick(w, SIM_DT);   // own predicted bullets + replays of the others' shots
        vehicles_client_post(w, cmd.seq);
        if (w->chunks) chunks_update(w, SIM_DT);
        water_tick(w);   // the swell's wind from the replicated weather
        w->tick++;
    }
    revive_frame(w);   // downed players are interactable (revive prompt)
    // Fire animation before the hold point is evaluated: the kick lands on the shot's own tick.
    game_fire_tick();
    // Render-side character work at tick rate (LOD-scaled post-pass, ragdolls).
    chars_present_tick(w, G->view.eye, G->view.fov > 1 ? G->view.fov : 75.0f, (float)render_height(), SIM_DT, game_view_slot());
    weapon_feel_tick();
    spectator_dump_tick(w, cmd.seq);
}

// ------------------------------------------------------------------ focus

static void update_focus(void) {
    World *w = G->world;
    Player *p = game_local_player();
    G->focus = ENT_NONE;
    G->focus_prompt[0] = 0;
    if (!p || p->sim.move.mode == MOVE_DEAD || p->sim.move.mode == MOVE_SEATED) return;   // seated: F / 1-4 drive the vehicle
    vec3 eye = G->view.eye;
    vec3 dir = dir_from_angles(G->view.yaw, G->view.pitch);
    float range = G->interact_range->f;
    BodyId self = p->character ? phys_character_inner_body(p->character) : PHYS_INVALID_BODY;
    RayHit rh;
    float wall = range;
    EntityId hit_ent = ENT_NONE;
    if (phys_raycast(w->phys, eye, v3_scale(dir, range), PL_MASK_WORLD, self, &rh)) {
        wall = rh.distance;
        hit_ent = (EntityId)rh.user;
    }
    EntityId best = ENT_NONE;
    float best_d = 1e9f;
    if (ent_valid(w, hit_ent) && (w->comps[ENT_INDEX(hit_ent)] & C_INTERACT) && w->inter[ENT_INDEX(hit_ent)].enabled) {
        best = hit_ent;
        best_d = wall;
    }
    // Small things without bodies (pickups): closest-approach test against their focus point.
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_INTERACT) || !w->inter[i].enabled || (w->flags[i] & EF_HIDDEN)) continue;
        if (w->body[i].body != PHYS_INVALID_BODY) continue;
        EntityId e = ENT_MAKE(i, w->gen[i]);
        vec3 fp = ent_focus_point(w, e);
        vec3 to = v3_sub(fp, eye);
        float t = v3_dot(to, dir);
        if (t < 0.1f || t > w->inter[i].range) continue;
        float miss = v3_len(v3_sub(to, v3_scale(dir, t)));
        float tol = fmaxf(0.12f, v3_len(w->vis[i].half) + 0.06f);
        if (miss > tol || t > wall + 0.15f) continue;
        if (t < best_d) { best_d = t; best = e; }
    }
    if (!best) return;
    int bi = (int)ENT_INDEX(best);
    if (best_d > w->inter[bi].range) return;
    G->focus = best;
    char prompt[80] = { 0 };
    if (w->inter[bi].def->on_focus) w->inter[bi].def->on_focus(w, best, G->local_slot, prompt, sizeof(prompt));
    char key[48];
    binding_name(input_get_binding(ACT_USE, 0), key, sizeof(key));
    if (w->flags[bi] & EF_LOCKED) snprintf(G->focus_prompt, sizeof(G->focus_prompt), "%s", prompt);
    else snprintf(G->focus_prompt, sizeof(G->focus_prompt), "[%s] %s", key, prompt);
}

// ------------------------------------------------------------------ frame

static void handle_hotbar(void) {
    Player *p = game_local_player();
    if (!p) return;
    if (phone_ui_blocks_game()) return;   // the gamepad drives the phone panel (phonenav held)
    vehicles_input();   // F / 1-4 / V / H while seated or looking at a vehicle
    // The built-in phone (docs/PHONE.md): X takes it out over whatever slot is selected and puts it
    // away again; choosing a slot puts it away too.
    if (input_consume_press(ACT_PHONE)) G->phone_out = !G->phone_out;
    for (int i = 0; i < 9; i++)
        if (input_consume_press((Action)(ACT_SLOT1 + i))) { G->hotbar_slot = (u8)i; G->phone_out = false; }
    // Phone camera mode: the wheel zooms instead of switching items.
    if (input_consume_press(ACT_NEXT_ITEM) && !phone_wheel(-1)) { G->hotbar_slot = (u8)((G->hotbar_slot + 1) % INV_HOTBAR); G->phone_out = false; }
    if (input_consume_press(ACT_PREV_ITEM) && !phone_wheel(1)) { G->hotbar_slot = (u8)((G->hotbar_slot + INV_HOTBAR - 1) % INV_HOTBAR); G->phone_out = false; }
    // Drop (G) is a tap of the throwables wheel key (game_wheels.c).
    if (input_consume_press(ACT_FLASHLIGHT)) {
        G->flashlight = !G->flashlight;
        audio_play_2d("click", 0.5f, SND_SFX);
    }
}

// ADS progress is simulated at the tick rate; the zoom and the sight presentation use it
// interpolated to the frame so aiming in and out is smooth at any frame rate.
float game_ads_vis(void) {
    Player *p = game_view_player();
    if (!p) return 0.0f;
    return saturate(lerpf(G->ads_prev, p->sim.wpn.ads_t, saturate(G->alpha)));
}

void game_frame(float dt) {
    handpose_system_poll();
    if (!G->world) return;
    game_fire_frame();   // fire_compare driver
    bool ui_blocking = G->menu_open || G->inventory_open || G->container_open || G->bench_open || console_is_open();
    InputFrame f = input_frame();
    // The door peek and an open radial wheel take the look (game_peek.c, game_wheels.c).
    bool peek_look = peek_frame(dt, f.look_delta, ui_blocking);
    bool wheel_look = wheels_frame(dt, ui_blocking);
    G->frame_look = ui_blocking || peek_look || wheel_look ? v2(0, 0) : f.look_delta;
    camera_apply_look(&G->view, G->frame_look);
    vehicles_view_frame(&G->view);
    if (!ui_blocking && !spectator_controls_input()) handle_hotbar();
    spectator_frame(dt);   // spectator controls, death camera -> spectate, free camera
    G->use_pressed_frame = !ui_blocking && input_peek_press(ACT_USE);
    // The interaction may finish inside this frame's tick (pickups vanish): remember its target
    // for the hand reach.
    if (G->use_pressed_frame) {
        G->use_focus = G->focus;
        G->use_focus_hands = -1;
        if (ent_valid(G->world, G->focus)) {
            const InteractDef *def = G->world->inter[ENT_INDEX(G->focus)].def;
            if (handpose_interact_has_target(def)) {
                G->use_focus_point = ent_focus_point(G->world, G->focus);
                G->use_focus_hands = (int)def->hands;
            }
        }
    }

    G->accumulator += dt * wheels_timescale() * game_fire_timescale() * (G->host_timescale ? G->host_timescale->f : 1.0f);   // slowed while a radial wheel is open (single player), x the fire tuner's slow motion, x host_timescale
    if (G->paused) G->accumulator = 0;   // the pause menu (single player / host alone) stops the simulation
    int steps = 0;
    while (G->accumulator >= SIM_DT && steps < 8) {
        tick();
        G->accumulator -= SIM_DT;
        steps++;
    }
    if (steps == 8) G->accumulator = 0; // spiral of death guard
    game_holdpoint_test_frame();
    G->alpha = (float)(G->accumulator / SIM_DT);
    game_death_frame(dt);   // death sequence: post / mix / HUD from this frame's state (docs/REVIVE.md)
    game_revive_frame(G->world, dt);   // reconstruction bodies + particles
    // Reconciliation corrections fade over cam_recon_smooth_time (the head stabilizer adds its own spring).
    {
        float tsm = camera_fp_cvars()->recon_smooth ? camera_fp_cvars()->recon_smooth->f : 0.08f;
        G->error_offset = tsm > 1e-4f ? v3_scale(G->error_offset, 1.0f - exp_decay(3.0f / tsm, dt)) : v3(0, 0, 0);
    }
    if (G->mode == GM_CLIENT) net_client_interpolate(G->world, dt);
    update_focus();
    game_hud_tick(dt);
}

// ------------------------------------------------------------------ setup / teardown

static void destroy_world(void) {
    if (!G->world) return;
    render_wait_idle();
    if (G->world->chunks) chunks_destroy(G->world);
    world_destroy(G->world);
    G->world = NULL;
    arena_reset(&g_engine.level_arena);
    dbg_clear();
}

static bool load_map(World *w, const char *map) {
    foliage_level_reset();   // test foliage placements of the previous map (its physics went with its world)
    if (!strcmp(map, "streaming") || !strcmp(map, "terrain") || !strcmp(map, "flat")) {
        render_vgeo_set_level(NULL, 0);
        return chunks_create(w, !strcmp(map, "flat") ? CHUNKS_FLAT : CHUNKS_NOISE, (u32)G->world_seed->i);
    }
    char path[128];
    snprintf(path, sizeof(path), "data/levels/%s.json", map);
    bool ok = level_load(w, path);
    // Virtualized geometry placements go to the renderer (render-only, no collision).
    RenderVgeoPlacement pl[MAX_LEVEL_VGEO];
    int n = ok ? w->level.vgeo_count : 0;
    for (int i = 0; i < n; i++) {
        const LevelVgeo *v = &w->level.vgeo[i];
        pl[i] = (RenderVgeoPlacement){ v->mesh, v->pos, v->color, v->yaw, v->scale, v->scale_jitter, v->spacing, v->roughness, { v->grid[0], v->grid[1] }, v->seed };
    }
    render_vgeo_set_level(pl, n);
    return ok;
}

bool game_host(const char *map) { return game_host_ex(map, 0); }

// flags: GH_KEEP_NET (a lobby session is starting its game: the connected players stay connected and
// are welcomed after the load), GH_NO_AUTOHOST (main menu background: not reachable on the LAN).
bool game_host_ex(const char *map, u32 flags) {
    if (!(flags & GH_KEEP_NET)) net_stop();
    destroy_world();
    G->world = world_create(true, &G->items);
    revive_world_reset(G->world);
    dg_strcpy(G->map, sizeof(G->map), map);
    if (!load_map(G->world, map)) {
        log_error("game: failed to load map '%s', falling back to devroom", map);
        destroy_world();
        G->world = world_create(true, &G->items);
        revive_world_reset(G->world);
        dg_strcpy(G->map, sizeof(G->map), "devroom");
        if (!load_map(G->world, "devroom")) return false;
    }
    daynight_level_loaded(G->world);
    weather_level_loaded(G->world, G->map);
    water_level_loaded(G->world, G->map);
    vehicles_level_loaded(G->world, G->map);
    handpose_system_scan(G->world, false);   // interactables placed in this level
    flesh_fx_clear();
    bal_world_clear();
    bal_fx_clear();
    horde_level_reset();
    spectate_reset();
    spectator_reset();
    game_hud_reset();
    G->mode = GM_HOST;
    G->local_slot = 0;
    G->client_seq = 0;
    G->accumulator = 0;
    G->hotbar_slot = 0;
    memset(&G->view, 0, sizeof(G->view));
    game_spawn_player(G->world, 0, G->cl_name->str, true, -1);
    G->view.yaw = G->world->players[0].sim.move.yaw;
    if (G->net_autohost->i && !(flags & (GH_KEEP_NET | GH_NO_AUTOHOST))) net_host(TRANSPORT_LAN, (u16)cvar_find("net_port")->i);
    log_info("game: hosting '%s'", G->map);
    return true;
}

void game_cl_welcome(int slot, const char *map, u32 seed, u32 server_tick) {
    destroy_world();
    G->world = world_create(false, &G->items);
    revive_world_reset(G->world);
    dg_strcpy(G->map, sizeof(G->map), map);
    cvar_set_int(G->world_seed, (int)seed);
    if (!load_map(G->world, map)) log_error("client: cannot load map '%s'", map);
    daynight_level_loaded(G->world);   // config; the time itself arrives with every snapshot
    weather_level_loaded(G->world, map);   // puddle mask; the weather state arrives with the snapshots
    water_level_loaded(G->world, map);     // the same bodies as the host (nothing replicated)
    vehicles_level_loaded(G->world, map);   // spike zones; vehicles replicate
    handpose_system_scan(G->world, false);   // interactables placed in this level
    flesh_fx_clear();
    bal_world_clear();
    bal_fx_clear();
    horde_level_reset();
    spectator_reset();
    game_hud_reset();
    G->mode = GM_CLIENT;
    G->local_slot = slot;
    G->client_seq = 0;
    G->last_ack_seq = 0;
    G->accumulator = 0;
    G->world->tick = server_tick;
    memset(&G->view, 0, sizeof(G->view));
    memset(G->cmd_history, 0, sizeof(G->cmd_history));
    // The local player's slot is filled when the first snapshot creates its entity.
    log_info("client: joined as slot %d on '%s'", slot, map);
}

void game_cl_disconnected(const char *reason) {
    log_warn("disconnected: %s", reason ? reason : "");
    game_disconnect_to_local(reason);
}

void game_disconnect_to_local(const char *reason) {
    net_stop();
    // Main menu builds (not --test): a lost / left session returns to the menu.
    if (mainmenu_enabled()) {
        mainmenu_enter(reason);
        return;
    }
    char hint[64];
    snprintf(hint, sizeof(hint), "Disconnected: %s", reason ? reason : "");
    game_host(G->g_map->str);
    game_add_hint(hint);
}

int game_sv_player_join(int peer, const char *name) {
    World *w = G->world;
    if (!w || G->mode != GM_HOST) return -1;
    bool join_dead = spectate_join_dead(w);   // sv_join_dead: before the joiner counts as alive
    for (int s = 0; s < MAX_PLAYERS; s++) {
        if (w->players[s].active) continue;
        game_spawn_player(w, s, name, false, peer);
        ev_hint(w, -1, "%s joined", name);
        revive_sv_player_joined(w, s);   // the DNA registry (events are not replayed to late joiners)
        if (join_dead) {
            // Late joiner: dead and hidden until the respawn rules let it in, spectating meanwhile.
            Player *p = &w->players[s];
            int i = (int)ENT_INDEX(p->ent);
            w->flags[i] |= EF_DEAD | EF_HIDDEN;
            w->health[i].hp = 0;
            p->sim.move.mode = MOVE_DEAD;
            spectate_on_death(w, s, -1);
            spectate_request(w, s, SPEC_IN_EYE, SPEC_REQ_AUTO, false);
        }
        return s;
    }
    return -1;
}

void game_sv_player_leave(int slot) {
    World *w = G->world;
    if (!w || slot < 0 || slot >= MAX_PLAYERS) return;
    char name[32];
    dg_strcpy(name, sizeof(name), w->players[slot].name);
    spectate_player_left(slot);
    game_remove_player(w, slot);
    ev_hint(w, -1, "%s left", name);
}

void game_sv_receive_cmd(int slot, const UserCmd *cmd) {
    World *w = G->world;
    if (!w || slot < 0 || slot >= MAX_PLAYERS) return;
    Player *p = &w->players[slot];
    if (!p->active || cmd->seq <= p->last_processed_seq) return;
    // Keep the queue sorted by seq: redundant sends fill gaps left by lost packets.
    int at = p->queue_count;
    for (int i = 0; i < p->queue_count; i++) {
        if (p->queue[i].seq == cmd->seq) return;   // duplicate (redundant send)
        if (p->queue[i].seq > cmd->seq) { at = i; break; }
    }
    if (p->queue_count >= 32) {
        if (at == 0) return;
        memmove(p->queue, p->queue + 1, sizeof(UserCmd) * 31);
        p->queue_count = 31;
        at--;
    }
    memmove(p->queue + at + 1, p->queue + at, sizeof(UserCmd) * (size_t)(p->queue_count - at));
    p->queue[at] = *cmd;
    p->queue_count++;
}

// ------------------------------------------------------------------ console commands

static void cmd_map(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: map <devroom|streaming|flat>"); return; }
    cvar_set_cv(G->g_map, argv[1]);
    if (mainmenu_active()) mainmenu_session_started();   // map from the console: the menu closes
    game_host(argv[1]);
}

static void cmd_flashlight(int argc, const char **argv) {
    G->flashlight = argc > 1 ? atoi(argv[1]) != 0 : !G->flashlight;
}

static void cmd_give(int argc, const char **argv) {
    World *w = G->world;
    if (argc < 2 || G->mode != GM_HOST) { log_info("usage: give <item> [count] [player slot] (host only); give_list [filter] lists the item ids, give_all / give_ammo"); return; }
    u16 item = item_find(&G->items, argv[1]);
    if (!item) { log_warn("give: unknown item '%s'", argv[1]); return; }
    int count = argc > 2 ? atoi(argv[2]) : (G->items.items[item].max_stack > 1 ? G->items.items[item].max_stack : 1);
    const WeaponDef *wd = item_weapon(&G->items, item);
    int target = argc > 3 ? atoi(argv[3]) : G->local_slot;
    if (target < 0 || target >= MAX_PLAYERS || !w->players[target].active) { log_warn("give: no player in slot %d", target); return; }
    Player *p = &w->players[target];
    int left = inv_add(&p->inv, &G->items, item, count, wd ? (i16)wd->mag_size : 0);
    p->inv.revision++;
    log_info("gave %d x %s", count - left, argv[1]);
}

// give_list [filter]: every item id (what `give` takes), with its kind and name.
static void cmd_give_list(int argc, const char **argv) {
    static const char *kinds[ITEM_KIND_COUNT] = { "misc", "ammo", "consumable", "weapon", "key", "attachment", "throwable" };
    const char *f = argc > 1 ? argv[1] : NULL;
    int n = 0;
    for (int i = 1; i < G->items.item_count; i++) {
        const ItemDef *d = &G->items.items[i];
        if (f && !strstr(d->id, f) && !strstr(d->name, f)) continue;
        log_info("  %-22s %-10s %s", d->id, (unsigned)d->kind < ITEM_KIND_COUNT ? kinds[d->kind] : "?", d->name);
        n++;
    }
    log_info("give_list: %d items (give <id> [count])", n);
}

// give_all [player slot]: one of every item (a full stack of stackables) while there is room;
// give_ammo [player slot]: a full stack of every ammo type.
static void give_kinds(int argc, const char **argv, bool ammo_only) {
    World *w = G->world;
    if (!w || G->mode != GM_HOST) { log_info("%s: host only", ammo_only ? "give_ammo" : "give_all"); return; }
    int target = argc > 1 ? atoi(argv[1]) : G->local_slot;
    if (target < 0 || target >= MAX_PLAYERS || !w->players[target].active) { log_warn("give: no player in slot %d", target); return; }
    Player *p = &w->players[target];
    int given = 0, full = 0;
    for (int i = 1; i < G->items.item_count; i++) {
        const ItemDef *d = &G->items.items[i];
        if (ammo_only ? d->kind != ITEM_AMMO : d->kind == ITEM_KEY) continue;
        int count = d->max_stack > 1 ? d->max_stack : 1;
        const WeaponDef *wd = item_weapon(&G->items, (u16)i);
        int left = inv_add(&p->inv, &G->items, (u16)i, count, wd ? (i16)wd->mag_size : 0);
        if (left < count) given++;
        else full++;
    }
    p->inv.revision++;
    log_info("%s: %d items given%s", ammo_only ? "give_ammo" : "give_all", given, full ? " (inventory full for the rest)" : "");
}
static void cmd_give_all(int argc, const char **argv) { give_kinds(argc, argv, false); }
static void cmd_give_ammo(int argc, const char **argv) { give_kinds(argc, argv, true); }

// Host: put attachment id `att` (0 = remove) into attachment slot `aslot` of the weapon in
// inventory slot `inv_slot` of player `slot`. Validated against the weapon's slots.
bool game_sv_attach(World *w, int slot, int inv_slot, int aslot, int att) {
    if (!w || slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) return false;
    Player *p = &w->players[slot];
    if (inv_slot < 0 || inv_slot >= INV_SLOTS || aslot < 0 || aslot >= ATT_SLOTS) return false;
    InvSlot *s = &p->inv.slots[inv_slot];
    const WeaponDef *wd = item_weapon(w->items, s->item);
    if (!wd || !wd->att_slots[aslot].present) { ev_hint(w, slot, "No %s slot on this weapon", attach_slot_name(aslot)); return false; }
    const AttachmentDef *a = attach_def(w->items, (u8)att);
    if (att && (!a || a->slot != aslot || !attach_allowed(wd, a))) { ev_hint(w, slot, "Doesn't fit"); return false; }
    if (s->att[aslot] == (u8)att) return true;
    s->att[aslot] = (u8)att;
    // Capacity may have shrunk (extended magazine removed): the extra rounds go back to the reserve.
    WeaponDef eff;
    attach_weapon_effective(w->items, wd, s->att, &eff);
    if (eff.mag_size > 0 && s->ammo > eff.mag_size) {
        int extra = s->ammo - eff.mag_size;
        s->ammo = (i16)eff.mag_size;
        if (wd->ammo_item) inv_add(&p->inv, w->items, wd->ammo_item, extra, 0);
    }
    p->inv.revision++;
    ev_hint(w, slot, a ? "Attached %s" : "Removed %s", a ? a->name : attach_slot_name(aslot));
    return true;
}

// attach <slot> <attachment|none>: equipped weapon (host applies, clients send an RPC).
static void cmd_attach(int argc, const char **argv) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!w || !p) return;
    int inv_slot = DG_CLAMP((int)p->sim.wpn.slot, 0, INV_HOTBAR - 1);
    const InvSlot *s = &p->inv.slots[inv_slot];
    const WeaponDef *wd = item_weapon(&G->items, s->item);
    if (argc < 3) {
        log_info("usage: attach <optic|muzzle|underbarrel|magazine|stock> <attachment id|none>");
        if (!wd) { log_info("  (no weapon equipped)"); return; }
        for (int k = 0; k < ATT_SLOTS; k++) {
            if (!wd->att_slots[k].present) continue;
            const AttachmentDef *cur = attach_def(&G->items, s->att[k]);
            char opts[256] = "";
            for (int i = 1; i <= G->items.attachment_count; i++) {
                const AttachmentDef *a = &G->items.attachments[i];
                if (a->slot == k && attach_allowed(wd, a)) { strncat(opts, " ", sizeof(opts) - strlen(opts) - 1); strncat(opts, a->id, sizeof(opts) - strlen(opts) - 1); }
            }
            log_info("  %-11s %-14s options:%s", attach_slot_name(k), cur ? cur->id : "-", opts);
        }
        return;
    }
    int k = attach_slot_from_name(argv[1]);
    if (k < 0) { log_warn("attach: unknown slot '%s'", argv[1]); return; }
    int id = 0;
    if (strcmp(argv[2], "none") && strcmp(argv[2], "0")) {
        id = attach_find(&G->items, argv[2]);
        if (!id) { log_warn("attach: unknown attachment '%s'", argv[2]); return; }
    }
    if (!wd) { log_warn("attach: no weapon equipped"); return; }
    game_rpc(RPC_ATTACH, inv_slot, k, id);
}

// bench: open the weapon bench UI here (dev; the dev room benches open it with Use).
static void cmd_bench(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!G->world || !game_local_player()) return;
    G->bench_open = !G->bench_open;
    G->bench_ent = 0;
}

static void cmd_noclip(int argc, const char **argv) { G->noclip_request = true; }

// kill [player slot]: suicide, or (host, tests) kill another player.
static void cmd_kill(int argc, const char **argv) {
    if (G->mode != GM_HOST) return;
    Player *p = game_local_player();
    if (argc > 1) {
        int s = atoi(argv[1]);
        p = s >= 0 && s < MAX_PLAYERS && G->world->players[s].active ? &G->world->players[s] : NULL;
        if (!p) { log_warn("kill: no player in slot %d", s); return; }
    }
    if (p) apply_damage_ex(G->world, p->ent, 10000, DMG_GENERIC, p->ent, p->sim.move.pos, v3(0, 0, 0), -1, 0.0f, DMGF_KILL);   // kill-type: no down
}

static void cmd_damage(int argc, const char **argv) {
    if (G->mode != GM_HOST || argc < 2) { log_info("usage: damage <amount> (host only)"); return; }
    Player *p = game_local_player();
    if (!p) return;
    vec3 from = v3_madd(p->sim.move.pos, right_from_yaw(G->view.yaw), 3.0f); // hit from the right
    apply_damage(G->world, p->ent, (float)atof(argv[1]), DMG_BULLET, ENT_NONE, v3_add(p->sim.move.pos, v3(0, 1.4f, 0)),
                 v3_norm(v3_sub(p->sim.move.pos, from)), false);
}

static void cmd_setpos(int argc, const char **argv) {
    Player *p = game_local_player();
    if (!p || argc < 4) { log_info("usage: setpos x y z [yaw_deg] [pitch_deg]"); return; }
    vec3 pos = v3((float)atof(argv[1]), (float)atof(argv[2]), (float)atof(argv[3]));
    p->sim.move.pos = pos;
    p->sim.move.vel = v3(0, 0, 0);
    p->sim.move.mode = MOVE_WALK;
    if (p->character) { phys_character_set_pos(p->character, pos); phys_character_set_vel(p->character, v3(0, 0, 0)); }
    if (argc > 4) G->view.yaw = (float)atof(argv[4]) * DG_DEG2RAD;
    if (argc > 5) G->view.pitch = (float)atof(argv[5]) * DG_DEG2RAD;
    G->view.smooth_init = false;
}

static void cmd_getpos(int argc, const char **argv) {
    Player *p = game_local_player();
    if (!p) return;
    vec3 v = p->sim.move.pos;
    log_info("pos %.2f %.2f %.2f yaw %.1f pitch %.1f mode %d ground %d", v.x, v.y, v.z, G->view.yaw * DG_RAD2DEG,
             G->view.pitch * DG_RAD2DEG, p->sim.move.mode, p->sim.move.on_ground);
}

static void cmd_setang(int argc, const char **argv) {
    if (argc < 3) return;
    G->view.yaw = (float)atof(argv[1]) * DG_DEG2RAD;
    G->view.pitch = (float)atof(argv[2]) * DG_DEG2RAD;
}

static void cmd_slot(int argc, const char **argv) {
    if (argc > 1) { G->hotbar_slot = (u8)DG_CLAMP(atoi(argv[1]) - 1, 0, 8); G->phone_out = false; }
}

static void cmd_phone_out(int argc, const char **argv) {
    G->phone_out = argc > 1 ? atoi(argv[1]) != 0 : !G->phone_out;
}

static void cmd_inv(int argc, const char **argv) {
    Player *p = game_local_player();
    if (!p) return;
    for (int i = 0; i < INV_SLOTS; i++) {
        InvSlot *s = &p->inv.slots[i];
        if (s->item) log_info("  slot %2d: %-14s x%d ammo %d", i, item_def(&G->items, s->item)->id, s->count, s->ammo);
    }
    log_info("  equipped mag (predicted) %d", p->sim.wpn.mag);
    Health *h = &G->world->health[ENT_INDEX(p->ent)];
    log_info("  hp %.0f armor %.0f, hotbar %d, focus %u", h->hp, h->armor, G->hotbar_slot + 1, G->focus);
}

static void cmd_lookat(int argc, const char **argv) {
    if (argc < 4) return;
    vec3 target = v3((float)atof(argv[1]), (float)atof(argv[2]), (float)atof(argv[3]));
    vec3 eye = G->view.eye;
    // Teleported since the last frame (setpos just before): the eye the player will have there, the
    // simulated eye + the first-person camera's offset (docs/AIM.md), not last frame's camera.
    Player *lp = game_local_player();
    if (lp && G->world) {
        vec3 se = player_eye_pos(&lp->sim.move), ec;
        if (v3_dist(se, eye) > 1.0f) eye = chars_eye_cam(G->world, lp->ent, &ec, NULL) ? aim_cam_to_world(se, G->view.yaw, G->view.pitch, ec) : se;
    }
    vec3 d = v3_sub(target, eye);
    G->view.yaw = atan2f(-d.x, -d.z);
    G->view.pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z));
}

// wpn_magdump [weapon] [10|25|50] [ads 0/1]: stands on the recoil lane, faces the wall at that
// distance and empties a full magazine; every bullet is checked against its cone.
static void cmd_magdump(int argc, const char **argv) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!w || !p || G->mode != GM_HOST) { log_info("usage: wpn_magdump [weapon] [10|25|50] [ads] (host only)"); return; }
    if (argc > 1) {
        u16 item = item_find(&G->items, argv[1]);
        const WeaponDef *wd = item ? item_weapon(&G->items, item) : NULL;
        if (!wd || wd->kind == WPN_MELEE) { log_warn("wpn_magdump: %s is not a gun", argv[1]); return; }
        int at = -1;
        for (int i = 0; i < INV_HOTBAR; i++) if (p->inv.slots[i].item == item) at = i;
        if (at < 0) {
            inv_add(&p->inv, &G->items, item, 1, (i16)wd->mag_size);
            for (int i = 0; i < INV_HOTBAR; i++) if (p->inv.slots[i].item == item) at = i;
        }
        if (at < 0) { log_warn("wpn_magdump: no free hotbar slot"); return; }
        G->hotbar_slot = (u8)at;
    }
    InvSlot *s = &p->inv.slots[G->hotbar_slot];
    const WeaponDef *wd = item_weapon(&G->items, s->item);
    if (!wd || wd->kind == WPN_MELEE) { log_warn("wpn_magdump: equip a gun first"); return; }
    s->ammo = (i16)wd->mag_size;
    p->sim.wpn.mag = s->ammo;
    p->sim.wpn.reloading = 0;
    float dist = argc > 2 ? (float)atof(argv[2]) : 25.0f;
    vec3 target = dist <= 15 ? v3(-1.5f, 1.35f, 18) : (dist <= 35 ? v3(1.5f, 1.35f, 3) : v3(0, 1.35f, -22));
    vec3 feet = v3(0, 0.05f, 28);
    p->sim.move.pos = feet;
    p->sim.move.vel = v3(0, 0, 0);
    p->sim.move.mode = MOVE_WALK;
    if (p->character) { phys_character_set_pos(p->character, feet); phys_character_set_vel(p->character, v3(0, 0, 0)); }
    vec3 d = v3_sub(target, v3_add(feet, v3(0, PLAYER_EYE_STAND, 0)));
    G->view.yaw = atan2f(-d.x, -d.z);
    G->view.pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z));
    G->view.smooth_init = false;
    G->magdump_ads = argc > 3 ? atoi(argv[3]) != 0 : true;
    G->magdump_dist = v3_len(d);
    G->magdump_ticks = 1;
    G->magdump_shots = 0;
    G->magdump_last_mag = s->ammo;
    recoil_reset(&p->sim.wpn.rc, wd->recoil.seed_per_mag ? recoil_mag_seed(G->client_seq, (u32)G->local_slot) : (dg_hash_str(wd->id) | 1u));
    p->sim.wpn.rc.have_prev = 0;   // the teleport / view snap is not "player input"
    weapons_debug_reset();
    log_info("magdump: %s, %d rounds at %.1f m (%s)", wd->id, wd->mag_size, G->magdump_dist, G->magdump_ads ? "ADS" : "hip");
}

static void cmd_weapon_reload(int argc, const char **argv) {
    if (weapons_reload_defs(&G->items, "data/weapons")) game_add_hint("Weapon definitions reloaded");
}

// ------------------------------------------------------------------ characters / animation

static void cmd_zombie_spawn(int argc, const char **argv) {
    World *w = G->world;
    if (!w || G->mode != GM_HOST) { log_info("usage: zombie_spawn [count] [character_id|id] [radius] (host only)"); return; }
    int count = argc > 1 ? atoi(argv[1]) : 1;
    int cid = 0;
    if (argc > 2) {
        const CharacterDef *d = anim_lib_char_by_id(g_engine.anim, argv[2]);
        cid = d ? d->net_id : atoi(argv[2]);
    }
    float radius = argc > 3 ? (float)atof(argv[3]) : 3.0f;
    Player *p = game_local_player();
    vec3 c = p ? v3_add(p->sim.move.pos, v3_scale(forward_from_yaw(G->view.yaw), radius + 2.0f)) : v3(0, 0, 0);
    for (int k = 0; k < count; k++) {
        float a = (float)k / (float)DG_MAX(1, count) * 2.0f * DG_PI;
        vec3 pos = v3_add(c, v3(sinf(a) * radius * (count > 1), 0, cosf(a) * radius * (count > 1)));
        RayHit rh;
        if (phys_raycast(w->phys, v3_add(pos, v3(0, 3, 0)), v3(0, -8, 0), PL_MASK(PL_STATIC), PHYS_INVALID_BODY, &rh)) pos.y = rh.point.y;
        int id = cid;
        if (!id) {
            static const char *kinds[3] = { "zombie_placeholder", "zombie_brute", "zombie_runner" };
            const CharacterDef *d = anim_lib_char_by_id(g_engine.anim, kinds[k % 3]);
            id = d ? d->net_id : 2;
        }
        spawn_zombie(w, pos, G->view.yaw + DG_PI + a * 0.3f, (u8)id);
    }
    log_info("spawned %d zombies", count);
}

static void cmd_zombie_attack(int argc, const char **argv) { if (G->world) zombies_attack(G->world); }

// zombie_kill_nearest [count] [head]: lethal hits from the local player on the nearest zombies (host;
// kill counter / feed tests, docs/UI_WHEELS_HUD.md).
static void cmd_zombie_kill_nearest(int argc, const char **argv) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!w || !p || !w->authority) return;
    int n = argc > 1 ? atoi(argv[1]) : 1;
    bool head = argc > 2 && !strcmp(argv[2], "head");
    for (int k = 0; k < n; k++) {
        int best = -1;
        float bd = 1e9f;
        ENT_FOREACH(w, i) {
            if (w->type[i] != ET_ZOMBIE || (w->flags[i] & EF_DEAD) || !(w->comps[i] & C_HEALTH)) continue;
            float d = v3_dist(w->xf[i].pos, p->sim.move.pos);
            if (d < bd) { bd = d; best = i; }
        }
        if (best < 0) { log_info("zombie_kill_nearest: no zombie left"); return; }
        vec3 at = v3_add(w->xf[best].pos, v3(0, head ? 1.65f : 1.1f, 0));
        apply_damage_part(w, ENT_MAKE(best, w->gen[best]), 10000.0f, DMG_BULLET, p->ent, at, v3_norm(v3_sub(at, player_eye_pos(&p->sim.move))), head ? BP_HEAD : BP_CHEST, 1.0f);
    }
}

static void cmd_zombie_clear(int argc, const char **argv) {
    World *w = G->world;
    if (!w || G->mode != GM_HOST) return;
    int n = 0;
    ENT_FOREACH(w, i) if (w->type[i] == ET_ZOMBIE) { ent_destroy(w, ENT_MAKE(i, w->gen[i])); n++; }
    log_info("removed %d zombies", n);
}

static void warn_live_session(const char *what) {
    if (net_role() != NET_OFF && G->mode == GM_HOST && net_host_client_count() > 0)
        log_warn("%s: asset hashes changed in a live session - clients desync until they reload the same files", what);
}

static void cmd_anim_reload(int argc, const char **argv) {
    if (!g_engine.anim) return;
    anim_lib_reload_meta(g_engine.anim);
    log_info("anim_reload: manifest %08x", anim_manifest_combined(&g_engine.anim->manifest));
    warn_live_session("anim_reload");
}

static void cmd_anim_rebake(int argc, const char **argv) {
    if (!g_engine.anim) return;
    anim_lib_rebake(g_engine.anim);
    log_info("anim_rebake: %d clips, manifest %08x", g_engine.anim->clip_count, anim_manifest_combined(&g_engine.anim->manifest));
    warn_live_session("anim_rebake");
}

static void cmd_character_reload(int argc, const char **argv) {
    if (!g_engine.anim) return;
    anim_lib_reload_characters(g_engine.anim);
    g_engine.anim->revision++;
    warn_live_session("character_reload");
}

static void cmd_hitbox_reload(int argc, const char **argv) {
    if (!g_engine.anim) return;
    anim_lib_reload_hitboxes(g_engine.anim);
    warn_live_session("hitbox_reload");
}

static void cmd_anim_template(int argc, const char **argv) {
    if (!g_engine.anim) return;
    anim_lib_write_template(g_engine.anim, "player", false);
    anim_lib_write_template(g_engine.anim, "zombie", true);
}

static void cmd_ragdoll(int argc, const char **argv) {
    World *w = G->world;
    if (!w) return;
    // ragdoll [push N]: drops (or pushes) the character under the crosshair.
    vec3 dir = dir_from_angles(G->view.yaw, G->view.pitch);
    EntityId e;
    HitboxRayHit h;
    if (!chars_raycast(w, game_local_player() ? game_local_player()->ent : ENT_NONE, G->view.eye, dir, 60.0f, 0, &e, &h)) { log_info("ragdoll: no character under the crosshair"); return; }
    float strength = argc > 2 ? (float)atof(argv[2]) : 120.0f;
    chars_ragdoll_now(w, e, v3_scale(dir, strength));
}

static void cmd_char_kill(int argc, const char **argv) {
    // char_kill [damage]: host-side lethal hit on the character under the crosshair (dies -> ragdoll).
    World *w = G->world;
    if (!w || !w->authority) { log_info("char_kill: host only"); return; }
    vec3 dir = dir_from_angles(G->view.yaw, G->view.pitch);
    EntityId e;
    HitboxRayHit h;
    if (!chars_raycast(w, game_local_player() ? game_local_player()->ent : ENT_NONE, G->view.eye, dir, 120.0f, 0, &e, &h)) { log_info("char_kill: no character under the crosshair"); return; }
    float dmg = argc > 1 ? (float)atof(argv[1]) : 100000.0f;
    float done = apply_damage_part(w, e, dmg, DMG_BULLET, game_local_player() ? game_local_player()->ent : ENT_NONE, h.point, dir, h.part, 1.0f);
    log_info("char_kill: ent %u hit %s for %.0f", e, body_part_name(h.part), done);
}

// char_spawn <character id> x y z [yaw] [item]: a character (dummy, or zombie for zombie ids) at an exact spot.
static void cmd_char_spawn(int argc, const char **argv) {
    World *w = G->world;
    if (!w || !w->authority || !g_engine.anim || argc < 5) { log_info("usage: char_spawn <character> x y z [yaw_deg] [item] (host)"); return; }
    const CharacterDef *d = anim_lib_char_by_id(g_engine.anim, argv[1]);
    if (!d) { log_info("char_spawn: unknown character '%s'", argv[1]); return; }
    vec3 pos = v3((float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4]));
    float yaw = argc > 5 ? (float)atof(argv[5]) * DG_DEG2RAD : 0.0f;
    EntityId e;
    if (d->kind == CK_ZOMBIE) {
        e = spawn_zombie(w, pos, yaw, d->net_id);
    } else {
        e = spawn_dummy(w, pos, yaw, d->hp > 0 ? d->hp : 300);
        if (e) {
            char_detach(w, e);
            w->char_id[ENT_INDEX(e)] = d->net_id;
            char_attach(w, e, d->net_id);
        }
    }
    if (e && argc > 6) w->item[ENT_INDEX(e)] = item_find(w->items, argv[6]);
    log_info("char_spawn: %s ent %u", d->id, e);
}

// LOD line-up: one player-model dummy (rifle) and one zombie at 5 / 15 / 30 / 60 / 120 m along the view.
static EntityId g_lineup[16];
static int g_lineup_count;

static void cmd_anim_lod_lineup(int argc, const char **argv) {
    World *w = G->world;
    if (!w || !w->authority || !g_engine.anim) { log_info("anim_lod_lineup: host only"); return; }
    for (int k = 0; k < g_lineup_count; k++) if (ent_valid(w, g_lineup[k])) ent_destroy(w, g_lineup[k]);
    g_lineup_count = 0;
    if (argc > 1 && !strcmp(argv[1], "clear")) { log_info("anim_lod_lineup: cleared"); return; }
    Player *p = game_local_player();
    if (!p) return;
    static const float dist[5] = { 5, 15, 30, 60, 120 };
    vec3 fwd = forward_from_yaw(G->view.yaw), right = right_from_yaw(G->view.yaw);
    const CharacterDef *pd = anim_lib_char_by_id(g_engine.anim, "player");
    const CharacterDef *zd = anim_lib_char_by_id(g_engine.anim, "zombie_placeholder");
    for (int k = 0; k < 5; k++) {
        for (int side = 0; side < 2; side++) {
            vec3 pos = v3_add(p->sim.move.pos, v3_add(v3_scale(fwd, dist[k]), v3_scale(right, side ? 0.8f : -0.8f)));
            RayHit rh;
            if (phys_raycast(w->phys, v3_add(pos, v3(0, 10, 0)), v3(0, -30, 0), PL_MASK(PL_STATIC), PHYS_INVALID_BODY, &rh)) pos.y = rh.point.y;
            float yaw = G->view.yaw + DG_PI;
            EntityId e = ENT_NONE;
            if (side == 0 && pd) {
                e = spawn_dummy(w, pos, yaw, 300);
                if (e) {
                    char_detach(w, e);
                    w->char_id[ENT_INDEX(e)] = pd->net_id;
                    char_attach(w, e, pd->net_id);
                    w->item[ENT_INDEX(e)] = item_find(w->items, "rifle");
                }
            } else if (zd) {
                e = spawn_zombie(w, pos, yaw, zd->net_id);
                CharacterInst *ci = e ? char_get(w, e) : NULL;
                if (ci) ci->ai.wander_radius = 0;
            }
            if (e && g_lineup_count < 16) g_lineup[g_lineup_count++] = e;
        }
    }
    log_info("anim_lod_lineup: %d characters at 5/15/30/60/120 m (r_lod_debug 1 shows LOD + pixels; anim_lod_lineup clear)", g_lineup_count);
}

static void cmd_char_list(int argc, const char **argv) {
    World *w = G->world;
    if (!w || !w->chars) return;
    for (int k = 0; k < MAX_CHARACTERS; k++) {
        CharacterInst *c = w->chars->c[k];
        if (!c) continue;
        const ControllerInst *ct = &c->rt.ctrl;
        log_info("  ent %5u %-18s %-7s state %s/%s phase %.2f legs %+.0f/%+.0f mv %.2f,%.2f lod %d %.0fpx resyncs %d events hit %u die %u%s", c->ent, c->def->id,
                 ent_type_name((EntityType)w->type[ENT_INDEX(c->ent)]), ctrl_state_name(ct, 0, ct->layer[0].state), ctrl_state_name(ct, 1, ct->layer[1].state),
                 ct->layer[0].phase, c->rt.leg_yaw * DG_RAD2DEG, c->rt.leg_yaw_target * DG_RAD2DEG, c->rt.in.move_local.x, c->rt.in.move_local.y, c->lod, c->pixels, c->resyncs, c->events_received[AEV_HIT], c->events_received[AEV_DIE],
                 c->dead ? (c->rag.active ? " DEAD ragdoll" : (c->settled ? " DEAD settled" : " DEAD")) : "");
    }
    int ev, df;
    chars_budget_stats(&ev, &df);
    log_info("  %d characters, %d ragdolls; last tick %d post-passes, %d deferred by anim_eval_budget", chars_count(w), w->chars->ragdolls, ev, df);
}

static void cmd_status(int argc, const char **argv) {
    World *w = G->world;
    if (!w) return;
    log_info("map %s mode %s tick %u entities %d bodies %d", G->map, G->mode == GM_HOST ? "host" : "client", w->tick, w->max_used,
             phys_body_count(w->phys));
    if (G->mode == GM_CLIENT)
        log_info("  prediction error %.4f m, recoil prediction error %.4f (max %.4f)", G->prediction_error, G->recoil_pred_error,
                 G->recoil_pred_error_max);
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (p->active) log_info("  slot %d %-16s %s peer %d hp %.0f", s, p->name, p->local ? "local" : "remote", p->peer, w->health[ENT_INDEX(p->ent)].hp);
    }
}

// Settings migrations after the config file is loaded (archived values of renamed / repurposed
// cvars). 1: cam_bob_scale now offsets the camera from the head eye (default 0).
void game_config_loaded(void) {
    Cvar *ver = cvar_find("cfg_migration");
    if (!ver) return;
    if (ver->i < 1) {
        Cvar *bob = cvar_find("cam_bob_scale");
        if (bob) cvar_reset(bob);
        log_info("config: migrated cam_bob_scale to its new default (first-person camera on the head)");
    }
    if (ver->i < 2) {
        // 2: gun sway / recoil kick back on (they were saved as 0 by the camera-fix build).
        const char *names[] = { "wpn_kick_scale", "cam_kick_scale" };
        for (int i = 0; i < 2; i++) { Cvar *c = cvar_find(names[i]); if (c) cvar_reset(c); }
        log_info("config: weapon kick / camera kick reset to their defaults");
    }
    if (ver->i < 3) {
        // 3: sharper scopes (the old default picture resolution 0.5 -> 1.0).
        Cvar *ps = cvar_find("r_scope_pip_scale");
        if (ps && ps->f < 0.99f) { cvar_reset(ps); log_info("config: scope picture resolution raised to %.2f", ps->f); }
    }
    if (ver->i < 4) {
        // 4: weapon sway on (docs/RECOIL.md "Weapon sway"): walk bob and look lag back to their
        // new defaults; the reticle-moving aim sway / bob off (the gun sways about the reticle).
        const char *names[] = { "wpn_bob_scale", "wpn_look_lag" };
        for (int i = 0; i < 2; i++) { Cvar *c = cvar_find(names[i]); if (c) cvar_reset(c); }
        log_info("config: weapon sway enabled (bob, look lag reset to their defaults)");
    }
    cvar_set_int(ver, 4);
}

void game_init(void) {
    cvar_reg_int("cfg_migration", 0, 0, 1000, CVAR_ARCHIVE, "settings migration level (internal)");
    G = (Game *)calloc(1, sizeof(Game));
    G->local_slot = -1;
    G->inv_selected = -1;
    G->g_map = cvar_reg_string("g_map", "devroom", CVAR_ARCHIVE, "boot map: devroom | streaming | flat");
    G->sv_hostname = cvar_reg_string("sv_hostname", "Deadground Dev Server", CVAR_ARCHIVE, "server name shown in LAN browser");
    G->cl_name = cvar_reg_string("cl_name", "Survivor", CVAR_ARCHIVE, "player name");
    G->dev_mode = cvar_reg_bool("developer", true, CVAR_ARCHIVE, "developer HUD info");
    G->r_drawhitboxes = cvar_reg_bool("r_drawhitboxes", false, 0, "draw hitbox capsules");
    G->r_drawcollision = cvar_reg_bool("r_drawcollision", false, 0, "draw physics collision shapes");
    G->cl_showfps = cvar_reg_bool("cl_showfps", true, CVAR_ARCHIVE, "show FPS counter");
    G->net_autohost = cvar_reg_bool("net_autohost", true, CVAR_ARCHIVE, "accept LAN connections when hosting");
    G->host_timescale = cvar_reg_float("host_timescale", 1.0f, 0.01f, 4.0f, CVAR_CHEAT, "simulation speed (slow motion for the ballistics range / screenshots; single player or hosting alone)");
    G->interact_range = cvar_reg_float("cl_interact_range", 3.0f, 0.5f, 10.0f, 0, "interaction focus raycast length (m); each interactable also has its own range");
    G->world_seed = cvar_reg_int("world_seed", 1337, 0, 999999999, CVAR_ARCHIVE, "procedural world seed");
    cmd_register("map", cmd_map, "host a new game on a map");
    wheels_register();
    revive_register();
    game_death_register();
    game_pings_register();
    hud_register();
    openworld_register_commands();
    cmd_register("give", cmd_give, "give <item> [count]");
    cmd_register("give_list", cmd_give_list, "give_list [filter]: the item ids give takes");
    cmd_register("give_all", cmd_give_all, "give_all [player slot]: one of every item into the inventory (host)");
    cmd_register("give_ammo", cmd_give_ammo, "give_ammo [player slot]: a full stack of every ammo type (host)");
    cmd_register("flashlight", cmd_flashlight, "flashlight [0|1]: toggle / set the flashlight (key F)");
    cmd_register("bench", cmd_bench, "toggle the weapon bench (attachments) UI");
    cmd_register("attach", cmd_attach, "attach <slot> <attachment|none>: weapon attachments of the equipped weapon (no args: list)");
    cmd_register("noclip", cmd_noclip, "toggle noclip (sv_cheats)");
    cmd_register("kill", cmd_kill, "kill [player slot]: suicide (host: or another player)");
    cmd_register("damage", cmd_damage, "damage <amount>: hurt yourself (testing)");
    cmd_register("setpos", cmd_setpos, "teleport: setpos x y z [yaw] [pitch]");
    cmd_register("getpos", cmd_getpos, "print position");
    cmd_register("setang", cmd_setang, "set view angles: setang yaw pitch");
    cmd_register("slot", cmd_slot, "select hotbar slot 1-9");
    cmd_register("phone_out", cmd_phone_out, "phone_out [0|1]: take the built-in phone out / put it away (key X)");
    cmd_register("status", cmd_status, "print game status");
    cmd_register("inv", cmd_inv, "print local inventory");
    cmd_register("lookat", cmd_lookat, "aim the view at a world point: lookat x y z");
    cmd_register("zombie_spawn", cmd_zombie_spawn, "zombie_spawn [count] [character_id|id] [radius]: spawn test zombies in front of you");
    cmd_register("zombie_attack", cmd_zombie_attack, "all zombies play an attack");
    cmd_register("zombie_kill_nearest", cmd_zombie_kill_nearest, "zombie_kill_nearest [count] [head]: kill the nearest zombies as the local player (HUD tests)");
    cmd_register("zombie_clear", cmd_zombie_clear, "remove all zombies");
    cmd_register("anim_reload", cmd_anim_reload, "reload anim sets, clips.json, controller.json (hold styles, presets)");
    cmd_register("anim_rebake", cmd_anim_rebake, "re-bake changed FBX clips (cache keyed by file hash)");
    cmd_register("character_reload", cmd_character_reload, "reload assets/characters/*.json");
    cmd_register("hitbox_reload", cmd_hitbox_reload, "reload assets/human/hitboxes.json");
    cmd_register("anim_template", cmd_anim_template, "write anim_sets/player_template.json + zombie_template.json");
    cmd_register("ragdoll", cmd_ragdoll, "ragdoll [push N]: ragdoll / push the character under the crosshair (local)");
    cmd_register("char_kill", cmd_char_kill, "char_kill [damage]: lethal hit on the character under the crosshair (host)");
    cmd_register("char_spawn", cmd_char_spawn, "char_spawn <character> x y z [yaw] [item]: spawn a character (host)");
    cmd_register("anim_lod_lineup", cmd_anim_lod_lineup, "anim_lod_lineup [clear]: characters at 5/15/30/60/120 m along the view (host)");
    cmd_register("char_list", cmd_char_list, "list characters: state, LOD, resyncs, events");
    cmd_register("wpn_magdump", cmd_magdump, "wpn_magdump [weapon] [10|25|50] [ads]: empty a magazine at the recoil wall");
    cmd_register("weapon_reload", cmd_weapon_reload, "re-read data/weapons/*.json (live recoil tuning)");
    weapons_register_cvars();
    bal_world_register();
    weapons_add_shot_hook(on_shot, NULL);
    game_fire_init();
    crosshair_init();
    recoil_debug_init();
    camera_register_cvars();
    camera_fp_register_cvars();
    spectator_init();
    game_holdpoint_register();
    handpose_system_init();
    scenes_init();
    hp_editor_register();   // scene handpose
    game_post_init();
    player_register_cvars();
    // Animation assets (shared skeleton) before anything spawns characters.
    characters_register_cvars();
    cvar_reg_bool("cl_thirdperson", false, 0, "debug: camera behind the local player (full body visible)");
    cvar_reg_float("cl_thirdperson_dist", 3.0f, 0.5f, 10.0f, 0, "debug third-person camera distance");
    cvar_reg_bool("cl_camera_socket", true, CVAR_ARCHIVE, "first-person camera on the player model's head socket (assets/characters/player.json \"sockets\")");
    cvar_reg_float("cl_camera_socket_follow", 0.35f, 0.0f, 1.0f, CVAR_ARCHIVE, "how much of the head socket's vertical bob the first-person camera takes (0 = none, rock steady)");
    cvar_reg_float("cl_camera_socket_roll", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "how much of the head socket's roll the camera takes (0 = none)");
    cvar_reg_bool("cl_healthbars", false, CVAR_ARCHIVE, "health bars and names above characters / dummies (HUD code kept, hidden by default)");
    G->anim_rate = cvar_reg_int("anim_sample_rate", SIM_HZ, 30, 120, 0, "FBX bake sample rate (Hz); defaults to the simulation rate");
    g_engine.anim = (AnimLibrary *)calloc(1, sizeof(AnimLibrary));
    if (!anim_lib_load(g_engine.anim, (float)G->anim_rate->i)) {
        log_error("game: animation assets failed to load (characters fall back to capsules)");
        free(g_engine.anim);
        g_engine.anim = NULL;
    }
    anim_debug_init();
    if (g_engine.anim) movement_set_eye_offset(chars_eye_offset);
    if (g_engine.anim) attach_set_hold_params(&g_engine.anim->proc);   // scope rail positions use the styles' eye relief
    if (!items_load(&G->items, "data/items.json", "data/weapons")) log_error("game: item data failed to load");
    bal_world_load();
    g_engine.items = &G->items;
    basic_meshes_create(&G->meshes);
    fx_init(&G->meshes);
    flesh_fx_init();
    horde_init();
    hands_init();
    game_render_init();
    handpose_system_startup();   // items, attachments, vehicles and interactables are registered now
    game_fidelity_init();   // last: every graphics cvar is registered
}

void game_shutdown(void) {
    if (!G) return;
    net_stop();
    phone_shutdown();
    destroy_world();
    hands_shutdown();
    fx_shutdown();
    free(G);
    G = NULL;
}
