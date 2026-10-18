// Weapon muzzle socket: file text patching, number format, bore helpers (docs/HANDPOSE.md "Muzzle").
#include "hp_muzzle.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float round_res(float f) {
    double k = round(1.0 / (double)HP_MUZZLE_RES);
    double r = round((double)f * k) / k;   // in double: 0.06 rounds to the float "0.06" parses to
    return r == 0.0 ? 0.0f : (float)r;
}

vec3 hp_muzzle_round(vec3 v) { return v3(round_res(v.x), round_res(v.y), round_res(v.z)); }

static void format_num(float f, char *out, int size) {
    char b[48];
    snprintf(b, sizeof(b), "%.4f", (double)round_res(f));
    char *dot = strchr(b, '.');
    if (dot) {
        char *e = b + strlen(b) - 1;
        while (e > dot && *e == '0') *e-- = 0;
        if (e == dot) *e = 0;
    }
    snprintf(out, (size_t)size, "%s", strcmp(b, "-0") ? b : "0");
}

void hp_muzzle_format(vec3 v, char *out, int size) {
    char x[24], y[24], z[24];
    format_num(v.x, x, sizeof(x));
    format_num(v.y, y, sizeof(y));
    format_num(v.z, z, sizeof(z));
    snprintf(out, (size_t)size, "[%s, %s, %s]", x, y, z);
}

// ------------------------------------------------------------------ a minimal JSON scanner
// Strict JSON (cJSON reads the data files): the spans of values and members in the text, so a
// patch replaces exactly the bytes it means to and nothing else moves.

static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char *skip_string(const char *p) {   // at '"': just after the closing quote
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!*++p) return NULL;
        } else if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *skip_value(const char *p) {     // at the value: just after it (NULL: malformed)
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = skip_string(p);
                if (!p) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
            p++;
        }
        return NULL;
    }
    const char *s = p;
    while (*p && !strchr(",}] \t\r\n", *p)) p++;
    return p > s ? p : NULL;
}

typedef struct JMember { const char *key, *val, *end; } JMember;   // key at its quote, value [val, end)
typedef struct JObject {
    const char *open, *close;  // '{', '}'
    int count;
    JMember last;              // the last member (count > 0)
} JObject;

// Scans the object at p; *has / *found: the member `key`. False: not an object, or malformed.
static bool scan_object(const char *p, const char *key, JObject *o, JMember *found, bool *has) {
    *has = false;
    memset(o, 0, sizeof(*o));
    if (*p != '{') return false;
    o->open = p;
    size_t kl = strlen(key);
    p = ws(p + 1);
    while (*p != '}') {
        if (*p != '"') return false;
        JMember m;
        m.key = p;
        const char *ke = skip_string(p);
        if (!ke) return false;
        p = ws(ke);
        if (*p != ':') return false;
        m.val = ws(p + 1);
        m.end = skip_value(m.val);
        if (!m.end) return false;
        if (!*has && (size_t)(ke - m.key - 2) == kl && !strncmp(m.key + 1, key, kl)) {
            *found = m;
            *has = true;
        }
        o->last = m;
        o->count++;
        p = ws(m.end);
        if (*p == ',') p = ws(p + 1);
        else if (*p != '}') return false;
    }
    o->close = p;
    return true;
}

// hold, hold.sockets and hold.sockets.muzzle of a weapon file.
typedef struct MuzzleSpans {
    JObject hold, sockets;
    bool has_sockets, has_muzzle;
    JMember muzzle;
} MuzzleSpans;

static bool find_spans(const char *text, MuzzleSpans *s) {
    memset(s, 0, sizeof(*s));
    JObject root;
    JMember hold, sk;
    bool has;
    if (!scan_object(ws(text), "hold", &root, &hold, &has) || !has) return false;
    if (!scan_object(hold.val, "sockets", &s->hold, &sk, &s->has_sockets)) return false;
    if (s->has_sockets && !scan_object(sk.val, "muzzle", &s->sockets, &s->muzzle, &s->has_muzzle)) return false;
    return true;
}

bool hp_muzzle_json_read(const char *text, vec3 *out) {
    MuzzleSpans s;
    if (!text || !find_spans(text, &s) || !s.has_muzzle || *s.muzzle.val != '[') return false;
    const char *p = s.muzzle.val + 1;
    float v[3];
    for (int i = 0; i < 3; i++) {
        char *e;
        p = ws(p);
        v[i] = strtof(p, &e);
        if (e == p) return false;
        p = ws(e);
        if (i < 2) {
            if (*p != ',') return false;
            p++;
        }
    }
    if (*p != ']') return false;
    *out = v3(v[0], v[1], v[2]);
    return true;
}

// The whitespace before `at` when it starts its line (the member's indentation); false when other
// text precedes it on the line (a one-line object).
static bool line_indent(const char *text, const char *at, char *out, int size) {
    const char *b = at;
    while (b > text && (b[-1] == ' ' || b[-1] == '\t')) b--;
    if (b > text && b[-1] != '\n') return false;
    int n = DG_MIN((int)(at - b), size - 1);
    memcpy(out, b, (size_t)n);
    out[n] = 0;
    return true;
}

// text[0, at) + ins + text[from, end).
static char *splice(const char *text, const char *at, const char *from, const char *ins) {
    size_t a = (size_t)(at - text), il = strlen(ins), rest = strlen(from);
    char *o = (char *)malloc(a + il + rest + 1);
    if (!o) return NULL;
    memcpy(o, text, a);
    memcpy(o + a, ins, il);
    memcpy(o + a + il, from, rest + 1);
    return o;
}

// A member appended to object o: on its own line indented like the last member (inline in a
// one-line object); an empty object becomes "{ member }".
static char *add_member(const char *text, const JObject *o, const char *member, const char *nl) {
    char ins[256], ind[64];
    if (!o->count) {
        snprintf(ins, sizeof(ins), " %s ", member);
        return splice(text, o->open + 1, o->close, ins);
    }
    if (line_indent(text, o->last.key, ind, sizeof(ind))) snprintf(ins, sizeof(ins), ",%s%s%s", nl, ind, member);
    else snprintf(ins, sizeof(ins), ", %s", member);
    return splice(text, o->last.end, o->last.end, ins);
}

char *hp_muzzle_json_patch(const char *text, vec3 v) {
    MuzzleSpans s;
    if (!text || !find_spans(text, &s)) return NULL;
    char val[96], member[160];
    hp_muzzle_format(v, val, sizeof(val));
    const char *nl = strstr(text, "\r\n") ? "\r\n" : "\n";
    if (s.has_muzzle) return splice(text, s.muzzle.val, s.muzzle.end, val);
    if (s.has_sockets) {
        snprintf(member, sizeof(member), "\"muzzle\": %s", val);
        return add_member(text, &s.sockets, member, nl);
    }
    snprintf(member, sizeof(member), "\"sockets\": { \"muzzle\": %s }", val);
    return add_member(text, &s.hold, member, nl);
}

// ------------------------------------------------------------------ bore helpers

int hp_muzzle_barrel_part(const WeaponDef *wd) {
    int best = -1;
    for (int i = 0; wd && i < wd->part_count; i++) {
        if (wd->iron_parts & (1u << i)) continue;
        const WeaponModelPart *p = &wd->parts[i];
        if (best < 0 || p->offset.z - p->half.z < wd->parts[best].offset.z - wd->parts[best].half.z) best = i;
    }
    return best;
}

vec3 hp_muzzle_snap(const WeaponDef *wd, vec3 m, int how) {
    int b = hp_muzzle_barrel_part(wd);
    if (b < 0) return m;
    const WeaponModelPart *p = &wd->parts[b];
    return v3(p->offset.x, p->offset.y, how == HPM_TIP ? p->offset.z - p->half.z : m.z);
}
