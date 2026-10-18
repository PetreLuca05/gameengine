#include "syn_dsp.h"
#include <math.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#define SYN_X86 1
#endif

u32 syn_ftz_enable(void) {
#if SYN_X86
    u32 prev = _mm_getcsr();
    _mm_setcsr(prev | 0x8040u);   // FTZ (bit 15) + DAZ (bit 6)
    return prev;
#elif defined(__aarch64__)
    u64 prev;
    __asm__ volatile("mrs %0, fpcr" : "=r"(prev));
    __asm__ volatile("msr fpcr, %0" : : "r"(prev | (1ull << 24)));   // FZ
    return (u32)prev;
#else
    return 0;
#endif
}

void syn_ftz_restore(u32 prev) {
#if SYN_X86
    _mm_setcsr(prev);
#elif defined(__aarch64__)
    u64 v = prev;
    __asm__ volatile("msr fpcr, %0" : : "r"(v));
#else
    (void)prev;
#endif
}

// ------------------------------------------------------------------ biquad

void syn_biquad_design(SynBiquad *f, SynFilterMode mode, float hz, float q, float gain_db, float sr) {
    hz = syn_clampf(hz, 5.0f, sr * 0.49f);
    q = q > 0.05f ? q : 0.05f;
    double w0 = 2.0 * 3.14159265358979 * hz / sr, cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * q), A = pow(10.0, gain_db / 40.0);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (mode) {
    case SYN_LP: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case SYN_HP: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case SYN_BP: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;   // 0 dB peak
    case SYN_NOTCH: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case SYN_AP: b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case SYN_PEAK: b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
    case SYN_LSHELF: {
        double s = 2 * sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
    } break;
    case SYN_HSHELF: {
        double s = 2 * sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
    } break;
    }
    f->b0 = (float)(b0 / a0);
    f->b1 = (float)(b1 / a0);
    f->b2 = (float)(b2 / a0);
    f->a1 = (float)(a1 / a0);
    f->a2 = (float)(a2 / a0);
}

void syn_biquad_run(SynBiquad *f, const float *in, float *out, int n) {
    float z1 = f->z1, z2 = f->z2;
    const float b0 = f->b0, b1 = f->b1, b2 = f->b2, a1 = f->a1, a2 = f->a2;
    for (int i = 0; i < n; i++) {
        float x = in[i], y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        out[i] = y;
    }
    f->z1 = z1;
    f->z2 = z2;
}

float syn_biquad_mag(const SynBiquad *f, float hz, float sr) {
    double w = 2.0 * 3.14159265358979 * hz / sr;
    double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    double nr = f->b0 + f->b1 * c1 + f->b2 * c2, ni = -(f->b1 * s1 + f->b2 * s2);
    double dr = 1.0 + f->a1 * c1 + f->a2 * c2, di = -(f->a1 * s1 + f->a2 * s2);
    return (float)sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

// ------------------------------------------------------------------ SVF

void syn_svf_design(SynSvf *f, float hz, float q, float sr) {
    hz = syn_clampf(hz, 5.0f, sr * 0.49f);
    f->g = tanf(SYN_PI * hz / sr);
    f->k = 1.0f / (q > 0.05f ? q : 0.05f);
    f->a1 = 1.0f / (1.0f + f->g * (f->g + f->k));
    f->a2 = f->g * f->a1;
    f->a3 = f->g * f->a2;
}

void syn_svf_run(SynSvf *f, SynFilterMode mode, const float *in, float *out, int n) {
    float ic1 = f->ic1, ic2 = f->ic2;
    for (int i = 0; i < n; i++) {
        float v0 = in[i];
        float v3 = v0 - ic2;
        float v1 = f->a1 * ic1 + f->a2 * v3;
        float v2 = ic2 + f->a2 * ic1 + f->a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        float y;
        switch (mode) {
        case SYN_HP: y = v0 - f->k * v1 - v2; break;
        case SYN_BP: y = v1; break;
        case SYN_NOTCH: y = v0 - f->k * v1; break;
        default: y = v2; break;
        }
        out[i] = y;
    }
    f->ic1 = ic1;
    f->ic2 = ic2;
}

// ------------------------------------------------------------------ smoothing

void syn_smooth_init(SynSmooth *s, float value, float tau_s, float sr) {
    s->cur = s->target = value;
    s->coef = tau_s > 0 ? 1.0f - expf(-(float)SYN_BLOCK / (tau_s * sr)) : 1.0f;
}

void syn_smooth_block(SynSmooth *s, float *v0, float *v1) {
    *v0 = s->cur;
    float d = s->target - s->cur;
    s->cur = fabsf(d) < 1e-7f * (1.0f + fabsf(s->target)) ? s->target : s->cur + s->coef * d;
    *v1 = s->cur;
}

void syn_ramp_mul(const float *in, float *out, int n, float v0, float v1) {
    float step = (v1 - v0) / (float)n;
    for (int i = 0; i < n; i++) out[i] = in[i] * (v0 + step * (float)(i + 1));
}

void syn_ramp_mac(const float *in, float *out, int n, float v0, float v1) {
    float step = (v1 - v0) / (float)n;
    for (int i = 0; i < n; i++) out[i] += in[i] * (v0 + step * (float)(i + 1));
}

// ------------------------------------------------------------------ noise

void syn_noise_run(SynNoise *s, SynNoiseColor c, float *out, int n, float amp, float density, float sr) {
    switch (c) {
    case SYN_PINK:
        // Paul Kellet's refined pink filter (-3 dB / octave within 0.05 dB above 9 Hz).
        for (int i = 0; i < n; i++) {
            float w = syn_rngbi(&s->rng);
            float *b = s->b;
            b[0] = 0.99886f * b[0] + w * 0.0555179f;
            b[1] = 0.99332f * b[1] + w * 0.0750759f;
            b[2] = 0.96900f * b[2] + w * 0.1538520f;
            b[3] = 0.86650f * b[3] + w * 0.3104856f;
            b[4] = 0.55000f * b[4] + w * 0.5329522f;
            b[5] = -0.7616f * b[5] - w * 0.0168980f;
            out[i] = amp * 0.11f * (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f);
            b[6] = w * 0.115926f;
        }
        break;
    case SYN_BROWN:
        for (int i = 0; i < n; i++) {
            s->brown = 0.996f * s->brown + 0.06f * syn_rngbi(&s->rng);
            out[i] = amp * 1.5f * s->brown;   // ~0.6 RMS, like the other colours
        }
        break;
    case SYN_VELVET: {
        // Sparse +-1 impulses, one per period at a random offset (Karjalainen / Jarvelainen).
        float period = sr / (density > 1.0f ? density : 1.0f);
        for (int i = 0; i < n; i++) {
            out[i] = 0.0f;
            s->velvet_pos -= 1.0f;
            if (s->velvet_pos <= 0.0f) {
                out[i] = amp * (syn_rng_u32(&s->rng) & 1u ? 1.0f : -1.0f);
                s->velvet_pos += period * (0.5f + syn_rng01(&s->rng));
            }
        }
    } break;
    default:
        for (int i = 0; i < n; i++) out[i] = amp * syn_rngbi(&s->rng);
        break;
    }
}
