#include "fire_anim.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *k_class_names[FC_COUNT] = { "rifle", "pistol", "smg", "lmg", "shotgun", "dmr", "sniper", "crossbow" };
static const char *k_ch_names[FA_CH] = { "pitch", "yaw", "roll", "back", "up" };

const char *fire_class_name(int c) { return (c >= 0 && c < FC_COUNT) ? k_class_names[c] : "rifle"; }
int fire_class_from_name(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < FC_COUNT; i++) if (!strcmp(s, k_class_names[i])) return i;
    return -1;
}

// ------------------------------------------------------------------ class defaults

static FaChannel CH(float peak, float omega, float zeta, float ads, float follow, float sign_rnd) {
    FaChannel c = { peak, 0.85f, 1.15f, omega, zeta, ads, follow, sign_rnd, 0.0f, 0.0f };
    return c;
}

// Keys: t, pitch, yaw, roll, back, up (deg / m, before the per-shot scale).
static void curve_set(FaCurve *c, int n, const float (*k)[6]) {
    memset(c, 0, sizeof(*c));
    c->n = DG_MIN(n, FA_MAX_KEYS);
    for (int i = 0; i < c->n; i++) {
        c->t[i] = k[i][0];
        for (int ch = 0; ch < FA_CH; ch++) c->v[i][ch] = k[i][1 + ch];
    }
}
#define CURVE(c, ...) do { static const float k_[][6] = { __VA_ARGS__ }; curve_set(&(c), (int)(sizeof(k_) / sizeof(k_[0])), k_); } while (0)

void fire_anim_class_defaults(FireAnimDef *d, int cls) {
    memset(d, 0, sizeof(*d));
    cls = DG_CLAMP(cls, 0, FC_COUNT - 1);
    d->cls = cls;
    snprintf(d->cls_name, sizeof(d->cls_name), "%s", k_class_names[cls]);
    for (int i = 0; i < FA_CH; i++) d->curve_amp[i] = 1.0f;
    d->first_mult = 1.25f;
    d->hip_scale = 1.0f;
    d->ads_scale = 1.0f;
    d->settle_time = 0.35f;
    d->ride_release = 0.11f;
    d->hand_omega = 70.0f;
    d->hand_zeta = 0.8f;
    d->hand_max_err = 0.01f;
    d->shoulder_push = 0.35f;
    d->shoulder_omega = 26.0f;
    d->shoulder_zeta = 0.55f;
    d->trigger_pull = 1.0f;
    d->slide_lock_empty = true;
    d->wobble_omega = 34.0f;
    d->wobble_zeta = 0.3f;
    d->wobble_gain = 0.6f;
    d->heat_per_shot = 0.06f;
    d->heat_decay = 0.5f;
    d->cam_gain[FA_PITCH] = 0.32f;
    d->cam_gain[FA_YAW] = 0.25f;
    d->cam_gain[FA_ROLL] = 0.45f;
    d->cam_gain[FA_BACK] = 0.5f;
    d->cam_gain[FA_UP] = 0.0f;
    d->cam_delay = 2;
    d->cam_omega = 20.0f;
    d->cam_zeta = 0.42f;
    d->fov_release = 7.0f / 60.0f;
    d->screen_release = 0.1f;
    d->blur_ticks = 2;
    switch (cls) {
    case FC_PISTOL:
        d->mass = 0.9f; d->moment = 0.3f; d->length = 0.2f;
        d->ch[FA_PITCH] = CH(5.0f, 16.0f, 0.45f, 0.4f, 0.10f, 0.0f);
        d->ch[FA_YAW] = CH(1.2f, 17.0f, 0.5f, 0.35f, 0.0f, 1.0f);
        d->ch[FA_ROLL] = CH(2.0f, 18.0f, 0.42f, 0.5f, 0.0f, 0.35f);
        d->ch[FA_BACK] = CH(0.022f, 18.0f, 0.5f, 0.7f, 0.05f, 0.0f);
        d->ch[FA_UP] = CH(0.012f, 16.0f, 0.5f, 0.0f, 0.0f, 0.0f);
        // Sharp up + the slide flip (roll snaps and comes back), short.
        CURVE(d->first, { 0, 2.5f, 0, 1.2f, 0.01f, 0.006f }, { 0.033f, 1.5f, 0, -0.4f, 0.006f, 0.004f }, { 0.1f, 0, 0, 0, 0, 0 }, { 0.16f, -0.5f, 0, 0, -0.002f, 0 }, { 0.28f, 0, 0, 0, 0, 0 });
        CURVE(d->sustained, { 0, 1.8f, 0, 0.9f, 0.008f, 0.004f }, { 0.033f, 1.0f, 0, -0.3f, 0.004f, 0.002f }, { 0.1f, 0, 0, 0, 0, 0 }, { 0.2f, 0, 0, 0, 0, 0 });
        CURVE(d->release, { 0, 0, 0, 0, 0, 0 }, { 0.12f, -0.6f, 0, 0, 0, 0 }, { 0.3f, 0.15f, 0, 0, 0, 0 }, { 0.5f, 0, 0, 0, 0, 0 });
        d->slide_travel = 0.025f; d->slide_time = 0.07f;
        d->fov_punch = 2.0f; d->roll_twitch = 0.6f; d->screen_kick = 0.008f;
        d->shoulder_push = 0.2f;
        break;
    case FC_SMG:
        d->mass = 2.8f; d->moment = 0.35f; d->length = 0.4f;
        d->ch[FA_PITCH] = CH(1.5f, 18.0f, 0.5f, 0.45f, 0.18f, 0.0f);
        d->ch[FA_YAW] = CH(0.9f, 19.0f, 0.5f, 0.4f, 0.1f, 1.0f);
        d->ch[FA_ROLL] = CH(0.8f, 20.0f, 0.5f, 0.5f, 0.0f, 1.0f);
        d->ch[FA_BACK] = CH(0.012f, 20.0f, 0.5f, 0.7f, 0.12f, 0.0f);
        d->ch[FA_UP] = CH(0.005f, 18.0f, 0.5f, 0.0f, 0.1f, 0.0f);
        CURVE(d->first, { 0, 0.8f, 0, 0.3f, 0.008f, 0.002f }, { 0.05f, 0.2f, 0, 0, 0.003f, 0 }, { 0.12f, -0.2f, 0, 0, 0, 0 }, { 0.22f, 0, 0, 0, 0, 0 });
        CURVE(d->sustained, { 0, 0.8f, 0, 0.2f, 0.004f, 0.001f }, { 0.05f, 0.2f, 0, 0, 0.001f, 0 }, { 0.12f, 0, 0, 0, 0, 0 });
        CURVE(d->release, { 0, 0, 0, 0, 0, 0 }, { 0.1f, -0.8f, 0, 0, -0.003f, 0 }, { 0.26f, 0.25f, 0, 0, 0.001f, 0 }, { 0.45f, 0, 0, 0, 0, 0 });
        d->slide_travel = 0.03f; d->slide_time = 0.05f;
        d->fov_punch = 2.0f; d->roll_twitch = 0.4f; d->screen_kick = 0.005f;
        d->settle_time = 0.25f;
        break;
    case FC_LMG:
        d->mass = 9.0f; d->moment = 0.6f; d->length = 0.8f;
        d->ch[FA_PITCH] = CH(1.7f, 8.0f, 0.42f, 0.45f, 0.3f, 0.0f);
        d->ch[FA_YAW] = CH(1.0f, 9.0f, 0.45f, 0.4f, 0.2f, 1.0f);
        d->ch[FA_ROLL] = CH(1.0f, 10.0f, 0.42f, 0.5f, 0.1f, 0.8f);
        d->ch[FA_BACK] = CH(0.016f, 10.0f, 0.42f, 0.7f, 0.25f, 0.0f);
        d->ch[FA_UP] = CH(0.006f, 9.0f, 0.45f, 0.0f, 0.2f, 0.0f);
        // Heavy and slow: rides up and back with every shot, long settle.
        CURVE(d->first, { 0, 1.0f, 0, 0.4f, 0.014f, 0.003f }, { 0.08f, 0.7f, 0, 0.2f, 0.009f, 0.002f }, { 0.25f, 0, 0, 0, 0, 0 }, { 0.4f, -0.4f, 0, 0, -0.002f, 0 }, { 0.65f, 0, 0, 0, 0, 0 });
        CURVE(d->sustained, { 0, 1.0f, 0, 0.3f, 0.008f, 0.002f }, { 0.08f, 0.7f, 0, 0.1f, 0.005f, 0.001f }, { 0.25f, 0, 0, 0, 0, 0 });
        CURVE(d->release, { 0, 0, 0, 0, 0, 0 }, { 0.2f, -1.4f, 0, 0, -0.006f, 0 }, { 0.5f, 0.45f, 0, 0, 0.002f, 0 }, { 0.9f, 0, 0, 0, 0, 0 });
        d->slide_travel = 0.04f; d->slide_time = 0.08f; d->slide_lock_empty = false;
        d->fov_punch = 2.5f; d->roll_twitch = 0.5f; d->screen_kick = 0.01f; d->chroma = 0.3f;
        d->heat_per_shot = 0.04f; d->heat_decay = 0.35f;
        d->settle_time = 0.5f;
        break;
    case FC_SHOTGUN:
        d->mass = 3.4f; d->moment = 0.55f; d->length = 0.7f;
        d->ch[FA_PITCH] = CH(6.5f, 9.0f, 0.35f, 0.4f, 0.1f, 0.0f);
        d->ch[FA_YAW] = CH(1.6f, 10.0f, 0.4f, 0.35f, 0.0f, 1.0f);
        d->ch[FA_ROLL] = CH(2.5f, 11.0f, 0.38f, 0.5f, 0.0f, 0.5f);
        d->ch[FA_BACK] = CH(0.045f, 10.0f, 0.38f, 0.65f, 0.1f, 0.0f);
        d->ch[FA_UP] = CH(0.02f, 9.0f, 0.4f, 0.0f, 0.0f, 0.0f);
        // Heavy back + up, held for a beat at the top (the pause), then a slow heavy return.
        CURVE(d->first, { 0, 3.0f, 0, 0.8f, 0.02f, 0.008f }, { 0.05f, 3.0f, 0, 0.8f, 0.02f, 0.008f }, { 0.2f, 0.8f, 0, 0.2f, 0.004f, 0.002f }, { 0.36f, -1.0f, 0, 0, -0.004f, 0 }, { 0.6f, 0, 0, 0, 0, 0 });
        d->sustained = d->first;
        CURVE(d->release, { 0, 0, 0, 0, 0, 0 }, { 0.5f, 0, 0, 0, 0, 0 });
        d->bolt_delay = 0.28f; d->slide_travel = 0.09f; d->slide_time = 0.35f; d->slide_lock_empty = false;
        d->fov_punch = 4.0f; d->roll_twitch = 1.0f; d->screen_kick = 0.018f; d->chroma = 0.6f;
        d->shoulder_push = 0.5f;
        d->first_mult = 1.0f;
        break;
    case FC_DMR:
        d->mass = 4.5f; d->moment = 0.5f; d->length = 0.75f;
        d->ch[FA_PITCH] = CH(4.0f, 11.0f, 0.38f, 0.4f, 0.1f, 0.0f);
        d->ch[FA_YAW] = CH(1.2f, 12.0f, 0.42f, 0.35f, 0.0f, 1.0f);
        d->ch[FA_ROLL] = CH(1.5f, 13.0f, 0.4f, 0.5f, 0.0f, 0.6f);
        d->ch[FA_BACK] = CH(0.026f, 12.0f, 0.42f, 0.7f, 0.08f, 0.0f);
        d->ch[FA_UP] = CH(0.012f, 11.0f, 0.42f, 0.0f, 0.0f, 0.0f);
        CURVE(d->first, { 0, 2.0f, 0, 0.5f, 0.014f, 0.004f }, { 0.06f, 1.2f, 0, 0.2f, 0.008f, 0.002f }, { 0.18f, 0, 0, 0, 0, 0 }, { 0.3f, -0.6f, 0, 0, -0.002f, 0 }, { 0.5f, 0, 0, 0, 0, 0 });
        d->sustained = d->first;
        CURVE(d->release, { 0, 0, 0, 0, 0, 0 }, { 0.4f, 0, 0, 0, 0, 0 });
        d->slide_travel = 0.035f; d->slide_time = 0.08f;
        d->fov_punch = 3.0f; d->roll_twitch = 0.7f; d->screen_kick = 0.012f; d->chroma = 0.2f;
        d->shoulder_push = 0.45f;
        d->first_mult = 1.1f;
        break;
    case FC_SNIPER:
        d->mass = 6.5f; d->moment = 0.55f; d->length = 0.95f;
        d->ch[FA_PITCH] = CH(8.0f, 7.0f, 0.33f, 0.35f, 0.08f, 0.0f);
        d->ch[FA_YAW] = CH(1.8f, 8.0f, 0.38f, 0.3f, 0.0f, 1.0f);
        d->ch[FA_ROLL] = CH(3.0f, 9.0f, 0.36f, 0.5f, 0.0f, 0.5f);
        d->ch[FA_BACK] = CH(0.05f, 8.0f, 0.36f, 0.65f, 0.08f, 0.0f);
        d->ch[FA_UP] = CH(0.02f, 7.0f, 0.38f, 0.0f, 0.0f, 0.0f);
        // Big and long, the shoulder takes it (pushback), a slow heavy settle.
        CURVE(d->first, { 0, 4.0f, 0, 1.0f, 0.03f, 0.01f }, { 0.08f, 3.0f, 0, 0.6f, 0.025f, 0.006f }, { 0.3f, 0.6f, 0, 0, 0.004f, 0 }, { 0.55f, -1.2f, 0, 0, -0.005f, 0 }, { 0.9f, 0, 0, 0, 0, 0 });
        d->sustained = d->first;
        CURVE(d->release, { 0, 0, 0, 0, 0, 0 }, { 0.6f, 0, 0, 0, 0, 0 });
        d->bolt_delay = 0.45f; d->slide_travel = 0.08f; d->slide_time = 0.6f; d->slide_lock_empty = false;
        d->fov_punch = 4.0f; d->roll_twitch = 1.2f; d->screen_kick = 0.02f; d->chroma = 0.7f;
        d->shoulder_push = 0.6f;
        d->first_mult = 1.0f;
        d->cam_delay = 3;
        break;
    case FC_CROSSBOW:
        d->mass = 3.0f; d->moment = 0.4f; d->length = 0.6f;
        d->ch[FA_PITCH] = CH(3.0f, 12.0f, 0.45f, 0.4f, 0.05f, 0.0f);
        d->ch[FA_YAW] = CH(0.5f, 13.0f, 0.45f, 0.35f, 0.0f, 1.0f);
        d->ch[FA_ROLL] = CH(0.5f, 14.0f, 0.45f, 0.5f, 0.0f, 0.5f);
        d->ch[FA_BACK] = CH(0.02f, 13.0f, 0.45f, 0.7f, 0.05f, 0.0f);
        d->ch[FA_UP] = CH(0.006f, 12.0f, 0.45f, 0.0f, 0.0f, 0.0f);
        CURVE(d->first, { 0, 1.2f, 0, 0.2f, 0.006f, 0.002f }, { 0.08f, 0.5f, 0, 0, 0.002f, 0 }, { 0.2f, -0.3f, 0, 0, 0, 0 }, { 0.4f, 0, 0, 0, 0, 0 });
        d->sustained = d->first;
        CURVE(d->release, { 0, 0, 0, 0, 0, 0 }, { 0.3f, 0, 0, 0, 0, 0 });
        d->fov_punch = 1.5f; d->roll_twitch = 0.3f; d->screen_kick = 0.006f;
        d->first_mult = 1.0f;
        break;
    default:   // FC_RIFLE
        d->mass = 3.6f; d->moment = 0.45f; d->length = 0.62f;
        d->ch[FA_PITCH] = CH(2.6f, 13.0f, 0.4f, 0.45f, 0.15f, 0.0f);
        d->ch[FA_YAW] = CH(0.9f, 14.0f, 0.45f, 0.35f, 0.08f, 1.0f);
        d->ch[FA_ROLL] = CH(1.2f, 15.0f, 0.4f, 0.5f, 0.0f, 0.7f);
        d->ch[FA_BACK] = CH(0.02f, 16.0f, 0.45f, 0.7f, 0.1f, 0.0f);
        d->ch[FA_UP] = CH(0.008f, 14.0f, 0.45f, 0.0f, 0.05f, 0.0f);
        // Muzzle up + back with a slight roll; the first shot pushes the shoulder harder.
        CURVE(d->first, { 0, 1.0f, 0, 0.5f, 0.014f, 0.003f }, { 0.05f, 0.5f, 0, 0.2f, 0.008f, 0.002f }, { 0.14f, 0, 0, 0, 0, 0 }, { 0.24f, -0.35f, 0, 0, -0.002f, 0 }, { 0.4f, 0, 0, 0, 0, 0 });
        CURVE(d->sustained, { 0, 1.0f, 0, 0.3f, 0.008f, 0.002f }, { 0.05f, 0.5f, 0, 0.1f, 0.004f, 0.001f }, { 0.14f, 0, 0, 0, 0, 0 });
        CURVE(d->release, { 0, 0, 0, 0, 0, 0 }, { 0.14f, -1.0f, 0, 0, -0.004f, 0 }, { 0.36f, 0.3f, 0, 0, 0.001f, 0 }, { 0.6f, 0, 0, 0, 0, 0 });
        d->slide_travel = 0.03f; d->slide_time = 0.06f;
        d->fov_punch = 2.5f; d->roll_twitch = 0.5f; d->screen_kick = 0.009f;
        break;
    }
    // Sustained fire rides up and back (the climb and the push build over the string).
    float ride = cls == FC_SMG ? 0.45f : cls == FC_LMG ? 0.5f : cls == FC_RIFLE ? 0.42f : 0.3f;
    d->ch[FA_PITCH].ride = ride;
    d->ch[FA_PITCH].ride_max = cls == FC_LMG ? 3.0f : 2.4f;
    d->ch[FA_BACK].ride = ride * 0.7f;
    d->ch[FA_BACK].ride_max = 1.6f;
    d->ch[FA_UP].ride = ride * 0.6f;
    d->ch[FA_UP].ride_max = 1.6f;
    d->ch[FA_YAW].ride = 0.15f;
    d->ch[FA_YAW].ride_max = 1.5f;
    d->ride_release = cls == FC_LMG ? 0.16f : 0.11f;
    d->ref_mass = d->mass;
    d->ref_moment = d->moment;
    fire_anim_resolve(d);
}

// ------------------------------------------------------------------ json

static const cJSON *jget(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = jget(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void parse_curve(FaCurve *c, const cJSON *a) {
    if (!cJSON_IsArray(a)) return;
    memset(c, 0, sizeof(*c));
    const cJSON *e;
    cJSON_ArrayForEach(e, a) {
        if (c->n >= FA_MAX_KEYS) break;
        if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) < 1 + FA_CH) continue;
        c->t[c->n] = (float)cJSON_GetArrayItem(e, 0)->valuedouble;
        for (int ch = 0; ch < FA_CH; ch++) c->v[c->n][ch] = (float)cJSON_GetArrayItem(e, 1 + ch)->valuedouble;
        c->n++;
    }
}

void fire_anim_parse(FireAnimDef *d, const cJSON *j) {
    if (!j) return;
    d->mass = jf(j, "mass", d->mass);
    d->moment = jf(j, "moment", d->moment);
    d->length = jf(j, "length", d->length);
    // A class file sets the reference the peaks were authored at; a weapon override only its mass.
    if (jget(j, "class_mass")) d->ref_mass = jf(j, "class_mass", d->ref_mass);
    if (jget(j, "class_moment")) d->ref_moment = jf(j, "class_moment", d->ref_moment);
    const cJSON *sp = jget(j, "springs");
    for (int ch = 0; ch < FA_CH; ch++) {
        const cJSON *o = jget(sp, k_ch_names[ch]);
        if (!o) continue;
        FaChannel *c = &d->ch[ch];
        c->peak = jf(o, "peak", c->peak);
        const cJSON *r = jget(o, "random");
        if (cJSON_IsArray(r) && cJSON_GetArraySize(r) >= 2) {
            c->rnd_min = (float)cJSON_GetArrayItem(r, 0)->valuedouble;
            c->rnd_max = (float)cJSON_GetArrayItem(r, 1)->valuedouble;
        }
        c->omega = jf(o, "omega", c->omega);
        c->zeta = jf(o, "zeta", c->zeta);
        c->ads = jf(o, "ads", c->ads);
        c->follow = jf(o, "follow", c->follow);
        c->sign_rnd = jf(o, "sign_random", c->sign_rnd);
        c->ride = jf(o, "ride", c->ride);
        c->ride_max = jf(o, "ride_max", c->ride_max);
    }
    d->first_mult = jf(j, "first_shot", d->first_mult);
    d->hip_scale = jf(j, "hip_scale", d->hip_scale);
    d->ads_scale = jf(j, "ads_scale", d->ads_scale);
    d->settle_time = jf(j, "string_gap", d->settle_time);
    d->ride_release = jf(j, "ride_release", d->ride_release);
    const cJSON *cv = jget(j, "curves");
    parse_curve(&d->first, jget(cv, "first"));
    parse_curve(&d->sustained, jget(cv, "sustained"));
    parse_curve(&d->release, jget(cv, "release"));
    const cJSON *hb = jget(j, "body");
    d->hand_omega = jf(hb, "hand_omega", d->hand_omega);
    d->hand_zeta = jf(hb, "hand_zeta", d->hand_zeta);
    d->hand_max_err = jf(hb, "hand_max_err", d->hand_max_err);
    d->shoulder_push = jf(hb, "shoulder_push", d->shoulder_push);
    d->shoulder_omega = jf(hb, "shoulder_omega", d->shoulder_omega);
    d->shoulder_zeta = jf(hb, "shoulder_zeta", d->shoulder_zeta);
    d->trigger_pull = jf(hb, "trigger_pull", d->trigger_pull);
    const cJSON *me = jget(j, "mechanical");
    d->slide_travel = jf(me, "slide_travel", d->slide_travel);
    d->slide_time = jf(me, "slide_time", d->slide_time);
    const cJSON *lk = jget(me, "lock_empty");
    if (cJSON_IsBool(lk)) d->slide_lock_empty = cJSON_IsTrue(lk);
    d->bolt_delay = jf(me, "bolt_delay", d->bolt_delay);
    d->wobble_omega = jf(me, "wobble_omega", d->wobble_omega);
    d->wobble_zeta = jf(me, "wobble_zeta", d->wobble_zeta);
    d->wobble_gain = jf(me, "wobble_gain", d->wobble_gain);
    d->heat_per_shot = jf(me, "heat_per_shot", d->heat_per_shot);
    d->heat_decay = jf(me, "heat_decay", d->heat_decay);
    d->droop = jf(me, "droop", d->droop);
    const cJSON *ca = jget(j, "camera");
    const cJSON *g = jget(ca, "gain");
    for (int ch = 0; ch < FA_CH; ch++) d->cam_gain[ch] = jf(g, k_ch_names[ch], d->cam_gain[ch]);
    d->cam_delay = (int)jf(ca, "delay_ticks", (float)d->cam_delay);
    d->cam_omega = jf(ca, "omega", d->cam_omega);
    d->cam_zeta = jf(ca, "zeta", d->cam_zeta);
    d->fov_punch = jf(ca, "fov_punch", d->fov_punch);
    d->fov_release = jf(ca, "fov_release", d->fov_release);
    d->roll_twitch = jf(ca, "roll_twitch", d->roll_twitch);
    d->screen_kick = jf(ca, "screen_kick", d->screen_kick);
    d->screen_release = jf(ca, "screen_release", d->screen_release);
    d->blur_ticks = jf(ca, "blur_ticks", d->blur_ticks);
    d->chroma = jf(ca, "chroma", d->chroma);
}

static double r5(float x) { return round((double)x * 100000.0) / 100000.0; }
static void put(cJSON *o, const char *k, float v) { cJSON_AddNumberToObject(o, k, r5(v)); }
static void put_curve(cJSON *o, const char *k, const FaCurve *c) {
    cJSON *a = cJSON_AddArrayToObject(o, k);
    for (int i = 0; i < c->n; i++) {
        cJSON *e = cJSON_CreateArray();
        cJSON_AddItemToArray(e, cJSON_CreateNumber(r5(c->t[i])));
        for (int ch = 0; ch < FA_CH; ch++) cJSON_AddItemToArray(e, cJSON_CreateNumber(r5(c->v[i][ch])));
        cJSON_AddItemToArray(a, e);
    }
}

char *fire_anim_to_json(const FireAnimDef *d) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "class", d->cls_name);
    put(o, "mass", d->mass);
    put(o, "moment", d->moment);
    put(o, "length", d->length);
    put(o, "class_mass", d->ref_mass);
    put(o, "class_moment", d->ref_moment);
    put(o, "first_shot", d->first_mult);
    put(o, "hip_scale", d->hip_scale);
    put(o, "ads_scale", d->ads_scale);
    put(o, "string_gap", d->settle_time);
    put(o, "ride_release", d->ride_release);
    cJSON *sp = cJSON_AddObjectToObject(o, "springs");
    for (int ch = 0; ch < FA_CH; ch++) {
        const FaChannel *c = &d->ch[ch];
        cJSON *e = cJSON_AddObjectToObject(sp, k_ch_names[ch]);
        put(e, "peak", c->peak);
        cJSON *r = cJSON_AddArrayToObject(e, "random");
        cJSON_AddItemToArray(r, cJSON_CreateNumber(r5(c->rnd_min)));
        cJSON_AddItemToArray(r, cJSON_CreateNumber(r5(c->rnd_max)));
        put(e, "omega", c->omega);
        put(e, "zeta", c->zeta);
        put(e, "ads", c->ads);
        put(e, "follow", c->follow);
        put(e, "sign_random", c->sign_rnd);
        put(e, "ride", c->ride);
        put(e, "ride_max", c->ride_max);
    }
    cJSON *cv = cJSON_AddObjectToObject(o, "curves");
    put_curve(cv, "first", &d->first);
    put_curve(cv, "sustained", &d->sustained);
    put_curve(cv, "release", &d->release);
    cJSON *hb = cJSON_AddObjectToObject(o, "body");
    put(hb, "hand_omega", d->hand_omega);
    put(hb, "hand_zeta", d->hand_zeta);
    put(hb, "hand_max_err", d->hand_max_err);
    put(hb, "shoulder_push", d->shoulder_push);
    put(hb, "shoulder_omega", d->shoulder_omega);
    put(hb, "shoulder_zeta", d->shoulder_zeta);
    put(hb, "trigger_pull", d->trigger_pull);
    cJSON *me = cJSON_AddObjectToObject(o, "mechanical");
    put(me, "slide_travel", d->slide_travel);
    put(me, "slide_time", d->slide_time);
    cJSON_AddBoolToObject(me, "lock_empty", d->slide_lock_empty);
    put(me, "bolt_delay", d->bolt_delay);
    put(me, "wobble_omega", d->wobble_omega);
    put(me, "wobble_zeta", d->wobble_zeta);
    put(me, "wobble_gain", d->wobble_gain);
    put(me, "heat_per_shot", d->heat_per_shot);
    put(me, "heat_decay", d->heat_decay);
    put(me, "droop", d->droop);
    cJSON *ca = cJSON_AddObjectToObject(o, "camera");
    cJSON *g = cJSON_AddObjectToObject(ca, "gain");
    for (int ch = 0; ch < FA_CH; ch++) put(g, k_ch_names[ch], d->cam_gain[ch]);
    put(ca, "delay_ticks", (float)d->cam_delay);
    put(ca, "omega", d->cam_omega);
    put(ca, "zeta", d->cam_zeta);
    put(ca, "fov_punch", d->fov_punch);
    put(ca, "fov_release", d->fov_release);
    put(ca, "roll_twitch", d->roll_twitch);
    put(ca, "screen_kick", d->screen_kick);
    put(ca, "screen_release", d->screen_release);
    put(ca, "blur_ticks", d->blur_ticks);
    put(ca, "chroma", d->chroma);
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    return s;
}

void fire_anim_att_parse(FaAttMod *m, const cJSON *j) {
    memset(m, 0, sizeof(*m));
    for (int ch = 0; ch < FA_CH; ch++) m->impulse[ch] = 1.0f;
    m->omega_mult = 1.0f;
    m->yaw_lag = 1.0f;
    m->settle_mult = 1.0f;
    m->fov_punch = 1.0f;
    m->screen_kick = 1.0f;
    if (!j) return;
    m->set = true;
    m->mass = jf(j, "mass", 0.0f);
    m->dist = jf(j, "dist", 0.0f);
    m->height = jf(j, "height", 0.0f);
    const cJSON *im = jget(j, "impulse");
    float all = jf(im, "all", 1.0f);
    for (int ch = 0; ch < FA_CH; ch++) m->impulse[ch] = all * jf(im, k_ch_names[ch], 1.0f);
    m->omega_mult = jf(j, "omega_mult", 1.0f);
    m->zeta_add = jf(j, "zeta_add", 0.0f);
    m->yaw_lag = jf(j, "yaw_lag", 1.0f);
    m->settle_mult = jf(j, "settle_mult", 1.0f);
    m->droop = jf(j, "droop", 0.0f);
    m->fov_punch = jf(j, "fov_punch", 1.0f);
    m->screen_kick = jf(j, "screen_kick", 1.0f);
}

void fire_anim_add_attachment(FireAnimDef *d, const FaAttMod *m) {
    if (!m || !m->set || d->att_count >= FA_MAX_ATT_MASS) return;
    d->att[d->att_count++] = *m;
}

// ------------------------------------------------------------------ mass model

// Radius of gyration about the grip: front-heavy guns (moment -> 1) carry their mass further out.
static float gyration(float moment, float length) { return 0.05f + (0.15f + 0.45f * saturate(moment)) * fmaxf(0.15f, length); }

static float inertia_of(const FireAnimDef *d, int ch, float mass, float moment, bool with_att) {
    float L = gyration(moment, d->length);
    float I;
    switch (ch) {
    case FA_PITCH: I = mass * L * L; break;
    case FA_YAW: I = mass * L * L; break;
    case FA_ROLL: I = mass * 0.045f * 0.045f * 4.0f; break;
    default: I = mass; break;   // translations
    }
    if (!with_att) return I;
    for (int i = 0; i < d->att_count; i++) {
        const FaAttMod *a = &d->att[i];
        switch (ch) {
        case FA_PITCH: I += a->mass * (a->dist * a->dist + a->height * a->height); break;
        case FA_YAW: I = (I + a->mass * (a->dist * a->dist + a->height * a->height)) * a->yaw_lag; break;
        case FA_ROLL: I += a->mass * a->height * a->height; break;
        default: I += a->mass; break;
        }
    }
    return I;
}

float fire_anim_inertia(const FireAnimDef *d, int ch) { return d->m[DG_CLAMP(ch, 0, FA_CH - 1)]; }

void fire_anim_resolve(FireAnimDef *d) {
    if (d->ref_mass <= 0.0f) d->ref_mass = fmaxf(0.1f, d->mass);
    if (d->mass <= 0.0f) d->mass = d->ref_mass;
    float omega_mult = 1.0f, zeta_add = 0.0f, settle = 1.0f, droop = d->droop, imp[FA_CH];
    for (int ch = 0; ch < FA_CH; ch++) imp[ch] = 1.0f;
    for (int i = 0; i < d->att_count; i++) {
        const FaAttMod *a = &d->att[i];
        omega_mult *= a->omega_mult;
        zeta_add += a->zeta_add;
        settle *= a->settle_mult;
        droop += a->droop;
        for (int ch = 0; ch < FA_CH; ch++) imp[ch] *= a->impulse[ch];
    }
    for (int ch = 0; ch < FA_CH; ch++) {
        const FaChannel *c = &d->ch[ch];
        float m_ref = inertia_of(d, ch, d->ref_mass, d->ref_moment, false);
        float m = inertia_of(d, ch, d->mass, d->moment, true);
        float w = fmaxf(0.5f, c->omega * omega_mult);
        float zeta = clampf(c->zeta + zeta_add, 0.05f, 2.0f);
        // Stiffness is set at the reference mass: added mass slows the spring down (w = sqrt(k / m)).
        float k = m_ref * w * w;
        d->k[ch] = k;
        d->m[ch] = m;
        d->c[ch] = 2.0f * zeta * sqrtf(k * m);   // the damping ratio holds at any mass: the overshoot stays
        // The impulse that gives `peak` at the reference mass; a heavier gun peaks at peak * sqrt(m_ref / m).
        d->J[ch] = c->peak * imp[ch] * sqrtf(k * m_ref);
    }
    d->time_stretch = sqrtf(d->m[FA_PITCH] / fmaxf(1e-6f, inertia_of(d, FA_PITCH, d->ref_mass, d->ref_moment, false))) * settle;
    d->droop_total = droop;
    float fp = 1.0f, sk = 1.0f;
    for (int i = 0; i < d->att_count; i++) { fp *= d->att[i].fov_punch; sk *= d->att[i].screen_kick; }
    d->fov_mult = fp;
    d->screen_mult = sk;
}

// ------------------------------------------------------------------ evaluation

static float curve_sample(const FaCurve *c, float t, int ch) {
    if (c->n <= 0 || t < 0.0f) return 0.0f;
    if (t >= c->t[c->n - 1]) return c->v[c->n - 1][ch];
    if (t <= c->t[0]) return c->v[0][ch];
    for (int i = 0; i < c->n - 1; i++) {
        if (t <= c->t[i + 1]) {
            float u = (t - c->t[i]) / fmaxf(1e-6f, c->t[i + 1] - c->t[i]);
            u = u * u * (3.0f - 2.0f * u);   // smooth between keys (no corners in the motion)
            return lerpf(c->v[i][ch], c->v[i + 1][ch], u);
        }
    }
    return c->v[c->n - 1][ch];
}
static float curve_len(const FaCurve *c) { return c->n > 0 ? c->t[c->n - 1] : 0.0f; }
static const FaCurve *curve_of(const FireAnimDef *d, int kind) {
    return kind == 1 ? &d->first : kind == 2 ? &d->sustained : kind == 3 ? &d->release : NULL;
}

// The curve value of the playing instance (deg / m), already scaled.
static float layer1(const FireAnimState *s, const FireAnimDef *d, int ch) {
    const FaCurve *c = curve_of(d, s->curve_kind);
    if (!c) return 0.0f;
    float ts = fmaxf(0.2f, d->time_stretch);
    return curve_sample(c, s->curve_t / ts, ch) * s->curve_amp * s->curve_ch[ch] * d->curve_amp[ch];
}

void fire_anim_reset(FireAnimState *s) { memset(s, 0, sizeof(*s)); }

void fire_anim_shot(FireAnimState *s, const FireAnimDef *d, const FireShot *shot) {
    float sub = saturate(shot->sub_tick);
    float scale = shot->scale * lerpf(d->hip_scale, d->ads_scale, saturate(shot->ads_t));
    bool first = s->string_shots == 0 || s->since_shot > d->settle_time;
    if (first) s->string_shots = 0;
    Rng rng = rng_make((u64)shot->seed * 2654435761ull + 17u);
    float mult = first ? d->first_mult : 1.0f;
    // Layer 1 crop: the unfinished curve is frozen into the springs (auto fire compounds, no snap).
    float before[FA_CH];
    for (int ch = 0; ch < FA_CH; ch++) before[ch] = s->gun[ch].x + layer1(s, d, ch);
    if (s->curve_kind == 1 || s->curve_kind == 2) {
        for (int ch = 0; ch < FA_CH; ch++) s->gun[ch].x += layer1(s, d, ch);
    }
    for (int ch = 0; ch < FA_CH; ch++) {
        const FaChannel *c = &d->ch[ch];
        float r = rng_range(&rng, c->rnd_min, c->rnd_max);
        float sg = (c->sign_rnd > 0.0f && rng_float(&rng) < c->sign_rnd * 0.5f) ? -1.0f : 1.0f;
        float ads = lerpf(1.0f, c->ads, saturate(shot->ads_t));
        // The first shot of a string pushes the shoulder harder (kickback); the climb builds with the string.
        float J = d->J[ch] * r * sg * ads * scale * (ch == FA_BACK ? mult : 1.0f);
        float m = fmaxf(1e-6f, d->m[ch]);
        float amp = J / sqrtf(fmaxf(1e-9f, d->k[ch] * m));   // where a velocity impulse would peak
        float f = saturate(c->follow);
        // The kick is a displacement on this tick (no ease-in), plus a little momentum that carries.
        s->gun[ch].x += amp * (1.0f - f);
        s->gun[ch].v += f * J / m;
        // Ride: the rest point moves with the string, saturating at ride_max x the kick.
        if (c->ride > 0.0f && !first) {
            float lim = c->ride_max * fabsf(amp) + 1e-9f;
            s->ride[ch] += amp * c->ride * saturate(1.0f - fabsf(s->ride[ch]) / lim);
        }
        s->curve_ch[ch] = r * sg * ads;
    }
    s->curve_kind = first ? 1 : 2;
    s->curve_t = -sub * (1.0f / 60.0f);
    s->curve_amp = scale * sqrtf(fmaxf(1e-6f, inertia_of(d, FA_PITCH, d->ref_mass, d->ref_moment, false)) / fmaxf(1e-6f, d->m[FA_PITCH]));
    s->crop = shot->interval;
    // The gun jumped this tick; the hands stay where they were and catch up (layer 3).
    for (int ch = 0; ch < FA_CH; ch++) {
        float after = s->gun[ch].x + layer1(s, d, ch);
        s->hand[ch].x -= after - before[ch];
    }
    s->string_shots++;
    s->since_shot = 0.0f;
    s->kicked = true;
    s->firing = true;
    s->heat = fminf(1.0f, s->heat + d->heat_per_shot);
    // Layer 3: trigger pull, shoulders.
    s->trigger = 1.0f;
    // Layer 4: slide / bolt.
    s->slide_t = 0.0f;
    s->slide_active = d->slide_travel > 0.0f;
    s->slide_locked = d->slide_lock_empty && shot->mag_left <= 0 && d->bolt_delay <= 0.0f;
    s->eject_pending = false;
    s->eject_done = false;
    // Layer 5: perceptual punches (scaled with the shot).
    float ps = scale * mult;
    s->fov = fmaxf(s->fov * (1.0f - saturate(s->fov_t / fmaxf(1e-3f, d->fov_release))), d->fov_punch * d->fov_mult * ps);
    s->fov_t = -sub * (1.0f / 60.0f);
    s->roll_sign = rng_float(&rng) < 0.5f ? -1.0f : 1.0f;
    s->roll_amp = d->roll_twitch * ps;
    s->roll_ticks = 0;
    s->screen = fmaxf(s->screen, d->screen_kick * d->screen_mult * ps);
    s->blur_left = (int)(d->blur_ticks + 0.5f);
}

static void spring_step(FaSpring *sp, float k, float c, float m, float rest, float dt) {
    // Semi-implicit Euler, 4 substeps (stable for the stiff hand springs at 60 Hz).
    float h = dt * 0.25f, im = 1.0f / fmaxf(1e-6f, m);
    for (int i = 0; i < 4; i++) {
        sp->v += (-(sp->x - rest) * k - sp->v * c) * im * h;
        sp->x += sp->v * h;
    }
}
static void follow_step(FaSpring *sp, float target, float w, float zeta, float dt) {
    float h = dt * 0.25f;
    for (int i = 0; i < 4; i++) {
        sp->v += (w * w * (target - sp->x) - 2.0f * zeta * w * sp->v) * h;
        sp->x += sp->v * h;
    }
}

void fire_anim_step(FireAnimState *s, const FireAnimDef *d, float dt, bool trigger_held, float perceptual, FireAnimOut *out) {
    FireAnimOut o;
    memset(&o, 0, sizeof(o));
    s->prev = s->last;
    s->tick++;
    s->since_shot += dt;
    // String end: the trigger let go (or the gap grew past the fire interval): the release settle.
    if (s->firing && (!trigger_held || s->since_shot > fmaxf(d->settle_time, s->crop * 1.6f))) {
        s->firing = false;
        if (s->string_shots >= 2 && d->release.n > 0) {
            if (s->curve_kind == 1 || s->curve_kind == 2)
                for (int ch = 0; ch < FA_CH; ch++) s->gun[ch].x += layer1(s, d, ch);
            float amt = fminf(1.0f, (float)s->string_shots / 6.0f);
            s->curve_kind = 3;
            s->curve_t = 0.0f;
            for (int ch = 0; ch < FA_CH; ch++) s->curve_ch[ch] = 1.0f;
            s->curve_amp = amt * sqrtf(fmaxf(1e-6f, s->heat + 0.5f));
        }
    }
    if (s->since_shot > d->settle_time && !s->firing && s->curve_kind != 3) s->string_shots = 0;
    // Layer 1.
    s->curve_t += dt;
    const FaCurve *cc = curve_of(d, s->curve_kind);
    if (cc && s->curve_t > curve_len(cc) * fmaxf(0.2f, d->time_stretch)) {
        if (s->curve_kind == 3) s->string_shots = 0;
        s->curve_kind = 0;
    }
    // Layer 2: mass springs (front-heavy droop builds with the heat of sustained fire).
    s->heat = fmaxf(0.0f, s->heat - d->heat_decay * dt);
    float gun[FA_CH], hand[FA_CH];
    if (!s->firing) {
        float kr = expf(-dt / fmaxf(1e-3f, d->ride_release * fmaxf(0.2f, d->time_stretch)));
        for (int ch = 0; ch < FA_CH; ch++) s->ride[ch] *= kr;
    }
    for (int ch = 0; ch < FA_CH; ch++) {
        float rest = s->ride[ch] + (ch == FA_PITCH ? -d->droop_total * s->heat : 0.0f);
        spring_step(&s->gun[ch], d->k[ch], d->c[ch], d->m[ch], rest, dt);
        gun[ch] = s->gun[ch].x + layer1(s, d, ch);
    }
    // Layer 3: the hands trail the gun (soft springs), the shoulders take the push.
    // The hands ride the gun's smooth motion exactly (glue); only the one-tick kick leaves them
    // behind (hand = gun + error), and they close that gap over the next 1-2 ticks with flex.
    for (int ch = 0; ch < FA_CH; ch++) {
        follow_step(&s->hand[ch], 0.0f, d->hand_omega, d->hand_zeta, dt);
        hand[ch] = gun[ch] + s->hand[ch].x;
    }
    follow_step(&s->shoulder, fmaxf(0.0f, gun[FA_BACK]) * d->shoulder_push, d->shoulder_omega, d->shoulder_zeta, dt);
    s->trigger = fmaxf(0.0f, s->trigger - dt / 0.09f);
    // Layer 4: slide / bolt (fast back, eased forward; locks back on empty), attachment wobble.
    if (s->slide_active) {
        s->slide_t += dt;
        float t0 = d->bolt_delay;
        float u = s->slide_t - t0;
        float travel = d->slide_travel;
        if (u < 0.0f) s->slide = 0.0f;
        else if (s->slide_locked) s->slide = travel;
        else {
            float back = d->bolt_delay > 0.0f ? d->slide_time * 0.4f : 1.0f / 60.0f;
            float fwd = d->bolt_delay > 0.0f ? d->slide_time * 0.6f : d->slide_time;
            if (u < back) s->slide = travel * saturate(u / back);
            else {
                float r = saturate((u - back) / fmaxf(1e-3f, fwd));
                s->slide = travel * (1.0f - r * r * (3.0f - 2.0f * r));
                if (!s->eject_done && (d->bolt_delay > 0.0f ? r > 0.0f : r >= 0.5f)) { s->eject_pending = true; s->eject_done = true; }
                if (r >= 1.0f) s->slide_active = false;
            }
        }
    }
    follow_step(&s->wobble[0], gun[FA_PITCH], d->wobble_omega, d->wobble_zeta, dt);
    follow_step(&s->wobble[1], gun[FA_ROLL], d->wobble_omega, d->wobble_zeta, dt);
    // Layer 5: the camera follows the body 2-3 ticks late, with its own overshoot.
    int dl = DG_CLAMP(d->cam_delay, 0, FA_CAM_DELAY_MAX);
    for (int i = FA_CAM_DELAY_MAX; i > 0; i--) {
        s->shoulder_hist[i] = s->shoulder_hist[i - 1];
        for (int ch = 0; ch < FA_CH; ch++) s->hand_hist[i][ch] = s->hand_hist[i - 1][ch];
    }
    s->shoulder_hist[0] = s->shoulder.x;
    for (int ch = 0; ch < FA_CH; ch++) s->hand_hist[0][ch] = hand[ch];
    for (int ch = 0; ch < FA_CH; ch++) {
        float src = ch == FA_BACK ? s->shoulder_hist[dl] : s->hand_hist[dl][ch];
        follow_step(&s->cam[ch], src * d->cam_gain[ch], d->cam_omega, d->cam_zeta, dt);
    }
    s->fov_t += dt;
    float fu = saturate(s->fov_t / fmaxf(1e-3f, d->fov_release));
    float fov = s->fov * (1.0f - fu) * (1.0f - fu);
    if (fu >= 1.0f) s->fov = 0.0f;
    float roll = 0.0f;
    if (s->roll_amp > 0.0f) {
        roll = s->roll_ticks == 0 ? s->roll_amp : (s->roll_ticks == 1 ? s->roll_amp * 0.4f : 0.0f);
        if (++s->roll_ticks >= 2) s->roll_amp = 0.0f;
        roll *= s->roll_sign;
    }
    float screen = s->screen;
    s->screen *= expf(-dt / fmaxf(1e-3f, d->screen_release / 3.0f));
    float blur = 0.0f;
    if (s->blur_left > 0) { blur = s->blur_left >= (int)(d->blur_ticks + 0.5f) ? 1.0f : 0.5f; s->blur_left--; }

    o.gun_pos = v3(0, gun[FA_UP], gun[FA_BACK]);
    o.gun_rot = v3(gun[FA_PITCH], gun[FA_YAW], gun[FA_ROLL]);
    vec3 he = v3(0, s->hand[FA_UP].x, s->hand[FA_BACK].x);
    float hl = v3_len(he);
    if (hl > d->hand_max_err && hl > 0.0f) he = v3_scale(he, d->hand_max_err / hl);
    o.hand_pos = he;
    vec3 hr = v3(s->hand[FA_PITCH].x, s->hand[FA_YAW].x, s->hand[FA_ROLL].x);
    float hrl = v3_len(hr), hrmax = 6.0f;
    if (hrl > hrmax) hr = v3_scale(hr, hrmax / hrl);
    o.hand_rot = hr;
    o.shoulder = s->shoulder.x;
    o.trigger = s->trigger * d->trigger_pull;
    o.slide = s->slide;
    o.wobble_rot = v3((s->wobble[0].x - gun[FA_PITCH]) * d->wobble_gain, 0, (s->wobble[1].x - gun[FA_ROLL]) * d->wobble_gain);
    o.cam_rot = v3(s->cam[FA_PITCH].x, s->cam[FA_YAW].x, s->cam[FA_ROLL].x + roll * perceptual);
    o.cam_back = s->cam[FA_BACK].x;
    o.fov_punch = fov * perceptual;
    o.roll_twitch = roll * perceptual;
    o.screen_kick = screen;
    o.blur = blur * perceptual;
    o.chroma = blur > 0.0f ? d->chroma * perceptual : 0.0f;
    s->last = o;
    if (s->tick == 1) s->prev = o;
    if (out) *out = o;
}

void fire_anim_sample(const FireAnimState *s, const FireAnimOut *cur, float alpha, FireAnimOut *out) {
    const FireAnimOut *a = &s->prev;
    FireAnimOut o = *cur;
    float t = saturate(alpha);
    o.gun_pos = v3_lerp(a->gun_pos, cur->gun_pos, t);
    o.gun_rot = v3_lerp(a->gun_rot, cur->gun_rot, t);
    o.hand_pos = v3_lerp(a->hand_pos, cur->hand_pos, t);
    o.hand_rot = v3_lerp(a->hand_rot, cur->hand_rot, t);
    o.shoulder = lerpf(a->shoulder, cur->shoulder, t);
    o.trigger = lerpf(a->trigger, cur->trigger, t);
    o.slide = lerpf(a->slide, cur->slide, t);
    o.wobble_rot = v3_lerp(a->wobble_rot, cur->wobble_rot, t);
    o.cam_rot = v3_lerp(a->cam_rot, cur->cam_rot, t);
    o.cam_back = lerpf(a->cam_back, cur->cam_back, t);
    o.fov_punch = lerpf(a->fov_punch, cur->fov_punch, t);
    o.screen_kick = lerpf(a->screen_kick, cur->screen_kick, t);
    // The 1-2 tick punches (roll twitch, blur, chroma) are not interpolated: they hold for their tick.
    *out = o;
}

bool fire_anim_take_eject(FireAnimState *s) {
    bool e = s->eject_pending;
    s->eject_pending = false;
    return e;
}

FaMetrics fire_anim_metrics(const float *x, int n, float settle_frac) {
    FaMetrics m = { 0, -1, 0, 0 };
    for (int i = 0; i < n; i++)
        if (fabsf(x[i]) > fabsf(m.peak)) { m.peak = x[i]; m.peak_tick = i; }
    if (m.peak_tick < 0) return m;
    float lim = fabsf(m.peak) * settle_frac;
    m.settle_tick = m.peak_tick;
    for (int i = n - 1; i >= 0; i--)
        if (fabsf(x[i]) > lim) { m.settle_tick = i + 1; break; }
    float sg = m.peak > 0 ? 1.0f : -1.0f, os = 0.0f;
    for (int i = m.peak_tick; i < n; i++) os = fmaxf(os, -x[i] * sg);
    m.overshoot = fabsf(m.peak) > 1e-9f ? os / fabsf(m.peak) : 0.0f;
    return m;
}
