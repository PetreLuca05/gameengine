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
    BTN_VEHICLE = 1 << 11,    // enter / exit a vehicle (edge)
    BTN_LIGHTS  = 1 << 12,    // vehicle headlights on (state, driver)
    BTN_HORN    = 1 << 13,    // vehicle horn (state, driver)
    BTN_PHONE   = 1 << 14,    // the built-in phone is out (state, docs/PHONE.md)
    BTN_VEH_CAM = 1 << 15,    // vehicle third-person camera chosen (state; spectators follow it)
};

typedef struct UserCmd {
    u32 seq;           // client sequence number (one per client tick)
    u32 view_tick;     // server tick the client was rendering (lag compensation), *256 fixed point
    float yaw, pitch;  // absolute view angles (radians)
    i8 move_x, move_y; // -127..127 (right, forward)
    u16 buttons;
    u8 slot;           // selected hotbar slot
    u8 seat_req;       // vehicles: requested seat + 1 (keys 1-4 while seated), 0 = none
    u32 use_target;    // EntityId the client is focusing
    u32 phone_rot;     // phone held by a paired real phone: packed rotation relative to the view yaw (docs/PHONE.md)
    u8 phone_flags;    // CHAR_PHONE_* (anim/character_rt.h): landscape / driven / recording
    u8 phone_zoom;     // phone camera zoom x 32 (spectators see the same lens)
    // Aim model (docs/AIM.md): the held weapon's muzzle socket in camera space relative to the eye
    // (mm, from the pose the shooter sees; validated by the host), and the shooter's predicted aim
    // point distance of its last shot (cm, 0 = none; host discrepancy log).
    i16 muzzle_cam[3];
    u16 aim_dist;
    // The first-person camera (the head's "camera" socket) relative to the simulated eye, in the same
    // camera space and units: the camera ray starts where the player's view does (validated by the host).
    i16 eye_cam[3];
} UserCmd;

#endif
