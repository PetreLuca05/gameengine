#include "input.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct InputSystem {
    Binding binds[ACT_COUNT][BINDS_PER_ACTION];
    bool phys_down[ACT_COUNT];   // from devices
    bool script_down[ACT_COUNT]; // from +action console commands
    int press_count[ACT_COUNT];
    bool keys[SDL_SCANCODE_COUNT];
    bool mouse_buttons[8];
    vec2 mouse_accum;
    float wheel_accum;
    InputFrame frame;
    bool game_focus;
    bool ads;
    SDL_Gamepad *pad;
    int capture_action, capture_slot;
    bool capturing;

    float ads_fov_scale;
    Cvar *sens, *sens_y, *ads_mult, *invert_y, *pad_sens, *pad_deadzone, *m_raw;
} InputSystem;

#define IN (g_engine.input)

static const char *k_action_names[ACT_COUNT] = {
    "forward", "back", "left", "right", "jump", "crouch", "sprint", "walk",
    "use", "attack", "aim", "reload", "drop", "inventory", "netstats",
    "slot1", "slot2", "slot3", "slot4", "slot5", "slot6", "slot7", "slot8", "slot9",
    "nextitem", "previtem", "firemode",
};

static const char *k_action_labels[ACT_COUNT] = {
    "Move Forward", "Move Back", "Strafe Left", "Strafe Right", "Jump / Mantle", "Crouch", "Sprint", "Walk",
    "Use / Interact", "Attack / Fire", "Aim Down Sights", "Reload", "Drop Item", "Inventory", "Net Stats",
    "Hotbar 1", "Hotbar 2", "Hotbar 3", "Hotbar 4", "Hotbar 5", "Hotbar 6", "Hotbar 7", "Hotbar 8", "Hotbar 9",
    "Next Item", "Previous Item", "Fire Mode",
};

const char *action_name(Action a) { return (a >= 0 && a < ACT_COUNT) ? k_action_names[a] : "?"; }
const char *action_label(Action a) { return (a >= 0 && a < ACT_COUNT) ? k_action_labels[a] : "?"; }

static Binding key(SDL_Scancode sc) { Binding b = { BIND_KEY, (int)sc }; return b; }
static Binding mouse(int button) { Binding b = { BIND_MOUSE, button }; return b; }
static Binding padb(SDL_GamepadButton btn) { Binding b = { BIND_PAD_BUTTON, (int)btn }; return b; }
static Binding pada(SDL_GamepadAxis axis, bool neg) { Binding b = { BIND_PAD_AXIS, (int)axis * 2 + (neg ? 1 : 0) }; return b; }
static Binding wheel(int dir) { Binding b = { BIND_WHEEL, dir }; return b; }

void input_reset_bindings(void) {
    memset(IN->binds, 0, sizeof(IN->binds));
    IN->binds[ACT_FORWARD][0] = key(SDL_SCANCODE_W);
    IN->binds[ACT_BACK][0] = key(SDL_SCANCODE_S);
    IN->binds[ACT_LEFT][0] = key(SDL_SCANCODE_A);
    IN->binds[ACT_RIGHT][0] = key(SDL_SCANCODE_D);
    IN->binds[ACT_JUMP][0] = key(SDL_SCANCODE_SPACE);
    IN->binds[ACT_JUMP][1] = padb(SDL_GAMEPAD_BUTTON_SOUTH);
    IN->binds[ACT_CROUCH][0] = key(SDL_SCANCODE_LCTRL);
    IN->binds[ACT_CROUCH][1] = padb(SDL_GAMEPAD_BUTTON_EAST);
    IN->binds[ACT_SPRINT][0] = key(SDL_SCANCODE_LSHIFT);
    IN->binds[ACT_SPRINT][1] = padb(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    IN->binds[ACT_WALK][0] = key(SDL_SCANCODE_LALT);
    IN->binds[ACT_USE][0] = key(SDL_SCANCODE_E);
    IN->binds[ACT_USE][1] = padb(SDL_GAMEPAD_BUTTON_WEST);
    IN->binds[ACT_ATTACK][0] = mouse(SDL_BUTTON_LEFT);
    IN->binds[ACT_ATTACK][1] = pada(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, false);
    IN->binds[ACT_AIM][0] = mouse(SDL_BUTTON_RIGHT);
    IN->binds[ACT_AIM][1] = pada(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, false);
    IN->binds[ACT_RELOAD][0] = key(SDL_SCANCODE_R);
    IN->binds[ACT_RELOAD][1] = padb(SDL_GAMEPAD_BUTTON_NORTH);
    IN->binds[ACT_DROP][0] = key(SDL_SCANCODE_G);
    IN->binds[ACT_INVENTORY][0] = key(SDL_SCANCODE_TAB);
    IN->binds[ACT_INVENTORY][1] = padb(SDL_GAMEPAD_BUTTON_BACK);
    IN->binds[ACT_NETSTATS][0] = key(SDL_SCANCODE_F3);
    for (int i = 0; i < 9; i++) IN->binds[ACT_SLOT1 + i][0] = key((SDL_Scancode)(SDL_SCANCODE_1 + i));
    IN->binds[ACT_NEXT_ITEM][0] = wheel(-1);
    IN->binds[ACT_NEXT_ITEM][1] = padb(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    IN->binds[ACT_PREV_ITEM][0] = wheel(1);
    IN->binds[ACT_PREV_ITEM][1] = padb(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    IN->binds[ACT_FIREMODE][0] = key(SDL_SCANCODE_B);
    IN->binds[ACT_FIREMODE][1] = padb(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
}

static bool binding_eq(Binding a, Binding b) { return a.type == b.type && a.code == b.code; }

void input_bind_missing_defaults(void) {
    // Actions added after a config was saved (it starts with "unbindall") get their defaults,
    // unless that input is already taken by another action.
    Binding cur[ACT_COUNT][BINDS_PER_ACTION];
    memcpy(cur, IN->binds, sizeof(cur));
    input_reset_bindings();
    Binding def[ACT_COUNT][BINDS_PER_ACTION];
    memcpy(def, IN->binds, sizeof(def));
    memcpy(IN->binds, cur, sizeof(cur));
    for (int a = 0; a < ACT_COUNT; a++) {
        bool any = false;
        for (int s = 0; s < BINDS_PER_ACTION; s++) any |= cur[a][s].type != BIND_NONE;
        if (any) continue;
        for (int s = 0; s < BINDS_PER_ACTION; s++) {
            if (def[a][s].type == BIND_NONE) continue;
            bool taken = false;
            for (int o = 0; o < ACT_COUNT && !taken; o++)
                for (int t = 0; t < BINDS_PER_ACTION; t++) taken |= binding_eq(IN->binds[o][t], def[a][s]);
            if (!taken) IN->binds[a][s] = def[a][s];
        }
    }
}

const char *binding_name(Binding b, char *buf, size_t size) {
    switch (b.type) {
    case BIND_KEY: snprintf(buf, size, "%s", SDL_GetScancodeName((SDL_Scancode)b.code)); break;
    case BIND_MOUSE: snprintf(buf, size, "MOUSE%d", b.code); break;
    case BIND_WHEEL: snprintf(buf, size, b.code > 0 ? "MWHEELUP" : "MWHEELDOWN"); break;
    case BIND_PAD_BUTTON: {
        const char *n = SDL_GetGamepadStringForButton((SDL_GamepadButton)b.code);
        snprintf(buf, size, "PAD_%s", n ? n : "?");
        break;
    }
    case BIND_PAD_AXIS: {
        const char *n = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)(b.code / 2));
        snprintf(buf, size, "PADAXIS_%s%s", n ? n : "?", (b.code & 1) ? "-" : "+");
        break;
    }
    default: snprintf(buf, size, "---"); break;
    }
    // Replace spaces so names round-trip through the config tokenizer.
    for (char *p = buf; *p; p++) if (*p == ' ') *p = '_';
    return buf;
}

bool binding_from_name(const char *name, Binding *out) {
    char tmp[64];
    dg_strcpy(tmp, sizeof(tmp), name);
    for (char *p = tmp; *p; p++) if (*p == '_') *p = ' ';
    if (SDL_strncasecmp(name, "MOUSE", 5) == 0) { out->type = BIND_MOUSE; out->code = atoi(name + 5); return out->code > 0; }
    if (SDL_strcasecmp(name, "MWHEELUP") == 0) { *out = wheel(1); return true; }
    if (SDL_strcasecmp(name, "MWHEELDOWN") == 0) { *out = wheel(-1); return true; }
    if (SDL_strncasecmp(name, "PADAXIS_", 8) == 0) {
        char axis[48];
        dg_strcpy(axis, sizeof(axis), name + 8);
        size_t l = strlen(axis);
        if (!l) return false;
        bool neg = axis[l - 1] == '-';
        axis[l - 1] = 0;
        SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(axis);
        if (a == SDL_GAMEPAD_AXIS_INVALID) return false;
        *out = pada(a, neg);
        return true;
    }
    if (SDL_strncasecmp(name, "PAD_", 4) == 0) {
        SDL_GamepadButton btn = SDL_GetGamepadButtonFromString(name + 4);
        if (btn == SDL_GAMEPAD_BUTTON_INVALID) return false;
        *out = padb(btn);
        return true;
    }
    SDL_Scancode sc = SDL_GetScancodeFromName(tmp);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    *out = key(sc);
    return true;
}

static int action_from_name(const char *n) {
    for (int i = 0; i < ACT_COUNT; i++) if (SDL_strcasecmp(k_action_names[i], n) == 0) return i;
    return -1;
}

static void cmd_bind(int argc, const char **argv) {
    if (argc < 3) { log_info("usage: bind <key> <action> [slot]"); return; }
    Binding b;
    if (!binding_from_name(argv[1], &b)) { log_warn("bind: unknown key '%s'", argv[1]); return; }
    int a = action_from_name(argv[2]);
    if (a < 0) { log_warn("bind: unknown action '%s'", argv[2]); return; }
    int slot = argc > 3 ? DG_CLAMP(atoi(argv[3]), 0, BINDS_PER_ACTION - 1) : -1;
    if (slot < 0) {
        slot = 0;
        for (int s = 0; s < BINDS_PER_ACTION; s++) if (IN->binds[a][s].type == BIND_NONE) { slot = s; break; }
    }
    IN->binds[a][slot] = b;
    config_mark_dirty();
}

static void cmd_unbindall(int argc, const char **argv) { memset(IN->binds, 0, sizeof(IN->binds)); }

static void cmd_bindlist(int argc, const char **argv) {
    for (int a = 0; a < ACT_COUNT; a++) {
        char b0[48], b1[48];
        log_info("  %-12s %-16s %s", k_action_names[a], binding_name(IN->binds[a][0], b0, sizeof(b0)),
                 binding_name(IN->binds[a][1], b1, sizeof(b1)));
    }
}

static void cmd_plus(int argc, const char **argv) {
    // registered once per action as "+name" / "-name"
    const char *n = argv[0];
    int a = action_from_name(n + 1);
    if (a < 0) return;
    bool down = n[0] == '+';
    if (down && !IN->script_down[a]) IN->press_count[a]++;
    IN->script_down[a] = down;
}

static void write_binds(FILE *f) {
    fprintf(f, "unbindall\n");
    for (int a = 0; a < ACT_COUNT; a++)
        for (int s = 0; s < BINDS_PER_ACTION; s++) {
            if (IN->binds[a][s].type == BIND_NONE) continue;
            char name[48];
            fprintf(f, "bind %s %s %d\n", binding_name(IN->binds[a][s], name, sizeof(name)), k_action_names[a], s);
        }
}

void input_init(void) {
    IN = (InputSystem *)calloc(1, sizeof(InputSystem));
    IN->sens = cvar_reg_float("cl_sensitivity", 2.5f, 0.05f, 20.0f, CVAR_ARCHIVE, "mouse sensitivity (X)");
    IN->sens_y = cvar_reg_float("cl_sensitivity_y", 2.5f, 0.05f, 20.0f, CVAR_ARCHIVE, "mouse sensitivity (Y)");
    IN->ads_mult = cvar_reg_float("cl_ads_sensitivity", 0.7f, 0.1f, 2.0f, CVAR_ARCHIVE, "sensitivity multiplier while aiming");
    IN->invert_y = cvar_reg_bool("cl_invert_y", false, CVAR_ARCHIVE, "invert mouse/stick Y");
    IN->pad_sens = cvar_reg_float("pad_sensitivity", 3.0f, 0.1f, 10.0f, CVAR_ARCHIVE, "gamepad look speed");
    IN->pad_deadzone = cvar_reg_float("pad_deadzone", 0.15f, 0.0f, 0.9f, CVAR_ARCHIVE, "gamepad stick deadzone");
    input_reset_bindings();
    cmd_register("bind", cmd_bind, "bind <key> <action> [slot]");
    cmd_register("unbindall", cmd_unbindall, "clear all bindings");
    cmd_register("bindlist", cmd_bindlist, "list bindings");
    for (int a = 0; a < ACT_COUNT; a++) {
        char n[64];
        snprintf(n, sizeof(n), "+%s", k_action_names[a]);
        cmd_register(n, cmd_plus, "press action (scripting)");
        snprintf(n, sizeof(n), "-%s", k_action_names[a]);
        cmd_register(n, cmd_plus, "release action (scripting)");
    }
    config_add_writer(write_binds);
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}

void input_shutdown(void) {
    if (!IN) return;
    if (IN->pad) SDL_CloseGamepad(IN->pad);
    free(IN);
    IN = NULL;
}

static bool binding_active(Binding b) {
    switch (b.type) {
    case BIND_KEY: return IN->keys[b.code];
    case BIND_MOUSE: return b.code < 8 && IN->mouse_buttons[b.code];
    case BIND_PAD_BUTTON: return IN->pad && SDL_GetGamepadButton(IN->pad, (SDL_GamepadButton)b.code);
    case BIND_PAD_AXIS: {
        if (!IN->pad) return false;
        float v = SDL_GetGamepadAxis(IN->pad, (SDL_GamepadAxis)(b.code / 2)) / 32767.0f;
        return (b.code & 1) ? v < -0.5f : v > 0.5f;
    }
    default: return false;
    }
}

static void finish_capture(Binding b) {
    IN->binds[IN->capture_action][IN->capture_slot] = b;
    config_mark_dirty();
    IN->capturing = false;
    char n[48];
    log_info("bound %s to %s", binding_name(b, n, sizeof(n)), k_action_names[IN->capture_action]);
}

void input_begin_frame(void) { IN->wheel_accum = 0; }

void input_handle_event(const SDL_Event *ev) {
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        if (ev->key.repeat) break;
        if (IN->capturing) {
            if (ev->key.scancode == SDL_SCANCODE_ESCAPE) IN->capturing = false;
            else finish_capture(key(ev->key.scancode));
            break;
        }
        IN->keys[ev->key.scancode] = true;
        break;
    case SDL_EVENT_KEY_UP: IN->keys[ev->key.scancode] = false; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (IN->capturing) { finish_capture(mouse(ev->button.button)); break; }
        if (ev->button.button < 8) IN->mouse_buttons[ev->button.button] = IN->game_focus;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.button < 8) IN->mouse_buttons[ev->button.button] = false;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (IN->game_focus) { IN->mouse_accum.x += ev->motion.xrel; IN->mouse_accum.y += ev->motion.yrel; }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (IN->capturing) { finish_capture(wheel(ev->wheel.y > 0 ? 1 : -1)); break; }
        if (IN->game_focus) IN->wheel_accum += ev->wheel.y;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!IN->pad) {
            IN->pad = SDL_OpenGamepad(ev->gdevice.which);
            if (IN->pad) log_info("gamepad connected: %s", SDL_GetGamepadName(IN->pad));
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (IN->pad && SDL_GetGamepadID(IN->pad) == ev->gdevice.which) {
            SDL_CloseGamepad(IN->pad);
            IN->pad = NULL;
            log_info("gamepad disconnected");
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (IN->capturing) finish_capture(padb((SDL_GamepadButton)ev->gbutton.button));
        break;
    default: break;
    }
}

static float apply_deadzone(float v, float dz) {
    float a = fabsf(v);
    if (a < dz) return 0.0f;
    return signf(v) * (a - dz) / (1.0f - dz);
}

void input_end_frame(float dt) {
    // Recompute action state and count rising edges.
    for (int a = 0; a < ACT_COUNT; a++) {
        bool down = false;
        if (IN->game_focus) {
            for (int s = 0; s < BINDS_PER_ACTION; s++) {
                Binding b = IN->binds[a][s];
                if (b.type == BIND_WHEEL) {
                    if ((b.code > 0 && IN->wheel_accum > 0) || (b.code < 0 && IN->wheel_accum < 0)) IN->press_count[a]++;
                } else if (binding_active(b)) {
                    down = true;
                }
            }
        }
        if (down && !IN->phys_down[a]) IN->press_count[a]++;
        IN->phys_down[a] = down;
    }

    // Look
    float scale = input_aim_sensitivity_scale();
    float k = 0.0022f * scale; // radians per pixel at sensitivity 1
    vec2 look = v2(-IN->mouse_accum.x * k * IN->sens->f, -IN->mouse_accum.y * k * IN->sens_y->f);
    IN->mouse_accum = v2(0, 0);
    vec2 move = v2(0, 0);
    if (input_down(ACT_FORWARD)) move.y += 1;
    if (input_down(ACT_BACK)) move.y -= 1;
    if (input_down(ACT_RIGHT)) move.x += 1;
    if (input_down(ACT_LEFT)) move.x -= 1;
    if (IN->pad && IN->game_focus) {
        float dz = IN->pad_deadzone->f;
        float lx = apply_deadzone(SDL_GetGamepadAxis(IN->pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f, dz);
        float ly = apply_deadzone(SDL_GetGamepadAxis(IN->pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f, dz);
        float rx = apply_deadzone(SDL_GetGamepadAxis(IN->pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f, dz);
        float ry = apply_deadzone(SDL_GetGamepadAxis(IN->pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f, dz);
        move.x += lx;
        move.y -= ly;
        // Quadratic response curve for finer aim.
        float s = IN->pad_sens->f * scale * dt;
        look.x -= rx * fabsf(rx) * s;
        look.y -= ry * fabsf(ry) * s;
    }
    if (IN->invert_y->i) look.y = -look.y;
    float ml = v2_len(move);
    if (ml > 1.0f) move = v2_scale(move, 1.0f / ml);
    IN->frame.look_delta = look;
    IN->frame.move = move;
}

bool input_down(Action a) { return IN->phys_down[a] || IN->script_down[a]; }

bool input_consume_press(Action a) {
    if (IN->press_count[a] > 0) { IN->press_count[a] = 0; return true; }
    return false;
}

bool input_peek_press(Action a) { return IN->press_count[a] > 0; }
InputFrame input_frame(void) { return IN->frame; }
void input_set_ads(bool ads) { IN->ads = ads; }
void input_set_ads_fov_scale(float s) { IN->ads_fov_scale = s > 0 ? s : 1.0f; }
// While aiming: cl_ads_sensitivity x the sight FOV scaling (ads_sens_mode, game_sights.c).
float input_aim_sensitivity_scale(void) { return IN->ads ? IN->ads_mult->f * (IN->ads_fov_scale > 0 ? IN->ads_fov_scale : 1.0f) : 1.0f; }

void input_set_game_focus(bool focus) {
    if (IN->game_focus == focus) return;
    IN->game_focus = focus;
    if (g_engine.window) SDL_SetWindowRelativeMouseMode(g_engine.window, focus);
    if (!focus) {
        memset(IN->mouse_buttons, 0, sizeof(IN->mouse_buttons));
        IN->mouse_accum = v2(0, 0);
    }
}

bool input_game_focus(void) { return IN->game_focus; }
bool input_gamepad_connected(void) { return IN->pad != NULL; }
Binding input_get_binding(Action a, int slot) { return IN->binds[a][slot]; }
void input_set_binding(Action a, int slot, Binding b) { IN->binds[a][slot] = b; config_mark_dirty(); }

void input_begin_capture(Action a, int slot) {
    IN->capturing = true;
    IN->capture_action = a;
    IN->capture_slot = slot;
}

bool input_capturing(void) { return IN->capturing; }
