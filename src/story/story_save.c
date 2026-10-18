// Story save / load (docs/STORY.md "Saves"). Pure; see story_save.h.
#include "story/story_save.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static const char *const PROMPT_STATES[] = {"pending", "done", "dismissed"};

static void put_u64(cJSON *o, const char *key, u64 v) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%016" PRIx64, v);
    cJSON_AddStringToObject(o, key, buf);
}

static u64 get_u64(const cJSON *o, const char *key) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
    return s ? (u64)strtoull(s, NULL, 16) : 0;
}

static double num(const cJSON *o, const char *key, double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

char *story_save_write(const StoryState *s, const StoryDef *d, const cJSON *world, const cJSON *players) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "format", STORY_SAVE_FORMAT);
    cJSON_AddStringToObject(root, "chapter", d->id);
    cJSON_AddStringToObject(root, "checkpoint", s->checkpoint >= 0 ? d->checkpoint[s->checkpoint] : "");
    cJSON_AddNumberToObject(root, "now", s->now);
    cJSON_AddNumberToObject(root, "clock", s->clock);
    if (s->clock_override) {
        cJSON *co = cJSON_AddObjectToObject(root, "clock_blend");
        cJSON_AddNumberToObject(co, "target", s->clock_target);
        cJSON_AddNumberToObject(co, "rate", s->clock_rate);
    }
    cJSON_AddBoolToObject(root, "ended", s->ended);
    cJSON_AddNumberToObject(root, "roles", s->roles);
    put_u64(root, "seed", s->seed);
    put_u64(root, "rng_state", s->rng.state);
    put_u64(root, "rng_inc", s->rng.inc);

    cJSON *objs = cJSON_AddObjectToObject(root, "objectives");
    for (int i = 0; i < d->obj_n; i++) {
        const StoryObjRt *o = &s->obj[i];
        cJSON *e = cJSON_AddObjectToObject(objs, d->obj[i].id);
        cJSON_AddStringToObject(e, "state", story_obj_state_name((StoryObjState)o->state));
        if (o->revealed) cJSON_AddBoolToObject(e, "revealed", true);
        if (o->by_role) cJSON_AddBoolToObject(e, "by_role", true);
        cJSON_AddNumberToObject(e, "t_active", o->t_active);
        cJSON_AddNumberToObject(e, "t_done", o->t_done);
    }
    cJSON *trigs = cJSON_AddObjectToObject(root, "triggers");
    for (int t = 0; t < d->trig_n; t++) {
        const StoryTrigRt *r = &s->trig[t];
        cJSON *e = cJSON_AddObjectToObject(trigs, d->trig[t].id);
        cJSON_AddNumberToObject(e, "fired", r->fired);
        cJSON_AddBoolToObject(e, "enabled", r->enabled);
        cJSON_AddBoolToObject(e, "armed", r->armed);
        cJSON_AddNumberToObject(e, "t_armed", r->t_armed);
        cJSON_AddNumberToObject(e, "t_fired", r->t_fired);
    }
    cJSON *seqs = cJSON_AddArrayToObject(root, "sequences");
    for (int i = 0; i < s->seq_n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "trigger", d->trig[s->seq[i].trig].id);
        cJSON_AddNumberToObject(e, "pc", s->seq[i].pc);
        cJSON_AddNumberToObject(e, "wait", s->seq[i].wait);
        cJSON_AddItemToArray(seqs, e);
    }
    cJSON *flags = cJSON_AddObjectToObject(root, "flags");
    for (int f = 0; f < s->flag_n; f++) cJSON_AddNumberToObject(flags, s->flag_name[f], s->flag_val[f]);
    cJSON *found = cJSON_AddArrayToObject(root, "journal");
    for (int c = 0; c < d->collect_n; c++)
        if (s->found[c]) cJSON_AddItemToArray(found, cJSON_CreateString(d->collect[c].id));
    cJSON *prompts = cJSON_AddObjectToObject(root, "prompts");
    for (int p = 0; p < d->prompt_n; p++)
        if (s->prompt[p] != SPS_PENDING) cJSON_AddStringToObject(prompts, d->prompt[p].id, PROMPT_STATES[s->prompt[p]]);
    cJSON *seen = cJSON_AddArrayToObject(root, "seen");
    for (int i = 0; i < s->seen_n; i++) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%08x", s->seen[i]);
        cJSON_AddItemToArray(seen, cJSON_CreateString(buf));
    }
    cJSON_AddItemToObject(root, "world", world ? cJSON_Duplicate(world, true) : cJSON_CreateObject());
    cJSON_AddItemToObject(root, "players", players ? cJSON_Duplicate(players, true) : cJSON_CreateArray());
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    return out;
}

static int state_parse(const char *s) {
    for (int i = SOS_INACTIVE; i <= SOS_FAILED; i++)
        if (s && !strcmp(s, story_obj_state_name((StoryObjState)i))) return i;
    return -1;
}

bool story_save_read(StoryState *s, const StoryDef *d, const char *json, cJSON **world, cJSON **players, int *unknown, char *err, int err_size) {
    if (world) *world = NULL;
    if (players) *players = NULL;
    if (unknown) *unknown = 0;
    if (err_size > 0) err[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(err, (size_t)err_size, "save: JSON syntax error");
        return false;
    }
    int skipped = 0;
    bool ok = false;
    const char *chapter = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "chapter"));
    int format = (int)num(root, "format", 0);
    if (format < 1 || format > STORY_SAVE_FORMAT) {
        snprintf(err, (size_t)err_size, "save: format %d not supported (this build reads %d)", format, STORY_SAVE_FORMAT);
        goto done;
    }
    if (!chapter || strcmp(chapter, d->id)) {
        snprintf(err, (size_t)err_size, "save: chapter '%s' is not '%s'", chapter ? chapter : "?", d->id);
        goto done;
    }
    story_init(s, d, get_u64(root, "seed"));
    s->rng.state = get_u64(root, "rng_state");
    s->rng.inc = get_u64(root, "rng_inc");
    s->now = (float)num(root, "now", 0);
    s->clock = (float)num(root, "clock", d->start_time);
    const cJSON *cb = cJSON_GetObjectItemCaseSensitive(root, "clock_blend");
    if (cb) {
        s->clock_override = true;
        s->clock_target = (float)num(cb, "target", s->clock);
        s->clock_rate = (float)num(cb, "rate", 0);
    }
    s->ended = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ended"));
    s->roles = (u8)num(root, "roles", 0);
    const char *cp = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "checkpoint"));
    s->checkpoint = (i16)(cp && *cp ? story_find_checkpoint(d, cp) : STORY_NONE);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "objectives")) {
        int i = story_find_objective(d, e->string);
        int st = state_parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "state")));
        if (i == STORY_NONE || st < 0) { skipped++; continue; }
        StoryObjRt *o = &s->obj[i];
        o->state = (u8)st;
        o->revealed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "revealed"));
        o->by_role = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "by_role"));
        o->t_active = (float)num(e, "t_active", 0);
        o->t_done = (float)num(e, "t_done", 0);
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "triggers")) {
        int t = story_find_trigger(d, e->string);
        if (t == STORY_NONE) { skipped++; continue; }
        StoryTrigRt *r = &s->trig[t];
        r->fired = (u16)num(e, "fired", 0);
        r->enabled = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(e, "enabled"));
        r->armed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "armed"));
        r->t_armed = (float)num(e, "t_armed", 0);
        r->t_fired = (float)num(e, "t_fired", 0);
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "sequences")) {
        int t = story_find_trigger(d, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "trigger")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "trigger")) : "");
        if (t == STORY_NONE || s->seq_n >= STORY_MAX_SEQ) { skipped++; continue; }
        StorySeq *q = &s->seq[s->seq_n++];
        q->trig = (i16)t;
        q->pc = (i16)num(e, "pc", 0);
        q->wait = (float)num(e, "wait", 0);
        if (q->pc > d->trig[t].action_n) q->pc = d->trig[t].action_n;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "flags"))
        if (cJSON_IsNumber(e)) story_set_flag(s, e->string, (i32)e->valuedouble);
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "journal")) {
        int c = story_find_collectible(d, cJSON_IsString(e) ? e->valuestring : "");
        if (c == STORY_NONE) { skipped++; continue; }
        s->found[c] = 1;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "prompts")) {
        int p = story_find_prompt(d, e->string);
        const char *v = cJSON_GetStringValue(e);
        if (p == STORY_NONE || !v) { skipped++; continue; }
        for (int k = 0; k < ARRAY_COUNT(PROMPT_STATES); k++)
            if (!strcmp(v, PROMPT_STATES[k])) s->prompt[p] = (u8)k;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "seen")) {
        if (s->seen_n < STORY_MAX_SEEN && cJSON_IsString(e)) s->seen[s->seen_n++] = (u32)strtoul(e->valuestring, NULL, 16);
    }
    if (world) {
        cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "world");
        *world = w ? cJSON_Duplicate(w, true) : NULL;
    }
    if (players) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "players");
        *players = p ? cJSON_Duplicate(p, true) : NULL;
    }
    ok = true;
done:
    if (unknown) *unknown = skipped;
    cJSON_Delete(root);
    return ok;
}

bool story_save_info(const char *json, StorySaveInfo *info) {
    memset(info, 0, sizeof(*info));
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const char *ch = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "chapter"));
    const char *cp = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "checkpoint"));
    dg_strcpy(info->chapter, sizeof(info->chapter), ch ? ch : "");
    dg_strcpy(info->checkpoint, sizeof(info->checkpoint), cp ? cp : "");
    info->clock = (float)num(root, "clock", 0);
    info->play_time = (float)num(root, "now", 0);
    info->ended = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ended"));
    cJSON_Delete(root);
    return info->chapter[0] != 0;
}
