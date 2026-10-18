// Story saves: <pref>/saves/story/<chapter>_<slot>.json (docs/STORY.md "Saves"). Pure: the story state
// is written by id (objectives, triggers, prompts, collectibles), so a save survives chapter edits that
// add or reorder entries. The host passes its own blobs for the world deltas and the per-role player
// state; they round-trip untouched.
#ifndef DG_STORY_SAVE_H
#define DG_STORY_SAVE_H

#include "story/story.h"
#include "cJSON.h"

#define STORY_SAVE_FORMAT 1

typedef struct StorySaveInfo {   // the header the chapter select / Continue shows without a full load
    char chapter[STORY_ID];
    char checkpoint[STORY_ID];
    float clock;                // minutes of day
    float play_time;            // story seconds
    bool ended;
} StorySaveInfo;

// Returns a malloc'd JSON string (free()). world / players may be NULL; they are duplicated in.
char *story_save_write(const StoryState *s, const StoryDef *d, const cJSON *world, const cJSON *players);
// Restores *s from a save of the same chapter (story_init + the saved values). world / players, when
// not NULL, receive detached copies (cJSON_Delete them; NULL if the save had none). Ids the chapter no
// longer has are skipped and counted in *unknown (may be NULL).
bool story_save_read(StoryState *s, const StoryDef *d, const char *json, cJSON **world, cJSON **players, int *unknown, char *err, int err_size);
bool story_save_info(const char *json, StorySaveInfo *info);

#endif
