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
bool audio_available(void);

#endif
