// Audio playback via miniaudio: sound groups (master/music/sfx/voice), 3D one-shots, music loop.
// Sounds are loaded by name from assets/sounds/<name>.wav (or .ogg).
#ifndef DG_AUDIO_H
#define DG_AUDIO_H

#include "core/base.h"
#include "core/dgmath.h"

typedef enum SoundCategory { SND_SFX, SND_MUSIC, SND_VOICE, SND_UI } SoundCategory;

bool audio_init(void);
void audio_shutdown(void);
void audio_update(vec3 listener_pos, vec3 listener_fwd, vec3 listener_up);
void audio_play(const char *name, vec3 pos, float volume, SoundCategory cat);
void audio_play_2d(const char *name, float volume, SoundCategory cat);
void audio_play_music(const char *name);
// Ambient loops (rain, wind ...): started on the first call with volume > 0, faded to the volume,
// stopped at 0. Non-spatial, effects group.
void audio_loop_set(const char *name, float volume);
// Scales every ambient loop (the hand pose editor's frozen world: 0).
void audio_set_ambience_gain(float g);
// Positional loop (key: one per source, e.g. an entity id): refresh every frame, else it fades out.
void audio_loop3d(u32 key, const char *name, vec3 pos, float volume, float pitch);
bool audio_available(void);

// Death / downed mix snapshot (docs/REVIVE.md "Death sequence"): the world buses (effects, music,
// voice; not the UI) through a low-pass at lpf_hz and a gain, plus the procedural ear ring and
// heartbeat (audio/death_synth.h). Neutral: lpf_hz >= 20000, world_gain 1, ring / heart 0.
typedef struct AudioDeathMix {
    float lpf_hz, world_gain;
    float ring_gain, ring_freq, warble_hz, warble_depth;
    float heart_gain, heart_bpm;
} AudioDeathMix;
void audio_set_death_mix(const AudioDeathMix *m);
const AudioDeathMix *audio_death_mix(void);   // the last set (tests, debug)

#endif
