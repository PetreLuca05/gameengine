// Unit tests for the ledge grab / climb rules (player/ledge_rules.c, docs/CONTROLLER.md): the fall
// grab, the auto-climb (second jump while moving towards the ledge), their edge rules, the regrab
// cooldown and the hang's climb / let go.
// Build: part of the normal CMake build (target test_ledge); run: ctest or build/bin/test_ledge.
#include "player/ledge_rules.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

// Falling, facing the wall straight on, nothing pressed.
static LedgeInput falling(int kind) {
    LedgeInput in;
    memset(&in, 0, sizeof(in));
    in.falling = true;
    in.facing_cos = 1.0f;
    in.cone_cos = cosf(45.0f * 3.14159265f / 180.0f);
    in.kind = kind;
    return in;
}

// Running at the ledge: forward held since the take-off, the second jump press in reach.
static LedgeInput run_climb(int kind) {
    LedgeInput in = falling(kind);
    in.falling = false;   // still rising: the climb does not wait for the fall
    in.fwd_held = true;
    in.climb_intent = true;
    in.jump_fresh = true;
    return in;
}

static void test_fall_grab(void) {
    LedgeInput in = falling(LEDGE_HANG);
    CHECK(ledge_decide(&in) == LA_GRAB, "falling past a hang ledge in reach grabs it");
    in.falling = false;
    CHECK(ledge_decide(&in) == LA_NONE, "rising past it does not");
    in = falling(LEDGE_MANTLE);
    CHECK(ledge_decide(&in) == LA_NONE, "a low ledge passed in a fall is landed on, not grabbed");
    in = falling(LEDGE_NONE);
    CHECK(ledge_decide(&in) == LA_NONE, "nothing in reach: normal fall");
    in = falling(LEDGE_HANG);
    in.crouch_held = true;
    CHECK(ledge_decide(&in) == LA_NONE, "crouch-dropping: no grab");
    in = falling(LEDGE_HANG);
    in.facing_cos = cosf(60.0f * 3.14159265f / 180.0f);
    CHECK(ledge_decide(&in) == LA_NONE, "facing 60 deg away (cone 45): no grab");
    in = falling(LEDGE_HANG);
    in.grab_forbidden = true;
    CHECK(ledge_decide(&in) == LA_NONE, "pl_hang_two_handed 0 with a rifle out: no grab");
    in = falling(LEDGE_HANG);
    in.fwd_held = true;
    in.climb_intent = true;
    CHECK(ledge_decide(&in) == LA_GRAB, "forward held, no second jump: grab (and the prompts)");
}

static void test_auto_climb(void) {
    LedgeInput in = run_climb(LEDGE_HANG);
    CHECK(ledge_decide(&in) == LA_PULLUP, "run + jump + forward + second jump at a high ledge: pull-up, no hang");
    in = run_climb(LEDGE_MANTLE);
    CHECK(ledge_decide(&in) == LA_MANTLE, "a mid ledge: mantle");
    in = run_climb(LEDGE_VAULT);
    CHECK(ledge_decide(&in) == LA_VAULT, "a low obstacle: vault");
    in = run_climb(LEDGE_HANG);
    in.falling = true;
    CHECK(ledge_decide(&in) == LA_PULLUP, "the second press while falling still climbs");
    // Without the second press, or with forward released: the fall case.
    in = run_climb(LEDGE_HANG);
    in.jump_fresh = false;
    in.falling = true;
    CHECK(ledge_decide(&in) == LA_GRAB, "no second jump: grab");
    in.jump_held = true;
    CHECK(ledge_decide(&in) == LA_GRAB, "the take-off press still held is not a second press");
    in.hold_to_jump = true;
    CHECK(ledge_decide(&in) == LA_PULLUP, "pl_climb_hold_to_jump 1: the held jump climbs");
    in = run_climb(LEDGE_HANG);
    in.fwd_held = false;
    in.falling = true;
    CHECK(ledge_decide(&in) == LA_GRAB, "forward released: grab");
    in = run_climb(LEDGE_HANG);
    in.climb_intent = false;
    in.falling = true;
    CHECK(ledge_decide(&in) == LA_GRAB, "walked off an edge (no take-off towards it): grab");
    in = run_climb(LEDGE_HANG);
    in.facing_cos = 0.3f;
    CHECK(ledge_decide(&in) == LA_NONE, "facing away from the wall: no climb (and rising: no grab)");
    in = run_climb(LEDGE_NONE);
    CHECK(ledge_decide(&in) == LA_NONE, "no clearance (the detector found nothing to climb into): no climb");
    in = run_climb(LEDGE_HANG);
    in.on_ground = true;
    CHECK(ledge_decide(&in) == LA_NONE && !ledge_climb_input(&in), "on the ground: the ground rules apply");
    CHECK(ledge_climb_input(&(LedgeInput){ .climb_intent = true, .fwd_held = true, .jump_fresh = true }), "climb input");
}

static void test_regrab(void) {
    vec3 last = v3(10, 3, 5);
    CHECK(ledge_regrab_blocked(v3(10.4f, 3.1f, 5.2f), last, 0.3f), "the ledge just let go of: blocked");
    CHECK(!ledge_regrab_blocked(v3(10.4f, 3.1f, 5.2f), last, 0.0f), "after the cooldown: free");
    CHECK(!ledge_regrab_blocked(v3(13.0f, 3.0f, 5.0f), last, 0.3f), "another ledge 3 m along: free");
    CHECK(!ledge_regrab_blocked(v3(10.0f, 1.5f, 5.0f), last, 0.3f), "a ledge below: free");
    LedgeInput in = falling(LEDGE_HANG);
    in.regrab_blocked = true;
    CHECK(ledge_decide(&in) == LA_NONE, "regrab cooldown: no grab");
    in = run_climb(LEDGE_MANTLE);
    in.regrab_blocked = true;
    CHECK(ledge_decide(&in) == LA_NONE, "regrab cooldown: no climb either");
}

static void test_hang(void) {
    CHECK(hang_decide(false, false, 0.0f, 1.0f, 1.0f, 0.0f) == HA_STAY, "hanging, nothing pressed");
    CHECK(hang_decide(true, false, 0.0f, 1.0f, 1.0f, 0.0f) == HA_CLIMB, "Space climbs");
    CHECK(hang_decide(true, false, 0.0f, 0.05f, 0.05f, 0.0f) == HA_STAY, "not in the first 0.15 s");
    CHECK(hang_decide(false, true, 0.0f, 1.0f, 1.0f, 0.0f) == HA_LET_GO, "Ctrl lets go");
    CHECK(hang_decide(false, false, -1.0f, 1.0f, 1.0f, 0.0f) == HA_LET_GO, "S lets go");
    CHECK(hang_decide(false, false, 1.0f, 5.0f, 5.0f, 0.0f) == HA_STAY, "forward held does not climb (the prompt asks for Space)");
    CHECK(hang_decide(false, false, 0.0f, 100.0f, 100.0f, 0.0f) == HA_STAY, "no stamina rule: hang forever (Chapter 1)");
    CHECK(hang_decide(false, false, 0.0f, 8.0f, 8.0f, 8.0f) == HA_LET_GO, "pl_hang_stamina 8: drops at 8 s");
    CHECK(hang_decide(false, false, 0.0f, 7.9f, 7.9f, 8.0f) == HA_STAY, "still holding at 7.9 s");
}

int main(void) {
    test_fall_grab();
    test_auto_climb();
    test_regrab();
    test_hang();
    printf("test_ledge: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
