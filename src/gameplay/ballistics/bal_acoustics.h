// Gunfire acoustics (pure): when and how loud a listener hears the supersonic crack, the subsonic
// whizz, the muzzle report and the impact. Each client evaluates its own listener against the
// bullets it simulates, so nothing here goes over the network. docs/BALLISTICS.md "Sound".
#ifndef DG_BAL_ACOUSTICS_H
#define DG_BAL_ACOUSTICS_H

#include "core/base.h"
#include "core/dgmath.h"

// Crack: the Mach cone of a supersonic bullet sweeping the listener. For the straight segment
// p0 -> p1 flown at `speed` starting at time t0, the shock arrives at
//   t = min over τ of  τ + |L - p(τ)| / c,
// reached where the line to the listener makes the Mach angle (cos = c / v) with the track.
// Returns true when that minimum lies inside this segment; *arrival is the absolute time, *miss
// the perpendicular miss distance.
bool bal_crack_segment(vec3 p0, vec3 p1, float t0, float speed, float sound_speed, vec3 listener,
                       float *arrival, float *miss);
// Whizz (subsonic): the closest approach in this segment, heard r / c later. true when the closest
// point is inside the segment.
bool bal_whizz_segment(vec3 p0, vec3 p1, float t0, float speed, float sound_speed, vec3 listener,
                       float *arrival, float *miss);

float bal_report_delay(float distance, float sound_speed);          // s
// Air absorption as a low-pass: 20 kHz near, falling a decade per 900 m, floor 1.5 kHz.
float bal_lowpass_hz(float distance);
// N-wave crack level vs miss distance (peak overpressure ∝ r^-3/4), 1 at 1 m, clamped to 1.
float bal_crack_gain(float miss);
// N-wave duration grows with r^(1/4): 0 short, 1 medium, 2 long sample.
int bal_crack_variant(float miss);
// Linear gain of a dB offset.
float bal_db_gain(float db);

#endif
