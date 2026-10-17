// Unit tests for the pure spectating core (src/net/spectate_core.c, docs/SPECTATE.md "Tests"):
// target rules, event routing, the forwarded command ring, the observer clock under lag / loss /
// jitter (delay bound, extrapolation, rewind), the wire blocks (every local-data field reaches the
// observer) and a 600-tick determinism check of the recoil / spread path (target vs observer
// rebuilt from an authoritative snapshot + replay + extrapolate-and-rewind).
#include "net/spectate.h"
#include "gameplay/events.h"
#include "gameplay/recoil.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail, g_checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

// ------------------------------------------------------------------ rules

static void test_rules(void) {
    SpecPlayerInfo pl[4];
    memset(pl, 0, sizeof(pl));
    for (int i = 0; i < 4; i++) { pl[i].active = true; pl[i].alive = true; }
    pl[0].alive = false;          // the observer
    pl[3].active = false;
    SpecRules r = { true, true, true };
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, 1, &r) == SPEC_OK, "dead observer -> living player");
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, 0, &r) == SPEC_ERR_SELF, "self");
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, 3, &r) == SPEC_ERR_TARGET, "inactive target");
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, 7, &r) == SPEC_ERR_TARGET, "out of range");
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, -1, &r) == SPEC_ERR_TARGET, "negative");
    CHECK(spec_validate(pl, 4, 0, 9, 1, &r) == SPEC_ERR_MODE, "bad mode");
    CHECK(spec_validate(pl, 4, 0, SPEC_FREE, -1, &r) == SPEC_OK, "free camera needs no target");
    CHECK(spec_validate(pl, 4, 1, SPEC_IN_EYE, 2, &r) == SPEC_ERR_ALIVE, "living players cannot spectate");
    pl[1].dev = true;
    CHECK(spec_validate(pl, 4, 1, SPEC_IN_EYE, 2, &r) == SPEC_OK, "dev may spectate alive");
    pl[1].dev = false;
    pl[2].alive = false;
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, 2, &r) == SPEC_ERR_DEAD, "dead target");
    pl[2].alive = true;
    pl[2].mode = SPEC_IN_EYE;
    CHECK(spec_validate(pl, 4, 0, SPEC_THIRD, 2, &r) == SPEC_ERR_SPECTATING, "spectating target");
    pl[2].mode = SPEC_OFF;
    pl[0].team = 1;
    pl[1].team = 2;
    r.fp_enemies = false;
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, 1, &r) == SPEC_ERR_ENEMY, "first person on an enemy");
    CHECK(spec_validate(pl, 4, 0, SPEC_THIRD, 1, &r) == SPEC_OK, "third person on an enemy is fine");
    pl[1].team = 1;
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, 1, &r) == SPEC_OK, "friend");
    r.allowed = false;
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, 1, &r) == SPEC_ERR_DISABLED, "disabled");
    CHECK(spec_validate(pl, 4, 0, SPEC_OFF, -1, &r) == SPEC_OK, "leaving is always allowed");
    r.allowed = true;
    r.round_allows = false;
    CHECK(spec_validate(pl, 4, 0, SPEC_IN_EYE, 1, &r) == SPEC_ERR_DISABLED, "pre-round");
    r.round_allows = true;
    pl[0].team = pl[1].team = 0;
    r.fp_enemies = true;
    // Cycling: 1, 2 (3 inactive, 0 self), wrapping both ways.
    CHECK(spec_cycle(pl, 4, 0, SPEC_IN_EYE, -1, 1, &r) == 1, "first next");
    CHECK(spec_cycle(pl, 4, 0, SPEC_IN_EYE, 1, 1, &r) == 2, "next");
    CHECK(spec_cycle(pl, 4, 0, SPEC_IN_EYE, 2, 1, &r) == 1, "next wraps");
    CHECK(spec_cycle(pl, 4, 0, SPEC_IN_EYE, 1, -1, &r) == 2, "prev wraps");
    CHECK(spec_cycle(pl, 4, 0, SPEC_FREE, 1, 1, &r) == 2, "cycling from the free camera picks a living player");
    pl[1].alive = pl[2].alive = false;
    CHECK(spec_cycle(pl, 4, 0, SPEC_IN_EYE, -1, 1, &r) == -1, "nobody alive");
}

// ------------------------------------------------------------------ events

static void test_events(void) {
    const u8 ALL = EVENT_ALL;
    // Plain client 2, not spectating.
    CHECK(spec_event_delivers(ALL, ALL, EV_KILL, 2, SPEC_NONE), "broadcast");
    CHECK(spec_event_delivers(2, ALL, EV_HURT, 2, SPEC_NONE), "own");
    CHECK(!spec_event_delivers(1, ALL, EV_HURT, 2, SPEC_NONE), "someone else's");
    CHECK(!spec_event_delivers(ALL, 2, EV_TRACER, 2, SPEC_NONE), "own predicted");
    CHECK(spec_event_delivers(ALL, 1, EV_TRACER, 2, SPEC_NONE), "another shooter's tracer");
    // Client 2 watching 1.
    CHECK(spec_event_delivers(1, ALL, EV_HURT, 2, 1), "target's hurt marker");
    CHECK(spec_event_delivers(1, ALL, EV_HIT_CONFIRM, 2, 1), "target's hit marker");
    CHECK(spec_event_delivers(1, ALL, EV_HINT, 2, 1), "target's hint");
    CHECK(!spec_event_delivers(1, ALL, EV_CONTAINER, 2, 1), "target's container window stays with the target");
    CHECK(!spec_event_delivers(1, ALL, EV_BENCH, 2, 1), "bench");
    CHECK(!spec_event_delivers(1, ALL, EV_RESPAWN, 2, 1), "respawn reset");
    CHECK(!spec_event_delivers(ALL, 1, EV_TRACER, 2, 1), "the observer predicts the target's tracers");
    CHECK(!spec_event_delivers(ALL, 1, EV_USE_ANIM, 2, 1), "and its use animation");
    CHECK(spec_event_delivers(ALL, 3, EV_TRACER, 2, 1), "a third player's tracer");
    CHECK(!spec_event_delivers(3, ALL, EV_HURT, 2, 1), "a third player's hurt marker");
}

// ------------------------------------------------------------------ ring

static UserCmd mk(u32 seq) {
    UserCmd c;
    memset(&c, 0, sizeof(c));
    c.seq = seq;
    c.move_y = (i8)(seq % 100);
    return c;
}

static void test_ring(void) {
    SpecCmdRing r;
    spec_ring_reset(&r);
    UserCmd c = mk(5);
    CHECK(spec_ring_put(&r, &c, 10), "put");
    CHECK(!spec_ring_put(&r, &c, 10), "duplicate");
    CHECK(spec_ring_get(&r, 5) && spec_ring_get(&r, 5)->move_y == 5, "get");
    CHECK(!spec_ring_get(&r, 6), "missing");
    CHECK(spec_ring_contiguous(&r, 4) == 5, "contiguous from 4");
    CHECK(spec_ring_contiguous(&r, 3) == 3, "gap at 4");
    for (u32 s = 6; s <= 9; s++) { c = mk(s); spec_ring_put(&r, &c, s * 16); }
    CHECK(spec_ring_contiguous(&r, 4) == 9, "contiguous to 9");
    CHECK(spec_ring_send_ms(&r, 7) == 7 * 16, "send stamp");
    c = mk(200);
    spec_ring_put(&r, &c, 0);
    c = mk(100);
    CHECK(!spec_ring_put(&r, &c, 0), "older than the ring");
    CHECK(!spec_ring_get(&r, 9 + SPEC_RING), "aliasing slot not confused");
    c = mk(0);
    CHECK(!spec_ring_put(&r, &c, 0), "seq 0 is never a command");
}

// ------------------------------------------------------------------ clock: delay under lag / loss / jitter

typedef struct Packet { int arrive; UserCmd cmd[SPEC_CMDS_PER_PACKET]; int count; } Packet;

static u32 g_lcg = 12345;
static float frand(void) { g_lcg = g_lcg * 1664525u + 1013904223u; return (float)(g_lcg >> 8) / 16777216.0f; }

// The target makes one command per tick and sends the last 4 (MSG_CMDS); the host relays them at
// once (MSG_SPEC_CMDS, 4 again). lag_ticks covers both hops; loss is per hop.
static void run_net(int lag_ticks, float loss, int jitter, int extrap_max, int ticks, int *max_lag, float *mean_lag, u32 *stalls, int *max_extrap) {
    static Packet q[4096];
    int qn = 0;
    SpecCmdRing ring;
    spec_ring_reset(&ring);
    SpecClock clk;
    memset(&clk, 0, sizeof(clk));
    *max_lag = 0;
    *max_extrap = 0;
    double sum = 0;
    int n = 0;
    for (int t = 1; t <= ticks; t++) {
        // Target tick t produces seq t and sends it (with the 3 before).
        bool lost = frand() < loss || frand() < loss;   // either hop
        if (!lost && qn < 4096) {
            Packet *p = &q[qn++];
            p->arrive = t + lag_ticks + (jitter ? (int)(frand() * (float)(jitter + 1)) : 0);
            p->count = 0;
            for (int k = 0; k < SPEC_CMDS_PER_PACKET; k++) if ((u32)t > (u32)k) p->cmd[p->count++] = mk((u32)t - (u32)k);
        }
        // Observer tick t: deliveries, then the clock.
        for (int i = 0; i < qn; i++) {
            if (q[i].arrive > t) continue;
            for (int k = 0; k < q[i].count; k++) spec_ring_put(&ring, &q[i].cmd[k], 0);
            q[i] = q[--qn];
            i--;
        }
        if (!clk.started) {
            if (ring.newest) spec_clock_reset(&clk, spec_ring_contiguous(&ring, 0));   // the switch snapshot / history
            continue;
        }
        spec_clock_step(&clk, spec_ring_contiguous(&ring, clk.sim_seq), extrap_max);
        // Snapshots (30 Hz, lossy like the relay): the host simulated the target up to about one
        // hop ago. Reconciling moves past commands that every redundant copy of was lost.
        int ack = t - lag_ticks - 1;
        if (t % 2 == 0 && ack > 0 && frand() >= loss) spec_clock_reconcile(&clk, (u32)ack);
        if (clk.extrap > *max_extrap) *max_extrap = clk.extrap;
        if (t > 120) {
            int lag = t - (int)clk.display;   // ticks between the target's present and what is shown
            if (lag > *max_lag) *max_lag = lag;
            sum += lag;
            n++;
        }
    }
    *mean_lag = n ? (float)(sum / n) : 0;
    *stalls = clk.stalls;
}

static void test_clock_delay(void) {
    int max_lag, max_ex;
    float mean;
    u32 stalls;
    // 80 ms fake lag each way on the target and the host = 2 x 80 ms = ~10 ticks; 3 % loss per hop.
    g_lcg = 777;
    run_net(10, 0.03f, 0, 6, 20000, &max_lag, &mean, &stalls, &max_ex);
    printf("  lag 10 ticks, 3%% loss: mean %.2f max %d ticks, stalls %u, max extrap %d\n", mean, max_lag, stalls, max_ex);
    CHECK(max_lag <= 10 + 1, "delay stays within both hops + 1 tick (max %d)", max_lag);
    CHECK(mean < 10.5f, "mean delay %.2f", mean);
    CHECK(max_ex <= 6, "extrapolation bounded");
    // Jitter up to 3 ticks: the clock rides the early edge, extrapolates over late packets.
    g_lcg = 99;
    run_net(10, 0.03f, 3, 6, 20000, &max_lag, &mean, &stalls, &max_ex);
    printf("  lag 10 +0..3 jitter, 3%% loss: mean %.2f max %d ticks, stalls %u, max extrap %d\n", mean, max_lag, stalls, max_ex);
    CHECK(max_lag <= 10 + 3 + 1, "jitter: delay %d", max_lag);
    CHECK(max_ex <= 6, "jitter: extrapolation bounded");
    // A long outage: extrapolation runs out, the target holds, then catches up two per tick.
    g_lcg = 5;
    run_net(4, 0.45f, 0, 2, 4000, &max_lag, &mean, &stalls, &max_ex);
    CHECK(max_ex <= 2, "outage: extrapolation limit honoured");
    CHECK(stalls > 0, "outage: the target is held");
    CHECK(mean < 12.0f, "outage: catches up (mean %.2f)", mean);
}

// ------------------------------------------------------------------ clock: rewind contract

// A toy simulation whose state depends on the order of every command: the observer, following the
// SpecStep contract (rewind, real commands, extrapolation, reconcile replays), must land on the
// exact state the target had for every confirmed seq.
typedef struct Toy { u64 h; u32 n; } Toy;
static void toy_step(Toy *s, const UserCmd *c) { s->h = s->h * 1099511628211ull ^ (u64)(c->seq * 31u + (u32)(c->move_y + 128) + c->buttons); s->n++; }

static void test_clock_rewind(void) {
    enum { T = 600 };
    static Toy truth[T + 1];
    static UserCmd cmds[T + 1];
    memset(&truth[0], 0, sizeof(Toy));
    for (u32 s = 1; s <= T; s++) {
        cmds[s] = mk(s);
        cmds[s].buttons = (u16)((s % 7 == 0) ? BTN_ATTACK : 0);
        truth[s] = truth[s - 1];
        toy_step(&truth[s], &cmds[s]);
    }
    SpecCmdRing ring;
    spec_ring_reset(&ring);
    SpecClock clk;
    spec_clock_reset(&clk, 0);
    Toy cur = { 0, 0 }, confirmed = cur;
    int arrive_at[T + 1];
    g_lcg = 4242;
    for (u32 s = 1; s <= T; s++) arrive_at[s] = (int)s + 3 + (frand() < 0.2f ? (int)(frand() * 6) : 0);   // late bursts
    int mismatches = 0, extrap_used = 0, rewinds = 0;
    for (int t = 1; t <= T + 20; t++) {
        for (u32 s = 1; s <= T; s++) if (arrive_at[s] == t) spec_ring_put(&ring, &cmds[s], 0);
        SpecStep st = spec_clock_step(&clk, spec_ring_contiguous(&ring, clk.sim_seq), 4);
        if (st.rewind) { cur = confirmed; rewinds++; }
        for (u32 s = st.real_from; s <= st.real_to; s++) toy_step(&cur, spec_ring_get(&ring, s));
        if (st.real_to >= st.real_from) {
            confirmed = cur;
            if (memcmp(&confirmed, &truth[clk.sim_seq], sizeof(Toy))) mismatches++;
        }
        for (int k = 0; k < st.extrap_ticks; k++) {
            UserCmd e = spec_extrap_cmd(spec_ring_get(&ring, clk.sim_seq) ? spec_ring_get(&ring, clk.sim_seq) : &cmds[1]);
            toy_step(&cur, &e);
            extrap_used++;
        }
        // An authoritative snapshot every 2nd tick for a seq a few ticks back.
        if (t % 2 == 0 && clk.sim_seq > 6) {
            u32 ack = clk.sim_seq - 5;
            SpecStep rc = spec_clock_reconcile(&clk, ack);
            cur = truth[ack];   // restore the host's state
            for (u32 s = rc.real_from; s <= rc.real_to; s++) toy_step(&cur, spec_ring_get(&ring, s));
            confirmed = cur;
            if (memcmp(&confirmed, &truth[clk.sim_seq], sizeof(Toy))) mismatches++;
            for (int k = 0; k < rc.extrap_ticks; k++) { UserCmd e = spec_extrap_cmd(spec_ring_get(&ring, clk.sim_seq)); toy_step(&cur, &e); }
        }
    }
    CHECK(mismatches == 0, "confirmed state equals the target's at every seq (%d mismatches)", mismatches);
    CHECK(clk.sim_seq == T, "all commands simulated (%u)", clk.sim_seq);
    CHECK(extrap_used > 0 && rewinds > 0, "the run exercised extrapolation (%d) and rewinds (%d)", extrap_used, rewinds);
    UserCmd last = mk(9);
    last.buttons = BTN_ATTACK | BTN_AIM | BTN_JUMP | BTN_CROUCH | BTN_USE;
    UserCmd e = spec_extrap_cmd(&last);
    CHECK(e.buttons == (BTN_AIM | BTN_CROUCH), "extrapolation keeps held states, drops actions (%x)", e.buttons);
}

// ------------------------------------------------------------------ wire: every local-data field reaches the observer

typedef struct Field { const char *name; size_t off, size; } Field;
#define F(path) { #path, offsetof(SpecLocalData, path), sizeof(((SpecLocalData *)0)->path) }

// Every field of the local data, enumerated. A new PlayerSim field fails the coverage check below
// until it is listed here (and DG_PROTOCOL_VERSION is bumped).
static const Field k_fields[] = {
    F(ack_seq), F(hold_progress), F(open_container), F(sim.last_buttons), F(sim.pad),
    F(sim.move.pos), F(sim.move.vel), F(sim.move.yaw), F(sim.move.pitch), F(sim.move.body_yaw), F(sim.move.body_turning), F(sim.move.pad_body),
    F(sim.move.mode), F(sim.move.on_ground), F(sim.move.crouched), F(sim.move.sprinting), F(sim.move.crouch_toggled), F(sim.move.sprint_toggled),
    F(sim.move.ground_steep), F(sim.move.pad0), F(sim.move.coyote), F(sim.move.jump_buffer), F(sim.move.crouch_amount), F(sim.move.air_time),
    F(sim.move.land_speed), F(sim.move.land_tick), F(sim.move.fall_start_y), F(sim.move.m_start), F(sim.move.m_mid), F(sim.move.m_end),
    F(sim.move.m_t), F(sim.move.m_dur), F(sim.move.m_end_crouched), F(sim.move.m_kind), F(sim.move.pad1), F(sim.move.hang_pos),
    F(sim.move.hang_normal), F(sim.move.ledge_point), F(sim.move.hang_time), F(sim.move.regrab_cooldown), F(sim.move.hand_anchor),
    F(sim.move.hand_anchor_valid), F(sim.move.pad2), F(sim.move.vehicle), F(sim.move.seat), F(sim.move.veh_onehand), F(sim.move.veh_raise),
    F(sim.move.pad3), F(sim.move.veh_stun), F(sim.move.stride),
    F(sim.move.water), F(sim.move.head_under), F(sim.move.swim_sprint), F(sim.move.pad4), F(sim.move.water_depth), F(sim.move.water_surface),
    F(sim.move.water_column), F(sim.move.oxygen), F(sim.move.stamina), F(sim.move.wet), F(sim.move.swim_phase),
    F(sim.wpn.slot), F(sim.wpn.prev_slot), F(sim.wpn.reloading), F(sim.wpn.swing_hit_done), F(sim.wpn.cooldown), F(sim.wpn.reload_t),
    F(sim.wpn.swing_t), F(sim.wpn.use_t), F(sim.wpn.equip_t), F(sim.wpn.shots), F(sim.wpn.recoil_kick), F(sim.wpn.mag), F(sim.wpn.fire_mode),
    F(sim.wpn.burst_left), F(sim.wpn.ads_t), F(sim.wpn.since_sprint), F(sim.wpn.breath), F(sim.wpn.breath_holding), F(sim.wpn.breath_exhausted),
    F(sim.wpn.ads_on), F(sim.wpn.item), F(sim.wpn.sway_k),
    F(sim.wpn.rc.mag_seed), F(sim.wpn.rc.shot_index), F(sim.wpn.rc.recovering), F(sim.wpn.rc.have_prev), F(sim.wpn.rc.acc_pitch),
    F(sim.wpn.rc.acc_yaw), F(sim.wpn.rc.countered_pitch), F(sim.wpn.rc.countered_yaw), F(sim.wpn.rc.pool_pitch), F(sim.wpn.rc.pool_yaw),
    F(sim.wpn.rc.pool_start), F(sim.wpn.rc.since_shot), F(sim.wpn.rc.bloom), F(sim.wpn.rc.prev_cmd_pitch), F(sim.wpn.rc.prev_cmd_yaw),
    F(sim.wpn.rc.emitted_pitch), F(sim.wpn.rc.emitted_yaw), F(sim.wpn.rc.ads_t),
};

static void test_wire(void) {
    u8 buf[4096];
    // Distinct byte pattern in every field, decoded into a poisoned struct: each field must match.
    SpecLocalData src, dst;
    for (size_t i = 0; i < sizeof(src); i++) ((u8 *)&src)[i] = (u8)(i * 7 + 3);
    Writer wr = writer_make(buf, sizeof(buf));
    spec_write_local(&wr, &src);
    memset(&dst, 0xCD, sizeof(dst));
    Reader rd = reader_make(buf, wr.len);
    spec_read_local(&rd, &dst);
    CHECK(!rd.overflow && rd.pos == wr.len, "local data decodes exactly");
    int nf = (int)(sizeof(k_fields) / sizeof(k_fields[0]));
    int bad = 0;
    static u8 covered[sizeof(SpecLocalData)];
    for (int k = 0; k < nf; k++) {
        if (memcmp((u8 *)&src + k_fields[k].off, (u8 *)&dst + k_fields[k].off, k_fields[k].size)) { printf("  field %s lost\n", k_fields[k].name); bad++; }
        for (size_t b = 0; b < k_fields[k].size; b++) covered[k_fields[k].off + b]++;
    }
    CHECK(bad == 0, "%d of %d fields did not reach the observer", bad, nf);
    // Coverage: bytes not in the table may only be alignment padding (< 4 bytes before an aligned field).
    int uncovered_run = 0, worst = 0, overlap = 0;
    for (size_t i = 0; i < sizeof(SpecLocalData); i++) {
        if (covered[i] > 1) overlap++;
        if (!covered[i]) uncovered_run++;
        else { if (uncovered_run > worst) worst = uncovered_run; uncovered_run = 0; }
    }
    if (uncovered_run > worst) worst = uncovered_run;
    CHECK(overlap == 0, "field table overlaps itself");
    CHECK(worst < 4, "an unlisted field in the local data (%d uncovered bytes): add it to k_fields", worst);

    SpecBlock b = { 3, SPEC_IN_EYE, 1, SPECF_FORCED | SPECF_CAN_RESPAWN, 25, 2, 41, 7 }, b2;
    memset(&b2, 0xEE, sizeof(b2));
    wr = writer_make(buf, sizeof(buf));
    spec_write_block(&wr, &b);
    rd = reader_make(buf, wr.len);
    spec_read_block(&rd, &b2);
    CHECK(!rd.overflow && !memcmp(&b, &b2, sizeof(b)), "spec block round trip");

    // Relay: a switch burst of the last SPEC_HISTORY commands, then redundant packets.
    SpecCmdRing host, obs;
    spec_ring_reset(&host);
    spec_ring_reset(&obs);
    for (u32 s = 1; s <= 50; s++) { UserCmd c = mk(s); spec_ring_put(&host, &c, s * 3); }
    wr = writer_make(buf, sizeof(buf));
    spec_write_cmds(&wr, 2, &host, 1, 50);
    rd = reader_make(buf, wr.len);
    int got = 0;
    CHECK(spec_read_cmds(&rd, 2, &obs, &got) == 2 && got == SPEC_HISTORY, "history burst (%d)", got);
    CHECK(spec_ring_contiguous(&obs, 50 - SPEC_HISTORY) == 50, "history contiguous");
    CHECK(spec_ring_send_ms(&obs, 40) == 120, "stamps travel");
    rd = reader_make(buf, wr.len);
    CHECK(spec_read_cmds(&rd, 5, &obs, &got) == 2 && got == 0, "another target's relay is ignored");
    rd = reader_make(buf, 5);
    CHECK(spec_read_cmds(&rd, 2, &obs, &got) == SPEC_NONE, "truncated relay rejected");
}

// ------------------------------------------------------------------ determinism: target vs observer, 600 ticks

typedef struct GunSim { RecoilParams p; RecoilState s; float view_pitch, view_yaw; float spread; } GunSim;

static UserCmd stream_cmd(u32 seq) {
    UserCmd c = mk(seq);
    u32 h = (seq * 2654435761u) >> 7;
    // Bursts of fire, aim in and out, a mouse that drags down against the climb.
    bool fire = (seq / 40) % 3 != 2;
    c.buttons = (u16)((fire ? BTN_ATTACK : 0) | (((seq / 90) & 1) ? BTN_AIM : 0));
    c.pitch = -0.002f * (float)(h % 7) * (fire ? 1.0f : 0.0f);
    c.yaw = 0.0015f * (float)((int)(h % 5) - 2);
    return c;
}

// One tick of the deterministic gun state (the PlayerSim part of weapons_tick that recoil.c owns).
static void gun_tick(GunSim *g, const UserCmd *c) {
    RecoilMods m;
    memset(&m, 0, sizeof(m));
    m.fire_mode = FM_AUTO;
    m.extra = 1;
    m.scale = 1;
    m.ads_t = (c->buttons & BTN_AIM) ? 1.0f : 0.0f;
    g->s.ads_t = m.ads_t;
    g->view_pitch += c->pitch * 57.29578f;
    g->view_yaw += c->yaw * 57.29578f;
    recoil_observe_input(&g->p, &g->s, g->view_pitch, g->view_yaw);
    RecoilKick total = { 0, 0 };
    if ((c->buttons & BTN_ATTACK) && (c->seq % 6) == 0) {   // 600 rpm at 60 Hz
        RecoilKick k = recoil_shot(&g->p, &g->s, &m);
        total.pitch += k.pitch;
        total.yaw += k.yaw;
    }
    RecoilKick r = recoil_tick(&g->p, &g->s, 1.0f / 60.0f);
    total.pitch += r.pitch;
    total.yaw += r.yaw;
    recoil_set_emitted(&g->s, total);
    g->view_pitch += total.pitch;
    g->view_yaw += total.yaw;
    g->spread = recoil_spread(&g->p, &g->s, &m);
}

static void test_determinism(void) {
    enum { T = 600 };
    static GunSim target[T + 1];
    memset(&target[0], 0, sizeof(GunSim));
    recoil_params_default(&target[0].p);
    recoil_reset(&target[0].s, recoil_mag_seed(1, 2));
    for (u32 s = 1; s <= T; s++) { target[s] = target[s - 1]; UserCmd c = stream_cmd(s); gun_tick(&target[s], &c); }
    // Observer: starts from the host's snapshot at seq 200, follows the relayed commands, every 9th
    // tick extrapolates 3 ticks then rewinds (late commands), every 25th reconciles to the truth.
    GunSim cur = target[200], confirmed = cur;
    int diffs = 0;
    for (u32 s = 201; s <= T; s++) {
        if (s % 9 == 0) {
            UserCmd last = stream_cmd(s - 1);
            for (int k = 0; k < 3; k++) { UserCmd e = spec_extrap_cmd(&last); e.seq = last.seq + 1 + (u32)k; gun_tick(&cur, &e); }
            cur = confirmed;   // the real command arrived: rewind
        }
        UserCmd c = stream_cmd(s);
        gun_tick(&cur, &c);
        confirmed = cur;
        if (s % 25 == 0) {
            GunSim snap = target[s - 4];   // authoritative state 4 ticks back + replay
            for (u32 r = s - 3; r <= s; r++) { UserCmd rc = stream_cmd(r); gun_tick(&snap, &rc); }
            if (memcmp(&snap.s, &cur.s, sizeof(RecoilState))) diffs++;
            cur = confirmed = snap;
        }
        bool same = !memcmp(&cur.s, &target[s].s, sizeof(RecoilState)) && cur.view_pitch == target[s].view_pitch &&
                    cur.view_yaw == target[s].view_yaw && cur.spread == target[s].spread;
        if (!same) diffs++;
    }
    CHECK(diffs == 0, "600 ticks: observer recoil / view / spread bit-identical to the target (%d ticks differ)", diffs);
    CHECK(target[T].s.shot_index > 0 || target[T].s.acc_pitch != 0 || target[T - 50].s.bloom > 0, "the stream fired");
}

int main(void) {
    test_rules();
    test_events();
    test_ring();
    test_clock_delay();
    test_clock_rewind();
    test_wire();
    test_determinism();
    printf("test_spectate: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
