#include "debug_draw.h"
#include "render.h"
#include "core/engine.h"
#include "ui/font.h"
#include <stdlib.h>
#include <math.h>

#define MAX_TIMED_LINES 65536
#define MAX_TEXTS 1024

typedef struct TimedLine { vec3 a, b; u32 color; float ttl; bool depth; } TimedLine;
typedef struct DbgText { vec3 pos; char text[64]; u32 color; float size, ttl; } DbgText;

typedef struct DebugDraw {
    TimedLine *lines;
    int line_count;
    DbgText texts[MAX_TEXTS];
    int text_count;
} DebugDraw;

#define D (g_engine.debug)

void dbg_init(void) {
    D = (DebugDraw *)calloc(1, sizeof(DebugDraw));
    D->lines = (TimedLine *)malloc(sizeof(TimedLine) * MAX_TIMED_LINES);
}

void dbg_shutdown(void) {
    if (!D) return;
    free(D->lines);
    free(D);
    D = NULL;
}

void dbg_clear(void) {
    if (!D) return;
    D->line_count = 0;
    D->text_count = 0;
}

void dbg_line(vec3 a, vec3 b, u32 color, float duration, bool depth) {
    if (!D || D->line_count >= MAX_TIMED_LINES) return;
    TimedLine *l = &D->lines[D->line_count++];
    l->a = a; l->b = b; l->color = color; l->ttl = duration; l->depth = depth;
}

void dbg_arrow(vec3 a, vec3 b, u32 color, float duration) {
    dbg_line(a, b, color, duration, false);
    vec3 d = v3_sub(b, a);
    float len = v3_len(d);
    if (len < 1e-4f) return;
    d = v3_scale(d, 1.0f / len);
    vec3 side = fabsf(d.y) < 0.9f ? v3_norm(v3_cross(d, v3(0, 1, 0))) : v3(1, 0, 0);
    vec3 up = v3_cross(side, d);
    float h = fminf(0.15f, len * 0.3f);
    vec3 base = v3_madd(b, d, -h);
    dbg_line(b, v3_madd(base, side, h * 0.5f), color, duration, false);
    dbg_line(b, v3_madd(base, side, -h * 0.5f), color, duration, false);
    dbg_line(b, v3_madd(base, up, h * 0.5f), color, duration, false);
    dbg_line(b, v3_madd(base, up, -h * 0.5f), color, duration, false);
}

void dbg_cross(vec3 p, float s, u32 color, float duration) {
    dbg_line(v3_add(p, v3(-s, 0, 0)), v3_add(p, v3(s, 0, 0)), color, duration, false);
    dbg_line(v3_add(p, v3(0, -s, 0)), v3_add(p, v3(0, s, 0)), color, duration, false);
    dbg_line(v3_add(p, v3(0, 0, -s)), v3_add(p, v3(0, 0, s)), color, duration, false);
}

void dbg_box(vec3 c, vec3 h, quat q, u32 color, float duration) {
    vec3 corners[8];
    for (int i = 0; i < 8; i++) {
        vec3 l = v3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
        corners[i] = v3_add(c, quat_rotate(q, l));
    }
    static const int e[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                  { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    for (int i = 0; i < 12; i++) dbg_line(corners[e[i][0]], corners[e[i][1]], color, duration, true);
}

void dbg_aabb(vec3 mn, vec3 mx, u32 color, float duration) {
    dbg_box(v3_scale(v3_add(mn, mx), 0.5f), v3_scale(v3_sub(mx, mn), 0.5f), quat_identity(), color, duration);
}

void dbg_circle(vec3 c, vec3 n, float r, u32 color, float duration) {
    n = v3_norm(n);
    vec3 t = fabsf(n.y) < 0.9f ? v3_norm(v3_cross(n, v3(0, 1, 0))) : v3(1, 0, 0);
    vec3 b = v3_cross(n, t);
    const int seg = 20;
    vec3 prev = v3_madd(c, t, r);
    for (int i = 1; i <= seg; i++) {
        float a = (float)i / seg * 2.0f * DG_PI;
        vec3 p = v3_add(c, v3_add(v3_scale(t, cosf(a) * r), v3_scale(b, sinf(a) * r)));
        dbg_line(prev, p, color, duration, true);
        prev = p;
    }
}

void dbg_sphere(vec3 c, float r, u32 color, float duration) {
    dbg_circle(c, v3(1, 0, 0), r, color, duration);
    dbg_circle(c, v3(0, 1, 0), r, color, duration);
    dbg_circle(c, v3(0, 0, 1), r, color, duration);
}

void dbg_capsule_axis(vec3 a, vec3 b, float r, u32 color, float duration) {
    vec3 axis = v3_sub(b, a);
    float len = v3_len(axis);
    vec3 n = len > 1e-5f ? v3_scale(axis, 1.0f / len) : v3(0, 1, 0);
    dbg_circle(a, n, r, color, duration);
    dbg_circle(b, n, r, color, duration);
    vec3 t = fabsf(n.y) < 0.9f ? v3_norm(v3_cross(n, v3(0, 1, 0))) : v3(1, 0, 0);
    vec3 s = v3_cross(n, t);
    vec3 dirs[4] = { t, v3_neg(t), s, v3_neg(s) };
    for (int i = 0; i < 4; i++) {
        dbg_line(v3_madd(a, dirs[i], r), v3_madd(b, dirs[i], r), color, duration, true);
        // hemisphere arcs
        vec3 pa = v3_madd(a, dirs[i], r), pb = v3_madd(b, dirs[i], r);
        for (int k = 1; k <= 4; k++) {
            float ang = (float)k / 4 * DG_PI * 0.5f;
            vec3 qa = v3_add(a, v3_add(v3_scale(dirs[i], cosf(ang) * r), v3_scale(n, -sinf(ang) * r)));
            vec3 qb = v3_add(b, v3_add(v3_scale(dirs[i], cosf(ang) * r), v3_scale(n, sinf(ang) * r)));
            dbg_line(pa, qa, color, duration, true);
            dbg_line(pb, qb, color, duration, true);
            pa = qa; pb = qb;
        }
    }
}

void dbg_capsule(vec3 bottom, float r, float height, u32 color, float duration) {
    vec3 a = v3_add(bottom, v3(0, r, 0));
    vec3 b = v3_add(bottom, v3(0, fmaxf(height - r, r), 0));
    dbg_capsule_axis(a, b, r, color, duration);
}

void dbg_text(vec3 pos, const char *text, u32 color, float size, float duration) {
    if (!D || D->text_count >= MAX_TEXTS) return;
    DbgText *t = &D->texts[D->text_count++];
    t->pos = pos;
    dg_strcpy(t->text, sizeof(t->text), text);
    t->color = color;
    t->size = size;
    t->ttl = duration;
}

void dbg_submit(float dt, vec3 cam_pos, vec3 cam_right, vec3 cam_up) {
    if (!D) return;
    int n_depth = 0, n_overlay = 0;
    for (int i = 0; i < D->line_count; i++) {
        if (D->lines[i].depth) n_depth++;
        else n_overlay++;
    }
    LineVertex *vd = n_depth ? render_lines_alloc((u32)n_depth * 2, true) : NULL;
    LineVertex *vo = n_overlay ? render_lines_alloc((u32)n_overlay * 2, false) : NULL;
    int id = 0, io = 0;
    int keep = 0;
    for (int i = 0; i < D->line_count; i++) {
        TimedLine *l = &D->lines[i];
        LineVertex *dst = l->depth ? (vd ? vd + 2 * id++ : NULL) : (vo ? vo + 2 * io++ : NULL);
        if (dst) {
            dst[0].pos = l->a; dst[0].color = l->color;
            dst[1].pos = l->b; dst[1].color = l->color;
        }
        l->ttl -= dt;
        if (l->ttl > 0.0f) D->lines[keep++] = *l;
    }
    D->line_count = keep;

    keep = 0;
    for (int i = 0; i < D->text_count; i++) {
        DbgText *t = &D->texts[i];
        float dist = v3_dist(t->pos, cam_pos);
        if (dist < 60.0f) font_text3d_billboard(t->pos, cam_right, cam_up, t->size, t->color, t->text);
        t->ttl -= dt;
        if (t->ttl > 0.0f) D->texts[keep++] = *t;
    }
    D->text_count = keep;
}
