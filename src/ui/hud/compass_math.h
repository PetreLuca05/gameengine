// Compass math (docs/UI_WHEELS_HUD.md "Compass"). Pure, inline (tests/test_hud.c).
// World: north = -Z, east = +X. View yaw 0 looks north and grows turning left, so the heading
// (0 north, 90 east, clockwise) is -yaw in degrees.
#ifndef DG_COMPASS_MATH_H
#define DG_COMPASS_MATH_H

#include "core/base.h"
#include "core/dgmath.h"
#include <math.h>
#include <stdio.h>

DG_INLINE float compass_wrap360(float d) {
    d = fmodf(d, 360.0f);
    return d < 0 ? d + 360.0f : d;
}
DG_INLINE float compass_wrap180(float d) {
    d = compass_wrap360(d);
    return d > 180.0f ? d - 360.0f : d;
}
DG_INLINE float compass_heading_deg(float yaw_rad) { return compass_wrap360(-yaw_rad * DG_RAD2DEG); }
// Bearing from `from` to `to` (0 north, 90 east).
DG_INLINE float compass_bearing_deg(vec3 from, vec3 to) { return compass_wrap360(atan2f(to.x - from.x, -(to.z - from.z)) * DG_RAD2DEG); }
// Signed degrees from the heading to a bearing (-180..180, + = to the right).
DG_INLINE float compass_offset_deg(float bearing, float heading) { return compass_wrap180(bearing - heading); }
// Strip label for a whole degree: N / NE / E ... on multiples of 45, else the number ("15").
DG_INLINE void compass_label(int deg, char *out, int size) {
    static const char *const k[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    deg = ((deg % 360) + 360) % 360;
    if (deg % 45 == 0) snprintf(out, (size_t)size, "%s", k[deg / 45]);
    else snprintf(out, (size_t)size, "%d", deg);
}
// Position line: "X 12.3  Z -45.6  H 1.8" (height above sea level = y).
DG_INLINE void compass_coords_text(vec3 p, char *out, int size) {
    snprintf(out, (size_t)size, "X %.1f   Z %.1f   H %.1f", (double)p.x, (double)p.z, (double)p.y);
}

#endif
