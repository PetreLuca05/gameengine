// Nominal first-person eye calibration (docs/CAMERA_HOLDPOINT.md "Rule 2"): runs the shared animation
// (the same controller and post-pass every character uses) through every posture x hold style x
// speed x view pitch sample and records the head eye's mean over the clip cycles - the head with all
// animation-induced motion removed - into the camera's table (src/player/camera_fp.h).
#ifndef DG_EYE_NOMINAL_H
#define DG_EYE_NOMINAL_H

#include "anim/anim_library.h"
#include "player/camera_fp.h"

struct ItemDB;

// warm / sample: ticks at 60 Hz before and while averaging (0: 90 / 120). Returns false without the
// player character or its "camera" socket.
bool eye_nominal_calibrate(const AnimLibrary *L, const CharacterDef *def, const struct ItemDB *db, EyeNominalTable *out, int warm, int sample);

#endif
