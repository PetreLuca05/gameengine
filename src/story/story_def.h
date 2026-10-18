// Story mode: the chapter definition (docs/STORY.md "Data"). Pure: assets/story/chapters/<id>.json ->
// StoryDef, plus validation (unique ids, references, the objective DAG, the soft-lock lint). No World,
// no engine; the host runtime (src/story/story_run.c), tools and tests/test_story.c share it.
#ifndef DG_STORY_DEF_H
#define DG_STORY_DEF_H

#include "core/base.h"

#define STORY_ID 48                 // ids, entity / volume / item names
#define STORY_ARG 64                // free string arguments (text ids, presets, paths)
#define STORY_MAX_OBJ 128
#define STORY_MAX_REQ 8             // requires per objective
#define STORY_MAX_TRIG 256          // authored + the implicit objective complete_on / fail_on triggers
#define STORY_MAX_COND 2048         // condition nodes, all triggers
#define STORY_MAX_ACT 2048          // actions, all triggers
#define STORY_MAX_BEAT 32
#define STORY_MAX_CHECKPOINT 32
#define STORY_MAX_PROMPT 32
#define STORY_MAX_COLLECT 64
#define STORY_MAX_ROLE 4
#define STORY_MAX_REPLACE 8
#define STORY_MAX_START_ITEMS 8
#define STORY_NONE (-1)

// ------------------------------------------------------------------ conditions

typedef enum StoryCondType {
    SC_TRUE,
    SC_ALL, SC_ANY, SC_NOT,     // children: first_child .. first_child + child_n - 1 (contiguous)
    SC_VOLUME,                  // name; on: enter / exit (events) / inside (query); who: any / all
    SC_INTERACT,                // name = entity
    SC_ITEM,                    // name = item; on: pickup / use / read (events) / has (query)
    SC_ENTITY,                  // name; on: killed / opened / closed
    SC_SIGNAL,                  // name: a custom event (UI choices, sequences ending, scripted beats)
    SC_OBJECTIVE,               // ref = objective index, state
    SC_FLAG,                    // name; value (== compare)
    SC_TIME,                    // num = story clock, minutes of day (>=)
    SC_TIMER,                   // num = seconds since `since` (objective done / trigger fired / start)
    SC_RANDOM,                  // num = chance per evaluation (seeded state RNG)
    SC_COUNT
} StoryCondType;

typedef enum StoryOn { SON_DEFAULT, SON_ENTER, SON_EXIT, SON_INSIDE, SON_PICKUP, SON_USE, SON_READ, SON_HAS, SON_KILLED, SON_OPENED, SON_CLOSED } StoryOn;
typedef enum StorySince { SINCE_START, SINCE_OBJECTIVE, SINCE_TRIGGER } StorySince;

typedef struct StoryCond {
    u8 type;                    // StoryCondType
    u8 on;                      // StoryOn
    u8 who_all;                 // SC_VOLUME inside: every player (else any)
    u8 since;                   // StorySince (SC_TIMER)
    u8 ever;                    // event conditions: also true if the event happened on any earlier tick
                                // (default for objective complete_on / fail_on: reading a note before
                                // its objective is active must still count)
    i16 first_child, child_n;
    i16 ref;                    // objective / trigger index (resolved)
    i16 state;                  // SC_OBJECTIVE: StoryObjState
    i32 value;                  // SC_FLAG
    float num;
    char name[STORY_ID];
} StoryCond;

// ------------------------------------------------------------------ actions

typedef enum StoryActType {
    // handled inside the pure story state
    SA_WAIT,                    // num[0] = seconds
    SA_OBJECTIVE,               // ref = objective, ival = StoryObjOp
    SA_FLAG,                    // name, ival = value
    SA_ENABLE, SA_DISABLE,      // ref = trigger
    SA_JOURNAL,                 // ref = collectible (found)
    SA_CHECKPOINT,              // ref = checkpoint
    SA_END_CHAPTER,
    SA_TIME,                    // num[0] = minutes of day, num[1] = blend seconds (the clock is story state)
    // forwarded to the runtime (StoryOut)
    SA_SPAWN,                   // name = marker / entity template, arg = character or item
    SA_DESPAWN,                 // name
    SA_AUDIO,                   // arg = sound / VO id, name = emitter entity (empty = 2D), ival = StoryAudioKind, num[0] = volume
    SA_SUBTITLE,                // arg = script id
    SA_WEATHER,                 // arg = preset, num[0] = blend seconds
    SA_UNLOCK, SA_LOCK,         // name = door / entity
    SA_GIVE, SA_TAKE,           // arg = item, ival = count
    SA_MARKER,                  // name = entity, arg = objective id (label), ival = 1 on / 0 off
    SA_CAMHINT,                 // name = entity, num[0] = strength 0..1, num[1] = seconds
    SA_FADE,                    // ival = 1 out / 0 in, num[0] = seconds
    SA_POWER,                   // name = group, ival = StoryPower
    SA_LIGHT,                   // name = group, ival = on
    SA_DOOR,                    // name, ival = StoryDoorOp
    SA_SEQUENCE,                // name = vista / vfx sequence
    SA_PROMPT,                  // ref = prompt (force show)
    SA_PHONE,                   // arg = message / photo / voicemail id (phone content unlock)
    SA_TELEPORT,                // name = story point: every player stands there (waking up after a night)
    SA_TITLE,                   // arg = script id (title + one line), num[0] = seconds: the centred card ("Day 3")
    SA_COUNT
} StoryActType;

typedef enum StoryObjOp { SOO_ACTIVATE, SOO_COMPLETE, SOO_FAIL, SOO_REVEAL } StoryObjOp;
typedef enum StoryAudioKind { SAK_SFX, SAK_VO, SAK_MUSIC, SAK_AMBIENT } StoryAudioKind;
typedef enum StoryPower { SPW_OFF, SPW_ON, SPW_FLICKER } StoryPower;
typedef enum StoryDoorOp { SDO_OPEN, SDO_CLOSE, SDO_SLAM } StoryDoorOp;

typedef struct StoryAction {
    u8 type;                    // StoryActType
    i16 ref;
    i32 ival;
    float num[2];
    char name[STORY_ID];
    char arg[STORY_ARG];
} StoryAction;

// ------------------------------------------------------------------ triggers, objectives, the rest

typedef struct StoryTrigger {
    char id[STORY_ID];
    i16 cond;                   // root condition node
    i16 arm;                    // STORY_NONE = armed from the start
    i16 first_action, action_n;
    i16 objective;              // implicit complete_on / fail_on trigger: only live while that objective is active
    float fallback;             // > 0: fire this long after arming even if cond never held
    float cooldown;             // repeatable triggers: seconds between fires
    bool once;
    bool start_disabled;
    bool implicit;
} StoryTrigger;

typedef struct StoryObjective {
    char id[STORY_ID];
    char text[STORY_ARG];       // script id for the HUD line ("obj.notes")
    char marker[STORY_ID];      // entity the compass / world marker points at while active (empty = none)
    char checkpoint[STORY_ID];  // checkpoint taken when it completes (empty = none)
    i16 parent;
    i16 requires[STORY_MAX_REQ];
    i8 require_n;
    i8 need;                    // group: 0 = every required child, N = any N children
    bool optional, hidden;
    bool manual;                // activates only through an `objective ... set: active` action (scripted beats)
    bool has_children;
    i16 complete_trig, fail_trig; // implicit triggers (STORY_NONE)
} StoryObjective;

typedef struct StoryBeat {
    i16 objective;              // the beat is current while this objective is active (or the last done)
    float from, to;             // minutes of day
    float expected;             // seconds of play the window is spread over
    i16 day;                    // "day": 1.. (multi-day chapters; the clock restarts each morning), else the previous beat's
} StoryBeat;

typedef struct StoryPromptDef {
    char id[STORY_ID];
    char action[STORY_ID];      // input action the prompt teaches ("move", "inventory"); done when performed
    char text[STORY_ARG];
    i16 after;                  // objective that must be active or done (STORY_NONE = from the start)
    i16 until;                  // objective whose completion retires the prompt (STORY_NONE = only the action)
} StoryPromptDef;

typedef enum StoryCollectKind { SCK_DOCUMENT, SCK_PHOTO, SCK_VOICEMAIL, SCK_MESSAGE, SCK_ITEM } StoryCollectKind;

typedef struct StoryCollectible {
    char id[STORY_ID];
    char text[STORY_ARG];       // script id (title + body come from the .story file)
    char image[STORY_ARG];
    u8 kind;
} StoryCollectible;

typedef struct StoryRole {
    char id[STORY_ID];
    char spawn[STORY_ID];
    i16 replaces[STORY_MAX_REPLACE]; // objectives auto-completed while this role is played
    i8 replace_n;
    i8 slot;
} StoryRole;

typedef struct StoryDef {
    char id[STORY_ID];
    char title[STORY_ARG];
    char map[STORY_ID];
    char script[STORY_ARG];
    // start conditions
    char start_checkpoint[STORY_ID];
    char start_weather[STORY_ID];
    char start_season[16];
    float start_time;           // minutes of day
    char start_items[STORY_MAX_START_ITEMS][STORY_ID];
    int start_counts[STORY_MAX_START_ITEMS];
    int start_item_n;

    StoryObjective obj[STORY_MAX_OBJ];
    int obj_n;
    StoryTrigger trig[STORY_MAX_TRIG];
    int trig_n;
    StoryCond cond[STORY_MAX_COND];
    int cond_n;
    StoryAction act[STORY_MAX_ACT];
    int act_n;
    StoryBeat beat[STORY_MAX_BEAT];
    int beat_n;
    char checkpoint[STORY_MAX_CHECKPOINT][STORY_ID];
    int checkpoint_n;
    StoryPromptDef prompt[STORY_MAX_PROMPT];
    int prompt_n;
    StoryCollectible collect[STORY_MAX_COLLECT];
    int collect_n;
    StoryRole role[STORY_MAX_ROLE];
    int role_n;
    int players;                // "players": 1 = single player (the host refuses joins), up to 4
} StoryDef;

// Parses the chapter JSON into *d (zeroed first). Returns false with a message on a syntax or
// reference error; the def is unusable then. Run story_def_validate afterwards for the lint.
bool story_def_parse(StoryDef *d, const char *json, char *err, int err_size);

// Structural lint: objective DAG (requires + parent edges) acyclic, every required objective has a
// completion route (complete_on, children, or an action that completes it), an end_chapter action
// exists, checkpoints / beats sane. Writes up to max_lines messages ("<where>: <problem>") into out
// (newline separated); returns the number of problems.
int story_def_validate(const StoryDef *d, char *out, int out_size);

int story_find_objective(const StoryDef *d, const char *id);
int story_find_trigger(const StoryDef *d, const char *id);
int story_find_collectible(const StoryDef *d, const char *id);
int story_find_checkpoint(const StoryDef *d, const char *id);
int story_find_prompt(const StoryDef *d, const char *id);

// "HH:MM" (or a plain number of minutes) -> minutes of day; < 0 on a bad string.
float story_parse_clock(const char *s);
const char *story_act_name(StoryActType t);
const char *story_cond_name(StoryCondType t);

#endif
