// Dynamic resolution (r_dynres, docs/RENDERING_POST.md "Dynamic resolution"): the share of the display
// resolution a temporal upscaler (TAA-U, FSR 2) renders, lowered while the GPU frame runs over the
// frame-rate target and raised back towards the player's own r_upscale_ratio when there is room. Pure
// (no renderer): tests/test_dynres.c.
//
// Every change re-creates the render-resolution targets (a short stall), so the controller changes
// rarely: the GPU time is smoothed, it must stay over budget (or well under it) for a while, the ratio
// moves in quantized steps and holds for a cooldown after each change.
#ifndef DG_DYNRES_H
#define DG_DYNRES_H

#include "core/base.h"

typedef struct DynresParams {
    float target_ms;          // the frame budget (1000 / target fps)
    float min_ratio, max_ratio;
    float step;               // quantization of the ratio (0.05)
    float over, under;        // budget fractions: lower above over x budget, raise below under x budget
    float over_hold, under_hold;   // seconds the smoothed time must stay past them
    float cooldown;           // seconds after a change
} DynresParams;

typedef struct DynresState {
    float ratio;              // current share (0 = not started: takes max_ratio)
    float gpu_ms;             // smoothed GPU frame time
    float over_t, under_t, cool_t;
    int changes;
} DynresState;

void dynres_defaults(DynresParams *p, float target_fps, float min_ratio, float max_ratio);
// One frame: the last completed GPU frame time (ms, <= 0: unknown, ignored) and this frame's dt (s).
// Returns the ratio to render at (quantized; a change only after the holds and the cooldown).
float dynres_update(DynresState *s, const DynresParams *p, float gpu_ms, float dt);

#endif
