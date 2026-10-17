// Radial wheel geometry (docs/UI_WHEELS_HUD.md): slot angles, direction -> slot with a centre dead zone
// and angular hysteresis, the virtual cursor fed by mouse / stick deltas. Pure (tests/test_hud.c).
//
// Angles are radians, 0 = up (12 o'clock), increasing clockwise; screen space (y down). Slot 0 is
// centred at the top, slot k at k * 2pi / n.
#ifndef DG_RADIAL_MATH_H
#define DG_RADIAL_MATH_H

#include "core/base.h"
#include "core/dgmath.h"

#define RADIAL_MIN_SLOTS 2
#define RADIAL_MAX_SLOTS 12

typedef struct RadialSel {
    int slot;              // selected slot (-1 none: inside the dead zone)
    vec2 cursor;           // virtual cursor relative to the centre (pixels / stick units)
} RadialSel;

typedef struct RadialParams {
    int count;             // slots (2..12)
    float dead_zone;       // cursor length below which nothing changes (selection keeps)
    float radius;          // virtual cursor clamp (mouse deltas)
    float hysteresis;      // radians past a boundary before switching (fraction of a slot capped)
} RadialParams;

float radial_slot_angle(int slot, int count);                 // centre angle of a slot
float radial_angle_of(vec2 dir);                              // screen dir -> 0..2pi (0 up, clockwise)
// The slot under a direction without hysteresis (-1 for a zero vector).
int radial_slot_for_angle(float angle, int count);
// Angular distance from a's centre (0..pi).
float radial_angle_dist(float a, float b);
// Selection update from the cursor: inside the dead zone the selection stays; outside it changes
// only once the cursor is past the current slot's edge by `hysteresis`.
void radial_update(RadialSel *s, const RadialParams *p);
// Mouse / pad: accumulate a delta into the virtual cursor (clamped to the radius) and update.
void radial_feed_delta(RadialSel *s, const RadialParams *p, vec2 delta);
// Stick: the cursor is the stick itself (scaled to the radius); a centred stick keeps the selection.
void radial_feed_stick(RadialSel *s, const RadialParams *p, vec2 stick);
// Number keys / scroll: absolute slot or a step (wraps), clears the cursor.
void radial_select(RadialSel *s, const RadialParams *p, int slot);
void radial_step(RadialSel *s, const RadialParams *p, int dir);

#endif
