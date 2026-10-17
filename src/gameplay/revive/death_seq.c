// First-person death sequence: timeline, curves and the ragdoll impact detector (pure).
#include "death_seq.h"
#include "core/dgmath.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>

void death_params_defaults(DeathSeqParams *p) {
    memset(p, 0, sizeof(*p));
    p->s1_time = 0.4f;
    p->desat = 0.95f;
    p->contrast = -0.15f;
    p->vignette = 0.35f;
    p->blur = 0.35f;
    p->exposure = -0.4f;
    p->impact_timeout = 2.5f;
    p->impact_min_time = 0.15f;
    p->impact_min_speed = 1.2f;
    p->impact_speed_drop = 0.45f;
    p->impact_contact_dist = 0.14f;
    p->hold_time = 1.0f;
    p->fade_in = 0.8f;
    p->lpf_from = 20000.0f;
    p->lpf_to = 400.0f;
    p->lpf_sweep = 0.5f;
    p->world_duck = 0.45f;
    p->ring_attack = 0.25f;
    p->ring_level = 0.32f;
    p->ring_fade = 2.0f;
    p->ring_freq = 3600.0f;
    p->warble_hz = 5.5f;
    p->warble_depth = 0.004f;
    p->heart_level = 0.5f;
    p->heart_bpm_start = 92.0f;
    p->heart_bpm_end = 46.0f;
    p->d_desat = 0.45f;
    p->d_vignette = 0.22f;
    p->d_blur = 0.08f;
    p->d_lpf = 2500.0f;
    p->d_ring = 0.06f;
    p->d_attack = 0.6f;
    p->d_release = 0.8f;
    p->d_world = 0.8f;
}

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

bool death_params_parse_visual(DeathSeqParams *p, const char *json, char *err, int err_size) {
    cJSON *r = cJSON_Parse(json);
    if (!r) {
        if (err) snprintf(err, (size_t)err_size, "death_sequence.json: parse error");
        return false;
    }
    const cJSON *s1 = cJSON_GetObjectItemCaseSensitive(r, "stage1");
    p->s1_time = fmaxf(jf(s1, "time", p->s1_time), 0.01f);
    p->desat = jf(s1, "desaturate", p->desat);
    p->contrast = jf(s1, "contrast", p->contrast);
    p->vignette = jf(s1, "vignette", p->vignette);
    p->blur = jf(s1, "blur", p->blur);
    p->exposure = jf(s1, "exposure", p->exposure);
    const cJSON *im = cJSON_GetObjectItemCaseSensitive(r, "impact");
    p->impact_timeout = jf(im, "timeout", p->impact_timeout);
    p->impact_min_time = jf(im, "min_time", p->impact_min_time);
    p->impact_min_speed = jf(im, "min_speed", p->impact_min_speed);
    p->impact_speed_drop = jf(im, "speed_drop", p->impact_speed_drop);
    p->impact_contact_dist = jf(im, "contact_dist", p->impact_contact_dist);
    const cJSON *s2 = cJSON_GetObjectItemCaseSensitive(r, "stage2");
    p->hold_time = jf(s2, "hold_time", p->hold_time);
    p->fade_in = fmaxf(jf(s2, "fade_in", p->fade_in), 0.01f);
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(r, "downed");
    p->d_desat = jf(d, "desaturate", p->d_desat);
    p->d_vignette = jf(d, "vignette", p->d_vignette);
    p->d_blur = jf(d, "blur", p->d_blur);
    p->d_attack = fmaxf(jf(d, "attack", p->d_attack), 0.01f);
    p->d_release = fmaxf(jf(d, "release", p->d_release), 0.01f);
    cJSON_Delete(r);
    return true;
}

bool death_params_parse_ring(DeathSeqParams *p, const char *json, char *err, int err_size) {
    cJSON *r = cJSON_Parse(json);
    if (!r) {
        if (err) snprintf(err, (size_t)err_size, "death_ring.json: parse error");
        return false;
    }
    const cJSON *ring = cJSON_GetObjectItemCaseSensitive(r, "ring");
    p->ring_freq = jf(ring, "pitch", p->ring_freq);
    p->warble_hz = jf(ring, "warble_hz", p->warble_hz);
    p->warble_depth = jf(ring, "warble_depth", p->warble_depth);
    p->ring_level = jf(ring, "level", p->ring_level);
    p->ring_attack = fmaxf(jf(ring, "attack", p->ring_attack), 0.01f);
    p->ring_fade = fmaxf(jf(ring, "fade", p->ring_fade), 0.01f);
    const cJSON *mix = cJSON_GetObjectItemCaseSensitive(r, "mix");
    p->lpf_from = jf(mix, "lpf_from", p->lpf_from);
    p->lpf_to = jf(mix, "lpf_to", p->lpf_to);
    p->lpf_sweep = fmaxf(jf(mix, "sweep", p->lpf_sweep), 0.01f);
    p->world_duck = jf(mix, "world_duck", p->world_duck);
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(r, "heartbeat");
    p->heart_level = jf(h, "level", p->heart_level);
    p->heart_bpm_start = jf(h, "bpm_start", p->heart_bpm_start);
    p->heart_bpm_end = jf(h, "bpm_end", p->heart_bpm_end);
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(r, "downed");
    p->d_lpf = jf(d, "lpf", p->d_lpf);
    p->d_ring = jf(d, "ring", p->d_ring);
    p->d_world = jf(d, "world", p->d_world);
    cJSON_Delete(r);
    return true;
}

const char *death_stage_name(DeathStage s) {
    static const char *n[] = { "none", "fall", "black", "fade_in", "done" };
    return (unsigned)s <= DS_DONE ? n[s] : "?";
}

void deathseq_reset(DeathSeq *s) {
    float dw = s->downed_w;
    memset(s, 0, sizeof(*s));
    s->downed_w = dw;   // the downed variant recovers along its own curve
}

void deathseq_start(DeathSeq *s) {
    float dw = s->downed_w;
    memset(s, 0, sizeof(*s));
    s->downed_w = dw;
    s->stage = DS_FALL;
}

void deathseq_impact(DeathSeq *s, ImpactReason why) {
    if (s->stage != DS_FALL) return;
    s->stage = DS_BLACK;
    s->t_stage = 0;
    s->cut_pending = 1;
    s->impact_t = s->t;
    s->impact_reason = (u8)why;
}

void deathseq_skip(DeathSeq *s) {
    if (s->stage == DS_FALL) deathseq_impact(s, IMPACT_FORCED);
    if (s->stage == DS_BLACK) {
        s->stage = DS_FADE_IN;
        s->t_stage = 0;
        s->cut_pending = 1;
        s->skipped = 1;
    }
}

void deathseq_step(DeathSeq *s, const DeathSeqParams *p, float dt, bool downed) {
    float rate = downed ? dt / p->d_attack : -dt / p->d_release;
    s->downed_w = saturate(s->downed_w + rate);
    if (s->stage == DS_NONE || s->stage == DS_DONE) return;
    s->t += dt;
    s->t_stage += dt;
    switch ((DeathStage)s->stage) {
    case DS_FALL:
        if (s->t_stage >= p->impact_timeout) deathseq_impact(s, IMPACT_TIMEOUT);
        break;
    case DS_BLACK:
        if (s->t_stage >= p->ring_fade + p->hold_time) {
            s->stage = DS_FADE_IN;
            s->t_stage = 0;
            s->cut_pending = 1;   // the view may have switched to spectating under the black
        }
        break;
    case DS_FADE_IN:
        if (s->t_stage >= p->fade_in) s->stage = DS_DONE;
        break;
    default:
        break;
    }
}

void deathseq_frame_done(DeathSeq *s) { s->cut_pending = 0; }

DeathFx deathfx_neutral(const DeathSeqParams *p) {
    DeathFx f;
    memset(&f, 0, sizeof(f));
    f.lpf_hz = p->lpf_from;
    f.world_gain = 1.0f;
    return f;
}

// Log-space cutoff interpolation (a linear sweep in Hz would sit near 20 kHz too long).
static float lpf_lerp(float from, float to, float t) {
    from = fmaxf(from, 20.0f);
    to = fmaxf(to, 20.0f);
    return from * powf(to / from, saturate(t));
}

static float ease_out3(float u) { u = saturate(u); float v = 1.0f - u; return 1.0f - v * v * v; }

static float ring_attack_env(const DeathSeqParams *p, float t) { return p->ring_level * smoothstepf(0.0f, p->ring_attack, t); }

static void fall_visual(DeathFx *f, const DeathSeqParams *p, float t) {
    float e = ease_out3(t / p->s1_time);
    f->desat = p->desat * e;
    f->contrast = p->contrast * e;
    f->vignette = p->vignette * e;
    f->blur = p->blur * e;
    // Exposure drifts on slower than the desaturation.
    f->exposure = p->exposure * smoothstepf(0.0f, p->s1_time * 3.0f, t);
}

static DeathFx eval_stage(const DeathSeq *s, const DeathSeqParams *p) {
    DeathFx f = deathfx_neutral(p);
    switch ((DeathStage)s->stage) {
    case DS_FALL: {
        fall_visual(&f, p, s->t);
        f.lpf_hz = lpf_lerp(p->lpf_from, p->lpf_to, s->t / p->lpf_sweep);
        f.world_gain = lerpf(1.0f, p->world_duck, smoothstepf(0.0f, p->lpf_sweep, s->t));
        f.ring_gain = ring_attack_env(p, s->t);
        f.heart_gain = p->heart_level;
        f.heart_bpm = lerpf(p->heart_bpm_start, p->heart_bpm_end, saturate(s->t / fmaxf(p->impact_timeout, 0.1f)));
        f.hud_hidden = true;
        break;
    }
    case DS_BLACK: {
        fall_visual(&f, p, s->impact_t);
        f.black = 1.0f;
        float k = 1.0f - smoothstepf(0.0f, p->ring_fade, s->t_stage);   // zero slope at both ends: no pops
        float tc = s->impact_t;
        f.lpf_hz = lpf_lerp(p->lpf_from, p->lpf_to, tc / p->lpf_sweep);
        f.world_gain = lerpf(1.0f, p->world_duck, smoothstepf(0.0f, p->lpf_sweep, tc)) * k;
        f.ring_gain = ring_attack_env(p, tc) * k;
        f.heart_gain = 0.0f;   // the heartbeat stops at the cut
        f.camera_cut = s->cut_pending != 0;
        f.hud_hidden = true;
        break;
    }
    case DS_FADE_IN: {
        float k = smoothstepf(0.0f, p->fade_in, s->t_stage);
        f.black = 1.0f - k;
        f.lpf_hz = lpf_lerp(p->lpf_to, p->lpf_from, k);
        f.world_gain = k;
        f.camera_cut = s->cut_pending != 0;
        break;
    }
    default:
        break;
    }
    return f;
}

static void apply_downed(DeathFx *f, const DeathSeqParams *p, float w) {
    if (w <= 0.0f) return;
    float e = smoothstepf(0.0f, 1.0f, w);
    f->desat = fmaxf(f->desat, p->d_desat * e);
    f->vignette = fmaxf(f->vignette, p->d_vignette * e);
    f->blur = fmaxf(f->blur, p->d_blur * e);
    f->lpf_hz = fminf(f->lpf_hz, lpf_lerp(p->lpf_from, p->d_lpf, e));
    f->ring_gain = fmaxf(f->ring_gain, p->d_ring * e);
    f->world_gain = fminf(f->world_gain, lerpf(1.0f, p->d_world, e));
}

DeathFx deathseq_eval(const DeathSeq *s, const DeathSeqParams *p) {
    DeathFx f = eval_stage(s, p);
    // The downed variant never darkens a black frame or a fade into spectating.
    if (s->stage == DS_NONE || s->stage == DS_DONE || s->stage == DS_FALL) apply_downed(&f, p, s->downed_w);
    return f;
}

DeathFx deathseq_preview(const DeathSeqParams *p, int stage, float t01) {
    DeathSeq s;
    memset(&s, 0, sizeof(s));
    t01 = saturate(t01);
    if (stage == 1) {
        s.stage = DS_FALL;
        s.t = s.t_stage = t01 * p->impact_timeout;
    } else if (stage == 2) {
        float black = p->ring_fade + p->hold_time;
        float t = t01 * (black + p->fade_in);
        s.impact_t = p->impact_timeout * 0.5f;
        if (t < black) { s.stage = DS_BLACK; s.t_stage = t; }
        else { s.stage = DS_FADE_IN; s.t_stage = t - black; }
    } else {
        s.downed_w = t01;
    }
    return deathseq_eval(&s, p);
}

ImpactReason death_impact_detect(DeathSeq *s, const DeathSeqParams *p, float head_speed, float chest_speed, bool contact) {
    if (s->stage != DS_FALL) return IMPACT_NONE;
    float speed = fmaxf(head_speed, chest_speed);
    ImpactReason why = IMPACT_NONE;
    if (s->t >= p->impact_min_time) {
        if (contact && fmaxf(s->peak_speed, speed) >= 0.5f) why = IMPACT_CONTACT;   // significant: the body was moving
        else if (s->last_speed >= p->impact_min_speed && speed < s->last_speed * (1.0f - p->impact_speed_drop)) why = IMPACT_VELOCITY;
    }
    s->peak_speed = fmaxf(s->peak_speed, speed);
    s->last_speed = speed;
    if (why != IMPACT_NONE) deathseq_impact(s, why);
    return why;
}
