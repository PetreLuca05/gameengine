// Linear arena allocator. Used for per-frame scratch and per-level data.
#ifndef DG_ARENA_H
#define DG_ARENA_H

#include "base.h"

typedef struct Arena {
    u8 *base;
    size_t cap;
    size_t used;
    size_t peak;
    const char *name;
} Arena;

typedef struct ArenaMark { Arena *arena; size_t used; } ArenaMark;

void arena_init(Arena *a, size_t cap, const char *name);
void arena_free(Arena *a);
void *arena_push(Arena *a, size_t size, size_t align);   // zeroed
void *arena_push_nz(Arena *a, size_t size, size_t align); // not zeroed
void arena_reset(Arena *a);
char *arena_strdup(Arena *a, const char *s);
char *arena_printf(Arena *a, const char *fmt, ...);

DG_INLINE ArenaMark arena_mark(Arena *a) { ArenaMark m = { a, a->used }; return m; }
DG_INLINE void arena_pop_to(ArenaMark m) { m.arena->used = m.used; }

#define ARENA_PUSH(a, T) ((T *)arena_push((a), sizeof(T), _Alignof(T)))
#define ARENA_PUSH_N(a, T, n) ((T *)arena_push((a), sizeof(T) * (size_t)(n), _Alignof(T)))

#endif
