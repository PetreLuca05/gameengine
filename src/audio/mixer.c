#include "mixer.h"
#include "mix_atomic.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CMD_START, CMD_PARAMS, CMD_ENV, CMD_VOLUME, CMD_STOP, CMD_VIRTUAL, CMD_BUS, CMD_MASTER, CMD_LISTENER, CMD_REVERB, CMD_DEGRADE };

typedef struct MixCmd {
    u8 type, slot, layer, count;
    u32 gen;
    int dop;
    union {
        MixStart start;
        float params[SYN_MAX_PARAMS];
        EnvParams env;
        float f;
        MixListener lis;
        EnvReverbParams rev;
    } u;
} MixCmd;

typedef struct ALayer { SynVoice syn; SynSmooth gain; bool optional, done; int gate; } ALayer;

typedef struct AVoice {
    bool active, env, virt, stopping;
    u32 gen;
    u8 bus;
    int nlayers, dop;
    ALayer layer[RCP_MAX_LAYERS];
    EnvChain chain;
    i64 wait;                    // frames before it starts
    int tail;                    // frames the chain still rings after the layers end (-1: not yet)
    SynSmooth vol;
    float fade, fade_step;
} AVoice;

typedef struct GSlot { bool used; u32 gen; int dop; float score; double started; } GSlot;

struct Mixer {
    float sr;
    double (*clock)(void);
    // ring
    MixCmd ring[MIX_CMDS];
    mix_atomic head, tail;
    // game side
    GSlot slot[MIX_VOICES];
    bool dop_used[MIX_DOPPLER_LINES];
    int dropped, stolen;
    // published by the audio thread
    mix_atomic done_gen[MIX_VOICES];
    volatile float peak[MIX_VOICES];
    volatile float stat_us, stat_avg_us, stat_buffer_us, stat_peak;
    volatile int stat_active, stat_rendered, stat_virtual, stat_layers, stat_degrade;
    // audio side
    AVoice voice[MIX_VOICES];
    u8 *arena;                   // MIX_VOICES * MIX_ARENA
    float *dop_mem;              // MIX_DOPPLER_LINES * MIX_DOPPLER_LEN
    EnvReverb reverb[ENV_ZONES];
    SynSmooth bus_gain[SYN_BUS_COUNT], master;
    MixListener lis;
    SynSmooth lis_lp, lis_low, lis_high, lis_gain, death_lp, death_gain;
    SynBiquad f_lp[2], f_low[2], f_high[2], f_death[2];
    float f_applied[4];
    float scratch[SYN_MAX_NODES * SYN_BLOCK];
    int degrade, force_degrade;
    float avg_us;
};

static DG_THREAD_LOCAL bool t_in_render;
bool mixer_in_render(void) { return t_in_render; }

int mixer_handle_slot(MixHandle h) { return h ? (int)(h & 0xFFu) : -1; }
static u32 handle_gen(MixHandle h) { return h >> 8; }

Mixer *mixer_create(float sr, double (*clock)(void)) {
    Mixer *m = (Mixer *)calloc(1, sizeof(Mixer));
    if (!m) return NULL;
    m->sr = sr;
    m->clock = clock;
    m->arena = (u8 *)calloc(MIX_VOICES, MIX_ARENA);
    m->dop_mem = (float *)calloc((size_t)MIX_DOPPLER_LINES * MIX_DOPPLER_LEN, sizeof(float));
    if (!m->arena || !m->dop_mem) { mixer_destroy(m); return NULL; }
    for (int i = 0; i < MIX_VOICES; i++) m->done_gen[i] = -1;
    EnvReverbParams rp = { 1.0f, 0.6f, 1.0f, 10.0f, 0.0f };
    for (int z = 0; z < ENV_ZONES; z++) env_reverb_init(&m->reverb[z], sr, &rp);
    for (int b = 0; b < SYN_BUS_COUNT; b++) syn_smooth_init(&m->bus_gain[b], 1.0f, 0.05f, sr);
    syn_smooth_init(&m->master, 1.0f, 0.05f, sr);
    MixListener l = { 20000.0f, 0, 0, 1.0f, 20000.0f, 1.0f };
    m->lis = l;
    syn_smooth_init(&m->lis_lp, l.lp_hz, 0.08f, sr);
    syn_smooth_init(&m->lis_low, 0, 0.08f, sr);
    syn_smooth_init(&m->lis_high, 0, 0.08f, sr);
    syn_smooth_init(&m->lis_gain, 1, 0.05f, sr);
    syn_smooth_init(&m->death_lp, 20000.0f, 0.03f, sr);
    syn_smooth_init(&m->death_gain, 1, 0.05f, sr);
    for (int c = 0; c < 2; c++) {
        syn_biquad_design(&m->f_lp[c], SYN_LP, 20000.0f, 0.707f, 0, sr);
        syn_biquad_design(&m->f_low[c], SYN_LSHELF, 150.0f, 0.707f, 0, sr);
        syn_biquad_design(&m->f_high[c], SYN_HSHELF, 3500.0f, 0.707f, 0, sr);
        syn_biquad_design(&m->f_death[c], SYN_LP, 20000.0f, 0.707f, 0, sr);
    }
    m->f_applied[0] = m->f_applied[3] = 20000.0f;
    m->force_degrade = -1;
    return m;
}

void mixer_destroy(Mixer *m) {
    if (!m) return;
    free(m->arena);
    free(m->dop_mem);
    free(m);
}

float mixer_rate(const Mixer *m) { return m->sr; }

// ------------------------------------------------------------------ queue (game thread produces)

static bool push(Mixer *m, const MixCmd *c) {
    long h = mix_load(&m->head), next = (h + 1) % MIX_CMDS;
    if (next == mix_load(&m->tail)) { m->dropped++; return false; }
    m->ring[h] = *c;
    mix_store(&m->head, next);
    return true;
}

static bool live(const Mixer *m, MixHandle h, int *slot) {
    int s = mixer_handle_slot(h);
    if (s < 0 || s >= MIX_VOICES) return false;
    const GSlot *g = &m->slot[s];
    if (!g->used || g->gen != handle_gen(h)) return false;
    *slot = s;
    return true;
}

bool mixer_alive(const Mixer *m, MixHandle h) {
    int s;
    if (!live(m, h, &s)) return false;
    return mix_load(&m->done_gen[s]) != (long)m->slot[s].gen;
}

void mixer_update(Mixer *m) {
    for (int s = 0; s < MIX_VOICES; s++) {
        GSlot *g = &m->slot[s];
        if (g->used && mix_load(&m->done_gen[s]) == (long)g->gen) {
            g->used = false;
            if (g->dop >= 0) m->dop_used[g->dop] = false;
            g->dop = -1;
        }
    }
}

MixHandle mixer_start(Mixer *m, const MixStart *s) {
    mixer_update(m);
    int pick = -1;
    for (int i = 0; i < MIX_VOICES && pick < 0; i++) if (!m->slot[i].used) pick = i;
    if (pick < 0) {
        // Full: steal the least important voice, if it matters less than this one.
        float worst = s->score;
        for (int i = 0; i < MIX_VOICES; i++)
            if (m->slot[i].score < worst) { worst = m->slot[i].score; pick = i; }
        if (pick < 0) return 0;
        GSlot *g = &m->slot[pick];
        if (g->dop >= 0) m->dop_used[g->dop] = false;
        g->dop = -1;
        g->used = false;
        m->stolen++;
    }
    int dop = -1;
    if (s->doppler)
        for (int i = 0; i < MIX_DOPPLER_LINES && dop < 0; i++) if (!m->dop_used[i]) dop = i;
    MixCmd *c = (MixCmd *)malloc(sizeof(MixCmd));
    if (!c) return 0;
    GSlot *g = &m->slot[pick];
    u32 gen = (g->gen + 1) & 0xFFFFFFu;
    if (!gen) gen = 1;
    c->type = CMD_START;
    c->slot = (u8)pick;
    c->gen = gen;
    c->dop = dop;
    c->u.start = *s;
    // Layers must fit the voice arena: optional ones go first.
    u32 bytes = 0;
    int keep = 0;
    for (int i = 0; i < s->nlayers; i++) {
        const MixLayerStart *l = &s->layer[i];
        if (!l->patch || !l->patch->compiled) continue;
        u32 sz = (l->patch->state_bytes + 15u) & ~15u;
        if (bytes + sz > MIX_ARENA) continue;
        bytes += sz;
        c->u.start.layer[keep++] = *l;
    }
    c->u.start.nlayers = keep;
    bool ok = keep > 0 && push(m, c);
    free(c);
    if (!ok) return 0;
    g->used = true;
    g->gen = gen;
    g->dop = dop;
    g->score = s->score;
    g->started = m->clock ? m->clock() : 0.0;
    if (dop >= 0) m->dop_used[dop] = true;
    return (MixHandle)pick | (gen << 8);
}

static void voice_cmd(Mixer *m, MixHandle h, MixCmd *c) {
    int s;
    if (!live(m, h, &s)) return;
    c->slot = (u8)s;
    c->gen = m->slot[s].gen;
    push(m, c);
}

void mixer_params(Mixer *m, MixHandle h, int layer, const float *params, int count) {
    MixCmd c;
    c.type = CMD_PARAMS;
    c.layer = (u8)layer;
    c.count = (u8)DG_MIN(count, SYN_MAX_PARAMS);
    memcpy(c.u.params, params, sizeof(float) * c.count);
    voice_cmd(m, h, &c);
}

void mixer_env(Mixer *m, MixHandle h, const EnvParams *p) {
    MixCmd c;
    c.type = CMD_ENV;
    c.u.env = *p;
    voice_cmd(m, h, &c);
}

void mixer_volume(Mixer *m, MixHandle h, float v) {
    MixCmd c;
    c.type = CMD_VOLUME;
    c.u.f = v;
    voice_cmd(m, h, &c);
}

void mixer_score(Mixer *m, MixHandle h, float score) {
    int s;
    if (live(m, h, &s)) m->slot[s].score = score;
}

void mixer_stop(Mixer *m, MixHandle h, float fade_s) {
    MixCmd c;
    c.type = CMD_STOP;
    c.u.f = fade_s;
    voice_cmd(m, h, &c);
}

void mixer_virtual(Mixer *m, MixHandle h, bool on) {
    MixCmd c;
    c.type = CMD_VIRTUAL;
    c.u.f = on ? 1.0f : 0.0f;
    voice_cmd(m, h, &c);
}

void mixer_set_bus(Mixer *m, int bus, float gain) {
    MixCmd c;
    c.type = CMD_BUS;
    c.layer = (u8)bus;
    c.u.f = gain;
    push(m, &c);
}

void mixer_set_master(Mixer *m, float gain) {
    MixCmd c;
    c.type = CMD_MASTER;
    c.u.f = gain;
    push(m, &c);
}

void mixer_set_listener(Mixer *m, const MixListener *l) {
    MixCmd c;
    c.type = CMD_LISTENER;
    c.u.lis = *l;
    push(m, &c);
}

void mixer_set_reverb(Mixer *m, int zone, const EnvReverbParams *p) {
    if (zone < 0 || zone >= ENV_ZONES) return;
    MixCmd c;
    c.type = CMD_REVERB;
    c.layer = (u8)zone;
    c.u.rev = *p;
    push(m, &c);
}

void mixer_force_degrade(Mixer *m, int level) {
    MixCmd c;
    c.type = CMD_DEGRADE;
    c.u.f = (float)level;
    push(m, &c);
}

MixStats mixer_stats(const Mixer *m) {
    MixStats s;
    memset(&s, 0, sizeof(s));
    s.active = m->stat_active;
    s.rendered = m->stat_rendered;
    s.virtual_count = m->stat_virtual;
    s.layers = m->stat_layers;
    s.render_us = m->stat_us;
    s.render_avg_us = m->stat_avg_us;
    s.buffer_us = m->stat_buffer_us;
    s.degrade = m->stat_degrade;
    s.dropped_cmds = m->dropped;
    s.stolen = m->stolen;
    s.peak = m->stat_peak;
    return s;
}

float mixer_voice_peak(const Mixer *m, MixHandle h) {
    int s;
    return live(m, h, &s) ? m->peak[s] : 0.0f;
}

double mixer_voice_age(const Mixer *m, MixHandle h) {
    int s;
    if (!live(m, h, &s) || !m->clock) return 0.0;
    return m->clock() - m->slot[s].started;
}

// ------------------------------------------------------------------ audio thread

static void finish(Mixer *m, int s) {
    AVoice *v = &m->voice[s];
    v->active = false;
    m->peak[s] = 0.0f;
    mix_store(&m->done_gen[s], (long)v->gen);
}

static void start_voice(Mixer *m, const MixCmd *c) {
    int s = c->slot;
    AVoice *v = &m->voice[s];
    const MixStart *st = &c->u.start;
    v->active = true;
    v->gen = c->gen;
    v->bus = st->bus < SYN_BUS_COUNT ? st->bus : SYN_BUS_SFX;
    v->env = st->env;
    v->virt = false;
    v->stopping = false;
    v->tail = -1;
    v->dop = c->dop;
    v->wait = st->doppler && c->dop >= 0 ? 0 : (i64)(st->start_delay * m->sr);
    syn_smooth_init(&v->vol, st->volume, 0.02f, m->sr);
    u8 *arena = m->arena + (size_t)s * MIX_ARENA;
    v->nlayers = st->nlayers;
    for (int i = 0; i < st->nlayers; i++) {
        const MixLayerStart *l = &st->layer[i];
        ALayer *a = &v->layer[i];
        syn_voice_init(&a->syn, l->patch, arena, l->params, st->seed + (u32)i * 7919u);
        arena += (l->patch->state_bytes + 15u) & ~15u;
        syn_smooth_init(&a->gain, l->gain, 0.02f, m->sr);
        a->optional = l->optional;
        a->done = false;
        a->gate = syn_patch_param(l->patch, "gate");
    }
    float *dop = v->dop >= 0 ? m->dop_mem + (size_t)v->dop * MIX_DOPPLER_LEN : NULL;
    env_chain_init(&v->chain, m->sr, dop, dop ? MIX_DOPPLER_LEN : 0, &st->env_params);
    m->peak[s] = 0.0f;
}

static void drain(Mixer *m) {
    long t = mix_load(&m->tail), h = mix_load(&m->head);
    while (t != h) {
        const MixCmd *c = &m->ring[t];
        // Global commands (bus, master, listener, reverb, degrade) leave slot unset.
        AVoice *v = &m->voice[c->slot < MIX_VOICES ? c->slot : 0];
        bool mine = c->type <= CMD_VIRTUAL && c->slot < MIX_VOICES && v->active && v->gen == c->gen;
        switch (c->type) {
        case CMD_START: start_voice(m, c); break;
        case CMD_PARAMS:
            if (mine && c->layer < v->nlayers)
                for (int i = 0; i < c->count; i++) syn_voice_set_param(&v->layer[c->layer].syn, i, c->u.params[i]);
            break;
        case CMD_ENV: if (mine) env_chain_set(&v->chain, &c->u.env); break;
        case CMD_VOLUME: if (mine) v->vol.target = c->u.f; break;
        case CMD_STOP:
            if (mine && !v->stopping) {
                v->stopping = true;
                v->fade = 1.0f;
                v->fade_step = 1.0f / (fmaxf(c->u.f, 0.005f) * m->sr);
                for (int i = 0; i < v->nlayers; i++) if (v->layer[i].gate >= 0) syn_voice_set_param(&v->layer[i].syn, v->layer[i].gate, 0.0f);
                if (v->wait > 0) finish(m, c->slot);   // never started: nothing to fade
            }
            break;
        case CMD_VIRTUAL: if (mine) v->virt = c->u.f > 0.5f; break;
        case CMD_BUS: if (c->layer < SYN_BUS_COUNT) m->bus_gain[c->layer].target = c->u.f; break;
        case CMD_MASTER: m->master.target = c->u.f; break;
        case CMD_LISTENER:
            m->lis = c->u.lis;
            m->lis_lp.target = c->u.lis.lp_hz;
            m->lis_low.target = c->u.lis.low_db;
            m->lis_high.target = c->u.lis.high_db;
            m->lis_gain.target = c->u.lis.gain;
            m->death_lp.target = c->u.lis.death_lp_hz;
            m->death_gain.target = c->u.lis.death_gain;
            break;
        case CMD_REVERB: env_reverb_set(&m->reverb[c->layer], &c->u.rev); break;
        case CMD_DEGRADE: m->force_degrade = (int)c->u.f; break;
        }
        t = (t + 1) % MIX_CMDS;
    }
    mix_store(&m->tail, t);
}

typedef struct Bufs {
    float bus[SYN_BUS_COUNT][2][SYN_BLOCK];
    float send[ENV_ZONES][SYN_BLOCK];
} Bufs;

// Renders one voice for n frames into the bus buffers; returns true if it produced audio.
static bool render_voice(Mixer *m, int s, Bufs *b, int n, int *layers) {
    AVoice *v = &m->voice[s];
    if (v->wait >= n) { v->wait -= n; return false; }
    int off = (int)v->wait;
    v->wait = 0;
    int len = n - off;
    if (v->virt) {
        // Virtual: time passes, nothing renders (inaudible or over budget); the game decides when it ends.
        for (int i = 0; i < v->nlayers; i++) v->layer[i].syn.t += (u64)len;
        return false;
    }
    float mono[SYN_BLOCK], tmp[SYN_BLOCK];
    memset(mono, 0, sizeof(mono));
    bool alive = false;
    for (int i = 0; i < v->nlayers; i++) {
        ALayer *a = &v->layer[i];
        if (a->done) continue;
        float g0, g1;
        syn_smooth_block(&a->gain, &g0, &g1);
        if (a->optional && m->degrade >= 1) { alive = true; continue; }
        if (!syn_voice_render(&a->syn, tmp, len, m->scratch)) a->done = true;
        else alive = true;
        syn_ramp_mac(tmp, mono + off, len, g0, g1);
        (*layers)++;
    }
    float v0, v1;
    syn_smooth_block(&v->vol, &v0, &v1);
    if (v->stopping) {
        float f0 = v->fade, f1 = fmaxf(v->fade - v->fade_step * (float)len, 0.0f);
        v0 *= f0, v1 *= f1;
        v->fade = f1;
    }
    syn_ramp_mul(mono + off, mono + off, len, v0, v1);
    float peak = 0.0f;
    for (int i = off; i < n; i++) {
        float a = mono[i] < 0 ? -mono[i] : mono[i];
        peak = a > peak ? a : peak;
    }
    m->peak[s] = peak > m->peak[s] ? peak : m->peak[s] * 0.95f;
    float *l = b->bus[v->bus][0], *r = b->bus[v->bus][1];
    if (v->env) {
        v->chain.tap_limit = m->degrade >= 2 ? 2 : ENV_MAX_TAPS;
        float *sends[ENV_ZONES] = { b->send[0] + off, b->send[1] + off, b->send[2] + off };
        env_chain_process(&v->chain, mono + off, len, l + off, r + off, sends);
    } else {
        for (int i = off; i < n; i++) { l[i] += mono[i]; r[i] += mono[i]; }
    }
    if (v->stopping && v->fade <= 0.0f) { finish(m, s); return true; }
    if (!alive) {
        // The layers ended: let the chain ring out (doppler line, early reflections), then free.
        if (v->tail < 0) {
            float ring_s = 0.0f;
            for (int t = 0; t < v->chain.ntaps; t++) ring_s = fmaxf(ring_s, v->chain.tap[t].delay.cur / m->sr);
            if (v->chain.dop) ring_s = fmaxf(ring_s, v->chain.delay.cur / m->sr);
            v->tail = v->env ? (int)((ring_s + 0.02f) * m->sr) : 0;
        }
        v->tail -= len;
        if (v->tail <= 0) finish(m, s);
    }
    return true;
}

static void listener_filters(Mixer *m) {
    float a, lp, low, high, dlp;
    syn_smooth_block(&m->lis_lp, &a, &lp);
    syn_smooth_block(&m->lis_low, &a, &low);
    syn_smooth_block(&m->lis_high, &a, &high);
    syn_smooth_block(&m->death_lp, &a, &dlp);
    const float want[4] = { lp, low, high, dlp };
    for (int k = 0; k < 4; k++) {
        float d = fabsf(want[k] - m->f_applied[k]);
        bool change = k == 0 || k == 3 ? d > m->f_applied[k] * 0.002f : d > 0.02f;
        if (!change) continue;
        for (int c = 0; c < 2; c++) {
            SynBiquad *f = k == 0 ? &m->f_lp[c] : k == 1 ? &m->f_low[c] : k == 2 ? &m->f_high[c] : &m->f_death[c];
            float z1 = f->z1, z2 = f->z2;
            if (k == 0) syn_biquad_design(f, SYN_LP, fminf(lp, 20000.0f), 0.707f, 0, m->sr);
            else if (k == 1) syn_biquad_design(f, SYN_LSHELF, 150.0f, 0.707f, low, m->sr);
            else if (k == 2) syn_biquad_design(f, SYN_HSHELF, 3500.0f, 0.707f, high, m->sr);
            else syn_biquad_design(f, SYN_LP, fminf(dlp, 20000.0f), 0.707f, 0, m->sr);
            f->z1 = z1, f->z2 = z2;
        }
        m->f_applied[k] = want[k];
    }
}

DG_INLINE float soft_clip(float x) {
    float ax = fabsf(x);
    if (ax <= 0.8f) return x;
    float y = 0.8f + 0.2f * tanhf((ax - 0.8f) / 0.2f);
    return x < 0 ? -y : y;
}

void mixer_render(Mixer *m, float *out, int frames) {
    t_in_render = true;
    u32 ftz = syn_ftz_enable();
    double t0 = m->clock ? m->clock() : 0.0;
    drain(m);
    int rendered = 0, layers = 0, active = 0, virt = 0;
    float peak = 0.0f;
    Bufs b;
    for (int off = 0; off < frames; off += SYN_BLOCK) {
        int n = frames - off < SYN_BLOCK ? frames - off : SYN_BLOCK;
        memset(&b, 0, sizeof(b));
        for (int s = 0; s < MIX_VOICES; s++) {
            if (!m->voice[s].active) continue;
            if (render_voice(m, s, &b, n, &layers) && off == 0) rendered++;
        }
        // Late reverbs into the effects bus.
        for (int z = 0; z < ENV_ZONES; z++) env_reverb_process(&m->reverb[z], b.send[z], n, b.bus[SYN_BUS_SFX][0], b.bus[SYN_BUS_SFX][1]);
        listener_filters(m);
        float bg0[SYN_BUS_COUNT], bg1[SYN_BUS_COUNT], m0, m1, lg0, lg1, dg0, dg1;
        for (int k = 0; k < SYN_BUS_COUNT; k++) syn_smooth_block(&m->bus_gain[k], &bg0[k], &bg1[k]);
        syn_smooth_block(&m->master, &m0, &m1);
        syn_smooth_block(&m->lis_gain, &lg0, &lg1);
        syn_smooth_block(&m->death_gain, &dg0, &dg1);
        float world[2][SYN_BLOCK], ui[2][SYN_BLOCK];
        for (int c = 0; c < 2; c++) {
            memset(world[c], 0, sizeof(world[c]));
            memset(ui[c], 0, sizeof(ui[c]));
            for (int k = 0; k < SYN_BUS_COUNT; k++) {
                if (k == SYN_BUS_UI) syn_ramp_mac(b.bus[k][c], ui[c], n, bg0[k], bg1[k]);
                else syn_ramp_mac(b.bus[k][c], world[c], n, bg0[k], bg1[k]);
            }
            // World buses: listener state, then the death / downed snapshot (the UI stays clean).
            syn_biquad_run(&m->f_low[c], world[c], world[c], n);
            syn_biquad_run(&m->f_high[c], world[c], world[c], n);
            syn_biquad_run(&m->f_lp[c], world[c], world[c], n);
            syn_biquad_run(&m->f_death[c], world[c], world[c], n);
        }
        float inv = 1.0f / (float)n;
        for (int i = 0; i < n; i++) {
            float t = (float)(i + 1) * inv;
            float g = (lg0 + (lg1 - lg0) * t) * (dg0 + (dg1 - dg0) * t);
            float mg = m0 + (m1 - m0) * t;
            for (int c = 0; c < 2; c++) {
                float x = soft_clip(mg * (world[c][i] * g + ui[c][i]));
                out[(off + i) * 2 + c] = x;
                float ax = x < 0 ? -x : x;
                peak = ax > peak ? ax : peak;
            }
        }
    }
    for (int s = 0; s < MIX_VOICES; s++) {
        active += m->voice[s].active;
        virt += m->voice[s].active && m->voice[s].virt;
    }
    double t1 = m->clock ? m->clock() : 0.0;
    float us = (float)((t1 - t0) * 1e6);
    float buf_us = (float)frames / m->sr * 1e6f;
    m->avg_us = m->avg_us > 0 ? 0.92f * m->avg_us + 0.08f * us : us;
    // Graceful degradation: past 18 % of the buffer drop optional layers, past 30 % also most
    // early reflections; back under 10 % restores both.
    float load = m->avg_us / fmaxf(buf_us, 1.0f);
    if (load > 0.30f) m->degrade = 2;
    else if (load > 0.18f && m->degrade < 1) m->degrade = 1;
    else if (load < 0.10f) m->degrade = 0;
    if (m->force_degrade >= 0) m->degrade = m->force_degrade;
    m->stat_us = us;
    m->stat_avg_us = m->avg_us;
    m->stat_buffer_us = buf_us;
    m->stat_active = active;
    m->stat_virtual = virt;
    m->stat_rendered = rendered;
    m->stat_layers = layers;
    m->stat_degrade = m->degrade;
    m->stat_peak = peak;
    syn_ftz_restore(ftz);
    t_in_render = false;
}
