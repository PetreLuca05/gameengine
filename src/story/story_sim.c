// Story simulation: objectives, triggers, action sequences, the beat-windowed clock, prompts (docs/STORY.md).
// Pure; see story.h.
#include "story/story.h"
#include <math.h>
#include <stdio.h>

static void note(StoryOut *out, StoryNoteKind kind, int ref, int state, const StoryAction *act) {
    if (!out) return;
    if (out->count >= STORY_MAX_NOTES) { out->dropped++; return; }
    StoryNote *n = &out->n[out->count++];
    n->kind = (u8)kind;
    n->ref = (i16)ref;
    n->state = (i16)state;
    n->act = act;
}

const char *story_obj_state_name(StoryObjState st) {
    static const char *const names[] = {"inactive", "active", "done", "failed"};
    return st <= SOS_FAILED ? names[st] : "?";
}

void story_init(StoryState *s, const StoryDef *d, u64 seed) {
    memset(s, 0, sizeof(*s));
    s->clock = d->start_time;
    s->checkpoint = (i16)story_find_checkpoint(d, d->start_checkpoint);
    s->seed = seed;
    s->rng = rng_make(seed);
    for (int t = 0; t < d->trig_n; t++) s->trig[t].enabled = !d->trig[t].start_disabled;
}

bool story_event(StoryState *s, StoryEventKind kind, const char *name) {
    if (s->ev_n >= STORY_MAX_EVENTS) return false;
    StoryEvent *e = &s->ev[s->ev_n++];
    e->kind = (u8)kind;
    dg_strcpy(e->name, sizeof(e->name), name);
    return true;
}

static u32 event_hash(int kind, const char *name) { return dg_hash_u32(dg_hash_str(name) + (u32)kind * 0x9E3779B9u); }

static bool seen(const StoryState *s, int kind, const char *name) {
    u32 h = event_hash(kind, name);
    for (int i = 0; i < s->seen_n; i++)
        if (s->seen[i] == h) return true;
    return false;
}

static bool has_event_now(const StoryState *s, StoryEventKind kind, const char *name) {
    for (int i = 0; i < s->ev_n; i++)
        if (s->ev[i].kind == kind && !strcmp(s->ev[i].name, name)) return true;
    return false;
}

static bool has_event(const StoryState *s, const StoryCond *c, StoryEventKind kind, const char *name) {
    return has_event_now(s, kind, name) || (c->ever && seen(s, kind, name));
}

static int flag_index(const StoryState *s, const char *name) {
    for (int i = 0; i < s->flag_n; i++)
        if (!strcmp(s->flag_name[i], name)) return i;
    return STORY_NONE;
}

i32 story_flag(const StoryState *s, const char *name) {
    int i = flag_index(s, name);
    return i == STORY_NONE ? 0 : s->flag_val[i];
}

void story_set_flag(StoryState *s, const char *name, i32 value) {
    int i = flag_index(s, name);
    if (i == STORY_NONE) {
        if (s->flag_n >= STORY_MAX_FLAGS) return;
        i = s->flag_n++;
        dg_strcpy(s->flag_name[i], STORY_ID, name);
    }
    s->flag_val[i] = value;
}

// ------------------------------------------------------------------ conditions

static bool eval(StoryState *s, const StoryDef *d, const StoryView *v, int ci) {
    const StoryCond *c = &d->cond[ci];
    switch ((StoryCondType)c->type) {
    case SC_TRUE: return true;
    case SC_ALL:
        for (int k = 0; k < c->child_n; k++)
            if (!eval(s, d, v, c->first_child + k)) return false;
        return true;
    case SC_ANY:
        for (int k = 0; k < c->child_n; k++)
            if (eval(s, d, v, c->first_child + k)) return true;
        return false;
    case SC_NOT: return !eval(s, d, v, c->first_child);
    case SC_VOLUME:
        if (c->on == SON_ENTER) return has_event(s, c, SEV_VOLUME_ENTER, c->name);
        if (c->on == SON_EXIT) return has_event(s, c, SEV_VOLUME_EXIT, c->name);
        return v && v->volume_inside && v->volume_inside(v->user, c->name, c->who_all);
    case SC_INTERACT: return has_event(s, c, SEV_INTERACT, c->name);
    case SC_ITEM:
        switch (c->on) {
        case SON_USE: return has_event(s, c, SEV_ITEM_USE, c->name);
        case SON_READ: return has_event(s, c, SEV_ITEM_READ, c->name);
        case SON_HAS: return v && v->has_item && v->has_item(v->user, c->name);
        default: return has_event(s, c, SEV_ITEM_PICKUP, c->name);
        }
    case SC_ENTITY:
        return has_event(s, c, c->on == SON_OPENED ? SEV_ENTITY_OPENED : c->on == SON_CLOSED ? SEV_ENTITY_CLOSED : SEV_ENTITY_KILLED, c->name);
    case SC_SIGNAL: return has_event(s, c, SEV_SIGNAL, c->name);
    case SC_OBJECTIVE: return s->obj[c->ref].state == c->state;
    case SC_FLAG: return story_flag(s, c->name) == c->value;
    case SC_TIME: return s->clock >= c->num;
    case SC_TIMER: {
        float since = 0.0f;
        if (c->since == SINCE_OBJECTIVE) {
            const StoryObjRt *o = &s->obj[c->ref];
            if (o->state != SOS_DONE) return false;
            since = o->t_done;
        } else if (c->since == SINCE_TRIGGER) {
            const StoryTrigRt *t = &s->trig[c->ref];
            if (!t->fired) return false;
            since = t->t_fired;
        }
        return s->now - since >= c->num;
    }
    case SC_RANDOM: return rng_float(&s->rng) < c->num;
    case SC_COUNT: break;
    }
    return false;
}

// ------------------------------------------------------------------ objectives

static void set_obj(StoryState *s, const StoryDef *d, int i, StoryObjState st, StoryOut *out) {
    StoryObjRt *o = &s->obj[i];
    if (o->state == st) return;
    o->state = (u8)st;
    if (st == SOS_ACTIVE) o->t_active = s->now;
    if (st == SOS_DONE || st == SOS_FAILED) {
        o->t_done = s->now;
        o->revealed = true;
    }
    note(out, SN_OBJECTIVE, i, st, NULL);
    if (st == SOS_DONE && d->obj[i].checkpoint[0]) {
        int cp = story_find_checkpoint(d, d->obj[i].checkpoint);
        if (cp != STORY_NONE) {
            s->checkpoint = (i16)cp;
            note(out, SN_CHECKPOINT, cp, 0, NULL);
        }
    }
}

static bool requires_done(const StoryState *s, const StoryDef *d, int i) {
    const StoryObjective *o = &d->obj[i];
    for (int k = 0; k < o->require_n; k++)
        if (s->obj[o->requires[k]].state != SOS_DONE) return false;
    return true;
}

// Propagates until stable: roles complete what they replace, inactive objectives whose parent is
// active and whose requires are done activate, groups complete (or fail) from their children.
static void propagate(StoryState *s, const StoryDef *d, StoryOut *out) {
    for (int r = 0; r < d->role_n; r++) {
        if (!(s->roles & (1u << r))) continue;
        for (int k = 0; k < d->role[r].replace_n; k++) {
            int o = d->role[r].replaces[k];
            if (s->obj[o].state != SOS_DONE) {
                s->obj[o].by_role = true;
                set_obj(s, d, o, SOS_DONE, out);
            }
        }
    }
    for (int pass = 0; pass < STORY_MAX_OBJ; pass++) {
        bool changed = false;
        for (int i = 0; i < d->obj_n; i++) {
            const StoryObjective *o = &d->obj[i];
            StoryObjRt *rt = &s->obj[i];
            if (rt->state == SOS_INACTIVE && !o->manual) {
                bool parent_ok = o->parent == STORY_NONE || s->obj[o->parent].state == SOS_ACTIVE;
                if (parent_ok && requires_done(s, d, i)) {
                    set_obj(s, d, i, SOS_ACTIVE, out);
                    changed = true;
                }
            }
            if (rt->state == SOS_ACTIVE && o->has_children) {
                int done = 0, req = 0, req_done = 0;
                bool req_failed = false;
                for (int c = i + 1; c < d->obj_n; c++) {
                    if (d->obj[c].parent != i) continue;
                    bool ok = s->obj[c].state == SOS_DONE;
                    done += ok;
                    if (!d->obj[c].optional) {
                        req++;
                        req_done += ok;
                        req_failed |= s->obj[c].state == SOS_FAILED;
                    }
                }
                bool complete = o->need > 0 ? done >= o->need : (req > 0 && req_done == req);
                if (complete) {
                    set_obj(s, d, i, SOS_DONE, out);
                    changed = true;
                } else if (o->need == 0 && req_failed && !o->optional) {
                    set_obj(s, d, i, SOS_FAILED, out);
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
}

void story_force_objective(StoryState *s, const StoryDef *d, int obj, StoryObjOp op, StoryOut *out) {
    if (obj < 0 || obj >= d->obj_n) return;
    switch (op) {
    case SOO_ACTIVATE: if (s->obj[obj].state == SOS_INACTIVE) set_obj(s, d, obj, SOS_ACTIVE, out); break;
    case SOO_COMPLETE: set_obj(s, d, obj, SOS_DONE, out); break;
    case SOO_FAIL: set_obj(s, d, obj, SOS_FAILED, out); break;
    case SOO_REVEAL:
        if (!s->obj[obj].revealed) {
            s->obj[obj].revealed = true;
            note(out, SN_OBJECTIVE, obj, s->obj[obj].state, NULL);
        }
        break;
    }
    propagate(s, d, out);
}

void story_set_role(StoryState *s, int role, bool present) {
    if (role < 0 || role >= STORY_MAX_ROLE) return;
    if (present) s->roles |= (u8)(1u << role);
    else s->roles &= (u8)~(1u << role);
}

void story_set_clock(StoryState *s, float minutes) {
    s->clock = minutes;
    s->clock_override = false;
}

// ------------------------------------------------------------------ sequences

static void start_seq(StoryState *s, int trig) {
    if (s->seq_n >= STORY_MAX_SEQ) return; // validated chapters never get here; drop rather than corrupt
    StorySeq *q = &s->seq[s->seq_n++];
    q->trig = (i16)trig;
    q->pc = 0;
    q->wait = 0.0f;
}

// Runs a sequence until it waits or ends. Returns false when it ended.
static bool run_seq(StoryState *s, const StoryDef *d, StorySeq *q, StoryOut *out) {
    const StoryTrigger *t = &d->trig[q->trig];
    while (q->pc < t->action_n && !s->ended) {
        const StoryAction *a = &d->act[t->first_action + q->pc++];
        switch ((StoryActType)a->type) {
        case SA_WAIT:
            if (a->num[0] > 0) {
                q->wait = a->num[0];
                return true;
            }
            break;
        case SA_OBJECTIVE: story_force_objective(s, d, a->ref, (StoryObjOp)a->ival, out); break;
        case SA_FLAG: story_set_flag(s, a->name, a->ival); break;
        case SA_ENABLE: s->trig[a->ref].enabled = true; break;
        case SA_DISABLE: s->trig[a->ref].enabled = false; break;
        case SA_JOURNAL:
            if (!s->found[a->ref]) {
                s->found[a->ref] = 1;
                note(out, SN_JOURNAL, a->ref, 0, a);
            }
            break;
        case SA_CHECKPOINT:
            s->checkpoint = a->ref;
            note(out, SN_CHECKPOINT, a->ref, 0, a);
            break;
        case SA_END_CHAPTER:
            s->ended = true;
            note(out, SN_END, 0, 0, a);
            break;
        case SA_TIME:
            if (a->num[1] > 0) {
                s->clock_override = true;
                s->clock_target = a->num[0];
                s->clock_rate = fabsf(a->num[0] - s->clock) / a->num[1];
            } else {
                story_set_clock(s, a->num[0]);
            }
            break;
        case SA_PROMPT: note(out, SN_PROMPT, a->ref, 0, a); break;
        default: note(out, SN_ACTION, t->first_action + q->pc - 1, 0, a); break;
        }
    }
    return false;
}

static void remove_seq(StoryState *s, int i) {
    s->seq[i] = s->seq[--s->seq_n];
}

// ------------------------------------------------------------------ triggers

static void fire(StoryState *s, const StoryDef *d, int t, StoryOut *out) {
    StoryTrigRt *rt = &s->trig[t];
    rt->fired++;
    rt->t_fired = s->now;
    note(out, SN_TRIGGER, t, 0, NULL);
    start_seq(s, t);
    // Run it right away (actions with no wait apply this tick).
    if (!run_seq(s, d, &s->seq[s->seq_n - 1], out)) remove_seq(s, s->seq_n - 1);
}

bool story_fire(StoryState *s, const StoryDef *d, int t, StoryOut *out) {
    if (t < 0 || t >= d->trig_n) return false;
    if (d->trig[t].once && s->trig[t].fired) return false;
    fire(s, d, t, out);
    propagate(s, d, out);
    return true;
}

static void eval_triggers(StoryState *s, const StoryDef *d, const StoryView *v, StoryOut *out) {
    for (int t = 0; t < d->trig_n && !s->ended; t++) {
        const StoryTrigger *tr = &d->trig[t];
        StoryTrigRt *rt = &s->trig[t];
        if (!rt->enabled || (tr->once && rt->fired)) continue;
        if (tr->objective != STORY_NONE && s->obj[tr->objective].state != SOS_ACTIVE) continue;
        if (!tr->once && rt->fired && s->now - rt->t_fired < tr->cooldown) continue;
        if (!rt->armed && (tr->arm == STORY_NONE || eval(s, d, v, tr->arm))) {
            rt->armed = true;
            rt->t_armed = s->now;
        }
        if (!rt->armed) continue;
        bool go = tr->cond != STORY_NONE && eval(s, d, v, tr->cond);
        if (!go && tr->fallback > 0 && s->now - rt->t_armed >= tr->fallback) go = true;
        if (go) {
            fire(s, d, t, out);
            propagate(s, d, out); // later triggers this tick see the new objective states
        }
    }
}

// ------------------------------------------------------------------ clock

int story_current_beat(const StoryState *s, const StoryDef *d) {
    int cur = STORY_NONE;
    for (int b = 0; b < d->beat_n; b++) {
        u8 st = s->obj[d->beat[b].objective].state;
        if (st == SOS_ACTIVE || st == SOS_DONE) cur = b;
    }
    return cur;
}

static void tick_clock(StoryState *s, const StoryDef *d, float dt) {
    if (s->clock_override) {
        float step = s->clock_rate * dt;
        float diff = s->clock_target - s->clock;
        if (fabsf(diff) <= step || step <= 0) {
            s->clock = s->clock_target;
            s->clock_override = false;
        } else {
            s->clock += diff > 0 ? step : -step;
        }
        return;
    }
    int b = story_current_beat(s, d);
    if (b == STORY_NONE) return;
    const StoryBeat *bt = &d->beat[b];
    if (s->clock < bt->from) {
        s->clock = fminf(bt->from, s->clock + STORY_CLOCK_CATCHUP * dt);
    } else if (s->clock < bt->to) {
        float rate = (bt->to - bt->from) / bt->expected;
        s->clock = fminf(bt->to, s->clock + rate * dt);
    }
}

// ------------------------------------------------------------------ prompts / collectibles from events

static void apply_events(StoryState *s, const StoryDef *d, StoryOut *out) {
    for (int i = 0; i < s->ev_n; i++) {
        const StoryEvent *e = &s->ev[i];
        if (!seen(s, e->kind, e->name) && s->seen_n < STORY_MAX_SEEN) s->seen[s->seen_n++] = event_hash(e->kind, e->name);
        if (e->kind == SEV_ACTION || e->kind == SEV_PROMPT_DISMISS) {
            for (int p = 0; p < d->prompt_n; p++) {
                if (s->prompt[p] != SPS_PENDING) continue;
                if (e->kind == SEV_ACTION && !strcmp(d->prompt[p].action, e->name)) s->prompt[p] = SPS_DONE;
                if (e->kind == SEV_PROMPT_DISMISS && !strcmp(d->prompt[p].id, e->name)) s->prompt[p] = SPS_DISMISSED;
            }
        } else if (e->kind == SEV_ITEM_READ || e->kind == SEV_ITEM_PICKUP) {
            int c = story_find_collectible(d, e->name);
            if (c != STORY_NONE && !s->found[c]) {
                s->found[c] = 1;
                note(out, SN_JOURNAL, c, 0, NULL);
            }
        }
    }
}

int story_prompt_current(const StoryState *s, const StoryDef *d) {
    for (int p = 0; p < d->prompt_n; p++) {
        const StoryPromptDef *pr = &d->prompt[p];
        if (s->prompt[p] != SPS_PENDING) continue;
        if (pr->after != STORY_NONE && s->obj[pr->after].state != SOS_ACTIVE && s->obj[pr->after].state != SOS_DONE) continue;
        if (pr->until != STORY_NONE && s->obj[pr->until].state == SOS_DONE) continue;
        return p;
    }
    return STORY_NONE;
}

// ------------------------------------------------------------------ tick

int story_tick(StoryState *s, const StoryDef *d, const StoryView *v, float dt, StoryOut *out) {
    if (out) out->count = out->dropped = 0;
    if (s->ended) {
        s->ev_n = 0;
        return out ? out->count : 0;
    }
    s->now += dt;
    apply_events(s, d, out);
    // Running sequences first (they were started on an earlier tick), then the triggers; a trigger
    // fired now runs its first actions immediately.
    for (int i = 0; i < s->seq_n;) {
        StorySeq *q = &s->seq[i];
        q->wait -= dt;
        if (q->wait > 0) { i++; continue; }
        if (run_seq(s, d, q, out)) i++;
        else remove_seq(s, i);
    }
    propagate(s, d, out);
    eval_triggers(s, d, v, out);
    tick_clock(s, d, dt);
    s->ev_n = 0;
    return out ? out->count : 0;
}

// ------------------------------------------------------------------ queries

int story_active_leaves(const StoryState *s, const StoryDef *d, int *idx, int max) {
    int n = 0;
    for (int i = 0; i < d->obj_n && n < max; i++) {
        const StoryObjective *o = &d->obj[i];
        if (s->obj[i].state != SOS_ACTIVE || o->optional) continue;
        if (o->hidden && !s->obj[i].revealed) continue;
        bool leaf = true, live = true;
        for (int c = i + 1; c < d->obj_n && leaf; c++)
            if (d->obj[c].parent == i && s->obj[c].state == SOS_ACTIVE && !d->obj[c].optional) leaf = false;
        for (int p = o->parent; p != STORY_NONE && live; p = d->obj[p].parent) live = s->obj[p].state == SOS_ACTIVE;
        if (leaf && live) idx[n++] = i;
    }
    return n;
}

static u32 mix(u32 h, u32 v) { return (h ^ v) * 16777619u; }

u32 story_state_hash(const StoryState *s, const StoryDef *d) {
    u32 h = 2166136261u;
    for (int i = 0; i < d->obj_n; i++) h = mix(h, (u32)s->obj[i].state | (u32)s->obj[i].revealed << 4 | (u32)s->obj[i].by_role << 5);
    for (int t = 0; t < d->trig_n; t++) h = mix(h, (u32)s->trig[t].fired | (u32)s->trig[t].enabled << 16 | (u32)s->trig[t].armed << 17);
    for (int f = 0; f < s->flag_n; f++) h = mix(mix(h, dg_hash_str(s->flag_name[f])), (u32)s->flag_val[f]);
    for (int c = 0; c < d->collect_n; c++) h = mix(h, s->found[c]);
    for (int p = 0; p < d->prompt_n; p++) h = mix(h, s->prompt[p]);
    h = mix(h, (u32)(i32)lrintf(s->clock * 60.0f)); // clock to the second
    h = mix(h, (u32)(s->checkpoint + 1) | (u32)s->ended << 8 | (u32)s->roles << 9);
    return h;
}
