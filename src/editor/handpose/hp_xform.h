// Blender-style modal transforms for the hand pose rig (docs/HANDPOSE.md "Transform tool"). Pure:
// the editor feeds mouse positions, keys and a projection; the module returns each transformed
// control's new world transform. Grab / rotate / scale (G / R / S), axis constraints (X / Y / Z,
// pressed again: the next orientation, Shift+axis: the plane without it), Ctrl snapping, typed
// values, Esc / right click cancel, Enter / click confirm, Shift precision (a tenth of the mouse
// motion, finer snapping); orientation Global / Local / Parent / Normal; pivot individual origins /
// median / active.
#ifndef DG_HP_XFORM_H
#define DG_HP_XFORM_H

#include "core/base.h"
#include "anim/anim.h"   // Xform

#define HPX_MAX 48

typedef enum HpxMode { HPX_NONE, HPX_GRAB, HPX_ROTATE, HPX_SCALE } HpxMode;
typedef enum HpxOrient { HPX_GLOBAL, HPX_LOCAL, HPX_PARENT, HPX_NORMAL, HPX_ORIENT_COUNT } HpxOrient;
typedef enum HpxPivot { HPX_PIVOT_INDIVIDUAL, HPX_PIVOT_MEDIAN, HPX_PIVOT_ACTIVE, HPX_PIVOT_COUNT } HpxPivot;

// World -> screen pixels (false: behind the camera) and the camera position (rotation signs).
typedef bool (*HpxProjectFn)(void *ctx, vec3 world, vec2 *px);
typedef struct HpxView {
    HpxProjectFn project;
    void *ctx;
    vec3 cam_pos;
} HpxView;

typedef struct HpXform {
    int mode;                      // HpxMode (HPX_NONE: idle)
    int orient;                    // the orientation the axes are in (cycles with repeated axis keys)
    int pivot_mode;
    int axis;                      // -1 free, 0..2 constrained axis
    bool plane;                    // Shift+axis: the plane perpendicular to `axis`
    int axis_presses;              // repeated presses of the same axis key cycle global -> local -> free
    char num[24];                  // typed value ("" = mouse)
    vec2 mouse0;                   // where the transform started (screen px)
    bool precise;                  // set by the caller (Shift held): mouse motion counts a tenth, snaps 1/5 the step
    vec2 mouse_last, mouse_off;    // precision: the transform follows the mouse + mouse_off
    vec3 pivot;                    // world
    quat basis[HPX_ORIENT_COUNT];  // the axis frames per orientation (active control)
    int count, active;
    int ids[HPX_MAX];              // caller ids (rig controls)
    Xform start[HPX_MAX];          // world transforms when it started
    Xform out[HPX_MAX];            // the current result
    // readout
    vec3 delta;                    // grab: world translation
    float angle_deg;               // rotate
    float factor;                  // scale
} HpXform;

// Starts a transform of n controls (their start world transforms, the active one's index into
// them) with the axis frames per orientation (global = identity).
void hpx_begin(HpXform *x, int mode, vec2 mouse, int n, const int *ids, const Xform *start, int active, const quat basis[HPX_ORIENT_COUNT], int orient, int pivot_mode);
void hpx_axis(HpXform *x, int axis, bool shift);   // X / Y / Z (0..2)
// Typed input: '0'..'9', '.', '-' (negates), '\b' (backspace). False: not a numeric character.
bool hpx_char(HpXform *x, char c);
// Recomputes out[] for the current mouse position; snap = Ctrl held (5 mm / 5 deg / 0.05, with
// precise 1 mm / 1 deg / 0.01).
void hpx_update(HpXform *x, vec2 mouse, const HpxView *v, bool snap);
void hpx_cancel(HpXform *x);       // out = start, mode none
void hpx_end(HpXform *x);          // mode none (out keeps the result)
bool hpx_active(const HpXform *x);
const char *hpx_mode_name(int mode);
const char *hpx_orient_name(int o);
const char *hpx_pivot_name(int p);
// The status line ("Rotate Z (local) 12.5 deg [typed]").
void hpx_status(const HpXform *x, char *out, int size);
// The world axis of the current constraint (or zero when free).
vec3 hpx_axis_world(const HpXform *x, int axis);

#endif
