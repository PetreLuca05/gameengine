// Story solver bot. Pure; see story_bot.h.
#include "story/story_bot.h"
#include <stdio.h>

void story_bot_init(StoryBot *b, u32 choice) {
    memset(b, 0, sizeof(*b));
    b->choice = choice;
    b->think = 0.5f;
    b->last_trigger = STORY_NONE;
}

static bool bot_inside(void *user, const char *volume, bool all_players) {
    DG_UNUSED(all_players);
    const StoryBot *b = user;
    return !strcmp(b->volume, volume);
}

static bool bot_has(void *user, const char *item) {
    const StoryBot *b = user;
    for (int i = 0; i < b->item_n; i++)
        if (!strcmp(b->items[i], item)) return true;
    return false;
}

StoryView story_bot_view(StoryBot *b) {
    StoryView v = {b, bot_inside, bot_has};
    return v;
}

// Can the bot make this condition true by acting? (Event leaves are; time, timers, flags and
// objective states are not: the bot waits for them.)
static bool actionable(const StoryDef *d, int ci) {
    const StoryCond *c = &d->cond[ci];
    switch ((StoryCondType)c->type) {
    case SC_ALL: {
        bool any = false;
        for (int k = 0; k < c->child_n; k++) any |= actionable(d, c->first_child + k);
        return any;
    }
    case SC_ANY:
        for (int k = 0; k < c->child_n; k++)
            if (actionable(d, c->first_child + k)) return true;
        return false;
    case SC_VOLUME: case SC_INTERACT: case SC_ITEM: case SC_ENTITY: case SC_SIGNAL: return true;
    default: return false;
    }
}

static void act(StoryBot *b, StoryState *s, const StoryDef *d, int ci, u32 salt) {
    const StoryCond *c = &d->cond[ci];
    const char *first = NULL;
    switch ((StoryCondType)c->type) {
    case SC_ALL:
        for (int k = 0; k < c->child_n; k++) act(b, s, d, c->first_child + k, salt * 31u + (u32)k);
        return;
    case SC_ANY: {
        int opts[64], n = 0;
        for (int k = 0; k < c->child_n && n < 64; k++)
            if (actionable(d, c->first_child + k)) opts[n++] = c->first_child + k;
        if (n) act(b, s, d, opts[dg_hash_u32(b->choice ^ salt) % (u32)n], salt * 37u + 1u);
        return;
    }
    case SC_VOLUME:
        if (c->on == SON_ENTER) story_event(s, SEV_VOLUME_ENTER, c->name);
        else if (c->on == SON_EXIT) story_event(s, SEV_VOLUME_EXIT, c->name);
        dg_strcpy(b->volume, sizeof(b->volume), c->on == SON_EXIT ? "" : c->name);
        first = c->name;
        break;
    case SC_INTERACT: story_event(s, SEV_INTERACT, c->name); first = c->name; break;
    case SC_ITEM:
        if (c->on == SON_USE) story_event(s, SEV_ITEM_USE, c->name);
        else if (c->on == SON_READ) story_event(s, SEV_ITEM_READ, c->name);
        else story_event(s, SEV_ITEM_PICKUP, c->name);
        if (b->item_n < STORY_BOT_ITEMS && !bot_has(b, c->name)) dg_strcpy(b->items[b->item_n++], STORY_ID, c->name);
        first = c->name;
        break;
    case SC_ENTITY:
        story_event(s, c->on == SON_OPENED ? SEV_ENTITY_OPENED : c->on == SON_CLOSED ? SEV_ENTITY_CLOSED : SEV_ENTITY_KILLED, c->name);
        first = c->name;
        break;
    case SC_SIGNAL: story_event(s, SEV_SIGNAL, c->name); first = c->name; break;
    default: return;
    }
    if (first && !b->last_target[0]) dg_strcpy(b->last_target, sizeof(b->last_target), first);
}

int story_bot_step(StoryBot *b, StoryState *s, const StoryDef *d, float dt) {
    b->cooldown -= dt;
    if (b->cooldown > 0 || s->ended) return STORY_NONE;
    // Live, actionable triggers. Implicit objective triggers of required objectives come first (the
    // critical path), then optional objectives and authored triggers.
    int cand[STORY_MAX_TRIG], n = 0, required = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int t = 0; t < d->trig_n; t++) {
            const StoryTrigger *tr = &d->trig[t];
            const StoryTrigRt *rt = &s->trig[t];
            if (!rt->enabled || (tr->once && rt->fired)) continue;
            if (tr->objective != STORY_NONE && s->obj[tr->objective].state != SOS_ACTIVE) continue;
            bool crit = tr->implicit && !d->obj[tr->objective].optional;
            if ((pass == 0) != crit) continue;
            int ci = !rt->armed && tr->arm != STORY_NONE ? tr->arm : tr->cond;
            if (ci == STORY_NONE || !actionable(d, ci)) continue;
            cand[n++] = t;
        }
        if (pass == 0) required = n;
    }
    b->last_trigger = STORY_NONE;
    b->last_target[0] = 0;
    if (!n) return STORY_NONE;
    // Branch choice: rotate among the optional candidates when no required one is live (decisions are
    // modelled as optional children of a need-1 group).
    int pick = required ? cand[b->choice % (u32)required] : cand[dg_hash_u32(b->choice + (u32)b->steps * 7u) % (u32)n];
    const StoryTrigger *tr = &d->trig[pick];
    int ci = !s->trig[pick].armed && tr->arm != STORY_NONE ? tr->arm : tr->cond;
    act(b, s, d, ci, (u32)pick);
    b->last_trigger = pick;
    b->steps++;
    b->cooldown = b->think;
    return pick;
}
