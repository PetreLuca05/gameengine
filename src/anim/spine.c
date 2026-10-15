// Spine / look: aim pitch and yaw (relative to the rendered body yaw) distributed across
// spine, spine1, spine2, neck and head with per-bone weights and clamps.
#include "procedural_internal.h"
#include <math.h>

void proc_spine(ProcState *st, const ProcInput *in, Pose *pose, Xform *model) {
    const Skeleton *s = in->skel;
    const ProcParams *p = in->params;
    int bones[5] = { s->role[BR_SPINE], s->role[BR_SPINE1], s->role[BR_SPINE2], s->role[BR_NECK], s->role[BR_HEAD] };
    float yaw = wrap_angle(in->aim_yaw - st->render_body_yaw);
    float lim_y = p->yaw_limit * DG_DEG2RAD, lim_p = p->pitch_limit * DG_DEG2RAD;
    yaw = clampf(yaw, -lim_y, lim_y);
    float pitch = clampf(in->aim_pitch, -lim_p, lim_p);
    float acc_yaw = 0;
    for (int i = 0; i < 5; i++) {
        int b = bones[i];
        if (b < 0) continue;
        float w = p->spine_weights[i];
        float y = yaw * w, pt = pitch * (pitch > 0 ? p->spine_weights_up[i] : w);
        acc_yaw += y;
        // Yaw about model up, pitch about the (already yawed) lateral axis.
        vec3 lateral = quat_rotate(quat_yaw(acc_yaw), v3(1, 0, 0));
        quat q = quat_mul(quat_axis_angle(lateral, pt), quat_yaw(y));
        proc_rotate_bone(s, pose, model, b, q);
    }
}
