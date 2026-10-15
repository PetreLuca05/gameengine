// Turn in place: the body yaw is simulation state (host-owned, replicated quantized,
// predicted for the local player); the render side only smooths it and hides the foot
// slide with foot locking (foot_ik.c). The spine counter-rotates towards the aim (spine.c).
#include "procedural_internal.h"
#include <math.h>

float anim_body_yaw_step(float body, float aim, bool moving, const ProcParams *p, float dt, u8 *turning) {
    float diff = wrap_angle(aim - body);
    float threshold = (p ? p->turn_threshold : 65.0f) * DG_DEG2RAD;
    float rate = (p ? p->turn_speed : 260.0f) * DG_DEG2RAD;
    if (moving) {
        // Moving: the body faces the aim (strafe clips cover sideways motion).
        *turning = 0;
        float step = rate * 1.6f * dt;
        if (fabsf(diff) <= step) return aim;
        return wrap_angle(body + signf(diff) * step);
    }
    if (!*turning && fabsf(diff) > threshold) *turning = 1;
    if (*turning) {
        float step = rate * dt;
        if (fabsf(diff) <= step || fabsf(diff) < 4.0f * DG_DEG2RAD) {
            *turning = 0;
            return fabsf(diff) <= step ? aim : wrap_angle(body + signf(diff) * step);
        }
        return wrap_angle(body + signf(diff) * step);
    }
    return body;
}

void proc_turn(ProcState *st, const ProcInput *in) {
    if (!in->turn) { st->render_body_yaw = in->body_yaw; return; }
    // Critically damped follow of the replicated yaw (hides quantization / snapshot steps).
    float d = wrap_angle(in->body_yaw - st->render_body_yaw);
    if (fabsf(d) > 1.5f) st->render_body_yaw = in->body_yaw;
    else st->render_body_yaw = wrap_angle(st->render_body_yaw + d * (1.0f - expf(-in->dt * 22.0f)));
}
