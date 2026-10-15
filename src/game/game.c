#include "game.h"
#include "game_internal.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "input/input.h"
#include "world/level.h"
#include "world/chunks.h"
#include "gameplay/entities.h"
#include "gameplay/health.h"
#include "gameplay/weapons.h"
#include "gameplay/effects.h"
#include "player/hands.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "net/net.h"
#include "audio/audio.h"
#include "ui/console.h"
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

// ------------------------------------------------------------------ players

static vec3 pick_spawn(World *w, u32 *index, float *yaw) {
    LevelStatic *ls = &w->level;
    if (w->streaming) {
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
    w->flags[i] &= ~(u32)EF_DEAD;
    ent_set_pos(w, p->ent, pos, quat_yaw(yaw), true);
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
    if (p->sim.move.mode == MOVE_DEAD) return;
    EntityId target = (EntityId)cmd->use_target;
    bool use_down = (cmd->buttons & BTN_USE) != 0;
    if ((pressed & BTN_USE) && interact_valid(w, slot, target)) {
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
    player_move_tick(w, slot, &p->sim, p->character, &c, dt, first);
    WeaponOutput wo;
    weapons_tick(w, slot, &p->sim, &c, pressed, dt, first, &wo);
    if (first && p->local && G) {
        G->view.yaw += wo.view_kick.x;
        G->view.pitch = clampf(G->view.pitch + wo.view_kick.y, -88.0f * DG_DEG2RAD, 88.0f * DG_DEG2RAD);
    }
    if (w->authority) interact_tick(w, slot, &c, pressed, dt);
    p->last_cmd = c;

    // Mirror onto the player entity (replicated to others).
    int i = (int)ENT_INDEX(p->ent);
    const PlayerMove *m = &p->sim.move;
    w->xf[i].pos = m->pos;
    w->xf[i].rot = quat_yaw(m->yaw);
    w->anim[i] = m->pitch;
    u32 f = w->flags[i] & (EF_DEAD | EF_HIDDEN);
    if (m->crouched) f |= EF_CROUCHED;
    if (m->sprinting) f |= EF_SPRINT;
    if (m->mode == MOVE_HANG) f |= EF_HANG;
    if (m->mode == MOVE_MANTLE || m->mode == MOVE_VAULT) f |= EF_MANTLE;
    if (p->sim.wpn.reloading) f |= EF_RELOADING;
    if (c.buttons & BTN_AIM) f |= EF_ADS;
    if (p->sim.wpn.recoil_kick > 0.8f) f |= EF_FIRING;
    w->flags[i] = f;
    const InvSlot *eq = weapon_equipped(w, slot);
    w->item[i] = eq ? eq->item : 0;
}

static void host_tick(World *w, const UserCmd *local_cmd) {
    world_store_prev(w);
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (!p->active) continue;
        UserCmd cmd;
        if (p->local) {
            cmd = *local_cmd;
        } else if (p->queue_count > 0) {
            cmd = p->queue[0];
            memmove(p->queue, p->queue + 1, sizeof(UserCmd) * (size_t)(p->queue_count - 1));
            p->queue_count--;
            // Too far behind (bursty network): drop extra commands to bound latency.
            while (p->queue_count > 6) {
                cmd = p->queue[0];
                memmove(p->queue, p->queue + 1, sizeof(UserCmd) * (size_t)(p->queue_count - 1));
                p->queue_count--;
            }
        } else {
            // No input arrived: repeat the last command without edge-triggered buttons.
            cmd = p->last_cmd;
            cmd.buttons &= (u16)~(BTN_JUMP | BTN_USE | BTN_RELOAD);
            cmd.buttons |= (u16)(p->sim.last_buttons & (BTN_JUMP | BTN_USE | BTN_RELOAD));
        }
        game_sim_player(w, s, &cmd, SIM_DT, true);
        if (!p->local && cmd.seq > p->last_processed_seq) p->last_processed_seq = cmd.seq;
        // Death / respawn
        int i = (int)ENT_INDEX(p->ent);
        if (w->flags[i] & EF_DEAD) {
            p->respawn_timer -= SIM_DT;
            if (p->respawn_timer <= 0) game_respawn_player(w, s);
        }
        // Fell out of the world.
        if (p->sim.move.pos.y < -60.0f && p->sim.move.mode != MOVE_NOCLIP) game_respawn_player(w, s);
    }
    entities_tick(w, SIM_DT);
    projectiles_tick(w, SIM_DT);
    if (w->chunks) chunks_update(w, SIM_DT);
    phys_step(w->phys, SIM_DT);
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
        bool to_local = G->local_slot >= 0 && (ev->target == EVENT_ALL || ev->target == G->local_slot) && ev->except != G->local_slot;
        if (to_local) game_handle_event(ev);
    }
    net_host_send_events(w);
    w->event_count = 0;
}

// ------------------------------------------------------------------ commands

static i8 axis_to_i8(float v) { return (i8)DG_CLAMP((int)lrintf(v * 127.0f), -127, 127); }

static UserCmd build_cmd(void) {
    UserCmd c;
    memset(&c, 0, sizeof(c));
    c.seq = ++G->client_seq;
    c.yaw = G->view.yaw;
    c.pitch = G->view.pitch;
    bool can_play = input_game_focus() && !G->menu_open && !G->inventory_open && !G->container_open && !console_is_open();
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
    if (input_down(ACT_AIM)) c.buttons |= BTN_AIM;
    if (input_down(ACT_RELOAD)) c.buttons |= BTN_RELOAD;
    // Edge presses shorter than a tick still register.
    if (input_consume_press(ACT_JUMP)) c.buttons |= BTN_JUMP;
    if (input_consume_press(ACT_USE)) c.buttons |= BTN_USE;
    if (input_consume_press(ACT_ATTACK)) c.buttons |= BTN_ATTACK;
    if (input_consume_press(ACT_RELOAD)) c.buttons |= BTN_RELOAD;
    if (input_consume_press(ACT_CROUCH)) c.buttons |= BTN_CROUCH;
    if (G->noclip_request) { c.buttons |= BTN_NOCLIP; G->noclip_request = false; }
    if (G->menu_open || G->inventory_open || G->container_open || console_is_open()) {
        c.move_x = c.move_y = 0;
        c.buttons &= (u16)(BTN_CROUCH | BTN_NOCLIP);
    }
    c.slot = G->hotbar_slot;
    c.use_target = G->focus;
    c.view_tick = net_client_view_tick256();
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
    vec3 after = p->sim.move.pos;
    float err = v3_dist(before, after);
    G->prediction_error = err;
    if (err < 1.0f) G->error_offset = v3_add(G->error_offset, v3_sub(before, after));
    else G->error_offset = v3(0, 0, 0);
}

static void tick(void) {
    UserCmd cmd = build_cmd();
    G->last_cmd = cmd;
    World *w = G->world;
    if (G->mode == GM_HOST) {
        host_tick(w, &cmd);
        dispatch_events(w);
        net_host_after_tick(w);
    } else if (G->mode == GM_CLIENT) {
        G->cmd_history[cmd.seq % CMD_HISTORY] = cmd;
        net_client_send_cmds(G->cmd_history, CMD_HISTORY, cmd.seq);
        Player *p = game_local_player();
        if (p && p->character) {
            G->predicted_prev = p->sim.move;
            game_sim_player(w, G->local_slot, &cmd, SIM_DT, true);
        }
        if (w->chunks) chunks_update(w, SIM_DT);
        w->tick++;
    }
}

// ------------------------------------------------------------------ focus

static void update_focus(void) {
    World *w = G->world;
    Player *p = game_local_player();
    G->focus = ENT_NONE;
    G->focus_prompt[0] = 0;
    if (!p || p->sim.move.mode == MOVE_DEAD) return;
    vec3 eye = G->view.eye;
    vec3 dir = dir_from_angles(G->view.yaw, G->view.pitch);
    float range = 3.0f;
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
    for (int i = 0; i < 9; i++)
        if (input_consume_press((Action)(ACT_SLOT1 + i))) G->hotbar_slot = (u8)i;
    if (input_consume_press(ACT_NEXT_ITEM)) G->hotbar_slot = (u8)((G->hotbar_slot + 1) % INV_HOTBAR);
    if (input_consume_press(ACT_PREV_ITEM)) G->hotbar_slot = (u8)((G->hotbar_slot + INV_HOTBAR - 1) % INV_HOTBAR);
    if (input_consume_press(ACT_DROP)) game_rpc(RPC_INV_DROP, G->hotbar_slot, 0, 0);
}

void game_frame(float dt) {
    if (!G->world) return;
    bool ui_blocking = G->menu_open || G->inventory_open || G->container_open || console_is_open();
    InputFrame f = input_frame();
    G->frame_look = ui_blocking ? v2(0, 0) : f.look_delta;
    camera_apply_look(&G->view, G->frame_look);
    if (!ui_blocking) handle_hotbar();
    G->use_pressed_frame = !ui_blocking && input_peek_press(ACT_USE);

    G->accumulator += dt;
    int steps = 0;
    while (G->accumulator >= SIM_DT && steps < 8) {
        tick();
        G->accumulator -= SIM_DT;
        steps++;
    }
    if (steps == 8) G->accumulator = 0; // spiral of death guard
    G->alpha = (float)(G->accumulator / SIM_DT);
    G->error_offset = v3_scale(G->error_offset, 1.0f - exp_decay(12.0f, dt));
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
    if (!strcmp(map, "streaming") || !strcmp(map, "terrain") || !strcmp(map, "flat")) {
        return chunks_create(w, !strcmp(map, "flat") ? CHUNKS_FLAT : CHUNKS_NOISE, (u32)G->world_seed->i);
    }
    char path[128];
    snprintf(path, sizeof(path), "data/levels/%s.json", map);
    return level_load(w, path);
}

bool game_host(const char *map) {
    net_stop();
    destroy_world();
    G->world = world_create(true, &G->items);
    dg_strcpy(G->map, sizeof(G->map), map);
    if (!load_map(G->world, map)) {
        log_error("game: failed to load map '%s', falling back to devroom", map);
        destroy_world();
        G->world = world_create(true, &G->items);
        dg_strcpy(G->map, sizeof(G->map), "devroom");
        if (!load_map(G->world, "devroom")) return false;
    }
    G->mode = GM_HOST;
    G->local_slot = 0;
    G->client_seq = 0;
    G->accumulator = 0;
    G->hotbar_slot = 0;
    memset(&G->view, 0, sizeof(G->view));
    game_spawn_player(G->world, 0, G->cl_name->str, true, -1);
    G->view.yaw = G->world->players[0].sim.move.yaw;
    if (G->net_autohost->i) net_host(TRANSPORT_LAN, (u16)cvar_find("net_port")->i);
    log_info("game: hosting '%s'", G->map);
    return true;
}

void game_cl_welcome(int slot, const char *map, u32 seed, u32 server_tick) {
    destroy_world();
    G->world = world_create(false, &G->items);
    dg_strcpy(G->map, sizeof(G->map), map);
    cvar_set_int(G->world_seed, (int)seed);
    if (!load_map(G->world, map)) log_error("client: cannot load map '%s'", map);
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
    char hint[64];
    snprintf(hint, sizeof(hint), "Disconnected: %s", reason ? reason : "");
    game_host(G->g_map->str);
    game_add_hint(hint);
}

int game_sv_player_join(int peer, const char *name) {
    World *w = G->world;
    if (!w || G->mode != GM_HOST) return -1;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        if (w->players[s].active) continue;
        game_spawn_player(w, s, name, false, peer);
        ev_hint(w, -1, "%s joined", name);
        return s;
    }
    return -1;
}

void game_sv_player_leave(int slot) {
    World *w = G->world;
    if (!w || slot < 0 || slot >= MAX_PLAYERS) return;
    char name[32];
    dg_strcpy(name, sizeof(name), w->players[slot].name);
    game_remove_player(w, slot);
    ev_hint(w, -1, "%s left", name);
}

void game_sv_receive_cmd(int slot, const UserCmd *cmd) {
    World *w = G->world;
    if (!w || slot < 0 || slot >= MAX_PLAYERS) return;
    Player *p = &w->players[slot];
    if (!p->active || cmd->seq <= p->last_processed_seq) return;
    for (int i = 0; i < p->queue_count; i++) if (p->queue[i].seq >= cmd->seq) return; // duplicate (redundant send)
    if (p->queue_count >= 32) { memmove(p->queue, p->queue + 1, sizeof(UserCmd) * 31); p->queue_count = 31; }
    p->queue[p->queue_count++] = *cmd;
}

// ------------------------------------------------------------------ console commands

static void cmd_map(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: map <devroom|streaming|flat>"); return; }
    cvar_set_cv(G->g_map, argv[1]);
    game_host(argv[1]);
}

static void cmd_give(int argc, const char **argv) {
    World *w = G->world;
    if (argc < 2 || G->mode != GM_HOST) { log_info("usage: give <item> [count] (host only)"); return; }
    u16 item = item_find(&G->items, argv[1]);
    if (!item) { log_warn("give: unknown item '%s'", argv[1]); return; }
    int count = argc > 2 ? atoi(argv[2]) : (G->items.items[item].max_stack > 1 ? G->items.items[item].max_stack : 1);
    const WeaponDef *wd = item_weapon(&G->items, item);
    Player *p = &w->players[G->local_slot];
    int left = inv_add(&p->inv, &G->items, item, count, wd ? (i16)wd->mag_size : 0);
    log_info("gave %d x %s", count - left, argv[1]);
}

static void cmd_noclip(int argc, const char **argv) { G->noclip_request = true; }

static void cmd_kill(int argc, const char **argv) {
    if (G->mode != GM_HOST) return;
    Player *p = game_local_player();
    if (p) apply_damage(G->world, p->ent, 10000, DMG_GENERIC, p->ent, p->sim.move.pos, v3(0, 0, 0), false);
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
    if (argc > 1) G->hotbar_slot = (u8)DG_CLAMP(atoi(argv[1]) - 1, 0, 8);
}

static void cmd_inv(int argc, const char **argv) {
    Player *p = game_local_player();
    if (!p) return;
    for (int i = 0; i < INV_SLOTS; i++) {
        InvSlot *s = &p->inv.slots[i];
        if (s->item) log_info("  slot %2d: %-14s x%d ammo %d", i, item_def(&G->items, s->item)->id, s->count, s->ammo);
    }
    Health *h = &G->world->health[ENT_INDEX(p->ent)];
    log_info("  hp %.0f armor %.0f, hotbar %d, focus %u", h->hp, h->armor, G->hotbar_slot + 1, G->focus);
}

static void cmd_lookat(int argc, const char **argv) {
    if (argc < 4) return;
    vec3 target = v3((float)atof(argv[1]), (float)atof(argv[2]), (float)atof(argv[3]));
    vec3 d = v3_sub(target, G->view.eye);
    G->view.yaw = atan2f(-d.x, -d.z);
    G->view.pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z));
}

static void cmd_status(int argc, const char **argv) {
    World *w = G->world;
    if (!w) return;
    log_info("map %s mode %s tick %u entities %d bodies %d", G->map, G->mode == GM_HOST ? "host" : "client", w->tick, w->max_used,
             phys_body_count(w->phys));
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (p->active) log_info("  slot %d %-16s %s peer %d hp %.0f", s, p->name, p->local ? "local" : "remote", p->peer, w->health[ENT_INDEX(p->ent)].hp);
    }
}

void game_init(void) {
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
    G->world_seed = cvar_reg_int("world_seed", 1337, 0, 999999999, CVAR_ARCHIVE, "procedural world seed");
    cmd_register("map", cmd_map, "host a new game on a map");
    cmd_register("give", cmd_give, "give <item> [count]");
    cmd_register("noclip", cmd_noclip, "toggle noclip (sv_cheats)");
    cmd_register("kill", cmd_kill, "suicide");
    cmd_register("setpos", cmd_setpos, "teleport: setpos x y z [yaw] [pitch]");
    cmd_register("getpos", cmd_getpos, "print position");
    cmd_register("setang", cmd_setang, "set view angles: setang yaw pitch");
    cmd_register("slot", cmd_slot, "select hotbar slot 1-9");
    cmd_register("status", cmd_status, "print game status");
    cmd_register("inv", cmd_inv, "print local inventory");
    cmd_register("lookat", cmd_lookat, "aim the view at a world point: lookat x y z");
    camera_register_cvars();
    player_register_cvars();
    if (!items_load(&G->items, "data/items.json", "data/weapons.json")) log_error("game: item data failed to load");
    g_engine.items = &G->items;
    basic_meshes_create(&G->meshes);
    fx_init(&G->meshes);
    hands_init();
    game_render_init();
}

void game_shutdown(void) {
    if (!G) return;
    net_stop();
    destroy_world();
    hands_shutdown();
    fx_shutdown();
    free(G);
    G = NULL;
}
