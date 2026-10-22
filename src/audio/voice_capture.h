// Voice chat microphone (docs/AUDIO_PROCEDURAL.md "Voice chat"): the default capture device through
// miniaudio at 16 kHz mono 16-bit (miniaudio converts), into a lock-free ring the game thread reads.
// Opened on the first push to talk, kept open after that.
#ifndef DG_VOICE_CAPTURE_H
#define DG_VOICE_CAPTURE_H

#include "core/base.h"

bool voice_capture_open(void);          // false: no device (logged once)
void voice_capture_close(void);
bool voice_capture_is_open(void);
int voice_capture_read(i16 *out, int max);   // samples since the last read (drops the oldest past 1 s)

#endif
