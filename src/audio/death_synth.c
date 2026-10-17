// Procedural tinnitus + heartbeat (docs/REVIVE.md): see death_synth.h.
#include "death_synth.h"
#include <math.h>

#define TWO_PI 6.28318530717958647692

void death_synth_init(DeathSynth *s) {
    memset(s, 0, sizeof(*s));
    s->target.ring_freq = 3600.0f;
    s->target.heart_bpm = 70.0f;
    s->smooth_tau = 0.025f;
    s->beat_t = 10.0;
}

void death_synth_set(DeathSynth *s, const DeathSynthParams *p) { s->target = *p; }

bool death_synth_silent(const DeathSynth *s) {
    return s->ring_gain < 1e-5f && s->heart_gain < 1e-5f && s->target.ring_gain <= 0.0f && s->target.heart_gain <= 0.0f;
}

// One heartbeat "lub-dub": two damped low sines.
static float heart_sample(double t) {
    float v = 0.0f;
    if (t < 0.35) {
        float a = (float)(1.0 - exp(-t / 0.004));            // 4 ms attack
        v += a * expf((float)(-t / 0.07)) * sinf((float)(TWO_PI * 52.0 * t));
    }
    double t2 = t - 0.2;
    if (t2 >= 0.0 && t2 < 0.35) {
        float a = (float)(1.0 - exp(-t2 / 0.004));
        v += 0.65f * a * expf((float)(-t2 / 0.06)) * sinf((float)(TWO_PI * 44.0 * t2));
    }
    return v;
}

void death_synth_render(DeathSynth *s, float *out, int frames, int channels, float sample_rate) {
    if (frames <= 0 || sample_rate <= 0) return;
    if (death_synth_silent(s)) return;
    DeathSynthParams p = s->target;
    float dt = 1.0f / sample_rate;
    float k = 1.0f - expf(-dt / fmaxf(s->smooth_tau, 1e-4f));
    double beat_len = 60.0 / (double)fmaxf(p.heart_bpm, 20.0f);
    for (int f = 0; f < frames; f++) {
        s->ring_gain += (p.ring_gain - s->ring_gain) * k;
        s->heart_gain += (p.heart_gain - s->heart_gain) * k;
        s->warble_phase += TWO_PI * p.warble_hz * dt;
        if (s->warble_phase > TWO_PI) s->warble_phase -= TWO_PI;
        double freq = p.ring_freq * (1.0 + p.warble_depth * sin(s->warble_phase));
        s->ring_phase += TWO_PI * freq * dt;
        if (s->ring_phase > TWO_PI) s->ring_phase -= TWO_PI;
        float ring = (float)(sin(s->ring_phase) + 0.08 * sin(2.0 * s->ring_phase));   // a faint second partial
        s->beat_t += dt;
        if (s->beat_t >= beat_len) s->beat_t -= beat_len;
        float heart = heart_sample(s->beat_t);
        float v = ring * s->ring_gain * 0.5f + heart * s->heart_gain * 0.8f;
        for (int c = 0; c < channels; c++) out[f * channels + c] += v;
    }
}
