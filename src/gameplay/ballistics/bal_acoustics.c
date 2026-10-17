// Gunfire acoustics (docs/BALLISTICS.md "Sound").
#include "gameplay/ballistics/bal_acoustics.h"
#include <math.h>

bool bal_crack_segment(vec3 p0, vec3 p1, float t0, float speed, float sound_speed, vec3 listener,
                       float *arrival, float *miss) {
    if (speed <= sound_speed) return false;
    vec3 seg = v3_sub(p1, p0);
    float len = v3_len(seg);
    if (len < 1e-4f) return false;
    vec3 u = v3_scale(seg, 1.0f / len);
    vec3 to = v3_sub(listener, p0);
    float x = v3_dot(to, u);                         // listener's along-track coordinate
    float r = sqrtf(fmaxf(v3_len2(to) - x * x, 0.0f));
    float m = sound_speed / speed;                   // cos of the Mach angle measured from the track
    float s = x - r * m / sqrtf(1.0f - m * m);       // emission point along the track
    if (s < 0.0f || s >= len) return false;
    float d = sqrtf((x - s) * (x - s) + r * r);
    *arrival = t0 + s / speed + d / sound_speed;
    *miss = r;
    return true;
}

bool bal_whizz_segment(vec3 p0, vec3 p1, float t0, float speed, float sound_speed, vec3 listener,
                       float *arrival, float *miss) {
    vec3 seg = v3_sub(p1, p0);
    float len = v3_len(seg);
    if (len < 1e-4f || speed <= 0.0f) return false;
    vec3 u = v3_scale(seg, 1.0f / len);
    vec3 to = v3_sub(listener, p0);
    float x = v3_dot(to, u);
    if (x < 0.0f || x >= len) return false;
    float r = sqrtf(fmaxf(v3_len2(to) - x * x, 0.0f));
    *arrival = t0 + x / speed + r / sound_speed;
    *miss = r;
    return true;
}

float bal_report_delay(float distance, float sound_speed) { return distance / sound_speed; }

float bal_lowpass_hz(float distance) {
    float f = 20000.0f * powf(10.0f, -distance / 900.0f);
    return fmaxf(f, 1500.0f);
}

float bal_crack_gain(float miss) { return fminf(1.0f, powf(fmaxf(miss, 1.0f), -0.75f) * 4.0f); }

int bal_crack_variant(float miss) {
    // N-wave length ≈ 0.2 ms · r^(1/4): about 0.2 ms at 1 m, 0.45 ms at 25 m, 0.63 ms at 100 m.
    float dur = 0.2f * powf(fmaxf(miss, 1.0f), 0.25f);
    return dur < 0.35f ? 0 : (dur < 0.55f ? 1 : 2);
}

float bal_db_gain(float db) { return powf(10.0f, db / 20.0f); }
