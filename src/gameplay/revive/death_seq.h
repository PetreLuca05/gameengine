// First-person death experience: pure timeline + curves (docs/REVIVE.md "Death sequence").
// Purely local and cosmetic: it never gates host state (spectating starts on its own timer).
//
//   NONE -death-> FALL (fast desaturate, ear ring rises, low-pass sweep)
//        -ragdoll head/torso impact (or timeout)-> BLACK (cut in one frame, ring fades to silence)
//        -silence + hold-> FADE_IN (spectating / respawn view, audio returns) -> DONE
//   Downed: a light persistent variant (DOWNED weight) that recovers on revive.
#ifndef DG_DEATH_SEQ_H
#define DG_DEATH_SEQ_H

#include "core/base.h"

typedef struct DeathSeqParams {
    // stage 1: the fall (seen through the ragdoll's eyes)
    float s1_time;              // s to reach the desaturated target
    float desat, contrast, vignette, blur, exposure;   // targets (contrast / exposure are offsets)
    float impact_timeout;       // s: cut even if no impact was detected
    float impact_min_time;      // s: ignore contacts right after death (already touching the ground)
    float impact_min_speed;     // m/s the body must have reached for the velocity-drop fallback
    float impact_speed_drop;    // fraction of the peak speed lost in one step = impact
    float impact_contact_dist;  // m: head / chest this close to the ground = contact
    // stage 2: black
    float hold_time;            // s of black after silence
    float fade_in;              // s fade into spectating
    // audio (assets/audio/death_ring.json)
    float lpf_from, lpf_to;     // Hz
    float lpf_sweep;            // s
    float world_duck;           // world gain while ringing (muffled residual sound)
    float ring_attack;          // s
    float ring_level;
    float ring_fade;            // s after the cut
    float ring_freq, warble_hz, warble_depth;   // Hz, Hz, fraction of ring_freq
    float heart_level, heart_bpm_start, heart_bpm_end;
    // downed light variant
    float d_desat, d_vignette, d_blur, d_lpf, d_ring, d_attack, d_release, d_world;
} DeathSeqParams;

typedef enum DeathStage { DS_NONE, DS_FALL, DS_BLACK, DS_FADE_IN, DS_DONE } DeathStage;
typedef enum ImpactReason { IMPACT_NONE, IMPACT_CONTACT, IMPACT_VELOCITY, IMPACT_TIMEOUT, IMPACT_FORCED } ImpactReason;

typedef struct DeathSeq {
    u8 stage;                   // DeathStage
    u8 cut_pending;             // the next evaluated frame is the cut (camera cut, TAA reset)
    u8 impact_reason;           // ImpactReason
    u8 skipped;
    float t;                    // s since death
    float t_stage;              // s in the current stage
    float impact_t;             // s from death to the cut
    float downed_w;             // 0..1 light variant weight
    float peak_speed;           // impact detector
    float last_speed;
} DeathSeq;

// Everything the post stack and the mixer need for one frame (all neutral = zeros / 1 / lpf_from).
typedef struct DeathFx {
    float desat;                // 0..1 saturation removed
    float contrast;             // offset (negative = flatter)
    float vignette;             // added edge vignette
    float blur;                 // 0..1
    float exposure;             // EV offset
    float black;                // 0..1 (1 = black frame)
    bool camera_cut;            // reset temporal history this frame
    float lpf_hz;               // world bus low-pass cutoff
    float world_gain;           // world buses (ambience / weapons / vehicles)
    float ring_gain;            // tinnitus voice
    float heart_gain, heart_bpm;
    bool hud_hidden;
} DeathFx;

void death_params_defaults(DeathSeqParams *p);
// Two files share the struct: the visual timeline (death_sequence.json) and the ring (death_ring.json).
bool death_params_parse_visual(DeathSeqParams *p, const char *json, char *err, int err_size);
bool death_params_parse_ring(DeathSeqParams *p, const char *json, char *err, int err_size);

void deathseq_reset(DeathSeq *s);            // revive / respawn: back to neutral at once
void deathseq_start(DeathSeq *s);            // the local player died
// The ragdoll's head / torso hit the ground (from the impact detector): the black cut.
void deathseq_impact(DeathSeq *s, ImpactReason why);
void deathseq_skip(DeathSeq *s);             // skip to the fade-in
// Advance one frame. downed: the local player is knocked (drives the light variant).
void deathseq_step(DeathSeq *s, const DeathSeqParams *p, float dt, bool downed);
DeathFx deathseq_eval(const DeathSeq *s, const DeathSeqParams *p);
// After the eval of a frame (consumes the one-frame cut).
void deathseq_frame_done(DeathSeq *s);
DeathFx deathfx_neutral(const DeathSeqParams *p);
// Scrub without dying (death_preview): stage 1 (fall), 2 (black / ring fade), 3 (downed variant).
DeathFx deathseq_preview(const DeathSeqParams *p, int stage, float t01);
const char *death_stage_name(DeathStage s);

// Impact detector over the ragdoll's head + chest bodies, fed once per physics tick.
// contact: either body is within impact_contact_dist of the ground (short downward cast by the caller).
ImpactReason death_impact_detect(DeathSeq *s, const DeathSeqParams *p, float head_speed, float chest_speed, bool contact);

#endif
