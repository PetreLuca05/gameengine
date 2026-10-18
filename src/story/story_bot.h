// Story solver bot (docs/STORY.md "Tests"): walks a chapter's objective graph by reading the live
// triggers and producing the events that satisfy them (interactions, items, volumes, signals), one
// target at a time, like a player who always knows what to do next. Pure: tests/test_story.c runs it
// headless to prove a chapter cannot soft-lock along any branch; the in-game `story_bot` command (src/story/
// story_run.c) turns the same targets into teleports + real interactions.
#ifndef DG_STORY_BOT_H
#define DG_STORY_BOT_H

#include "story/story.h"

#define STORY_BOT_ITEMS 32

typedef struct StoryBot {
    u32 choice;                 // branch selector: rotates candidate order and `any` picks
    float think;                // seconds between targets (a player takes time)
    float cooldown;
    char volume[STORY_ID];      // the one volume the bot stands in
    char items[STORY_BOT_ITEMS][STORY_ID];
    int item_n;
    int steps;
    int last_trigger;           // trigger the last step worked on (STORY_NONE: none was satisfiable)
    char last_target[STORY_ID]; // its first event name (logs)
} StoryBot;

void story_bot_init(StoryBot *b, u32 choice);
// Call before story_tick: queues the events for one target when the think timer allows. Returns the
// trigger worked on, or STORY_NONE (waiting on time / timers / fallbacks).
int story_bot_step(StoryBot *b, StoryState *s, const StoryDef *d, float dt);
StoryView story_bot_view(StoryBot *b);

#endif
