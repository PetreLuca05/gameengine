// Environment model (docs/AUDIO_PROCEDURAL.md "Environment model"): the physics that turns where a
// sound is, what is around it and between it and the ear, and the weather into EnvParams. Pure:
// geometry arrives as ray results (env_world.c casts them), so every formula is unit tested.
#ifndef DG_ENV_MODEL_H
#define DG_ENV_MODEL_H

#include "core/dgmath.h"
#include "env_chain.h"

#define ENV_SOUND_SPEED 343.0f
#define ENV_BAND_HZ_LOW 125.0f   // band centres for absorption per band
#define ENV_BAND_HZ_MID 1000.0f
#define ENV_BAND_HZ_HIGH 4000.0f

typedef struct EnvWeather {
    float temp_c;                // C
    float humidity;              // relative, %
    float rain;                  // 0..1 intensity at the listener
    float wind;                  // m/s at the listener
    float snow;                  // 0..1 snow cover (damps outdoor reflections)
    float exposure;              // 0 under a roof .. 1 open sky (listener)
} EnvWeather;

float env_sound_speed(float temp_c);   // 331.3 sqrt(1 + T / 273.15)
// ISO 9613-1 atmospheric absorption coefficient, dB per metre, at 1 atm.
float env_air_alpha(float hz, float temp_c, float rh);
// Air absorption over dist: per-band loss (dB, <= 0, at the band centres) and the chain's top cut:
// where the loss exceeds the high band's own by 6 dB (the steeper fall above 4 kHz).
void env_air(float dist, float temp_c, float rh, float band_db[ENV_BANDS], float *lp_hz);
// Inverse distance law as miniaudio's (the samples keep their loudness): min / (min + rolloff (d - min)).
float env_distance_gain(float d, float min_dist, float max_dist, float rolloff);
// Source directivity. cos_axis = cos(angle between the source forward and the listener); gain in dB and
// the top cut behind it.
void env_directivity(float cos_axis, float front_db, float rear_db, float rear_lp, float *gain_db, float *lp_hz);
// Ground effect: the interference dip of a source and listener near soft ground, on the mid band (dB <= 0).
float env_ground_db(float dist, float softness);
// The weather's noise floor at the ear (linear, the level of a typical sound at 1) and the masking
// gain for a voice of that level: sounds well above the floor are untouched, sounds near it sink.
float env_noise_floor(const EnvWeather *w);
float env_masking_gain(float level, float floor);

// ------------------------------------------------------------------ walls

typedef struct EnvWall { int material; float thickness; } EnvWall;
// Transmission loss through walls (dB <= 0 per band; material table, mass law).
void env_transmission(const EnvWall *walls, int n, float out_db[ENV_BANDS]);
// Diffraction around an edge: detour = extra path length over the straight line (m), dB <= 0 per band
// (Maekawa: N = 2 detour / lambda, ~ 10 log10(3 + 20 N)).
void env_diffraction(float detour, float out_db[ENV_BANDS]);

// ------------------------------------------------------------------ rooms

#define ENV_PROBE_RAYS 12
extern const vec3 env_probe_dir[ENV_PROBE_RAYS];   // 0..5: +x -x +y -y +z -z, then 6 diagonals

typedef struct EnvProbeHit { bool hit; float dist; int material; } EnvProbeHit;

typedef struct EnvRoom {
    bool indoor;
    float enclosed;              // fraction of the probe rays that hit within the probe range
    vec3 mn, mx;                 // the box fitted to the axis rays
    vec3 dims;
    float volume, surface;
    float absorb[ENV_BANDS];     // mean absorption (open directions count as 1)
    float rt60[ENV_BANDS];       // Eyring
    int wall_material[6];        // per axis wall (-1 open)
} EnvRoom;

// Probe from origin with the rays in env_probe_dir (max_dist: their length).
void env_room_estimate(vec3 origin, const EnvProbeHit hits[ENV_PROBE_RAYS], float max_dist, EnvRoom *out);
bool env_room_contains(const EnvRoom *r, vec3 p, float margin);
// Early reflections of a source in the room: the six first-order image sources. Taps carry the
// extra delay over the direct path, the relative gain (wall reflection x spreading), a top cut and
// the pan seen from the listener. Returns the tap count.
int env_room_taps(const EnvRoom *r, vec3 src, vec3 lis, vec3 lis_right, float c, EnvTap *out, int max);
// Late reverb for a room estimate (indoor) or the outdoor diffuse field (forest density 0..1, urban 0..1).
void env_room_reverb(const EnvRoom *r, EnvReverbParams *out);
void env_outdoor_reverb(float forest, float urban, float snow, EnvReverbParams *out);

// ------------------------------------------------------------------ outdoor echoes

typedef struct EnvEchoRay { vec3 dir; bool hit; float dist; int material; } EnvEchoRay;
typedef struct EnvEcho { vec3 point; float extra_s, gain, lp_hz; int rays, material; } EnvEcho;

// Clusters the source's echo scan (rays around it, ordered by azimuth) into reflectors and returns
// the strongest discrete echoes: extra delay (|SR| + |RL| - |SL|) / c, gain (reflectivity x spreading
// x reflector size), air absorption over the longer path. Echoes closer than 25 ms to the direct
// sound merge into it and are skipped.
int env_outdoor_echoes(vec3 src, vec3 lis, const EnvEchoRay *rays, int nrays, float c, float snow,
                       float temp_c, float rh, EnvEcho *out, int max);

// Listener-relative pan (-1 left .. 1 right) and front (cos of the azimuth) of a point.
void env_pan(vec3 lis, vec3 lis_right, vec3 lis_fwd, vec3 p, float *pan, float *front);

#endif
