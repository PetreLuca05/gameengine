// Voice chat codec (docs/AUDIO_PROCEDURAL.md "Voice chat"): 16 kHz mono, 20 ms frames, IMA ADPCM
// (4 bits a sample, 64 kbit/s). Every frame carries its own decoder state (predictor + step index),
// so a lost packet costs only its own audio. voice_upsample3: 16 kHz -> the mixer's 48 kHz.
// Pure C: tests/test_voice.c.
#ifndef DG_VOICE_CODEC_H
#define DG_VOICE_CODEC_H

#include "core/base.h"

#define VOICE_RATE 16000
#define VOICE_FRAME 320                          // samples per frame (20 ms)
#define VOICE_FRAME_BYTES (3 + VOICE_FRAME / 2)  // i16 predictor, u8 step index, 2 samples a byte
#define VOICE_PACKET_FRAMES 2                    // frames per network packet (40 ms)

typedef struct VoiceEncoder { int pred, index; } VoiceEncoder;

// pcm[VOICE_FRAME] -> out[VOICE_FRAME_BYTES]; the encoder state runs on from frame to frame.
void voice_encode(VoiceEncoder *e, const i16 *pcm, u8 *out);
// in[VOICE_FRAME_BYTES] -> pcm[VOICE_FRAME] (self-contained).
void voice_decode(const u8 *in, i16 *pcm);

typedef struct VoiceUpsample { float last; } VoiceUpsample;
// n samples at 16 kHz -> 3 n at 48 kHz in -1..1 (linear, continuous across calls).
void voice_upsample3(VoiceUpsample *u, const i16 *in, int n, float *out);

#endif
