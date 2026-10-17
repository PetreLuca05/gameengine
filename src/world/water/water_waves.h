// Ocean swell model (docs/WATER.md "Waves"): the wave band every system agrees on. A fixed set of
// Gerstner components (wave vectors and phases from the seed, never from the wind) whose amplitudes
// come from a fetch-limited JONSWAP spectrum of the replicated wind, so a wind change re-weights the
// waves smoothly and never moves a crest. Only the WAVE_ACTIVE largest components are evaluated.
//
// Determinism: the evaluation uses only + - * / sqrt and the polynomial wsin / wcos / wexp / wlog
// below (no libm), in a file built without FP contraction (CMake: -ffp-contract=off, /fp:precise),
// so host, clients and every process compute bit-identical heights for the same (seed, wind, tick).
// Every angular frequency is a multiple of 2 pi / WAVE_REPEAT, so the time wraps exactly at
// WAVE_REPEAT seconds and stays small in float. shaders/water/water_waves.glsl is the GPU twin
// (same components, same polynomials); the FFT detail band on the GPU only holds wavelengths below
// WAVE_MIN_LEN, so CPU and GPU agree to within that band's few centimetres.
//
// Pure: no engine, no renderer (tests/test_water.c).
#ifndef DG_WATER_WAVES_H
#define DG_WATER_WAVES_H

#include "core/base.h"

#define WAVE_FREQS 24                 // log-spaced frequencies of the fixed set
#define WAVE_DIRS 4                   // directions per frequency (90 degrees apart, seeded offset)
#define WAVE_MAX (WAVE_FREQS * WAVE_DIRS)
#define WAVE_ACTIVE 32                // components evaluated (largest amplitude first)
#define WAVE_REPEAT 600               // s: time period of the whole field (every omega is a multiple of 2 pi / it)
#define WAVE_MIN_LEN 6.0f             // m: shortest swell wavelength (the FFT detail band is below it)
#define WAVE_MAX_LEN 180.0f           // m: longest
#define WAVE_GRAVITY 9.81f

typedef struct WaveParams {
    float wind;                       // m/s at 10 m (the weather's mean wind)
    float wind_dir_deg;               // direction the wind blows towards (0 = -Z, 90 = +X), as WindBase
    float fetch_km;                   // fetch: distance the wind has blown over open water (default 30)
    float scale;                      // amplitude multiplier (the body's wave_scale; 1 = physical)
    float choppiness;                 // 0..1: horizontal (Gerstner) displacement, clamped to avoid loops
    float spread;                     // directional spreading exponent s of cos^2s (default 6)
} WaveParams;

typedef struct WaveComp {             // fixed (seeded)
    float dx, dz;                     // unit direction of travel
    float k, omega, phase;            // wavenumber (1/m), angular frequency (rad/s, quantized), phase (rad)
    float domega;                     // frequency bin width (rad/s)
} WaveComp;

typedef struct WaveActive {           // evaluated set (also the GPU upload: 2 vec4 each)
    float dx, dz, k, omega;
    float amp, qa, phase, pad;        // vertical amplitude (m), horizontal amplitude (m), phase
} WaveActive;

typedef struct WaveModel {
    u32 seed;
    WaveComp comp[WAVE_MAX];
    WaveParams p;
    bool params_valid;
    int active_n;
    WaveActive act[WAVE_ACTIVE];
    float hs;                         // significant wave height of the evaluated set (m): 4 sqrt(sum a^2 / 2)
    float peak_len;                   // m: wavelength of the spectral peak
    float steepness;                  // sum k * qa of the evaluated set (< 1: no loops)
} WaveModel;

typedef struct WaveSample {
    float height;                     // surface height above the still level (m)
    float normal[3];
    float vel[3];                     // orbital velocity of the surface water (m/s)
    float jacobian;                   // horizontal compression (< 0.3: folding crest, foam)
} WaveSample;

WaveParams wave_params_default(void);
void waves_init(WaveModel *m, u32 seed);
// Recomputes the amplitudes (and the active set) when p differs from the current parameters.
void waves_set_params(WaveModel *m, const WaveParams *p);

// Wrapped wave time (s) for a server tick + fraction at hz ticks per second: exact for any tick.
float waves_time(u32 tick, float frac, int hz);

// Gerstner displacement of the undisplaced surface point (x0, z0) at time t: out = (dx, dy, dz).
void waves_displace(const WaveModel *m, float x0, float z0, float t, float out[3]);
// Surface height above the still level at world (x, z) (the horizontal displacement inverted with
// `iters` fixed-point steps; 2 is the default everywhere, physics and GPU).
float waves_height(const WaveModel *m, float x, float z, float t, int iters);
// Height, normal, orbital velocity and jacobian at world (x, z).
void waves_sample(const WaveModel *m, float x, float z, float t, WaveSample *out);
// A 5-point patch for buoyancy: the (approximately) inverted centre (x, z) and the surface points
// whose undisplaced positions are +-e along x and z from it (angle addition: 4 sincos per component
// instead of 15).
// Index 0 centre, 1 +x, 2 -x, 3 +z, 4 -z. px / pz are the displaced (true) world positions.
void waves_patch(const WaveModel *m, float x, float z, float t, float e, float px[5], float pz[5], float h[5], float vel[5][3]);

// Deterministic polynomial math (the GPU twin uses the same coefficients).
float wsin(float x);
float wcos(float x);
void wsincos(float x, float *s, float *c);
float wexp(float x);
float wlog(float x);

#endif
