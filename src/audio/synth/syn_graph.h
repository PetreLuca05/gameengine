// Modular synth graph (docs/AUDIO_PROCEDURAL.md "Synth graph"): a patch is a DAG of nodes (sources,
// envelopes, filters, shapers, physical models, samples) processing SYN_BLOCK-frame mono buffers.
// syn_patch_compile turns the node list into a flat execution order (Kahn; cycles are rejected:
// feedback lives inside nodes - comb, waveguide, Karplus-Strong), lays out every node's state in one
// arena and checks it fits a voice. A voice renders its patch with no allocation: the mixer hands it
// a preallocated arena and a shared scratch area.
//
// Node parameters are constants, patch parameters (named, set per voice from the recipe's bindings
// and smoothed) or modulation from another node's output (block rate). Patches are immutable once
// compiled; voices only read them, so the audio thread shares them with the game thread safely.
#ifndef DG_SYN_GRAPH_H
#define DG_SYN_GRAPH_H

#include "syn_dsp.h"

#define SYN_MAX_NODES 32
#define SYN_MAX_PARAMS 16        // named patch parameters
#define SYN_NODE_PARAMS 8        // parameter slots per node
#define SYN_MAX_INPUTS 6
#define SYN_MAX_ARR 24           // modal modes, envelope stages, mix gains
#define SYN_VOICE_ARENA (64 * 1024)   // bytes of node state per voice layer (patches compile at the mixer rate, 48 kHz)

typedef enum SynNodeType {
    SN_OSC, SN_NOISE, SN_IMPULSE, SN_FRIEDLANDER, SN_NWAVE, SN_PULSES, SN_GLOTTAL, SN_GRAINS,
    SN_ENV, SN_BIQUAD, SN_SVF, SN_ONEPOLE, SN_COMB, SN_ALLPASS, SN_TILT, SN_SHAPER, SN_RING, SN_AM,
    SN_GAIN, SN_MIX, SN_MODAL, SN_KARPLUS, SN_WAVEGUIDE, SN_FORMANT, SN_DELAY, SN_CONV, SN_SAMPLE,
    SN_COUNT
} SynNodeType;

typedef enum SynSrcKind { SYN_SRC_CONST, SYN_SRC_PARAM, SYN_SRC_MOD } SynSrcKind;
// value = scale * source + offset (CONST: value only)
typedef struct SynParamSrc { u8 kind, index; float value, scale, offset; } SynParamSrc;

typedef struct SynNodeDef {
    u8 type;
    u8 opt;                      // wave / colour / filter mode / shaper mode / grain source / loop
    i8 sustain;                  // envelope: stage held while gate > 0.5 (-1 none)
    char id[24];
    i8 in[SYN_MAX_INPUTS];
    u8 nin;
    SynParamSrc p[SYN_NODE_PARAMS];
    float arr[3][SYN_MAX_ARR];   // modal f / t60 / gain; env time_ms / level / curve; mix gains
    u8 narr;
    int sample;                  // sample bank id (SN_SAMPLE, SN_GRAINS opt 3), -1 none
    float max_ms;                // delay buffer length (comb, allpass, delay, karplus, waveguide)
    u32 state_off, state_size;   // arena layout (syn_patch_compile)
} SynNodeDef;

typedef struct SynPatch {
    char name[48];
    int nnodes;
    SynNodeDef nodes[SYN_MAX_NODES];
    u8 order[SYN_MAX_NODES];     // execution order (dependencies first)
    int out;                     // output node
    int nparams;
    char param_name[SYN_MAX_PARAMS][24];
    float param_def[SYN_MAX_PARAMS], param_lo[SYN_MAX_PARAMS], param_hi[SYN_MAX_PARAMS];
    float param_tau;             // s, smoothing of patch parameters (default 0.015)
    float max_dur;               // s hard cap (0: until its envelopes / one-shots end and it is silent)
    bool loop;                   // never ends by itself (engines, wind): the mixer stops it
    bool compiled;
    float sr;
    u32 state_bytes;
} SynPatch;

// Per node type: name ("osc"), parameter slot names and defaults (the JSON keys, the editor rows).
typedef struct SynNodeInfo {
    const char *name;
    const char *param[SYN_NODE_PARAMS];
    float def[SYN_NODE_PARAMS];
    const char *opts;            // "|"-separated option names (opt index), NULL none
    u8 min_in, max_in;
} SynNodeInfo;
const SynNodeInfo *syn_node_info(int type);
int syn_node_type(const char *name);         // -1 unknown
int syn_param_slot(int type, const char *name);
int syn_node_opt(int type, const char *name);   // option index, -1 unknown

// Samples (decoded mono float at the mixer rate) for SN_SAMPLE / sample grains; owned by the mixer.
typedef struct SynSampleBank {
    const float *const *data;
    const int *frames;
    int count;
} SynSampleBank;
void syn_set_sample_bank(const SynSampleBank *bank);

// Compiles in place: order, state layout, sample-rate dependent sizes. false + err on a cycle, a
// dangling input, too much state.
bool syn_patch_compile(SynPatch *p, float sr, char *err, int err_size);
int syn_patch_param(const SynPatch *p, const char *name);   // -1 unknown
int syn_patch_node(const SynPatch *p, const char *id);       // -1 unknown

// ------------------------------------------------------------------ voices

typedef struct SynVoice {
    const SynPatch *patch;
    u8 *arena;                   // patch->state_bytes, SYN_VOICE_ARENA at most
    SynSmooth param[SYN_MAX_PARAMS];
    u32 seed;
    u64 t;                       // frames rendered
    int quiet_blocks;
    bool done;
} SynVoice;

// params: initial values (NULL: the patch defaults). The arena is cleared and every node seeded.
void syn_voice_init(SynVoice *v, const SynPatch *p, void *arena, const float *params, u32 seed);
void syn_voice_set_param(SynVoice *v, int index, float value);
void syn_voice_snap_param(SynVoice *v, int index, float value);   // no glide (voice start)
// Renders n <= SYN_BLOCK frames into out (overwrites). scratch: SYN_MAX_NODES * SYN_BLOCK floats.
// Returns false once the voice has finished (out is then silent).
bool syn_voice_render(SynVoice *v, float *out, int n, float *scratch);

// Formant tables (SN_FORMANT vowel index 0..5: a e i o u schwa) for the editor / tests.
#define SYN_VOWELS 6
#define SYN_FORMANTS 5
extern const float syn_vowel_f[SYN_VOWELS][SYN_FORMANTS];

#endif
