// Procedural death / downed voice (docs/REVIVE.md "Death sequence"): a high tinnitus tone with a
// slight warble and a slowing heartbeat. Pure DSP (no miniaudio): the audio thread renders it from
// targets the game sets per frame; every gain is smoothed per sample, so target jumps never pop.
#ifndef DG_DEATH_SYNTH_H
#define DG_DEATH_SYNTH_H

#include "core/base.h"

typedef struct DeathSynthParams {
    float ring_gain;            // 0..1 target
    float ring_freq;            // Hz (~3-4 kHz)
    float warble_hz, warble_depth;   // pitch wobble rate (Hz) and depth (fraction of ring_freq)
    float heart_gain;           // 0..1 target
    float heart_bpm;
} DeathSynthParams;

typedef struct DeathSynth {
    DeathSynthParams target;
    float ring_gain, heart_gain;     // smoothed
    double ring_phase, warble_phase; // radians
    double beat_t;                   // s since the last beat
    float smooth_tau;                // s (gain smoothing time constant)
} DeathSynth;

void death_synth_init(DeathSynth *s);
void death_synth_set(DeathSynth *s, const DeathSynthParams *p);   // any thread: plain floats, read per block
// Mixes (adds) frames of interleaved audio into out (channels 1 or 2) at sample_rate.
void death_synth_render(DeathSynth *s, float *out, int frames, int channels, float sample_rate);
bool death_synth_silent(const DeathSynth *s);   // nothing audible and nothing targeted

#endif
