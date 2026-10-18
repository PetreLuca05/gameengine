// Per-voice environment chain (docs/AUDIO_PROCEDURAL.md "Environment chain"): what happens to a sound
// between its source and the ear, as DSP. Mono in, stereo out, plus sends to the shared late reverbs.
//
//   [doppler delay] -> 2x one-pole LP (air absorption, off-axis, transmission) -> 3 bands (< 250,
//   250..2k, > 2k: occlusion, ground, air per band) -> gain -> early-reflection ring
//      -> direct: spherical-head model (Brown-Duda shelf per ear, Woodworth ITD), pinna darkening behind
//      -> taps (<= ENV_MAX_TAPS): delay, gain, one-pole LP, pan (image sources of the room)
//      -> sends to the zone reverbs (listener room, remote room, outdoor diffuse)
//
// Every parameter is a target smoothed per block and ramped inside it (no zipper noise). The chain
// never allocates: the mixer owns the memory (the doppler line comes from a pool).
#ifndef DG_ENV_CHAIN_H
#define DG_ENV_CHAIN_H

#include "audio/synth/syn_dsp.h"

#define ENV_MAX_TAPS 6
#define ENV_ZONES 3              // 0 listener room, 1 remote room, 2 outdoor diffuse (forest, urban)
#define ENV_RING 8192            // early-reflection / ITD ring (170 ms at 48 kHz), power of two
#define ENV_BANDS 3

typedef struct EnvTap { float delay_s, gain, lp_hz, pan; } EnvTap;   // delay after the direct sound; pan -1 left .. 1 right

typedef struct EnvParams {
    float gain;                  // linear: distance law x directivity x masking x listener state
    float lp_hz;                 // the top cut (air, off-axis, transmission): two one-poles
    float band_db[ENV_BANDS];    // <= 0: occlusion / transmission / ground / air per band
    float delay_s;               // doppler voices: the current propagation delay (others start late instead)
    float pan;                   // -1 left .. 1 right (sin of the azimuth)
    float front;                 // cos of the azimuth: < 0 behind (a little darker)
    float spread;                // 0 point .. 1 everywhere (2D sounds, very close sources)
    float send[ENV_ZONES];
    int ntaps;
    EnvTap tap[ENV_MAX_TAPS];
} EnvParams;

void env_params_neutral(EnvParams *p);   // 2D, unity, no filtering

typedef struct EnvTapState { SynSmooth gain, delay, pan; float lp_coef, lp; } EnvTapState;

typedef struct EnvChain {
    float sr;
    EnvParams target;
    SynSmooth gain, lp, band[ENV_BANDS], pan, front, spread, send[ENV_ZONES], delay;
    float lp1, lp2, x250, x2k, shadow_l, shadow_r, head_x[2], er_lp[2];
    int ntaps, tap_limit;        // tap_limit: the mixer's degradation (fewer reflections under load)
    EnvTapState tap[ENV_MAX_TAPS];
    float ring[ENV_RING];
    int ring_pos;
    float *dop;                  // doppler line (NULL: none)
    int dop_len, dop_pos;
} EnvChain;

// dop / dop_len: a doppler line (power of two) or NULL. p: the starting parameters (snapped, no glide).
void env_chain_init(EnvChain *c, float sr, float *dop, int dop_len, const EnvParams *p);
void env_chain_set(EnvChain *c, const EnvParams *p);
// Adds the processed block into out_l / out_r and the sends into send[z] (any may be NULL).
void env_chain_process(EnvChain *c, const float *in, int n, float *out_l, float *out_r, float *const send[ENV_ZONES]);

// ------------------------------------------------------------------ late reverb

#define ENV_FDN 8
#define ENV_FDN_MAX 9000         // samples per line at size 2.5, 48 kHz

typedef struct EnvReverbParams {
    float rt60_low, rt60_high;   // s
    float size;                  // 0.3 (closet) .. 2.5 (hall): scales the line lengths
    float predelay_ms;
    float wet;                   // output gain
} EnvReverbParams;

typedef struct EnvReverb {
    float sr;
    EnvReverbParams target;
    SynSmooth rt_lo, rt_hi, size, wet;
    float line[ENV_FDN][ENV_FDN_MAX];
    int pos[ENV_FDN];
    float damp[ENV_FDN];
    float pre[4096];
    int pre_pos;
    float ap_buf[2][512];
    int ap_pos[2];
} EnvReverb;

void env_reverb_init(EnvReverb *r, float sr, const EnvReverbParams *p);
void env_reverb_set(EnvReverb *r, const EnvReverbParams *p);
// in: the summed sends (mono); adds the wet stereo output.
void env_reverb_process(EnvReverb *r, const float *in, int n, float *out_l, float *out_r);

#endif
