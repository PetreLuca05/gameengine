// Recipe binding expressions (docs/AUDIO_PROCEDURAL.md "Bindings"): "lerp(barrel, 0.25, 22, 0.6, 14)
// * (device == 3 ? 0.4 : 1)" compiled once to a small stack program, evaluated on the game thread
// whenever a voice starts or its event values change. Variables are the recipe's declared inputs.
//   operators  + - * / % ^, comparisons < > <= >= == !=, && || !, c ? a : b, parentheses
//   functions  lerp(x, x0, y0, x1, y1) (clamped linear map), clamp(x, lo, hi), min, max, abs, sqrt,
//              cbrt, pow, exp, log, log10, db(x) (dB -> gain), todb(g), step(edge, x), floor,
//              sel(i, a, b, ...) (pick by index), mat_contact(m) / mat_noise(m) / mat_noise_hz(m) /
//              mat_modes(m) / mat_hard(m) (assets/audio/materials.json)
#ifndef DG_SYN_EXPR_H
#define DG_SYN_EXPR_H

#include "core/base.h"

#define SYN_EXPR_CODE 96
#define SYN_EXPR_CONSTS 24

typedef struct SynExpr {
    u8 op[SYN_EXPR_CODE];
    u8 arg[SYN_EXPR_CODE];
    float k[SYN_EXPR_CONSTS];
    u8 ncode, nk;
} SynExpr;

// names: the variable names (index = slot). false + err on a syntax error / unknown name / overflow.
bool syn_expr_compile(SynExpr *e, const char *src, const char *const *names, int nnames, char *err, int err_size);
void syn_expr_const(SynExpr *e, float v);
float syn_expr_eval(const SynExpr *e, const float *vars);

#endif
