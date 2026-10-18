#include "syn_expr.h"
#include "syn_material.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OP_CONST, OP_VAR, OP_NEG, OP_NOT, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE, OP_AND, OP_OR, OP_SELECT,   // SELECT: c a b -> c ? a : b
    OP_FN,   // arg = function id; arity from the table (sel: arity in the next op's arg)
};

typedef struct Fn { const char *name; int arity; } Fn;   // arity -1: variadic (sel)
enum { FN_LERP, FN_CLAMP, FN_MIN, FN_MAX, FN_ABS, FN_SQRT, FN_CBRT, FN_POW, FN_EXP, FN_LOG, FN_LOG10, FN_DB, FN_TODB,
       FN_STEP, FN_FLOOR, FN_SEL, FN_MAT_CONTACT, FN_MAT_NOISE, FN_MAT_NOISE_HZ, FN_MAT_MODES, FN_MAT_HARD, FN_COUNT };
static const Fn k_fn[FN_COUNT] = {
    { "lerp", 5 }, { "clamp", 3 }, { "min", 2 }, { "max", 2 }, { "abs", 1 }, { "sqrt", 1 }, { "cbrt", 1 }, { "pow", 2 },
    { "exp", 1 }, { "log", 1 }, { "log10", 1 }, { "db", 1 }, { "todb", 1 }, { "step", 2 }, { "floor", 1 }, { "sel", -1 },
    { "mat_contact", 1 }, { "mat_noise", 1 }, { "mat_noise_hz", 1 }, { "mat_modes", 1 }, { "mat_hard", 1 },
};

typedef struct Parser {
    const char *s;
    SynExpr *e;
    const char *const *names;
    int nnames;
    char *err;
    int err_size;
    bool bad;
} Parser;

static void fail(Parser *p, const char *msg) {
    if (!p->bad) snprintf(p->err, p->err_size, "expr: %s at '%.16s'", msg, p->s);
    p->bad = true;
}

static void emit(Parser *p, int op, int arg) {
    if (p->e->ncode >= SYN_EXPR_CODE) { fail(p, "too long"); return; }
    p->e->op[p->e->ncode] = (u8)op;
    p->e->arg[p->e->ncode] = (u8)arg;
    p->e->ncode++;
}

static void emit_const(Parser *p, float v) {
    if (p->e->nk >= SYN_EXPR_CONSTS) { fail(p, "too many constants"); return; }
    p->e->k[p->e->nk] = v;
    emit(p, OP_CONST, p->e->nk++);
}

static void ws(Parser *p) { while (*p->s && isspace((unsigned char)*p->s)) p->s++; }
static bool eat(Parser *p, const char *t) {
    ws(p);
    size_t n = strlen(t);
    if (!strncmp(p->s, t, n)) { p->s += n; return true; }
    return false;
}

static void expr(Parser *p);

static void primary(Parser *p) {
    ws(p);
    if (eat(p, "(")) {
        expr(p);
        if (!eat(p, ")")) fail(p, "expected )");
        return;
    }
    if (isdigit((unsigned char)*p->s) || *p->s == '.') {
        char *end;
        float v = strtof(p->s, &end);
        p->s = end;
        emit_const(p, v);
        return;
    }
    if (isalpha((unsigned char)*p->s) || *p->s == '_') {
        char name[32];
        int n = 0;
        while ((isalnum((unsigned char)*p->s) || *p->s == '_') && n < 31) name[n++] = *p->s++;
        name[n] = 0;
        if (eat(p, "(")) {
            int f = -1;
            for (int i = 0; i < FN_COUNT; i++) if (!strcmp(k_fn[i].name, name)) f = i;
            if (f < 0) { fail(p, "unknown function"); return; }
            int argc = 0;
            if (!eat(p, ")")) {
                do { expr(p); argc++; } while (eat(p, ","));
                if (!eat(p, ")")) { fail(p, "expected )"); return; }
            }
            if (k_fn[f].arity >= 0 && argc != k_fn[f].arity) { fail(p, "wrong argument count"); return; }
            if (f == FN_SEL && (argc < 2 || argc > 24)) { fail(p, "sel takes 2..24 arguments"); return; }
            emit(p, OP_FN, f);
            if (f == FN_SEL) emit(p, OP_CONST, argc);   // arity marker (not a constant: skipped by eval)
            return;
        }
        if (!strcmp(name, "pi")) { emit_const(p, 3.14159265f); return; }
        for (int i = 0; i < p->nnames; i++)
            if (!strcmp(p->names[i], name)) { emit(p, OP_VAR, i); return; }
        fail(p, "unknown name");
        return;
    }
    fail(p, "expected a value");
}

static void unary(Parser *p);
static void power(Parser *p) {
    primary(p);
    if (eat(p, "^")) { unary(p); emit(p, OP_POW, 0); }
}
static void unary(Parser *p) {
    if (eat(p, "-")) { unary(p); emit(p, OP_NEG, 0); return; }
    ws(p);
    if (p->s[0] == '!' && p->s[1] != '=') { p->s++; unary(p); emit(p, OP_NOT, 0); return; }
    power(p);
}
static void mul(Parser *p) {
    unary(p);
    for (;;) {
        if (eat(p, "*")) { unary(p); emit(p, OP_MUL, 0); }
        else if (eat(p, "/")) { unary(p); emit(p, OP_DIV, 0); }
        else if (eat(p, "%")) { unary(p); emit(p, OP_MOD, 0); }
        else return;
    }
}
static void add(Parser *p) {
    mul(p);
    for (;;) {
        if (eat(p, "+")) { mul(p); emit(p, OP_ADD, 0); }
        else if (eat(p, "-")) { mul(p); emit(p, OP_SUB, 0); }
        else return;
    }
}
static void cmp(Parser *p) {
    add(p);
    if (eat(p, "<=")) { add(p); emit(p, OP_LE, 0); }
    else if (eat(p, ">=")) { add(p); emit(p, OP_GE, 0); }
    else if (eat(p, "==")) { add(p); emit(p, OP_EQ, 0); }
    else if (eat(p, "!=")) { add(p); emit(p, OP_NE, 0); }
    else if (eat(p, "<")) { add(p); emit(p, OP_LT, 0); }
    else if (eat(p, ">")) { add(p); emit(p, OP_GT, 0); }
}
static void and_(Parser *p) {
    cmp(p);
    while (eat(p, "&&")) { cmp(p); emit(p, OP_AND, 0); }
}
static void or_(Parser *p) {
    and_(p);
    while (eat(p, "||")) { and_(p); emit(p, OP_OR, 0); }
}
static void expr(Parser *p) {
    or_(p);
    if (eat(p, "?")) {
        expr(p);
        if (!eat(p, ":")) { fail(p, "expected :"); return; }
        expr(p);
        emit(p, OP_SELECT, 0);
    }
}

bool syn_expr_compile(SynExpr *e, const char *src, const char *const *names, int nnames, char *err, int err_size) {
    memset(e, 0, sizeof(*e));
    Parser p = { src, e, names, nnames, err, err_size, false };
    expr(&p);
    ws(&p);
    if (!p.bad && *p.s) fail(&p, "trailing text");
    return !p.bad;
}

void syn_expr_const(SynExpr *e, float v) {
    memset(e, 0, sizeof(*e));
    e->k[0] = v;
    e->nk = 1;
    e->op[0] = OP_CONST;
    e->ncode = 1;
}

float syn_expr_eval(const SynExpr *e, const float *vars) {
    float st[SYN_EXPR_CODE];
    int sp = 0;
    for (int i = 0; i < e->ncode; i++) {
        int op = e->op[i], arg = e->arg[i];
        float b, a;
        switch (op) {
        case OP_CONST: st[sp++] = e->k[arg]; break;
        case OP_VAR: st[sp++] = vars[arg]; break;
        case OP_NEG: st[sp - 1] = -st[sp - 1]; break;
        case OP_NOT: st[sp - 1] = st[sp - 1] > 0.5f ? 0.0f : 1.0f; break;
        case OP_SELECT: {
            float f = st[--sp], t = st[--sp], c = st[--sp];
            st[sp++] = c > 0.5f ? t : f;
        } break;
        case OP_FN: {
            int f = arg;
            if (f == FN_SEL) {
                int argc = e->arg[++i];
                sp -= argc;
                int idx = (int)floorf(st[sp] + 0.5f);
                idx = DG_CLAMP(idx, 0, argc - 2);
                float pick = st[sp + 1 + idx];
                st[sp++] = pick;
                break;
            }
            float *x = &st[sp - k_fn[f].arity];
            float r = 0;
            switch (f) {
            case FN_LERP: {
                float x0 = x[1], y0 = x[2], x1 = x[3], y1 = x[4];
                float u = x1 != x0 ? (x[0] - x0) / (x1 - x0) : 0.0f;
                u = u < 0 ? 0 : (u > 1 ? 1 : u);
                r = y0 + (y1 - y0) * u;
            } break;
            case FN_CLAMP: r = x[0] < x[1] ? x[1] : (x[0] > x[2] ? x[2] : x[0]); break;
            case FN_MIN: r = fminf(x[0], x[1]); break;
            case FN_MAX: r = fmaxf(x[0], x[1]); break;
            case FN_ABS: r = fabsf(x[0]); break;
            case FN_SQRT: r = sqrtf(fmaxf(x[0], 0.0f)); break;
            case FN_CBRT: r = cbrtf(x[0]); break;
            case FN_POW: r = powf(fmaxf(x[0], 0.0f), x[1]); break;
            case FN_EXP: r = expf(x[0]); break;
            case FN_LOG: r = logf(fmaxf(x[0], 1e-12f)); break;
            case FN_LOG10: r = log10f(fmaxf(x[0], 1e-12f)); break;
            case FN_DB: r = powf(10.0f, x[0] / 20.0f); break;
            case FN_TODB: r = 20.0f * log10f(fmaxf(x[0], 1e-9f)); break;
            case FN_STEP: r = x[1] >= x[0] ? 1.0f : 0.0f; break;
            case FN_FLOOR: r = floorf(x[0]); break;
            case FN_MAT_CONTACT: r = syn_material((int)(x[0] + 0.5f))->contact_ms; break;
            case FN_MAT_NOISE: r = syn_material((int)(x[0] + 0.5f))->noise; break;
            case FN_MAT_NOISE_HZ: r = syn_material((int)(x[0] + 0.5f))->noise_hz; break;
            case FN_MAT_MODES: r = (float)syn_material((int)(x[0] + 0.5f))->nmodes; break;
            case FN_MAT_HARD: r = 1.0f - syn_material((int)(x[0] + 0.5f))->absorb[2]; break;
            }
            sp -= k_fn[f].arity;
            st[sp++] = r;
        } break;
        default: {
            b = st[--sp];
            a = st[--sp];
            float r = 0;
            switch (op) {
            case OP_ADD: r = a + b; break;
            case OP_SUB: r = a - b; break;
            case OP_MUL: r = a * b; break;
            case OP_DIV: r = b != 0 ? a / b : 0.0f; break;
            case OP_MOD: r = b != 0 ? fmodf(a, b) : 0.0f; break;
            case OP_POW: r = powf(fmaxf(a, 0.0f), b); break;
            case OP_LT: r = a < b; break;
            case OP_GT: r = a > b; break;
            case OP_LE: r = a <= b; break;
            case OP_GE: r = a >= b; break;
            case OP_EQ: r = fabsf(a - b) < 1e-4f; break;
            case OP_NE: r = fabsf(a - b) >= 1e-4f; break;
            case OP_AND: r = a > 0.5f && b > 0.5f; break;
            case OP_OR: r = a > 0.5f || b > 0.5f; break;
            }
            st[sp++] = r;
        } break;
        }
    }
    float r = sp > 0 ? st[sp - 1] : 0.0f;
    return r == r ? r : 0.0f;
}
