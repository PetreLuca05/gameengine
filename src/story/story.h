// Story mode: the chapter's runtime state and its simulation (docs/STORY.md). Pure: the host feeds
// events (interactions, items, volumes, signals, performed input actions) and a StoryView for queries,
// story_tick advances objectives, triggers, scripted sequences and the beat-windowed clock, and returns
// notes (runtime actions, objective changes, journal unlocks, checkpoints, the end) for the host glue
// (src/story/story_run.c) to apply and replicate. Same inputs + same dt sequence = same state.
#ifndef DG_STORY_H
#define DG_STORY_H

#include "story/story_def.h"

#define STORY_MAX_SEQ 32            // running action sequences (a trigger's `do` list with waits)
#define STORY_MAX_EVENTS 64         // events queued for one tick
#define STORY_MAX_FLAGS 64
#define STORY_MAX_SEEN 512          // distinct (event kind, name) pairs remembered for `ever` conditions
#define STORY_MAX_NOTES 256         // notes one tick can return
#define STORY_CLOCK_CATCHUP 2.0f    // clock minutes per second while catching up to a beat's window

typedef enum StoryObjState { SOS_INACTIVE, SOS_ACTIVE, SOS_DONE, SOS_FAILED } StoryObjState;

typedef enum StoryEventKind {
    SEV_INTERACT,               // entity name
    SEV_ITEM_PICKUP, SEV_ITEM_USE, SEV_ITEM_READ, // item id (a read collectible id also marks it found)
    SEV_ENTITY_KILLED, SEV_ENTITY_OPENED, SEV_ENTITY_CLOSED,
    SEV_VOLUME_ENTER, SEV_VOLUME_EXIT,
    SEV_SIGNAL,                 // custom: UI choices, sequence ends ("choice_open")
    SEV_ACTION,                 // an input action was performed ("move"): retires prompts
    SEV_PROMPT_DISMISS,         // a prompt id the player dismissed
    SEV_COUNT
} StoryEventKind;

typedef struct StoryEvent {
    u8 kind;
    char name[STORY_ID];
} StoryEvent;

typedef struct StoryObjRt {
    u8 state;                   // StoryObjState
    bool revealed;              // hidden objectives show once revealed (or completed)
    bool by_role;               // completed because a co-op role replaced it
    float t_active, t_done;     // story seconds (t_done also for failed)
} StoryObjRt;

typedef struct StoryTrigRt {
    bool enabled, armed;
    u16 fired;
    float t_armed, t_fired;
} StoryTrigRt;

typedef struct StorySeq {
    i16 trig;
    i16 pc;                     // next action, relative to the trigger's first action
    float wait;
} StorySeq;

typedef enum StoryPromptState { SPS_PENDING, SPS_DONE, SPS_DISMISSED } StoryPromptState;

typedef struct StoryState {
    float now;                  // story seconds since the chapter started (paused while the sim is not ticked)
    float clock;                // minutes of day (drives daynight; time_scale is 0 in story mode)
    float clock_target, clock_rate; // a `time` action blend in progress
    bool clock_override;
    StoryObjRt obj[STORY_MAX_OBJ];
    StoryTrigRt trig[STORY_MAX_TRIG];
    StorySeq seq[STORY_MAX_SEQ];
    int seq_n;
    char flag_name[STORY_MAX_FLAGS][STORY_ID];
    i32 flag_val[STORY_MAX_FLAGS];
    int flag_n;
    u8 found[STORY_MAX_COLLECT];
    u8 prompt[STORY_MAX_PROMPT];   // StoryPromptState
    u32 seen[STORY_MAX_SEEN];   // hashes of every (kind, name) event so far (sorted insert not needed: small)
    int seen_n;
    u8 roles;                   // bit per role index currently played
    i16 checkpoint;             // last checkpoint reached (index), STORY_NONE before the first
    bool ended;
    u64 seed;
    Rng rng;
    StoryEvent ev[STORY_MAX_EVENTS];
    int ev_n;
} StoryState;

typedef enum StoryNoteKind {
    SN_ACTION,                  // a runtime action to apply (act)
    SN_OBJECTIVE,               // ref = objective, state = new StoryObjState (also reveals: state unchanged, revealed)
    SN_JOURNAL,                 // ref = collectible found
    SN_CHECKPOINT,              // ref = checkpoint reached: the host saves
    SN_PROMPT,                  // ref = prompt to show now (forced)
    SN_TRIGGER,                 // ref = trigger fired (debug log)
    SN_END,                     // the chapter ended
} StoryNoteKind;

typedef struct StoryNote {
    u8 kind;
    i16 ref;
    i16 state;
    const StoryAction *act;
} StoryNote;

typedef struct StoryOut {
    StoryNote n[STORY_MAX_NOTES];
    int count;
    int dropped;                // notes past STORY_MAX_NOTES (a bug in the chapter: too much in one tick)
} StoryOut;

// World queries the conditions need. Any callback may be NULL (the query is false).
typedef struct StoryView {
    void *user;
    bool (*volume_inside)(void *user, const char *volume, bool all_players);
    bool (*has_item)(void *user, const char *item);
} StoryView;

void story_init(StoryState *s, const StoryDef *d, u64 seed);
// Queue an event for the next story_tick. False when the queue is full (the event is dropped).
bool story_event(StoryState *s, StoryEventKind kind, const char *name);
// Advances by dt seconds. out may be NULL (notes discarded). Returns out->count (0 with NULL).
int story_tick(StoryState *s, const StoryDef *d, const StoryView *v, float dt, StoryOut *out);

// Dev / console / co-op hooks; changes are reported through the next tick's notes.
void story_force_objective(StoryState *s, const StoryDef *d, int obj, StoryObjOp op, StoryOut *out);
void story_set_role(StoryState *s, int role, bool present);
void story_set_clock(StoryState *s, float minutes);
void story_set_flag(StoryState *s, const char *name, i32 value);
i32 story_flag(const StoryState *s, const char *name);
// Fires a trigger now regardless of its condition (story_fire); false if it is spent or unknown.
bool story_fire(StoryState *s, const StoryDef *d, int trig, StoryOut *out);

// The HUD line: active, required, revealed leaves whose ancestors are all active, in definition
// order. Returns the count (<= max).
int story_active_leaves(const StoryState *s, const StoryDef *d, int *idx, int max);
// The prompt to show (first pending whose `after` objective is active / done and whose `until`
// objective is not done), or STORY_NONE.
int story_prompt_current(const StoryState *s, const StoryDef *d);
// The timeline beat the clock is following, or STORY_NONE before the first.
int story_current_beat(const StoryState *s, const StoryDef *d);
// Stable hash of the replicated / saved state (desync and determinism tests).
u32 story_state_hash(const StoryState *s, const StoryDef *d);
const char *story_obj_state_name(StoryObjState st);

#endif
