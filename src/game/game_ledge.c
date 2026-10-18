// Ledge grab / climb, game side (docs/CONTROLLER.md): the two-handed item rule's hook, the hang
// prompts (local UI only: "[Space] Climb", "[Ctrl/S] Let go", gamepad A / B, from the action map) and
// the ledge_report test command. The traversal itself is predicted movement (player/movement.c).
#include "game_internal.h"
#include "core/log.h"
#include "gameplay/weapons.h"
#include "input/input.h"
#include <stdio.h>
#include <string.h>

static struct {
    bool drawn;          // the HUD drew the hang prompts last frame (tests)
    bool logged;         // the shown state last logged
} GL;

// pl_hang_two_handed 0: a two-handed item out forbids the grab (a sidearm / empty hands may).
static bool ledge_two_handed(World *w, int slot) {
    const InvSlot *eq = weapon_equipped(w, slot);
    const WeaponDef *wd = eq ? item_weapon(w->items, eq->item) : NULL;
    return wd && !weapon_is_sidearm(wd);
}

// The key of an action as the prompt shows it: the pad binding with a gamepad, else the first key.
static void action_key(Action a, char *out, size_t size) {
    out[0] = 0;
    bool pad = input_gamepad_connected();
    for (int i = 0; i < BINDS_PER_ACTION; i++) {
        Binding b = input_get_binding(a, i);
        if (b.type == BIND_NONE) continue;
        bool is_pad = b.type == BIND_PAD_BUTTON || b.type == BIND_PAD_AXIS;
        if (is_pad != pad) continue;
        binding_name(b, out, size);
        return;
    }
    Binding b = input_get_binding(a, 0);
    if (b.type != BIND_NONE) binding_name(b, out, size);
}

bool ledge_hang_prompts(char *climb, size_t climb_size, char *let_go, size_t let_go_size) {
    Game *g = game();
    int vs = game_view_slot();
    if (!g || !g->world || vs < 0 || vs != g->local_slot) return false;   // local UI only
    const Player *p = &g->world->players[vs];
    if (!p->active || p->sim.move.mode != MOVE_HANG) return false;
    char jump[32], crouch[32], back[32];
    action_key(ACT_JUMP, jump, sizeof(jump));
    action_key(ACT_CROUCH, crouch, sizeof(crouch));
    action_key(ACT_BACK, back, sizeof(back));
    snprintf(climb, climb_size, "[%s] Climb", jump);
    // The pad has no "back" button of its own (the stick): only the crouch button is named.
    if (input_gamepad_connected() || !back[0]) snprintf(let_go, let_go_size, "[%s] Let go", crouch);
    else snprintf(let_go, let_go_size, "[%s/%s] Let go", crouch, back);
    return true;
}

void ledge_prompts_drawn(bool drawn) {
    GL.drawn = drawn;
    if (drawn != GL.logged) {
        GL.logged = drawn;
        char c[64], l[64];
        if (drawn && ledge_hang_prompts(c, sizeof(c), l, sizeof(l))) log_info("ledge: prompts shown: %s | %s", c, l);
        else log_info("ledge: prompts hidden");
    }
}

static const char *move_mode_name(int mode) {
    switch (mode) {
    case MOVE_WALK: return "walk";
    case MOVE_MANTLE: return "mantle";
    case MOVE_VAULT: return "vault";
    case MOVE_HANG: return "hang";
    case MOVE_SWIM: return "swim";
    default: return "other";
    }
}

static void cmd_ledge_report(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    Game *g = game();
    int vs = game_view_slot();
    if (!g || !g->world || vs < 0) { log_info("ledge_report: no player"); return; }
    const PlayerMove *m = &g->world->players[vs].sim.move;
    char c[64] = "", l[64] = "";
    bool prompts = ledge_hang_prompts(c, sizeof(c), l, sizeof(l));
    log_info("ledge_report: mode %s pos %.3f %.3f %.3f vel %.2f %.2f %.2f ground %d intent %d regrab %.2f s hang %.2f s ledge %.3f %.3f %.3f prompts %s (%s | %s) drawn %d",
             move_mode_name(m->mode), m->pos.x, m->pos.y, m->pos.z, m->vel.x, m->vel.y, m->vel.z, m->on_ground, m->climb_intent, m->regrab_cooldown,
             m->hang_time, m->ledge_point.x, m->ledge_point.y, m->ledge_point.z, prompts ? "on" : "off", c, l, GL.drawn);
}

void ledge_register(void) {
    movement_set_two_handed(ledge_two_handed);
    cmd_register("ledge_report", cmd_ledge_report, "ledge_report: the viewed player's movement mode, ledge, regrab cooldown and hang prompts (tests, docs/CONTROLLER.md)");
}
