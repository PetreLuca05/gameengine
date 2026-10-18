// Story mode glue (docs/STORY.md): the host runs the chapter (src/story: StoryDef + StoryState, ticked
// with the simulation), feeds it world events (interactions, items, volumes, doors, kills) and applies its
// notes (spawn, audio, subtitles, weather, time, doors, power, markers...). Everything players see goes
// out as EV_STORY events with script ids; every machine (the host's own player included) builds its
// StoryUi from them. Saves are JSON by entity name (src/story/story_save.c + the world blob below).
#include "game_story.h"
#include "game_vista.h"
#include "game_internal.h"
#include "story/story_save.h"
#include "story/story_bot.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "gameplay/entities.h"
#include "gameplay/characters.h"
#include "gameplay/health.h"
#include "gameplay/zombies/horde.h"
#include "anim/anim_library.h"
#include "audio/audio.h"
#include "input/input.h"
#include "ui/mainmenu/mainmenu.h"
#include "game/profile.h"
#include "net/lobby_net.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define G (g_engine.game)

#define STORY_MAX_POINTS 64
#define STORY_MAX_VOLUMES 64
#define STORY_MAX_INTERACT 64
#define STORY_MAX_POWER 8
#define STORY_MAX_SPAWNED 64   // story zombies / items alive at once (saved by name; chapter 1 ends with 31)

typedef struct StoryPoint { char name[STORY_ID]; vec3 pos; float yaw; } StoryPoint;
typedef struct StoryVolume { char name[STORY_ID]; vec3 mn, mx; } StoryVolume;

typedef enum StoryInteractKind { SIK_USE, SIK_READ, SIK_HOLD, SIK_PICKUP } StoryInteractKind;

// A story interactable (the level's story.interact): a named marker made usable.
typedef struct StoryInteract {
    char name[STORY_ID];
    u8 kind;
    char prompt[STORY_ARG];
    char signal[STORY_ID];
    char show[STORY_ARG];       // script id shown to the user (document / message)
    char audio[STORY_ARG];      // played at the object (VO ids start with "vo.")
    char subtitle[STORY_ARG];
    char read[STORY_ID];        // collectible / item id reported as read
    char peek[STORY_ID];        // door to peek through
    char when[STORY_ID];        // usable only while this objective is active ("" = always)
    char blocked[STORY_ARG];    // the prompt otherwise ("" = no prompt)
    char move_ent[STORY_ID], move_to[STORY_ID];   // push a prop to a point (barricade)
    InteractDef def;            // per object (hold time)
} StoryInteract;

typedef struct StoryPowerGroup { char name[STORY_ID]; u32 groups[16]; int group_n; u8 state; } StoryPowerGroup;

typedef struct StorySpawned { char name[STORY_ID]; char character[STORY_ID]; bool chase; float wander; } StorySpawned;

static struct {
    // the level's story block (host + client)
    char level[64];
    StoryPoint points[STORY_MAX_POINTS];
    int point_n;
    StoryVolume volumes[STORY_MAX_VOLUMES];
    int volume_n;
    StoryInteract inter[STORY_MAX_INTERACT];
    int inter_n;
    StoryPowerGroup power[STORY_MAX_POWER];
    int power_n;
    // host chapter
    bool active;
    StoryDef *def;
    StoryState *st;
    StoryOut *out;
    StoryScript script;
    bool script_ok;
    char pending[STORY_ID];     // chapter waiting for its map
    char *pending_save;         // save JSON to restore once the map is loaded
    float last_clock;
    float dead_time;
    vec3 spawn_pos[MAX_PLAYERS];
    bool moved[MAX_PLAYERS];
    u32 vol_inside[STORY_MAX_VOLUMES];   // player bits per volume entry
    u16 item_count[MAX_PLAYERS][256];
    u8 door_open[MAX_ENTITIES];
    u8 named_alive[MAX_ENTITIES];
    u8 light_saved[MAX_ENTITIES];        // EF_ON before a power cut (bit 1 = valid)
    StorySpawned spawned[STORY_MAX_SPAWNED];
    int spawned_n;
    struct { char name[STORY_ID]; float t0; } vseq[24];   // started vista sequences (story seconds; game_vista.c)
    int vseq_n;
    float end_left;
    bool reload_pending;                 // the host died: back to the last checkpoint (outside the sim tick)
    bool menu_pending;                   // the chapter ended: back to the menu (outside the sim tick)
    bool bot;                            // story_bot: the solver drives events (no movement)
    StoryBot botst;
    // presentation (every machine)
    StoryUi ui;
    StoryDef *ui_def;                    // client copy (the host's UI shares SR.def)
    StoryState *ui_mirror;
    StoryScript ui_script;
    bool ui_script_ok;
    float camhint_left, camhint_strength;
    vec3 camhint_pos;
    float look_shown;
    Cvar *save_dir, *autosave;
} SR;

bool story_active(void) { return SR.active; }
bool story_solo(void) { return SR.active && SR.def && SR.def->players <= 1; }
const StoryUi *story_ui(void) { return &SR.ui; }

// ------------------------------------------------------------------ files

static char *read_text(const char *rel) {
    char buf[512];
    return platform_read_file(platform_data_path(rel, buf, sizeof(buf)), NULL);
}

static StoryDef *load_def(const char *chapter, StoryScript *script, bool *script_ok) {
    char path[160], err[256];
    snprintf(path, sizeof(path), "assets/story/chapters/%s.json", chapter);
    char *text = read_text(path);
    if (!text) { log_error("story: cannot read %s", path); return NULL; }
    StoryDef *d = malloc(sizeof(StoryDef));
    bool ok = story_def_parse(d, text, err, sizeof(err));
    free(text);
    if (!ok) { log_error("story: %s: %s", path, err); free(d); return NULL; }
    char lint[1024];
    int problems = story_def_validate(d, lint, sizeof(lint));
    if (problems) log_warn("story: %s: %d lint problem(s):\n%s", path, problems, lint);
    *script_ok = false;
    char *st = d->script[0] ? read_text(d->script) : NULL;
    if (st) {
        *script_ok = story_script_parse(script, st, err, sizeof(err));
        if (!*script_ok) log_error("story: %s: %s", d->script, err);
        free(st);
    } else if (d->script[0]) {
        log_warn("story: cannot read %s (texts show their ids)", d->script);
    }
    return d;
}

bool story_save_path(const char *chapter, const char *slot, char *out, int size) {
    const char *dir = SR.save_dir && SR.save_dir->str[0] ? SR.save_dir->str : NULL;
    char base[512];
    if (dir) snprintf(base, sizeof(base), "%s", dir);
    else snprintf(base, sizeof(base), "%ssaves/story", platform_pref_dir());
    SDL_CreateDirectory(base);
    return snprintf(out, (size_t)size, "%s/%s_%s.json", base, chapter, slot) < size;
}

// ------------------------------------------------------------------ the level's story block

static const StoryPoint *find_point(const char *name) {
    for (int i = 0; i < SR.point_n; i++)
        if (!strcmp(SR.points[i].name, name)) return &SR.points[i];
    return NULL;
}

bool story_point_pos(const char *name, vec3 *out) {
    const StoryPoint *p = find_point(name);
    if (p) *out = p->pos;
    return p != NULL;
}

static StoryInteract *find_inter(const char *name) {
    for (int i = 0; i < SR.inter_n; i++)
        if (!strcmp(SR.inter[i].name, name)) return &SR.inter[i];
    return NULL;
}

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static vec3 jvec(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return v3(0, 0, 0);
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}

static void story_focus(World *w, EntityId self, int player, char *prompt, int size);
static bool story_can_use(World *w, EntityId self, int player);
static void story_use(World *w, EntityId self, int player);

static void parse_level_block(const char *map) {
    SR.point_n = SR.volume_n = SR.inter_n = SR.power_n = 0;
    dg_strcpy(SR.level, sizeof(SR.level), map);
    char path[160];
    snprintf(path, sizeof(path), "data/levels/%s.json", map);
    char *text = read_text(path);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *sb = root ? cJSON_GetObjectItemCaseSensitive(root, "story") : NULL;
    if (!sb) { cJSON_Delete(root); return; }
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(sb, "points")) {
        if (SR.point_n >= STORY_MAX_POINTS) break;
        StoryPoint *p = &SR.points[SR.point_n++];
        dg_strcpy(p->name, sizeof(p->name), e->string);
        p->pos = jvec(e, "pos");
        p->yaw = jnum(e, "yaw", 0) * DG_DEG2RAD;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(sb, "volumes")) {
        if (SR.volume_n >= STORY_MAX_VOLUMES) break;
        StoryVolume *v = &SR.volumes[SR.volume_n++];
        dg_strcpy(v->name, sizeof(v->name), jstr(e, "name"));
        v->mn = jvec(e, "min");
        v->mx = jvec(e, "max");
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(sb, "interact")) {
        if (SR.inter_n >= STORY_MAX_INTERACT) break;
        StoryInteract *s = &SR.inter[SR.inter_n++];
        memset(s, 0, sizeof(*s));
        dg_strcpy(s->name, sizeof(s->name), jstr(e, "name"));
        const char *k = jstr(e, "kind");
        s->kind = !strcmp(k, "read") ? SIK_READ : !strcmp(k, "hold") ? SIK_HOLD : !strcmp(k, "pickup") ? SIK_PICKUP : SIK_USE;
        dg_strcpy(s->prompt, sizeof(s->prompt), jstr(e, "prompt"));
        dg_strcpy(s->signal, sizeof(s->signal), jstr(e, "signal"));
        dg_strcpy(s->show, sizeof(s->show), jstr(e, "show"));
        dg_strcpy(s->audio, sizeof(s->audio), jstr(e, "audio"));
        dg_strcpy(s->subtitle, sizeof(s->subtitle), jstr(e, "subtitle"));
        dg_strcpy(s->read, sizeof(s->read), jstr(e, "read"));
        if (s->kind == SIK_READ && !s->read[0]) dg_strcpy(s->read, sizeof(s->read), s->name);
        dg_strcpy(s->peek, sizeof(s->peek), jstr(e, "peek"));
        dg_strcpy(s->when, sizeof(s->when), jstr(e, "when"));
        dg_strcpy(s->blocked, sizeof(s->blocked), jstr(e, "blocked"));
        const cJSON *mv = cJSON_GetObjectItemCaseSensitive(e, "move");
        dg_strcpy(s->move_ent, sizeof(s->move_ent), jstr(mv, "entity"));
        dg_strcpy(s->move_to, sizeof(s->move_to), jstr(mv, "to"));
        s->def.name = s->kind == SIK_READ ? "story_read" : s->kind == SIK_HOLD ? "story_hold" : s->kind == SIK_PICKUP ? "story_pickup" : "story_use";
        s->def.on_focus = story_focus;
        s->def.can_use = story_can_use;
        s->def.on_use = story_use;
        s->def.hold_time = s->kind == SIK_HOLD ? jnum(e, "hold", 2.0f) : 0.0f;
        s->def.hands = s->kind == SIK_HOLD ? HANDS_BOTH : HANDS_RIGHT;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(sb, "power")) {
        if (SR.power_n >= STORY_MAX_POWER) break;
        StoryPowerGroup *pw = &SR.power[SR.power_n++];
        memset(pw, 0, sizeof(*pw));
        dg_strcpy(pw->name, sizeof(pw->name), e->string);
        pw->state = SPW_ON;
        const cJSON *g;
        cJSON_ArrayForEach(g, e) if (pw->group_n < 16 && cJSON_IsNumber(g)) pw->groups[pw->group_n++] = (u32)g->valueint;
    }
    cJSON_Delete(root);
    log_info("story: level '%s': %d points, %d volumes, %d interactables, %d power groups", map, SR.point_n, SR.volume_n, SR.inter_n, SR.power_n);
}

// Story interactables are markers: give the named ones their definition (host at load; clients as
// their replicas appear, since the definition is not replicated).
static void bind_interactables(World *w) {
    if (!SR.inter_n) return;
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_MARKER || !w->names[i][0] || (w->comps[i] & C_INTERACT)) continue;
        StoryInteract *s = find_inter(w->names[i]);
        if (!s) continue;
        w->comps[i] |= C_INTERACT;
        w->inter[i].def = &s->def;
        w->inter[i].range = 1.9f;
        w->inter[i].enabled = !(w->flags[i] & EF_HIDDEN);
        w->inter[i].focus_offset = v3(0, 0, 0);
    }
}

// "when": the objective that has to be active, or "prefix_*": any objective so named (one per day:
// "sleep_*"). Host: the story state; clients: the replicated mirror.
static bool inter_open(const StoryInteract *s) {
    if (!s || !s->when[0]) return true;
    const StoryDef *d = SR.def ? SR.def : SR.ui.def;
    const StoryState *st = SR.st ? SR.st : SR.ui_mirror;
    if (!d || !st) return false;
    size_t n = strlen(s->when);
    bool prefix = n > 0 && s->when[n - 1] == '*';
    for (int o = 0; o < d->obj_n; o++) {
        bool match = prefix ? !strncmp(d->obj[o].id, s->when, n - 1) : !strcmp(d->obj[o].id, s->when);
        if (match && st->obj[o].state == SOS_ACTIVE) return true;
    }
    return false;
}

static void story_focus(World *w, EntityId self, int player, char *prompt, int size) {
    DG_UNUSED(player);
    StoryInteract *s = find_inter(w->names[ENT_INDEX(self)]);
    if (!inter_open(s)) { snprintf(prompt, (size_t)size, "%s", s->blocked); return; }
    snprintf(prompt, (size_t)size, "%s", s && s->prompt[0] ? s->prompt : "Use");
}

static bool story_can_use(World *w, EntityId self, int player) {
    DG_UNUSED(player);
    return inter_open(find_inter(w->names[ENT_INDEX(self)]));
}

// ------------------------------------------------------------------ events out

static void send(World *w, int target, StoryEvKind kind, const char *text, u32 ent, u32 ent2, float value, vec3 pos) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_STORY;
    ev.target = target < 0 ? EVENT_ALL : (u8)target;
    ev.except = EVENT_ALL;
    ev.id = (u32)kind;
    ev.ent = ent;
    ev.ent2 = ent2;
    ev.value = value;
    ev.pos = pos;
    if (text) dg_strcpy(ev.text, sizeof(ev.text), text);
    ev_push(w, &ev);
}

// Vista sequences (docs/STORY.md "Vista"): the host remembers when each started; clients get the name and
// its age (pos.x: the story clock it was sent at), so a late joiner, a load or a story_time run-ahead
// sees the same city (vista_resync).
static void vseq_start(World *w, const char *name) {
    int k = 0;
    while (k < SR.vseq_n && strcmp(SR.vseq[k].name, name)) k++;
    if (k < SR.vseq_n) {   // restarted: moves to the end (later sequences win)
        memmove(&SR.vseq[k], &SR.vseq[k + 1], sizeof(SR.vseq[0]) * (size_t)(SR.vseq_n - k - 1));
        SR.vseq_n--;
    }
    if (SR.vseq_n >= ARRAY_COUNT(SR.vseq)) return;
    dg_strcpy(SR.vseq[SR.vseq_n].name, STORY_ID, name);
    SR.vseq[SR.vseq_n++].t0 = SR.st->now;
    send(w, -1, SEVT_SEQUENCE, name, 0, 0, 0.0f, v3(SR.st->now, 0, 0));
}

static void vista_resync(World *w, int target) {
    send(w, target, SEVT_SEQUENCE, "", 0, 0, 0, v3(SR.st->now, 0, 0));   // "": forget every sequence
    for (int k = 0; k < SR.vseq_n; k++) send(w, target, SEVT_SEQUENCE, SR.vseq[k].name, 0, 0, SR.st->now - SR.vseq[k].t0, v3(SR.st->now, 0, 0));
}

static void send_sync(World *w, int target) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_STORY;
    ev.target = target < 0 ? EVENT_ALL : (u8)target;
    ev.except = EVENT_ALL;
    ev.id = SEVT_SYNC;
    int n = DG_MIN(SR.def->obj_n, 128);
    ev.data[0] = (u8)n;
    for (int i = 0; i < n; i++) {
        const StoryObjRt *o = &SR.st->obj[i];
        ev.data[1 + i] = (u8)(o->state | (o->revealed ? 16 : 0) | (o->by_role ? 32 : 0));
    }
    for (int c = 0; c < SR.def->collect_n && c < 64; c++)
        if (SR.st->found[c]) ev.data[130 + c / 8] |= (u8)(1u << (c % 8));
    ev_push(w, &ev);
}

// Where an action's name points: a story point, else a named entity.
static bool resolve_pos(World *w, const char *name, vec3 *pos, EntityId *ent) {
    if (ent) *ent = ENT_NONE;
    const StoryPoint *p = find_point(name);
    if (p) { *pos = p->pos; return true; }
    EntityId e = name[0] ? ent_find_by_name(w, name) : ENT_NONE;
    if (e) {
        *pos = w->xf[ENT_INDEX(e)].pos;
        if (ent) *ent = e;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ power, doors, spawns

static bool group_in(const StoryPowerGroup *pw, u32 g) {
    for (int k = 0; k < pw->group_n; k++)
        if (pw->groups[k] == (g & ~LIGHT_GROUP_NO_SHADOW)) return true;
    return false;
}

static void set_power(World *w, const char *name, int state) {
    StoryPowerGroup *pw = NULL;
    for (int i = 0; i < SR.power_n; i++)
        if (!strcmp(SR.power[i].name, name)) pw = &SR.power[i];
    if (!pw) { log_warn("story: unknown power group '%s'", name); return; }
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_LIGHT || !group_in(pw, w->light[i].group)) continue;
        if (state == SPW_OFF && pw->state != SPW_OFF) {
            SR.light_saved[i] = (u8)(2 | ((w->flags[i] & EF_ON) ? 1 : 0));
            w->flags[i] &= ~(u32)EF_ON;
            w->light[i].flicker = 0;
        } else if (state == SPW_ON) {
            if (SR.light_saved[i] & 2) {
                if (SR.light_saved[i] & 1) w->flags[i] |= EF_ON;
                else w->flags[i] &= ~(u32)EF_ON;
            }
            SR.light_saved[i] = 0;
            w->light[i].flicker = 0;
        } else if (state == SPW_FLICKER) {
            w->flags[i] |= EF_ON;
            w->light[i].flicker = 0.9f;
        }
    }
    pw->state = (u8)state;
}

// While a group is off, switches can't bring its lamps back.
static void enforce_power(World *w) {
    for (int p = 0; p < SR.power_n; p++) {
        if (SR.power[p].state != SPW_OFF) continue;
        ENT_FOREACH(w, i) {
            if (w->type[i] == ET_LIGHT && group_in(&SR.power[p], w->light[i].group) && (w->flags[i] & EF_ON)) {
                SR.light_saved[i] = 2 | 1;   // the switch was flipped on: on when the power returns
                w->flags[i] &= ~(u32)EF_ON;
            }
        }
    }
}

static void door_set(World *w, const char *name, int op) {
    EntityId e = ent_find_by_name(w, name);
    if (!e || w->type[ENT_INDEX(e)] != ET_DOOR) { log_warn("story: no door '%s'", name); return; }
    int i = (int)ENT_INDEX(e);
    Door *d = &w->door[i];
    bool open = op != SDO_CLOSE;
    if (open) w->flags[i] &= ~(u32)EF_LOCKED;
    if (open && d->progress < 0.02f) d->swing_sign = 1.0f;
    d->target = open ? 1.0f : 0.0f;
    if (open) w->flags[i] |= EF_OPEN;
    else w->flags[i] &= ~(u32)EF_OPEN;
    d->speed_mult = op == SDO_SLAM ? 3.5f : 1.0f;
    ev_sound(w, op == SDO_SLAM ? "door_slam" : (open ? "door_open" : "door_close"), w->xf[i].pos, op == SDO_SLAM ? 1.6f : 1.0f, EVENT_ALL);
}

static EntityId spawn_named(World *w, const char *name, const char *what, int count, float wander) {
    vec3 pos;
    const StoryPoint *pt = find_point(name);
    if (!pt) { log_warn("story: spawn '%s': no story point of that name", name); return ENT_NONE; }
    pos = pt->pos;
    EntityId e = ENT_NONE;
    if (!strncmp(what, "zombie", 6) || !what[0]) {
        const CharacterDef *cd = g_engine.anim ? anim_lib_char_by_id(g_engine.anim, what[0] ? what : "zombie_placeholder") : NULL;
        if (!cd && g_engine.anim) cd = anim_lib_char_by_id(g_engine.anim, "zombie_placeholder");
        e = spawn_zombie(w, pos, pt->yaw, cd ? cd->net_id : 2);
        CharacterInst *ci = e ? char_get(w, e) : NULL;
        if (ci && wander > 0) ci->ai.wander_radius = wander;   // stays near its spot (the corridor's crowd at 10A's door)
    } else {
        u16 item = item_find(w->items, what);
        if (!item) { log_warn("story: spawn '%s': unknown item / character '%s'", name, what); return ENT_NONE; }
        e = spawn_pickup(w, v3_add(pos, v3(0, 0.3f, 0)), item, count > 0 ? count : 1, 0, 0);
    }
    if (e) {
        dg_strcpy(w->names[ENT_INDEX(e)], sizeof(w->names[0]), name);
        SR.named_alive[ENT_INDEX(e)] = 1;
        if (SR.spawned_n >= STORY_MAX_SPAWNED) log_warn("story: spawn '%s': more than %d story spawns (not saved, never woken)", name, STORY_MAX_SPAWNED);
        if (SR.spawned_n < STORY_MAX_SPAWNED) {
            StorySpawned *s = &SR.spawned[SR.spawned_n++];
            dg_strcpy(s->name, sizeof(s->name), name);
            dg_strcpy(s->character, sizeof(s->character), what);
            s->chase = false;
            s->wander = wander;
        }
    }
    return e;
}

static void give_all(World *w, const char *item_id, int count, bool take) {
    u16 item = item_find(w->items, item_id);
    if (!item) { log_warn("story: unknown item '%s'", item_id); return; }
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (!p->active) continue;
        if (take) inv_remove(&p->inv, item, count);
        else inv_add(&p->inv, w->items, item, count, 0);
        p->inv.revision++;
    }
}

// ------------------------------------------------------------------ notes -> world / events

static void apply_action(World *w, const StoryAction *a) {
    vec3 pos = v3(0, 0, 0);
    EntityId ent = ENT_NONE;
    switch ((StoryActType)a->type) {
    case SA_SPAWN: spawn_named(w, a->name, a->arg, a->ival, a->num[0]); break;
    case SA_DESPAWN: {
        EntityId e = ent_find_by_name(w, a->name);
        if (e) ent_destroy(w, e);
        break;
    }
    case SA_AUDIO: {
        bool at = a->name[0] && resolve_pos(w, a->name, &pos, &ent);
        send(w, -1, SEVT_AUDIO, a->arg, at ? 1u : 0u, (u32)a->ival, a->num[0], pos);
        break;
    }
    case SA_SUBTITLE: send(w, -1, SEVT_SUBTITLE, a->arg, 0, 0, 0, pos); break;
    case SA_WEATHER: weather_set_preset(w, a->arg, a->num[0]); break;
    case SA_UNLOCK: case SA_LOCK: {
        EntityId e = ent_find_by_name(w, a->name);
        if (!e) { log_warn("story: no entity '%s' to %s", a->name, a->type == SA_LOCK ? "lock" : "unlock"); break; }
        if (a->type == SA_LOCK) w->flags[ENT_INDEX(e)] |= EF_LOCKED;
        else w->flags[ENT_INDEX(e)] &= ~(u32)EF_LOCKED;
        break;
    }
    case SA_GIVE: give_all(w, a->arg, a->ival, false); break;
    case SA_TAKE: give_all(w, a->arg, a->ival, true); break;
    case SA_MARKER:
        resolve_pos(w, a->name, &pos, &ent);
        send(w, -1, SEVT_MARKER, a->arg[0] ? a->arg : a->name, ent, (u32)a->ival, 0, pos);
        break;
    case SA_CAMHINT:
        if (resolve_pos(w, a->name, &pos, &ent)) {
            GameEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EV_STORY;
            ev.target = EVENT_ALL;
            ev.except = EVENT_ALL;
            ev.id = SEVT_CAMHINT;
            ev.pos = pos;
            ev.value = a->num[0];
            ev.dir = v3(a->num[1], 0, 0);
            ev_push(w, &ev);
        }
        break;
    case SA_FADE: send(w, -1, SEVT_FADE, NULL, 0, (u32)a->ival, a->num[0], pos); break;
    case SA_POWER: set_power(w, a->name, a->ival); break;
    case SA_LIGHT: {
        char *end;
        long g = strtol(a->name, &end, 10);
        ENT_FOREACH(w, i) {
            if (w->type[i] != ET_LIGHT) continue;
            bool hit = *end ? !strcmp(w->names[i], a->name) : (w->light[i].group & ~LIGHT_GROUP_NO_SHADOW) == (u32)g;
            if (!hit) continue;
            if (a->ival) w->flags[i] |= EF_ON;
            else w->flags[i] &= ~(u32)EF_ON;
        }
        break;
    }
    case SA_DOOR: door_set(w, a->name, a->ival); break;
    case SA_SEQUENCE: vseq_start(w, a->name); break;
    case SA_PHONE: send(w, -1, SEVT_PHONE, a->arg, 0, 0, 0, pos); break;
    case SA_TITLE: send(w, -1, SEVT_TITLE, a->arg, 0, 0, a->num[0], pos); break;
    case SA_CHASE: {
        // The story's idle zombies named <prefix>... wake up: the horde senses, chases and attacks with them.
        size_t n = strlen(a->name);
        int woke = 0;
        for (int k = 0; k < SR.spawned_n; k++) {
            StorySpawned *sp = &SR.spawned[k];
            if (sp->chase || strncmp(sp->name, a->name, n)) continue;
            EntityId z = ent_find_by_name(w, sp->name);
            if (z && horde_adopt(w, z)) { sp->chase = true; woke++; }
        }
        log_info("story: chase '%s': %d zombie(s)", a->name, woke);
        break;
    }
    case SA_TELEPORT: {
        // Waking up somewhere else (a night passed): every player, a little rested.
        const StoryPoint *pt = find_point(a->name);
        if (!pt) { log_warn("story: teleport '%s': no story point of that name", a->name); break; }
        for (int s = 0; s < MAX_PLAYERS; s++) {
            Player *p = &w->players[s];
            if (!p->active || (w->flags[ENT_INDEX(p->ent)] & EF_DEAD)) continue;
            float hp = w->health[ENT_INDEX(p->ent)].hp;
            game_respawn_player_at(w, s, pt->pos, pt->yaw, fminf(100.0f, hp + 25.0f));
            if (p->local && G) G->view.yaw = pt->yaw, G->view.pitch = 0;
        }
        break;
    }
    default: break;
    }
}

static bool save_slot(World *w, const char *slot);

static void apply_notes(World *w, const StoryOut *o) {
    for (int k = 0; k < o->count; k++) {
        const StoryNote *n = &o->n[k];
        switch ((StoryNoteKind)n->kind) {
        case SN_ACTION: apply_action(w, n->act); break;
        case SN_OBJECTIVE: {
            const StoryObjRt *r = &SR.st->obj[n->ref];
            send(w, -1, SEVT_OBJECTIVE, NULL, (u32)n->ref, (u32)(r->state | (r->revealed ? 16 : 0) | (r->by_role ? 32 : 0)), 0, v3(0, 0, 0));
            log_info("story: objective %s -> %s", SR.def->obj[n->ref].id, story_obj_state_name((StoryObjState)r->state));
            break;
        }
        case SN_JOURNAL: send(w, -1, SEVT_JOURNAL, NULL, (u32)n->ref, 0, 0, v3(0, 0, 0)); break;
        case SN_CHECKPOINT:
            log_info("story: checkpoint %s", SR.def->checkpoint[n->ref]);
            if (SR.autosave->i) save_slot(w, "auto");
            break;
        case SN_PROMPT: send(w, -1, SEVT_PROMPT, NULL, (u32)n->ref, 0, 0, v3(0, 0, 0)); break;
        case SN_TRIGGER: log_info("story: trigger %s", SR.def->trig[n->ref].id); break;
        case SN_END:
            log_info("story: chapter %s ended (%.0f s)", SR.def->id, SR.st->now);
            SR.end_left = 8.0f;
            send(w, -1, SEVT_END, SR.def->id, 0, 0, SR.end_left, v3(0, 0, 0));
            break;
        }
    }
    if (o->dropped) log_warn("story: %d notes dropped this tick", o->dropped);
}

// ------------------------------------------------------------------ interactions (host)

static void story_use(World *w, EntityId self, int player) {
    int i = (int)ENT_INDEX(self);
    StoryInteract *s = find_inter(w->names[i]);
    if (!s || !SR.active) return;
    // SEV_INTERACT comes from story_on_interact (every named entity); the kind adds the rest.
    if (s->read[0]) story_event(SR.st, s->kind == SIK_PICKUP ? SEV_ITEM_PICKUP : SEV_ITEM_READ, s->read);
    if (s->signal[0]) story_event(SR.st, SEV_SIGNAL, s->signal);
    if (s->show[0]) send(w, player, SEVT_SHOW, s->show, 0, 0, 0, v3(0, 0, 0));
    if (s->audio[0]) send(w, -1, SEVT_AUDIO, s->audio, 1, !strncmp(s->audio, "vo.", 3) ? SAK_VO : SAK_SFX, 1.0f, w->xf[i].pos);
    if (s->subtitle[0]) send(w, -1, SEVT_SUBTITLE, s->subtitle, 0, 0, 0, v3(0, 0, 0));
    if (s->peek[0]) {
        EntityId door = ent_find_by_name(w, s->peek);
        if (door) send(w, player, SEVT_PEEK, NULL, door, 0, 0, v3(0, 0, 0));
    }
    if (s->move_ent[0]) {
        EntityId e = ent_find_by_name(w, s->move_ent);
        const StoryPoint *to = find_point(s->move_to);
        if (e && to) {
            vec3 p = to->pos;
            p.y += w->vis[ENT_INDEX(e)].half.y;
            ent_set_pos(w, e, p, quat_yaw(to->yaw), true);
            ev_sound(w, "door_close", p, 0.8f, EVENT_ALL);
        }
    }
    if (s->kind == SIK_PICKUP || s->kind == SIK_READ) {
        if (s->kind == SIK_PICKUP) {
            w->flags[i] |= EF_HIDDEN;
            w->inter[i].enabled = false;
        }
    }
}

void story_on_interact(World *w, EntityId e, int slot) {
    DG_UNUSED(slot);
    if (!SR.active || !ent_valid(w, e)) return;
    const char *name = w->names[ENT_INDEX(e)];
    if (name[0]) story_event(SR.st, SEV_INTERACT, name);
}

// ------------------------------------------------------------------ world polling (host)

static bool view_inside(void *user, const char *volume, bool all_players) {
    World *w = user;
    int in = 0, players = 0;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (!p->active || p->sim.move.mode == MOVE_DEAD) continue;
        players++;
        vec3 f = p->sim.move.pos;
        bool here = false;
        for (int v = 0; v < SR.volume_n && !here; v++) {
            const StoryVolume *vo = &SR.volumes[v];
            here = !strcmp(vo->name, volume) && f.x >= vo->mn.x && f.x <= vo->mx.x && f.y >= vo->mn.y && f.y <= vo->mx.y && f.z >= vo->mn.z && f.z <= vo->mx.z;
        }
        in += here;
    }
    return all_players ? (players > 0 && in == players) : in > 0;
}

static bool view_has(void *user, const char *item) {
    World *w = user;
    u16 id = item_find(w->items, item);
    for (int s = 0; s < MAX_PLAYERS && id; s++)
        if (w->players[s].active && inv_has(&w->players[s].inv, id)) return true;
    return false;
}

static void poll_world(World *w) {
    StoryState *st = SR.st;
    // Volumes: enter / exit per entry (a name with several boxes reports each box).
    for (int v = 0; v < SR.volume_n; v++) {
        const StoryVolume *vo = &SR.volumes[v];
        u32 bits = 0;
        for (int s = 0; s < MAX_PLAYERS; s++) {
            Player *p = &w->players[s];
            if (!p->active || p->sim.move.mode == MOVE_DEAD) continue;
            vec3 f = p->sim.move.pos;
            if (f.x >= vo->mn.x && f.x <= vo->mx.x && f.y >= vo->mn.y && f.y <= vo->mx.y && f.z >= vo->mn.z && f.z <= vo->mx.z) bits |= 1u << s;
        }
        if (bits & ~SR.vol_inside[v]) story_event(st, SEV_VOLUME_ENTER, vo->name);
        if (SR.vol_inside[v] & ~bits) story_event(st, SEV_VOLUME_EXIT, vo->name);
        SR.vol_inside[v] = bits;
    }
    // Items: any increase of a player's count is a pickup (containers, pickups, gifts).
    int items = DG_MIN(w->items->item_count, 256);
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (!p->active) continue;
        for (int it = 1; it < items; it++) {
            int c = inv_count(&p->inv, (u16)it);
            if (c > SR.item_count[s][it]) story_event(st, SEV_ITEM_PICKUP, item_def(w->items, (u16)it)->id);
            SR.item_count[s][it] = (u16)c;
        }
        // The first real move after spawning ("wake up").
        if (!SR.moved[s] && v3_len(v3_flat(v3_sub(p->sim.move.pos, SR.spawn_pos[s]))) > 0.6f) {
            SR.moved[s] = true;
            story_event(st, SEV_SIGNAL, "player_moved");
        }
    }
    // Doors open / close, named characters killed or gone.
    for (int i = 1; i < w->max_used; i++) {
        if (!SR.named_alive[i]) continue;
        bool alive = w->alive[i] && !(w->flags[i] & EF_DEAD);
        if (w->alive[i] && w->type[i] == ET_DOOR) {
            u8 open = w->door[i].target > 0.5f;
            if (open != SR.door_open[i]) story_event(st, open ? SEV_ENTITY_OPENED : SEV_ENTITY_CLOSED, w->names[i]);
            SR.door_open[i] = open;
        }
        if (!alive) {
            story_event(st, SEV_ENTITY_KILLED, w->names[i]);
            SR.named_alive[i] = 0;
        }
    }
}

static void track_named(World *w) {
    memset(SR.named_alive, 0, sizeof(SR.named_alive));
    ENT_FOREACH(w, i) {
        if (!w->names[i][0] || w->type[i] == ET_PLAYER) continue;
        SR.named_alive[i] = 1;
        if (w->type[i] == ET_DOOR) SR.door_open[i] = w->door[i].target > 0.5f;
    }
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        int items = DG_MIN(w->items->item_count, 256);
        for (int it = 1; it < items; it++) SR.item_count[s][it] = p->active ? (u16)inv_count(&p->inv, (u16)it) : 0;
    }
}

// ------------------------------------------------------------------ saves: the world blob

static cJSON *slots_json(const InvSlot *slots, int n) {
    cJSON *a = cJSON_CreateArray();
    for (int k = 0; k < n; k++) {
        cJSON *e = cJSON_CreateArray();
        cJSON_AddItemToArray(e, cJSON_CreateNumber(slots[k].item));
        cJSON_AddItemToArray(e, cJSON_CreateNumber(slots[k].count));
        cJSON_AddItemToArray(e, cJSON_CreateNumber(slots[k].ammo));
        cJSON_AddItemToArray(a, e);
    }
    return a;
}

static void slots_read(const cJSON *a, InvSlot *slots, int n) {
    memset(slots, 0, sizeof(InvSlot) * (size_t)n);
    int k = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, a) {
        if (k >= n) break;
        slots[k].item = (u16)cJSON_GetArrayItem(e, 0)->valueint;
        slots[k].count = (u16)cJSON_GetArrayItem(e, 1)->valueint;
        slots[k].ammo = (i16)cJSON_GetArrayItem(e, 2)->valueint;
        k++;
    }
}

static cJSON *world_blob(World *w) {
    cJSON *root = cJSON_CreateObject();
    cJSON *doors = cJSON_AddObjectToObject(root, "doors"), *conts = cJSON_AddObjectToObject(root, "containers");
    cJSON *lights = cJSON_AddObjectToObject(root, "lights"), *props = cJSON_AddObjectToObject(root, "props");
    cJSON *hidden = cJSON_AddArrayToObject(root, "hidden"), *alive = cJSON_AddArrayToObject(root, "alive");
    ENT_FOREACH(w, i) {
        const char *n = w->names[i];
        if (!n[0] || w->type[i] == ET_PLAYER) continue;
        cJSON_AddItemToArray(alive, cJSON_CreateString(n));
        if (w->flags[i] & EF_HIDDEN) cJSON_AddItemToArray(hidden, cJSON_CreateString(n));
        switch (w->type[i]) {
        case ET_DOOR: {
            cJSON *d = cJSON_AddObjectToObject(doors, n);
            cJSON_AddBoolToObject(d, "open", w->door[i].target > 0.5f);
            cJSON_AddBoolToObject(d, "locked", (w->flags[i] & EF_LOCKED) != 0);
            break;
        }
        case ET_CONTAINER: cJSON_AddItemToObject(conts, n, slots_json(w->cont[i].slots, CONTAINER_SLOTS)); break;
        case ET_LIGHT: cJSON_AddBoolToObject(lights, n, (w->flags[i] & EF_ON) || (SR.light_saved[i] & 1)); break;
        case ET_PROP: {
            cJSON *p = cJSON_CreateArray();
            vec3 q = w->xf[i].pos;
            cJSON_AddItemToArray(p, cJSON_CreateNumber(q.x));
            cJSON_AddItemToArray(p, cJSON_CreateNumber(q.y));
            cJSON_AddItemToArray(p, cJSON_CreateNumber(q.z));
            cJSON_AddItemToObject(props, n, p);
            break;
        }
        default: break;
        }
    }
    cJSON *power = cJSON_AddObjectToObject(root, "power");
    for (int p = 0; p < SR.power_n; p++) cJSON_AddNumberToObject(power, SR.power[p].name, SR.power[p].state);
    cJSON *sp = cJSON_AddArrayToObject(root, "spawned");
    for (int k = 0; k < SR.spawned_n; k++) {
        EntityId e = ent_find_by_name(w, SR.spawned[k].name);
        if (!e || (w->flags[ENT_INDEX(e)] & EF_DEAD)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", SR.spawned[k].name);
        cJSON_AddStringToObject(o, "what", SR.spawned[k].character);
        if (SR.spawned[k].chase) cJSON_AddBoolToObject(o, "chase", true);
        if (SR.spawned[k].wander > 0) cJSON_AddNumberToObject(o, "wander", SR.spawned[k].wander);
        cJSON_AddItemToArray(sp, o);
    }
    cJSON_AddStringToObject(root, "weather", weather_name(w));
    cJSON *vs = cJSON_AddObjectToObject(root, "sequences");
    for (int k = 0; k < SR.vseq_n; k++) cJSON_AddNumberToObject(vs, SR.vseq[k].name, SR.vseq[k].t0);
    return root;
}

static bool in_list(const cJSON *arr, const char *name) {
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) if (cJSON_IsString(e) && !strcmp(e->valuestring, name)) return true;
    return false;
}

static void apply_world_blob(World *w, const cJSON *root) {
    if (!root) return;
    const cJSON *alive = cJSON_GetObjectItemCaseSensitive(root, "alive"), *hidden = cJSON_GetObjectItemCaseSensitive(root, "hidden");
    const cJSON *doors = cJSON_GetObjectItemCaseSensitive(root, "doors"), *conts = cJSON_GetObjectItemCaseSensitive(root, "containers");
    const cJSON *lights = cJSON_GetObjectItemCaseSensitive(root, "lights"), *props = cJSON_GetObjectItemCaseSensitive(root, "props");
    ENT_FOREACH(w, i) {
        const char *n = w->names[i];
        if (!n[0] || w->type[i] == ET_PLAYER) continue;
        if (alive && !in_list(alive, n)) { ent_destroy(w, ent_from_index(w, i)); continue; }   // taken / killed / despawned
        if (in_list(hidden, n)) { w->flags[i] |= EF_HIDDEN; w->inter[i].enabled = false; }
        const cJSON *e;
        if (w->type[i] == ET_DOOR && (e = cJSON_GetObjectItemCaseSensitive(doors, n))) {
            bool open = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "open"));
            w->door[i].target = w->door[i].progress = open ? 1.0f : 0.0f;
            if (open) w->flags[i] |= EF_OPEN;
            else w->flags[i] &= ~(u32)EF_OPEN;
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "locked"))) w->flags[i] |= EF_LOCKED;
            else w->flags[i] &= ~(u32)EF_LOCKED;
        }
        if (w->type[i] == ET_CONTAINER && (e = cJSON_GetObjectItemCaseSensitive(conts, n))) {
            slots_read(e, w->cont[i].slots, CONTAINER_SLOTS);
            w->cont[i].revision++;
        }
        if (w->type[i] == ET_LIGHT && (e = cJSON_GetObjectItemCaseSensitive(lights, n))) {
            if (cJSON_IsTrue(e)) w->flags[i] |= EF_ON;
            else w->flags[i] &= ~(u32)EF_ON;
        }
        if (w->type[i] == ET_PROP && (e = cJSON_GetObjectItemCaseSensitive(props, n)) && cJSON_GetArraySize(e) >= 3) {
            vec3 p = v3((float)cJSON_GetArrayItem(e, 0)->valuedouble, (float)cJSON_GetArrayItem(e, 1)->valuedouble, (float)cJSON_GetArrayItem(e, 2)->valuedouble);
            ent_set_pos(w, ent_from_index(w, i), p, w->xf[i].rot, true);
        }
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "power")) {
        for (int p = 0; p < SR.power_n; p++)
            if (!strcmp(SR.power[p].name, e->string) && cJSON_IsNumber(e)) set_power(w, e->string, e->valueint);
    }
    SR.spawned_n = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "spawned")) {
        EntityId z = spawn_named(w, jstr(e, "name"), jstr(e, "what"), 1, jnum(e, "wander", 0));
        if (z && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "chase")) && horde_adopt(w, z)) SR.spawned[SR.spawned_n - 1].chase = true;
    }
    const char *wx = jstr(root, "weather");
    if (wx[0] && strcmp(wx, "custom")) weather_set_preset(w, wx, 0);
    SR.vseq_n = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "sequences")) {
        if (SR.vseq_n >= ARRAY_COUNT(SR.vseq) || !cJSON_IsNumber(e)) continue;
        dg_strcpy(SR.vseq[SR.vseq_n].name, STORY_ID, e->string);
        SR.vseq[SR.vseq_n++].t0 = (float)e->valuedouble;
    }
}

static cJSON *players_blob(World *w) {
    cJSON *arr = cJSON_CreateArray();
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (!p->active) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", s);
        cJSON *pos = cJSON_AddArrayToObject(o, "pos");
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(p->sim.move.pos.x));
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(p->sim.move.pos.y));
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(p->sim.move.pos.z));
        cJSON_AddNumberToObject(o, "yaw", p->sim.move.yaw);
        cJSON_AddNumberToObject(o, "hp", w->health[ENT_INDEX(p->ent)].hp);
        cJSON_AddItemToObject(o, "inventory", slots_json(p->inv.slots, INV_SLOTS));
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static void apply_player(World *w, int s, const cJSON *players) {
    const cJSON *o;
    cJSON_ArrayForEach(o, players) {
        if ((int)jnum(o, "slot", -1) != s) continue;
        Player *p = &w->players[s];
        if (!p->active) return;
        float hp = jnum(o, "hp", 100);
        game_respawn_player_at(w, s, jvec(o, "pos"), jnum(o, "yaw", 0), hp > 1 ? hp : 100);
        slots_read(cJSON_GetObjectItemCaseSensitive(o, "inventory"), p->inv.slots, INV_SLOTS);
        p->inv.revision++;
        if (p->local && G) G->view.yaw = p->sim.move.yaw;
        SR.spawn_pos[s] = p->sim.move.pos;
        return;
    }
}

static bool save_slot(World *w, const char *slot) {
    if (!SR.active) return false;
    cJSON *wb = world_blob(w), *pb = players_blob(w);
    char *text = story_save_write(SR.st, SR.def, wb, pb);
    cJSON_Delete(wb);
    cJSON_Delete(pb);
    char path[600];
    bool ok = text && story_save_path(SR.def->id, slot, path, sizeof(path)) && platform_write_file(path, text, strlen(text));
    free(text);
    if (ok) {
        log_info("story: saved %s", path);
        Profile *prof = lobby_profile();   // the menu's profile (it saves itself too: keep it in step)
        snprintf(prof->last_session, sizeof(prof->last_session), "story:%s:%s", SR.def->id, slot);
        profile_save(prof);   // no-op in --test runs
    } else {
        log_warn("story: save to slot '%s' failed", slot);
    }
    return ok;
}

bool story_continue_available(char *chapter, int size) {
    const Profile *prof = lobby_profile();
    if (strncmp(prof->last_session, "story:", 6)) return false;
    char ch[STORY_ID] = "", slot[32] = "";
    if (sscanf(prof->last_session + 6, "%47[^:]:%31s", ch, slot) != 2) return false;
    char path[600];
    if (!story_save_path(ch, slot, path, sizeof(path)) || !platform_file_exists(path)) return false;
    if (chapter) dg_strcpy(chapter, (size_t)size, ch);
    return true;
}

// ------------------------------------------------------------------ chapter lifecycle

static void ui_set_chapter(const char *id);

static void free_chapter(void) {
    if (SR.ui.def && SR.ui.def == SR.def) ui_set_chapter("");   // the HUD shared this definition
    free(SR.def);
    free(SR.st);
    free(SR.out);
    if (SR.script_ok) story_script_free(&SR.script);
    SR.def = NULL;
    SR.st = NULL;
    SR.out = NULL;
    SR.script_ok = false;
    SR.active = false;
}

void story_stop(void) {
    if (SR.active) log_info("story: stopped");
    if (SR.active && SR.def && SR.def->zombie_sense > 0) horde_set_sense(0);
    free_chapter();
    free(SR.pending_save);
    SR.pending_save = NULL;
    SR.pending[0] = 0;
    SR.bot = false;
    if (G && G->world && G->world->authority) send(G->world, -1, SEVT_CHAPTER, "", 0, 0, 0, v3(0, 0, 0));
}

bool story_start(const char *chapter, const char *slot) {
    StoryScript tmp;
    bool tmp_ok = false;
    StoryDef *d = load_def(chapter, &tmp, &tmp_ok);
    if (!d) return false;
    char *save = NULL;
    if (slot && slot[0]) {
        char path[600];
        if (story_save_path(chapter, slot, path, sizeof(path))) save = platform_read_file(path, NULL);
        if (!save) log_warn("story: no save '%s' for %s, starting from the beginning", slot, chapter);
    }
    char map[STORY_ID];
    dg_strcpy(map, sizeof(map), d->map);
    bool solo = d->players <= 1;
    free(d);
    if (tmp_ok) story_script_free(&tmp);
    story_stop();
    dg_strcpy(SR.pending, sizeof(SR.pending), chapter);
    SR.pending_save = save;
    if (mainmenu_active()) mainmenu_session_started();
    // A single-player chapter is not reachable on the LAN (and game_sv_player_join refuses joins).
    if (!game_host_ex(map, solo ? GH_NO_AUTOHOST : 0)) { story_stop(); return false; }
    return SR.active;
}

static void activate(World *w) {
    bool ok = false;
    SR.def = load_def(SR.pending, &SR.script, &SR.script_ok);
    if (!SR.def) { SR.pending[0] = 0; return; }
    SR.st = malloc(sizeof(StoryState));
    SR.out = malloc(sizeof(StoryOut));
    story_init(SR.st, SR.def, (u64)dg_hash_str(SR.def->id));
    SR.active = true;
    if (SR.def->zombie_sense > 0) horde_set_sense(SR.def->zombie_sense);   // story zombies wake up near the player
    SR.pending[0] = 0;
    SR.spawned_n = 0;
    SR.vseq_n = 0;
    SR.end_left = 0;
    SR.dead_time = 0;
    memset(SR.light_saved, 0, sizeof(SR.light_saved));
    memset(SR.vol_inside, 0, sizeof(SR.vol_inside));
    bind_interactables(w);
    // Clock: the story drives the time of day; season / weather from the start conditions.
    weather_set_preset(w, SR.def->start_weather, 0);
    daynight_story_drive(w, SR.def->start_time / 60.0f, 0);
    SR.last_clock = SR.def->start_time;
    cJSON *wb = NULL, *pb = NULL;
    if (SR.pending_save) {
        char err[160];
        int unknown = 0;
        ok = story_save_read(SR.st, SR.def, SR.pending_save, &wb, &pb, &unknown, err, sizeof(err));
        if (!ok) {
            log_error("story: %s", err);
            story_init(SR.st, SR.def, (u64)dg_hash_str(SR.def->id));
        } else if (unknown) {
            log_warn("story: the save names %d entries this chapter no longer has", unknown);
        }
        free(SR.pending_save);
        SR.pending_save = NULL;
    }
    // Roles: the host plays slot 0's role; its spawn point.
    story_set_role(SR.st, 0, true);
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (!p->active) continue;
        const StoryPoint *sp = NULL;
        for (int r = 0; r < SR.def->role_n; r++)
            if (SR.def->role[r].slot == s) sp = find_point(SR.def->role[r].spawn);
        if (sp) game_respawn_player_at(w, s, sp->pos, sp->yaw, 100.0f);
        if (p->local && G) G->view.yaw = p->sim.move.yaw;
        if (!ok) {
            inv_clear(&p->inv);
            for (int k = 0; k < SR.def->start_item_n; k++) {
                u16 it = item_find(w->items, SR.def->start_items[k]);
                if (it) inv_add(&p->inv, w->items, it, SR.def->start_counts[k], 0);
            }
            p->inv.revision++;
        }
        SR.spawn_pos[s] = p->sim.move.pos;
        SR.moved[s] = false;
    }
    if (ok) {
        apply_world_blob(w, wb);
        for (int s = 0; s < MAX_PLAYERS; s++) apply_player(w, s, pb);
        daynight_story_drive(w, SR.st->clock / 60.0f, 0);
        SR.last_clock = SR.st->clock;
        for (int s = 0; s < MAX_PLAYERS; s++) SR.moved[s] = true;
    }
    cJSON_Delete(wb);
    cJSON_Delete(pb);
    track_named(w);
    send(w, -1, SEVT_CHAPTER, SR.def->id, 0, 0, 0, v3(0, 0, 0));
    send_sync(w, -1);
    vista_resync(w, -1);
    log_info("story: chapter %s '%s' %s (map %s)", SR.def->id, SR.def->title, ok ? "restored" : "started", SR.def->map);
}

void story_level_loaded(World *w, const char *map) {
    ui_set_chapter("");   // a new level: the HUD waits for the chapter's EV_STORY again
    parse_level_block(map);
    vista_level_loaded(map);
    if (!w->authority) return;
    if (SR.active) free_chapter();   // a (re)load: a chapter restarts through story_start
    if (SR.pending[0]) activate(w);
    else if (SR.inter_n) bind_interactables(w);
}

void story_sv_player_joined(World *w, int slot) {
    if (!SR.active) return;
    send(w, slot, SEVT_CHAPTER, SR.def->id, 0, 0, 0, v3(0, 0, 0));
    send_sync(w, slot);
    vista_resync(w, slot);
    for (int r = 0; r < SR.def->role_n; r++) {
        if (SR.def->role[r].slot != slot) continue;
        story_set_role(SR.st, r, true);
        const StoryPoint *sp = find_point(SR.def->role[r].spawn);
        if (sp) game_respawn_player_at(w, slot, sp->pos, sp->yaw, 100.0f);
    }
    SR.spawn_pos[slot] = w->players[slot].sim.move.pos;
    SR.moved[slot] = false;
}

void story_sv_player_left(World *w, int slot) {
    DG_UNUSED(w);
    if (!SR.active) return;
    for (int r = 0; r < SR.def->role_n; r++)
        if (SR.def->role[r].slot == slot && slot != 0) story_set_role(SR.st, r, false);
}

// ------------------------------------------------------------------ host tick

void story_host_tick(World *w) {
    if (!SR.active || !w->authority) return;
    if (SR.st->ended) {
        if (SR.end_left > 0 && (SR.end_left -= SIM_DT) <= 0) SR.menu_pending = true;
        return;
    }
    if (SR.bot) {
        int t = story_bot_step(&SR.botst, SR.st, SR.def, SIM_DT);
        if (t != STORY_NONE) log_info("story_bot: %s (%s)", SR.def->trig[t].id, SR.botst.last_target);
    }
    poll_world(w);
    StoryView v = SR.bot ? story_bot_view(&SR.botst) : (StoryView){ w, view_inside, view_has };
    story_tick(SR.st, SR.def, &v, SIM_DT, SR.out);
    apply_notes(w, SR.out);
    enforce_power(w);
    float rate = (SR.st->clock - SR.last_clock) / SIM_DT;
    SR.last_clock = SR.st->clock;
    daynight_story_drive(w, SR.st->clock / 60.0f, fmaxf(rate, 0.0f));
    // The host's player died: back to the last checkpoint (co-op players respawn normally).
    Player *p = &w->players[0];
    if (p->active && (w->flags[ENT_INDEX(p->ent)] & EF_DEAD)) {
        if ((SR.dead_time += SIM_DT) > 5.0f && !SR.reload_pending) {
            log_info("story: died, back to the last checkpoint");
            SR.reload_pending = true;
        }
    } else {
        SR.dead_time = 0;
    }
}

// ------------------------------------------------------------------ presentation (every machine)

static const StoryScript *ui_script(void) {
    if (SR.ui_def && SR.ui_script_ok) return &SR.ui_script;
    if (!SR.ui_def && SR.script_ok) return &SR.script;
    return NULL;
}

static void ui_set_chapter(const char *id) {
    // Leaving / switching: free the client copy.
    if (SR.ui_def) {
        free(SR.ui_def);
        SR.ui_def = NULL;
        if (SR.ui_script_ok) story_script_free(&SR.ui_script);
        SR.ui_script_ok = false;
    }
    free(SR.ui_mirror);
    SR.ui_mirror = NULL;
    memset(&SR.ui, 0, sizeof(SR.ui));
    SR.ui.prompt = SR.ui.flash_obj = STORY_NONE;
    if (!id[0]) return;
    const StoryDef *d = NULL;
    if (SR.active && !strcmp(SR.def->id, id)) {
        d = SR.def;   // the host shares its definition and script
    } else {
        SR.ui_def = load_def(id, &SR.ui_script, &SR.ui_script_ok);
        d = SR.ui_def;
    }
    if (!d) return;
    SR.ui_mirror = calloc(1, sizeof(StoryState));
    story_init(SR.ui_mirror, d, 1);
    SR.ui.active = true;
    SR.ui.def = d;
    SR.ui.mirror = SR.ui_mirror;
    SR.ui.script = ui_script();
    SR.look_shown = 0;
}

static void add_subtitle(const char *id) {
    const StoryScript *sc = ui_script();
    const StoryEntry *e = sc ? story_script_find(sc, id) : NULL;
    float t = SR.ui.time;
    // Queue after what is still showing.
    for (int i = 0; i < SR.ui.sub_n; i++) t = fmaxf(t, SR.ui.subs[i].start + SR.ui.subs[i].dur);
    if (!e) {
        if (SR.ui.sub_n < STORY_UI_SUBS) {
            StoryUiSub *s = &SR.ui.subs[SR.ui.sub_n++];
            s->speaker[0] = 0;
            dg_strcpy(s->text, sizeof(s->text), id);
            s->start = t;
            s->dur = 3.0f;
        }
        return;
    }
    if (e->cue_n) {
        for (int k = 0; k < e->cue_n && SR.ui.sub_n < STORY_UI_SUBS; k++) {
            const StoryCue *c = &sc->cue[e->first_cue + k];
            StoryUiSub *s = &SR.ui.subs[SR.ui.sub_n++];
            dg_strcpy(s->speaker, sizeof(s->speaker), c->speaker);
            dg_strcpy(s->text, sizeof(s->text), c->text);
            s->start = t + c->start;
            s->dur = c->dur;
        }
    } else if (SR.ui.sub_n < STORY_UI_SUBS) {
        StoryUiSub *s = &SR.ui.subs[SR.ui.sub_n++];
        s->speaker[0] = 0;
        dg_strcpy(s->text, sizeof(s->text), e->body);
        s->start = t;
        s->dur = story_cue_duration((int)strlen(e->body));
    }
}

void story_handle_event(const GameEvent *ev) {
    World *w = G ? G->world : NULL;
    if (ev->id == SEVT_CHAPTER) { ui_set_chapter(ev->text); return; }
    if (!SR.ui.active) return;
    const StoryDef *d = SR.ui.def;
    StoryState *m = SR.ui_mirror;
    switch ((StoryEvKind)ev->id) {
    case SEVT_OBJECTIVE:
        if ((int)ev->ent < d->obj_n) {
            u8 st = (u8)(ev->ent2 & 15u);
            if (st == SOS_DONE && m->obj[ev->ent].state != SOS_DONE && !d->obj[ev->ent].optional) {
                SR.ui.flash_obj = (int)ev->ent;
                SR.ui.flash_time = SR.ui.time;
            }
            m->obj[ev->ent].state = st;
            m->obj[ev->ent].revealed = (ev->ent2 & 16u) != 0;
            m->obj[ev->ent].by_role = (ev->ent2 & 32u) != 0;
        }
        break;
    case SEVT_SYNC: {
        int n = DG_MIN(ev->data[0], d->obj_n);
        for (int i = 0; i < n; i++) {
            m->obj[i].state = ev->data[1 + i] & 15u;
            m->obj[i].revealed = (ev->data[1 + i] & 16u) != 0;
            m->obj[i].by_role = (ev->data[1 + i] & 32u) != 0;
        }
        for (int c = 0; c < d->collect_n && c < 64; c++) m->found[c] = (ev->data[130 + c / 8] >> (c % 8)) & 1u;
        break;
    }
    case SEVT_SUBTITLE: add_subtitle(ev->text); break;
    case SEVT_SHOW:
        dg_strcpy(SR.ui.doc, sizeof(SR.ui.doc), ev->text);
        SR.ui.doc_time = SR.ui.time;
        break;
    case SEVT_PROMPT:
        if ((int)ev->ent < d->prompt_n) m->prompt[ev->ent] = SPS_PENDING;
        break;
    case SEVT_JOURNAL:
        if ((int)ev->ent < d->collect_n) m->found[ev->ent] = 1;
        break;
    case SEVT_FADE:
        SR.ui.fade_from = SR.ui.fade;
        SR.ui.fade_to = ev->ent2 ? 1.0f : 0.0f;
        SR.ui.fade_t = 0;
        SR.ui.fade_len = fmaxf(ev->value, 0.01f);
        if (!ev->ent2 && SR.ui.fade < 0.01f) SR.ui.fade_from = 1.0f;   // a chapter opening on "fade in"
        break;
    case SEVT_AUDIO: {
        int cat = ev->ent2 == SAK_VO ? SND_VOICE : ev->ent2 == SAK_MUSIC ? SND_MUSIC : SND_SFX;
        if (ev->ent) audio_play(ev->text, ev->pos, ev->value, cat);
        else audio_play_2d(ev->text, ev->value, cat);
        break;
    }
    case SEVT_MARKER:
        for (int i = 0; i < STORY_UI_MARKERS; i++) {
            StoryUiMarker *mk = &SR.ui.markers[i];
            bool same = mk->on && (ev->ent ? mk->ent == ev->ent : !strcmp(mk->label, ev->text));
            if (!same && !(ev->ent2 && !mk->on)) continue;
            mk->on = ev->ent2 != 0;
            mk->ent = ev->ent;
            mk->pos = ev->pos;
            dg_strcpy(mk->label, sizeof(mk->label), ev->text);
            break;
        }
        break;
    case SEVT_CAMHINT:
        SR.camhint_pos = ev->pos;
        SR.camhint_strength = ev->value;
        SR.camhint_left = ev->dir.x;
        break;
    case SEVT_PEEK:
        if (w) peek_start((EntityId)ev->ent);
        break;
    case SEVT_SEQUENCE: vista_sequence(ev->text, ev->value, ev->pos.x); break;
    case SEVT_PHONE: log_info("story: phone content '%s'", ev->text); break;
    case SEVT_END:
        SR.ui.ended = true;
        SR.ui.end_time = SR.ui.time;
        break;
    case SEVT_TITLE:
        dg_strcpy(SR.ui.title, sizeof(SR.ui.title), ev->text);
        SR.ui.title_time = SR.ui.time;
        SR.ui.title_len = ev->value > 0 ? ev->value : 4.0f;
        break;
    case SEVT_CHAPTER: break;
    }
}

// Prompts retire when this machine's player performs the action (each player learns separately).
static void prompt_input(float dt) {
    StoryState *m = SR.ui_mirror;
    const StoryDef *d = SR.ui.def;
    int cur = story_prompt_current(m, d);
    if (cur != SR.ui.prompt) {
        SR.ui.prompt = cur;
        SR.ui.prompt_time = SR.ui.time;
    }
    if (cur == STORY_NONE) return;
    const char *a = d->prompt[cur].action;
    bool done = false;
    float shown = SR.ui.time - SR.ui.prompt_time;
    if (!strcmp(a, "move")) done = input_down(ACT_FORWARD) || input_down(ACT_BACK) || input_down(ACT_LEFT) || input_down(ACT_RIGHT);
    else if (!strcmp(a, "look")) done = (SR.look_shown += dt) > 3.0f;
    else if (!strcmp(a, "interact") || !strcmp(a, "push")) done = input_peek_press(ACT_USE);
    else if (!strcmp(a, "hold_interact")) done = input_down(ACT_USE) && shown > 1.0f;
    else if (!strcmp(a, "phone")) done = input_peek_press(ACT_PHONE) || input_peek_press(ACT_PHONE_UI);
    else if (!strcmp(a, "inventory")) done = input_peek_press(ACT_INVENTORY);
    else if (!strcmp(a, "hotbar")) done = input_peek_press(ACT_SLOT1) || input_peek_press(ACT_SLOT2) || input_peek_press(ACT_SLOT3) || input_peek_press(ACT_NEXT_ITEM);
    else if (!strcmp(a, "heal_wheel")) done = input_peek_press(ACT_HEAL_WHEEL);
    else if (!strcmp(a, "attack")) done = input_peek_press(ACT_ATTACK);
    else if (!strcmp(a, "sprint")) done = input_down(ACT_SPRINT);
    else if (!strcmp(a, "journal")) done = SR.ui.journal_open;
    if (!done && shown > 12.0f) done = true;   // never nag: actions without a binding retire themselves
    if (done) m->prompt[cur] = SPS_DONE;
}

void story_frame(World *w, float dt) {
    // Host: deferred world changes (never inside the simulation tick that noticed them).
    if (SR.reload_pending) {
        SR.reload_pending = false;
        char ch[STORY_ID];
        dg_strcpy(ch, sizeof(ch), SR.def ? SR.def->id : "");
        if (ch[0]) story_start(ch, "auto");
        return;
    }
    if (SR.menu_pending) {
        SR.menu_pending = false;
        story_stop();
        if (mainmenu_enabled()) mainmenu_enter("Chapter complete");
        return;
    }
    if (w && SR.inter_n) bind_interactables(w);   // replicas appear over time on clients
    vista_frame(dt, SR.active && SR.st ? &SR.st->now : NULL);
    if (!SR.ui.active) return;
    SR.ui.time += dt;
    // Subtitles: drop finished cues.
    int k = 0;
    for (int i = 0; i < SR.ui.sub_n; i++)
        if (SR.ui.subs[i].start + SR.ui.subs[i].dur > SR.ui.time) SR.ui.subs[k++] = SR.ui.subs[i];
    SR.ui.sub_n = k;
    // Fade.
    if (SR.ui.fade_len > 0) {
        SR.ui.fade_t += dt;
        float t = fminf(1.0f, SR.ui.fade_t / SR.ui.fade_len);
        SR.ui.fade = SR.ui.fade_from + (SR.ui.fade_to - SR.ui.fade_from) * t;
        if (t >= 1.0f) SR.ui.fade_len = 0;
    }
    // Document panel: the use key (or walking away after a moment) closes it.
    if (SR.ui.doc[0] && SR.ui.time - SR.ui.doc_time > 0.35f && input_consume_press(ACT_USE)) SR.ui.doc[0] = 0;
    if (SR.ui.doc[0] && SR.ui.time - SR.ui.doc_time > 20.0f) SR.ui.doc[0] = 0;
    // Markers follow their entities.
    for (int i = 0; i < STORY_UI_MARKERS && w; i++) {
        StoryUiMarker *mk = &SR.ui.markers[i];
        if (mk->on && mk->ent && ent_valid(w, mk->ent)) mk->pos = v3_add(w->xf[ENT_INDEX(mk->ent)].pos, v3(0, 1.0f, 0));
    }
    // Camera hint: a gentle pull on the view (never forced: input still wins).
    if (SR.camhint_left > 0 && G) {
        SR.camhint_left -= dt;
        vec3 to = v3_sub(SR.camhint_pos, G->view.eye);
        float yaw = atan2f(-to.x, -to.z);
        float d = yaw - G->view.yaw;
        while (d > DG_PI) d -= 2.0f * DG_PI;
        while (d < -DG_PI) d += 2.0f * DG_PI;
        G->view.yaw += d * fminf(1.0f, SR.camhint_strength * 2.0f * dt);
    }
    prompt_input(dt);
}

void story_debug_text(char *out, int size) {
    out[0] = 0;
    if (!SR.active) { snprintf(out, (size_t)size, "story: no chapter (host)"); return; }
    int n = snprintf(out, (size_t)size, "story %s  t %.0f s  clock %02d:%02d  beat %d  checkpoint %s%s\n", SR.def->id, SR.st->now,
                     (int)(SR.st->clock / 60.0f), (int)fmodf(SR.st->clock, 60.0f), story_current_beat(SR.st, SR.def),
                     SR.st->checkpoint >= 0 ? SR.def->checkpoint[SR.st->checkpoint] : "-", SR.st->ended ? "  ENDED" : "");
    for (int i = 0; i < SR.def->obj_n && n < size; i++) {
        const StoryObjRt *o = &SR.st->obj[i];
        if (o->state == SOS_INACTIVE) continue;
        int depth = 0;
        for (int p = SR.def->obj[i].parent; p != STORY_NONE; p = SR.def->obj[p].parent) depth++;
        n += snprintf(out + n, (size_t)(size - n), "%*s%s %s%s\n", depth * 2, "", o->state == SOS_DONE ? "[x]" : o->state == SOS_FAILED ? "[!]" : "[ ]",
                      SR.def->obj[i].id, SR.def->obj[i].optional ? " (opt)" : "");
    }
    for (int s = 0; s < SR.st->seq_n && n < size; s++)
        n += snprintf(out + n, (size_t)(size - n), "seq %s pc %d wait %.1f\n", SR.def->trig[SR.st->seq[s].trig].id, SR.st->seq[s].pc, SR.st->seq[s].wait);
}

// ------------------------------------------------------------------ console

static World *host_world(const char *cmd) {
    World *w = G ? G->world : NULL;
    if (!w || !w->authority) { log_info("%s: host only", cmd); return NULL; }
    return w;
}

static void cmd_story_chapter(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: story_chapter <id | number> [save slot]   (assets/story/chapters)"); return; }
    char id[STORY_ID];
    dg_strcpy(id, sizeof(id), argv[1]);
    if (!strcmp(id, "1") || !strcmp(id, "01")) dg_strcpy(id, sizeof(id), "01_tenth_floor");
    story_start(id, argc > 2 ? argv[2] : NULL);
}

static void cmd_story_objective(int argc, const char **argv) {
    if (!host_world("story_objective")) return;
    if (!SR.active || argc < 2) { log_info("usage: story_objective <id> [complete | fail | activate | reveal]"); return; }
    int o = story_find_objective(SR.def, argv[1]);
    if (o == STORY_NONE) { log_info("story_objective: unknown objective '%s'", argv[1]); return; }
    const char *op = argc > 2 ? argv[2] : "complete";
    StoryObjOp k = !strcmp(op, "fail") ? SOO_FAIL : !strcmp(op, "activate") ? SOO_ACTIVATE : !strcmp(op, "reveal") ? SOO_REVEAL : SOO_COMPLETE;
    story_force_objective(SR.st, SR.def, o, k, SR.out);
    apply_notes(G->world, SR.out);
    SR.out->count = 0;
}

static void cmd_story_time(int argc, const char **argv) {
    World *w = host_world("story_time");
    if (!w || !SR.active) return;
    if (argc < 2) { log_info("story_time: clock %02d:%02d (story_time HH:MM sets the clock; story_time <seconds> runs the story ahead)", (int)(SR.st->clock / 60), (int)fmodf(SR.st->clock, 60)); return; }
    if (strchr(argv[1], ':')) {
        float m = story_parse_clock(argv[1]);
        if (m < 0) { log_info("story_time: bad clock '%s'", argv[1]); return; }
        story_set_clock(SR.st, m);
        SR.last_clock = m;
        daynight_story_drive(w, m / 60.0f, 0);
        return;
    }
    // Run the story ahead (timers, waits, the clock); world events still need the world.
    float secs = (float)atof(argv[1]);
    for (float t = 0; t < secs && !SR.st->ended; t += SIM_DT) story_host_tick(w);
    if (SR.active) vista_resync(w, -1);   // the sequences started on the way are older than their events say
}

static void cmd_story_save(int argc, const char **argv) {
    World *w = host_world("story_save");
    if (!w || !SR.active) { log_info("story_save: no chapter running"); return; }
    save_slot(w, argc > 1 ? argv[1] : "manual");
}

static void cmd_story_load(int argc, const char **argv) {
    if (argc < 2 && !SR.active) {
        char ch[STORY_ID];
        if (story_continue_available(ch, sizeof(ch))) story_start(ch, "auto");
        else log_info("story_load: nothing to continue");
        return;
    }
    char ch[STORY_ID];
    dg_strcpy(ch, sizeof(ch), SR.active ? SR.def->id : "01_tenth_floor");
    story_start(ch, argc > 1 ? argv[1] : "auto");
}

static void cmd_story_debug(int argc, const char **argv) {
    SR.ui.debug = argc > 1 ? atoi(argv[1]) != 0 : !SR.ui.debug;
    char buf[4096];
    story_debug_text(buf, sizeof(buf));
    log_info("%s", buf);
}

static void cmd_story_bot(int argc, const char **argv) {
    if (!host_world("story_bot") || !SR.active) { log_info("story_bot: start a chapter first"); return; }
    SR.bot = argc > 1 ? strcmp(argv[1], "off") != 0 : !SR.bot;
    story_bot_init(&SR.botst, argc > 1 ? (u32)atoi(argv[1]) : 0);
    log_info("story_bot %s", SR.bot ? "on (the solver drives the chapter's events)" : "off");
}

static void cmd_story_fire(int argc, const char **argv) {
    if (!host_world("story_fire") || !SR.active || argc < 2) { log_info("usage: story_fire <trigger>"); return; }
    int t = story_find_trigger(SR.def, argv[1]);
    if (!story_fire(SR.st, SR.def, t, SR.out)) { log_info("story_fire: '%s' unknown or spent", argv[1]); return; }
    apply_notes(G->world, SR.out);
    SR.out->count = 0;
}

static void cmd_story_flag(int argc, const char **argv) {
    if (!SR.active || argc < 2) { log_info("usage: story_flag <name> [value]"); return; }
    if (argc > 2) story_set_flag(SR.st, argv[1], atoi(argv[2]));
    log_info("story_flag %s = %d", argv[1], story_flag(SR.st, argv[1]));
}

static void cmd_story_signal(int argc, const char **argv) {
    if (!SR.active || argc < 2) { log_info("usage: story_signal <name>   (choice_open, phone_messages_read, ...)"); return; }
    story_event(SR.st, SEV_SIGNAL, argv[1]);
}

static void cmd_story_stop(int argc, const char **argv) {
    DG_UNUSED(argc);
    DG_UNUSED(argv);
    story_stop();
}

static void cmd_story_journal(int argc, const char **argv) {
    SR.ui.journal_open = argc > 1 ? atoi(argv[1]) != 0 : !SR.ui.journal_open;
}

void story_register(void) {
    SR.save_dir = cvar_reg("story_save_dir", "", CVAR_STRING, CVAR_EDITABLE, "Story saves folder (empty = <pref dir>/saves/story; tests point it at a scratch folder)");
    SR.autosave = cvar_reg_bool("story_autosave", true, CVAR_ARCHIVE | CVAR_EDITABLE, "Story mode: save to slot 'auto' at every checkpoint");
    cmd_register("story_chapter", cmd_story_chapter, "story_chapter <id | 1> [save slot]: start a story chapter (host; docs/STORY.md)");
    cmd_register("story_objective", cmd_story_objective, "story_objective <id> [complete | fail | activate | reveal]: force an objective (host)");
    cmd_register("story_time", cmd_story_time, "story_time <HH:MM | seconds>: set the story clock, or run the story ahead (host)");
    cmd_register("story_save", cmd_story_save, "story_save [slot]: save the running chapter (host)");
    cmd_register("story_load", cmd_story_load, "story_load [slot]: load a story save (default: the last checkpoint)");
    cmd_register("story_debug", cmd_story_debug, "story_debug [0/1]: story overlay (objectives, sequences, clock) + print it");
    cmd_register("story_bot", cmd_story_bot, "story_bot [choice | off]: the solver bot drives the chapter's events (host)");
    cmd_register("story_fire", cmd_story_fire, "story_fire <trigger>: fire a story trigger now (host)");
    cmd_register("story_flag", cmd_story_flag, "story_flag <name> [value]: read / set a story flag (host)");
    cmd_register("story_signal", cmd_story_signal, "story_signal <name>: send a story signal (host)");
    cmd_register("story_stop", cmd_story_stop, "story_stop: end the running story chapter (the map stays)");
    cmd_register("story_journal", cmd_story_journal, "story_journal [0/1]: show the story journal (documents, photos, voicemails found)");
    SR.ui.prompt = SR.ui.flash_obj = STORY_NONE;
}
