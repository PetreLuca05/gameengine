#include "vehicle_def.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ defaults

static void wheel(VehicleWheelDef *w, vec3 pos, float radius, float width, float steer, bool driven, float brake, float handbrake) {
    memset(w, 0, sizeof(*w));
    w->pos = pos;
    w->radius = radius;
    w->width = width;
    w->susp_min = 0.05f;
    w->susp_max = 0.32f;
    w->susp_freq = 1.6f;
    w->susp_damp = 0.5f;
    w->max_steer_deg = steer;
    w->driven = driven;
    w->max_brake = brake;
    w->max_handbrake = handbrake;
    w->left = pos.x < -0.01f;
}

static void seat(VehicleSeatDef *s, vec3 pos, vec3 exit, bool driver, bool can_shoot) {
    memset(s, 0, sizeof(*s));
    s->pos = pos;
    s->exit = exit;
    s->eye = v3(0.0f, 0.62f, 0.06f);
    s->driver = driver;
    s->can_shoot = can_shoot;
    s->yaw_limit_deg = 135.0f;
}

// Origin at ground level under the body centre (at rest); forward -Z, right +X.
void vehicle_def_defaults(VehicleDef *d, VehicleKind kind) {
    memset(d, 0, sizeof(*d));
    d->kind = kind;
    d->impact_damage_speed = 6.0f;
    d->impact_damage_scale = 25.0f;
    d->tire_bullet_damage = 0.34f;
    d->tire_leak_per_hit = 0.04f;
    d->tire_burst_health = 0.05f;
    d->one_hand_spread_mult = 1.6f;
    d->passenger_spread_mult = 1.3f;
    if (kind == VK_CAR) {
        dg_strcpy(d->name, sizeof(d->name), "car");
        dg_strcpy(d->display, sizeof(d->display), "Hatchback");
        d->mass = 1250.0f;
        d->box_count = 2;
        d->box_center[0] = v3(0, 0.62f, 0);
        d->box_half[0] = v3(0.86f, 0.30f, 2.05f);
        d->box_center[1] = v3(0, 1.18f, 0.2f);
        d->box_half[1] = v3(0.76f, 0.26f, 1.15f);
        d->com_offset = v3(0, -0.2f, 0);
        d->wheel_count = 4;
        wheel(&d->wheels[0], v3(-0.78f, 0.55f, -1.30f), 0.32f, 0.22f, 32.0f, true, 1800.0f, 0.0f);
        wheel(&d->wheels[1], v3(0.78f, 0.55f, -1.30f), 0.32f, 0.22f, 32.0f, true, 1800.0f, 0.0f);
        wheel(&d->wheels[2], v3(-0.78f, 0.55f, 1.25f), 0.32f, 0.22f, 0.0f, false, 1500.0f, 3500.0f);
        wheel(&d->wheels[3], v3(0.78f, 0.55f, 1.25f), 0.32f, 0.22f, 0.0f, false, 1500.0f, 3500.0f);
        d->max_torque = 380.0f;
        d->min_rpm = 900.0f;
        d->max_rpm = 6200.0f;
        float g[5] = { 3.1f, 1.95f, 1.4f, 1.05f, 0.85f };
        memcpy(d->gears, g, sizeof(g));
        d->gear_count = 5;
        d->reverse_ratio = -3.0f;
        d->diff_ratio = 3.9f;
        d->fuel_capacity = 45.0f;
        d->fuel_idle_lph = 2.0f;
        d->fuel_full_lph = 60.0f;
        d->hp = 1000.0f;
        d->seat_count = 4;
        seat(&d->seats[0], v3(-0.36f, 0.62f, 0.05f), v3(-1.45f, 0.05f, 0.05f), true, false);
        seat(&d->seats[1], v3(0.36f, 0.62f, 0.05f), v3(1.45f, 0.05f, 0.05f), false, true);
        seat(&d->seats[2], v3(-0.38f, 0.64f, 0.95f), v3(-1.45f, 0.05f, 0.95f), false, true);
        seat(&d->seats[3], v3(0.38f, 0.64f, 0.95f), v3(1.45f, 0.05f, 0.95f), false, true);
        d->steer_pivot = v3(-0.36f, 0.96f, -0.36f);
        d->steer_axis = v3_norm(v3(0, 0.45f, 0.89f));
        d->steer_ratio = 3.0f;
        d->steer_radius = 0.19f;
        d->pedal_l = v3(-0.44f, 0.36f, -0.88f);
        d->pedal_r = v3(-0.28f, 0.36f, -0.88f);
        d->fuel_cap = v3(0.87f, 0.86f, 1.25f);
        d->exhaust = v3(-0.5f, 0.32f, 2.08f);
        d->headlight_l = v3(-0.6f, 0.72f, -2.06f);
        d->headlight_r = v3(0.6f, 0.72f, -2.06f);
        d->camera_tp = v3(0, 2.3f, 6.2f);
        d->one_hand_steer_limit = 0.35f;
        d->exit_ragdoll_speed = 6.0f;
        d->color = v3(0.55f, 0.1f, 0.08f);
    } else {
        dg_strcpy(d->name, sizeof(d->name), "moto");
        dg_strcpy(d->display, sizeof(d->display), "Motorcycle");
        d->mass = 230.0f;
        d->box_count = 2;
        d->box_center[0] = v3(0, 0.62f, 0);
        d->box_half[0] = v3(0.18f, 0.24f, 0.72f);
        d->box_center[1] = v3(0, 0.93f, 0.1f);
        d->box_half[1] = v3(0.16f, 0.09f, 0.45f);
        d->com_offset = v3(0, -0.1f, 0);
        d->wheel_count = 2;
        wheel(&d->wheels[0], v3(0, 0.62f, -0.74f), 0.31f, 0.12f, 30.0f, false, 500.0f, 0.0f);
        wheel(&d->wheels[1], v3(0, 0.62f, 0.72f), 0.31f, 0.14f, 0.0f, true, 250.0f, 300.0f);
        d->wheels[0].susp_freq = d->wheels[1].susp_freq = 2.0f;
        d->max_torque = 130.0f;
        d->min_rpm = 1200.0f;
        d->max_rpm = 9000.0f;
        float g[5] = { 2.6f, 1.9f, 1.5f, 1.2f, 1.0f };
        memcpy(d->gears, g, sizeof(g));
        d->gear_count = 5;
        d->reverse_ratio = -2.5f;
        d->diff_ratio = 4.5f;
        d->max_lean_deg = 45.0f;
        d->fuel_capacity = 14.0f;
        d->fuel_idle_lph = 1.5f;
        d->fuel_full_lph = 30.0f;
        d->hp = 400.0f;
        d->seat_count = 2;
        seat(&d->seats[0], v3(0, 0.92f, -0.02f), v3(-0.95f, 0.05f, -0.02f), true, false);
        seat(&d->seats[1], v3(0, 0.97f, 0.45f), v3(0.95f, 0.05f, 0.45f), false, true);
        d->seats[0].eye = v3(0, 0.66f, -0.05f);
        d->steer_pivot = v3(0, 1.1f, -0.36f);
        d->steer_axis = v3_norm(v3(0, 0.9f, 0.42f));
        d->steer_ratio = 1.0f;
        d->steer_radius = 0.3f;
        d->pedal_l = v3(-0.2f, 0.38f, -0.02f);
        d->pedal_r = v3(0.2f, 0.38f, -0.02f);
        d->fuel_cap = v3(0, 1.03f, -0.2f);
        d->exhaust = v3(0.2f, 0.42f, 0.95f);
        d->headlight_l = d->headlight_r = v3(0, 1.0f, -0.62f);
        d->camera_tp = v3(0, 1.8f, 4.6f);
        d->one_hand_steer_limit = 0.25f;
        d->one_hand_spread_mult = 1.8f;
        d->exit_ragdoll_speed = 5.0f;
        d->color = v3(0.1f, 0.2f, 0.45f);
    }
}

// ------------------------------------------------------------------ JSON

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static vec3 jv3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}
static bool jb(const cJSON *o, const char *k, bool def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

bool vehicle_def_parse(VehicleDef *d, const char *json, size_t len) {
    cJSON *r = cJSON_ParseWithLength(json, len);
    if (!r) return false;
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(r, "kind");
    VehicleKind k = cJSON_IsString(kind) && !strcmp(kind->valuestring, "motorcycle") ? VK_MOTO : VK_CAR;
    vehicle_def_defaults(d, k);
    const cJSON *s;
    if (cJSON_IsString(s = cJSON_GetObjectItemCaseSensitive(r, "name"))) dg_strcpy(d->name, sizeof(d->name), s->valuestring);
    if (cJSON_IsString(s = cJSON_GetObjectItemCaseSensitive(r, "display"))) dg_strcpy(d->display, sizeof(d->display), s->valuestring);
    d->mass = jf(r, "mass", d->mass);
    d->com_offset = jv3(r, "com_offset", d->com_offset);
    const cJSON *boxes = cJSON_GetObjectItemCaseSensitive(r, "body");
    if (cJSON_IsArray(boxes) && cJSON_GetArraySize(boxes) > 0) {
        d->box_count = 0;
        const cJSON *b;
        cJSON_ArrayForEach(b, boxes) {
            if (d->box_count >= VEH_MAX_BOXES) break;
            d->box_center[d->box_count] = jv3(b, "center", v3(0, 0.6f, 0));
            d->box_half[d->box_count] = jv3(b, "half", v3(0.8f, 0.3f, 2.0f));
            d->box_count++;
        }
    }
    const cJSON *wheels = cJSON_GetObjectItemCaseSensitive(r, "wheels");
    if (cJSON_IsArray(wheels) && cJSON_GetArraySize(wheels) > 0) {
        int n = 0;
        const cJSON *w;
        cJSON_ArrayForEach(w, wheels) {
            if (n >= VEH_MAX_WHEELS) break;
            VehicleWheelDef *wd = &d->wheels[n];
            VehicleWheelDef base = n < d->wheel_count ? *wd : d->wheels[0];
            *wd = base;
            wd->pos = jv3(w, "pos", base.pos);
            wd->radius = jf(w, "radius", base.radius);
            wd->width = jf(w, "width", base.width);
            wd->susp_min = jf(w, "suspension_min", base.susp_min);
            wd->susp_max = jf(w, "suspension_max", base.susp_max);
            wd->susp_freq = jf(w, "suspension_frequency", base.susp_freq);
            wd->susp_damp = jf(w, "suspension_damping", base.susp_damp);
            wd->max_steer_deg = jf(w, "max_steer", base.max_steer_deg);
            wd->max_brake = jf(w, "max_brake", base.max_brake);
            wd->max_handbrake = jf(w, "max_handbrake", base.max_handbrake);
            wd->driven = jb(w, "driven", base.driven);
            wd->left = wd->pos.x < -0.01f;
            n++;
        }
        d->wheel_count = n;
    }
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(r, "engine");
    d->max_torque = jf(e, "max_torque", d->max_torque);
    d->min_rpm = jf(e, "min_rpm", d->min_rpm);
    d->max_rpm = jf(e, "max_rpm", d->max_rpm);
    d->diff_ratio = jf(e, "differential_ratio", d->diff_ratio);
    d->reverse_ratio = jf(e, "reverse_ratio", d->reverse_ratio);
    d->max_lean_deg = jf(r, "max_lean", d->max_lean_deg);
    const cJSON *gears = e ? cJSON_GetObjectItemCaseSensitive(e, "gears") : NULL;
    if (cJSON_IsArray(gears) && cJSON_GetArraySize(gears) > 0) {
        d->gear_count = 0;
        const cJSON *g;
        cJSON_ArrayForEach(g, gears) if (d->gear_count < VEH_MAX_GEARS && cJSON_IsNumber(g)) d->gears[d->gear_count++] = (float)g->valuedouble;
    }
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "fuel");
    d->fuel_capacity = jf(f, "capacity", d->fuel_capacity);
    d->fuel_idle_lph = jf(f, "idle_lph", d->fuel_idle_lph);
    d->fuel_full_lph = jf(f, "full_lph", d->fuel_full_lph);
    const cJSON *dm = cJSON_GetObjectItemCaseSensitive(r, "damage");
    d->hp = jf(dm, "hp", d->hp);
    d->impact_damage_speed = jf(dm, "impact_speed", d->impact_damage_speed);
    d->impact_damage_scale = jf(dm, "impact_scale", d->impact_damage_scale);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(r, "tires");
    d->tire_bullet_damage = jf(t, "bullet_damage", d->tire_bullet_damage);
    d->tire_leak_per_hit = jf(t, "leak_per_hit", d->tire_leak_per_hit);
    d->tire_burst_health = jf(t, "burst_health", d->tire_burst_health);
    // Sockets: seat_n / exit_n / eye_n, grip_l / grip_r (optional; else from the steering pivot),
    // steer_pivot / steer_axis, pedal_l / pedal_r, fuel_cap, exhaust, headlight_l / _r, camera_tp.
    const cJSON *so = cJSON_GetObjectItemCaseSensitive(r, "sockets");
    const cJSON *seats = cJSON_GetObjectItemCaseSensitive(r, "seats");
    if (cJSON_IsArray(seats) && cJSON_GetArraySize(seats) > 0) {
        int n = 0;
        const cJSON *st;
        cJSON_ArrayForEach(st, seats) {
            if (n >= VEH_MAX_SEATS) break;
            VehicleSeatDef base = n < d->seat_count ? d->seats[n] : d->seats[d->seat_count > 0 ? d->seat_count - 1 : 0];
            VehicleSeatDef *sd = &d->seats[n];
            *sd = base;
            sd->driver = jb(st, "driver", n == 0);
            sd->can_shoot = jb(st, "can_shoot", !sd->driver);
            sd->yaw_limit_deg = jf(st, "yaw_limit", base.yaw_limit_deg);
            n++;
        }
        d->seat_count = n;
    }
    for (int i = 0; i < d->seat_count; i++) {
        char k1[16], k2[16], k3[16];
        snprintf(k1, sizeof(k1), "seat_%d", i);
        snprintf(k2, sizeof(k2), "exit_%d", i);
        snprintf(k3, sizeof(k3), "eye_%d", i);
        d->seats[i].pos = jv3(so, k1, d->seats[i].pos);
        d->seats[i].exit = jv3(so, k2, d->seats[i].exit);
        d->seats[i].eye = jv3(so, k3, d->seats[i].eye);
    }
    d->steer_pivot = jv3(so, "steer_pivot", d->steer_pivot);
    d->steer_axis = v3_norm(jv3(so, "steer_axis", d->steer_axis));
    d->steer_ratio = jf(so, "steer_ratio", d->steer_ratio);
    d->steer_radius = jf(so, "steer_radius", d->steer_radius);
    d->grip_l = jv3(so, "grip_l", d->grip_l);
    d->grip_r = jv3(so, "grip_r", d->grip_r);
    d->pedal_l = jv3(so, "pedal_l", d->pedal_l);
    d->pedal_r = jv3(so, "pedal_r", d->pedal_r);
    d->fuel_cap = jv3(so, "fuel_cap", d->fuel_cap);
    d->exhaust = jv3(so, "exhaust", d->exhaust);
    d->headlight_l = jv3(so, "headlight_l", d->headlight_l);
    d->headlight_r = jv3(so, "headlight_r", d->headlight_r);
    d->camera_tp = jv3(so, "camera_tp", d->camera_tp);
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(r, "handling");
    d->one_hand_steer_limit = jf(h, "one_hand_steer_limit", d->one_hand_steer_limit);
    d->one_hand_spread_mult = jf(h, "one_hand_spread_mult", d->one_hand_spread_mult);
    d->passenger_spread_mult = jf(h, "passenger_spread_mult", d->passenger_spread_mult);
    d->exit_ragdoll_speed = jf(h, "exit_ragdoll_speed", d->exit_ragdoll_speed);
    d->color = jv3(r, "color", d->color);
    cJSON_Delete(r);
    return true;
}

bool vehicle_lib_load(VehicleLib *lib, const char *dir) {
    memset(lib, 0, sizeof(*lib));
    static const char *names[2] = { "car", "moto" };
    for (int i = 0; i < 2; i++) {
        VehicleDef *d = &lib->defs[lib->count];
        vehicle_def_defaults(d, i == 0 ? VK_CAR : VK_MOTO);
        char full[512];
        snprintf(full, sizeof(full), "%s%s/%s.json", platform_base_dir(), dir, names[i]);
        size_t size = 0;
        char *text = platform_read_file(full, &size);
        if (text) {
            if (!vehicle_def_parse(d, text, size)) log_warn("vehicles: %s/%s.json: JSON error, built-in defaults", dir, names[i]);
            free(text);
        }
        lib->count++;
    }
    return true;
}

int vehicle_lib_index(const VehicleLib *lib, const char *name) {
    for (int i = 0; i < lib->count; i++) if (!strcmp(lib->defs[i].name, name)) return i;
    return -1;
}

const VehicleDef *vehicle_lib_find(const VehicleLib *lib, const char *name) {
    int i = vehicle_lib_index(lib, name);
    return i >= 0 ? &lib->defs[i] : NULL;
}

// ------------------------------------------------------------------ fuel

float vehicle_fuel_rate(const VehicleDef *d, float throttle, float rpm_frac) {
    throttle = fminf(fmaxf(throttle, 0.0f), 1.0f);
    rpm_frac = fminf(fmaxf(rpm_frac, 0.0f), 1.0f);
    // Idle burn + load: throttle x engine speed (pumping losses make high rpm costly).
    float load = throttle * (0.3f + 0.7f * rpm_frac);
    float lph = d->fuel_idle_lph + (d->fuel_full_lph - d->fuel_idle_lph) * load;
    return lph / 3600.0f;
}

// ------------------------------------------------------------------ tires

void tire_init(TireState *t) {
    t->pressure = 1.0f;
    t->health = 1.0f;
    t->leak = 0.0f;
}

bool tire_hit(TireState *t, const VehicleDef *d) {
    t->health = fmaxf(0.0f, t->health - d->tire_bullet_damage);
    t->leak += d->tire_leak_per_hit;
    if (t->health <= d->tire_burst_health) {
        t->pressure = 0.0f;
        return true;
    }
    t->pressure = fmaxf(0.0f, t->pressure - 0.08f);   // the first gush
    return false;
}

void tire_tick(TireState *t, float dt) {
    if (t->leak > 0.0f) t->pressure = fmaxf(0.0f, t->pressure - t->leak * dt);
}

void tire_repair(TireState *t) {
    t->leak = 0.0f;
    t->pressure = fmaxf(t->pressure, 0.85f);
    t->health = fmaxf(t->health, 0.4f);
}

void tire_replace(TireState *t) { tire_init(t); }

void tire_grip(const TireState *t, float *lon, float *lat) {
    // Full grip down to ~70 % pressure, then a steep drop; on the rim the steel slides.
    float p = fminf(fmaxf(t->pressure, 0.0f), 1.0f);
    float k = p >= 0.7f ? 1.0f : 0.35f + 0.65f * (p / 0.7f) * (p / 0.7f);
    float wear = 0.85f + 0.15f * t->health;
    if (tire_on_rim(t)) k = 0.25f;
    *lon = k * wear;
    *lat = k * wear * (tire_on_rim(t) ? 0.8f : 1.0f);
}

float tire_radius_scale(const TireState *t) {
    float p = fminf(fmaxf(t->pressure, 0.0f), 1.0f);
    return 0.82f + 0.18f * sqrtf(p);
}

bool tire_on_rim(const TireState *t) { return t->pressure < 0.05f; }

float vehicle_tire_pull(const VehicleDef *d, const TireState *tires) {
    if (d->kind == VK_MOTO) return 0.0f;
    float l = 0, r = 0;
    for (int i = 0; i < d->wheel_count; i++) {
        float def = 1.0f - fminf(fmaxf(tires[i].pressure, 0.0f), 1.0f);
        if (d->wheels[i].left) l += def;
        else r += def;
    }
    return fminf(fmaxf((r - l) * 0.18f, -0.35f), 0.35f);
}

// ------------------------------------------------------------------ driving

float vehicle_steer_clamp(const VehicleDef *d, float steer, bool one_hand) {
    float lim = one_hand ? fmaxf(d->one_hand_steer_limit, 0.05f) : 1.0f;
    return fminf(fmaxf(steer, -lim), lim);
}

float vehicle_steer_smooth(float cur, float target, float speed_mps, float dt) {
    // Keyboard steering: fast at walking pace, slower (and with less lock) at speed.
    float rate = (target == 0.0f || target * cur < 0.0f) ? 5.0f : 3.2f - fminf(speed_mps / 30.0f, 1.0f) * 1.6f;
    float lim = 1.0f - fminf(speed_mps / 40.0f, 1.0f) * 0.55f;
    target = fminf(fmaxf(target, -lim), lim);
    float d = target - cur, step = rate * dt;
    if (fabsf(d) <= step) return target;
    return cur + (d > 0 ? step : -step);
}

float vehicle_impact_damage(const VehicleDef *d, float delta_v) {
    float over = delta_v - d->impact_damage_speed;
    return over > 0.0f ? over * d->impact_damage_scale : 0.0f;
}

// ------------------------------------------------------------------ quantization

u8 veh_q01(float v) { return (u8)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f); }
float veh_dq01(u8 q) { return (float)q / 255.0f; }
i8 veh_qs(float v) { return (i8)lrintf(fminf(fmaxf(v, -1.0f), 1.0f) * 127.0f); }
float veh_dqs(i8 q) { return (float)q / 127.0f; }
