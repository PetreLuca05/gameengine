// Global wind field (docs/FOLIAGE_WIND.md "Wind field"): what every wind consumer samples (foliage
// WPO, particles, audio, lens droplets, leaf shedding). Built from the replicated weather wind
// (direction, speed, gust amount) and a time that every machine derives from the server tick, so
// wind_at is the same everywhere without replicating anything new:
//
//   gust fronts   1D noise along the wind direction, travelling at the wind speed (a gust visibly
//                 sweeps across a forest), slightly wavy across it; its troughs are the lulls
//   turbulence    low-frequency 3D noise advected with the wind: local direction / strength wobble
//   sources       local, short-lived pushes (explosions, helicopter downwash, vehicle wakes)
//
// The renderer gets a 64 x 64 world-space tile (4 m texels, toroidal around the camera) per frame:
// RGBA16F = (instant wind x, z, settled bend x, z). The settled bend is a damped spring per texel
// (period WIND_SETTLE_PERIOD, slightly under-damped): trees lean into a gust late and rock back
// after it. Pure: no renderer, no world (tests/test_wind.c).
#ifndef DG_WIND_H
#define DG_WIND_H

#include "core/base.h"

#define WIND_TEX 64                  // texels per side
#define WIND_TEXEL 4.0f              // m per texel (window: 256 m)
#define WIND_SOURCES 32
#define WIND_SETTLE_PERIOD 3.2f      // s: natural period of the settled-bend spring
#define WIND_SETTLE_DAMPING 0.45f    // damping ratio (< 1: one visible rebound after a gust)
#define WIND_GUST_WAVELENGTH 90.0f   // m: mean distance between gust fronts
#define WIND_TURB_SCALE 38.0f        // m: turbulence feature size
#define WIND_MAX_SPEED 40.0f         // m/s: wind_at never exceeds this (sources included)

typedef enum WindSourceKind {
    WIND_SRC_BLAST,                  // explosion: a ring blowing outwards, expanding and decaying
    WIND_SRC_DOWNWASH,               // helicopter (stub): steady radial push under it while alive
    WIND_SRC_WAKE,                   // vehicle wake: a push along dir in a capsule behind pos
    WIND_SRC_GUST,                   // a triggered gust: a front band (radius = half width) sweeping along dir from pos at strength / 1.5 m/s
} WindSourceKind;

typedef struct WindSource {
    float pos[3];
    float dir[2];                    // WAKE: unit direction of travel (x, z)
    float radius;                    // m
    float strength;                  // m/s at the centre / ring
    float life, age;                 // s
    u8 kind, used;
} WindSource;

// Replicated input: the weather's wind. speed is the mean (gusts are the field's own).
typedef struct WindBase {
    float dir_deg;                   // WP_WIND_DIR: the direction the wind blows towards (0 = -Z, 90 = +X)
    float speed;                     // WP_WIND, m/s
    float gust;                      // WP_GUST, 0..1
} WindBase;

typedef struct WindField {
    WindBase base;
    float dir[2];                    // unit (x, z) of base.dir_deg
    double time;                     // s: tick time (+ the slow-motion offset)
    u32 seed;
    WindSource src[WIND_SOURCES];
    // Tile (render): window origin in texels (texel (ox, oz) is the window's min corner), its springs.
    i32 ox, oz;
    bool tile_valid;
    float inst[WIND_TEX * WIND_TEX][2];      // instant wind of each texel (x, z), at its world texel centre
    float bend[WIND_TEX * WIND_TEX][2];      // settled bend (m/s equivalent)
    float bend_v[WIND_TEX * WIND_TEX][2];
    u16 texels[WIND_TEX * WIND_TEX * 4];     // RGBA16F upload, indexed by world texel mod WIND_TEX
    float global_bend[2], global_bend_v[2];  // the same spring for the field outside the tile
} WindField;

void wind_init(WindField *f, u32 seed);
// New weather input / time (every frame, before sampling): time = tick * dt (+ offset).
void wind_set(WindField *f, const WindBase *b, double time);

// Instant wind (m/s, world x, y, z) at a world position: gusts, lulls, turbulence and the sources.
// Deterministic for (base, time, seed, sources). y is a small updraft component of the turbulence.
void wind_at(const WindField *f, const float pos[3], float out[3]);
// Gust factor at pos (speed multiplier, ~0.35 in a lull .. ~1.9 in a strong gust at gust 1).
float wind_gust_at(const WindField *f, const float pos[3]);
// Distance along the wind direction of the nearest gust crest ahead of pos (m, >= 0), its factor.
float wind_gust_front(const WindField *f, const float pos[3], float *factor);

// Local sources (not replicated: each machine adds its own from replicated events).
void wind_add_blast(WindField *f, const float pos[3], float radius, float strength);
void wind_add_downwash(WindField *f, const float pos[3], float radius, float strength, float life);
void wind_add_wake(WindField *f, const float pos[3], const float vel[3], float radius);
// A gust front starting at pos (put it upwind), sweeping along the wind for `range` m (tuning / tests).
void wind_add_gust(WindField *f, const float pos[3], float strength, float width, float range);
// Ages the sources (dt s) and drops the dead ones.
void wind_sources_step(WindField *f, float dt);

// Render tile: re-centres the window on cam (toroidal: texels that change world cell restart their
// spring at the instant wind), evaluates the instant wind per texel, steps the springs (dt s) and
// packs `texels`. Window texel (i, j) covers world [(ox + i) * WIND_TEXEL, +WIND_TEXEL).
void wind_tile_update(WindField *f, const float cam[3], float dt);
// Bilinear sample of the packed tile exactly as the shader does (texel centres, toroidal wrap, the
// half-precision values): out = (instant x, z, bend x, z). False outside the window (out = the global
// spring / the instant wind at pos).
bool wind_tile_sample(const WindField *f, float x, float z, float out[4]);

// IEEE half conversion of the packed tile (exposed for the tests).
u16 wind_f2h(float f);
float wind_h2f(u16 h);

#endif
