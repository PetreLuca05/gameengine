// Action-mapped input: keyboard / mouse / gamepad, rebindable, scriptable via "+action"/"-action".
#ifndef DG_INPUT_H
#define DG_INPUT_H

#include "core/base.h"
#include "core/dgmath.h"

union SDL_Event;

typedef enum Action {
    ACT_FORWARD, ACT_BACK, ACT_LEFT, ACT_RIGHT,
    ACT_JUMP, ACT_CROUCH, ACT_SPRINT, ACT_WALK,
    ACT_USE, ACT_ATTACK, ACT_AIM, ACT_RELOAD, ACT_DROP,
    ACT_INVENTORY, ACT_NETSTATS,
    ACT_SLOT1, ACT_SLOT2, ACT_SLOT3, ACT_SLOT4, ACT_SLOT5, ACT_SLOT6, ACT_SLOT7, ACT_SLOT8, ACT_SLOT9,
    ACT_NEXT_ITEM, ACT_PREV_ITEM, ACT_FIREMODE,
    ACT_COUNT
} Action;

typedef enum BindType { BIND_NONE, BIND_KEY, BIND_MOUSE, BIND_WHEEL, BIND_PAD_BUTTON, BIND_PAD_AXIS } BindType;

typedef struct Binding {
    BindType type;
    int code;  // scancode / mouse button / wheel dir (+1/-1) / pad button / pad axis*2+(neg?1:0)
} Binding;

#define BINDS_PER_ACTION 2

typedef struct InputFrame {
    vec2 look_delta;   // radians this frame (already scaled by sensitivity, invert applied)
    vec2 move;         // -1..1 (x right, y forward)
} InputFrame;

void input_init(void);
void input_shutdown(void);
void input_begin_frame(void);
void input_handle_event(const union SDL_Event *ev);
void input_end_frame(float dt);   // computes look/move for this frame

bool input_down(Action a);
// Edge count since last consume; consumed by the sim tick that builds a command.
bool input_consume_press(Action a);
bool input_peek_press(Action a);
InputFrame input_frame(void);
float input_aim_sensitivity_scale(void);
void input_set_ads(bool ads);

// Mouse capture / game focus
void input_set_game_focus(bool focus);   // relative mouse when true
bool input_game_focus(void);
bool input_gamepad_connected(void);

// Binding management
const char *action_name(Action a);
const char *action_label(Action a);
Binding input_get_binding(Action a, int slot);
void input_set_binding(Action a, int slot, Binding b);
const char *binding_name(Binding b, char *buf, size_t size);
bool binding_from_name(const char *name, Binding *out);
void input_reset_bindings(void);
void input_bind_missing_defaults(void);  // after loading a config saved before new actions existed
// Rebind capture: the next pressed input becomes the binding. Escape cancels.
void input_begin_capture(Action a, int slot);
bool input_capturing(void);

#endif
