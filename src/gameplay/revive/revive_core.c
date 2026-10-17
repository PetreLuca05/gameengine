// Pure downed / revive / corpse / DNA / fabricator state machines (docs/REVIVE.md).
#include "revive_core.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>

// ------------------------------------------------------------------ rules

void revive_rules_defaults(ReviveRules *r) {
    memset(r, 0, sizeof(*r));
    r->downs_enabled = true;
    r->bleed_time = 60.0f;
    r->bleed_scale = 0.6f;
    r->bleed_min = 15.0f;
    r->max_downs = 0;
    r->overkill = 0.0f;
    r->downed_move_speed = 0.6f;
    r->downed_hp = 100.0f;
    r->keep_sidearm = true;
    r->downed_can_shoot = true;
    r->revive_time = 6.0f;
    r->revive_range = 1.6f;
    r->revive_hp = 30.0f;
    r->move_tolerance = 0.35f;
    r->revive_pauses_bleed = true;
    r->revive_resets_downs = false;
    r->corpse_lifetime = 600.0f;
    r->corpse_extract_despawn = 20.0f;
    r->corpse_range = 0.6f;
    r->extract_time = 3.0f;
    r->samples_per_corpse = 1;
    r->extract_needs_crouch = true;
    r->sample_on_death = SAMPLE_DROP;
    r->sample_weight = 0.1f;
    r->insert_time = 5.0f;
    r->insert_range = 1.8f;
    r->recon_time = 10.0f;
    r->open_time = 1.5f;
    r->cooldown = 3.0f;
    r->queue_max = FAB_QUEUE_MAX;
    r->respawn_hp = 60.0f;
    r->eject_on_power_loss = true;
    r->require_power = false;
    r->respawn_mode = RESPAWN_BOTH;
    r->auto_ping_down = true;
    r->auto_ping_death = true;
    r->death_skippable = true;
    r->self_revive = false;
}

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static bool jb(const cJSON *o, const char *k, bool def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    return def;
}
static const char *js(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

const char *respawn_mode_name(RespawnMode m) {
    static const char *n[] = { "timer", "fabricator", "both" };
    return (unsigned)m < 3 ? n[m] : "?";
}

bool revive_rules_parse(ReviveRules *r, const char *json, char *err, int err_size) {
    revive_rules_defaults(r);
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        if (err) snprintf(err, (size_t)err_size, "revive_rules.json: parse error");
        return false;
    }
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "downed");
    r->downs_enabled = jb(d, "enabled", r->downs_enabled);
    r->bleed_time = jf(d, "bleed_time", r->bleed_time);
    r->bleed_scale = jf(d, "bleed_scale", r->bleed_scale);
    r->bleed_min = jf(d, "bleed_min", r->bleed_min);
    r->max_downs = (int)jf(d, "max_downs", (float)r->max_downs);
    r->overkill = jf(d, "overkill", r->overkill);
    r->downed_move_speed = jf(d, "move_speed", r->downed_move_speed);
    r->downed_hp = jf(d, "hp", r->downed_hp);
    r->keep_sidearm = jb(d, "keep_sidearm", r->keep_sidearm);
    r->downed_can_shoot = jb(d, "sidearm_can_shoot", r->downed_can_shoot);
    r->auto_ping_down = jb(d, "auto_ping", r->auto_ping_down);
    r->self_revive = jb(d, "self_revive", r->self_revive);
    const cJSON *rv = cJSON_GetObjectItemCaseSensitive(root, "revive");
    r->revive_time = jf(rv, "time", r->revive_time);
    r->revive_range = jf(rv, "range", r->revive_range);
    r->revive_hp = jf(rv, "hp", r->revive_hp);
    r->move_tolerance = jf(rv, "move_tolerance", r->move_tolerance);
    r->revive_pauses_bleed = jb(rv, "pauses_bleed", r->revive_pauses_bleed);
    r->revive_resets_downs = jb(rv, "resets_downs", r->revive_resets_downs);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "corpse");
    r->corpse_lifetime = jf(c, "lifetime", r->corpse_lifetime);
    r->corpse_extract_despawn = jf(c, "despawn_after_extract", r->corpse_extract_despawn);
    r->corpse_range = jf(c, "range", r->corpse_range);
    r->extract_time = jf(c, "extract_time", r->extract_time);
    r->samples_per_corpse = (int)jf(c, "samples", (float)r->samples_per_corpse);
    r->extract_needs_crouch = jb(c, "crouch", r->extract_needs_crouch);
    r->auto_ping_death = jb(c, "auto_ping", r->auto_ping_death);
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "sample");
    const char *loss = js(s, "on_carrier_death");
    if (loss) r->sample_on_death = !strcmp(loss, "lost") ? SAMPLE_LOST : !strcmp(loss, "keep") ? SAMPLE_KEEP : SAMPLE_DROP;
    r->sample_weight = jf(s, "weight", r->sample_weight);
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "fabricator");
    r->insert_time = jf(f, "insert_time", r->insert_time);
    r->insert_range = jf(f, "insert_range", r->insert_range);
    r->recon_time = jf(f, "reconstruct_time", r->recon_time);
    r->open_time = jf(f, "open_time", r->open_time);
    r->cooldown = jf(f, "cooldown", r->cooldown);
    r->queue_max = (int)jf(f, "queue_max", (float)r->queue_max);
    r->respawn_hp = jf(f, "respawn_hp", r->respawn_hp);
    r->eject_on_power_loss = jb(f, "eject_on_power_loss", r->eject_on_power_loss);
    r->require_power = jb(f, "require_power", r->require_power);
    const cJSON *items = f ? cJSON_GetObjectItemCaseSensitive(f, "respawn_items") : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (r->respawn_item_n >= REVIVE_MAX_RESPAWN_ITEMS) break;
        const cJSON *id = cJSON_IsArray(it) ? cJSON_GetArrayItem(it, 0) : it;
        const cJSON *n = cJSON_IsArray(it) ? cJSON_GetArrayItem(it, 1) : NULL;
        if (!cJSON_IsString(id)) continue;
        dg_strcpy(r->respawn_items[r->respawn_item_n], sizeof(r->respawn_items[0]), id->valuestring);
        r->respawn_item_counts[r->respawn_item_n] = cJSON_IsNumber(n) ? (int)n->valuedouble : 1;
        r->respawn_item_n++;
    }
    const char *mode = js(root, "respawn_mode");
    if (mode) r->respawn_mode = !strcmp(mode, "timer") ? RESPAWN_TIMER : !strcmp(mode, "fabricator") ? RESPAWN_FABRICATOR : RESPAWN_BOTH;
    r->death_skippable = jb(root, "death_skippable", r->death_skippable);
    // Sanity: the state machines count in u16 ticks and u8 queues.
    r->queue_max = DG_CLAMP(r->queue_max, 1, FAB_QUEUE_MAX);
    r->samples_per_corpse = DG_CLAMP(r->samples_per_corpse, 1, 8);
    r->bleed_scale = DG_CLAMP(r->bleed_scale, 0.05f, 1.0f);
    r->bleed_min = fmaxf(r->bleed_min, 1.0f);
    cJSON_Delete(root);
    return true;
}

// ------------------------------------------------------------------ downed

u16 down_bleed_total(const ReviveRules *r, int downs_before) {
    float t = r->bleed_time * powf(r->bleed_scale, (float)(downs_before < 0 ? 0 : downs_before));
    return revive_ticks(fmaxf(t, fminf(r->bleed_min, r->bleed_time)));
}

bool down_should_knock(const ReviveRules *r, const DownState *d, bool kill_type, float overkill_amount) {
    if (!r->downs_enabled || kill_type || d->downed) return false;
    if (r->max_downs > 0 && d->downs >= r->max_downs) return false;
    if (r->overkill > 0 && overkill_amount > r->overkill) return false;
    return true;
}

void down_enter(DownState *d, const ReviveRules *r) {
    d->bleed_total = down_bleed_total(r, d->downs);
    d->bleed_ticks = d->bleed_total;
    d->downed = 1;
    d->downs++;
    d->reviver = REVIVE_NOBODY;
    d->revive_ticks = d->revive_total = 0;
    d->self_reviving = 0;
}

bool down_step(DownState *d, const ReviveRules *r) {
    if (!d->downed) return false;
    bool paused = r->revive_pauses_bleed && (d->reviver != REVIVE_NOBODY || d->self_reviving);
    if (!paused && d->bleed_ticks > 0) d->bleed_ticks--;
    return d->bleed_ticks == 0;
}

void down_revived(DownState *d, const ReviveRules *r) {
    d->downed = 0;
    d->reviver = REVIVE_NOBODY;
    d->revive_ticks = d->revive_total = 0;
    d->bleed_ticks = 0;
    d->self_reviving = 0;
    if (r->revive_resets_downs) d->downs = 0;
}

void down_clear(DownState *d) {
    memset(d, 0, sizeof(*d));
    d->reviver = REVIVE_NOBODY;
}

// ------------------------------------------------------------------ timed actions

ActResult act_step(ActionState *a, const ActInput *in) {
    switch ((ActPhase)a->phase) {
    case ACTP_DONE:
    case ACTP_CANCELLED:
        // Hold through the end: a new action needs a fresh press.
        if (!in->use_down) {
            a->phase = ACTP_IDLE;
            a->kind = ACT_NONE;
            a->target = 0;
        }
        return ACTR_NONE;
    case ACTP_IDLE:
        if (in->use_pressed && in->afford != ACT_NONE && in->target && in->target_ok && in->dur > 0) {
            a->kind = (u8)in->afford;
            a->phase = ACTP_RUNNING;
            a->cancel = ACTC_NONE;
            a->target = in->target;
            a->ticks = 0;
            a->dur = in->dur;
            a->start_pos = in->pos;
            return ACTR_STARTED;
        }
        return ACTR_NONE;
    case ACTP_RUNNING: {
        ActCancel why = ACTC_NONE;
        vec3 d = v3_sub(in->pos, a->start_pos);
        if (!in->use_down) why = ACTC_RELEASE;
        else if (in->damaged) why = ACTC_DAMAGE;
        else if (!in->target_ok) why = ACTC_RANGE;
        else if (d.x * d.x + d.z * d.z > in->move_tolerance * in->move_tolerance) why = ACTC_MOVE;
        if (why != ACTC_NONE) {
            act_cancel(a, why);
            return ACTR_CANCELLED;
        }
        a->ticks++;
        if (a->ticks >= a->dur) {
            a->phase = ACTP_DONE;
            return ACTR_DONE;
        }
        return ACTR_PROGRESS;
    }
    }
    return ACTR_NONE;
}

void act_cancel(ActionState *a, ActCancel why) {
    if (a->phase != ACTP_RUNNING) return;
    a->phase = ACTP_CANCELLED;
    a->cancel = (u8)why;
}

const char *act_kind_name(ActKind k) {
    static const char *n[] = { "none", "revive", "extract", "insert", "self_revive" };
    return (unsigned)k < ACT_COUNT ? n[k] : "?";
}

const char *act_cancel_name(ActCancel c) {
    static const char *n[] = { "", "released", "moved", "damaged", "too far", "target lost", "interrupted" };
    return (unsigned)c <= ACTC_HOST ? n[c] : "?";
}

// ------------------------------------------------------------------ corpse

void corpse_init(CorpseState *c, const ReviveRules *r, int owner_slot, u32 tick) {
    memset(c, 0, sizeof(*c));
    c->death_tick = tick;
    c->owner_slot = (u8)owner_slot;
    c->samples_left = (u8)r->samples_per_corpse;
    c->extractor = REVIVE_NOBODY;
}

bool corpse_extract(CorpseState *c, int extractor, u32 tick) {
    if (!c->samples_left) return false;
    c->samples_left--;
    c->extractor = (u8)extractor;
    if (!c->samples_left) c->extract_tick = tick ? tick : 1;
    return true;
}

bool corpse_should_despawn(const CorpseState *c, const ReviveRules *r, u32 tick) {
    if (tick - c->death_tick >= (u32)(r->corpse_lifetime * REVIVE_HZ)) return true;
    if (c->extract_tick && tick - c->extract_tick >= (u32)(r->corpse_extract_despawn * REVIVE_HZ)) return true;
    return false;
}

float corpse_distance(vec3 feet, vec3 a, vec3 b) {
    // Horizontal closest point on the body segment.
    float abx = b.x - a.x, abz = b.z - a.z;
    float len2 = abx * abx + abz * abz;
    float t = len2 > 1e-6f ? ((feet.x - a.x) * abx + (feet.z - a.z) * abz) / len2 : 0.0f;
    t = saturate(t);
    float px = a.x + abx * t, pz = a.z + abz * t;
    float dx = feet.x - px, dz = feet.z - pz;
    return sqrtf(dx * dx + dz * dz);
}

bool corpse_in_range(const ReviveRules *r, vec3 feet, vec3 a, vec3 b) {
    float lo = fminf(a.y, b.y) - 1.0f, hi = fmaxf(a.y, b.y) + 0.6f;
    if (feet.y < lo || feet.y > hi) return false;
    return corpse_distance(feet, a, b) <= r->corpse_range;
}

// ------------------------------------------------------------------ DNA registry

void dna_reset(DnaRegistry *g) {
    memset(g, 0, sizeof(*g));
    g->next_uid = 1;
}

int dna_alloc(DnaRegistry *g, int player_slot, const char *name, int extractor, u32 avatar, u32 corpse) {
    for (int i = 0; i < DNA_MAX; i++) {
        DnaSample *s = &g->s[i];
        if (s->status != DNA_FREE) continue;
        memset(s, 0, sizeof(*s));
        if (!g->next_uid) g->next_uid = 1;
        s->uid = g->next_uid++;
        s->status = DNA_CARRIED;
        s->player_slot = (u8)player_slot;
        s->carrier = (u8)extractor;
        s->extractor = (u8)extractor;
        s->avatar = avatar;
        s->corpse = corpse;
        dg_strcpy(s->name, sizeof(s->name), name);
        return i + 1;
    }
    return 0;
}

void dna_free(DnaRegistry *g, int id) {
    if (id >= 1 && id <= DNA_MAX) memset(&g->s[id - 1], 0, sizeof(g->s[0]));
}

DnaSample *dna_get(DnaRegistry *g, int id) {
    if (id < 1 || id > DNA_MAX || g->s[id - 1].status == DNA_FREE) return NULL;
    return &g->s[id - 1];
}

const DnaSample *dna_get_c(const DnaRegistry *g, int id) { return dna_get((DnaRegistry *)g, id); }

int dna_find_player(const DnaRegistry *g, int player_slot) {
    int best = 0;
    u32 best_uid = 0;
    for (int i = 0; i < DNA_MAX; i++) {
        const DnaSample *s = &g->s[i];
        if (s->status != DNA_FREE && s->player_slot == player_slot && s->uid >= best_uid) best = i + 1, best_uid = s->uid;
    }
    return best;
}

// ------------------------------------------------------------------ fabricator

void fab_init(FabMachine *f, bool powered) {
    memset(f, 0, sizeof(*f));
    f->powered = powered ? 1 : 0;
    f->state = powered ? FAB_IDLE : FAB_UNPOWERED;
}

bool fab_enqueue(FabMachine *f, const ReviveRules *r, int sample_id) {
    if (sample_id <= 0 || sample_id > 255 || f->qn >= r->queue_max || f->qn >= FAB_QUEUE_MAX) return false;
    if (r->require_power && !f->powered) return false;
    for (int i = 0; i < f->qn; i++) if (f->queue[i] == sample_id) return false;
    if (f->current == sample_id) return false;
    f->queue[f->qn++] = (u8)sample_id;
    return true;
}

bool fab_remove(FabMachine *f, int sample_id) {
    for (int i = 0; i < f->qn; i++) {
        if (f->queue[i] != sample_id) continue;
        memmove(&f->queue[i], &f->queue[i + 1], (size_t)(f->qn - i - 1));
        f->qn--;
        f->queue[f->qn] = 0;
        return true;
    }
    return false;
}

static u32 dur_ticks(float s) { return (u32)revive_ticks(s); }

FabEvent fab_step(FabMachine *f, const ReviveRules *r, u32 tick) {
    u32 el = tick - f->state_tick;
    switch ((FabStateId)f->state) {
    case FAB_UNPOWERED:
        return FABE_NONE;
    case FAB_IDLE:
        if (f->qn == 0) return FABE_NONE;
        f->current = f->queue[0];
        fab_remove(f, f->current);
        f->state = FAB_RECONSTRUCTING;
        f->state_tick = tick;
        return FABE_RECON_START;
    case FAB_RECONSTRUCTING:
        if (el < dur_ticks(r->recon_time)) return FABE_NONE;
        f->state = FAB_OPENING;
        f->state_tick = tick;
        return FABE_RESPAWN;
    case FAB_OPENING:
        if (el < dur_ticks(r->open_time)) return FABE_NONE;
        f->state = FAB_COOLDOWN;
        f->state_tick = tick;
        f->current = 0;
        return FABE_OPENED;
    case FAB_COOLDOWN:
        if (el < dur_ticks(r->cooldown)) return FABE_NONE;
        f->state = FAB_IDLE;
        f->state_tick = tick;
        return FABE_READY;
    default:
        return FABE_NONE;
    }
}

int fab_set_power(FabMachine *f, const ReviveRules *r, bool on, u32 tick, u8 *out, int out_cap) {
    int n = 0;
    if (on == (f->powered != 0)) return 0;
    f->powered = on ? 1 : 0;
    if (on) {
        if (f->state == FAB_UNPOWERED) { f->state = FAB_IDLE; f->state_tick = tick; }
        return 0;
    }
    if (r->eject_on_power_loss) {
        if (f->state == FAB_RECONSTRUCTING && f->current && n < out_cap) out[n++] = f->current;
        for (int i = 0; i < f->qn && n < out_cap; i++) out[n++] = f->queue[i];
        memset(f->queue, 0, sizeof(f->queue));
        f->qn = 0;
    }
    if (f->state == FAB_RECONSTRUCTING || r->eject_on_power_loss) f->current = 0;
    f->state = FAB_UNPOWERED;
    f->state_tick = tick;
    return n;
}

float fab_progress(const FabMachine *f, const ReviveRules *r, u32 tick) {
    if (f->state == FAB_OPENING || f->state == FAB_COOLDOWN) return f->state == FAB_OPENING ? 1.0f : 0.0f;
    if (f->state != FAB_RECONSTRUCTING) return 0.0f;
    u32 d = dur_ticks(r->recon_time);
    return d ? saturate((float)(tick - f->state_tick) / (float)d) : 1.0f;
}

float fab_remaining(const FabMachine *f, const ReviveRules *r, u32 tick) {
    float dur = f->state == FAB_RECONSTRUCTING ? r->recon_time : f->state == FAB_OPENING ? r->open_time : f->state == FAB_COOLDOWN ? r->cooldown : 0.0f;
    float el = (float)(tick - f->state_tick) / (float)REVIVE_HZ;
    return fmaxf(0.0f, dur - el);
}

const char *fab_state_name(FabStateId s) {
    static const char *n[] = { "idle", "reconstructing", "opening", "cooldown", "unpowered" };
    return (unsigned)s < FAB_STATE_COUNT ? n[s] : "?";
}
