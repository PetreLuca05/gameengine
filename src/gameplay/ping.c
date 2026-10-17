// Ping system core (src/gameplay/ping.h). Pure.
#include "ping.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const k_ids[PING_TYPES] = { "generic", "enemy", "loot", "danger", "go", "help", "vehicle", "defend" };

const char *ping_type_id(int type) { return type >= 0 && type < PING_TYPES ? k_ids[type] : "?"; }

int ping_type_from_id(const char *id) {
    for (int i = 0; i < PING_TYPES; i++) if (id && !strcmp(id, k_ids[i])) return i;
    return -1;
}

void ping_types_defaults(PingTypes *pt) {
    static const char *const labels[PING_TYPES] = { "Ping", "Enemy", "Loot", "Danger", "Go Here", "Need Help", "Vehicle", "Defend" };
    static const char *const chats[PING_TYPES] = { "pinged a spot", "spotted an enemy", "found loot", "warns: danger", "wants to go here", "needs help", "found a vehicle", "wants to defend here" };
    static const float life[PING_TYPES] = { 20, 10, 45, 20, 40, 25, 60, 60 };
    memset(pt, 0, sizeof(*pt));
    for (int i = 0; i < PING_TYPES; i++) {
        PingTypeDef *t = &pt->t[i];
        dg_strcpy(t->id, sizeof(t->id), k_ids[i]);
        dg_strcpy(t->label, sizeof(t->label), labels[i]);
        dg_strcpy(t->icon, sizeof(t->icon), k_ids[i]);
        dg_strcpy(t->chat, sizeof(t->chat), chats[i]);
        dg_strcpy(t->sound, sizeof(t->sound), (i == PING_ENEMY || i == PING_DANGER || i == PING_HELP) ? "ping_alert" : "ping");
        t->lifetime = life[i];
        t->color = i;
    }
}

bool ping_types_parse(PingTypes *pt, const char *json, char *err, int err_size) {
    if (err && err_size) err[0] = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) { if (err) snprintf(err, (size_t)err_size, "JSON syntax error"); return false; }
    PingTypes out = *pt;
    const char *bad = NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "slots")) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "id");
        int k = cJSON_IsString(v) ? ping_type_from_id(v->valuestring) : -1;
        if (k < 0) { bad = "unknown ping \"id\""; break; }
        PingTypeDef *t = &out.t[k];
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "label"))) dg_strcpy(t->label, sizeof(t->label), v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "icon"))) dg_strcpy(t->icon, sizeof(t->icon), v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "chat"))) dg_strcpy(t->chat, sizeof(t->chat), v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(e, "sound"))) dg_strcpy(t->sound, sizeof(t->sound), v->valuestring);
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "lifetime"))) t->lifetime = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(e, "color"))) t->color = DG_CLAMP(v->valueint, 0, 7);
        if (t->lifetime < 1.0f || t->lifetime > 600.0f) { bad = "\"lifetime\" must be 1..600 s"; break; }
    }
    cJSON_Delete(root);
    if (bad) { if (err) snprintf(err, (size_t)err_size, "%s", bad); return false; }
    *pt = out;
    return true;
}

Ping *ping_add(PingList *l, u32 uid, int owner, int type, vec3 pos, float now, float lifetime) {
    Ping *slot = NULL;
    for (int i = 0; i < PING_MAX; i++) if (l->p[i].active && l->p[i].uid == uid) slot = &l->p[i];
    if (!slot && ping_count_owner(l, owner) >= PING_PER_PLAYER) {
        // The owner's oldest goes.
        for (int i = 0; i < PING_MAX; i++) {
            Ping *p = &l->p[i];
            if (p->active && p->owner == owner && (!slot || p->born < slot->born || (p->born == slot->born && p->uid < slot->uid))) slot = p;
        }
    }
    if (!slot) for (int i = 0; i < PING_MAX && !slot; i++) if (!l->p[i].active) slot = &l->p[i];
    if (!slot) {
        // Full (more owners than slots): the globally oldest.
        slot = &l->p[0];
        for (int i = 1; i < PING_MAX; i++) if (l->p[i].born < slot->born) slot = &l->p[i];
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->uid = uid;
    slot->owner = (u8)owner;
    slot->type = (u8)DG_CLAMP(type, 0, PING_TYPES - 1);
    slot->pos = pos;
    slot->born = now;
    slot->expires = now + lifetime;
    return slot;
}

bool ping_remove(PingList *l, u32 uid) {
    for (int i = 0; i < PING_MAX; i++) if (l->p[i].active && l->p[i].uid == uid) { l->p[i].active = false; return true; }
    return false;
}

void ping_expire(PingList *l, float now) {
    for (int i = 0; i < PING_MAX; i++) if (l->p[i].active && now >= l->p[i].expires) l->p[i].active = false;
}

int ping_count_owner(const PingList *l, int owner) {
    int n = 0;
    for (int i = 0; i < PING_MAX; i++) n += l->p[i].active && l->p[i].owner == owner;
    return n;
}

int ping_count(const PingList *l) {
    int n = 0;
    for (int i = 0; i < PING_MAX; i++) n += l->p[i].active;
    return n;
}

const Ping *ping_find_near(const PingList *l, int owner, vec3 pos, float radius) {
    const Ping *best = NULL;
    float bd = radius;
    for (int i = 0; i < PING_MAX; i++) {
        const Ping *p = &l->p[i];
        if (!p->active || p->owner != owner) continue;
        float d = v3_dist(p->pos, pos);
        if (d <= bd) { bd = d; best = p; }
    }
    return best;
}

float ping_alpha(const Ping *p, float now) {
    float in = saturate((now - p->born) / 0.2f);
    float out = saturate((p->expires - now) / 1.5f);
    return fminf(in, out);
}

bool ping_rate_ok(PingLimiter *lim, int player, float now) {
    if (player < 0 || player >= PING_MAX_PLAYERS) return false;
    float *t = lim->t[player];
    int n = lim->n[player];
    // Ring of the last PING_RATE_COUNT times (newest at n - 1).
    if (n > 0 && now - t[(n - 1) % PING_RATE_COUNT] < PING_RATE_MIN_GAP) return false;
    if (n >= PING_RATE_COUNT && now - t[n % PING_RATE_COUNT] < PING_RATE_WINDOW) return false;
    t[n % PING_RATE_COUNT] = now;
    lim->n[player] = n + 1;
    return true;
}

void ping_rate_reset(PingLimiter *lim, int player) {
    if (player < 0 || player >= PING_MAX_PLAYERS) return;
    memset(lim->t[player], 0, sizeof(lim->t[player]));
    lim->n[player] = 0;
}

void ping_pack(int type, vec3 pos, int *a, int *b, int *c) {
    int y = DG_CLAMP((int)lrintf(pos.y * 100.0f) + 32768, 0, 65535);
    *a = (type & 0xFF) | (y << 8);
    *b = (int)lrintf(pos.x * 100.0f);
    *c = (int)lrintf(pos.z * 100.0f);
}

void ping_unpack(int a, int b, int c, int *type, vec3 *pos) {
    *type = a & 0xFF;
    *pos = v3((float)b / 100.0f, (float)(((a >> 8) & 0xFFFF) - 32768) / 100.0f, (float)c / 100.0f);
}
