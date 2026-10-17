// Downed / revive / corpse / DNA / fabricator: the pure state machines (docs/REVIVE.md).
// No World, no engine: everything here is driven by ticks and plain inputs so the host, the
// predicting client and tests/test_revive.c run the exact same code.
//
//   ALIVE --hp 0--> DOWNED --bleed out / lethal hit--> DEAD --> corpse (DNA source)
//     ^               | revive (6 s)                    |  extract (3 s) -> DNA sample item
//     +---------------+                                 |  insert into a fabricator (5 s)
//     +-------- fabricator respawn (10 s) <-------------+  (or the timed respawn, rule)
#ifndef DG_REVIVE_CORE_H
#define DG_REVIVE_CORE_H

#include "core/base.h"
#include "core/dgmath.h"

#define REVIVE_HZ 60                // simulation ticks per second (SIM_HZ)
#define FAB_QUEUE_MAX 5             // samples waiting in one fabricator (NetEnt.attach[5])
#define DNA_MAX 32                  // live DNA samples (registry entries)
#define REVIVE_NOBODY 0xFF          // DownState.reviver / CorpseState.extractor: none
#define REVIVE_MAX_RESPAWN_ITEMS 4

DG_INLINE u16 revive_ticks(float seconds) {
    float t = seconds * (float)REVIVE_HZ + 0.5f;
    return (u16)(t < 0 ? 0 : (t > 65535.0f ? 65535.0f : t));
}

// ------------------------------------------------------------------ rules (assets/gameplay/revive_rules.json)

typedef enum RespawnMode { RESPAWN_TIMER, RESPAWN_FABRICATOR, RESPAWN_BOTH } RespawnMode;
typedef enum SampleLossRule { SAMPLE_DROP, SAMPLE_LOST, SAMPLE_KEEP } SampleLossRule;

typedef struct ReviveRules {
    // downs
    bool downs_enabled;
    float bleed_time;           // s for the first down
    float bleed_scale;          // multiplier per earlier down (shorter each time)
    float bleed_min;            // s floor
    int max_downs;              // downs before the next 0 hp kills outright (0 = unlimited)
    float overkill;             // one hit dealing more than this past 0 hp kills outright (0 = off)
    float downed_move_speed;    // m/s crawl
    float downed_hp;            // health while downed (takes damage normally, 0 = dead)
    bool keep_sidearm;          // a held pistol stays in hand (else the held item drops)
    bool downed_can_shoot;      // the kept sidearm can fire
    // revive
    float revive_time, revive_range, revive_hp, move_tolerance;
    bool revive_pauses_bleed;
    bool revive_resets_downs;
    // corpse + extraction
    float corpse_lifetime;      // s the body stays a DNA source
    float corpse_extract_despawn; // s after the last sample is taken
    float corpse_range;         // m from the feet to the body (extremely close)
    float extract_time;
    int samples_per_corpse;
    bool extract_needs_crouch;  // auto-crouch while extracting
    // samples
    SampleLossRule sample_on_death;
    float sample_weight;
    // fabricator
    float insert_time, insert_range;
    float recon_time;           // reconstruction (the shader effect runs over this)
    float open_time;            // chamber opens + forced idle after the respawn
    float cooldown;
    int queue_max;
    float respawn_hp;
    bool eject_on_power_loss;
    bool require_power;
    char respawn_items[REVIVE_MAX_RESPAWN_ITEMS][32];
    int respawn_item_counts[REVIVE_MAX_RESPAWN_ITEMS];
    int respawn_item_n;
    RespawnMode respawn_mode;
    // feedback
    bool auto_ping_down, auto_ping_death;
    bool death_skippable;
    bool self_revive;           // the self-revive item hook (stub: no item ships yet)
} ReviveRules;

void revive_rules_defaults(ReviveRules *r);
// Parses the rules JSON over the defaults. False (defaults kept) on a parse error.
bool revive_rules_parse(ReviveRules *r, const char *json, char *err, int err_size);
const char *respawn_mode_name(RespawnMode m);

// ------------------------------------------------------------------ downed / bleed-out

typedef struct DownState {
    u8 downed;                  // 1 while knocked
    u8 downs;                   // downs since the last respawn (shortens the bleed-out)
    u8 reviver;                 // slot reviving this player (host-written), REVIVE_NOBODY
    u8 self_reviving;
    u16 bleed_ticks;            // ticks left
    u16 bleed_total;
    u16 revive_ticks;           // reviver's progress (host-written, both HUDs show it)
    u16 revive_total;
} DownState;

// Bleed-out length in ticks for the next down given how many came before.
u16 down_bleed_total(const ReviveRules *r, int downs_before);
// What 0 hp does: true = knocked down, false = dead (downs off, kill-type damage, max downs, overkill).
bool down_should_knock(const ReviveRules *r, const DownState *d, bool kill_type, float overkill_amount);
void down_enter(DownState *d, const ReviveRules *r);
// One tick. True when the bleed-out runs out (the player dies).
bool down_step(DownState *d, const ReviveRules *r);
void down_revived(DownState *d, const ReviveRules *r);
void down_clear(DownState *d);   // respawn: full reset
DG_INLINE float down_bleed_frac(const DownState *d) { return d->bleed_total ? (float)d->bleed_ticks / (float)d->bleed_total : 0.0f; }

// ------------------------------------------------------------------ timed actions (predicted)

typedef enum ActKind { RACT_NONE, RACT_REVIVE, RACT_EXTRACT, RACT_INSERT, RACT_SELF_REVIVE, RACT_COUNT } ActKind;
typedef enum ActPhase { ACTP_IDLE, ACTP_RUNNING, ACTP_DONE, ACTP_CANCELLED } ActPhase;
typedef enum ActCancel { ACTC_NONE, ACTC_RELEASE, ACTC_MOVE, ACTC_DAMAGE, ACTC_RANGE, ACTC_TARGET, ACTC_HOST } ActCancel;

typedef struct ActionState {
    u8 kind;                    // ActKind
    u8 phase;                   // ActPhase (done / cancelled wait for the use release)
    u8 cancel;                  // ActCancel (last cancel reason, HUD)
    u8 pad;
    u32 target;                 // EntityId (locked at the start: focus may wander)
    u16 ticks, dur;             // progress in ticks: 3 / 5 / 6 s are exact at 60 Hz
    vec3 start_pos;             // feet at the start (move interrupt)
} ActionState;

typedef struct ActInput {
    bool use_down, use_pressed;
    u32 target;                 // the focused entity this tick
    ActKind afford;             // what the focused target offers right now (caller decides), RACT_NONE = nothing
    bool target_ok;             // the locked target is still valid + in range (caller)
    bool damaged;               // took damage this tick (host only; clients get it back by reconcile)
    vec3 pos;                   // feet
    float move_tolerance;       // m
    u16 dur;                    // ticks, for a start
} ActInput;

typedef enum ActResult { ACTR_NONE, ACTR_STARTED, ACTR_PROGRESS, ACTR_DONE, ACTR_CANCELLED } ActResult;

ActResult act_step(ActionState *a, const ActInput *in);
void act_cancel(ActionState *a, ActCancel why);   // host interrupts (target died, machine busy ...)
DG_INLINE bool act_running(const ActionState *a) { return a->phase == ACTP_RUNNING; }
DG_INLINE float act_progress(const ActionState *a) { return a->dur ? (float)a->ticks / (float)a->dur : 0.0f; }
DG_INLINE float act_remaining(const ActionState *a) { return a->dur > a->ticks ? (float)(a->dur - a->ticks) / (float)REVIVE_HZ : 0.0f; }
const char *act_kind_name(ActKind k);
const char *act_cancel_name(ActCancel c);

// ------------------------------------------------------------------ corpse (DNA source)

typedef struct CorpseState {
    u32 death_tick;
    u32 extract_tick;           // tick the last sample was taken (0 = never)
    u8 owner_slot;
    u8 samples_left;
    u8 extractor;               // slot of the last extractor, REVIVE_NOBODY
    u8 pad;
} CorpseState;

void corpse_init(CorpseState *c, const ReviveRules *r, int owner_slot, u32 tick);
DG_INLINE bool corpse_dna_available(const CorpseState *c) { return c->samples_left > 0; }
bool corpse_extract(CorpseState *c, int extractor, u32 tick);   // takes one sample
bool corpse_should_despawn(const CorpseState *c, const ReviveRules *r, u32 tick);
// Extraction range gate: horizontal distance from the feet to the body segment a..b (pelvis -> head),
// with a vertical sanity bound.
float corpse_distance(vec3 feet, vec3 a, vec3 b);
bool corpse_in_range(const ReviveRules *r, vec3 feet, vec3 a, vec3 b);

// ------------------------------------------------------------------ DNA registry (host-validated sample items)

typedef enum DnaStatus { DNA_FREE, DNA_CARRIED, DNA_DROPPED, DNA_IN_FABRICATOR, DNA_RECONSTRUCTING } DnaStatus;

typedef struct DnaSample {
    u32 uid;                    // unique across the session (shown on the item)
    u8 status;                  // DnaStatus
    u8 player_slot;             // whose DNA
    u8 carrier;                 // slot carrying it, REVIVE_NOBODY
    u8 extractor;
    u32 avatar;                 // cosmetics / character hash (inventory icon tint)
    u32 corpse;                 // EntityId of the source corpse (may be gone)
    char name[24];              // player name at extraction
} DnaSample;

typedef struct DnaRegistry {
    DnaSample s[DNA_MAX];       // index + 1 = the sample id stored in InvSlot.ammo / fabricator queue
    u32 next_uid;
} DnaRegistry;

void dna_reset(DnaRegistry *g);
int dna_alloc(DnaRegistry *g, int player_slot, const char *name, int extractor, u32 avatar, u32 corpse);   // id 1..DNA_MAX, 0 = full
void dna_free(DnaRegistry *g, int id);
DnaSample *dna_get(DnaRegistry *g, int id);
const DnaSample *dna_get_c(const DnaRegistry *g, int id);
// Sample of the given player (newest), 0 = none.
int dna_find_player(const DnaRegistry *g, int player_slot);

// ------------------------------------------------------------------ fabricator

typedef enum FabStateId { FAB_IDLE, FAB_RECONSTRUCTING, FAB_OPENING, FAB_COOLDOWN, FAB_UNPOWERED, FAB_STATE_COUNT } FabStateId;
typedef enum FabEvent { FABE_NONE, FABE_RECON_START, FABE_RESPAWN, FABE_OPENED, FABE_READY } FabEvent;

typedef struct FabMachine {
    u8 state;                   // FabStateId
    u8 qn;                      // samples queued
    u8 current;                 // sample id being reconstructed (0 = none)
    u8 powered;
    u8 queue[FAB_QUEUE_MAX];    // sample ids, oldest first
    u8 pad[3];
    u32 state_tick;             // tick the current state started (progress = (now - state_tick) / dur)
} FabMachine;

void fab_init(FabMachine *f, bool powered);
bool fab_enqueue(FabMachine *f, const ReviveRules *r, int sample_id);
bool fab_remove(FabMachine *f, int sample_id);   // drop from the queue (sample invalid)
FabEvent fab_step(FabMachine *f, const ReviveRules *r, u32 tick);
// The player being reconstructed came back another way (timed respawn) or left: skip to the cooldown.
void fab_abort(FabMachine *f, u32 tick);
// Power change: when it goes off mid-cycle the current + queued samples are ejected (rule).
// Returns the number of ejected sample ids written to out.
int fab_set_power(FabMachine *f, const ReviveRules *r, bool on, u32 tick, u8 *out, int out_cap);
float fab_progress(const FabMachine *f, const ReviveRules *r, u32 tick);   // 0..1 of the reconstruction
float fab_remaining(const FabMachine *f, const ReviveRules *r, u32 tick);  // s of the current state
const char *fab_state_name(FabStateId s);

#endif
