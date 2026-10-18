// Story mode glue (docs/STORY.md): chapters on the host (src/story pure core + this runtime), the
// level's "story" block (points, volumes, story interactables, power groups), replication (EV_STORY),
// saves / Continue, console commands, and the client-side presentation state the HUD draws
// (src/ui/hud/hud_story.c).
#ifndef DG_GAME_STORY_H
#define DG_GAME_STORY_H

#include "game.h"
#include "story/story.h"
#include "story/story_script.h"

// EV_STORY kinds (GameEvent.id).
typedef enum StoryEvKind {
    SEVT_CHAPTER,       // text = chapter id ("" = story ended / stopped)
    SEVT_OBJECTIVE,     // ent = objective, ent2 = state | revealed << 4 | by_role << 5
    SEVT_SUBTITLE,      // text = script id
    SEVT_SHOW,          // to a player: text = script id (document / message panel)
    SEVT_PROMPT,        // ent = prompt index (forced)
    SEVT_JOURNAL,       // ent = collectible
    SEVT_FADE,          // ent2 = 1 out / 0 in, value = seconds
    SEVT_AUDIO,         // text = sound id, pos, value = volume, ent2 = StoryAudioKind, ent = 1 positional
    SEVT_MARKER,        // text = label, pos, ent = entity (0 = the point), ent2 = on
    SEVT_CAMHINT,       // pos, value = strength, dir.x = seconds
    SEVT_PEEK,          // to a player: ent = door
    SEVT_SYNC,          // data[0] = objective count, data[1..] = state | revealed << 4 | by_role << 5; data[130..137] = journal bits
    SEVT_SEQUENCE,      // text = sequence name (vista / VFX; presentation, docs/STORY.md §6)
    SEVT_PHONE,         // text = phone content id (messages / voicemail / gallery unlock)
    SEVT_END,           // the chapter ended: value = seconds until the host returns to the menu
} StoryEvKind;

#define STORY_UI_SUBS 8
#define STORY_UI_MARKERS 8

typedef struct StoryUiSub {
    char speaker[32];
    char text[256];
    float start, dur;           // ui time
} StoryUiSub;

typedef struct StoryUiMarker {
    bool on;
    EntityId ent;
    vec3 pos;
    char label[STORY_ARG];
} StoryUiMarker;

// What the HUD shows (every machine; filled from EV_STORY).
typedef struct StoryUi {
    bool active;
    const StoryDef *def;
    const StoryScript *script;
    const StoryState *mirror;   // objective / journal / prompt state as replicated (prompts are per machine)
    float time;
    StoryUiSub subs[STORY_UI_SUBS];
    int sub_n;
    char doc[STORY_ARG];        // open document / message panel ("" = none)
    float doc_time;
    int prompt;                 // the prompt showing (STORY_NONE)
    float prompt_time;
    int flash_obj;              // objective just completed (check mark), STORY_NONE
    float flash_time;
    float fade, fade_from, fade_to, fade_t, fade_len;
    StoryUiMarker markers[STORY_UI_MARKERS];
    bool ended;
    float end_time;
    bool journal_open;
    bool debug;                 // story_debug overlay
} StoryUi;

void story_register(void);
bool story_active(void);                         // host: a chapter is running
const StoryUi *story_ui(void);
void story_level_loaded(World *w, const char *map);   // host + client, after the level loads
void story_host_tick(World *w);                  // host, per sim tick (after entities_tick)
void story_on_interact(World *w, EntityId e, int slot);   // host: a player used an interactable
void story_sv_player_joined(World *w, int slot);
void story_sv_player_left(World *w, int slot);
void story_handle_event(const GameEvent *ev);    // EV_STORY, every machine
void story_frame(World *w, float dt);            // every machine, per frame
// Host: start a chapter (from its start, or from a save slot: "auto" = the last checkpoint).
bool story_start(const char *chapter, const char *slot);
void story_stop(void);
// Saves (<pref>/saves/story/<chapter>_<slot>.json): the Continue entry reads the newest.
bool story_save_path(const char *chapter, const char *slot, char *out, int size);
bool story_continue_available(char *chapter, int size);
// The debug overlay text (story_debug), "" when inactive.
void story_debug_text(char *out, int size);

#endif
