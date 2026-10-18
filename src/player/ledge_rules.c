// Ledge grab / climb rules (docs/CONTROLLER.md). Pure: unit tested by tests/test_ledge.c.
#include "player/ledge_rules.h"
#include <math.h>

bool ledge_climb_input(const LedgeInput *in) {
    bool jump = in->jump_fresh || (in->hold_to_jump && in->jump_held);
    return !in->on_ground && in->climb_intent && in->fwd_held && jump;
}

LedgeAction ledge_decide(const LedgeInput *in) {
    if (in->on_ground || in->kind == LEDGE_NONE || in->regrab_blocked) return LA_NONE;
    // Auto-climb: still moving towards the ledge, the second jump given while it is in reach.
    if (ledge_climb_input(in) && in->facing_cos >= in->cone_cos) {
        switch (in->kind) {
        case LEDGE_VAULT: return LA_VAULT;
        case LEDGE_MANTLE: return LA_MANTLE;
        case LEDGE_HANG: return LA_PULLUP;
        default: return LA_NONE;
        }
    }
    // Fall case: grab a hang ledge in reach (lower ledges are landed on, not grabbed).
    if (in->falling && !in->crouch_held && !in->grab_forbidden && in->kind == LEDGE_HANG && in->facing_cos >= in->cone_cos) return LA_GRAB;
    return LA_NONE;
}

bool ledge_regrab_blocked(vec3 top, vec3 last_ledge, float cooldown_left) {
    if (cooldown_left <= 0.0f) return false;
    float dx = top.x - last_ledge.x, dz = top.z - last_ledge.z;
    return dx * dx + dz * dz < 1.0f && fabsf(top.y - last_ledge.y) < 0.5f;
}

HangAction hang_decide(bool jump_pressed, bool crouch_pressed, float mv_y, float hang_time, float hang_used, float stamina) {
    if (stamina > 0.0f && hang_used >= stamina) return HA_LET_GO;
    if (jump_pressed && hang_time > 0.15f) return HA_CLIMB;
    if (crouch_pressed || mv_y < -0.5f) return HA_LET_GO;
    return HA_STAY;
}
