// Spectating, pure part (docs/SPECTATE.md): target rules, event routing, the forwarded command
// ring, the observer clock (simulate / extrapolate / rewind) and the wire blocks. No engine state:
// tests/test_spectate.c runs all of it.
#include "spectate.h"
#include "gameplay/events.h"

const char *spec_result_name(SpecResult r) {
    static const char *names[] = { "ok", "spectating is disabled", "only dead players can spectate", "no such player", "cannot spectate yourself",
                                   "that player is dead", "that player is spectating", "first person on enemies is disabled", "bad mode" };
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

const char *spec_mode_name(int mode) {
    static const char *names[] = { "off", "first person", "third person", "free camera" };
    return mode >= 0 && mode < SPEC_MODE_COUNT ? names[mode] : "?";
}

SpecResult spec_validate(const SpecPlayerInfo *pl, int n, int obs, int mode, int target, const SpecRules *r) {
    if (mode < 0 || mode >= SPEC_MODE_COUNT) return SPEC_ERR_MODE;
    if (mode == SPEC_OFF) return SPEC_OK;
    if (obs < 0 || obs >= n || !pl[obs].active) return SPEC_ERR_TARGET;
    if (!r->allowed || !r->round_allows) return SPEC_ERR_DISABLED;
    if (pl[obs].alive && !pl[obs].dev) return SPEC_ERR_ALIVE;
    if (mode == SPEC_FREE) return SPEC_OK;
    if (target < 0 || target >= n || !pl[target].active) return SPEC_ERR_TARGET;
    if (target == obs) return SPEC_ERR_SELF;
    if (!pl[target].alive) return SPEC_ERR_DEAD;
    if (pl[target].mode != SPEC_OFF) return SPEC_ERR_SPECTATING;
    bool enemy = pl[obs].team != pl[target].team && pl[obs].team && pl[target].team;
    if (mode == SPEC_IN_EYE && enemy && !r->fp_enemies) return SPEC_ERR_ENEMY;
    return SPEC_OK;
}

int spec_cycle(const SpecPlayerInfo *pl, int n, int obs, int mode, int from, int dir, const SpecRules *r) {
    if (n <= 0) return -1;
    if (mode == SPEC_FREE || mode == SPEC_OFF) mode = SPEC_IN_EYE;
    dir = dir < 0 ? -1 : 1;
    int start = from >= 0 && from < n ? from : (dir > 0 ? -1 : n);
    for (int k = 1; k <= n; k++) {
        int s = ((start + dir * k) % n + n) % n;
        if (spec_validate(pl, n, obs, mode, s, r) == SPEC_OK) return s;
    }
    return -1;
}

// Events about a player that its observers see as well (HUD and sound feedback). Container and
// bench windows, respawn resets and other UI stay with the player.
static bool forwarded_type(u8 type) {
    return type == EV_HINT || type == EV_HIT_CONFIRM || type == EV_HURT || type == EV_SOUND || type == EV_CHAT;
}

bool spec_event_delivers(u8 ev_target, u8 ev_except, u8 ev_type, int s, int spec_target) {
    bool watching = spec_target != SPEC_NONE && spec_target >= 0;
    bool to_me = ev_target == EVENT_ALL || ev_target == s;
    bool via_target = watching && ev_target == spec_target && forwarded_type(ev_type);
    if (!to_me && !via_target) return false;
    if (ev_except == s) return false;
    // The watched player predicted it (own shots, use animation): so did this observer.
    if (watching && ev_except == spec_target) return false;
    return true;
}

// ------------------------------------------------------------------ command ring

void spec_ring_reset(SpecCmdRing *r) { memset(r, 0, sizeof(*r)); }

bool spec_ring_put(SpecCmdRing *r, const UserCmd *c, u32 send_ms) {
    if (!c->seq) return false;
    if (r->newest >= SPEC_RING && c->seq <= r->newest - SPEC_RING) return false;   // older than the ring
    u32 k = c->seq & (SPEC_RING - 1);
    if (r->cmd[k].seq == c->seq) return false;   // duplicate (redundant send)
    // A slot still holding a newer command than this one: this one is stale.
    if (r->cmd[k].seq > c->seq) return false;
    r->cmd[k] = *c;
    r->send_ms[k] = send_ms;
    if (c->seq > r->newest) r->newest = c->seq;
    return true;
}

const UserCmd *spec_ring_get(const SpecCmdRing *r, u32 seq) {
    if (!seq) return NULL;
    const UserCmd *c = &r->cmd[seq & (SPEC_RING - 1)];
    return c->seq == seq ? c : NULL;
}

u32 spec_ring_send_ms(const SpecCmdRing *r, u32 seq) {
    return spec_ring_get(r, seq) ? r->send_ms[seq & (SPEC_RING - 1)] : 0;
}

u32 spec_ring_contiguous(const SpecCmdRing *r, u32 from) {
    u32 s = from;
    for (int k = 0; k < SPEC_RING && spec_ring_get(r, s + 1); k++) s++;
    return s;
}

// ------------------------------------------------------------------ observer clock

static SpecStep step_none(void) {
    SpecStep s;
    memset(&s, 0, sizeof(s));
    s.real_from = 1;
    s.real_to = 0;
    return s;
}

void spec_clock_reset(SpecClock *c, u32 ack_seq) {
    memset(c, 0, sizeof(*c));
    c->display = c->sim_seq = ack_seq;
    c->started = true;
}

SpecStep spec_clock_step(SpecClock *c, u32 avail, int extrap_max) {
    SpecStep s = step_none();
    if (!c->started) return s;
    if (extrap_max < 0) extrap_max = 0;
    c->display++;
    // A backlog (a late burst after a stall): catch up two per tick, jump when far behind. Same
    // rule as the host's command queue, so the observer never drifts behind the target.
    if (avail > c->display + 16) { c->display = avail - 2; c->catchups++; }
    else if (avail > c->display + 2) { c->display++; c->catchups++; }
    u32 real_to = DG_MIN(avail, c->display);
    if (real_to > c->sim_seq) {
        s.rewind = c->extrap > 0;
        s.real_from = c->sim_seq + 1;
        s.real_to = real_to;
        c->sim_seq = real_to;
        c->extrap = 0;
    }
    u32 want = c->display - c->sim_seq;
    if (want > (u32)extrap_max) {
        // Out of extrapolation: hold the target until its commands arrive.
        want = (u32)extrap_max;
        c->display = c->sim_seq + want;
        c->stalls++;
    }
    s.extrap_ticks = DG_MAX(0, (int)want - c->extrap);
    c->extrap = (int)want;
    return s;
}

SpecStep spec_clock_reconcile(SpecClock *c, u32 ack_seq) {
    SpecStep s = step_none();
    s.rewind = true;   // restore the authoritative state
    if (!c->started) { spec_clock_reset(c, ack_seq); return s; }
    if (ack_seq >= c->sim_seq) {
        // The host is at or ahead of what was simulated (relayed commands lost / late).
        c->sim_seq = ack_seq;
        if (c->display < ack_seq) c->display = ack_seq;
        u32 e = DG_MIN(c->display - c->sim_seq, (u32)c->extrap);
        c->display = c->sim_seq + e;
        c->extrap = (int)e;
        s.extrap_ticks = (int)e;
        return s;
    }
    s.real_from = ack_seq + 1;   // replays (not first simulations)
    s.real_to = c->sim_seq;
    s.extrap_ticks = c->extrap;
    return s;
}

UserCmd spec_extrap_cmd(const UserCmd *last) {
    UserCmd c = *last;
    c.buttons &= (u16) ~(BTN_JUMP | BTN_ATTACK | BTN_USE | BTN_RELOAD | BTN_FIREMODE | BTN_VEHICLE | BTN_NOCLIP);
    c.seat_req = 0;
    return c;
}

// ------------------------------------------------------------------ wire blocks

void spec_write_local(Writer *wr, const SpecLocalData *d) {
    w_u32(wr, d->ack_seq);
    w_bytes(wr, &d->sim, sizeof(PlayerSim));
    w_f32(wr, d->hold_progress);
    w_u32(wr, d->open_container);
}

void spec_read_local(Reader *rd, SpecLocalData *d) {
    d->ack_seq = r_u32(rd);
    r_bytes(rd, &d->sim, sizeof(PlayerSim));
    d->hold_progress = r_f32(rd);
    d->open_container = r_u32(rd);
}

void spec_write_block(Writer *wr, const SpecBlock *b) {
    w_u8(wr, b->data_slot);
    w_u8(wr, b->mode);
    w_u8(wr, b->target);
    w_u8(wr, b->flags);
    w_u8(wr, b->respawn_ds);
    w_u8(wr, b->killer);
    w_u16(wr, b->target_owd_ms);
    w_u32(wr, b->seq);
}

void spec_read_block(Reader *rd, SpecBlock *b) {
    b->data_slot = r_u8(rd);
    b->mode = r_u8(rd);
    b->target = r_u8(rd);
    b->flags = r_u8(rd);
    b->respawn_ds = r_u8(rd);
    b->killer = r_u8(rd);
    b->target_owd_ms = r_u16(rd);
    b->seq = r_u32(rd);
}

void spec_write_cmds(Writer *wr, u8 target, const SpecCmdRing *ring, u32 from, u32 to) {
    if (from == 0) from = 1;
    if (to >= from && to - from >= SPEC_HISTORY) from = to - SPEC_HISTORY + 1;
    w_u8(wr, target);
    u32 count_at = wr->len;
    w_u8(wr, 0);
    u8 n = 0;
    for (u32 s = from; s <= to && to >= from; s++) {
        const UserCmd *c = spec_ring_get(ring, s);
        if (!c) continue;
        w_bytes(wr, c, sizeof(UserCmd));
        w_u32(wr, spec_ring_send_ms(ring, s));
        n++;
    }
    if (!wr->overflow) wr->buf[count_at] = n;
}

int spec_read_cmds(Reader *rd, int expect_target, SpecCmdRing *ring, int *received) {
    int target = r_u8(rd);
    int n = r_u8(rd);
    if (received) *received = 0;
    if (target != expect_target) return rd->overflow ? SPEC_NONE : target;   // an old target's relay: ignored
    int got = 0;
    for (int k = 0; k < n && k < SPEC_HISTORY; k++) {
        UserCmd c;
        r_bytes(rd, &c, sizeof(c));
        u32 ms = r_u32(rd);
        if (rd->overflow) return SPEC_NONE;
        if (spec_ring_put(ring, &c, ms)) got++;
    }
    if (received) *received = got;
    return rd->overflow ? SPEC_NONE : target;
}
