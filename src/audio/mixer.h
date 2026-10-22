// The DG mixer (docs/AUDIO_PROCEDURAL.md "Mixer"): every effect, voice and UI sound - sampled or
// synthesized - is a voice here: up to four patch layers summed, then its environment chain
// (audio/env/env_chain.h), into a bus. Buses -> listener filter (vehicle interior, underwater, low
// health, helmet) -> death / downed low-pass -> master. Three shared late reverbs take the chains'
// sends. miniaudio only runs the device (and the music stream).
//
// Threads: the game thread owns slot allocation and talks to the audio thread through a lock-free
// single-producer / single-consumer command ring; the audio thread publishes per voice the generation
// that finished and its peak level. mixer_render never allocates (tests hook the allocator).
#ifndef DG_MIXER_H
#define DG_MIXER_H

#include "audio/synth/syn_recipe.h"
#include "audio/env/env_chain.h"

#define MIX_VOICES 128
#define MIX_ARENA (96 * 1024)          // node state per voice, all layers
#define MIX_DOPPLER_LINES 16
#define MIX_DOPPLER_LEN 65536          // 1.36 s at 48 kHz: the farthest doppler source
#define MIX_CMDS 2048
#define MIX_STREAMS 8                  // voice chat: one per player slot
#define MIX_STREAM_LEN 32768           // ring per stream (0.68 s at 48 kHz)

typedef u32 MixHandle;                 // slot | generation << 8; 0 = none

typedef struct MixLayerStart {
    const SynPatch *patch;
    float gain;
    bool optional;
    float params[SYN_MAX_PARAMS];
} MixLayerStart;

typedef struct MixStart {
    int nlayers;
    MixLayerStart layer[RCP_MAX_LAYERS];
    u32 seed;
    u8 bus;                            // SynBus
    bool env;                          // run the environment chain (else centred, unity)
    bool doppler;                      // propagation through a doppler line (arrival = env.delay_s)
    float start_delay;                 // s before it starts (non-doppler propagation)
    float volume;
    float score;                       // priority x audibility: who is stolen when full
    EnvParams env_params;
} MixStart;

typedef struct MixListener {
    float lp_hz;                       // world buses top cut (underwater ~400, interior ~1800, 20000 off)
    float low_db;                      // low shelf (vehicle cabin boom)
    float high_db;                     // high shelf (helmet, ADS focus)
    float gain;
    float death_lp_hz, death_gain;     // the death / downed snapshot
} MixListener;

typedef struct MixStats {
    int active, rendered, virtual_count, layers;
    float render_us, render_avg_us, buffer_us;   // the last callback, its running mean, the buffer length
    int degrade;                       // 0 full, 1 optional layers dropped, 2 also early reflections
    int dropped_cmds, stolen;
    float peak;
} MixStats;

typedef struct Mixer Mixer;

Mixer *mixer_create(float sample_rate, double (*clock)(void));
void mixer_destroy(Mixer *m);
float mixer_rate(const Mixer *m);

// Game thread.
MixHandle mixer_start(Mixer *m, const MixStart *s);   // 0: no slot (all busier) or the queue is full
bool mixer_alive(const Mixer *m, MixHandle h);
void mixer_params(Mixer *m, MixHandle h, int layer, const float *params, int count);
void mixer_env(Mixer *m, MixHandle h, const EnvParams *p);
void mixer_volume(Mixer *m, MixHandle h, float volume);
void mixer_score(Mixer *m, MixHandle h, float score);
void mixer_stop(Mixer *m, MixHandle h, float fade_s);   // gate 0 + a fade, then free
void mixer_virtual(Mixer *m, MixHandle h, bool on);
void mixer_set_bus(Mixer *m, int bus, float gain);
void mixer_set_master(Mixer *m, float gain);
void mixer_set_listener(Mixer *m, const MixListener *l);
void mixer_set_reverb(Mixer *m, int zone, const EnvReverbParams *p);
void mixer_force_degrade(Mixer *m, int level);          // -1 automatic (default), 0..2 fixed (tests, snd_degrade)
void mixer_update(Mixer *m);                            // reclaims finished slots
// Streams (voice chat, docs/AUDIO_PROCEDURAL.md "Voice chat"): mono 48 kHz audio pushed by the game
// thread into a ring, played after a jitter buffer (80 ms) on the voice bus. Each one is split between
// the proximity path (its environment chain: distance, occlusion, panning, reverb sends) and the radio
// path (band-passed 350 Hz .. 3.2 kHz, driven, hiss, a squelch burst at the start and the end) by
// `radio` 0..1.
int mixer_stream_push(Mixer *m, int stream, const float *pcm, int n);   // samples taken (0: the ring is full)
void mixer_stream_env(Mixer *m, int stream, const EnvParams *p, float radio);
float mixer_stream_level(const Mixer *m, int stream);                   // recent peak (talking indicator)
MixStats mixer_stats(const Mixer *m);
float mixer_voice_peak(const Mixer *m, MixHandle h);
double mixer_voice_age(const Mixer *m, MixHandle h);    // s since it was started
int mixer_handle_slot(MixHandle h);

// Audio thread: interleaved stereo float, any frame count.
void mixer_render(Mixer *m, float *out, int frames);
// True while the calling thread is inside mixer_render (the allocator guard).
bool mixer_in_render(void);

#endif
