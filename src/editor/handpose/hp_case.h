// Simulated cases of the hand pose editor (docs/HANDPOSE.md "Cases"): builds the case's scene with
// the real systems (props spawned in front of the local player, the player seated in a spawned
// vehicle, hanging from a spawned ledge, holding the case's item) and drives the real pipeline at
// the timeline time: the interaction (hands.c), the seat / ledge hand anchors (characters.c), the
// reload clip. Also the rig context (base frames) the editor needs for the case's controls.
#ifndef DG_HP_CASE_H
#define DG_HP_CASE_H

#include "hp_rig.h"
#include "world/world.h"

typedef struct HpCaseState {
    bool active;
    u32 case_hash;
    int kind;                  // HandCaseKind
    EntityId prop, vehicle;    // spawned (removed on leave)
    float t;                   // timeline time (s)
    bool playing, loop;
    float speed;
    bool view_only;            // the clip owns the hands (reload): scrubbing shows the live clip
    char status[160];
} HpCaseState;

const HpCaseState *hp_case_state(void);
// Builds the scene of case ci (leaves the current one first). False + why when it can't.
bool hp_case_enter(World *w, int ci, char *why, int why_size);
void hp_case_leave(World *w);
// Per frame: the timeline (playing: advances by dt * speed, loops / stops at the end) and the
// pipeline driven at its time.
void hp_case_frame(World *w, float dt);
void hp_case_set_time(float t);
void hp_case_play(bool on);
void hp_case_set_loop(bool on);
void hp_case_set_speed(float s);
// The rig's base frames for the case at the current time (false: not available this frame).
bool hp_case_rig_frames(World *w, HpRigCtx *ctx);
// The item the case holds (item / reload cases; "" none) and its forced hold state (-1 none).
const char *hp_case_item(void);
int hp_case_hold_state(void);

#endif
