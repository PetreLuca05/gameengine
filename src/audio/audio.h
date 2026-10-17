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
// Positional one-shot with travel delay (gunfire at the speed of sound), a low-pass (air
// absorption), pitch and its own distance law. Zero fields keep audio_play's defaults.
typedef struct AudioFx {
    float delay;          // s before it starts
    float lowpass_hz;     // 0 = off
    float pitch;          // 0 = 1
    float min_dist, max_dist, rolloff;
} AudioFx;
void audio_play_fx(const char *name, vec3 pos, float volume, SoundCategory cat, const AudioFx *fx);
// The listener of the last audio_update and the speed of sound (ballistics sets it from the
// weather's temperature): sound travel time from a point.
vec3 audio_listener_pos(void);
bool audio_has_sound(const char *name);   // assets/sounds/<name>.wav|.ogg exists
void audio_set_sound_speed(float c);
float audio_travel_delay(vec3 pos);
void audio_play_music(const char *name);
// Ambient loops (rain, wind ...): started on the first call with volume > 0, faded to the volume,
// stopped at 0. Non-spatial, effects group.
void audio_loop_set(const char *name, float volume);
// Scales every ambient loop (the hand pose editor's frozen world: 0).
void audio_set_ambience_gain(float g);
// Positional loop (key: one per source, e.g. an entity id): refresh every frame, else it fades out.
void audio_loop3d(u32 key, const char *name, vec3 pos, float volume, float pitch);
bool audio_available(void);

#endif
