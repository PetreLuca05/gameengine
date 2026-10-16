// Real phone orientation -> in-game phone rotation (pure, unit tested in tests/test_phone.c).
//
// Frames
//   device   W3C DeviceOrientation: x right, y towards the top of the screen, z out of the screen
//            (towards the user). The page sends q_dev = Rz(alpha) Rx(beta) Ry(gamma): device -> earth.
//   earth    x east, y north, z up.
//   game     x right (east), y up, -z forward (north before calibration). game = Rx(-90 deg) earth.
//   item     the phone item: x right, y up (top of the screen), +z out of the screen, -z = the back
//            camera lens. Identical to the device frame, so q_game = Rx(-90) * q_dev places the item.
//   yaw rel  relative to the player's view yaw: calibration stores the phone's heading when held
//            upright facing forward and removes it (yaw only: gravity keeps pitch / roll absolute).
// Pipeline (phone.c): q_dev -> phone_orient_game -> calibration -> sensitivity / invert ->
// 1-euro filter -> camera space (view pitch removed) -> reach cone clamp -> hold point override.
#ifndef DG_PHONE_ORIENT_H
#define DG_PHONE_ORIENT_H

#include "core/base.h"
#include "core/dgmath.h"

// W3C DeviceOrientationEvent alpha / beta / gamma (degrees) -> device-to-earth quaternion.
quat phone_device_quat(float alpha_deg, float beta_deg, float gamma_deg);
// Device-to-earth -> item-to-game (uncalibrated).
quat phone_orient_game(quat q_dev);
// Heading (yaw, radians, quat_get_yaw convention) of a phone pose: the lens direction when it
// points roughly horizontally, else the top of the screen (phone held flat).
float phone_heading(quat q_game);
// Removes a calibration heading: the result is relative to the player's view yaw.
quat phone_apply_calibration(quat q_game, float calib_yaw);

// Yaw / pitch / roll (radians) of q = Ry(yaw) Rx(pitch) Rz(roll) (quat_euler order) and back.
vec3 phone_euler_ypr(quat q);   // x yaw, y pitch, z roll
quat phone_from_ypr(vec3 ypr);
// Sensitivity (angle multiplier) and per-axis inversion, applied on the yaw / pitch / roll angles.
quat phone_shape(quat q_rel, float sensitivity, bool invert_yaw, bool invert_pitch, bool invert_roll);

// Reach cone in camera space (degrees): the arm can hold the phone anywhere inside it. Angles
// past knee * limit are compressed smoothly towards the limit, which is never exceeded.
typedef struct PhoneReach { float yaw, pitch_up, pitch_down, roll, knee; } PhoneReach;
PhoneReach phone_reach_default(void);
quat phone_clamp_reach(quat q_cam, const PhoneReach *r);
bool phone_in_reach(quat q_cam, const PhoneReach *r, float tol_deg);
// Yaw-relative rotation -> camera space for a view pitch (radians).
quat phone_to_camera(quat q_rel, float view_pitch);

// 1-euro filter on a rotation (Casiez et al. 2012): cutoff rises with the angular speed, so slow
// motion is smoothed and fast motion stays responsive.
typedef struct PhoneOneEuro { bool init; quat x; float speed; float min_cutoff, beta, d_cutoff; } PhoneOneEuro;
void phone_one_euro_init(PhoneOneEuro *f, float min_cutoff_hz, float beta, float d_cutoff_hz);
quat phone_one_euro(PhoneOneEuro *f, quat q, float dt);

// Smallest-three quaternion packing (2 + 3 x 10 bits): replicated phone_rot, ~0.1 deg error.
u32 phone_quat_pack(quat q);
quat phone_quat_unpack(u32 v);
// int16 x 4 (the page's binary orientation sample) <-> quaternion.
quat phone_quat_from_i16(const i16 v[4]);
void phone_quat_to_i16(quat q, i16 out[4]);

#endif
