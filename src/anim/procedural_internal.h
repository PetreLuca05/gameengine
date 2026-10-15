// Shared helpers between the procedural pass files (not public API).
#ifndef DG_PROCEDURAL_INTERNAL_H
#define DG_PROCEDURAL_INTERNAL_H

#include "procedural.h"

DG_INLINE quat euler_deg(vec3 e) { return quat_euler(e.y * DG_DEG2RAD, e.x * DG_DEG2RAD, e.z * DG_DEG2RAD); }

// Rotates bone b in model space about its own pivot by q (premultiplied), updating children.
void proc_rotate_bone(const Skeleton *s, Pose *p, Xform *model, int b, quat q);
// Shoulder reach: rotate the clavicle so the arm root gets within reach of target (<= max_deg).
float proc_clavicle_reach(const Skeleton *s, Pose *p, Xform *model, int clav, int arm, vec3 target, float reach, float margin, float max_deg);

// Steps (see procedural.h for the order).
void proc_turn(ProcState *st, const ProcInput *in);
void proc_lean(ProcState *st, const ProcInput *in, Pose *pose, Xform *model);
void proc_foot_pelvis(ProcState *st, const ProcInput *in, Pose *pose, Xform *model, float foot_offset[2], vec3 anim_foot[2]);
void proc_spine(ProcState *st, const ProcInput *in, Pose *pose, Xform *model);
void proc_hold(ProcState *st, const ProcInput *in, Pose *pose, Xform *model);
void proc_hand_anchors(ProcState *st, const ProcInput *in, Pose *pose, Xform *model);
void proc_foot_legs(ProcState *st, const ProcInput *in, Pose *pose, Xform *model, const float foot_offset[2], const vec3 anim_foot[2]);

#endif
