// Hand pose editor gizmo + viewport math (pure, docs/HANDPOSE.md): projection / mouse rays for a
// render camera, translate / rotate gizmo picking and dragging (local / world, snap), joint picking.
#ifndef DG_HP_GIZMO_H
#define DG_HP_GIZMO_H

#include "core/base.h"
#include "core/dgmath.h"
#include "anim/anim.h"

// A render camera: position, rotation (x right, y up, -z forward), vertical FOV, viewport (pixels).
typedef struct HpCamera {
    vec3 pos;
    quat rot;
    float fov_deg;
    float w, h;
} HpCamera;

// World -> screen (pixels, y down). False behind the camera.
bool hpg_project(const HpCamera *c, vec3 p, vec2 *out);
// Screen pixel -> world ray (unit direction).
void hpg_ray(const HpCamera *c, vec2 px, vec3 *origin, vec3 *dir);
// Ray / oriented box intersection (half extents): distance along the ray, false on a miss.
bool hpg_ray_box(vec3 o, vec3 d, vec3 center, quat rot, vec3 half, float *t);

typedef enum HpGizmoAxis { HPG_NONE = -1, HPG_X, HPG_Y, HPG_Z, HPG_RX, HPG_RY, HPG_RZ } HpGizmoAxis;
enum { HPG_TRANSLATE, HPG_ROTATE };

typedef struct HpGizmo {
    int mode;                  // HPG_TRANSLATE / HPG_ROTATE
    bool local;                // axes of the edited frame (else world)
    bool snap;
    float snap_move;           // m
    float snap_angle;          // deg
    float size;                // axis length in m (scaled with the camera distance by the caller)
    int hot;                   // axis under the mouse
    int active;                // axis being dragged
    Xform start;               // the edited frame at the press (world)
    vec3 start_point;          // translate: the axis point under the mouse at the press
    float start_angle;         // rotate: the ring angle at the press
} HpGizmo;

void hpg_init(HpGizmo *g);
// The gizmo frame: the edited frame's position, its rotation (local) or identity (world).
Xform hpg_frame(const HpGizmo *g, Xform edited);
// Axis under the mouse (pixels): axis segments for translate, rings for rotate. HPG_NONE if none
// is within `radius_px`.
int hpg_pick(const HpGizmo *g, const HpCamera *c, Xform frame, vec2 mouse, float radius_px);
// Press on an axis: remembers the start. Returns false if the axis can't be dragged from here.
bool hpg_begin(HpGizmo *g, const HpCamera *c, Xform edited, int axis, vec2 mouse);
// Drag: the edited frame (world) for the current mouse position, snapped.
Xform hpg_drag(const HpGizmo *g, const HpCamera *c, vec2 mouse);
void hpg_end(HpGizmo *g);
// Lines to draw (world): 3 axes or 3 rings (as segments). Returns the segment count; col[i] is the
// axis (HPG_X..HPG_RZ) of segment i.
int hpg_lines(const HpGizmo *g, Xform frame, vec3 *a, vec3 *b, int *axis, int max);

// Nearest projected point to the mouse among n points within radius_px: its index or -1.
int hpg_pick_point(const HpCamera *c, const vec3 *pts, int n, vec2 mouse, float radius_px);

#endif
