// Story mode: the narrative script format, assets/story/scripts/*.story (docs/STORY.md "Narrative
// script"). Markdown-like: `# Title`, then `## <id>  [kind · "Title" · Speaker · filter]` sections whose
// bodies hold `NAME: line` dialogue cues, `> quoted` document text and plain paragraphs. Events carry
// the ids; every machine resolves the text locally. Pure.
#ifndef DG_STORY_SCRIPT_H
#define DG_STORY_SCRIPT_H

#include "story/story_def.h"

typedef struct StoryCue {
    char speaker[32];
    char text[256];
    float start, dur;           // seconds from the entry's start (no audio: reading-speed estimate)
} StoryCue;

typedef struct StoryEntry {
    char id[STORY_ARG];
    char kind[16];              // text (default), document, voice, photo, message, voicemail, prompt, objective...
    char title[64];
    char speaker[32];           // voice entries: the default speaker
    char filter[16];            // voice entries: door / phone / radio / tv / wall (audio + subtitle style)
    const char *body;           // the section text, '>' markers stripped, trimmed
    int first_cue, cue_n;
    float duration;             // sum of the cues (0 for non-dialogue)
} StoryEntry;

typedef struct StoryScript {
    char title[STORY_ARG];
    StoryEntry *e;
    int n;
    StoryCue *cue;
    int cue_n;
    char *buf;                  // owned copy of the text, entries point into it
} StoryScript;

// The cue length used without audio: 0.4 s + 60 ms per character, at least 1.5 s.
float story_cue_duration(int chars);
bool story_script_parse(StoryScript *s, const char *text, char *err, int err_size);
void story_script_free(StoryScript *s);
const StoryEntry *story_script_find(const StoryScript *s, const char *id);
// Every text id the chapter uses (objective / prompt / collectible texts, subtitle and VO actions)
// exists in the script. Writes the missing ids (newline separated) into out; returns how many.
int story_script_lint(const StoryDef *d, const StoryScript *s, char *out, int out_size);

#endif
