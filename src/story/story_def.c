// Chapter JSON -> StoryDef and the structural lint (docs/STORY.md "Data"). Pure.
#include "story/story_def.h"
#include "cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Parse {
    StoryDef *d;
    char *err;
    int err_size;
    bool failed;
    bool ever_default;          // parsing an objective's complete_on / fail_on
} Parse;

static void perr(Parse *p, const char *fmt, ...) {
    if (p->failed) return; // keep the first error
    p->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, (size_t)p->err_size, fmt, ap);
    va_end(ap);
}

static const char *jstr(const cJSON *o, const char *key, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static double jnum(const cJSON *o, const char *key, double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static bool jbool(const cJSON *o, const char *key, bool def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    return def;
}

float story_parse_clock(const char *s) {
    if (!s || !*s) return -1.0f;
    int h = 0, m = 0;
    char extra;
    if (sscanf(s, "%d:%d%c", &h, &m, &extra) == 2) {
        if (h < 0 || h > 47 || m < 0 || m > 59) return -1.0f;
        return (float)(h * 60 + m);
    }
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end) return -1.0f;
    return (float)v;
}

// A clock value may be "HH:MM" or a number of minutes.
static float jclock(Parse *p, const cJSON *v, const char *where) {
    if (cJSON_IsNumber(v)) return (float)v->valuedouble;
    float t = cJSON_IsString(v) ? story_parse_clock(v->valuestring) : -1.0f;
    if (t < 0) perr(p, "%s: bad clock value (want \"HH:MM\")", where);
    return t;
}

int story_find_objective(const StoryDef *d, const char *id) {
    for (int i = 0; i < d->obj_n; i++)
        if (!strcmp(d->obj[i].id, id)) return i;
    return STORY_NONE;
}

int story_find_trigger(const StoryDef *d, const char *id) {
    for (int i = 0; i < d->trig_n; i++)
        if (!strcmp(d->trig[i].id, id)) return i;
    return STORY_NONE;
}

int story_find_collectible(const StoryDef *d, const char *id) {
    for (int i = 0; i < d->collect_n; i++)
        if (!strcmp(d->collect[i].id, id)) return i;
    return STORY_NONE;
}

int story_find_checkpoint(const StoryDef *d, const char *id) {
    for (int i = 0; i < d->checkpoint_n; i++)
        if (!strcmp(d->checkpoint[i], id)) return i;
    return STORY_NONE;
}

int story_find_prompt(const StoryDef *d, const char *id) {
    for (int i = 0; i < d->prompt_n; i++)
        if (!strcmp(d->prompt[i].id, id)) return i;
    return STORY_NONE;
}

static int need_objective(Parse *p, const char *id, const char *where) {
    int i = story_find_objective(p->d, id ? id : "");
    if (i == STORY_NONE) perr(p, "%s: unknown objective '%s'", where, id ? id : "");
    return i;
}

static const char *const COND_NAMES[SC_COUNT] = {
    "true", "all", "any", "not", "volume", "interact", "item", "entity", "signal", "objective", "flag", "time", "timer", "random",
};
const char *story_cond_name(StoryCondType t) { return t < SC_COUNT ? COND_NAMES[t] : "?"; }

static const char *const ACT_NAMES[SA_COUNT] = {
    "wait", "objective", "flag", "enable", "disable", "journal", "checkpoint", "end_chapter", "time",
    "spawn", "despawn", "audio", "subtitle", "weather", "unlock", "lock", "give", "take", "marker",
    "camhint", "fade", "power", "light", "door", "sequence", "prompt", "phone", "teleport", "title",
};
const char *story_act_name(StoryActType t) { return t < SA_COUNT ? ACT_NAMES[t] : "?"; }

static int obj_state_parse(const char *s) {
    if (!s || !strcmp(s, "done")) return 2;
    if (!strcmp(s, "active")) return 1;
    if (!strcmp(s, "failed")) return 3;
    if (!strcmp(s, "inactive")) return 0;
    return -1;
}

static u8 on_parse(Parse *p, const char *s, const char *where) {
    static const char *const names[] = {"", "enter", "exit", "inside", "pickup", "use", "read", "has", "killed", "opened", "closed"};
    if (!s) return SON_DEFAULT;
    for (int i = 0; i < ARRAY_COUNT(names); i++)
        if (!strcmp(s, names[i])) return (u8)i;
    perr(p, "%s: unknown 'on' value '%s'", where, s);
    return SON_DEFAULT;
}

static int alloc_conds(Parse *p, int n) {
    if (p->d->cond_n + n > STORY_MAX_COND) {
        perr(p, "too many condition nodes (max %d)", STORY_MAX_COND);
        return STORY_NONE;
    }
    int first = p->d->cond_n;
    p->d->cond_n += n;
    for (int i = 0; i < n; i++) memset(&p->d->cond[first + i], 0, sizeof(StoryCond));
    return first;
}

static void parse_cond_into(Parse *p, int idx, const cJSON *j, const char *where);

static void parse_children(Parse *p, StoryCond *c, int idx, const cJSON *arr, const char *where) {
    int n = cJSON_GetArraySize(arr);
    int first = alloc_conds(p, n);
    if (first == STORY_NONE) return;
    c = &p->d->cond[idx]; // alloc may not move the array (fixed), but re-fetch for clarity
    c->first_child = (i16)first;
    c->child_n = (i16)n;
    int k = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) parse_cond_into(p, first + k++, e, where);
}

static void parse_cond_into(Parse *p, int idx, const cJSON *j, const char *where) {
    if (p->failed) return;
    StoryCond *c = &p->d->cond[idx];
    c->ref = STORY_NONE;
    if (cJSON_IsArray(j)) { // shorthand: a list is "all"
        c->type = SC_ALL;
        parse_children(p, c, idx, j, where);
        return;
    }
    if (cJSON_IsBool(j)) {
        c->type = cJSON_IsTrue(j) ? SC_TRUE : SC_NOT;
        if (!cJSON_IsTrue(j)) { // false = not(true)
            int first = alloc_conds(p, 1);
            if (first == STORY_NONE) return;
            c = &p->d->cond[idx];
            c->first_child = (i16)first;
            c->child_n = 1;
            p->d->cond[first].type = SC_TRUE;
        }
        return;
    }
    if (!cJSON_IsObject(j)) {
        perr(p, "%s: a condition must be an object, a list or a bool", where);
        return;
    }
    const cJSON *v;
    if ((v = cJSON_GetObjectItemCaseSensitive(j, "all")) || (v = cJSON_GetObjectItemCaseSensitive(j, "any"))) {
        c->type = !strcmp(v->string, "all") ? SC_ALL : SC_ANY;
        if (!cJSON_IsArray(v)) { perr(p, "%s: '%s' wants a list", where, v->string); return; }
        parse_children(p, c, idx, v, where);
        return;
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(j, "not"))) {
        c->type = SC_NOT;
        int first = alloc_conds(p, 1);
        if (first == STORY_NONE) return;
        c = &p->d->cond[idx];
        c->first_child = (i16)first;
        c->child_n = 1;
        parse_cond_into(p, first, v, where);
        return;
    }
    static const struct { const char *key; StoryCondType type; } named[] = {
        {"volume", SC_VOLUME}, {"interact", SC_INTERACT}, {"item", SC_ITEM}, {"entity", SC_ENTITY},
        {"signal", SC_SIGNAL}, {"flag", SC_FLAG},
    };
    for (int i = 0; i < ARRAY_COUNT(named); i++) {
        if (!(v = cJSON_GetObjectItemCaseSensitive(j, named[i].key))) continue;
        if (!cJSON_IsString(v)) { perr(p, "%s: '%s' wants a name", where, named[i].key); return; }
        c->type = (u8)named[i].type;
        dg_strcpy(c->name, sizeof(c->name), v->valuestring);
        c->on = on_parse(p, jstr(j, "on", NULL), where);
        const char *who = jstr(j, "who", "any");
        c->who_all = !strcmp(who, "all");
        c->value = (i32)jnum(j, "value", 1);
        c->ever = jbool(j, "ever", p->ever_default);
        if (c->type == SC_VOLUME && c->on == SON_DEFAULT) c->on = SON_INSIDE;
        if (c->type == SC_ITEM && c->on == SON_DEFAULT) c->on = SON_PICKUP;
        if (c->type == SC_ENTITY && c->on == SON_DEFAULT) c->on = SON_KILLED;
        return;
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(j, "objective"))) {
        c->type = SC_OBJECTIVE;
        c->ref = (i16)need_objective(p, cJSON_IsString(v) ? v->valuestring : NULL, where);
        int st = obj_state_parse(jstr(j, "state", NULL));
        if (st < 0) perr(p, "%s: bad objective state '%s'", where, jstr(j, "state", ""));
        c->state = (i16)st;
        return;
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(j, "time"))) {
        c->type = SC_TIME;
        c->num = jclock(p, v, where);
        return;
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(j, "timer"))) {
        c->type = SC_TIMER;
        c->num = (float)(cJSON_IsNumber(v) ? v->valuedouble : 0);
        const char *since = jstr(j, "since", NULL);
        c->since = SINCE_START;
        if (since) {
            int o = story_find_objective(p->d, since), t = story_find_trigger(p->d, since);
            if (o != STORY_NONE) { c->since = SINCE_OBJECTIVE; c->ref = (i16)o; }
            else if (t != STORY_NONE) { c->since = SINCE_TRIGGER; c->ref = (i16)t; }
            else perr(p, "%s: timer since '%s' is neither an objective nor a trigger", where, since);
        }
        return;
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(j, "random"))) {
        c->type = SC_RANDOM;
        c->num = (float)(cJSON_IsNumber(v) ? v->valuedouble : 0);
        return;
    }
    perr(p, "%s: unknown condition", where);
}

static int parse_cond(Parse *p, const cJSON *j, const char *where) {
    int idx = alloc_conds(p, 1);
    if (idx == STORY_NONE) return STORY_NONE;
    parse_cond_into(p, idx, j, where);
    return idx;
}

static void parse_action(Parse *p, StoryAction *a, const cJSON *j, const char *where) {
    memset(a, 0, sizeof(*a));
    a->ref = STORY_NONE;
    if (!cJSON_IsObject(j)) { perr(p, "%s: an action must be an object", where); return; }
    // The action is the object's first key that names one ({"fade": "in", "time": 3} is a fade with a
    // duration, not a time action): parameters may share names with other actions.
    const cJSON *v = NULL;
    int type = -1;
    for (const cJSON *k = j->child; k && type < 0; k = k->next)
        for (int t = 0; t < SA_COUNT; t++)
            if (k->string && !strcmp(k->string, ACT_NAMES[t])) { type = t; v = k; break; }
    if (type < 0) { perr(p, "%s: unknown action", where); return; }
    a->type = (u8)type;
    const char *s = cJSON_IsString(v) ? v->valuestring : "";
    switch ((StoryActType)type) {
    case SA_WAIT: a->num[0] = (float)(cJSON_IsNumber(v) ? v->valuedouble : 0); break;
    case SA_OBJECTIVE: {
        a->ref = (i16)need_objective(p, s, where);
        const char *op = jstr(j, "set", "done");
        if (!strcmp(op, "done") || !strcmp(op, "complete")) a->ival = SOO_COMPLETE;
        else if (!strcmp(op, "active") || !strcmp(op, "activate")) a->ival = SOO_ACTIVATE;
        else if (!strcmp(op, "failed") || !strcmp(op, "fail")) a->ival = SOO_FAIL;
        else if (!strcmp(op, "reveal")) a->ival = SOO_REVEAL;
        else perr(p, "%s: bad objective op '%s'", where, op);
        break;
    }
    case SA_FLAG:
        dg_strcpy(a->name, sizeof(a->name), s);
        a->ival = (i32)jnum(j, "value", 1);
        break;
    case SA_ENABLE: case SA_DISABLE:
        a->ref = (i16)story_find_trigger(p->d, s);
        if (a->ref == STORY_NONE) perr(p, "%s: unknown trigger '%s'", where, s);
        break;
    case SA_JOURNAL:
        a->ref = (i16)story_find_collectible(p->d, s);
        if (a->ref == STORY_NONE) perr(p, "%s: unknown collectible '%s'", where, s);
        break;
    case SA_CHECKPOINT:
        a->ref = (i16)story_find_checkpoint(p->d, s);
        if (a->ref == STORY_NONE) perr(p, "%s: unknown checkpoint '%s'", where, s);
        break;
    case SA_PROMPT:
        a->ref = (i16)story_find_prompt(p->d, s);
        if (a->ref == STORY_NONE) perr(p, "%s: unknown prompt '%s'", where, s);
        break;
    case SA_END_CHAPTER: break;
    case SA_TIME:
        a->num[0] = jclock(p, v, where);
        a->num[1] = (float)jnum(j, "blend", 0);
        break;
    case SA_AUDIO: {
        dg_strcpy(a->arg, sizeof(a->arg), s);
        dg_strcpy(a->name, sizeof(a->name), jstr(j, "at", ""));
        const char *k = jstr(j, "kind", "sfx");
        a->ival = !strcmp(k, "vo") ? SAK_VO : !strcmp(k, "music") ? SAK_MUSIC : !strcmp(k, "ambient") ? SAK_AMBIENT : SAK_SFX;
        a->num[0] = (float)jnum(j, "volume", 1.0);
        break;
    }
    case SA_SUBTITLE: case SA_PHONE:
        dg_strcpy(a->arg, sizeof(a->arg), s);
        break;
    case SA_TELEPORT: dg_strcpy(a->name, sizeof(a->name), s); break;
    case SA_TITLE:
        dg_strcpy(a->arg, sizeof(a->arg), s);
        a->num[0] = (float)jnum(j, "time", 4.0);
        break;
    case SA_WEATHER:
        dg_strcpy(a->arg, sizeof(a->arg), s);
        a->num[0] = (float)jnum(j, "blend", 0);
        break;
    case SA_SPAWN:
        dg_strcpy(a->name, sizeof(a->name), s);
        dg_strcpy(a->arg, sizeof(a->arg), jstr(j, "character", jstr(j, "item", "")));
        a->ival = (i32)jnum(j, "count", 1);
        break;
    case SA_GIVE: case SA_TAKE:
        dg_strcpy(a->arg, sizeof(a->arg), s);
        a->ival = (i32)jnum(j, "count", 1);
        break;
    case SA_MARKER:
        dg_strcpy(a->name, sizeof(a->name), s);
        dg_strcpy(a->arg, sizeof(a->arg), jstr(j, "label", ""));
        a->ival = jbool(j, "on", true);
        break;
    case SA_CAMHINT:
        dg_strcpy(a->name, sizeof(a->name), s);
        a->num[0] = (float)jnum(j, "strength", 0.5);
        a->num[1] = (float)jnum(j, "time", 1.5);
        break;
    case SA_FADE:
        a->ival = !strcmp(s, "out");
        if (strcmp(s, "out") && strcmp(s, "in")) perr(p, "%s: fade wants \"in\" or \"out\"", where);
        a->num[0] = (float)jnum(j, "time", 1.0);
        break;
    case SA_POWER: {
        dg_strcpy(a->name, sizeof(a->name), s);
        const char *st = jstr(j, "state", "off");
        a->ival = !strcmp(st, "on") ? SPW_ON : !strcmp(st, "flicker") ? SPW_FLICKER : SPW_OFF;
        break;
    }
    case SA_LIGHT:
        dg_strcpy(a->name, sizeof(a->name), s);
        a->ival = jbool(j, "on", true);
        break;
    case SA_DOOR: {
        dg_strcpy(a->name, sizeof(a->name), s);
        const char *st = jstr(j, "state", "open");
        a->ival = !strcmp(st, "close") ? SDO_CLOSE : !strcmp(st, "slam") ? SDO_SLAM : SDO_OPEN;
        break;
    }
    case SA_DESPAWN: case SA_UNLOCK: case SA_LOCK: case SA_SEQUENCE:
        dg_strcpy(a->name, sizeof(a->name), s);
        break;
    case SA_COUNT: break;
    }
}

static void parse_actions(Parse *p, const cJSON *arr, i16 *first, i16 *n, const char *where) {
    *first = (i16)p->d->act_n;
    *n = 0;
    if (!arr) return;
    if (cJSON_IsObject(arr)) { // a single action
        if (p->d->act_n >= STORY_MAX_ACT) { perr(p, "too many actions (max %d)", STORY_MAX_ACT); return; }
        parse_action(p, &p->d->act[p->d->act_n++], arr, where);
        *n = 1;
        return;
    }
    if (!cJSON_IsArray(arr)) { perr(p, "%s: 'do' wants a list of actions", where); return; }
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (p->d->act_n >= STORY_MAX_ACT) { perr(p, "too many actions (max %d)", STORY_MAX_ACT); return; }
        parse_action(p, &p->d->act[p->d->act_n++], e, where);
        (*n)++;
    }
}

// Pass 1: register the objective tree (ids + parents) in depth-first order.
static void register_objectives(Parse *p, const cJSON *arr, int parent) {
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (p->failed) return;
        const char *id = jstr(e, "id", NULL);
        if (!id || !*id) { perr(p, "objective without an id"); return; }
        if (story_find_objective(p->d, id) != STORY_NONE) { perr(p, "objective '%s' defined twice", id); return; }
        if (p->d->obj_n >= STORY_MAX_OBJ) { perr(p, "too many objectives (max %d)", STORY_MAX_OBJ); return; }
        int i = p->d->obj_n++;
        StoryObjective *o = &p->d->obj[i];
        memset(o, 0, sizeof(*o));
        dg_strcpy(o->id, sizeof(o->id), id);
        o->parent = (i16)parent;
        o->complete_trig = o->fail_trig = STORY_NONE;
        if (parent != STORY_NONE) p->d->obj[parent].has_children = true;
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(e, "children");
        if (cJSON_IsArray(ch)) register_objectives(p, ch, i);
    }
}

static int add_implicit_trigger(Parse *p, int obj, const cJSON *when, bool fail) {
    if (p->d->trig_n >= STORY_MAX_TRIG) { perr(p, "too many triggers (max %d)", STORY_MAX_TRIG); return STORY_NONE; }
    int t = p->d->trig_n++;
    StoryTrigger *tr = &p->d->trig[t];
    memset(tr, 0, sizeof(*tr));
    snprintf(tr->id, sizeof(tr->id), "%.30s:%s", p->d->obj[obj].id, fail ? "fail" : "complete");
    char where[96];
    snprintf(where, sizeof(where), "objective %s %s", p->d->obj[obj].id, fail ? "fail_on" : "complete_on");
    p->ever_default = true;
    tr->cond = (i16)parse_cond(p, when, where);
    p->ever_default = false;
    tr->arm = STORY_NONE;
    tr->objective = (i16)obj;
    tr->once = true;
    tr->implicit = true;
    if (p->d->act_n >= STORY_MAX_ACT) { perr(p, "too many actions (max %d)", STORY_MAX_ACT); return STORY_NONE; }
    StoryAction *a = &p->d->act[p->d->act_n];
    memset(a, 0, sizeof(*a));
    a->type = SA_OBJECTIVE;
    a->ref = (i16)obj;
    a->ival = fail ? SOO_FAIL : SOO_COMPLETE;
    tr->first_action = (i16)p->d->act_n++;
    tr->action_n = 1;
    return t;
}

// Pass 2: objective fields (references resolve against the full id set).
static void parse_objectives(Parse *p, const cJSON *arr) {
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (p->failed) return;
        int i = story_find_objective(p->d, jstr(e, "id", ""));
        StoryObjective *o = &p->d->obj[i];
        char where[80];
        snprintf(where, sizeof(where), "objective %s", o->id);
        dg_strcpy(o->text, sizeof(o->text), jstr(e, "text", ""));
        dg_strcpy(o->marker, sizeof(o->marker), jstr(e, "marker", ""));
        dg_strcpy(o->checkpoint, sizeof(o->checkpoint), jstr(e, "checkpoint", ""));
        if (o->checkpoint[0] && story_find_checkpoint(p->d, o->checkpoint) == STORY_NONE)
            perr(p, "%s: unknown checkpoint '%s'", where, o->checkpoint);
        o->optional = jbool(e, "optional", false);
        o->hidden = jbool(e, "hidden", false);
        o->manual = !strcmp(jstr(e, "activate", "auto"), "manual");
        const cJSON *req = cJSON_GetObjectItemCaseSensitive(e, "requires");
        if (cJSON_IsString(req)) {
            o->requires[o->require_n++] = (i16)need_objective(p, req->valuestring, where);
        } else if (cJSON_IsArray(req)) {
            const cJSON *r;
            cJSON_ArrayForEach(r, req) {
                if (o->require_n >= STORY_MAX_REQ) { perr(p, "%s: too many requires (max %d)", where, STORY_MAX_REQ); break; }
                o->requires[o->require_n++] = (i16)need_objective(p, cJSON_IsString(r) ? r->valuestring : "", where);
            }
        }
        const cJSON *grp = cJSON_GetObjectItemCaseSensitive(e, "group");
        if (grp) {
            const cJSON *need = cJSON_GetObjectItemCaseSensitive(grp, "need");
            o->need = cJSON_IsNumber(need) ? (i8)need->valueint : 0;
        }
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(e, "complete_on");
        if (c) o->complete_trig = (i16)add_implicit_trigger(p, i, c, false);
        c = cJSON_GetObjectItemCaseSensitive(e, "fail_on");
        if (c) o->fail_trig = (i16)add_implicit_trigger(p, i, c, true);
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(e, "children");
        if (cJSON_IsArray(ch)) parse_objectives(p, ch);
    }
}

bool story_def_parse(StoryDef *d, const char *json, char *err, int err_size) {
    memset(d, 0, sizeof(*d));
    Parse p = {d, err, err_size, false, false};
    if (err_size > 0) err[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        perr(&p, "JSON syntax error near '%.24s'", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
        return false;
    }
    dg_strcpy(d->id, sizeof(d->id), jstr(root, "id", ""));
    dg_strcpy(d->title, sizeof(d->title), jstr(root, "title", d->id));
    dg_strcpy(d->map, sizeof(d->map), jstr(root, "map", ""));
    dg_strcpy(d->script, sizeof(d->script), jstr(root, "script", ""));
    if (!d->id[0]) perr(&p, "chapter without an id");

    // Pass 1: ids every reference can point at.
    const cJSON *e, *arr;
    arr = cJSON_GetObjectItemCaseSensitive(root, "checkpoints");
    cJSON_ArrayForEach(e, arr) {
        if (d->checkpoint_n >= STORY_MAX_CHECKPOINT) { perr(&p, "too many checkpoints (max %d)", STORY_MAX_CHECKPOINT); break; }
        if (!cJSON_IsString(e)) { perr(&p, "checkpoints: want names"); break; }
        if (story_find_checkpoint(d, e->valuestring) != STORY_NONE) perr(&p, "checkpoint '%s' listed twice", e->valuestring);
        dg_strcpy(d->checkpoint[d->checkpoint_n++], STORY_ID, e->valuestring);
    }
    arr = cJSON_GetObjectItemCaseSensitive(root, "collectibles");
    cJSON_ArrayForEach(e, arr) {
        if (d->collect_n >= STORY_MAX_COLLECT) { perr(&p, "too many collectibles (max %d)", STORY_MAX_COLLECT); break; }
        StoryCollectible *c = &d->collect[d->collect_n];
        dg_strcpy(c->id, sizeof(c->id), jstr(e, "id", ""));
        if (!c->id[0]) { perr(&p, "collectible without an id"); break; }
        if (story_find_collectible(d, c->id) != STORY_NONE) perr(&p, "collectible '%s' defined twice", c->id);
        dg_strcpy(c->text, sizeof(c->text), jstr(e, "text", c->id));
        dg_strcpy(c->image, sizeof(c->image), jstr(e, "image", ""));
        const char *k = jstr(e, "kind", "document");
        c->kind = !strcmp(k, "photo") ? SCK_PHOTO : !strcmp(k, "voicemail") ? SCK_VOICEMAIL : !strcmp(k, "message") ? SCK_MESSAGE
                : !strcmp(k, "item") ? SCK_ITEM : SCK_DOCUMENT;
        d->collect_n++;
    }
    arr = cJSON_GetObjectItemCaseSensitive(root, "objectives");
    if (!cJSON_IsArray(arr) || !cJSON_GetArraySize(arr)) perr(&p, "chapter without objectives");
    else register_objectives(&p, arr, STORY_NONE);
    const cJSON *trigs = cJSON_GetObjectItemCaseSensitive(root, "triggers");
    cJSON_ArrayForEach(e, trigs) {
        if (d->trig_n >= STORY_MAX_TRIG) { perr(&p, "too many triggers (max %d)", STORY_MAX_TRIG); break; }
        const char *id = jstr(e, "id", NULL);
        if (!id || !*id) { perr(&p, "trigger without an id"); break; }
        if (story_find_trigger(d, id) != STORY_NONE) perr(&p, "trigger '%s' defined twice", id);
        StoryTrigger *t = &d->trig[d->trig_n++];
        memset(t, 0, sizeof(*t));
        dg_strcpy(t->id, sizeof(t->id), id);
    }
    const cJSON *prompts = cJSON_GetObjectItemCaseSensitive(root, "prompts");
    cJSON_ArrayForEach(e, prompts) {
        if (d->prompt_n >= STORY_MAX_PROMPT) { perr(&p, "too many prompts (max %d)", STORY_MAX_PROMPT); break; }
        StoryPromptDef *pr = &d->prompt[d->prompt_n++];
        dg_strcpy(pr->id, sizeof(pr->id), jstr(e, "id", ""));
        if (!pr->id[0]) perr(&p, "prompt without an id");
    }

    // Pass 2: everything that references ids.
    if (!p.failed && cJSON_IsArray(arr)) parse_objectives(&p, arr);
    int ti = 0;
    cJSON_ArrayForEach(e, trigs) {
        if (p.failed) break;
        StoryTrigger *t = &d->trig[ti++];
        char where[80];
        snprintf(where, sizeof(where), "trigger %s", t->id);
        const cJSON *when = cJSON_GetObjectItemCaseSensitive(e, "when");
        t->cond = when ? (i16)parse_cond(&p, when, where) : STORY_NONE;
        const cJSON *arm = cJSON_GetObjectItemCaseSensitive(e, "arm");
        t->arm = arm ? (i16)parse_cond(&p, arm, where) : STORY_NONE;
        const cJSON *fb = cJSON_GetObjectItemCaseSensitive(e, "fallback");
        t->fallback = (float)(cJSON_IsNumber(fb) ? fb->valuedouble : jnum(fb, "after", 0));
        t->cooldown = (float)jnum(e, "cooldown", 0);
        t->once = jbool(e, "once", true);
        t->start_disabled = jbool(e, "disabled", false);
        t->objective = STORY_NONE;
        if (!when && t->fallback <= 0) perr(&p, "%s: needs 'when' or a fallback", where);
        parse_actions(&p, cJSON_GetObjectItemCaseSensitive(e, "do"), &t->first_action, &t->action_n, where);
    }
    // Authored triggers hold the first indices (pass 1); parse_objectives appended the implicit
    // complete_on / fail_on ones after them.
    int pi = 0;
    cJSON_ArrayForEach(e, prompts) {
        if (p.failed) break;
        StoryPromptDef *pr = &d->prompt[pi++];
        dg_strcpy(pr->action, sizeof(pr->action), jstr(e, "action", pr->id));
        dg_strcpy(pr->text, sizeof(pr->text), jstr(e, "text", ""));
        const char *after = jstr(e, "after", NULL), *until = jstr(e, "until", NULL);
        pr->after = after ? (i16)need_objective(&p, after, "prompt") : STORY_NONE;
        pr->until = until ? (i16)need_objective(&p, until, "prompt") : STORY_NONE;
    }
    const cJSON *tl = cJSON_GetObjectItemCaseSensitive(root, "timeline");
    cJSON_ArrayForEach(e, tl) {
        if (p.failed) break;
        if (d->beat_n >= STORY_MAX_BEAT) { perr(&p, "too many beats (max %d)", STORY_MAX_BEAT); break; }
        StoryBeat *b = &d->beat[d->beat_n++];
        b->objective = (i16)need_objective(&p, jstr(e, "beat", ""), "timeline");
        b->from = jclock(&p, cJSON_GetObjectItemCaseSensitive(e, "from"), "timeline");
        b->to = jclock(&p, cJSON_GetObjectItemCaseSensitive(e, "to"), "timeline");
        b->expected = (float)jnum(e, "expected", 180);
        b->day = (i16)jnum(e, "day", d->beat_n > 1 ? d->beat[d->beat_n - 2].day : 1);
    }
    const cJSON *roles = cJSON_GetObjectItemCaseSensitive(root, "roles");
    cJSON_ArrayForEach(e, roles) {
        if (p.failed) break;
        if (d->role_n >= STORY_MAX_ROLE) { perr(&p, "too many roles (max %d)", STORY_MAX_ROLE); break; }
        StoryRole *r = &d->role[d->role_n++];
        r->slot = (i8)jnum(e, "slot", d->role_n - 1);
        dg_strcpy(r->id, sizeof(r->id), jstr(e, "id", ""));
        dg_strcpy(r->spawn, sizeof(r->spawn), jstr(e, "spawn", ""));
        const cJSON *rep;
        cJSON_ArrayForEach(rep, cJSON_GetObjectItemCaseSensitive(e, "replaces")) {
            if (r->replace_n >= STORY_MAX_REPLACE) { perr(&p, "role %s: too many replaces", r->id); break; }
            r->replaces[r->replace_n++] = (i16)need_objective(&p, cJSON_IsString(rep) ? rep->valuestring : "", "role replaces");
        }
    }
    d->players = (int)jnum(root, "players", 4);
    if (d->players < 1 || d->players > STORY_MAX_ROLE) perr(&p, "players must be 1..%d", STORY_MAX_ROLE);
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(root, "start");
    dg_strcpy(d->start_checkpoint, sizeof(d->start_checkpoint), jstr(start, "checkpoint", "start"));
    dg_strcpy(d->start_weather, sizeof(d->start_weather), jstr(start, "weather", "clear"));
    dg_strcpy(d->start_season, sizeof(d->start_season), jstr(start, "season", "summer"));
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(start, "time");
    d->start_time = st ? jclock(&p, st, "start") : 12.0f * 60.0f;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(start, "inventory")) {
        if (d->start_item_n >= STORY_MAX_START_ITEMS) break;
        const char *item = cJSON_IsString(e) ? e->valuestring : cJSON_IsArray(e) ? cJSON_GetStringValue(cJSON_GetArrayItem(e, 0)) : NULL;
        if (!item) continue;
        dg_strcpy(d->start_items[d->start_item_n], STORY_ID, item);
        d->start_counts[d->start_item_n++] = cJSON_IsArray(e) && cJSON_GetArraySize(e) > 1 ? cJSON_GetArrayItem(e, 1)->valueint : 1;
    }
    cJSON_Delete(root);
    return !p.failed;
}

// ------------------------------------------------------------------ lint

typedef struct Lint {
    char *out;
    int size, len, n;
} Lint;

static void lint(Lint *l, const char *fmt, ...) {
    l->n++;
    if (l->len >= l->size - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(l->out + l->len, (size_t)(l->size - l->len), fmt, ap);
    va_end(ap);
    if (w < 0) return;
    l->len += w;
    if (l->len >= l->size - 1) { l->len = l->size - 1; return; }
    l->out[l->len++] = '\n';
    l->out[l->len] = 0;
}

static bool objective_completed_by_action(const StoryDef *d, int obj) {
    for (int t = 0; t < d->trig_n; t++) {
        const StoryTrigger *tr = &d->trig[t];
        if (tr->implicit) continue;
        for (int k = 0; k < tr->action_n; k++) {
            const StoryAction *a = &d->act[tr->first_action + k];
            if (a->type == SA_OBJECTIVE && a->ref == obj && a->ival == SOO_COMPLETE) return true;
        }
    }
    for (int r = 0; r < d->role_n; r++)
        for (int k = 0; k < d->role[r].replace_n; k++)
            if (d->role[r].replaces[k] == obj && d->role[r].slot == 0) return true; // the host's role always plays
    return false;
}

// Depth-first cycle search over requires + child -> parent edges (an objective activates only after
// its parent is active and its requires are done).
static bool obj_cycle(const StoryDef *d, int i, u8 *mark) {
    if (mark[i] == 1) return true;
    if (mark[i] == 2) return false;
    mark[i] = 1;
    const StoryObjective *o = &d->obj[i];
    for (int k = 0; k < o->require_n; k++)
        if (o->requires[k] >= 0 && obj_cycle(d, o->requires[k], mark)) return true;
    if (o->parent >= 0 && obj_cycle(d, o->parent, mark)) return true;
    mark[i] = 2;
    return false;
}

int story_def_validate(const StoryDef *d, char *out, int out_size) {
    Lint l = {out, out_size, 0, 0};
    if (out_size > 0) out[0] = 0;
    u8 mark[STORY_MAX_OBJ] = {0};
    for (int i = 0; i < d->obj_n; i++) {
        memset(mark, 0, sizeof(mark));
        if (obj_cycle(d, i, mark)) lint(&l, "objective %s: dependency cycle (requires / parent)", d->obj[i].id);
    }
    bool has_end = false;
    for (int a = 0; a < d->act_n; a++) has_end |= d->act[a].type == SA_END_CHAPTER;
    if (!has_end) lint(&l, "chapter %s: no end_chapter action anywhere", d->id);
    for (int i = 0; i < d->obj_n; i++) {
        const StoryObjective *o = &d->obj[i];
        // A requirement on a descendant would deadlock: the child waits for the parent to activate.
        for (int k = 0; k < o->require_n; k++) {
            for (int p = d->obj[o->requires[k]].parent; p >= 0; p = d->obj[p].parent)
                if (p == i) lint(&l, "objective %s: requires its own descendant %s", o->id, d->obj[o->requires[k]].id);
        }
        if (o->optional) continue;
        bool route = o->complete_trig != STORY_NONE || o->has_children || objective_completed_by_action(d, i);
        if (!route) lint(&l, "objective %s: required but nothing completes it (add complete_on, children or an objective action)", o->id);
        if (o->has_children && o->need > 0) {
            int kids = 0;
            for (int c = 0; c < d->obj_n; c++) kids += d->obj[c].parent == i;
            if (o->need > kids) lint(&l, "objective %s: group needs %d of %d children", o->id, o->need, kids);
        } else if (o->has_children) {
            int req = 0;
            for (int c = 0; c < d->obj_n; c++) req += d->obj[c].parent == i && !d->obj[c].optional;
            if (!req && o->complete_trig == STORY_NONE) lint(&l, "objective %s: group of optional children never completes", o->id);
        }
        if (o->hidden && !o->optional) lint(&l, "objective %s: hidden objectives must be optional", o->id);
        if (o->manual) {
            bool activated = false;
            for (int a = 0; a < d->act_n; a++)
                activated |= d->act[a].type == SA_OBJECTIVE && d->act[a].ref == i && d->act[a].ival == SOO_ACTIVATE;
            if (!activated) lint(&l, "objective %s: manual activation but no action activates it", o->id);
        }
    }
    for (int b = 0; b < d->beat_n; b++) {
        const StoryBeat *bt = &d->beat[b];
        if (bt->to < bt->from) lint(&l, "timeline beat %s: 'to' before 'from'", d->obj[bt->objective].id);
        if (b > 0 && bt->day * 1440.0f + bt->from < d->beat[b - 1].day * 1440.0f + d->beat[b - 1].from)
            lint(&l, "timeline beat %s: starts before the previous beat (day %d)", d->obj[bt->objective].id, bt->day);
        if (b > 0 && bt->day < d->beat[b - 1].day) lint(&l, "timeline beat %s: day goes back", d->obj[bt->objective].id);
        if (bt->expected <= 0) lint(&l, "timeline beat %s: expected must be > 0", d->obj[bt->objective].id);
    }
    for (int r = 0; r < d->role_n; r++)
        if (d->role[r].slot >= d->players) lint(&l, "role %s: slot %d, but the chapter is for %d player(s)", d->role[r].id, d->role[r].slot, d->players);
    if (d->checkpoint_n && story_find_checkpoint(d, d->start_checkpoint) == STORY_NONE)
        lint(&l, "start: unknown checkpoint '%s'", d->start_checkpoint);
    for (int t = 0; t < d->trig_n; t++) {
        const StoryTrigger *tr = &d->trig[t];
        if (!tr->once && tr->cooldown <= 0 && !tr->implicit)
            lint(&l, "trigger %s: repeatable without a cooldown (fires every tick)", tr->id);
        int waits = 0;
        for (int k = 0; k < tr->action_n; k++) waits += d->act[tr->first_action + k].type == SA_WAIT;
        if (!tr->once && waits) lint(&l, "trigger %s: repeatable triggers cannot wait", tr->id);
    }
    return l.n;
}
