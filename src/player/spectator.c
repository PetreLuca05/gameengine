// Observer client (docs/SPECTATE.md "Observer client pipeline"): the watched player is re-simulated
// through the local-player path from its relayed commands and presented as this machine's view
// slot; the free camera; spectator controls; the death camera -> auto spectate flow; the delay
// readout and the determinism dump (spec_dump).
#include "spectator.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "input/input.h"
#include "net/net.h"
#include "net/replication.h"
#include "physics/physics.h"
#include "gameplay/characters.h"
#include "gameplay/weapons.h"
#include "anim/anim_library.h"
#include "ui/console.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define G (g_engine.game)

typedef struct Spectator {
    // This machine's spectating state, as the host confirmed it (snapshot block / host table).
    int mode, target;
    u32 seq;
    u8 flags, respawn_ds, killer;
    u16 target_owd_ms;
    // The watched player's replica (clients): its own character controller, re-simulated.
    int sim_slot;
    bool ready;
    SpecCmdRing ring;
    SpecClock clk;
    PlayerSim confirmed;         // state after clk.sim_seq (before extrapolation)
    UserCmd last_real;           // last relayed command simulated
    float real_yaw, real_pitch;  // view after it (command + recoil kick)
    UserCmd view_cmd;            // last command simulated (real or extrapolated)
    PlayerMove prev_move;        // render interpolation: the state one tick ago
    float yaw[2], pitch[2];      // view angles one tick ago / now (command + recoil kick)
    bool use_press;              // a use press of the watched player waiting for the hands (frame)
    vec2 kick;                   // recoil view kick of command kick_seq (game_sim_player)
    u32 kick_seq;
    vec3 error_offset;           // smoothed visual correction (rewinds / reconciliation)
    float frame_yaw, frame_pitch;
    vec2 frame_look;
    // stats
    float delay_meas_ms, delay_est_ms, max_pop;
    u32 pops, rewinds, sims;
    double stats_time;
    // death flow
    float dead_time;
    bool auto_sent;
    // free camera
    vec3 free_pos;
    bool free_init;
    double free_sent;
    // target switch inertialization
    bool blend_on;
    vec3 blend_pos;
    float blend_yaw, blend_pitch, blend_t;
    bool cam_valid;
    vec3 cam_pos;
    float cam_yaw, cam_pitch;
    Cvar *death_cam_time, *show_delay, *extrap, *blend_time, *pop_warn, *free_speed, *free_collide, *dump;
} Spectator;

static Spectator SP;

static bool watch_mode(int m) { return m == SPEC_IN_EYE || m == SPEC_THIRD; }
static float lerp_ang(float a, float b, float t) { return wrap_angle(a + wrap_angle(b - a) * t); }
static u32 now_ms(void) { return (u32)(platform_time() * 1000.0); }

static Player *slot_player(World *w, int s) {
    if (!w || s < 0 || s >= MAX_PLAYERS || !w->players[s].active || !ent_valid(w, w->players[s].ent)) return NULL;
    return &w->players[s];
}

// ------------------------------------------------------------------ requests

static void request(int mode, int target_req, bool dev) { game_rpc(RPC_SPECTATE, mode, target_req, dev ? 1 : 0); }

// ------------------------------------------------------------------ replica (clients)

static void restore(World *w, Player *p, const PlayerSim *sim) {
    p->sim = *sim;
    if (!p->character) return;
    phys_character_set_crouched(w->phys, p->character, sim->move.crouched != 0);
    phys_character_set_pos(p->character, sim->move.pos);
    phys_character_set_vel(p->character, sim->move.vel);
}

static void demote(World *w) {
    if (SP.sim_slot < 0) return;
    Player *p = w ? slot_player(w, SP.sim_slot) : NULL;
    if (p && !p->local) {
        // Back to a snapshot-interpolated remote player with its kinematic capsule.
        if (p->character) phys_character_destroy(w->phys, p->character);
        p->character = NULL;
        repl_player_body(w, (int)ENT_INDEX(p->ent), true);
    }
    SP.sim_slot = -1;
}

static bool promote(World *w, int slot) {
    Player *p = slot_player(w, slot);
    if (!p || p->local) return false;
    // Its own character controller (as the local player): the replica moves through the world
    // exactly as on the target's machine. The kinematic capsule would collide with it.
    repl_player_body(w, (int)ENT_INDEX(p->ent), false);
    game_create_player_character(w, slot, p->sim.move.pos);
    SP.sim_slot = slot;
    return p->character != NULL;
}

// Presentation state of the view (camera smoothing, head stabilizer, recoil springs) belongs to
// whoever is viewed: a switch starts it fresh.
static void reset_presentation(void) {
    G->view.smooth_init = false;
    memset(&G->cam_stab, 0, sizeof(G->cam_stab));
    memset(&G->recoil_vis, 0, sizeof(G->recoil_vis));
    G->spread_prev = G->spread_cur = 0;
    G->hitmarker_time = 0;
    G->damage_flash = 0;
    for (int i = 0; i < MAX_HURT_MARKERS; i++) G->hurt[i].time = 0;
    SP.error_offset = v3(0, 0, 0);
}

static void begin_switch(World *w) {
    if (SP.cam_valid) {
        SP.blend_on = true;
        SP.blend_t = 0;
        SP.blend_pos = SP.cam_pos;
        SP.blend_yaw = SP.cam_yaw;
        SP.blend_pitch = SP.cam_pitch;
    }
    SP.ready = false;
    demote(w);
    spec_ring_reset(&SP.ring);
    memset(&SP.clk, 0, sizeof(SP.clk));
    SP.max_pop = 0;
    reset_presentation();
}

static void sim_one(World *w, int slot, const UserCmd *c, bool first, bool anim) {
    game_sim_player(w, slot, c, SIM_DT, first);
    if (anim) chars_local_predict(w, slot, true);
    SP.view_cmd = *c;
    vec2 k = SP.kick_seq == c->seq ? SP.kick : v2(0, 0);
    // The target's camera after this tick: the command's angles plus this tick's recoil kick
    // (what the target's view holds after the same tick; its next command carries it).
    SP.yaw[1] = wrap_angle(c->yaw + k.x);
    SP.pitch[1] = clampf(c->pitch + k.y, -88.0f * DG_DEG2RAD, 88.0f * DG_DEG2RAD);
    SP.sims++;
}

static void extrapolate(World *w, int slot, int ticks, int already) {
    for (int k = 0; k < ticks; k++) {
        // The last command, held: its view including the recoil kick (what the target's next
        // command would carry without mouse movement), no new presses.
        UserCmd e = spec_extrap_cmd(&SP.last_real);
        e.yaw = SP.real_yaw;
        e.pitch = SP.real_pitch;
        e.seq = SP.clk.sim_seq + (u32)(already + k + 1);   // sway time keeps running
        sim_one(w, slot, &e, false, false);
    }
}

static void note_pop(vec3 expect, vec3 got) {
    float d = v3_dist(expect, got);
    if (d > SP.max_pop) SP.max_pop = d;
    if (SP.pop_warn && d > SP.pop_warn->f) {
        SP.pops++;
        log_debug("spectate: correction %.3f m (seq %u)", d, SP.clk.sim_seq);
    }
    if (d < 1.0f) SP.error_offset = v3_add(SP.error_offset, v3_sub(expect, got));
    else SP.error_offset = v3(0, 0, 0);
}

void spectator_cl_local_data(World *w, int slot, const SpecLocalData *d, const SpecBlock *b) {
    (void)b;
    if (!w || w->authority || slot != SP.target || !watch_mode(SP.mode)) return;
    Player *p = slot_player(w, slot);
    if (!p) return;
    if (SP.sim_slot != slot) {
        demote(w);
        if (!promote(w, slot)) return;
    }
    p->hold_progress = d->hold_progress;
    if (!SP.ready) {
        // First state of this target: start the replica here; the relayed history (sent right
        // after this snapshot) carries it to the target's present on the next ticks.
        restore(w, p, &d->sim);
        spec_clock_reset(&SP.clk, d->ack_seq);
        SP.confirmed = p->sim;
        SP.prev_move = p->sim.move;
        SP.yaw[0] = SP.yaw[1] = p->sim.move.yaw;
        SP.pitch[0] = SP.pitch[1] = p->sim.move.pitch;
        memset(&SP.last_real, 0, sizeof(SP.last_real));
        SP.last_real.seq = d->ack_seq;
        SP.last_real.yaw = p->sim.move.yaw;
        SP.last_real.pitch = p->sim.move.pitch;
        SP.last_real.slot = p->sim.wpn.slot < INV_HOTBAR ? p->sim.wpn.slot : 0;
        SP.real_yaw = SP.last_real.yaw;
        SP.real_pitch = SP.last_real.pitch;
        SP.view_cmd = SP.last_real;
        SP.ready = true;
        reset_presentation();
        return;
    }
    // Reconcile exactly like the local player: the host's state at ack, then the commands after it.
    vec3 before = p->sim.move.pos;
    SpecStep st = spec_clock_reconcile(&SP.clk, d->ack_seq);
    restore(w, p, &d->sim);
    for (u32 s = st.real_from; s <= st.real_to; s++) {
        const UserCmd *c = spec_ring_get(&SP.ring, s);
        if (c) sim_one(w, slot, c, false, false);
    }
    SP.real_yaw = SP.yaw[1];
    SP.real_pitch = SP.pitch[1];
    SP.confirmed = p->sim;
    extrapolate(w, slot, st.extrap_ticks, 0);
    note_pop(before, p->sim.move.pos);
}

void spectator_cl_cmds(World *w, Reader *r) {
    (void)w;
    // Relays for an earlier target (in flight across a switch) are ignored by the reader.
    spec_read_cmds(r, SP.target, &SP.ring, NULL);
}

void spectator_cl_inventory(World *w, int slot, u32 revision, const InvSlot *slots) {
    Player *p = slot_player(w, slot);
    if (!p || p->local) return;
    memcpy(p->inv.slots, slots, sizeof(p->inv.slots));
    p->inv.revision = revision;
}

static void apply_block(World *w, const SpecBlock *b) {
    int target = b->target == SPEC_NONE ? -1 : b->target;
    int was_watching = watch_mode(SP.mode) ? SP.target : -1;
    int now_watching = watch_mode(b->mode) ? target : -1;
    if (b->seq != SP.seq || now_watching != was_watching) {
        if (now_watching >= 0 || was_watching >= 0) begin_switch(w);
    }
    if (b->mode == SPEC_FREE && SP.mode != SPEC_FREE) SP.free_init = false;
    SP.mode = b->mode;
    SP.target = target;
    SP.seq = b->seq;
    SP.flags = b->flags;
    SP.respawn_ds = b->respawn_ds;
    SP.killer = b->killer;
    SP.target_owd_ms = b->target_owd_ms;
    if (!watch_mode(SP.mode) && SP.sim_slot >= 0) demote(w);
}

void spectator_cl_block(World *w, const SpecBlock *b, u32 tick) {
    (void)tick;
    apply_block(w, b);
}

// ------------------------------------------------------------------ per tick

// Host: the observer table is right here; the watched player is authoritative (no replica).
static void host_tick(World *w) {
    int local = G->local_slot;
    if (local < 0 || !spectate_observer(local)) return;
    SpecBlock b;
    spectate_fill_block(w, local, &b);
    b.target_owd_ms = net_player_owd_ms(b.target == SPEC_NONE ? -1 : b.target);
    apply_block(w, &b);
    Player *p = watch_mode(SP.mode) ? slot_player(w, SP.target) : NULL;
    if (!p) { SP.ready = false; return; }
    if (!SP.ready) {
        SP.ready = true;
        SP.yaw[1] = p->last_cmd.yaw;
        SP.pitch[1] = p->last_cmd.pitch;
    }
    SP.yaw[0] = SP.yaw[1];
    SP.pitch[0] = SP.pitch[1];
    vec2 k = SP.kick_seq == p->last_cmd.seq ? SP.kick : v2(0, 0);
    SP.yaw[1] = wrap_angle(p->last_cmd.yaw + k.x);
    SP.pitch[1] = clampf(p->last_cmd.pitch + k.y, -88.0f * DG_DEG2RAD, 88.0f * DG_DEG2RAD);
    if ((p->last_cmd.buttons & BTN_USE) && !(SP.view_cmd.buttons & BTN_USE)) SP.use_press = true;
    SP.view_cmd = p->last_cmd;
    SP.delay_est_ms = (float)b.target_owd_ms + SIM_DT * 500.0f;
}

void spectator_tick(World *w) {
    if (!w || !G) return;
    if (w->authority) { host_tick(w); return; }
    if (!SP.ready || SP.sim_slot < 0 || !watch_mode(SP.mode)) return;
    int slot = SP.sim_slot;
    Player *p = slot_player(w, slot);
    if (!p || !p->character) { SP.ready = false; return; }
    SP.prev_move = p->sim.move;
    SP.yaw[0] = SP.yaw[1];
    SP.pitch[0] = SP.pitch[1];
    vec3 before = p->sim.move.pos, vel = p->sim.move.vel;
    int extrap_max = SP.extrap ? SP.extrap->i : 6;
    SpecStep st = spec_clock_step(&SP.clk, spec_ring_contiguous(&SP.ring, SP.clk.sim_seq), extrap_max);
    if (st.rewind) {
        // The real commands arrived: drop the extrapolated ticks and simulate what really happened.
        restore(w, p, &SP.confirmed);
        SP.rewinds++;
    }
    for (u32 s = st.real_from; s <= st.real_to; s++) {
        const UserCmd *c = spec_ring_get(&SP.ring, s);
        if (!c) continue;
        // Effects (shots, kicks, sounds) for the newest two only when catching up a backlog; the
        // animation steps once per command like on the target.
        bool first = s + 1 >= st.real_to;
        if ((c->buttons & BTN_USE) && !(SP.last_real.buttons & BTN_USE)) SP.use_press = true;
        sim_one(w, slot, c, first, true);
        SP.last_real = *c;
        SP.real_yaw = SP.yaw[1];
        SP.real_pitch = SP.pitch[1];
        if (first) {
            u32 sent = spec_ring_send_ms(&SP.ring, s);
            float d = sent ? (float)(u32)(now_ms() - sent) : 0.0f;
            if (sent && d < 5000.0f) SP.delay_meas_ms = SP.delay_meas_ms > 0 ? SP.delay_meas_ms * 0.9f + d * 0.1f : d;
        }
    }
    if (st.real_to >= st.real_from) SP.confirmed = p->sim;
    extrapolate(w, slot, st.extrap_ticks, SP.clk.extrap - st.extrap_ticks);
    if (st.rewind) note_pop(v3_madd(before, vel, SIM_DT), p->sim.move.pos);
    // Estimated input -> screen delay: target -> host, host -> here, the backlog, half a tick.
    NetStats ns = net_stats();
    u32 avail = spec_ring_contiguous(&SP.ring, SP.clk.sim_seq);
    float backlog = (float)(avail > SP.clk.display ? avail - SP.clk.display : 0) * SIM_DT * 1000.0f;
    float est = (float)SP.target_owd_ms + ns.app_rtt_ms * 0.5f + backlog + SIM_DT * 500.0f;
    SP.delay_est_ms = SP.delay_est_ms > 0 ? SP.delay_est_ms * 0.95f + est * 0.05f : est;
    if (SP.dump && SP.dump->i && platform_time() - SP.stats_time > 2.0) {
        SP.stats_time = platform_time();
        log_info("spec_stats: target %d delay measured %.1f ms estimate %.1f ms, max correction %.3f m, rewinds %u, corrections > %.2f m: %u, stalls %u, catchups %u, extrap %d",
                 SP.target, SP.delay_meas_ms, SP.delay_est_ms, SP.max_pop, SP.rewinds, SP.pop_warn ? SP.pop_warn->f : 0.0f, SP.pops, SP.clk.stalls,
                 SP.clk.catchups, SP.clk.extrap);
    }
}

void spectator_note_kick(int slot, u32 seq, vec2 kick) {
    if (slot != SP.target) return;
    SP.kick = kick;
    SP.kick_seq = seq;
}

// ------------------------------------------------------------------ determinism dump

static u32 fnv(u32 h, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static void dump_slot(World *w, const char *who, int slot, u32 seq) {
    Player *p = slot_player(w, slot);
    if (!p) return;
    vec3 eye = player_eye_pos(&p->sim.move);
    u32 pose = 2166136261u;
    CharacterInst *c = char_get(w, p->ent);
    if (c && c->pose_cur && g_engine.anim) pose = fnv(pose, c->pose_cur, sizeof(Xform) * (size_t)g_engine.anim->skel.count);
    const InvSlot *eq = weapon_equipped(w, slot);
    const Health *h = &w->health[ENT_INDEX(p->ent)];
    log_info("spec_dump %s slot %d seq %u sim %08x move %08x wpn %08x rc %08x eye %.5f %.5f %.5f spread %.5f mag %d ammo %d hp %.1f slot %d reload %.4f use %.4f pose %08x",
             who, slot, seq, fnv(2166136261u, &p->sim, sizeof(PlayerSim)), fnv(2166136261u, &p->sim.move, sizeof(PlayerMove)),
             fnv(2166136261u, &p->sim.wpn, sizeof(WeaponState)), fnv(2166136261u, &p->sim.wpn.rc, sizeof(RecoilState)), eye.x, eye.y, eye.z,
             weapon_current_spread(w, slot), p->sim.wpn.mag, eq ? eq->ammo : 0, h->hp, p->sim.wpn.slot, p->sim.wpn.reload_t, p->sim.wpn.use_t, pose);
}

void spectator_dump_tick(World *w, u32 local_seq) {
    if (!SP.dump || !SP.dump->i || !w || !G) return;
    // The predicting player itself (the target's process), and the watched replica (the observer's)
    // whenever its state is a confirmed command (not extrapolated): tools/tests/spec_compare.py
    // matches the two logs by seq.
    if (spectator_view_slot() < 0) dump_slot(w, "local", G->local_slot, local_seq);
    else if (!w->authority && SP.clk.extrap == 0 && SP.clk.started) dump_slot(w, "observer", SP.target, SP.clk.sim_seq);
}

// ------------------------------------------------------------------ per frame: controls, death flow, free camera

static bool local_dead(void) {
    Player *lp = game_local_player();
    if (!lp) return false;
    return lp->sim.move.mode == MOVE_DEAD || (G->world->flags[ENT_INDEX(lp->ent)] & EF_DEAD);
}

static bool ui_blocking(void) {
    return G->menu_open || G->inventory_open || G->container_open || G->bench_open || console_is_open();
}

bool spectator_controls_input(void) { return SP.mode != SPEC_OFF; }

// Players a number key picks: living, in slot order.
static int nth_candidate(int n) {
    World *w = G->world;
    int k = 0;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = slot_player(w, s);
        if (!p || s == G->local_slot || (w->flags[ENT_INDEX(p->ent)] & EF_DEAD)) continue;
        if (k++ == n) return s;
    }
    return -1;
}

static void free_camera_move(float dt) {
    World *w = G->world;
    if (!SP.free_init) {
        SP.free_pos = SP.cam_valid ? SP.cam_pos : v3_add(G->view.eye, v3(0, 1.0f, 0));
        SP.free_init = true;
    }
    if (ui_blocking()) return;
    InputFrame f = input_frame();
    vec3 fwd = dir_from_angles(G->view.yaw, G->view.pitch), right = right_from_yaw(G->view.yaw);
    vec3 mv = v3_add(v3_scale(fwd, f.move.y), v3_scale(right, f.move.x));
    if (input_down(ACT_JUMP)) mv.y += 1.0f;
    if (input_down(ACT_CROUCH)) mv.y -= 1.0f;
    float speed = (SP.free_speed ? SP.free_speed->f : 8.0f) * (input_down(ACT_SPRINT) ? 3.0f : 1.0f);
    vec3 delta = v3_scale(mv, speed * dt);
    if (v3_len2(delta) > 1e-10f && SP.free_collide && SP.free_collide->i) {
        // Collision on: a sphere slides along walls (two passes), never through them.
        for (int pass = 0; pass < 2 && v3_len2(delta) > 1e-10f; pass++) {
            ShapeHit h;
            if (!phys_sphere_cast(w->phys, SP.free_pos, delta, 0.25f, PL_MASK_WORLD, PHYS_INVALID_BODY, &h)) break;
            float keep = fmaxf(0.0f, h.fraction - 0.02f);
            SP.free_pos = v3_madd(SP.free_pos, delta, keep);
            vec3 rest = v3_scale(delta, 1.0f - keep);
            delta = v3_sub(rest, v3_scale(h.normal, v3_dot(rest, h.normal)));
        }
    }
    SP.free_pos = v3_add(SP.free_pos, delta);
    // The host streams the world around the camera (relevance), 4 times a second.
    if (platform_time() - SP.free_sent > 0.25) {
        SP.free_sent = platform_time();
        game_rpc(RPC_SPEC_POS, (int)lrintf(SP.free_pos.x * 16.0f), (int)lrintf(SP.free_pos.y * 16.0f), (int)lrintf(SP.free_pos.z * 16.0f));
    }
}

void spectator_frame(float dt) {
    World *w = G ? G->world : NULL;
    if (!w) return;
    float tsm = camera_fp_cvars()->recon_smooth ? camera_fp_cvars()->recon_smooth->f : 0.08f;
    SP.error_offset = tsm > 1e-4f ? v3_scale(SP.error_offset, 1.0f - exp_decay(3.0f / tsm, dt)) : v3(0, 0, 0);
    bool dead = local_dead();
    if (!dead) { SP.dead_time = 0; SP.auto_sent = false; }
    else SP.dead_time += dt;
    bool ui = ui_blocking();
    bool can_respawn = (SP.flags & SPECF_CAN_RESPAWN) != 0;
    if (dead && can_respawn && !ui && input_consume_press(ACT_RELOAD)) game_rpc(RPC_RESPAWN, 0, 0, 0);
    if (SP.mode == SPEC_OFF) {
        if (!dead || ui) return;
        // Death camera on the ragdoll's head, then the killer's (or the nearest player's) eyes.
        // Not worth it for a respawn that is about to happen anyway; a click skips the wait.
        bool skip = input_consume_press(ACT_ATTACK);
        float left = SP.respawn_ds == 255 ? 1e9f : (float)SP.respawn_ds * 0.1f;
        float cam_t = SP.death_cam_time ? SP.death_cam_time->f : 3.0f;
        if (!SP.auto_sent && left > 1.5f && (SP.dead_time >= cam_t || skip)) {
            request(SPEC_IN_EYE, SPEC_REQ_AUTO, false);
            SP.auto_sent = true;
        }
        return;
    }
    if (SP.mode == SPEC_FREE) free_camera_move(dt);
    if (ui) return;
    int mode = SP.mode == SPEC_FREE ? SPEC_IN_EYE : SP.mode;
    if (input_consume_press(ACT_ATTACK) || input_consume_press(ACT_NEXT_ITEM)) request(mode, SPEC_REQ_NEXT, false);
    if (input_consume_press(ACT_AIM) || input_consume_press(ACT_PREV_ITEM)) request(mode, SPEC_REQ_PREV, false);
    if (input_consume_press(ACT_USE)) {
        // First person -> third person -> free camera -> first person.
        int next = SP.mode == SPEC_IN_EYE ? SPEC_THIRD : (SP.mode == SPEC_THIRD ? SPEC_FREE : SPEC_IN_EYE);
        request(next, SPEC_REQ_KEEP, false);
    }
    for (int i = 0; i < 8; i++) {
        if (!input_consume_press((Action)(ACT_SLOT1 + i))) continue;
        int s = nth_candidate(i);
        if (s >= 0) request(mode, s, false);
    }
}

// ------------------------------------------------------------------ presentation

int spectator_mode(void) { return SP.mode; }
int spectator_target(void) { return SP.target; }

int spectator_view_slot(void) {
    if (!G || !G->world || !watch_mode(SP.mode) || !SP.ready) return -1;
    if (!slot_player(G->world, SP.target)) return -1;
    if (!G->world->authority && SP.sim_slot != SP.target) return -1;
    return SP.target;
}

bool spectator_first_person(void) { return SP.mode == SPEC_IN_EYE && spectator_view_slot() >= 0; }

bool spectator_view_interp(PlayerMove *out, float alpha) {
    int s = spectator_view_slot();
    if (s < 0) return false;
    World *w = G->world;
    Player *p = &w->players[s];
    *out = p->sim.move;
    if (w->authority) {
        out->pos = v3_lerp(w->xf[ENT_INDEX(p->ent)].prev_pos, p->sim.move.pos, alpha);
        return true;
    }
    out->pos = v3_add(v3_lerp(SP.prev_move.pos, p->sim.move.pos, alpha), SP.error_offset);
    out->crouch_amount = lerpf(SP.prev_move.crouch_amount, p->sim.move.crouch_amount, alpha);
    out->stride = lerpf(SP.prev_move.stride, p->sim.move.stride, alpha);
    return true;
}

bool spectator_view_angles(float alpha, float *yaw, float *pitch) {
    if (spectator_view_slot() < 0) return false;
    *yaw = lerp_ang(SP.yaw[0], SP.yaw[1], saturate(alpha));
    *pitch = lerpf(SP.pitch[0], SP.pitch[1], saturate(alpha));
    SP.frame_look = v2(wrap_angle(*yaw - SP.frame_yaw), *pitch - SP.frame_pitch);
    SP.frame_yaw = *yaw;
    SP.frame_pitch = *pitch;
    return true;
}

const UserCmd *spectator_view_cmd(void) { return spectator_view_slot() >= 0 ? &SP.view_cmd : NULL; }
vec2 spectator_frame_look(void) { return SP.frame_look; }

bool spectator_take_use_press(void) {
    bool p = SP.use_press;
    SP.use_press = false;
    return p;
}

bool spectator_free_camera(RenderCamera *cam) {
    if (SP.mode != SPEC_FREE || !SP.free_init) return false;
    cam->pos = SP.free_pos;
    cam->yaw = G->view.yaw;
    cam->pitch = G->view.pitch;
    cam->roll = 0;
    return true;
}

void spectator_blend_camera(RenderCamera *cam, float dt) {
    if (SP.blend_on) {
        SP.blend_t += dt;
        float bt = SP.blend_time ? SP.blend_time->f : 0.15f;
        float k = bt > 1e-4f ? smoothstepf(0.0f, 1.0f, SP.blend_t / bt) : 1.0f;
        cam->pos = v3_lerp(SP.blend_pos, cam->pos, k);
        cam->yaw = lerp_ang(SP.blend_yaw, cam->yaw, k);
        cam->pitch = lerpf(SP.blend_pitch, cam->pitch, k);
        if (k >= 1.0f) SP.blend_on = false;
    }
    SP.cam_valid = true;
    SP.cam_pos = cam->pos;
    SP.cam_yaw = cam->yaw;
    SP.cam_pitch = cam->pitch;
}

float spectator_delay_ms(void) { return SP.delay_est_ms; }

bool spectator_hud(SpectatorHud *o) {
    memset(o, 0, sizeof(*o));
    if (!G || !G->world) return false;
    World *w = G->world;
    o->mode = SP.mode;
    o->target = SP.target;
    o->dead = local_dead();
    Player *t = slot_player(w, SP.target);
    if (t) dg_strcpy(o->name, sizeof(o->name), t->name);
    o->delay_ms = SP.delay_est_ms;
    o->show_delay = SP.show_delay && SP.show_delay->i && watch_mode(SP.mode) && net_role() != NET_OFF;
    o->can_respawn = (SP.flags & SPECF_CAN_RESPAWN) != 0;
    o->waiting_respawn = SP.respawn_ds == 255;
    o->respawn_s = SP.respawn_ds == 255 ? 0.0f : (float)SP.respawn_ds * 0.1f;
    o->first_person_denied = (SP.flags & SPECF_FP_DENIED) != 0;
    float cam_t = SP.death_cam_time ? SP.death_cam_time->f : 3.0f;
    o->death_cam_left = SP.mode == SPEC_OFF && o->dead ? fmaxf(0.0f, cam_t - SP.dead_time) : 0.0f;
    return SP.mode != SPEC_OFF || o->dead;
}

// ------------------------------------------------------------------ setup / commands

// The re-simulated player is "owned" here: snapshot interpolation leaves its transform alone.
static bool owned_hook(World *w, int i) {
    return !w->authority && SP.sim_slot >= 0 && w->type[i] == ET_PLAYER && w->player_slot[i] == SP.sim_slot;
}

void spectator_reset(void) {
    Cvar *c[8] = { SP.death_cam_time, SP.show_delay, SP.extrap, SP.blend_time, SP.pop_warn, SP.free_speed, SP.free_collide, SP.dump };
    memset(&SP, 0, sizeof(SP));
    SP.death_cam_time = c[0]; SP.show_delay = c[1]; SP.extrap = c[2]; SP.blend_time = c[3];
    SP.pop_warn = c[4]; SP.free_speed = c[5]; SP.free_collide = c[6]; SP.dump = c[7];
    SP.target = -1;
    SP.sim_slot = -1;
}

static int find_player(const char *arg) {
    World *w = G->world;
    if (!w) return -1;
    char *end;
    long n = strtol(arg, &end, 10);
    if (*end == 0 && n >= 0 && n < MAX_PLAYERS) return (int)n;
    for (int s = 0; s < MAX_PLAYERS; s++) if (w->players[s].active && !strcmp(w->players[s].name, arg)) return s;
    return -2;
}

static int parse_mode(const char *a, int fallback) {
    if (!a) return fallback;
    if (!strcmp(a, "eye") || !strcmp(a, "first")) return SPEC_IN_EYE;
    if (!strcmp(a, "third")) return SPEC_THIRD;
    if (!strcmp(a, "free")) return SPEC_FREE;
    return fallback;
}

// spectate <player|next|prev|off|free> [eye|third|free]: dev / host spectating without dying
// (clients: sv_cheats). Dead players use the same requests through the spectator controls.
static void cmd_spectate(int argc, const char **argv) {
    if (!G || !G->world) return;
    if (argc < 2) {
        log_info("usage: spectate <player name|slot|next|prev|off|free> [eye|third|free]   (now: %s, target %d, delay %.0f ms)", spec_mode_name(SP.mode), SP.target, SP.delay_est_ms);
        return;
    }
    const char *a = argv[1];
    int mode = parse_mode(argc > 2 ? argv[2] : NULL, SP.mode == SPEC_THIRD ? SPEC_THIRD : SPEC_IN_EYE);
    if (!strcmp(a, "off")) { request(SPEC_OFF, -1, true); return; }
    if (!strcmp(a, "free")) { request(SPEC_FREE, -1, true); return; }
    if (!strcmp(a, "next")) { request(mode, SPEC_REQ_NEXT, true); return; }
    if (!strcmp(a, "prev")) { request(mode, SPEC_REQ_PREV, true); return; }
    if (!strcmp(a, "auto")) { request(mode, SPEC_REQ_AUTO, true); return; }
    int s = find_player(a);
    if (s < 0) { log_warn("spectate: no player '%s'", a); return; }
    request(mode, s, true);
}

static void cmd_respawn(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    game_rpc(RPC_RESPAWN, 0, 0, 0);
}

void spectator_init(void) {
    SP.death_cam_time = cvar_reg_float("spec_death_cam_time", 3.0f, 0.0f, 30.0f, CVAR_ARCHIVE, "seconds the death camera stays on your body before spectating the killer / nearest player");
    SP.show_delay = cvar_reg_bool("spec_show_delay", true, CVAR_ARCHIVE, "show the spectator delay readout (input of the watched player -> your screen)");
    SP.extrap = cvar_reg_int("spec_extrap_ticks", 6, 0, 30, CVAR_ARCHIVE, "ticks a watched player is extrapolated with its last input while its commands are late");
    SP.blend_time = cvar_reg_float("spec_blend_time", 0.15f, 0.0f, 1.0f, CVAR_ARCHIVE, "camera blend (s) when the spectated player changes");
    SP.pop_warn = cvar_reg_float("spec_pop_warn", 0.05f, 0.0f, 5.0f, 0, "spectator corrections above this (m) are counted / logged (developer)");
    SP.free_speed = cvar_reg_float("spec_free_speed", 8.0f, 0.5f, 100.0f, CVAR_ARCHIVE, "spectator free camera speed (m/s, sprint x3)");
    SP.free_collide = cvar_reg_bool("spec_free_collide", true, CVAR_ARCHIVE, "spectator free camera collides with the world");
    SP.dump = cvar_reg_int("spec_dump", 0, 0, 1, 0, "log a per-tick state hash of the local / watched player (determinism test, tools/tests/spec_compare.py)");
    cmd_register("spectate", cmd_spectate, "spectate <player|next|prev|off|free|auto> [eye|third|free]: watch a player (host / sv_cheats while alive)");
    cmd_register("respawn", cmd_respawn, "respawn now when the server allows it (sv_respawn_mode 1)");
    repl_set_owned_hook(owned_hook);
    spectator_reset();
}
