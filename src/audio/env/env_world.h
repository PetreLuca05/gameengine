// Environment, world side (docs/AUDIO_PROCEDURAL.md "Environment model"): turns the world around each
// playing voice into its EnvParams, amortized per frame. Geometry comes through EnvWorldQuery
// callbacks (src/game/game_audio.c casts the physics rays; the tests pass a box world):
//   - occlusion: a ray listener -> source, a back-cast for each wall's thickness (<= 3 walls), four
//     offset rays for a diffraction path; the cheaper of transmission and diffraction wins. A few
//     voices per frame, nearest / loudest first; every voice once on emit.
//   - rooms: 12-ray probes cached per 2 m cell (listener and sources): indoor / outdoor, the box,
//     absorption, RT60 -> early-reflection taps (same room) and the zone reverbs.
//   - outdoor echoes: a 24-ray horizontal scan around echo sources (cached per 8 m cell) clustered
//     into reflectors -> child echo voices (env_world_echoes).
//   - distance law, propagation delay, air absorption (humidity / temperature), directivity, ground
//     effect, weather masking, pan, every frame for every voice (math only).
#ifndef DG_ENV_WORLD_H
#define DG_ENV_WORLD_H

#include "env_model.h"

typedef struct EnvRayHit { bool hit; float dist; vec3 point, normal; int material; } EnvRayHit;

typedef struct EnvWorldQuery {
    bool (*raycast)(vec3 from, vec3 dir_len, EnvRayHit *out, void *user);
    float (*foliage)(vec3 p, float radius, void *user);   // 0..1 vegetation density (NULL: none)
    void *user;
} EnvWorldQuery;

typedef struct EnvListener {
    vec3 pos, fwd, right, up;
    bool underwater, in_vehicle, ads, helmet;
    float health;                // 0..1
    EnvWeather weather;
    float sound_speed;           // m/s
} EnvListener;

typedef struct EnvSource {
    vec3 pos, fwd, vel;          // fwd zero: omnidirectional
    float volume;
    float min_dist, max_dist, rolloff;
    float front_db, rear_db, rear_lp;
    float reverb;                // send scale
    bool spatial, doppler, ambience, echo_child;
    int max_taps;
    float self_radius;           // m: geometry this close to the source is its own (the surface it lies on, a car's body)
} EnvSource;

#define ENV_MAX_WALLS 3

typedef struct EnvVoiceInfo {   // the last evaluation (audio_debug, tests)
    float dist, delay, gain, lp_hz;
    float band_db[ENV_BANDS], occl_db[ENV_BANDS];
    int walls, wall_material[ENV_MAX_WALLS];
    float wall_thick[ENV_MAX_WALLS];
    bool diffracted, same_room, src_indoor;
    int zone;
    float mask;
} EnvVoiceInfo;

typedef struct EnvVoiceState {
    EnvSource src;
    float occl_db[ENV_BANDS];    // the current occlusion (transmission or diffraction)
    int occl_frames;             // frames since its rays were cast (-1: never)
    EnvVoiceInfo info;
    vec3 occl_path;              // diffraction point (debug)
    float ground_y;              // the ground under the source (ground reflection), NAN unknown
    int ground_mat;
} EnvVoiceState;

typedef struct EnvWorldStats { int rays, probes, occl_updates, echo_scans; } EnvWorldStats;

void env_world_init(void);
void env_world_set_query(const EnvWorldQuery *q);
const EnvWorldQuery *env_world_query(void);
void env_world_set_listener(const EnvListener *l);
const EnvListener *env_world_listener(void);
void env_world_begin_frame(float dt);          // ages the caches, the per-frame ray budget
// A new voice: casts its occlusion rays now (the first block must already sound right).
void env_world_voice_start(EnvVoiceState *v, const EnvSource *s);
// Every frame: refreshes the voice (rays only when it is its turn) and writes its chain parameters.
// Returns the arrival delay (s) of a non-doppler voice (its start delay at emit).
float env_world_voice_eval(EnvVoiceState *v, bool rays, EnvParams *out);
// The listener's room and the zone reverbs (once per frame, after the voices).
void env_world_reverbs(EnvReverbParams out[ENV_ZONES]);
// Echo children of an outdoor impulsive source: the strongest discrete reflections (<= max).
int env_world_echoes(vec3 src, EnvEcho *out, int max);
// Room probe at p (cached per 2 m cell); fresh: ignore the cache.
const EnvRoom *env_world_room(vec3 p, bool fresh);
// Debug: the probe rays of the last listener probe and the last echo scan.
int env_world_debug_probe(vec3 *origin, vec3 *ends, bool *hits, int max);
int env_world_debug_scan(vec3 *origin, vec3 *ends, bool *hits, int max);
void env_world_field(float *forest, float *urban);   // around the listener (0..1)
EnvWorldStats env_world_stats(void);
// The ray budget per frame for occlusion updates (voices), and the probe range.
void env_world_set_budget(int occlusion_voices_per_frame);

#define ENV_ECHO_RAYS 24
#define ENV_PROBE_RANGE 40.0f
#define ENV_ECHO_RANGE 450.0f

#endif
