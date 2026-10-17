#include "audio.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "miniaudio.h"
#include "death_synth.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static void audio_loops_shutdown(void);

#define MAX_VOICES 48
#define MAX_KNOWN 64

typedef struct Voice { ma_sound sound; bool used; bool lpf_on; ma_lpf_node lpf; } Voice;
#define MAX_SCHEDULED 96
// Sounds that start later (gunfire travelling at the speed of sound): played by audio_update.
typedef struct Scheduled { double at; char name[32]; vec3 pos; float volume; SoundCategory cat; AudioFx fx; } Scheduled;
typedef struct KnownSound { u32 hash; bool exists; char path[256]; } KnownSound;

typedef struct AudioSystem {
    ma_engine engine;
    ma_sound_group groups[4];
    bool ok;
    Voice voices[MAX_VOICES];
    int next_voice;
    KnownSound known[MAX_KNOWN];
    int known_count;
    ma_sound music;
    bool music_loaded;
    Cvar *master, *music_vol, *sfx, *voice, *ui, *trace;
    // Death / downed mix (docs/REVIVE.md): world buses -> low-pass -> endpoint; the synth -> endpoint.
    ma_lpf_node lpf;
    bool lpf_ok;
    float lpf_applied;
    struct SynthNode { ma_node_base base; DeathSynth synth; float rate; } synth_node;
    bool synth_ok;
    AudioDeathMix mix;
    bool lpf_ready;   // per-voice low-pass (gunfire distance / air absorption)
    Scheduled sched[MAX_SCHEDULED];
    int sched_count;
} AudioSystem;

#define A (g_engine.audio)

static void synth_process(ma_node *node, const float **in, ma_uint32 *in_count, float **out, ma_uint32 *out_count) {
    (void)in;
    (void)in_count;
    struct SynthNode *n = (struct SynthNode *)node;
    memset(out[0], 0, sizeof(float) * 2 * *out_count);
    death_synth_render(&n->synth, out[0], (int)*out_count, 2, n->rate);
}

static ma_node_vtable g_synth_vtable = { synth_process, NULL, 0, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING };

static void death_mix_init(void) {
    ma_node_graph *graph = ma_engine_get_node_graph(&A->engine);
    ma_uint32 rate = ma_engine_get_sample_rate(&A->engine), ch = ma_engine_get_channels(&A->engine);
    A->mix.lpf_hz = 20000.0f;
    A->mix.world_gain = 1.0f;
    A->lpf_applied = 20000.0f;
    ma_lpf_node_config lc = ma_lpf_node_config_init(ch, rate, 20000.0, 2);
    if (ma_lpf_node_init(graph, &lc, NULL, &A->lpf) == MA_SUCCESS) {
        A->lpf_ok = true;
        ma_node_attach_output_bus(&A->lpf, 0, ma_engine_get_endpoint(&A->engine), 0);
        for (int i = 0; i < 3; i++) ma_node_attach_output_bus(&A->groups[i], 0, &A->lpf, 0);   // sfx, music, voice (not the UI)
    }
    ma_uint32 out_ch = 2;
    ma_node_config nc = ma_node_config_init();
    nc.vtable = &g_synth_vtable;
    nc.pOutputChannels = &out_ch;
    death_synth_init(&A->synth_node.synth);
    A->synth_node.rate = (float)rate;
    if (ma_node_init(graph, &nc, NULL, &A->synth_node.base) == MA_SUCCESS) {
        A->synth_ok = true;
        ma_node_attach_output_bus(&A->synth_node.base, 0, ma_engine_get_endpoint(&A->engine), 0);
    }
}

bool audio_init(void) {
    A = (AudioSystem *)calloc(1, sizeof(AudioSystem));
    A->master = cvar_reg_float("snd_master", 0.8f, 0, 1, CVAR_ARCHIVE, "master volume");
    A->music_vol = cvar_reg_float("snd_music", 0.4f, 0, 1, CVAR_ARCHIVE, "music volume");
    A->sfx = cvar_reg_float("snd_sfx", 0.9f, 0, 1, CVAR_ARCHIVE, "effects volume");
    A->voice = cvar_reg_float("snd_voice", 1.0f, 0, 1, CVAR_ARCHIVE, "voice chat volume (reserved)");
    A->ui = cvar_reg_float("snd_ui", 0.8f, 0, 1, CVAR_ARCHIVE, "interface sounds volume");
    A->trace = cvar_reg_bool("snd_trace", false, 0, "log every delayed / filtered sound (gunfire timing tests; works with --nosound)");
    for (int i = 1; i < g_engine.argc; i++)
        if (!strcmp(g_engine.argv[i], "--nosound") || !strcmp(g_engine.argv[i], "--test")) { log_info("audio: disabled by --nosound"); return false; }
    ma_engine_config cfg = ma_engine_config_init();
    if (ma_engine_init(&cfg, &A->engine) != MA_SUCCESS) {
        log_warn("audio: no output device, running silent");
        return false;
    }
    for (int i = 0; i < 4; i++) ma_sound_group_init(&A->engine, 0, NULL, &A->groups[i]);
    death_mix_init();
    // One low-pass per voice (distance / air absorption on gunfire), routed in only when used.
    A->lpf_ready = true;
    for (int i = 0; i < MAX_VOICES && A->lpf_ready; i++) {
        ma_lpf_node_config lc = ma_lpf_node_config_init(ma_engine_get_channels(&A->engine), ma_engine_get_sample_rate(&A->engine), 20000.0, 2);
        if (ma_lpf_node_init(ma_engine_get_node_graph(&A->engine), &lc, NULL, &A->voices[i].lpf) != MA_SUCCESS) A->lpf_ready = false;
    }
    A->ok = true;
    log_info("audio: miniaudio engine at %u Hz", ma_engine_get_sample_rate(&A->engine));
    return true;
}

void audio_shutdown(void) {
    if (!A) return;
    if (A->ok) {
        for (int i = 0; i < MAX_VOICES; i++) if (A->voices[i].used) ma_sound_uninit(&A->voices[i].sound);
        if (A->lpf_ready)
            for (int i = 0; i < MAX_VOICES; i++) ma_lpf_node_uninit(&A->voices[i].lpf, NULL);
        if (A->music_loaded) ma_sound_uninit(&A->music);
        audio_loops_shutdown();
        if (A->synth_ok) ma_node_uninit(&A->synth_node.base, NULL);
        for (int i = 0; i < 4; i++) ma_sound_group_uninit(&A->groups[i]);
        if (A->lpf_ok) ma_lpf_node_uninit(&A->lpf, NULL);
        ma_engine_uninit(&A->engine);
    }
    free(A);
    A = NULL;
}

bool audio_available(void) { return A && A->ok; }

void audio_set_death_mix(const AudioDeathMix *m) {
    if (!A) return;
    A->mix = *m;
    if (!A->ok) return;
    // The cutoff moves per frame (the sweep is ~0.5 s); tiny changes are skipped (filter reinit).
    float hz = fminf(fmaxf(m->lpf_hz, 40.0f), 20000.0f);
    if (A->lpf_ok && fabsf(hz - A->lpf_applied) > A->lpf_applied * 0.01f) {
        ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&A->engine), ma_engine_get_sample_rate(&A->engine), hz, 2);
        if (ma_lpf_node_reinit(&cfg, &A->lpf) == MA_SUCCESS) A->lpf_applied = hz;
    }
    if (A->synth_ok) {
        DeathSynthParams p = { m->ring_gain, m->ring_freq, m->warble_hz, m->warble_depth, m->heart_gain, m->heart_bpm };
        death_synth_set(&A->synth_node.synth, &p);
    }
}

const AudioDeathMix *audio_death_mix(void) { return A ? &A->mix : NULL; }

static const char *resolve(const char *name) {
    u32 h = dg_hash_str(name);
    for (int i = 0; i < A->known_count; i++)
        if (A->known[i].hash == h) return A->known[i].exists ? A->known[i].path : NULL;
    KnownSound *k = A->known_count < MAX_KNOWN ? &A->known[A->known_count++] : &A->known[0];
    k->hash = h;
    k->exists = false;
    const char *exts[] = { ".wav", ".ogg" };
    for (int e = 0; e < 2; e++) {
        char rel[128], buf[256];
        snprintf(rel, sizeof(rel), "assets/sounds/%s%s", name, exts[e]);
        const char *p = platform_data_path(rel, buf, sizeof(buf));
        if (platform_file_exists(p)) {
            dg_strcpy(k->path, sizeof(k->path), p);
            k->exists = true;
            break;
        }
    }
    if (!k->exists) log_debug("audio: no sound file for '%s'", name);
    return k->exists ? k->path : NULL;
}

static ma_sound_group *group_for(SoundCategory c) {
    switch (c) {
    case SND_MUSIC: return &A->groups[1];
    case SND_VOICE: return &A->groups[2];
    case SND_UI: return &A->groups[3];
    default: return &A->groups[0];
    }
}

static void play(const char *name, vec3 pos, float volume, SoundCategory cat, bool spatial, const AudioFx *fx) {
    if (!audio_available()) return;
    const char *path = resolve(name);
    if (!path) return;
    Voice *v = &A->voices[A->next_voice];
    A->next_voice = (A->next_voice + 1) % MAX_VOICES;
    if (v->used) ma_sound_uninit(&v->sound);
    v->used = false;
    // MA_SOUND_FLAG_DECODE caches decoded data in the resource manager, so repeats are cheap.
    if (ma_sound_init_from_file(&A->engine, path, MA_SOUND_FLAG_DECODE, group_for(cat), NULL, &v->sound) != MA_SUCCESS) return;
    v->used = true;
    ma_sound_set_volume(&v->sound, volume);
    if (spatial) {
        ma_sound_set_position(&v->sound, pos.x, pos.y, pos.z);
        ma_sound_set_min_distance(&v->sound, fx && fx->min_dist > 0 ? fx->min_dist : 1.5f);
        ma_sound_set_max_distance(&v->sound, fx && fx->max_dist > 0 ? fx->max_dist : 60.0f);
        ma_sound_set_rolloff(&v->sound, fx && fx->rolloff > 0 ? fx->rolloff : 1.0f);
        ma_sound_set_attenuation_model(&v->sound, ma_attenuation_model_inverse);
    } else {
        ma_sound_set_spatialization_enabled(&v->sound, MA_FALSE);
    }
    if (fx && fx->pitch > 0) ma_sound_set_pitch(&v->sound, fx->pitch);
    // Low-pass: the sound -> its voice's filter -> the category group.
    if (fx && fx->lowpass_hz > 0 && fx->lowpass_hz < 18000.0f && A->lpf_ready) {
        ma_lpf_config lc = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&A->engine), ma_engine_get_sample_rate(&A->engine), fx->lowpass_hz, 2);
        ma_lpf_node_reinit(&lc, &v->lpf);
        if (!v->lpf_on) ma_node_attach_output_bus(&v->lpf, 0, group_for(cat), 0);
        ma_node_attach_output_bus(&v->sound, 0, &v->lpf, 0);
        v->lpf_on = true;
    }
    ma_sound_start(&v->sound);
}

void audio_play(const char *name, vec3 pos, float volume, SoundCategory cat) { play(name, pos, volume, cat, true, NULL); }

bool audio_has_sound(const char *name) {
    if (!A || !name || !name[0]) return false;
    return resolve(name) != NULL;
}

void audio_play_fx(const char *name, vec3 pos, float volume, SoundCategory cat, const AudioFx *fx) {
    if (!A || !name || !name[0]) return;
    if (fx && fx->delay > 0.0005f) {
        if (A->sched_count >= MAX_SCHEDULED) return;   // a wall of gunfire: drop the newest
        Scheduled *s = &A->sched[A->sched_count++];
        s->at = platform_time() + fx->delay;
        dg_strcpy(s->name, sizeof(s->name), name);
        s->pos = pos;
        s->volume = volume;
        s->cat = cat;
        s->fx = *fx;
        s->fx.delay = 0.0f;
        if (A->trace && A->trace->i) log_info("snd: %s scheduled in %.3f s (lpf %.0f Hz, vol %.3f)", name, fx->delay, fx->lowpass_hz, volume);
        return;
    }
    if (A->trace && A->trace->i) log_info("snd: %s now (lpf %.0f Hz, vol %.3f)", name, fx ? fx->lowpass_hz : 0.0f, volume);
    play(name, pos, volume, cat, true, fx);
}

static void run_scheduled(void) {
    double now = platform_time();
    int keep = 0;
    for (int i = 0; i < A->sched_count; i++) {
        Scheduled *s = &A->sched[i];
        if (s->at > now) { A->sched[keep++] = *s; continue; }
        if (A->trace && A->trace->i) log_info("snd: %s starts (late %.1f ms)", s->name, (now - s->at) * 1000.0);
        play(s->name, s->pos, s->volume, s->cat, true, &s->fx);
    }
    A->sched_count = keep;
}
void audio_play_2d(const char *name, float volume, SoundCategory cat) { play(name, v3(0, 0, 0), volume, cat, false, NULL); }

void audio_play_music(const char *name) {
    if (!audio_available()) return;
    const char *path = resolve(name);
    if (!path) return;
    if (A->music_loaded) ma_sound_uninit(&A->music);
    A->music_loaded = ma_sound_init_from_file(&A->engine, path, MA_SOUND_FLAG_STREAM, group_for(SND_MUSIC), NULL, &A->music) == MA_SUCCESS;
    if (A->music_loaded) {
        ma_sound_set_looping(&A->music, MA_TRUE);
        ma_sound_set_spatialization_enabled(&A->music, MA_FALSE);
        ma_sound_start(&A->music);
    }
}

#define MAX_LOOPS 24
// Looping sounds: 2D ambience (audio_loop_set) and positional loops (audio_loop3d: engines, tire
// screech) that fade out on their own once nobody refreshes them.
typedef struct Loop { u32 hash; bool used, spatial; ma_sound sound; float vol, target, pitch; int idle; } Loop;
static Loop g_loops[MAX_LOOPS];

static Loop *loop_get(u32 h, const char *name, float volume, bool spatial) {
    Loop *l = NULL;
    for (int i = 0; i < MAX_LOOPS; i++) if (g_loops[i].used && g_loops[i].hash == h) l = &g_loops[i];
    if (l || volume <= 0.001f) return l;
    const char *path = resolve(name);
    if (!path) return NULL;
    for (int i = 0; i < MAX_LOOPS && !l; i++) if (!g_loops[i].used) l = &g_loops[i];
    if (!l) return NULL;
    if (ma_sound_init_from_file(&A->engine, path, MA_SOUND_FLAG_DECODE, group_for(SND_SFX), NULL, &l->sound) != MA_SUCCESS) return NULL;
    l->used = true;
    l->hash = h;
    l->vol = 0.0f;
    l->pitch = 1.0f;
    l->spatial = spatial;
    l->idle = 0;
    ma_sound_set_looping(&l->sound, MA_TRUE);
    ma_sound_set_spatialization_enabled(&l->sound, spatial ? MA_TRUE : MA_FALSE);
    if (spatial) {
        ma_sound_set_min_distance(&l->sound, 3.0f);
        ma_sound_set_max_distance(&l->sound, 120.0f);
    }
    ma_sound_set_volume(&l->sound, 0.0f);
    ma_sound_seek_to_pcm_frame(&l->sound, (ma_uint64)(h % 20000u));   // engines of the same model do not phase
    ma_sound_start(&l->sound);
    return l;
}

static float s_ambience_gain = 1.0f;
void audio_set_ambience_gain(float g) { s_ambience_gain = g < 0 ? 0 : g; }

void audio_loop_set(const char *name, float volume) {
    volume *= s_ambience_gain;
    if (!audio_available()) return;
    Loop *l = loop_get(dg_hash_str(name), name, volume, false);
    if (!l) return;
    l->target = volume;
    l->idle = -1000000;   // 2D loops never time out
}

void audio_loop3d(u32 key, const char *name, vec3 pos, float volume, float pitch) {
    if (!audio_available()) return;
    Loop *l = loop_get(dg_hash_str(name) ^ (key * 2654435761u) ^ 0x51ED27u, name, volume, true);
    if (!l) return;
    l->target = volume;
    l->idle = 0;
    l->pitch = pitch;
    ma_sound_set_position(&l->sound, pos.x, pos.y, pos.z);
    ma_sound_set_pitch(&l->sound, fmaxf(pitch, 0.05f));
}

static void audio_loops_shutdown(void) {
    for (int i = 0; i < MAX_LOOPS; i++) if (g_loops[i].used) { ma_sound_uninit(&g_loops[i].sound); g_loops[i].used = false; }
}

static void update_loops(float dt) {
    for (int i = 0; i < MAX_LOOPS; i++) {
        Loop *l = &g_loops[i];
        if (!l->used) continue;
        if (l->spatial && ++l->idle > 6) l->target = 0.0f;   // not refreshed: fade out
        l->vol += (l->target - l->vol) * (1.0f - expf(-dt * (l->spatial ? 10.0f : 3.0f)));
        ma_sound_set_volume(&l->sound, l->vol);
        if (l->target <= 0.001f && l->vol < 0.002f) { ma_sound_uninit(&l->sound); l->used = false; }
    }
}

static vec3 s_listener;
static float s_sound_speed = 343.0f;
vec3 audio_listener_pos(void) { return s_listener; }
void audio_set_sound_speed(float c) { s_sound_speed = c > 100.0f ? c : 343.0f; }
float audio_travel_delay(vec3 pos) { return v3_dist(pos, s_listener) / s_sound_speed; }

void audio_update(vec3 pos, vec3 fwd, vec3 up) {
    s_listener = pos;
    if (A) run_scheduled();
    if (audio_available()) update_loops(1.0f / 60.0f);
    if (!audio_available()) return;
    ma_engine_listener_set_position(&A->engine, 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(&A->engine, 0, fwd.x, fwd.y, fwd.z);
    ma_engine_listener_set_world_up(&A->engine, 0, up.x, up.y, up.z);
    ma_engine_set_volume(&A->engine, A->master->f);
    float wg = A->mix.world_gain;   // death / downed duck (docs/REVIVE.md)
    ma_sound_group_set_volume(&A->groups[0], A->sfx->f * wg);
    ma_sound_group_set_volume(&A->groups[1], A->music_vol->f * wg);
    ma_sound_group_set_volume(&A->groups[2], A->voice->f * wg);
    ma_sound_group_set_volume(&A->groups[3], A->ui->f);
    // Recycle finished voices.
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &A->voices[i];
        if (v->used && ma_sound_at_end(&v->sound)) {
            ma_sound_uninit(&v->sound);
            v->used = false;
            if (v->lpf_on) { ma_node_detach_output_bus(&v->lpf, 0); v->lpf_on = false; }
        }
    }
}
