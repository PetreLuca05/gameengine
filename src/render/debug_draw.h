// Immediate + timed debug drawing (lines, shapes, world-space text).
// duration <= 0 means "this frame only".
#ifndef DG_DEBUG_DRAW_H
#define DG_DEBUG_DRAW_H

#include "core/base.h"
#include "core/dgmath.h"

#define DBG_RED     0xFF3030FFu
#define DBG_GREEN   0xFF30FF30u
#define DBG_BLUE    0xFFFF6030u
#define DBG_YELLOW  0xFF30FFFFu
#define DBG_CYAN    0xFFFFFF30u
#define DBG_MAGENTA 0xFFFF30FFu
#define DBG_WHITE   0xFFFFFFFFu
#define DBG_ORANGE  0xFF2090FFu
#define DBG_GREY    0xFF909090u

void dbg_init(void);
void dbg_shutdown(void);
void dbg_line(vec3 a, vec3 b, u32 color, float duration, bool depth);
void dbg_arrow(vec3 a, vec3 b, u32 color, float duration);
void dbg_cross(vec3 p, float size, u32 color, float duration);
void dbg_box(vec3 center, vec3 half, quat rot, u32 color, float duration);
void dbg_aabb(vec3 mn, vec3 mx, u32 color, float duration);
void dbg_sphere(vec3 c, float r, u32 color, float duration);
void dbg_circle(vec3 c, vec3 normal, float r, u32 color, float duration);
// Capsule standing on bottom (total height includes caps).
void dbg_capsule(vec3 bottom, float radius, float height, u32 color, float duration);
void dbg_capsule_axis(vec3 a, vec3 b, float radius, u32 color, float duration);
void dbg_text(vec3 pos, const char *text, u32 color, float size, float duration);
// Flushes lines/text to the renderer and ages timed primitives.
void dbg_submit(float dt, vec3 cam_pos, vec3 cam_right, vec3 cam_up);
void dbg_clear(void);

#endif
