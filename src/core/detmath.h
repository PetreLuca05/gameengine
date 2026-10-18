// Cross-platform deterministic float math (docs/PLATFORM_MACOS.md "Determinism"): sinf / expf / powf
// ... give different last bits in Apple's libm, glibc and the MSVC CRT, so a module that must be
// bit-identical between a Mac and a PC (ballistics, recoil, sway, fire animation, aim: host and
// predicting client) calls these instead. Each one evaluates in double with only IEEE-exact
// operations (+ - * /, sqrt, frexp / ldexp, fmod, floor) and fixed polynomials, then rounds to float
// once: the same bits on x86_64 / arm64, MSVC / clang / gcc, given no FMA contraction (the build
// uses -ffp-contract=off, MSVC /fp:precise). Accuracy is within 1 ulp of the correctly rounded float
// (tests/test_detmath.c). sqrtf is already exact everywhere and stays.
#ifndef DG_DETMATH_H
#define DG_DETMATH_H

float dm_sinf(float x);
float dm_cosf(float x);
float dm_tanf(float x);
float dm_atanf(float x);
float dm_atan2f(float y, float x);
float dm_asinf(float x);
float dm_acosf(float x);
float dm_expf(float x);
float dm_exp2f(float x);
float dm_logf(float x);
float dm_log2f(float x);
float dm_powf(float x, float y);
float dm_cbrtf(float x);
double dm_exp(double x);   // the double exp of the same kernel (about 1e-15 relative)

#endif
