#include "horde_steer.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void horde_config_defaults(HordeConfig *c) {
    memset(c, 0, sizeof(*c));
    c->speed_mult = 1.0f;
    c->accel = 6.0f;
    c->turn_rate_deg = 220.0f;
    c->stop_dist = 0.9f;
    c->sep_radius = 0.85f;
    c->sep_weight = 2.2f;
    c->align_radius = 2.2f;
    c->align_weight = 0.25f;
    c->avoid_weight = 2.5f;
    c->seek_weight = 1.0f;
    c->speed_jitter = 0.1f;
    c->attack_range = 1.35f;
    c->attack_facing_deg = 45.0f;
    c->attack_cooldown = 1.6f;
    c->attack_hit_time = 0.45f;
    c->attack_damage = 10.0f;
    c->retarget_ticks = 15;
    c->cast_interval = 6;
    c->ground_interval = 8;
    c->cast_len = 1.1f;
    c->step_height = 0.45f;
    c->max_slope_deg = 50.0f;
    c->tier_dist[HT_NEAR] = 20.0f;
    c->tier_dist[HT_MID] = 50.0f;
    c->tier_dist[HT_FAR] = 140.0f;
    c->tier_hyst = 3.0f;
    int ti[HT_COUNT] = { 1, 2, 4, 60 };
    int ai[HT_COUNT] = { 1, 2, 4, 60 };
    int ni[HT_COUNT] = { 1, 2, 4, 0 };
    memcpy(c->tier_interval, ti, sizeof(ti));
    memcpy(c->anim_interval, ai, sizeof(ai));
    memcpy(c->net_interval, ni, sizeof(ni));
    c->physics_dist = 20.0f;
    c->physics_hyst = 4.0f;
    c->spawn_per_tick = 32;
    c->spawn_radius = 30.0f;
    c->net_near = 20.0f;
    c->net_mid = 50.0f;
    c->net_budget = 22000;
    c->capacity = 2048;
}

static float jf(const cJSON *o, const char *k, float d) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : d;
}
static void ji4(const cJSON *o, const char *k, int *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v)) return;
    for (int i = 0; i < HT_COUNT && i < cJSON_GetArraySize(v); i++) out[i] = (int)cJSON_GetArrayItem(v, i)->valuedouble;
}

bool horde_config_parse(HordeConfig *c, const char *json, char *err, int err_size) {
    horde_config_defaults(c);
    cJSON *r = cJSON_Parse(json);
    if (!r) { if (err) snprintf(err, (size_t)err_size, "zombie.json: parse error"); return false; }
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "movement");
    c->speed_mult = jf(m, "speed_mult", c->speed_mult);
    c->accel = jf(m, "accel", c->accel);
    c->turn_rate_deg = jf(m, "turn_rate_deg", c->turn_rate_deg);
    c->stop_dist = jf(m, "stop_dist", c->stop_dist);
    c->sep_radius = jf(m, "separation_radius", c->sep_radius);
    c->sep_weight = jf(m, "separation_weight", c->sep_weight);
    c->align_radius = jf(m, "alignment_radius", c->align_radius);
    c->align_weight = jf(m, "alignment_weight", c->align_weight);
    c->avoid_weight = jf(m, "avoid_weight", c->avoid_weight);
    c->seek_weight = jf(m, "seek_weight", c->seek_weight);
    c->speed_jitter = jf(m, "speed_jitter", c->speed_jitter);
    c->cast_interval = (int)jf(m, "cast_interval", (float)c->cast_interval);
    c->ground_interval = (int)jf(m, "ground_interval", (float)c->ground_interval);
    c->cast_len = jf(m, "cast_len", c->cast_len);
    c->step_height = jf(m, "step_height", c->step_height);
    c->max_slope_deg = jf(m, "max_slope_deg", c->max_slope_deg);
    c->retarget_ticks = (int)jf(m, "retarget_ticks", (float)c->retarget_ticks);
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(r, "attack");
    c->attack_range = jf(a, "range", c->attack_range);
    c->attack_facing_deg = jf(a, "facing_deg", c->attack_facing_deg);
    c->attack_cooldown = jf(a, "cooldown", c->attack_cooldown);
    c->attack_hit_time = jf(a, "hit_time", c->attack_hit_time);
    c->attack_damage = jf(a, "damage", c->attack_damage);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "tiers");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(t, "dist");
    if (cJSON_IsArray(d))
        for (int i = 0; i < HT_COUNT - 1 && i < cJSON_GetArraySize(d); i++) c->tier_dist[i] = (float)cJSON_GetArrayItem(d, i)->valuedouble;
    c->tier_hyst = jf(t, "hysteresis", c->tier_hyst);
    ji4(t, "sim_interval", c->tier_interval);
    ji4(t, "anim_interval", c->anim_interval);
    c->physics_dist = jf(t, "physics_dist", c->physics_dist);
    c->physics_hyst = jf(t, "physics_hysteresis", c->physics_hyst);
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(r, "spawn");
    c->spawn_per_tick = (int)jf(s, "per_tick", (float)c->spawn_per_tick);
    c->spawn_radius = jf(s, "radius", c->spawn_radius);
    c->capacity = DG_CLAMP((int)jf(s, "capacity", (float)c->capacity), 16, 2048);
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(r, "net");
    ji4(n, "interval", c->net_interval);
    c->net_near = jf(n, "near", c->net_near);
    c->net_mid = jf(n, "mid", c->net_mid);
    c->net_budget = (int)jf(n, "budget_bytes_per_s", (float)c->net_budget);
    for (int i = 0; i < HT_COUNT; i++) {
        c->tier_interval[i] = DG_MAX(1, c->tier_interval[i]);
        c->anim_interval[i] = DG_MAX(1, c->anim_interval[i]);
        c->net_interval[i] = DG_MAX(0, c->net_interval[i]);
    }
    c->cast_interval = DG_MAX(1, c->cast_interval);
    c->ground_interval = DG_MAX(1, c->ground_interval);
    c->retarget_ticks = DG_MAX(1, c->retarget_ticks);
    cJSON_Delete(r);
    return true;
}

int horde_tier_for(const HordeConfig *c, int current, float dist) {
    // Plain bands, then stay in the current tier while within its band +- hysteresis.
    int t = HT_DORMANT;
    for (int k = 0; k < HT_COUNT - 1; k++) if (dist < c->tier_dist[k]) { t = k; break; }
    if (current < 0 || current >= HT_COUNT || current == t) return t;
    float lo = current == 0 ? -1e9f : c->tier_dist[current - 1] - c->tier_hyst;
    float hi = current >= HT_COUNT - 1 ? 1e9f : c->tier_dist[current] + c->tier_hyst;
    return (dist >= lo && dist < hi) ? current : t;
}

static float wrapf(float a) {
    while (a > DG_PI) a -= 2.0f * DG_PI;
    while (a < -DG_PI) a += 2.0f * DG_PI;
    return a;
}

void horde_steer_range(const HordeSteerIn *in, HordeSteerOut *out, int begin, int end) {
    const HordeConfig *c = in->cfg;
    int nb[24];
    for (int i = begin; i < end && i < in->count; i++) {
        out->vel[i] = in->vel[i];
        out->yaw[i] = in->yaw[i];
        if (!in->active[i]) continue;
        int tier = in->tier[i];
        float dt = in->dt * (float)c->tier_interval[tier];
        vec3 p = in->pos[i];
        vec3 want = v3(0, 0, 0);
        float vmax = in->speed_max[i];
        bool busy = in->busy && in->busy[i];
        if (in->has_target[i] && !busy) {
            vec3 goal = in->target[i];
            vec3 next;
            if (in->path && in->path(in->path_ctx, p, goal, &next)) goal = next;
            vec3 to = v3(goal.x - p.x, 0, goal.z - p.z);
            float d = sqrtf(to.x * to.x + to.z * to.z);
            if (d > 1e-4f) {
                // Arrive: slow down inside the stop distance (they crowd in, they don't overshoot).
                float k = d < c->stop_dist ? d / c->stop_dist : 1.0f;
                want = v3_scale(to, vmax * k * c->seek_weight / d);
            }
        }
        if (tier <= HT_MID && in->grid) {
            float r = fmaxf(c->sep_radius, c->align_radius);
            int n = hgrid_query(in->grid, in->pos, p, r, i, nb, 24);
            vec3 sep = v3(0, 0, 0), ali = v3(0, 0, 0);
            int na = 0;
            for (int k = 0; k < n; k++) {
                int j = nb[k];
                float dx = p.x - in->pos[j].x, dz = p.z - in->pos[j].z;
                float d2 = dx * dx + dz * dz;
                if (d2 < c->sep_radius * c->sep_radius) {
                    float d = sqrtf(d2);
                    if (d < 1e-4f) {   // exactly overlapping: deterministic split by index
                        float a = (float)((i * 2654435761u) & 1023u) / 1023.0f * 6.2831853f;
                        dx = cosf(a); dz = sinf(a); d = 1.0f;
                    }
                    float w = (1.0f - d / c->sep_radius) / d;
                    sep.x += dx * w;
                    sep.z += dz * w;
                }
                if (d2 < c->align_radius * c->align_radius) { ali = v3_add(ali, in->vel[j]); na++; }
            }
            want = v3_add(want, v3_scale(sep, c->sep_weight * vmax));
            if (na > 0 && !busy) want = v3_add(want, v3_scale(v3(ali.x / (float)na, 0, ali.z / (float)na), c->align_weight));
        }
        if (in->avoid) want = v3_add(want, v3_scale(in->avoid[i], c->avoid_weight * vmax));
        want.y = 0;
        float wl = sqrtf(want.x * want.x + want.z * want.z);
        float cap = busy ? vmax * 0.35f : vmax;
        if (wl > cap) want = v3_scale(want, cap / wl);
        // Accelerate towards it.
        vec3 v = in->vel[i];
        vec3 dv = v3_sub(want, v);
        float dl = sqrtf(dv.x * dv.x + dv.z * dv.z), step = c->accel * dt;
        if (dl > step) dv = v3_scale(dv, step / dl);
        v = v3_add(v, dv);
        v.y = 0;
        out->vel[i] = v;
        // Face the motion (or the target while busy / standing).
        float sp = sqrtf(v.x * v.x + v.z * v.z);
        float face = in->yaw[i];
        if (sp > 0.15f) face = atan2f(-v.x, -v.z);
        else if (in->has_target[i]) {
            vec3 to = v3_sub(in->target[i], p);
            if (to.x * to.x + to.z * to.z > 1e-4f) face = atan2f(-to.x, -to.z);
        }
        float diff = wrapf(face - in->yaw[i]);
        float maxt = c->turn_rate_deg * DG_DEG2RAD * dt;
        out->yaw[i] = wrapf(in->yaw[i] + clampf(diff, -maxt, maxt));
    }
}
