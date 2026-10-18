#include "syn_graph.h"
#include "syn_material.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------ node table

static const SynNodeInfo k_info[SN_COUNT] = {
    [SN_OSC] = { "osc", { "hz", "amp", "fm", "pw", "phase" }, { 440, 1, 0, 0.5f, 0 }, "sine|saw|square|tri", 0, 1 },
    [SN_NOISE] = { "noise", { "amp", "density" }, { 1, 2000 }, "white|pink|brown|velvet", 0, 0 },
    [SN_IMPULSE] = { "impulse", { "amp", "width_ms", "delay_ms", "repeat_ms", "count" }, { 1, 0, 0, 0, 1 }, NULL, 0, 0 },
    [SN_FRIEDLANDER] = { "friedlander", { "amp", "dur_ms", "b", "delay_ms" }, { 1, 2, 1.5f, 0 }, NULL, 0, 0 },
    [SN_NWAVE] = { "nwave", { "amp", "dur_ms", "rise_us", "delay_ms" }, { 1, 0.3f, 20, 0 }, NULL, 0, 0 },
    [SN_PULSES] = { "pulses", { "hz", "amp", "jitter", "width_ms", "decay_ms", "count", "spread" }, { 30, 1, 0, 0.5f, 3, 4, 0.2f }, NULL, 0, 0 },
    [SN_GLOTTAL] = { "glottal", { "hz", "amp", "open", "jitter", "shimmer", "fry", "fry_hz", "breath" }, { 110, 1, 0.6f, 0.01f, 0.05f, 0, 30, 0.1f }, NULL, 0, 1 },
    [SN_GRAINS] = { "grains", { "rate", "amp", "hz", "spread", "dur_ms", "decay", "q" }, { 100, 1, 3000, 0.5f, 5, 0, 4 }, "noise|sine|click|sample", 0, 0 },
    [SN_ENV] = { "env", { "amp", "time", "gate" }, { 1, 1, 1 }, NULL, 0, 1 },
    [SN_BIQUAD] = { "biquad", { "hz", "q", "gain_db" }, { 1000, 0.707f, 0 }, "lp|hp|bp|notch|peak|lshelf|hshelf|ap", 1, 1 },
    [SN_SVF] = { "svf", { "hz", "q" }, { 1000, 0.707f }, "lp|hp|bp|notch", 1, 1 },
    [SN_ONEPOLE] = { "onepole", { "hz" }, { 1000 }, "lp|hp", 1, 1 },
    [SN_COMB] = { "comb", { "ms", "fb", "damp", "mix" }, { 10, 0.5f, 0.2f, 1 }, NULL, 1, 1 },
    [SN_ALLPASS] = { "allpass", { "ms", "g" }, { 5, 0.5f }, NULL, 1, 1 },
    [SN_TILT] = { "tilt", { "db", "pivot" }, { 0, 1000 }, NULL, 1, 1 },
    [SN_SHAPER] = { "shaper", { "drive", "mix", "amp", "bias" }, { 1, 1, 1, 0 }, "tanh|fold|clip|asym", 1, 1 },
    [SN_RING] = { "ring", { "amp" }, { 1 }, NULL, 2, 2 },
    [SN_AM] = { "am", { "hz", "depth", "amp" }, { 30, 0.5f, 1 }, "sine|noise", 1, 2 },
    [SN_GAIN] = { "gain", { "amp", "db" }, { 1, 0 }, NULL, 1, 1 },
    [SN_MIX] = { "mix", { "amp" }, { 1 }, NULL, 1, SYN_MAX_INPUTS },
    [SN_MODAL] = { "modal", { "pitch", "damp", "amp", "material", "spread", "bright" }, { 1, 1, 1, -1, 0, 0 }, NULL, 1, 1 },
    [SN_KARPLUS] = { "karplus", { "hz", "t60", "bright", "amp" }, { 110, 1, 0.5f, 1 }, NULL, 0, 1 },
    [SN_WAVEGUIDE] = { "waveguide", { "length", "reflect", "loss", "amp", "c" }, { 1, -0.9f, 0.3f, 1, 343 }, NULL, 1, 1 },
    [SN_FORMANT] = { "formant", { "vowel", "shift", "amp", "bw" }, { 0, 1, 1, 1 }, NULL, 1, 1 },
    [SN_DELAY] = { "delay", { "ms", "fb", "mix" }, { 50, 0, 0.5f }, NULL, 1, 1 },
    [SN_CONV] = { "conv", { "len_ms", "decay_ms", "bright", "amp", "mix" }, { 4, 1.5f, 0.5f, 1, 1 }, NULL, 1, 1 },
    [SN_SAMPLE] = { "sample", { "pitch", "amp", "start_ms", "jitter_ms" }, { 1, 1, 0, 0 }, "once|loop", 0, 0 },
};

const SynNodeInfo *syn_node_info(int type) { return type >= 0 && type < SN_COUNT ? &k_info[type] : NULL; }

int syn_node_type(const char *name) {
    for (int i = 0; i < SN_COUNT; i++) if (!strcmp(k_info[i].name, name)) return i;
    return -1;
}

int syn_param_slot(int type, const char *name) {
    const SynNodeInfo *in = syn_node_info(type);
    for (int i = 0; in && i < SYN_NODE_PARAMS; i++) if (in->param[i] && !strcmp(in->param[i], name)) return i;
    return -1;
}

int syn_node_opt(int type, const char *name) {
    const SynNodeInfo *in = syn_node_info(type);
    if (!in || !in->opts) return -1;
    const char *s = in->opts;
    size_t n = strlen(name);
    for (int idx = 0; *s; idx++) {
        const char *e = strchr(s, '|');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len == n && !strncmp(s, name, n)) return idx;
        if (!e) break;
        s = e + 1;
    }
    return -1;
}

static const SynSampleBank *g_bank;
void syn_set_sample_bank(const SynSampleBank *bank) { g_bank = bank; }

// Peterson & Barney style male formants (Hz), a e i o u schwa.
const float syn_vowel_f[SYN_VOWELS][SYN_FORMANTS] = {
    { 800, 1150, 2900, 3900, 4950 }, { 400, 1600, 2700, 3300, 4950 }, { 350, 1700, 2700, 3700, 4950 },
    { 450, 800, 2830, 3500, 4950 }, { 325, 700, 2530, 3500, 4950 }, { 500, 1500, 2500, 3500, 4500 },
};
static const float k_vowel_bw[SYN_FORMANTS] = { 80, 90, 120, 130, 140 };
static const float k_vowel_g[SYN_FORMANTS] = { 1.0f, 0.5f, 0.18f, 0.12f, 0.05f };

// ------------------------------------------------------------------ node state

#define GRAINS_MAX 16
#define MODES_MAX SYN_MAX_ARR
#define CONV_TAPS 256

typedef struct StOsc { double ph; } StOsc;
typedef struct StNoise { SynNoise n; } StNoise;
typedef struct StShot { u32 t; int fired; bool done; } StShot;   // impulse / friedlander / nwave
typedef struct StPulses { float ph, tail, lp; int cyl; float period_mul; float cylv[12]; SynRng r; } StPulses;
typedef struct StGlottal { float ph, prev, amp_mul, hz_mul, fry_ph; int period; SynRng r; } StGlottal;
typedef struct Grain { int left, len; float ph, inc, amp, s1, s2, b0, a1, a2; double spos; } Grain;
typedef struct StGrains { SynRng r; float t; int nact; Grain g[GRAINS_MAX]; } StGrains;   // g[0..nact) live
typedef struct StEnv { int stage; float level, from; u32 t; bool done, hold; } StEnv;
typedef struct StBiquad { SynBiquad f; float hz, q, gain; bool init; } StBiquad;
typedef struct StSvf { SynSvf f; float hz, q; bool init; } StSvf;
typedef struct StOnePole { float z; } StOnePole;
typedef struct StLine { int len, pos; float lp, ms; float buf[]; } StLine;   // comb / allpass / delay / karplus / waveguide
typedef struct StAm { float ph, a, b, period; SynRng r; } StAm;   // noise: smooth random between a and b
typedef struct StModal { int n; bool init; float pitch, damp, bright, spread; float a1[MODES_MAX], a2[MODES_MAX], b[MODES_MAX], y1[MODES_MAX], y2[MODES_MAX]; float detune[MODES_MAX]; } StModal;
typedef struct StFormant { SynBiquad f[SYN_FORMANTS]; float vowel, shift, bw; bool init; } StFormant;
typedef struct StConv { int n, pos; bool init; float ir[CONV_TAPS], hist[CONV_TAPS]; } StConv;
typedef struct StSample { double pos; bool done, init; } StSample;

static u32 line_floats(const SynNodeDef *d, float sr) {
    float ms = d->max_ms;
    if (ms <= 0) ms = d->type == SN_DELAY ? 100.0f : d->type == SN_KARPLUS ? 50.0f : 30.0f;
    return (u32)(ms * 0.001f * sr) + 4;
}

static u32 state_size(const SynNodeDef *d, float sr) {
    switch (d->type) {
    case SN_OSC: return sizeof(StOsc);
    case SN_NOISE: return sizeof(StNoise);
    case SN_IMPULSE: case SN_FRIEDLANDER: case SN_NWAVE: return sizeof(StShot);
    case SN_PULSES: return sizeof(StPulses);
    case SN_GLOTTAL: return sizeof(StGlottal);
    case SN_GRAINS: return sizeof(StGrains);
    case SN_ENV: return sizeof(StEnv);
    case SN_BIQUAD: return sizeof(StBiquad);
    case SN_SVF: return sizeof(StSvf);
    case SN_ONEPOLE: case SN_TILT: return sizeof(StOnePole);
    case SN_COMB: case SN_ALLPASS: case SN_DELAY: case SN_KARPLUS: case SN_WAVEGUIDE: return (u32)sizeof(StLine) + 4u * line_floats(d, sr);
    case SN_AM: return sizeof(StAm);
    case SN_MODAL: return sizeof(StModal);
    case SN_FORMANT: return sizeof(StFormant);
    case SN_CONV: return sizeof(StConv);
    case SN_SAMPLE: return sizeof(StSample);
    default: return 0;
    }
}

static bool is_terminator(const SynNodeDef *d) {
    return d->type == SN_ENV || d->type == SN_IMPULSE || d->type == SN_FRIEDLANDER || d->type == SN_NWAVE ||
           (d->type == SN_SAMPLE && d->opt == 0);
}

// ------------------------------------------------------------------ compile

int syn_patch_param(const SynPatch *p, const char *name) {
    for (int i = 0; i < p->nparams; i++) if (!strcmp(p->param_name[i], name)) return i;
    return -1;
}

int syn_patch_node(const SynPatch *p, const char *id) {
    for (int i = 0; i < p->nnodes; i++) if (!strcmp(p->nodes[i].id, id)) return i;
    return -1;
}

bool syn_patch_compile(SynPatch *p, float sr, char *err, int err_size) {
    p->compiled = false;
    p->sr = sr;
    if (p->nnodes <= 0 || p->nnodes > SYN_MAX_NODES) { snprintf(err, err_size, "%s: %d nodes", p->name, p->nnodes); return false; }
    if (p->param_tau <= 0) p->param_tau = 0.015f;
    // Dependencies: audio inputs and modulation sources.
    int indeg[SYN_MAX_NODES] = { 0 };
    u32 deps[SYN_MAX_NODES] = { 0 };   // bit j: node i depends on node j
    for (int i = 0; i < p->nnodes; i++) {
        SynNodeDef *d = &p->nodes[i];
        const SynNodeInfo *info = syn_node_info(d->type);
        if (!info) { snprintf(err, err_size, "%s: node %d has no type", p->name, i); return false; }
        if (d->nin < info->min_in || d->nin > info->max_in) {
            snprintf(err, err_size, "%s: node '%s' (%s) takes %d..%d inputs, has %d", p->name, d->id, info->name, info->min_in, info->max_in, d->nin);
            return false;
        }
        for (int k = 0; k < d->nin; k++) {
            if (d->in[k] < 0 || d->in[k] >= p->nnodes) { snprintf(err, err_size, "%s: node '%s' input %d dangles", p->name, d->id, k); return false; }
            deps[i] |= 1u << d->in[k];
        }
        for (int k = 0; k < SYN_NODE_PARAMS; k++) {
            if (d->p[k].kind == SYN_SRC_MOD) {
                if (d->p[k].index >= p->nnodes) { snprintf(err, err_size, "%s: node '%s' modulation dangles", p->name, d->id); return false; }
                deps[i] |= 1u << d->p[k].index;
            } else if (d->p[k].kind == SYN_SRC_PARAM && d->p[k].index >= p->nparams) {
                snprintf(err, err_size, "%s: node '%s' reads parameter %d of %d", p->name, d->id, d->p[k].index, p->nparams);
                return false;
            }
        }
        if (deps[i] & (1u << i)) { snprintf(err, err_size, "%s: node '%s' feeds itself (feedback lives inside comb / waveguide nodes)", p->name, d->id); return false; }
    }
    for (int i = 0; i < p->nnodes; i++)
        for (int j = 0; j < p->nnodes; j++) if (deps[i] & (1u << j)) indeg[i]++;
    // Kahn: repeatedly emit nodes whose dependencies are all emitted (lowest index first: stable).
    int n = 0;
    bool emitted[SYN_MAX_NODES] = { false };
    while (n < p->nnodes) {
        int pick = -1;
        for (int i = 0; i < p->nnodes && pick < 0; i++) if (!emitted[i] && indeg[i] == 0) pick = i;
        if (pick < 0) {
            int c = 0;
            for (int i = 0; i < p->nnodes; i++) if (!emitted[i]) { c = i; break; }
            snprintf(err, err_size, "%s: cycle through node '%s'", p->name, p->nodes[c].id);
            return false;
        }
        emitted[pick] = true;
        p->order[n++] = (u8)pick;
        for (int i = 0; i < p->nnodes; i++) if (deps[i] & (1u << pick)) indeg[i]--;
    }
    if (p->out < 0 || p->out >= p->nnodes) p->out = p->order[p->nnodes - 1];
    u32 off = 0;
    for (int i = 0; i < p->nnodes; i++) {
        SynNodeDef *d = &p->nodes[i];
        d->state_off = off;
        d->state_size = state_size(d, sr);
        off += (d->state_size + 15u) & ~15u;
    }
    if (off > SYN_VOICE_ARENA) { snprintf(err, err_size, "%s: %u bytes of state (voice arena %d): shorten max_ms", p->name, off, SYN_VOICE_ARENA); return false; }
    p->state_bytes = off;
    p->compiled = true;
    return true;
}

// ------------------------------------------------------------------ voice

void syn_voice_init(SynVoice *v, const SynPatch *p, void *arena, const float *params, u32 seed) {
    memset(v, 0, sizeof(*v));
    v->patch = p;
    v->arena = (u8 *)arena;
    v->seed = seed;
    memset(arena, 0, p->state_bytes);
    for (int i = 0; i < p->nparams; i++) syn_smooth_init(&v->param[i], params ? params[i] : p->param_def[i], p->param_tau, p->sr);
    for (int i = 0; i < p->nnodes; i++) {
        const SynNodeDef *d = &p->nodes[i];
        void *st = v->arena + d->state_off;
        u32 s = seed * 2654435761u + (u32)i * 0x9E3779B1u;
        switch (d->type) {
        case SN_NOISE: ((StNoise *)st)->n.rng = syn_rng(s); break;
        case SN_PULSES: ((StPulses *)st)->r = syn_rng(s); ((StPulses *)st)->period_mul = 1.0f; ((StPulses *)st)->ph = 0.99999f; break;   // the first pulse fires at once
        case SN_GLOTTAL: ((StGlottal *)st)->r = syn_rng(s); ((StGlottal *)st)->amp_mul = ((StGlottal *)st)->hz_mul = 1.0f; break;
        case SN_GRAINS: ((StGrains *)st)->r = syn_rng(s); break;
        case SN_AM: {
            StAm *a = (StAm *)st;
            a->r = syn_rng(s);
            a->a = syn_rngbi(&a->r);
            a->b = syn_rngbi(&a->r);
            a->period = 1.0f;
        } break;
        case SN_COMB: case SN_ALLPASS: case SN_DELAY: case SN_KARPLUS: case SN_WAVEGUIDE: ((StLine *)st)->len = (int)line_floats(d, p->sr); break;
        case SN_MODAL: {
            SynRng r = syn_rng(s);
            for (int k = 0; k < MODES_MAX; k++) ((StModal *)st)->detune[k] = syn_rngbi(&r);
        } break;
        case SN_OSC: ((StOsc *)st)->ph = 0; break;
        default: break;
        }
    }
}

void syn_voice_set_param(SynVoice *v, int index, float value) {
    if (index >= 0 && index < v->patch->nparams) v->param[index].target = value;
}

void syn_voice_snap_param(SynVoice *v, int index, float value) {
    if (index >= 0 && index < v->patch->nparams) v->param[index].cur = v->param[index].target = value;
}

typedef struct Ctx { float sr; u32 seed; u64 t; } Ctx;

DG_INLINE float fast_tanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

DG_INLINE float env_shape(float x, float k) {
    if (fabsf(k) < 1e-3f) return x;
    return (1.0f - expf(-k * x)) / (1.0f - expf(-k));
}

// Every process function: in = audio inputs, a / b = parameter values at block start / end.
// Returns true while the node is still producing (terminators: envelopes, one-shots).
static bool proc_node(const SynNodeDef *d, void *st, const float *const *in, float *out, int n, const float *a, const float *b, const Ctx *c) {
    const float sr = c->sr;
    switch (d->type) {
    case SN_OSC: {
        StOsc *s = (StOsc *)st;
        float ph = (float)s->ph;
        float hz0 = a[0], dhz = (b[0] - a[0]) / (float)n, fm = b[2], pw = syn_clampf(b[3], 0.02f, 0.98f);
        float step = (b[1] - a[1]) / (float)n, inv_sr = 1.0f / sr;
        const float *fmi = d->nin ? in[0] : NULL;
        // Phase increments first (frequency ramp + FM), then the waveform in its own loop.
        float dts[SYN_BLOCK];
        for (int i = 0; i < n; i++) {
            float hz = hz0 + dhz * (float)i + (fmi ? fm * fmi[i] : 0.0f);
            float dt = hz * inv_sr;
            dts[i] = dt < 0.0f ? 0.0f : (dt > 0.49f ? 0.49f : dt);
        }
        switch (d->opt) {
        case SYN_SAW:
            for (int i = 0; i < n; i++) {
                out[i] = 2.0f * ph - 1.0f - syn_polyblep(ph, dts[i]);
                ph += dts[i];
                if (ph >= 1.0f) ph -= 1.0f;
            }
            break;
        case SYN_SQUARE:
            for (int i = 0; i < n; i++) {
                float t2 = ph + (1.0f - pw);
                if (t2 >= 1.0f) t2 -= 1.0f;
                out[i] = (ph < pw ? 1.0f : -1.0f) + syn_polyblep(ph, dts[i]) - syn_polyblep(t2, dts[i]);
                ph += dts[i];
                if (ph >= 1.0f) ph -= 1.0f;
            }
            break;
        case SYN_TRI:
            for (int i = 0; i < n; i++) {
                out[i] = 1.0f - 4.0f * fabsf(ph - 0.5f);
                ph += dts[i];
                if (ph >= 1.0f) ph -= 1.0f;
            }
            break;
        default: {
            float off = b[4] - floorf(b[4]);
            for (int i = 0; i < n; i++) {
                float t = ph + off;
                out[i] = syn_fast_sin(t >= 1.0f ? t - 1.0f : t);
                ph += dts[i];
                if (ph >= 1.0f) ph -= 1.0f;
            }
        } break;
        }
        for (int i = 0; i < n; i++) out[i] *= a[1] + step * (float)(i + 1);
        s->ph = ph;
        return true;
    }
    case SN_NOISE:
        syn_noise_run(&((StNoise *)st)->n, (SynNoiseColor)d->opt, out, n, b[0], b[1], sr);
        return true;
    case SN_IMPULSE: {
        StShot *s = (StShot *)st;
        int count = (int)(b[4] + 0.5f);
        if (count < 1) count = 1;
        float w = b[1] * 0.001f * sr;
        u32 delay = (u32)(b[2] * 0.001f * sr), rep = (u32)(b[3] * 0.001f * sr);
        for (int i = 0; i < n; i++) {
            out[i] = 0.0f;
            u32 t = s->t + (u32)i;
            if (t < delay || s->done) continue;
            u32 local = t - delay;
            int k = rep ? (int)(local / rep) : 0;
            if (k >= count) { s->done = true; continue; }
            u32 within = rep ? local % rep : local;
            if (w < 1.0f) {
                if (within == 0) out[i] = b[0];
            } else if ((float)within < w) {
                // Unit area (amp = the area of a one-sample impulse): a wider contact is darker, not louder.
                out[i] = b[0] * (1.0f / w) * (1.0f - cosf(SYN_TWO_PI * (float)within / w));
            }
            if (!rep && (float)within >= (w < 1.0f ? 1.0f : w)) s->done = true;
        }
        s->t += (u32)n;
        return !s->done;
    }
    case SN_FRIEDLANDER: {
        // Friedlander blast wave: p(t) = P (1 - t/T) exp(-b t / T); positive phase T, negative tail.
        StShot *s = (StShot *)st;
        float T = fmaxf(b[1], 0.05f) * 0.001f * sr, bb = b[2];
        u32 delay = (u32)(b[3] * 0.001f * sr);
        for (int i = 0; i < n; i++) {
            u32 t = s->t + (u32)i;
            if (t < delay) { out[i] = 0; continue; }
            float x = (float)(t - delay) / T;
            if (x > 8.0f) { out[i] = 0; s->done = true; continue; }
            float onset = x < 0.05f ? x / 0.05f : 1.0f;   // a few samples of rise: no infinite slope
            out[i] = b[0] * onset * (1.0f - x) * expf(-bb * x);
        }
        s->t += (u32)n;
        return !s->done;
    }
    case SN_NWAVE: {
        // N-wave: +P after a finite rise, a linear fall to -P over the duration, back to 0 (raised-cosine edges).
        StShot *s = (StShot *)st;
        float D = fmaxf(b[1], 0.02f) * 0.001f * sr, R = fmaxf(b[2] * 1e-6f * sr, 1.0f);
        u32 delay = (u32)(b[3] * 0.001f * sr);
        for (int i = 0; i < n; i++) {
            u32 t = s->t + (u32)i;
            if (t < delay || s->done) { out[i] = 0; continue; }
            float x = (float)(t - delay), y;
            if (x < R) y = 0.5f * (1.0f - cosf(SYN_PI * x / R));
            else if (x < R + D) y = 1.0f - 2.0f * (x - R) / D;
            else if (x < 2 * R + D) y = -0.5f * (1.0f + cosf(SYN_PI * (x - R - D) / R));
            else { y = 0; s->done = true; }
            out[i] = b[0] * y;
        }
        s->t += (u32)n;
        return !s->done;
    }
    case SN_PULSES: {
        // Firing pulse train (engines): each pulse kicks an exponential tail; the attack is a one-pole.
        StPulses *s = (StPulses *)st;
        int count = DG_CLAMP((int)(b[5] + 0.5f), 1, 12);
        if (s->cylv[0] == 0.0f)
            for (int k = 0; k < 12; k++) s->cylv[k] = 1.0f + b[6] * syn_rngbi(&s->r) + 1e-6f;
        float dec = expf(-1.0f / fmaxf(b[4] * 0.001f * sr, 1.0f));
        float att = syn_onepole_coef(1000.0f / fmaxf(b[3], 0.05f), sr);
        for (int i = 0; i < n; i++) {
            float hz = a[0] + (b[0] - a[0]) * (float)i / (float)n;
            s->ph += hz / sr / s->period_mul;
            if (s->ph >= 1.0f) {
                s->ph -= 1.0f;
                s->cyl = (s->cyl + 1) % count;
                s->tail += s->cylv[s->cyl] * (1.0f + 0.5f * b[2] * syn_rngbi(&s->r));
                s->period_mul = 1.0f + b[2] * syn_rngbi(&s->r);
            }
            s->tail *= dec;
            s->lp += att * (s->tail - s->lp);
            out[i] = b[1] * s->lp;
        }
        return true;
    }
    case SN_GLOTTAL: {
        // Rosenberg glottal pulse, output as the flow derivative (lip radiation), with per-period
        // jitter (f0) / shimmer (amplitude), vocal fry (alternating periods + slow AM) and breath noise.
        StGlottal *s = (StGlottal *)st;
        float oq = syn_clampf(b[2], 0.2f, 0.95f), rise = 0.6f * oq;
        for (int i = 0; i < n; i++) {
            // An input bends the pitch: f0 x 2^(in) (in octaves; a call's contour).
            float bend = d->nin ? exp2f(syn_clampf(in[0][i], -3.0f, 3.0f)) : 1.0f;
            float hz = (a[0] + (b[0] - a[0]) * (float)i / (float)n) * s->hz_mul * bend;
            s->ph += hz / sr;
            if (s->ph >= 1.0f) {
                s->ph -= 1.0f;
                s->period++;
                s->hz_mul = 1.0f + b[3] * syn_rngbi(&s->r);
                s->amp_mul = (1.0f + b[4] * syn_rngbi(&s->r)) * ((s->period & 1) ? 1.0f - 0.6f * b[5] : 1.0f);
            }
            float t = s->ph, g;
            if (t < rise) g = 0.5f * (1.0f - cosf(SYN_PI * t / rise));
            else if (t < oq) g = cosf(0.5f * SYN_PI * (t - rise) / (oq - rise));
            else g = 0.0f;
            float dg = (g - s->prev) * sr / (hz * 4.0f + 1.0f);
            s->prev = g;
            s->fry_ph += b[6] / sr;
            if (s->fry_ph >= 1.0f) s->fry_ph -= 1.0f;
            float fry = 1.0f - b[5] * 0.5f * (1.0f + syn_fast_sin(s->fry_ph));
            float breath = b[7] * syn_rngbi(&s->r) * (0.3f + g);
            out[i] = b[1] * (dg * s->amp_mul * fry + breath);
        }
        return true;
    }
    case SN_GRAINS: {
        StGrains *s = (StGrains *)st;
        float rate = b[0] * (b[5] > 0 ? expf(-b[5] * s->t) : 1.0f);
        float q = fmaxf(b[6], 0.3f);
        memset(out, 0, sizeof(float) * (size_t)n);
        const float *sdata = NULL;
        int sframes = 0;
        if (d->opt == 3 && g_bank && d->sample >= 0 && d->sample < g_bank->count) { sdata = g_bank->data[d->sample]; sframes = g_bank->frames[d->sample]; }
        for (int i = 0; i < n; i++) {
            // Poisson arrivals: rate / sr per sample.
            if (syn_rng01(&s->r) < rate / sr) {
                Grain *g = s->nact < GRAINS_MAX ? &s->g[s->nact++] : NULL;
                if (g) {
                float hz = fmaxf(b[2] * (1.0f + b[3] * syn_rngbi(&s->r)), 40.0f);
                g->len = g->left = (int)(fmaxf(b[4], 0.2f) * 0.001f * sr * (0.5f + syn_rng01(&s->r)));
                g->amp = b[1] * (0.3f + 0.7f * syn_rng01(&s->r)) * (syn_rng_u32(&s->r) & 1u ? 1.0f : -1.0f);
                g->ph = 0;
                g->inc = hz / sr;
                g->s1 = g->s2 = 0;
                float w = SYN_TWO_PI * fminf(hz, sr * 0.45f) / sr, r = expf(-w / (2.0f * q));
                g->a1 = 2.0f * r * cosf(w);
                g->a2 = -r * r;
                g->b0 = (1.0f - r) * 2.0f;
                g->spos = sframes > 0 ? (double)(syn_rng01(&s->r) * (float)sframes) : 0.0;
                if (d->opt == 3) g->inc = hz / 1000.0f;   // sample grains: hz 1000 = the recorded pitch
                }
            }
            float acc = 0.0f;
            for (int k = 0; k < s->nact; k++) {
                Grain *g = &s->g[k];
                float age = (float)(g->len - g->left) / (float)g->len;
                float env = expf(-5.0f * age) * (age < 0.02f ? age / 0.02f : 1.0f);
                float x;
                switch (d->opt) {
                case 1: x = syn_fast_sin(g->ph); g->ph += g->inc; break;
                case 2: x = g->left == g->len ? 4.0f : 0.0f; break;
                case 3: x = sframes > 0 ? sdata[(int)g->spos % sframes] : 0.0f; g->spos += g->inc; env = 0.5f * (1.0f - cosf(SYN_TWO_PI * age)); break;
                default: x = syn_rngbi(&s->r); break;
                }
                if (d->opt != 1 && d->opt != 3) {
                    float y = g->b0 * x + g->a1 * g->s1 + g->a2 * g->s2;
                    g->s2 = g->s1;
                    g->s1 = y;
                    x = y;
                }
                acc += x * env * g->amp;
                if (--g->left <= 0) s->g[k--] = s->g[--s->nact];   // ended: the last live grain takes its place
            }
            out[i] = acc;
        }
        s->t += (float)n / sr;
        return true;
    }
    case SN_ENV: {
        StEnv *s = (StEnv *)st;
        int ns = d->narr;
        float ts = fmaxf(b[1], 0.01f);
        bool gate = b[2] > 0.5f;
        for (int i = 0; i < n; i++) {
            if (s->hold && !gate) s->hold = false;   // gate off: on to the release stages from the held level
            if (!s->done && !s->hold) {
                if (d->sustain >= 0 && s->stage <= d->sustain && !gate) {
                    // released before reaching the sustain: jump to the release from the current level
                    s->stage = d->sustain + 1;
                    s->from = s->level;
                    s->t = 0;
                }
                if (s->stage >= ns) {
                    s->done = true;
                } else {
                    float dur = fmaxf(d->arr[0][s->stage] * 0.001f * ts * sr, 1.0f);
                    float x = (float)s->t / dur;
                    if (x >= 1.0f) {
                        s->level = d->arr[1][s->stage];
                        s->from = s->level;
                        s->hold = s->stage == d->sustain && gate;
                        s->stage++;
                        s->t = 0;
                    } else {
                        s->level = s->from + (d->arr[1][s->stage] - s->from) * env_shape(x, d->arr[2][s->stage]);
                        s->t++;
                    }
                }
            }
            float e = s->level * (a[0] + (b[0] - a[0]) * (float)(i + 1) / (float)n);
            out[i] = d->nin ? in[0][i] * e : e;
        }
        return !s->done;
    }
    case SN_BIQUAD: {
        StBiquad *s = (StBiquad *)st;
        if (!s->init || fabsf(b[0] - s->hz) > s->hz * 5e-4f || b[1] != s->q || b[2] != s->gain) {
            SynBiquad keep = s->f;
            syn_biquad_design(&s->f, (SynFilterMode)d->opt, b[0], b[1], b[2], sr);
            s->f.z1 = s->init ? keep.z1 : 0;
            s->f.z2 = s->init ? keep.z2 : 0;
            s->hz = b[0], s->q = b[1], s->gain = b[2], s->init = true;
        }
        syn_biquad_run(&s->f, in[0], out, n);
        return true;
    }
    case SN_SVF: {
        StSvf *s = (StSvf *)st;
        if (!s->init || fabsf(b[0] - s->hz) > s->hz * 5e-4f || b[1] != s->q) {
            float ic1 = s->f.ic1, ic2 = s->f.ic2;
            syn_svf_design(&s->f, b[0], b[1], sr);
            s->f.ic1 = ic1, s->f.ic2 = ic2;
            s->hz = b[0], s->q = b[1], s->init = true;
        }
        static const SynFilterMode k_m[4] = { SYN_LP, SYN_HP, SYN_BP, SYN_NOTCH };
        syn_svf_run(&s->f, k_m[d->opt & 3], in[0], out, n);
        return true;
    }
    case SN_ONEPOLE: {
        StOnePole *s = (StOnePole *)st;
        float k = syn_onepole_coef(b[0], sr), z = s->z;
        for (int i = 0; i < n; i++) {
            z += k * (in[0][i] - z);
            out[i] = d->opt == 1 ? in[0][i] - z : z;
        }
        s->z = z;
        return true;
    }
    case SN_TILT: {
        StOnePole *s = (StOnePole *)st;
        float k = syn_onepole_coef(b[1], sr), z = s->z;
        float gl = syn_db(-b[0] * 2.5f), gh = syn_db(b[0] * 2.5f);
        for (int i = 0; i < n; i++) {
            z += k * (in[0][i] - z);
            out[i] = gl * z + gh * (in[0][i] - z);
        }
        s->z = z;
        return true;
    }
    case SN_COMB: case SN_ALLPASS: case SN_DELAY: {
        StLine *s = (StLine *)st;
        float d0 = a[0] * 0.001f * sr, d1 = b[0] * 0.001f * sr;
        float lim = (float)(s->len - 3), dd = (d1 - d0) / (float)n;
        for (int i = 0; i < n; i++) {
            float dl = syn_clampf(d0 + dd * (float)i, 1.0f, lim);
            float rp = (float)s->pos - dl;
            if (rp < 0) rp += (float)s->len;
            int i0 = (int)rp;
            float fr = rp - (float)i0;
            int i1 = i0 + 1 >= s->len ? 0 : i0 + 1;
            float del = s->buf[i0] + fr * (s->buf[i1] - s->buf[i0]);
            float x = in[0][i], y, w;
            if (d->type == SN_COMB) {
                s->lp += (1.0f - syn_clampf(b[2], 0, 0.99f)) * (del - s->lp);
                w = x + syn_clampf(b[1], -0.995f, 0.995f) * s->lp;
                y = x * (1.0f - b[3]) + w * b[3];
            } else if (d->type == SN_ALLPASS) {
                float g = syn_clampf(b[1], -0.95f, 0.95f);
                y = -g * x + del;
                w = x + g * y;
            } else {
                w = x + syn_clampf(b[1], -0.98f, 0.98f) * del;
                y = x * (1.0f - b[2]) + del * b[2];
            }
            s->buf[s->pos] = w;
            if (++s->pos >= s->len) s->pos = 0;
            out[i] = y;
        }
        return true;
    }
    case SN_KARPLUS: {
        StLine *s = (StLine *)st;
        float L = syn_clampf(sr / fmaxf(b[0], 1.0f), 2.0f, (float)(s->len - 3));
        float g = powf(10.0f, -3.0f * L / (sr * fmaxf(b[1], 0.01f)));
        float br = syn_clampf(b[2], 0.0f, 1.0f);
        if (s->ms == 0.0f && !d->nin) {
            // No exciter: a seeded noise burst one period long.
            SynRng r = syn_rng(c->seed ^ 0x51u);
            for (int k = 0; k < (int)L; k++) s->buf[k] = syn_rngbi(&r);
            s->pos = (int)L;
        }
        s->ms = 1.0f;
        for (int i = 0; i < n; i++) {
            float rp = (float)s->pos - L;
            while (rp < 0) rp += (float)s->len;
            int i0 = (int)rp, i1 = i0 + 1 >= s->len ? 0 : i0 + 1, im = i0 - 1 < 0 ? s->len - 1 : i0 - 1;
            float fr = rp - (float)i0;
            float y = s->buf[i0] + fr * (s->buf[i1] - s->buf[i0]);
            float avg = 0.5f * (y + s->buf[im]);
            float v = g * (br * y + (1.0f - br) * avg) + (d->nin ? in[0][i] : 0.0f);
            s->buf[s->pos] = v;
            if (++s->pos >= s->len) s->pos = 0;
            out[i] = b[3] * v;
        }
        return true;
    }
    case SN_WAVEGUIDE: {
        // Tube (exhaust, barrel): round-trip delay 2 L / c, an end reflection with a loss low-pass.
        StLine *s = (StLine *)st;
        float L = syn_clampf(2.0f * fmaxf(b[0], 0.01f) / fmaxf(b[4], 1.0f) * sr, 2.0f, (float)(s->len - 3));
        float refl = syn_clampf(b[1], -0.995f, 0.995f), loss = 1.0f - syn_clampf(b[2], 0.0f, 0.98f);
        for (int i = 0; i < n; i++) {
            float rp = (float)s->pos - L;
            while (rp < 0) rp += (float)s->len;
            int i0 = (int)rp, i1 = i0 + 1 >= s->len ? 0 : i0 + 1;
            float fr = rp - (float)i0;
            float del = s->buf[i0] + fr * (s->buf[i1] - s->buf[i0]);
            s->lp += loss * (del - s->lp);
            float y = in[0][i] + refl * s->lp;
            s->buf[s->pos] = y;
            if (++s->pos >= s->len) s->pos = 0;
            out[i] = b[3] * y;
        }
        return true;
    }
    case SN_SHAPER: {
        float step = (b[2] - a[2]) / (float)n;
        float off = d->opt == 0 ? fast_tanh(b[3]) : 0.0f;
        for (int i = 0; i < n; i++) {
            float x = in[0][i], u = b[0] * x + b[3], y;
            switch (d->opt) {
            case 1: y = syn_fast_sin(u * 0.25f); break;
            case 2: y = syn_clampf(u, -1.0f, 1.0f); break;
            case 3: y = u > 0 ? fast_tanh(u) : 0.5f * fast_tanh(2.0f * u); break;
            default: y = fast_tanh(u) - off; break;
            }
            out[i] = (a[2] + step * (float)(i + 1)) * (b[1] * y + (1.0f - b[1]) * x);
        }
        return true;
    }
    case SN_RING:
        for (int i = 0; i < n; i++) out[i] = in[0][i] * in[1][i] * b[0];
        return true;
    case SN_AM: {
        StAm *s = (StAm *)st;
        for (int i = 0; i < n; i++) {
            float m;
            if (d->nin > 1) m = in[1][i];
            else if (d->opt == 1) {
                // Smooth random (cosine-interpolated value noise, jittered periods): gusts, swells,
                // a crowd's swell - never a periodic wobble.
                m = s->a + (s->b - s->a) * (0.5f - 0.5f * cosf(SYN_PI * s->ph));
                s->ph += b[0] / (sr * s->period);
                if (s->ph >= 1.0f) {
                    s->ph -= 1.0f;
                    s->a = s->b;
                    s->b = syn_rngbi(&s->r);
                    s->period = 0.5f + syn_rng01(&s->r);
                }
            } else {
                m = syn_fast_sin(s->ph);
                s->ph += b[0] / sr;
                if (s->ph >= 1.0f) s->ph -= 1.0f;
            }
            out[i] = in[0][i] * (1.0f - b[1] + b[1] * (0.5f + 0.5f * m)) * b[2];
        }
        return true;
    }
    case SN_GAIN:
        syn_ramp_mul(in[0], out, n, a[0] * syn_db(a[1]), b[0] * syn_db(b[1]));
        return true;
    case SN_MIX: {
        memset(out, 0, sizeof(float) * (size_t)n);
        for (int k = 0; k < d->nin; k++) {
            float g = k < d->narr ? d->arr[0][k] : 1.0f;
            syn_ramp_mac(in[k], out, n, a[0] * g, b[0] * g);
        }
        return true;
    }
    case SN_MODAL: {
        StModal *s = (StModal *)st;
        if (!s->init || fabsf(b[0] - s->pitch) > 1e-3f * s->pitch || fabsf(b[1] - s->damp) > 1e-3f || b[4] != s->spread || b[5] != s->bright) {
            const float *f = d->arr[0], *t60 = d->arr[1], *gg = d->arr[2];
            int nm = d->narr;
            if (b[3] >= 0.0f) {
                const SynMaterial *m = syn_material((int)(b[3] + 0.5f));
                f = m->f, t60 = m->t60, gg = m->g, nm = m->nmodes;
            }
            s->n = DG_MIN(nm, MODES_MAX);
            for (int k = 0; k < s->n; k++) {
                float hz = f[k] * b[0] * (1.0f + b[4] * 0.05f * s->detune[k]);
                float w = SYN_TWO_PI * hz / sr;
                if (w >= SYN_PI * 0.95f || hz <= 0) { s->b[k] = 0; s->a1[k] = s->a2[k] = 0; continue; }
                float r = expf(-6.9077553f / (fmaxf(t60[k] * b[1], 1e-3f) * sr));
                s->a1[k] = 2.0f * r * cosf(w);
                s->a2[k] = -r * r;
                s->b[k] = gg[k] * sinf(w) * powf(hz / 1000.0f, b[5]);
            }
            s->pitch = b[0], s->damp = b[1], s->spread = b[4], s->bright = b[5], s->init = true;
        }
        float amp = b[2];
        for (int i = 0; i < n; i++) {
            float x = in[0][i], acc = 0.0f;
            for (int k = 0; k < s->n; k++) {
                float y = s->a1[k] * s->y1[k] + s->a2[k] * s->y2[k] + s->b[k] * x;
                s->y2[k] = s->y1[k];
                s->y1[k] = y;
                acc += y;
            }
            out[i] = amp * acc;
        }
        return true;
    }
    case SN_FORMANT: {
        StFormant *s = (StFormant *)st;
        if (!s->init || fabsf(b[0] - s->vowel) > 2e-3f || fabsf(b[1] - s->shift) > 2e-3f * s->shift || b[3] != s->bw) {
            float v = syn_clampf(b[0], 0.0f, (float)(SYN_VOWELS - 1));
            int i0 = (int)v, i1 = i0 + 1 < SYN_VOWELS ? i0 + 1 : i0;
            float fr = v - (float)i0;
            for (int k = 0; k < SYN_FORMANTS; k++) {
                float hz = (syn_vowel_f[i0][k] + fr * (syn_vowel_f[i1][k] - syn_vowel_f[i0][k])) * b[1];
                float q = hz / (k_vowel_bw[k] * fmaxf(b[3], 0.1f));
                float z1 = s->f[k].z1, z2 = s->f[k].z2;
                syn_biquad_design(&s->f[k], SYN_BP, hz, q, 0, sr);
                if (s->init) s->f[k].z1 = z1, s->f[k].z2 = z2;
            }
            s->vowel = b[0], s->shift = b[1], s->bw = b[3], s->init = true;
        }
        float tmp[SYN_BLOCK];
        memset(out, 0, sizeof(float) * (size_t)n);
        for (int k = 0; k < SYN_FORMANTS; k++) {
            syn_biquad_run(&s->f[k], in[0], tmp, n);
            float g = k_vowel_g[k] * b[2];
            for (int i = 0; i < n; i++) out[i] += g * tmp[i];
        }
        return true;
    }
    case SN_CONV: {
        // Short body / box IR: seeded exponentially decaying noise, low-passed by `bright`, unit energy.
        StConv *s = (StConv *)st;
        if (!s->init) {
            s->n = DG_CLAMP((int)(b[0] * 0.001f * sr), 1, CONV_TAPS);
            SynRng r = syn_rng(c->seed ^ 0xC0u);
            float lp = 0, k = syn_clampf(b[2], 0.02f, 1.0f), dec = 1.0f / fmaxf(b[1] * 0.001f * sr, 1.0f), e = 0;
            for (int t = 0; t < s->n; t++) {
                lp += k * (syn_rngbi(&r) - lp);
                s->ir[t] = lp * expf(-(float)t * dec);
                e += s->ir[t] * s->ir[t];
            }
            float norm = e > 0 ? 1.0f / sqrtf(e) : 0.0f;
            for (int t = 0; t < s->n; t++) s->ir[t] *= norm;
            s->init = true;
        }
        for (int i = 0; i < n; i++) {
            s->hist[s->pos] = in[0][i];
            float acc = 0.0f;
            int p = s->pos;
            for (int t = 0; t < s->n; t++) {
                acc += s->ir[t] * s->hist[p];
                if (--p < 0) p = CONV_TAPS - 1;
            }
            if (++s->pos >= CONV_TAPS) s->pos = 0;
            out[i] = in[0][i] * (1.0f - b[4]) + b[3] * b[4] * acc;
        }
        return true;
    }
    case SN_SAMPLE: {
        StSample *s = (StSample *)st;
        const float *data = NULL;
        int frames = 0;
        if (g_bank && d->sample >= 0 && d->sample < g_bank->count) { data = g_bank->data[d->sample]; frames = g_bank->frames[d->sample]; }
        if (!s->init) {
            SynRng r = syn_rng(c->seed ^ 0x5Au);
            s->pos = (double)((b[2] + b[3] * syn_rng01(&r)) * 0.001f * sr);
            s->init = true;
        }
        if (!data || frames < 2 || s->done) { memset(out, 0, sizeof(float) * (size_t)n); s->done = true; return false; }
        float step = (b[1] - a[1]) / (float)n;
        double inc = fmax((double)b[0], 0.01);
        for (int i = 0; i < n; i++) {
            if (s->pos >= frames - 1) {
                if (d->opt == 1) s->pos = fmod(s->pos, (double)(frames - 1));
                else { s->done = true; out[i] = 0; continue; }
            }
            int i0 = (int)s->pos;
            float fr = (float)(s->pos - i0);
            out[i] = (data[i0] + fr * (data[i0 + 1] - data[i0])) * (a[1] + step * (float)(i + 1));
            s->pos += inc;
        }
        return !s->done;
    }
    default:
        memset(out, 0, sizeof(float) * (size_t)n);
        return true;
    }
}

bool syn_voice_render(SynVoice *v, float *out, int n, float *scratch) {
    const SynPatch *p = v->patch;
    if (v->done || !p || !p->compiled) { memset(out, 0, sizeof(float) * (size_t)n); return false; }
    float pa[SYN_MAX_PARAMS], pb[SYN_MAX_PARAMS];
    for (int i = 0; i < p->nparams; i++) syn_smooth_block(&v->param[i], &pa[i], &pb[i]);
    Ctx c = { p->sr, v->seed, v->t };
    int terminators = 0, active = 0;
    for (int oi = 0; oi < p->nnodes; oi++) {
        int ni = p->order[oi];
        const SynNodeDef *d = &p->nodes[ni];
        float a[SYN_NODE_PARAMS], b[SYN_NODE_PARAMS];
        for (int k = 0; k < SYN_NODE_PARAMS; k++) {
            const SynParamSrc *s = &d->p[k];
            switch (s->kind) {
            case SYN_SRC_PARAM: a[k] = s->scale * pa[s->index] + s->offset; b[k] = s->scale * pb[s->index] + s->offset; break;
            case SYN_SRC_MOD: {
                const float *mb = scratch + (size_t)s->index * SYN_BLOCK;
                a[k] = s->scale * mb[0] + s->offset;
                b[k] = s->scale * mb[n - 1] + s->offset;
            } break;
            default: a[k] = b[k] = s->value; break;
            }
        }
        const float *ins[SYN_MAX_INPUTS];
        for (int k = 0; k < d->nin; k++) ins[k] = scratch + (size_t)d->in[k] * SYN_BLOCK;
        bool on = proc_node(d, v->arena + d->state_off, ins, scratch + (size_t)ni * SYN_BLOCK, n, a, b, &c);
        if (is_terminator(d)) { terminators++; active += on; }
    }
    const float *o = scratch + (size_t)p->out * SYN_BLOCK;
    float peak = 0.0f;
    for (int i = 0; i < n; i++) {
        float x = o[i];
        if (!(x == x) || x > 1e4f || x < -1e4f) x = 0.0f;   // NaN / blow-up guard: never reaches the mix
        out[i] = x;
        float ax = x < 0 ? -x : x;
        peak = ax > peak ? ax : peak;
    }
    v->t += (u64)n;
    if (p->loop) return true;
    float dur = (float)v->t / p->sr;
    if (p->max_dur > 0 && dur >= p->max_dur) v->done = true;
    if (!terminators && p->max_dur <= 0 && dur >= 4.0f) v->done = true;   // nothing ends it: 4 s safety
    if (terminators && !active) {
        v->quiet_blocks = peak < 3e-5f ? v->quiet_blocks + 1 : 0;
        if (v->quiet_blocks >= 8) v->done = true;
    }
    return !v->done;
}
