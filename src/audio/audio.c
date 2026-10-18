#include "audio.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/profiler.h"
#include "core/log.h"
#include "core/platform.h"
#include "miniaudio.h"
#include "mixer.h"
#include "audio/synth/syn_material.h"
#include "audio/env/env_world.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define AUDIO_RATE 48000          // the mixer and every patch run at 48 kHz (miniaudio resamples to the device)
#define MAX_SAMPLES 256
#define MAX_GVOICES MIX_VOICES
#define MAX_EVENTS 160
#define MAX_ECHOES 6
#define SCOPE_LEN (4 * AUDIO_RATE)   // the last 4 s of output (editor scope, audio_record)

// A name of the legacy API (audio_play "impact_concrete") mapped to a recipe with fixed inputs.
typedef struct SoundEvent {
    u32 hash;
    char recipe[32];
    int ninputs;
    char key[6][20];
    float val[6];
    float volume;
} SoundEvent;

// Game-side voice: what it is, where, its environment state; the mixer owns the audio.
typedef struct GVoice {
    bool used, loop, echo, virt;
    u16 gen;
    MixHandle h;
    int recipe;
    u32 seed;
    char sample[40];
    float inputs[RCP_MAX_INPUTS];
    int layer_map[RCP_MAX_LAYERS];   // mixer layer -> recipe layer
    int nlayers;
    float volume, pitch;
    u32 key;                          // loops: refreshed by key
    int idle;
    double started;
    EnvVoiceState env;
    EnvParams last;
    bool flat, fixed;                 // fixed: the preview's chain parameters, no world updates
} GVoice;

typedef struct AudioSystem {
    ma_engine engine;
    ma_sound_group groups[4];
    bool ok;
    ma_sound music;
    bool music_loaded;
    Cvar *master, *music_vol, *sfx, *voice, *ui, *trace;
    Cvar *env_on, *echoes, *echo_max, *occl_rays, *degrade, *helmet, *debug, *offline, *procedural;
    // Death / downed mix: music group -> low-pass -> endpoint; the mixer applies it to its own world buses.
    ma_lpf_node lpf;
    bool lpf_ok;
    float lpf_applied;
    AudioDeathMix mix;
    AudioVoice death_voice;
    float death_quiet;
    // DG mixer
    struct MixNode { ma_node_base base; Mixer *mixer; } node;
    bool node_ok;
    Mixer *mixer;
    SynLibrary lib;
    SynMaterialTable materials;
    // samples (decoded mono float at AUDIO_RATE)
    char sample_name[MAX_SAMPLES][40];
    float *sample_data[MAX_SAMPLES];
    int sample_frames[MAX_SAMPLES];
    int sample_count;
    u32 missing[64];
    int missing_count;
    SynSampleBank bank;
    SoundEvent events[MAX_EVENTS];
    int event_count;
    GVoice voices[MAX_GVOICES];
    int ray_cursor;
    AudioListenerState state;
    float ambience_gain;
    EnvReverbParams rev_applied[ENV_ZONES], rev_pin[ENV_ZONES];
    double rev_pin_until[ENV_ZONES];
    MixListener lis_applied;
    // scope (editor): the last rendered output, mono
    float scope[SCOPE_LEN];
    int scope_pos;
    float offline_buf[2 * 4096];
    double offline_acc;
} AudioSystem;

#define A (g_engine.audio)

static vec3 s_listener, s_fwd = { 0, 0, -1 }, s_up = { 0, 1, 0 };
static float s_sound_speed = 343.0f;

// ------------------------------------------------------------------ samples

static int sample_find(const char *name) {
    for (int i = 0; i < A->sample_count; i++) if (!strcmp(A->sample_name[i], name)) return i;
    return -1;
}

static bool sample_path(const char *name, char *out, size_t n) {
    const char *exts[] = { ".wav", ".ogg" };
    for (int e = 0; e < 2; e++) {
        char rel[128], buf[256];
        snprintf(rel, sizeof(rel), "assets/sounds/%s%s", name, exts[e]);
        const char *p = platform_data_path(rel, buf, sizeof(buf));
        if (platform_file_exists(p)) { dg_strcpy(out, n, p); return true; }
    }
    return false;
}

// Decodes on first use (game thread): mono float at the mixer rate. The bank only ever grows, so the
// audio thread can read any id it was handed.
static int sample_resolve(const char *name, void *user) {
    (void)user;
    int i = sample_find(name);
    if (i >= 0) return i;
    u32 h = dg_hash_str(name);
    for (int k = 0; k < A->missing_count; k++) if (A->missing[k] == h) return -1;
    char path[256];
    float *pcm = NULL;
    ma_uint64 frames = 0;
    if (A->sample_count < MAX_SAMPLES && sample_path(name, path, sizeof(path))) {
        ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 1, AUDIO_RATE);
        void *data = NULL;
        if (ma_decode_file(path, &dc, &frames, &data) == MA_SUCCESS && frames > 1) {
            pcm = (float *)malloc(sizeof(float) * (size_t)frames);
            if (pcm) memcpy(pcm, data, sizeof(float) * (size_t)frames);
        }
        ma_free(data, NULL);
    }
    if (!pcm) {
        if (A->missing_count < 64) A->missing[A->missing_count++] = h;
        log_debug("audio: no sound file for '%s'", name);
        return -1;
    }
    i = A->sample_count;
    dg_strcpy(A->sample_name[i], sizeof(A->sample_name[i]), name);
    A->sample_data[i] = pcm;
    A->sample_frames[i] = (int)frames;
    A->sample_count = i + 1;
    A->bank.count = A->sample_count;
    return i;
}

bool audio_has_sound(const char *name) {
    if (!A || !name || !name[0]) return false;
    return sample_resolve(name, NULL) >= 0;
}

// ------------------------------------------------------------------ library

static void load_dir(const char *rel, bool patches) {
    char buf[512], path[640], err[256];
    const char *dir = platform_data_path(rel, buf, sizeof(buf));
    int count = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    for (int i = 0; files && i < count; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        size_t sz;
        char *txt = platform_read_file(path, &sz);
        if (!txt) continue;
        int r = patches ? syn_lib_add_patch(&A->lib, txt, err, sizeof(err)) : syn_lib_add_recipe(&A->lib, txt, err, sizeof(err));
        if (r < 0) log_warn("audio: %s: %s", files[i], err);
        free(txt);
    }
    SDL_free(files);
}

static void load_events(void) {
    A->event_count = 0;
    char buf[512];
    size_t sz;
    char *txt = platform_read_file(platform_data_path("assets/audio/events.json", buf, sizeof(buf)), &sz);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "sounds")) {
        if (A->event_count >= MAX_EVENTS) break;
        SoundEvent *e = &A->events[A->event_count];
        memset(e, 0, sizeof(*e));
        e->hash = dg_hash_str(it->string);
        e->volume = 1.0f;
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(it, "recipe"), *v = cJSON_GetObjectItemCaseSensitive(it, "volume");
        if (!cJSON_IsString(r)) continue;
        dg_strcpy(e->recipe, sizeof(e->recipe), r->valuestring);
        if (cJSON_IsNumber(v)) e->volume = (float)v->valuedouble;
        const cJSON *in;
        cJSON_ArrayForEach(in, cJSON_GetObjectItemCaseSensitive(it, "inputs")) {
            if (e->ninputs >= 6 || !cJSON_IsNumber(in)) continue;
            dg_strcpy(e->key[e->ninputs], sizeof(e->key[0]), in->string);
            e->val[e->ninputs++] = (float)in->valuedouble;
        }
        A->event_count++;
    }
    cJSON_Delete(root);
}

static void load_library(void) {
    syn_material_defaults(&A->materials);
    char buf[512], err[160];
    size_t sz;
    char *txt = platform_read_file(platform_data_path("assets/audio/materials.json", buf, sizeof(buf)), &sz);
    if (txt && !syn_material_parse(&A->materials, txt, err, sizeof(err))) log_warn("audio: %s", err);
    free(txt);
    syn_material_set_table(&A->materials);
    load_dir("assets/audio/patches", true);
    load_dir("assets/audio/recipes", false);
    load_events();
    log_info("audio: %d patches, %d recipes, %d events", A->lib.npatches, A->lib.nrecipes, A->event_count);
}

void audio_reload(void) {
    if (!A || !A->mixer) return;
    // Patches are replaced in place (old copies stay alive for voices still rendering them); recipes
    // are game-thread data.
    load_library();
}

void audio_preview_reverb(int zone, const EnvReverbParams *p, float hold_s) {
    if (!A || zone < 0 || zone >= ENV_ZONES) return;
    A->rev_pin[zone] = *p;
    A->rev_pin_until[zone] = platform_time() + hold_s;
}

bool audio_has_recipe(const char *name) { return A && name && syn_lib_find_recipe(&A->lib, name) >= 0; }
struct Mixer *audio_mixer(void) { return A ? A->mixer : NULL; }
struct SynLibrary *audio_library(void) { return A ? &A->lib : NULL; }

// ------------------------------------------------------------------ device

static void mix_process(ma_node *node, const float **in, ma_uint32 *in_count, float **out, ma_uint32 *out_count) {
    (void)in;
    (void)in_count;
    struct MixNode *n = (struct MixNode *)node;
    mixer_render(n->mixer, out[0], (int)*out_count);
    // Scope for the editor (benign race: a display only).
    AudioSystem *a = A;
    if (a) {
        for (ma_uint32 i = 0; i < *out_count; i++) {
            a->scope[a->scope_pos] = 0.5f * (out[0][i * 2] + out[0][i * 2 + 1]);
            a->scope_pos = (a->scope_pos + 1) % SCOPE_LEN;
        }
    }
}

static ma_node_vtable g_mix_vtable = { mix_process, NULL, 0, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING };

int audio_scope(float *out, int max) {
    if (!A) return 0;
    int n = max < SCOPE_LEN ? max : SCOPE_LEN;
    int start = (A->scope_pos - n + SCOPE_LEN) % SCOPE_LEN;
    for (int i = 0; i < n; i++) out[i] = A->scope[(start + i) % SCOPE_LEN];
    return n;
}

static void cmd_synth_play(int argc, const char **argv);
static void cmd_voices(int argc, const char **argv);
static void cmd_reload(int argc, const char **argv) { (void)argc; (void)argv; audio_reload(); }
static void cmd_record(int argc, const char **argv);
static void cmd_env_probe(int argc, const char **argv);
static void audio_record_poll(void);

bool audio_init(void) {
    A = (AudioSystem *)calloc(1, sizeof(AudioSystem));
    A->master = cvar_reg_float("snd_master", 0.8f, 0, 1, CVAR_ARCHIVE | CVAR_EDITABLE, "master volume");
    A->music_vol = cvar_reg_float("snd_music", 0.4f, 0, 1, CVAR_ARCHIVE | CVAR_EDITABLE, "music volume");
    A->sfx = cvar_reg_float("snd_sfx", 0.9f, 0, 1, CVAR_ARCHIVE | CVAR_EDITABLE, "effects volume");
    A->voice = cvar_reg_float("snd_voice", 1.0f, 0, 1, CVAR_ARCHIVE | CVAR_EDITABLE, "voice chat volume (reserved)");
    A->ui = cvar_reg_float("snd_ui", 0.8f, 0, 1, CVAR_ARCHIVE | CVAR_EDITABLE, "interface sounds volume");
    A->trace = cvar_reg_bool("snd_trace", false, CVAR_LOCKED, "log every delayed / filtered sound (gunfire timing tests; works with --nosound)");
    A->env_on = cvar_reg_bool("snd_env", true, CVAR_LOCKED, "per-voice environment chain (distance, air, occlusion, rooms, echoes; docs/AUDIO_PROCEDURAL.md)");
    A->echoes = cvar_reg_bool("snd_echoes", true, CVAR_LOCKED, "outdoor discrete echoes of impulsive sounds (cliffs, facades, tree lines)");
    A->echo_max = cvar_reg_int("snd_echo_max", 4, 0, MAX_ECHOES, CVAR_LOCKED, "echo voices per impulsive outdoor sound");
    A->occl_rays = cvar_reg_int("snd_occlusion_voices", 6, 1, 64, CVAR_LOCKED, "voices whose occlusion rays are re-cast per frame");
    A->degrade = cvar_reg_int("snd_degrade", -1, -1, 2, CVAR_LOCKED, "mixer degradation: -1 automatic, 0 full, 1 no optional layers, 2 also fewer early reflections");
    A->helmet = cvar_reg_bool("snd_helmet", false, CVAR_LOCKED, "listener wears a helmet (stub: duller highs)");
    A->debug = cvar_reg_int("audio_debug", 0, 0, 3, CVAR_LOCKED, "audio debug: 1 voice readout, 2 + occlusion / reflection rays, 3 + room box and echo scan");
    A->offline = cvar_reg_bool("snd_offline", true, CVAR_LOCKED, "with no output device (--test / --nosound) run the mixer offline so voices still play (tests, audio_record)");
    A->procedural = cvar_reg_bool("snd_procedural", true, CVAR_LOCKED, "events with a recipe play it (0: their legacy samples only)");
    cmd_register("audio_synth_play", cmd_synth_play, "play a recipe at the crosshair / listener: audio_synth_play <recipe> [key=value ...] [@sample] [2d]");
    cmd_register("audio_voices", cmd_voices, "list the playing audio voices (recipe, distance, gain, occlusion, zone)");
    cmd_register("audio_reload", cmd_reload, "reload audio patches, recipes, materials and events from disk");
    cmd_register("audio_record", cmd_record, "record the mixer output: audio_record <file.wav> <seconds>");
    cmd_register("audio_env_probe", cmd_env_probe, "probe the room / outdoor echo field at the listener and log the estimate");
    A->ambience_gain = 1.0f;
    A->mix.lpf_hz = 20000.0f;
    A->mix.world_gain = 1.0f;
    A->state.health = 1.0f;
    A->state.temp_c = 15.0f;
    A->state.humidity = 60.0f;
    A->state.exposure = 1.0f;
    A->bank.data = (const float *const *)A->sample_data;
    A->bank.frames = A->sample_frames;
    syn_set_sample_bank(&A->bank);
    env_world_init();
    // The mixer and the library exist even without a device: tests and --nosound still emit voices
    // (and render them offline), so everything above the device stays testable.
    A->mixer = mixer_create((float)AUDIO_RATE, platform_time);
    syn_lib_init(&A->lib, (float)AUDIO_RATE, sample_resolve, NULL);
    load_library();
    for (int i = 1; i < g_engine.argc; i++)
        if (!strcmp(g_engine.argv[i], "--nosound") || !strcmp(g_engine.argv[i], "--test")) { log_info("audio: no device (--nosound), mixer offline"); return false; }
    ma_engine_config cfg = ma_engine_config_init();
    cfg.sampleRate = AUDIO_RATE;
    cfg.channels = 2;
    if (ma_engine_init(&cfg, &A->engine) != MA_SUCCESS) {
        log_warn("audio: no output device, running silent");
        return false;
    }
    for (int i = 0; i < 4; i++) ma_sound_group_init(&A->engine, 0, NULL, &A->groups[i]);
    ma_node_graph *graph = ma_engine_get_node_graph(&A->engine);
    A->lpf_applied = 20000.0f;
    ma_lpf_node_config lc = ma_lpf_node_config_init(2, AUDIO_RATE, 20000.0, 2);
    if (ma_lpf_node_init(graph, &lc, NULL, &A->lpf) == MA_SUCCESS) {
        A->lpf_ok = true;
        ma_node_attach_output_bus(&A->lpf, 0, ma_engine_get_endpoint(&A->engine), 0);
        ma_node_attach_output_bus(&A->groups[1], 0, &A->lpf, 0);   // music (the mixer filters its own world buses)
    }
    ma_uint32 out_ch = 2;
    ma_node_config nc = ma_node_config_init();
    nc.vtable = &g_mix_vtable;
    nc.pOutputChannels = &out_ch;
    A->node.mixer = A->mixer;
    if (A->mixer && ma_node_init(graph, &nc, NULL, &A->node.base) == MA_SUCCESS) {
        A->node_ok = true;
        ma_node_attach_output_bus(&A->node.base, 0, ma_engine_get_endpoint(&A->engine), 0);
    }
    A->ok = true;
    log_info("audio: miniaudio engine at %u Hz, DG mixer %s", ma_engine_get_sample_rate(&A->engine), A->node_ok ? "on" : "FAILED");
    return true;
}

void audio_shutdown(void) {
    if (!A) return;
    if (A->ok) {
        if (A->node_ok) ma_node_uninit(&A->node.base, NULL);
        if (A->music_loaded) ma_sound_uninit(&A->music);
        for (int i = 0; i < 4; i++) ma_sound_group_uninit(&A->groups[i]);
        if (A->lpf_ok) ma_lpf_node_uninit(&A->lpf, NULL);
        ma_engine_uninit(&A->engine);
    }
    mixer_destroy(A->mixer);
    syn_set_sample_bank(NULL);
    syn_lib_free(&A->lib);
    for (int i = 0; i < A->sample_count; i++) free(A->sample_data[i]);
    free(A);
    A = NULL;
}

bool audio_available(void) { return A && A->ok; }
static bool can_emit(void) { return A && A->mixer; }

// ------------------------------------------------------------------ emit

void audio_param(AudioParams *p, const char *key, float value) {
    for (int i = 0; i < p->n; i++) if (!strcmp(p->key[i], key)) { p->val[i] = value; return; }
    if (p->n >= AUDIO_MAX_PARAMS) return;
    p->key[p->n] = key;
    p->val[p->n++] = value;
}

void audio_emit_init(AudioEmit *e, const char *recipe, vec3 pos) {
    memset(e, 0, sizeof(*e));
    e->recipe = recipe;
    e->pos = pos;
    e->volume = 1.0f;
    e->delay = -1.0f;
}

static AudioVoice handle_of(int i) { return (AudioVoice)(i + 1) | ((AudioVoice)A->voices[i].gen << 16); }
static GVoice *voice_of(AudioVoice h) {
    if (!A || !h) return NULL;
    int i = (int)(h & 0xFFFFu) - 1;
    if (i < 0 || i >= MAX_GVOICES) return NULL;
    GVoice *v = &A->voices[i];
    return v->used && v->gen == (u16)(h >> 16) ? v : NULL;
}

static const SoundEvent *event_find(const char *name) {
    u32 h = dg_hash_str(name);
    for (int i = 0; i < A->event_count; i++) if (A->events[i].hash == h) return &A->events[i];
    return NULL;
}

static void fill_inputs(const SynRecipe *r, float *in, const AudioParams *p, float dist, u32 seed) {
    for (int i = 0; i < r->ninputs; i++) in[i] = r->input_def[i];
    for (int k = 0; p && k < p->n; k++) {
        int idx = syn_recipe_input(r, p->key[k]);
        if (idx >= 0) in[idx] = p->val[k];
    }
    int di = syn_recipe_input(r, "distance"), si = syn_recipe_input(r, "seed01");
    if (di >= 0) in[di] = dist;
    if (si >= 0) in[si] = (float)(dg_hash_u32(seed) >> 8) / 16777216.0f;
}

static void build_source(const SynRecipe *r, const AudioEmit *e, bool spatial, EnvSource *s) {
    memset(s, 0, sizeof(*s));
    s->pos = e->pos;
    s->fwd = e->fwd;
    s->vel = e->vel;
    s->volume = e->volume;
    s->min_dist = e->min_dist > 0 ? e->min_dist : r->min_dist;
    s->max_dist = e->max_dist > 0 ? e->max_dist : r->max_dist;
    s->rolloff = e->rolloff > 0 ? e->rolloff : r->rolloff;
    s->front_db = r->front_db;
    s->rear_db = r->rear_db;
    s->rear_lp = r->rear_lp;
    s->reverb = r->reverb;
    s->spatial = spatial;
    s->doppler = r->doppler;
    s->ambience = r->bus == SYN_BUS_AMBIENCE;
    s->max_taps = r->priority >= 0.8f ? 6 : 4;
    s->self_radius = e->self_radius;
}

static void apply_caps(const AudioEmit *e, EnvParams *ep) {
    if (e->lp_cap > 0 && e->lp_cap < ep->lp_hz) ep->lp_hz = e->lp_cap;
}

// Starts one voice (the direct sound or an echo child). Returns its index or -1.
static int start_voice(int ri, const AudioEmit *e, const AudioParams *p, const EnvSource *src, float start_delay, bool echo) {
    const SynRecipe *r = A->lib.recipe[ri];
    int slot = -1;
    for (int i = 0; i < MAX_GVOICES && slot < 0; i++) if (!A->voices[i].used) slot = i;
    if (slot < 0) {
        // Full: the oldest echo goes first, then the oldest quiet one-shot.
        double oldest = 1e30;
        for (int pass = 0; pass < 2 && slot < 0; pass++)
            for (int i = 0; i < MAX_GVOICES; i++) {
                GVoice *o = &A->voices[i];
                if (o->loop || (pass == 0 && !o->echo)) continue;
                if (o->started < oldest) { oldest = o->started; slot = i; }
            }
        if (slot < 0) return -1;
        mixer_stop(A->mixer, A->voices[slot].h, 0.01f);
        A->voices[slot].used = false;
    }
    GVoice *v = &A->voices[slot];
    u16 gen = (u16)(v->gen + 1);
    memset(v, 0, sizeof(*v));
    v->gen = gen ? gen : 1;
    float dist = v3_dist(e->pos, s_listener);
    fill_inputs(r, v->inputs, p, dist, e->seed);
    SynRecipeOut o;
    syn_recipe_eval(&A->lib, r, v->inputs, e->seed, e->sample, &o);
    MixStart ms;
    memset(&ms, 0, sizeof(ms));
    for (int i = 0; i < o.nlayers; i++) {
        if (!o.layer[i].on || !o.layer[i].patch) continue;
        if (echo && o.layer[i].optional) continue;   // echoes carry the body of the sound, not its details
        MixLayerStart *l = &ms.layer[ms.nlayers];
        l->patch = o.layer[i].patch;
        l->gain = o.layer[i].gain;
        l->optional = o.layer[i].optional;
        memcpy(l->params, o.layer[i].params, sizeof(l->params));
        int pp = syn_patch_param(l->patch, "pitch");
        if (pp >= 0 && e->pitch > 0 && !strncmp(l->patch->name, "sample:", 7)) l->params[pp] *= e->pitch;
        v->layer_map[ms.nlayers++] = i;
    }
    if (!ms.nlayers) { v->used = false; return -1; }
    v->nlayers = ms.nlayers;
    v->recipe = ri;
    v->seed = e->seed;
    if (e->sample) dg_strcpy(v->sample, sizeof(v->sample), e->sample);
    v->volume = e->volume;
    v->pitch = e->pitch;
    v->loop = r->loop;
    v->echo = echo;
    v->flat = !src->spatial;
    v->started = platform_time();
    env_world_voice_start(&v->env, src);
    EnvParams ep;
    float delay = env_world_voice_eval(&v->env, false, &ep);
    if (e->env) {
        ep = *e->env;
        v->fixed = true;
        delay = 0.0f;
    }
    apply_caps(e, &ep);
    if (!A->env_on->i && src->spatial) {
        // Chain off: the distance law and pan only (A / B against the full environment).
        float g = ep.gain, pan = ep.pan, fr = ep.front;
        env_params_neutral(&ep);
        ep.gain = g, ep.pan = pan, ep.front = fr, ep.spread = 0;
    }
    v->last = ep;
    ms.seed = e->seed;
    ms.bus = e->bus ? (u8)(e->bus - 1) : r->bus;
    ms.env = src->spatial;
    ms.doppler = src->doppler;
    ms.start_delay = start_delay >= 0 ? start_delay : (src->doppler ? 0.0f : delay);
    ms.volume = e->volume;
    ms.score = r->priority * fminf(1.0f, e->volume * ep.gain * 4.0f) + (r->loop ? 0.2f : 0.0f);
    ms.env_params = ep;
    v->h = mixer_start(A->mixer, &ms);
    if (!v->h) { v->used = false; return -1; }
    v->used = true;
    if (A->trace && A->trace->i)
        log_info("snd: %s%s starts in %.3f s (%.0f m, gain %.3f, lp %.0f Hz, occl %.0f/%.0f/%.0f dB, zone %d)", r->name, echo ? " echo" : "", ms.start_delay, dist, ep.gain,
                 ep.lp_hz, ep.band_db[0], ep.band_db[1], ep.band_db[2], v->env.info.zone);
    return slot;
}

static int recipe_for(const char *name, const AudioEmit *e, const AudioParams **pp, AudioParams *tmp, float *vol, bool loop, bool flat) {
    int ri = syn_lib_find_recipe(&A->lib, name);
    if (ri >= 0) return ri;
    const SoundEvent *ev = event_find(name);
    if (ev && A->procedural->i && (ri = syn_lib_find_recipe(&A->lib, ev->recipe)) >= 0) {
        // Event inputs first, the caller's values over them.
        memset(tmp, 0, sizeof(*tmp));
        for (int i = 0; i < ev->ninputs; i++) audio_param(tmp, ev->key[i], ev->val[i]);
        for (int i = 0; *pp && i < (*pp)->n; i++) audio_param(tmp, (*pp)->key[i], (*pp)->val[i]);
        *pp = tmp;
        *vol *= ev->volume;
        return ri;
    }
    (void)e;
    int bus = e->bus ? e->bus - 1 : SYN_BUS_SFX;
    return syn_lib_sample_recipe(&A->lib, name, loop, !flat, bus);
}

static AudioVoice emit(const AudioEmit *e, const AudioParams *p, u32 loop_key) {
    if (!can_emit() || !e->recipe || !e->recipe[0]) return 0;
    AudioParams tmp;
    float vol = e->volume;
    int ri = recipe_for(e->recipe, e, &p, &tmp, &vol, loop_key != 0, e->flat);
    if (ri < 0) return 0;
    const SynRecipe *r = A->lib.recipe[ri];
    AudioEmit ee = *e;
    ee.volume = vol;
    bool spatial = r->spatial && !e->flat;
    // A one-shot far beyond its distance law is not worth a voice.
    if (spatial && !loop_key && !r->loop && v3_dist(e->pos, s_listener) > r->max_dist * 1.2f) return 0;
    // Stacking guard: the same recipe again within stack_s near the same place folds into the louder
    // (a burst of pellets / gore particles in one frame otherwise piles up into one distorted sound).
    double now = platform_time();
    int same = 0, oldest = -1;
    for (int i = 0; i < MAX_GVOICES; i++) {
        GVoice *o = &A->voices[i];
        if (!o->used || o->recipe != ri || o->echo) continue;
        if (!loop_key && now - o->started < r->stack_s && v3_dist(o->env.src.pos, e->pos) < 2.0f) {
            if (vol > o->volume) { o->volume = vol; o->env.src.volume = vol; mixer_volume(A->mixer, o->h, vol); }
            return handle_of(i);
        }
        same++;
        if (!o->loop && (oldest < 0 || o->started < A->voices[oldest].started)) oldest = i;
    }
    if (same >= r->max_instances) {
        if (oldest < 0) return 0;
        mixer_stop(A->mixer, A->voices[oldest].h, 0.03f);
        A->voices[oldest].used = false;
    }
    EnvSource src;
    build_source(r, &ee, spatial, &src);
    int slot = start_voice(ri, &ee, p, &src, e->delay, false);
    if (slot < 0) return 0;
    GVoice *v = &A->voices[slot];
    v->key = loop_key;
    // Outdoor impulsive sounds: the surroundings give the tail. Each echo is the same recipe and seed
    // started later from its reflector, with that path's level and top cut (docs/AUDIO_PROCEDURAL.md "Echoes").
    if (r->echo && spatial && !e->no_echo && !e->env && A->echoes->i && A->env_on->i && env_world_query()) {
        const EnvRoom *room = env_world_room(e->pos, false);
        if (room && !room->indoor) {
            EnvEcho ech[MAX_ECHOES];
            int ne = env_world_echoes(e->pos, ech, A->echo_max->i);
            float direct = e->delay >= 0 ? e->delay : v3_dist(e->pos, s_listener) / s_sound_speed;
            for (int k = 0; k < ne; k++) {
                EnvSource es = src;
                es.pos = ech[k].point;
                es.echo_child = true;
                es.fwd = v3(0, 0, 0);
                es.volume = vol * ech[k].gain;
                es.rear_lp = ech[k].lp_hz;
                es.reverb = r->reverb;
                AudioEmit ce = ee;
                ce.volume = es.volume;
                ce.pos = es.pos;
                if (es.volume < 1e-3f) continue;
                start_voice(ri, &ce, p, &es, direct + ech[k].extra_s, true);
            }
        }
    }
    return handle_of(slot);
}

AudioVoice audio_emit(const AudioEmit *e, const AudioParams *p) { return emit(e, p, 0); }

void audio_voice_move(AudioVoice h, vec3 pos, vec3 fwd, vec3 vel) {
    GVoice *v = voice_of(h);
    if (!v) return;
    v->env.src.pos = pos;
    v->env.src.fwd = fwd;
    v->env.src.vel = vel;
}

void audio_voice_params(AudioVoice h, const AudioParams *p) {
    GVoice *v = voice_of(h);
    if (!v || !p) return;
    const SynRecipe *r = A->lib.recipe[v->recipe];
    float before[RCP_MAX_INPUTS];
    memcpy(before, v->inputs, sizeof(before));
    for (int k = 0; k < p->n; k++) {
        int idx = syn_recipe_input(r, p->key[k]);
        if (idx >= 0) v->inputs[idx] = p->val[k];
    }
    if (!memcmp(before, v->inputs, sizeof(before))) return;
    SynRecipeOut o;
    syn_recipe_eval(&A->lib, r, v->inputs, v->seed, v->sample[0] ? v->sample : NULL, &o);
    for (int i = 0; i < v->nlayers; i++) {
        const SynLayerOut *l = &o.layer[v->layer_map[i]];
        if (!l->patch) continue;
        float params[SYN_MAX_PARAMS];
        memcpy(params, l->params, sizeof(params));
        int pp = syn_patch_param(l->patch, "pitch");
        if (pp >= 0 && v->pitch > 0 && !strncmp(l->patch->name, "sample:", 7)) params[pp] *= v->pitch;
        mixer_params(A->mixer, v->h, i, params, l->patch->nparams);
    }
}

void audio_voice_volume(AudioVoice h, float volume) {
    GVoice *v = voice_of(h);
    if (!v || fabsf(volume - v->volume) < 1e-4f) return;
    v->volume = volume;
    v->env.src.volume = volume;
    mixer_volume(A->mixer, v->h, volume);
}

void audio_voice_stop(AudioVoice h, float fade_s) {
    GVoice *v = voice_of(h);
    if (!v) return;
    mixer_stop(A->mixer, v->h, fade_s);
    v->key = 0;
    v->loop = false;
}

bool audio_voice_alive(AudioVoice h) { return voice_of(h) != NULL; }

AudioVoice audio_loop_recipe(u32 key, const AudioEmit *e, const AudioParams *p) {
    if (!can_emit() || !key) return 0;
    for (int i = 0; i < MAX_GVOICES; i++) {
        GVoice *v = &A->voices[i];
        if (!v->used || v->key != key) continue;
        AudioVoice h = handle_of(i);
        v->idle = 0;
        v->env.src.pos = e->pos;
        v->env.src.fwd = e->fwd;
        v->env.src.vel = e->vel;
        audio_voice_volume(h, e->volume);
        audio_voice_params(h, p);
        if (e->pitch > 0 && fabsf(e->pitch - v->pitch) > 1e-3f) {
            // Sample loops pitch with the event (engine rpm on the legacy loop).
            v->pitch = e->pitch;
            const SynRecipe *r = A->lib.recipe[v->recipe];
            SynRecipeOut o;
            syn_recipe_eval(&A->lib, r, v->inputs, v->seed, v->sample[0] ? v->sample : NULL, &o);
            for (int l = 0; l < v->nlayers; l++) {
                const SynLayerOut *lo = &o.layer[v->layer_map[l]];
                if (!lo->patch || strncmp(lo->patch->name, "sample:", 7)) continue;
                float params[SYN_MAX_PARAMS];
                memcpy(params, lo->params, sizeof(params));
                int pp = syn_patch_param(lo->patch, "pitch");
                if (pp >= 0) params[pp] *= e->pitch;
                mixer_params(A->mixer, v->h, l, params, lo->patch->nparams);
            }
        }
        return h;
    }
    if (e->volume <= 0.001f) return 0;
    AudioVoice h = emit(e, p, key);
    GVoice *v = voice_of(h);
    if (v) { v->key = key; v->loop = true; }
    return h;
}

// ------------------------------------------------------------------ legacy API

static u8 bus_for(SoundCategory c) {
    switch (c) {
    case SND_VOICE: return SYN_BUS_VOICE + 1;
    case SND_UI: return SYN_BUS_UI + 1;
    default: return SYN_BUS_SFX + 1;
    }
}

void audio_play(const char *name, vec3 pos, float volume, SoundCategory cat) {
    if (!can_emit() || !name || !name[0]) return;
    AudioEmit e;
    audio_emit_init(&e, name, pos);
    e.volume = volume;
    e.bus = cat == SND_UI ? bus_for(cat) : 0;
    e.seed = dg_hash_u32((u32)(pos.x * 131.0f) ^ (u32)(pos.z * 977.0f) ^ (u32)(platform_time() * 1000.0));
    audio_emit(&e, NULL);
}

void audio_play_2d(const char *name, float volume, SoundCategory cat) {
    if (!can_emit() || !name || !name[0]) return;
    AudioEmit e;
    audio_emit_init(&e, name, s_listener);
    e.volume = volume;
    e.flat = true;
    e.bus = bus_for(cat);
    e.seed = dg_hash_u32((u32)(platform_time() * 1000.0));
    audio_emit(&e, NULL);
}

void audio_play_fx(const char *name, vec3 pos, float volume, SoundCategory cat, const AudioFx *fx) {
    if (!can_emit() || !name || !name[0]) return;
    AudioEmit e;
    audio_emit_init(&e, name, pos);
    e.volume = volume;
    e.bus = cat == SND_UI ? bus_for(cat) : 0;
    e.seed = dg_hash_u32((u32)(pos.x * 131.0f) ^ (u32)(pos.z * 977.0f) ^ (u32)(platform_time() * 1000.0));
    if (fx) {
        e.delay = fx->delay > 0.0005f ? fx->delay : -1.0f;
        e.lp_cap = fx->lowpass_hz;
        e.pitch = fx->pitch;
        e.min_dist = fx->min_dist;
        e.max_dist = fx->max_dist;
        e.rolloff = fx->rolloff;
    }
    audio_emit(&e, NULL);
}

void audio_play_music(const char *name) {
    if (!audio_available()) return;
    char path[256];
    if (!sample_path(name, path, sizeof(path))) return;
    if (A->music_loaded) ma_sound_uninit(&A->music);
    A->music_loaded = ma_sound_init_from_file(&A->engine, path, MA_SOUND_FLAG_STREAM, &A->groups[1], NULL, &A->music) == MA_SUCCESS;
    if (A->music_loaded) {
        ma_sound_set_looping(&A->music, MA_TRUE);
        ma_sound_set_spatialization_enabled(&A->music, MA_FALSE);
        ma_sound_start(&A->music);
    }
}

void audio_set_ambience_gain(float g) {
    if (A) A->ambience_gain = g < 0 ? 0 : g;
}

void audio_loop_set(const char *name, float volume) {
    if (!can_emit() || !name) return;
    AudioEmit e;
    audio_emit_init(&e, name, s_listener);
    e.volume = volume * A->ambience_gain;
    e.flat = true;
    e.bus = SYN_BUS_AMBIENCE + 1;
    u32 key = dg_hash_str(name) | 1u;
    AudioVoice h = audio_loop_recipe(key, &e, NULL);
    GVoice *v = voice_of(h);
    if (v) v->idle = -1000000;   // 2D ambience never times out (volume 0 fades it)
    if (v && e.volume <= 0.001f) audio_voice_stop(h, 0.4f);
}

void audio_loop3d(u32 key, const char *name, vec3 pos, float volume, float pitch) {
    if (!can_emit() || !name) return;
    AudioEmit e;
    audio_emit_init(&e, name, pos);
    e.volume = volume;
    e.pitch = pitch > 0.05f ? pitch : 0.05f;
    audio_loop_recipe((dg_hash_str(name) ^ (key * 2654435761u) ^ 0x51ED27u) | 1u, &e, NULL);
}

vec3 audio_listener_pos(void) { return s_listener; }
void audio_set_sound_speed(float c) { s_sound_speed = c > 100.0f ? c : 343.0f; }
float audio_sound_speed(void) { return s_sound_speed; }
float audio_travel_delay(vec3 pos) { return v3_dist(pos, s_listener) / s_sound_speed; }

void audio_set_listener_state(const AudioListenerState *s) {
    if (A) A->state = *s;
}

// ------------------------------------------------------------------ death mix

void audio_set_death_mix(const AudioDeathMix *m) {
    if (!A) return;
    A->mix = *m;
    // Music: the miniaudio low-pass (tiny changes are skipped: filter reinit). The mixer's world buses
    // follow in audio_update (listener filter).
    float hz = fminf(fmaxf(m->lpf_hz, 40.0f), 20000.0f);
    if (A->ok && A->lpf_ok && fabsf(hz - A->lpf_applied) > A->lpf_applied * 0.01f) {
        ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, 2, AUDIO_RATE, hz, 2);
        if (ma_lpf_node_reinit(&cfg, &A->lpf) == MA_SUCCESS) A->lpf_applied = hz;
    }
    if (!can_emit()) return;
    bool on = m->ring_gain > 1e-4f || m->heart_gain > 1e-4f;
    AudioParams p = { 0 };
    audio_param(&p, "ring_gain", m->ring_gain);
    audio_param(&p, "ring_freq", m->ring_freq > 0 ? m->ring_freq : 3600.0f);
    audio_param(&p, "warble_hz", m->warble_hz);
    audio_param(&p, "warble_depth", m->warble_depth);
    audio_param(&p, "heart_gain", m->heart_gain);
    audio_param(&p, "heart_bpm", m->heart_bpm > 0 ? m->heart_bpm : 70.0f);
    if (!voice_of(A->death_voice) && on) {
        AudioEmit e;
        audio_emit_init(&e, "death_ring", s_listener);
        e.flat = true;
        A->death_voice = audio_emit(&e, &p);
    } else if (voice_of(A->death_voice)) {
        audio_voice_params(A->death_voice, &p);
    }
    A->death_quiet = on ? 0.0f : A->death_quiet;
}

const AudioDeathMix *audio_death_mix(void) { return A ? &A->mix : NULL; }

// ------------------------------------------------------------------ update

static bool env_changed(const EnvParams *a, const EnvParams *b) {
    if (fabsf(a->gain - b->gain) > 0.004f * fmaxf(a->gain, 1e-3f) + 1e-6f) return true;
    if (fabsf(a->lp_hz - b->lp_hz) > 0.01f * a->lp_hz) return true;
    if (fabsf(a->pan - b->pan) > 0.005f || fabsf(a->front - b->front) > 0.02f || fabsf(a->spread - b->spread) > 0.01f) return true;
    if (fabsf(a->delay_s - b->delay_s) > 0.00005f) return true;
    for (int i = 0; i < ENV_BANDS; i++) if (fabsf(a->band_db[i] - b->band_db[i]) > 0.25f) return true;
    for (int i = 0; i < ENV_ZONES; i++) if (fabsf(a->send[i] - b->send[i]) > 0.01f) return true;
    if (a->ntaps != b->ntaps) return true;
    for (int i = 0; i < a->ntaps; i++)
        if (fabsf(a->tap[i].delay_s - b->tap[i].delay_s) > 0.0003f || fabsf(a->tap[i].gain - b->tap[i].gain) > 0.01f || fabsf(a->tap[i].pan - b->tap[i].pan) > 0.02f) return true;
    return false;
}

static bool reverb_changed(const EnvReverbParams *a, const EnvReverbParams *b) {
    return fabsf(a->rt60_low - b->rt60_low) > 0.02f || fabsf(a->rt60_high - b->rt60_high) > 0.02f || fabsf(a->size - b->size) > 0.02f ||
           fabsf(a->predelay_ms - b->predelay_ms) > 0.5f || fabsf(a->wet - b->wet) > 0.01f;
}

static void listener_mix(void) {
    const AudioListenerState *s = &A->state;
    MixListener l = { 20000.0f, 0, 0, 1.0f, 20000.0f, 1.0f };
    if (s->underwater) { l.lp_hz = 450.0f; l.low_db += 3.0f; l.gain *= 0.7f; }
    if (s->in_vehicle) { l.lp_hz = fminf(l.lp_hz, 2400.0f); l.low_db += 4.0f; l.gain *= 0.85f; }
    if (s->health < 0.3f) l.lp_hz = fminf(l.lp_hz, 3500.0f + 16500.0f * s->health / 0.3f);
    if (s->ads) l.high_db -= 1.0f;   // focus: a touch less hiss and clutter while aiming
    if (s->helmet || A->helmet->i) l.high_db -= 4.0f;
    l.death_lp_hz = fminf(fmaxf(A->mix.lpf_hz, 40.0f), 20000.0f);
    l.death_gain = fmaxf(A->mix.world_gain, 0.0f);
    const MixListener *o = &A->lis_applied;
    if (fabsf(l.lp_hz - o->lp_hz) > 1 || l.low_db != o->low_db || l.high_db != o->high_db || l.gain != o->gain || fabsf(l.death_lp_hz - o->death_lp_hz) > 1 || l.death_gain != o->death_gain) {
        mixer_set_listener(A->mixer, &l);
        A->lis_applied = l;
    }
}

void audio_update(vec3 pos, vec3 fwd, vec3 up) {
    s_listener = pos;
    s_fwd = fwd;
    s_up = up;
    if (!can_emit()) return;
    PROF_BEGIN("audio_update");
    float dt = (float)g_engine.frame_dt;
    if (dt <= 0 || dt > 0.25f) dt = 1.0f / 60.0f;
    EnvListener el = *env_world_listener();
    el.pos = pos;
    el.fwd = v3_norm(fwd);
    el.up = v3_norm(up);
    el.right = v3_norm(v3_cross(el.fwd, el.up));
    el.underwater = A->state.underwater;
    el.in_vehicle = A->state.in_vehicle;
    el.ads = A->state.ads;
    el.helmet = A->state.helmet;
    el.health = A->state.health;
    el.weather.temp_c = A->state.temp_c;
    el.weather.humidity = A->state.humidity;
    el.weather.rain = A->state.rain;
    el.weather.wind = A->state.wind;
    el.weather.snow = A->state.snow;
    el.weather.exposure = A->state.exposure;
    el.sound_speed = s_sound_speed;
    env_world_set_listener(&el);
    env_world_set_budget(A->occl_rays->i);
    env_world_begin_frame(dt);
    mixer_update(A->mixer);
    // Voices: reclaim the finished, time out unrefreshed loops, refresh every environment (rays for a
    // few per frame, round robin), virtualize the inaudible.
    int budget = A->occl_rays->i, live = 0;
    for (int k = 0; k < MAX_GVOICES; k++) {
        int i = (A->ray_cursor + k) % MAX_GVOICES;
        GVoice *v = &A->voices[i];
        if (!v->used) continue;
        if (!mixer_alive(A->mixer, v->h)) { v->used = false; continue; }
        live++;
        if (v->key && ++v->idle > 6) {
            mixer_stop(A->mixer, v->h, 0.15f);
            v->key = 0;
            v->loop = false;
        }
        if (v->flat || v->fixed) continue;
        bool rays = A->env_on->i && budget > 0 && !v->echo;
        if (rays) {
            budget--;
            A->ray_cursor = (i + 1) % MAX_GVOICES;
        }
        EnvParams ep;
        env_world_voice_eval(&v->env, rays, &ep);
        if (!A->env_on->i) {
            float g = ep.gain, pan = ep.pan, fr = ep.front;
            env_params_neutral(&ep);
            ep.gain = g, ep.pan = pan, ep.front = fr, ep.spread = 0;
        }
        if (env_changed(&ep, &v->last)) {
            mixer_env(A->mixer, v->h, &ep);
            v->last = ep;
        }
        // Inaudible (far beyond the distance law, behind walls): virtual until it comes back.
        float audible = v->volume * ep.gain * syn_db(fmaxf(ep.band_db[0], fmaxf(ep.band_db[1], ep.band_db[2])));
        bool virt = audible < 3e-5f;
        if (virt != v->virt) { mixer_virtual(A->mixer, v->h, virt); v->virt = virt; }
        if (virt && !v->loop && platform_time() - v->started > 8.0) mixer_stop(A->mixer, v->h, 0.01f);
        mixer_score(A->mixer, v->h, A->lib.recipe[v->recipe]->priority * fminf(1.0f, audible * 4.0f) + (v->loop ? 0.2f : 0.0f));
    }
    // Death ring: once both targets are back at zero (and its gains have glided down), stop it.
    if (voice_of(A->death_voice) && A->mix.ring_gain <= 1e-4f && A->mix.heart_gain <= 1e-4f) {
        A->death_quiet += dt;
        if (A->death_quiet > 3.0f) { audio_voice_stop(A->death_voice, 0.05f); A->death_voice = 0; }
    }
    EnvReverbParams rev[ENV_ZONES];
    env_world_reverbs(rev);
    for (int z = 0; z < ENV_ZONES; z++) if (platform_time() < A->rev_pin_until[z]) rev[z] = A->rev_pin[z];
    for (int z = 0; z < ENV_ZONES; z++)
        if (reverb_changed(&rev[z], &A->rev_applied[z])) { mixer_set_reverb(A->mixer, z, &rev[z]); A->rev_applied[z] = rev[z]; }
    listener_mix();
    // The death / downed duck rides on the listener filter's death gain (world buses only).
    mixer_set_master(A->mixer, A->master->f);
    mixer_set_bus(A->mixer, SYN_BUS_SFX, A->sfx->f);
    mixer_set_bus(A->mixer, SYN_BUS_VOICE, A->voice->f);
    mixer_set_bus(A->mixer, SYN_BUS_UI, A->ui->f);
    mixer_set_bus(A->mixer, SYN_BUS_AMBIENCE, A->sfx->f * A->ambience_gain);
    mixer_force_degrade(A->mixer, A->degrade->i);
    if (A->ok) {
        ma_engine_set_volume(&A->engine, 1.0f);
        ma_sound_group_set_volume(&A->groups[1], A->master->f * A->music_vol->f * A->mix.world_gain);
    } else if (A->offline->i) {
        // No device: render what a device would have pulled this frame (tests, audio_record).
        A->offline_acc += (double)dt * AUDIO_RATE;
        while (A->offline_acc >= 1.0) {
            int n = A->offline_acc > 4096 ? 4096 : (int)A->offline_acc;
            mixer_render(A->mixer, A->offline_buf, n);
            for (int i = 0; i < n; i++) {
                A->scope[A->scope_pos] = 0.5f * (A->offline_buf[i * 2] + A->offline_buf[i * 2 + 1]);
                A->scope_pos = (A->scope_pos + 1) % SCOPE_LEN;
            }
            A->offline_acc -= n;
        }
    }
    MixStats st = mixer_stats(A->mixer);
    prof_count(PC_AUDIO_VOICES, live);
    prof_count(PC_AUDIO_VOICES_SILENT, st.virtual_count);
    audio_record_poll();
    PROF_END();
}

// ------------------------------------------------------------------ debug / tools

int audio_debug_voices(AudioVoiceDebug *out, int max) {
    if (!A) return 0;
    int n = 0;
    double now = platform_time();
    for (int i = 0; i < MAX_GVOICES && n < max; i++) {
        GVoice *v = &A->voices[i];
        if (!v->used) continue;
        AudioVoiceDebug *d = &out[n++];
        dg_strcpy(d->recipe, sizeof(d->recipe), A->lib.recipe[v->recipe]->name);
        d->pos = v->env.src.pos;
        d->volume = v->volume;
        d->peak = mixer_voice_peak(A->mixer, v->h);
        d->age = (float)(now - v->started);
        d->echo = v->echo;
        d->loop = v->loop;
        d->virt = v->virt;
        d->env = v->flat ? NULL : &v->env.info;
    }
    return n;
}

static void cmd_voices(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!A) return;
    AudioVoiceDebug d[MAX_GVOICES];
    int n = audio_debug_voices(d, MAX_GVOICES);
    MixStats s = mixer_stats(A->mixer);
    log_info("audio: %d voices, mixer %d active (%d virtual), %.0f us / %.0f us buffer, degrade %d, %d stolen, %d dropped", n, s.active, s.virtual_count, s.render_avg_us,
             s.buffer_us, s.degrade, s.stolen, s.dropped_cmds);
    for (int i = 0; i < n; i++) {
        const EnvVoiceInfo *e = d[i].env;
        if (e)
            log_info("  %-16s%s %6.1f m  gain %.3f  lp %5.0f  occl %4.0f %4.0f %4.0f dB (%d walls%s)  zone %d%s  peak %.3f", d[i].recipe, d[i].echo ? "*" : " ", e->dist, e->gain,
                     e->lp_hz, e->occl_db[0], e->occl_db[1], e->occl_db[2], e->walls, e->diffracted ? ", diffracted" : "", e->zone, e->same_room ? " same room" : "", d[i].peak);
        else log_info("  %-16s  2D  vol %.2f  peak %.3f", d[i].recipe, d[i].volume, d[i].peak);
    }
}

static void cmd_synth_play(int argc, const char **argv) {
    if (argc < 2 || !A) { log_info("usage: audio_synth_play <recipe> [key=value ...] [@sample] [2d] [at=x,y,z] [seed=n] [volume=v] [self=m]"); return; }
    AudioParams p = { 0 };
    static char keys[AUDIO_MAX_PARAMS][24];
    AudioEmit e;
    vec3 pos = v3_madd(s_listener, s_fwd, 5.0f);
    audio_emit_init(&e, argv[1], pos);
    for (int i = 2; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (argv[i][0] == '@') e.sample = argv[i] + 1;
        else if (!strcmp(argv[i], "2d")) e.flat = true;
        else if (eq && !strncmp(argv[i], "at", 2)) sscanf(eq + 1, "%f,%f,%f", &e.pos.x, &e.pos.y, &e.pos.z);
        else if (eq && !strncmp(argv[i], "seed", 4)) e.seed = (u32)atoi(eq + 1);
        else if (eq && !strncmp(argv[i], "volume", 6)) e.volume = (float)atof(eq + 1);
        else if (eq && !strncmp(argv[i], "self", 4)) e.self_radius = (float)atof(eq + 1);
        else if (eq && p.n < AUDIO_MAX_PARAMS) {
            size_t n = (size_t)(eq - argv[i]);
            if (n >= sizeof(keys[0])) n = sizeof(keys[0]) - 1;
            memcpy(keys[p.n], argv[i], n);
            keys[p.n][n] = 0;
            audio_param(&p, keys[p.n], (float)atof(eq + 1));
        }
    }
    if (!e.seed) e.seed = dg_hash_u32((u32)(platform_time() * 1000.0));
    AudioVoice v = audio_emit(&e, &p);
    log_info("audio_synth_play: %s %s (%.1f m)", argv[1], v ? "playing" : "FAILED (no recipe / sample)", v3_dist(e.pos, s_listener));
}

static void cmd_env_probe(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    const EnvRoom *r = env_world_room(s_listener, true);
    if (!r || !env_world_query()) { log_info("audio_env_probe: no world"); return; }
    log_info("audio_env_probe: %s, enclosed %.0f%%, box %.2f x %.2f x %.2f m (%.0f m3), absorption %.2f / %.2f / %.2f, RT60 %.2f / %.2f / %.2f s", r->indoor ? "INDOOR" : "OUTDOOR",
             r->enclosed * 100.0f, r->dims.x, r->dims.y, r->dims.z, r->volume, r->absorb[0], r->absorb[1], r->absorb[2], r->rt60[0], r->rt60[1], r->rt60[2]);
    log_info("audio_env_probe: box min %.2f %.2f %.2f max %.2f %.2f %.2f", r->mn.x, r->mn.y, r->mn.z, r->mx.x, r->mx.y, r->mx.z);
    EnvEcho ech[MAX_ECHOES];
    int n = env_world_echoes(s_listener, ech, MAX_ECHOES);
    for (int i = 0; i < n; i++)
        log_info("audio_env_probe: echo %d at %.1f %.1f %.1f: +%.3f s (%.0f m), gain %.3f, lp %.0f Hz, %d rays", i, ech[i].point.x, ech[i].point.y, ech[i].point.z, ech[i].extra_s,
                 ech[i].extra_s * s_sound_speed, ech[i].gain, ech[i].lp_hz, ech[i].rays);
    if (!n) log_info("audio_env_probe: no discrete echoes");
}

// audio_record <file.wav> <seconds>: captures the scope ring after that long (offline or device).
static struct { char path[256]; double until; float secs; bool on; } s_rec;
static void cmd_record(int argc, const char **argv) {
    if (argc < 3) { log_info("usage: audio_record <file.wav> <seconds> (<= %.2f s)", (float)SCOPE_LEN / AUDIO_RATE); return; }
    dg_strcpy(s_rec.path, sizeof(s_rec.path), argv[1]);
    s_rec.secs = fminf((float)atof(argv[2]), (float)SCOPE_LEN / AUDIO_RATE);
    s_rec.until = platform_time() + s_rec.secs;
    s_rec.on = true;
}

static void audio_record_poll(void) {
    if (!s_rec.on || platform_time() < s_rec.until || !A) return;
    s_rec.on = false;
    int n = (int)(s_rec.secs * AUDIO_RATE);
    float *buf = (float *)malloc(sizeof(float) * (size_t)n);
    if (!buf) return;
    n = audio_scope(buf, n);
    size_t bytes = 44 + (size_t)n * 2;
    u8 *w = (u8 *)calloc(1, bytes);
    if (w) {
        u32 rate = AUDIO_RATE, data = (u32)n * 2, riff = (u32)bytes - 8, br = rate * 2;
        memcpy(w, "RIFF", 4); memcpy(w + 4, &riff, 4); memcpy(w + 8, "WAVEfmt ", 8);
        u32 fmt = 16; u16 pcm = 1, ch = 1, align = 2, bits = 16;
        memcpy(w + 16, &fmt, 4); memcpy(w + 20, &pcm, 2); memcpy(w + 22, &ch, 2); memcpy(w + 24, &rate, 4);
        memcpy(w + 28, &br, 4); memcpy(w + 32, &align, 2); memcpy(w + 34, &bits, 2); memcpy(w + 36, "data", 4); memcpy(w + 40, &data, 4);
        for (int i = 0; i < n; i++) {
            float x = buf[i] < -1 ? -1 : (buf[i] > 1 ? 1 : buf[i]);
            i16 s = (i16)(x * 32767.0f);
            memcpy(w + 44 + i * 2, &s, 2);
        }
        log_info("audio_record: %s %s (%d samples)", platform_write_file(s_rec.path, w, bytes) ? "wrote" : "cannot write", s_rec.path, n);
        free(w);
    }
    free(buf);
}
