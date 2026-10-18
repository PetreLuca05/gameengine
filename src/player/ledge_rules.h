// Ledge grab / climb rules (docs/CONTROLLER.md): which traversal a ledge in reach starts. One
// ledge detector (player/movement.c detect_ledge) feeds two cases:
//   auto-climb  moving towards the ledge (forward held since the jump, facing within
//               pl_climb_facing_cone of the wall) and jump pressed again while it is in reach (or
//               still held with pl_climb_hold_to_jump): vault / mantle / pull-up, no hang, no prompt
//   grab        otherwise, falling (not crouch-dropping) with a hang ledge in reach: hang + prompts
// Pure (no world, no physics): unit tested by tests/test_ledge.c.
#ifndef DG_LEDGE_RULES_H
#define DG_LEDGE_RULES_H

#include "core/base.h"
#include "core/dgmath.h"

// What detect_ledge found in front of the player.
typedef enum LedgeKind { LEDGE_NONE, LEDGE_MANTLE, LEDGE_VAULT, LEDGE_HANG } LedgeKind;

typedef enum LedgeAction {
    LA_NONE,      // keep falling / walking
    LA_VAULT,     // over a low, thin obstacle
    LA_MANTLE,    // onto a mid ledge
    LA_PULLUP,    // onto a high ledge in one go (auto-climb: no hang state)
    LA_GRAB,      // hang from it (fall case)
} LedgeAction;

typedef struct LedgeInput {
    bool on_ground;
    bool falling;          // airborne with a downward velocity
    bool crouch_held;      // deliberately dropping: no grab
    bool fwd_held;         // forward input now
    bool climb_intent;     // forward was held at the take-off (the jump started towards something)
    bool jump_fresh;       // a jump press within the jump buffer (a second press in the air)
    bool jump_held;
    bool hold_to_jump;     // pl_climb_hold_to_jump: a held jump counts as the second press
    float facing_cos;      // dot(view forward, into the wall)
    float cone_cos;        // cos(pl_climb_facing_cone)
    int kind;              // LedgeKind
    bool regrab_blocked;   // the ledge was just climbed from / let go of (pl_regrab_cooldown)
    bool grab_forbidden;   // pl_hang_two_handed 0 with a two-handed item
} LedgeInput;

// The airborne decision (the ground case, a jump press at a ledge, is unchanged: vault / mantle).
LedgeAction ledge_decide(const LedgeInput *in);

// True when the auto-climb inputs are given (the ledge detector is then worth running even rising).
bool ledge_climb_input(const LedgeInput *in);

// The regrab cooldown covers the ledge left last (within 1 m of it), not every ledge.
bool ledge_regrab_blocked(vec3 top, vec3 last_ledge, float cooldown_left);

typedef enum HangAction { HA_STAY, HA_CLIMB, HA_LET_GO } HangAction;
// Hang: Space (a fresh press, not the one held from the jump) climbs once the grab settled; Ctrl or
// S lets go; with pl_hang_stamina > 0 the grip gives out after that many seconds.
HangAction hang_decide(bool jump_pressed, bool crouch_pressed, float mv_y, float hang_time, float hang_used, float stamina);

#endif
