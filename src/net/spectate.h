// Spectating (docs/SPECTATE.md): a dead player (or dev) watches a living player in first person,
// CS:GO "in-eye" style, but from the target's own forwarded commands instead of interpolated
// snapshots. The host keeps one SpecObserver per slot; everything it sends a player about itself
// (the snapshot's local data, inventory, own events) also goes to that player's in-eye observers,
// and its raw commands are relayed to them the moment they arrive.
//
// The first half of this header is pure (no World, no engine state: tests/test_spectate.c); the
// second half is the host integration (src/net/spectate.c). The observer client is
// src/player/spectator.c.
#ifndef DG_SPECTATE_H
#define DG_SPECTATE_H

#include "core/base.h"
#include "game/usercmd.h"
#include "player/player.h"
#include "net/bitbuf.h"

typedef enum SpecMode { SPEC_OFF, SPEC_IN_EYE, SPEC_THIRD, SPEC_FREE, SPEC_MODE_COUNT } SpecMode;

#define SPEC_NONE 0xFF          // no target
#define SPEC_RING 64            // forwarded commands kept per target (power of two)
#define SPEC_HISTORY 32         // commands sent with a target switch (the view starts at once)
#define SPEC_CMDS_PER_PACKET 4  // redundancy of a relay packet (as MSG_CMDS)

// Target request codes in RPC_SPECTATE b (a = mode): >= 0 a slot.
enum SpecTargetReq { SPEC_REQ_NEXT = -1, SPEC_REQ_PREV = -2, SPEC_REQ_AUTO = -3, SPEC_REQ_KEEP = -4 };

// ------------------------------------------------------------------ rules (pure)

typedef struct SpecPlayerInfo {
    bool active;
    bool alive;
    bool dev;        // may spectate while alive (host's own player with the spectate command, sv_cheats)
    u8 team;         // 0 = no teams (everyone friendly)
    u8 mode;         // SpecMode it is in (a spectator is never a target)
} SpecPlayerInfo;

typedef struct SpecRules {
    bool allowed;        // sv_spec_allowed
    bool fp_enemies;     // sv_spec_fp_enemies: first person on another team's players
    bool round_allows;   // false before a round starts (no rounds yet: always true)
} SpecRules;

typedef enum SpecResult {
    SPEC_OK,
    SPEC_ERR_DISABLED,   // spectating off on this server / pre-round
    SPEC_ERR_ALIVE,      // the observer is alive (and not dev)
    SPEC_ERR_TARGET,     // no such player
    SPEC_ERR_SELF,
    SPEC_ERR_DEAD,       // the target is dead
    SPEC_ERR_SPECTATING, // the target is itself spectating
    SPEC_ERR_ENEMY,      // first person on an enemy is disabled
    SPEC_ERR_MODE,
} SpecResult;

const char *spec_result_name(SpecResult r);
const char *spec_mode_name(int mode);
// Is (mode, target) legal for observer obs? The free camera and SPEC_OFF need no target.
SpecResult spec_validate(const SpecPlayerInfo *pl, int n, int obs, int mode, int target, const SpecRules *r);
// Next (dir +1) / previous (-1) legal target after `from` (wrapping), or -1.
int spec_cycle(const SpecPlayerInfo *pl, int n, int obs, int mode, int from, int dir, const SpecRules *r);

// ------------------------------------------------------------------ event routing (pure)

// Does a client in slot s receive a GameEvent (target / except / type)? spec_target is the slot it
// watches in first or third person (it re-simulates that player, so it predicts what the player
// predicts), else SPEC_NONE.
bool spec_event_delivers(u8 ev_target, u8 ev_except, u8 ev_type, int s, int spec_target);

// ------------------------------------------------------------------ forwarded commands (pure)

typedef struct SpecCmdRing {
    UserCmd cmd[SPEC_RING];
    u32 send_ms[SPEC_RING];   // the target's send time stamp (same-clock delay measurement)
    u32 newest;               // highest seq stored
} SpecCmdRing;

void spec_ring_reset(SpecCmdRing *r);
// Stores c (by seq). False for duplicates and commands too old for the ring.
bool spec_ring_put(SpecCmdRing *r, const UserCmd *c, u32 send_ms);
const UserCmd *spec_ring_get(const SpecCmdRing *r, u32 seq);
u32 spec_ring_send_ms(const SpecCmdRing *r, u32 seq);
// Highest seq s >= from with every command from+1 .. s present (from when none).
u32 spec_ring_contiguous(const SpecCmdRing *r, u32 from);

// Observer clock: which of the target's commands this observer tick simulates.
//   display   the target timeline position shown (one step per observer tick)
//   sim_seq   the last real command simulated (the confirmed state)
//   extrap    ticks extrapolated beyond sim_seq with the last command
typedef struct SpecClock {
    u32 display, sim_seq;
    int extrap;
    bool started;
    u32 stalls;          // ticks the target was held (extrapolation used up)
    u32 catchups;        // ticks that advanced 2 (backlog) or jumped
} SpecClock;

typedef struct SpecStep {
    bool rewind;         // restore the confirmed state (drop the extrapolated ticks) before the real commands
    u32 real_from, real_to;   // real commands to simulate (first time), real_from > real_to: none
    int extrap_ticks;    // then this many ticks extrapolated with command real_to (or sim_seq)
} SpecStep;

void spec_clock_reset(SpecClock *c, u32 ack_seq);
// One observer tick. avail = spec_ring_contiguous(sim_seq): the newest command usable in order.
SpecStep spec_clock_step(SpecClock *c, u32 avail, int extrap_max);
// Authoritative state for ack_seq arrived: the replay range (exclusive of ack) and the ticks to
// extrapolate on top. The clock moves forward when the host is ahead of what was simulated.
SpecStep spec_clock_reconcile(SpecClock *c, u32 ack_seq);
// Extrapolation input: the last real command without its edge presses (no shots, jumps, uses).
UserCmd spec_extrap_cmd(const UserCmd *last);

// ------------------------------------------------------------------ wire blocks (pure)

// A player's "local data" (snapshot private block): sent to the player and to its in-eye observers.
typedef struct SpecLocalData {
    u32 ack_seq;          // last command the host simulated for the player
    PlayerSim sim;
    float hold_progress;
    u32 open_container;
} SpecLocalData;

void spec_write_local(Writer *wr, const SpecLocalData *d);
void spec_read_local(Reader *rd, SpecLocalData *d);

enum SpecFlags {
    SPECF_FORCED = 1 << 0,       // first snapshot after a target switch: reset the replica
    SPECF_CAN_RESPAWN = 1 << 1,  // the observer may press Respawn now
    SPECF_FP_DENIED = 1 << 2,    // first person was refused (enemy): third person instead
    SPECF_DEV = 1 << 3,          // spectating while alive (spectate command)
};

// Spectator block of every snapshot (after the local data): whose local data it was and the
// observer's own spectating state.
typedef struct SpecBlock {
    u8 data_slot;         // player the local data belongs to
    u8 mode;              // SpecMode
    u8 target;            // SPEC_NONE when none
    u8 flags;             // SpecFlags
    u8 respawn_ds;        // own respawn timer, 0.1 s (255 = waiting for a request)
    u8 killer;            // slot of the last killer or SPEC_NONE
    u16 target_owd_ms;    // host's estimate of the target's one-way delay to the host
    u32 seq;              // target switches so far (the observer resets on a change)
} SpecBlock;

void spec_write_block(Writer *wr, const SpecBlock *b);
void spec_read_block(Reader *rd, SpecBlock *b);

// MSG_SPEC_CMDS payload (after the type byte): target, count, (UserCmd, send_ms)[count].
void spec_write_cmds(Writer *wr, u8 target, const SpecCmdRing *ring, u32 from, u32 to);
// Reads into ring when the relay is for expect_target; returns the relay's target slot (SPEC_NONE
// on malformed data).
int spec_read_cmds(Reader *rd, int expect_target, SpecCmdRing *ring, int *received);

// ------------------------------------------------------------------ host (src/net/spectate.c)

struct World;
struct GameEvent;

typedef struct SpecObserver {
    u8 mode, target;
    u8 dev;               // entered with the spectate command (alive allowed)
    u8 killer;            // last killer slot (auto target)
    u8 forced;            // snapshots left that carry SPECF_FORCED + a command history burst
    u32 seq;              // switches
    vec3 free_pos;        // free camera position (relevance)
    bool free_pos_valid;
} SpecObserver;

void spectate_register(void);
void spectate_reset(void);                                  // new map / session
void spectate_host_tick(struct World *w);                   // validation, auto-advance, death flow
// Commands arrived for slot (remote or the host's own): kept and relayed to its observers now.
void spectate_host_cmds(struct World *w, int slot, const UserCmd *cmds, int count, u32 send_ms);
// Host request handling (RPC_SPECTATE / the spectate command). a = mode, b = target request.
SpecResult spectate_request(struct World *w, int obs, int mode, int target_req, bool dev);
void spectate_free_pos(int obs, vec3 pos);
void spectate_on_death(struct World *w, int victim, int killer);   // sets the respawn timer (sv_respawn_time)
void spectate_request_respawn(int slot);                           // RPC_RESPAWN (sv_respawn_mode 1)
bool spectate_respawn_ready(struct World *w, int slot);            // timer passed (and requested in mode 1)
bool spectate_join_dead(struct World *w);                          // a joining player starts dead (sv_join_dead)
void spectate_on_respawn(int slot);
void spectate_player_left(int slot);
const SpecObserver *spectate_observer(int slot);
// The slot whose local data / relevance a client in slot s gets (its target in eye / third person).
int spectate_source(struct World *w, int s);
// The host's relevance centre for client s.
vec3 spectate_relevance_center(struct World *w, int s);
// Snapshot spectator block for client s (data_slot = spectate_source).
void spectate_fill_block(struct World *w, int s, SpecBlock *b);
// The watched target of s for event routing (SPEC_NONE unless in eye / third person).
int spectate_event_target(int s);
u8 spectate_observers_mask(int target);                     // in-eye / third-person observers of target
// Relay writer used by the net layer: commands from .. to of target (see spec_write_cmds).
const SpecCmdRing *spectate_host_ring(int target);
// Net layer hook: send MSG_SPEC_CMDS for target from..to to client slot obs.
typedef void (*SpecSendFn)(int obs, int target, u32 from, u32 to);
void spectate_set_sender(SpecSendFn fn);
// Bytes relayed per observer since the last call (net_snapstats).
u32 spectate_take_relay_bytes(int obs);
void spectate_count_relay_bytes(int obs, u32 bytes);

#endif
