// Deterministic float math (detmath.h): double-precision kernels (Cody-Waite reduction + fixed
// polynomials), only IEEE-exact libm calls (sqrt, floor, frexp, ldexp, fmod), one rounding to float.
#include "detmath.h"
#include <math.h>

static const double PI = 3.14159265358979311600e+00;
static const double PIO2 = 1.57079632679489655800e+00;
static const double PIO4 = 7.85398163397448278999e-01;
static const double PIO6 = 5.23598775598298815659e-01;
static const double TWO_PI = 6.28318530717958623200e+00;
static const double TWO_OVER_PI = 6.36619772367581382433e-01;
// pi / 2 split (fdlibm pio2_1 / pio2_1t): k * PIO2_HI is exact for |k| < 2^20.
static const double PIO2_HI = 1.57079632673412561417e+00;
static const double PIO2_LO = 6.07710050650619224932e-11;
// ln 2 split (fdlibm ln2HI / ln2LO): k * LN2_HI is exact for |k| < 2^11.
static const double LN2_HI = 6.93147180369123816490e-01;
static const double LN2_LO = 1.90821492927058770002e-10;
static const double LN2 = 6.93147180559945286227e-01;
static const double LOG2E = 1.44269504088896338700e+00;
static const double SQRT_HALF = 7.07106781186547572737e-01;
static const double SQRT3 = 1.73205080756887719318e+00;
static const double TAN_PI_12 = 2.67949192431122696e-01;

static int dm_isnan(double x) { return x != x; }
static int dm_isinf(double x) { return x - x != x - x && x == x; }

// sin / cos of r, |r| <= pi / 4 (+ rounding): Taylor to r^13 / r^14 (truncation < 3e-14).
static double k_sin(double r) {
    double z = r * r;
    return r + r * z * (-1.0 / 6.0 + z * (1.0 / 120.0 + z * (-1.0 / 5040.0 + z * (1.0 / 362880.0 + z * (-1.0 / 39916800.0 + z * (1.0 / 6227020800.0))))));
}
static double k_cos(double r) {
    double z = r * r;
    return 1.0 + z * (-0.5 + z * (1.0 / 24.0 + z * (-1.0 / 720.0 + z * (1.0 / 40320.0 + z * (-1.0 / 3628800.0 + z * (1.0 / 479001600.0 + z * (-1.0 / 87178291200.0)))))));
}

// x = k * pi / 2 + r, |r| <= pi / 4; returns k mod 4.
static int reduce(double x, double *r) {
    if (fabs(x) > 1.0e6) x = fmod(x, TWO_PI);   // exact; angles this large carry no precision anyway
    double k = floor(x * TWO_OVER_PI + 0.5);
    *r = (x - k * PIO2_HI) - k * PIO2_LO;
    long long q = (long long)k;
    return (int)(((q % 4) + 4) % 4);
}

static double d_sin(double x) {
    double r;
    switch (reduce(x, &r)) {
    case 0: return k_sin(r);
    case 1: return k_cos(r);
    case 2: return -k_sin(r);
    default: return -k_cos(r);
    }
}
static double d_cos(double x) {
    double r;
    switch (reduce(x, &r)) {
    case 0: return k_cos(r);
    case 1: return -k_sin(r);
    case 2: return -k_cos(r);
    default: return k_sin(r);
    }
}

// atan of t in [0, 1]: atan(t) = pi / 6 + atan((t sqrt3 - 1) / (t + sqrt3)) above tan(pi / 12), then
// the alternating series to t^27 on |t| <= tan(pi / 12) (truncation < 2e-18).
static double k_atan01(double t) {
    double base = 0.0;
    if (t > TAN_PI_12) {
        t = (t * SQRT3 - 1.0) / (t + SQRT3);
        base = PIO6;
    }
    double z = t * t, s = 0.0;
    for (int n = 27; n >= 3; n -= 2) s = z * ((((n - 1) / 2) % 2 ? -1.0 : 1.0) / (double)n + s);
    return base + t + t * s;
}
static double d_atan(double x) {
    if (dm_isnan(x)) return x;
    double a = fabs(x), r;
    if (dm_isinf(a)) r = PIO2;
    else r = a > 1.0 ? PIO2 - k_atan01(1.0 / a) : k_atan01(a);
    return x < 0.0 ? -r : r;
}
static double d_atan2(double y, double x) {
    if (dm_isnan(x) || dm_isnan(y)) return x + y;
    int sy = signbit(y), sx = signbit(x);
    if (y == 0.0) return sx ? (sy ? -PI : PI) : y;
    if (x == 0.0) return sy ? -PIO2 : PIO2;
    if (dm_isinf(x) && dm_isinf(y)) {
        double r = sx ? 3.0 * PIO4 : PIO4;
        return sy ? -r : r;
    }
    if (dm_isinf(x)) return sx ? (sy ? -PI : PI) : (sy ? -0.0 : 0.0);
    double a = d_atan(fabs(y / x));
    if (sx) a = PI - a;
    return sy ? -a : a;
}

// exp: x = k ln2 + r, |r| <= ln2 / 2, Taylor to r^13 (truncation < 1e-17), scaled by 2^k (exact).
static double d_exp(double x) {
    if (dm_isnan(x)) return x;
    if (x > 709.78) return HUGE_VAL;
    if (x < -745.2) return 0.0;
    double k = floor(x * LOG2E + 0.5);
    double r = (x - k * LN2_HI) - k * LN2_LO;
    double p = 1.0 / 6227020800.0;
    static const double inv_fact[13] = { 1.0, 1.0, 1.0 / 2.0, 1.0 / 6.0, 1.0 / 24.0, 1.0 / 120.0, 1.0 / 720.0, 1.0 / 5040.0, 1.0 / 40320.0,
                                         1.0 / 362880.0, 1.0 / 3628800.0, 1.0 / 39916800.0, 1.0 / 479001600.0 };
    for (int n = 12; n >= 0; n--) p = p * r + inv_fact[n];
    return ldexp(p, (int)k);
}

// log: x = m 2^e, m in [sqrt(1/2), sqrt(2)), log m = 2 atanh(s), s = (m - 1) / (m + 1), series to
// s^21 (|s| <= 0.1716, truncation < 1e-18). Returns e and log m separately (log2 keeps e exact).
static double k_logm(double x, int *e) {
    double m = frexp(x, e);
    if (m < SQRT_HALF) { m *= 2.0; (*e)--; }
    double s = (m - 1.0) / (m + 1.0), z = s * s, p = 0.0;
    for (int n = 21; n >= 3; n -= 2) p = z * (1.0 / (double)n + p);
    return 2.0 * (s + s * p);
}
static double d_log(double x) {
    if (dm_isnan(x) || x < 0.0) return NAN;
    if (x == 0.0) return -HUGE_VAL;
    if (dm_isinf(x)) return x;
    int e;
    double lm = k_logm(x, &e);
    return (double)e * LN2_HI + ((double)e * LN2_LO + lm);
}

float dm_sinf(float x) { return (dm_isnan(x) || dm_isinf(x)) ? NAN : (float)d_sin((double)x); }
float dm_cosf(float x) { return (dm_isnan(x) || dm_isinf(x)) ? NAN : (float)d_cos((double)x); }
float dm_tanf(float x) {
    if (dm_isnan(x) || dm_isinf(x)) return NAN;
    double r;
    int q = reduce((double)x, &r);
    double s = k_sin(r), c = k_cos(r);
    return (float)((q & 1) ? -c / s : s / c);
}
float dm_atanf(float x) { return (float)d_atan((double)x); }
float dm_atan2f(float y, float x) { return (float)d_atan2((double)y, (double)x); }
float dm_asinf(float x) {
    double d = (double)x;
    if (dm_isnan(d) || fabs(d) > 1.0) return NAN;
    return (float)d_atan2(d, sqrt((1.0 - d) * (1.0 + d)));
}
float dm_acosf(float x) {
    double d = (double)x;
    if (dm_isnan(d) || fabs(d) > 1.0) return NAN;
    return (float)d_atan2(sqrt((1.0 - d) * (1.0 + d)), d);
}
double dm_exp(double x) { return d_exp(x); }
float dm_expf(float x) { return (float)d_exp((double)x); }
float dm_exp2f(float x) {
    double d = (double)x, k = floor(d);
    if (d == k && fabs(d) < 1100.0) return (float)ldexp(1.0, (int)k);   // exact powers of two
    return (float)d_exp(d * LN2);
}
float dm_logf(float x) { return (float)d_log((double)x); }
float dm_log2f(float x) {
    double d = (double)x;
    if (dm_isnan(d) || d < 0.0) return NAN;
    if (d == 0.0) return -INFINITY;
    if (dm_isinf(d)) return x;
    int e;
    double lm = k_logm(d, &e);
    return (float)((double)e + lm * LOG2E);
}
float dm_powf(float x, float y) {
    double dx = (double)x, dy = (double)y;
    if (dy == 0.0 || dx == 1.0) return 1.0f;
    if (dm_isnan(dx) || dm_isnan(dy)) return NAN;
    double sign = 1.0;
    if (dx < 0.0) {
        if (floor(dy) != dy) return NAN;   // negative base, fractional exponent
        if (fabs(dy) < 9007199254740992.0 && fmod(dy, 2.0) != 0.0) sign = -1.0;
        dx = -dx;
    }
    if (dx == 0.0) return (float)(dy > 0.0 ? sign * 0.0 : sign * HUGE_VAL);
    if (dm_isinf(dx)) return (float)(dy > 0.0 ? sign * HUGE_VAL : sign * 0.0);
    return (float)(sign * d_exp(dy * d_log(dx)));
}
float dm_cbrtf(float x) {
    double d = (double)x;
    if (d == 0.0 || dm_isnan(d) || dm_isinf(d)) return x;
    double a = fabs(d);
    int e;
    double m = frexp(a, &e);   // a = m 2^e, m in [0.5, 1)
    int r = ((e % 3) + 3) % 3;
    m = ldexp(m, r);           // m in [0.5, 4), e - r divisible by 3
    e -= r;
    double y = 0.6 + 0.4 * m;  // 6 Newton steps from a linear start: converged far below 1e-16
    for (int i = 0; i < 6; i++) y -= (y * y * y - m) / (3.0 * y * y);
    y = ldexp(y, e / 3);
    return (float)(d < 0.0 ? -y : y);
}
