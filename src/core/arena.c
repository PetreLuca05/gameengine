#include "arena.h"
#include "log.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

void arena_init(Arena *a, size_t cap, const char *name) {
    a->base = (u8 *)malloc(cap);
    a->cap = cap;
    a->used = 0;
    a->peak = 0;
    a->name = name;
    if (!a->base) log_fatal("arena '%s': out of memory (%zu bytes)", name, cap);
}

void arena_free(Arena *a) {
    free(a->base);
    memset(a, 0, sizeof(*a));
}

void *arena_push_nz(Arena *a, size_t size, size_t align) {
    size_t at = (a->used + (align - 1)) & ~(align - 1);
    if (at + size > a->cap) {
        log_fatal("arena '%s' overflow: need %zu, cap %zu", a->name, at + size, a->cap);
        return NULL;
    }
    a->used = at + size;
    if (a->used > a->peak) a->peak = a->used;
    return a->base + at;
}

void *arena_push(Arena *a, size_t size, size_t align) {
    void *p = arena_push_nz(a, size, align);
    if (p) memset(p, 0, size);
    return p;
}

void arena_reset(Arena *a) { a->used = 0; }

char *arena_strdup(Arena *a, const char *s) {
    size_t n = strlen(s);
    char *d = (char *)arena_push_nz(a, n + 1, 1);
    memcpy(d, s, n + 1);
    return d;
}

char *arena_printf(Arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *d = (char *)arena_push_nz(a, (size_t)n + 1, 1);
    vsnprintf(d, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return d;
}
