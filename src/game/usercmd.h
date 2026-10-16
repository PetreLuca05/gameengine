// UserCmd: one simulation tick of player input. Clients send these to the host;
// the host's local player feeds them straight into the simulation.
#ifndef DG_USERCMD_H
#define DG_USERCMD_H

#include "core/base.h"

enum CmdButtons {
    BTN_JUMP    = 1 << 0,
    BTN_CROUCH  = 1 << 1,
    BTN_SPRINT  = 1 << 2,
    BTN_USE     = 1 << 3,
    BTN_ATTACK  = 1 << 4,
    BTN_AIM     = 1 << 5,
    BTN_RELOAD  = 1 << 6,
    BTN_WALK    = 1 << 7,
    BTN_NOCLIP  = 1 << 8,   // dev
    BTN_FIREMODE = 1 << 9,  // cycle fire mode (edge)
    BTN_FLASHLIGHT = 1 << 10, // flashlight on (state, replicated as EF_FLASHLIGHT)
};

typedef struct UserCmd {
    u32 seq;           // client sequence number (one per client tick)
    u32 view_tick;     // server tick the client was rendering (lag compensation), *256 fixed point
    float yaw, pitch;  // absolute view angles (radians)
    i8 move_x, move_y; // -127..127 (right, forward)
    u16 buttons;
    u8 slot;           // selected hotbar slot
    u8 pad;
    u32 use_target;    // EntityId the client is focusing
} UserCmd;

#endif
