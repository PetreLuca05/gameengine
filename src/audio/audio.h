// Audio (docs/AUDIO_PROCEDURAL.md): every effect, voice and UI sound is a voice of the DG mixer
// (audio/mixer.h) - synthesized from a recipe (assets/audio/recipes), a sample, or both - shaped per
// voice by the environment chain (distance, air, occlusion, rooms, echoes, weather, listener state).
// miniaudio runs the device and the music stream. Samples are assets/sounds/<name>.wav (or .ogg).
#ifndef DG_AUDIO_H
#define DG_AUDIO_H

#include "core/base.h"
#include "core/dgmath.h"

typedef enum SoundCategory { SND_SFX, SND_MUSIC, SND_VOICE, SND_UI } SoundCategory;

bool audio_init(void);
void audio_shutdown(void);
void audio_update(vec3 listener_pos, vec3 listener_fwd, vec3 listener_up);
// A sound by name: assets/audio/events.json maps names to recipes (with fixed inputs), anything else
// plays the sample of that name. Positional sounds arrive after their travel time (d / c).
void audio_play(const char *name, vec3 pos, float volume, SoundCategory cat);
void audio_play_2d(const char *name, float volume, SoundCategory cat);
// Positional one-shot with a fixed delay (the ballistic crack's arrival; 0 = the travel time), an
// extra top cut, pitch and its own distance law. Zero fields keep the recipe's / audio_play's defaults.
typedef struct AudioFx {
    float delay;          // s before it starts (0: the propagation delay)
    float lowpass_hz;     // 0 = off (a cap over the environment's own air absorption)
    float pitch;          // 0 = 1
    float min_dist, max_dist, rolloff;
} AudioFx;
void audio_play_fx(const char *name, vec3 pos, float volume, SoundCategory cat, const AudioFx *fx);
// The listener of the last audio_update and the speed of sound (ballistics sets it from the
// weather's temperature): sound travel time from a point.
vec3 audio_listener_pos(void);
bool audio_has_sound(const char *name);   // assets/sounds/<name>.wav|.ogg exists
void audio_set_sound_speed(float c);
float audio_sound_speed(void);
float audio_travel_delay(vec3 pos);
void audio_play_music(const char *name);
// Ambient loops (rain, wind ...): started on the first call with volume > 0, faded to the volume,
// stopped at 0. Non-spatial, ambience bus.
void audio_loop_set(const char *name, float volume);
// Scales every ambient loop (the hand pose editor's frozen world: 0).
void audio_set_ambience_gain(float g);
// Positional loop (key: one per source, e.g. an entity id): refresh every frame, else it fades out.
void audio_loop3d(u32 key, const char *name, vec3 pos, float volume, float pitch);
bool audio_available(void);   // an output device is running

// ------------------------------------------------------------------ procedural voices

#define AUDIO_MAX_PARAMS 16
// An event's physical values by name (caliber, energy, material, speed, rpm, wind ...): the recipe's
// "inputs"; names it does not declare are ignored.
typedef struct AudioParams {
    int n;
    const char *key[AUDIO_MAX_PARAMS];
    float val[AUDIO_MAX_PARAMS];
} AudioParams;
void audio_param(AudioParams *p, const char *key, float value);

typedef u32 AudioVoice;   // 0 = none

typedef struct AudioEmit {
    const char *recipe;   // assets/audio/recipes/<name>.json (or an events.json name, or a sample)
    vec3 pos;
    vec3 fwd;             // the source's forward (directivity); zero: omnidirectional
    vec3 vel;             // m/s (doppler recipes)
    float volume;         // linear
    u32 seed;             // per-event variation (replicated events carry it: same sound on every machine)
    float delay;          // < 0: the propagation delay d / c; >= 0: exactly this (ballistic crack arrival)
    const char *sample;   // the "@" layers' sample (a weapon's sound_fire)
    float lp_cap;         // Hz, 0 off: an extra top cut (legacy AudioFx.lowpass_hz)
    float pitch;          // 0 = 1: multiplies sample layers' pitch
    float min_dist, max_dist, rolloff;   // 0: the recipe's distance law
    float self_radius;    // m: the emitter's own geometry around it (a vehicle's body), not occluding
    bool flat;            // force 2D (UI)
    bool no_echo;
    const struct EnvParams *env;   // fixed chain parameters (audio_env_preview), no world updates
    u8 bus;               // 0: the recipe's; else SynBus + 1
} AudioEmit;
void audio_emit_init(AudioEmit *e, const char *recipe, vec3 pos);
AudioVoice audio_emit(const AudioEmit *e, const AudioParams *p);
void audio_voice_move(AudioVoice v, vec3 pos, vec3 fwd, vec3 vel);
void audio_voice_params(AudioVoice v, const AudioParams *p);
void audio_voice_volume(AudioVoice v, float volume);
void audio_voice_stop(AudioVoice v, float fade_s);
bool audio_voice_alive(AudioVoice v);
// A procedural loop by key (engines, tires, wind, rain on a roof): refresh every frame with the
// current values; not refreshed for a few frames, it fades out. Returns its voice.
AudioVoice audio_loop_recipe(u32 key, const AudioEmit *e, const AudioParams *p);
bool audio_has_recipe(const char *name);
// Recipes + patches + materials + events again from disk (the editor's save, a JSON edit).
void audio_reload(void);
// audio_env_preview: holds a zone's late reverb for a few seconds against the world's.
struct EnvReverbParams;
void audio_preview_reverb(int zone, const struct EnvReverbParams *p, float hold_s);

// Listener state for the environment chain (game/game_audio.c, each frame before audio_update).
typedef struct AudioListenerState {
    bool underwater, in_vehicle, ads, helmet;
    float health;          // 0..1
    float temp_c, humidity, rain, wind, snow, exposure;
} AudioListenerState;
void audio_set_listener_state(const AudioListenerState *s);

// Death / downed mix snapshot (docs/REVIVE.md "Death sequence"): the world buses (effects, music,
// voice; not the UI) through a low-pass at lpf_hz and a gain, plus the procedural ear ring and
// heartbeat (the death_ring recipe). Neutral: lpf_hz >= 20000, world_gain 1, ring / heart 0.
typedef struct AudioDeathMix {
    float lpf_hz, world_gain;
    float ring_gain, ring_freq, warble_hz, warble_depth;
    float heart_gain, heart_bpm;
} AudioDeathMix;
void audio_set_death_mix(const AudioDeathMix *m);
const AudioDeathMix *audio_death_mix(void);   // the last set (tests, debug)

// Debug / tools (audio_debug, the recipe editor, tests).
typedef struct AudioVoiceDebug {
    char recipe[32];
    vec3 pos;
    float volume, peak, age;
    bool echo, loop, virt;
    const struct EnvVoiceInfo *env;
} AudioVoiceDebug;
int audio_debug_voices(AudioVoiceDebug *out, int max);
struct Mixer *audio_mixer(void);
struct SynLibrary *audio_library(void);
// The mixer's output of the last audio callback / offline pump into a mono ring (editor scope).
int audio_scope(float *out, int max);

#endif
