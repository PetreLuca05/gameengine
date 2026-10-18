// Procedural audio DSP primitives (docs/AUDIO_PROCEDURAL.md "Nodes"): filters, smoothers, noise,
// band-limited oscillators. Pure C, no allocation, no globals: the synth graph (syn_graph.h) and the
// per-voice environment chain (audio/env/env_chain.h) are built from these, and the tests compare
// them against reference coefficients.
#ifndef DG_SYN_DSP_H
#define DG_SYN_DSP_H

#include "core/base.h"
#include <math.h>

#define SYN_BLOCK 64           // frames per processing block (every node, every chain stage)
#define SYN_PI 3.14159265358979f
#define SYN_TWO_PI 6.28318530717959f

// Denormals: the audio thread runs with flush-to-zero / denormals-are-zero (x86 MXCSR, arm FPCR).
// Returns the previous state for syn_ftz_restore.
u32 syn_ftz_enable(void);
void syn_ftz_restore(u32 prev);

DG_INLINE float syn_db(float db) { return expf(db * 0.11512925f); }   // 10^(db/20)
DG_INLINE float syn_todb(float g) { return g > 1e-9f ? 8.6858896f * logf(g) : -180.0f; }
DG_INLINE float syn_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// ------------------------------------------------------------------ random

// xorshift32: per node / per grain noise; deterministic from the voice seed.
typedef struct SynRng { u32 s; } SynRng;
DG_INLINE SynRng syn_rng(u32 seed) { SynRng r = { dg_hash_u32(seed ^ 0x9E3779B9u) | 1u }; return r; }
DG_INLINE u32 syn_rng_u32(SynRng *r) { u32 x = r->s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return r->s = x; }
DG_INLINE float syn_rng01(SynRng *r) { return (float)(syn_rng_u32(r) >> 8) * (1.0f / 16777216.0f); }
DG_INLINE float syn_rngbi(SynRng *r) { return syn_rng01(r) * 2.0f - 1.0f; }

// ------------------------------------------------------------------ filters

typedef enum SynFilterMode {
    SYN_LP, SYN_HP, SYN_BP, SYN_NOTCH, SYN_PEAK, SYN_LSHELF, SYN_HSHELF, SYN_AP
} SynFilterMode;

// RBJ cookbook biquad, transposed direct form II. q: resonance (0.707 Butterworth), gain_db: peak / shelves.
typedef struct SynBiquad { float b0, b1, b2, a1, a2, z1, z2; } SynBiquad;
void syn_biquad_design(SynBiquad *f, SynFilterMode mode, float hz, float q, float gain_db, float sr);
void syn_biquad_run(SynBiquad *f, const float *in, float *out, int n);
// Magnitude response at hz (tests, the editor's curve).
float syn_biquad_mag(const SynBiquad *f, float hz, float sr);

// One-pole low-pass: y += a (x - y), a = 1 - exp(-2 pi fc / sr).
typedef struct SynOnePole { float a, z; } SynOnePole;
DG_INLINE float syn_onepole_coef(float hz, float sr) {
    float x = hz / sr;
    if (x >= 0.49f) return 1.0f;
    return 1.0f - expf(-SYN_TWO_PI * x);
}
DG_INLINE float syn_onepole_lp(SynOnePole *f, float x) { f->z += f->a * (x - f->z); return f->z; }

// State-variable filter (Simper / Cytomic TPT): stable under fast modulation.
typedef struct SynSvf { float g, k, a1, a2, a3, ic1, ic2; } SynSvf;
void syn_svf_design(SynSvf *f, float hz, float q, float sr);
// mode: SYN_LP / SYN_HP / SYN_BP / SYN_NOTCH
void syn_svf_run(SynSvf *f, SynFilterMode mode, const float *in, float *out, int n);

// ------------------------------------------------------------------ smoothing

// Parameter smoother: a one-pole glide toward the target at block rate, ramped linearly across the
// block so no step lands inside it (no zipper noise). syn_smooth_block returns the block's start
// and end values.
typedef struct SynSmooth { float cur, target, coef; } SynSmooth;
void syn_smooth_init(SynSmooth *s, float value, float tau_s, float sr);
void syn_smooth_block(SynSmooth *s, float *v0, float *v1);
// out[i] = in[i] * linear ramp v0 -> v1
void syn_ramp_mul(const float *in, float *out, int n, float v0, float v1);
void syn_ramp_mac(const float *in, float *out, int n, float v0, float v1);   // out += in * ramp

// ------------------------------------------------------------------ noise

typedef enum SynNoiseColor { SYN_WHITE, SYN_PINK, SYN_BROWN, SYN_VELVET } SynNoiseColor;
typedef struct SynNoise { SynRng rng; float b[7]; float brown; float velvet_pos; } SynNoise;
// density: velvet impulses per second (others ignore it)
void syn_noise_run(SynNoise *s, SynNoiseColor c, float *out, int n, float amp, float density, float sr);

// ------------------------------------------------------------------ oscillators

typedef enum SynWave { SYN_SINE, SYN_SAW, SYN_SQUARE, SYN_TRI } SynWave;
// PolyBLEP residual for a discontinuity at phase t (0..1) with increment dt.
DG_INLINE float syn_polyblep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
DG_INLINE float syn_fast_sin(float ph) {   // ph in cycles; ~1e-4 abs error
    ph -= (float)(int)ph;
    if (ph < 0) ph += 1.0f;
    float x = ph * SYN_TWO_PI - SYN_PI;   // -pi..pi, sin(x + pi) = -sin(x)
    float x2 = x * x;
    float s = x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * (-0.00019840874f + x2 * (2.7525562e-06f - x2 * 2.3889859e-08f)))));
    return -s;
}

#endif
