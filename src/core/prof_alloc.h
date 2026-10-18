// Allocation and memcpy accounting for the profiler (docs/PERF_AUDIT.md "Allocations"). CMake
// force-includes this header into every engine .c file (DG_PROFILE_ALLOC): malloc / calloc /
// realloc / free / memcpy keep their behaviour and, while a capture records, count calls, bytes and
// the call site (file:line, innermost zone). Third-party code is not wrapped.
#ifndef DG_PROF_ALLOC_H
#define DG_PROF_ALLOC_H

// Included before anything else, so the feature macros files set themselves (platform.c: fseeko,
// hws_server.c: IFF_UP) come too late on glibc: ask for all of them here.
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
// Every CRT header that (re)declares malloc / memcpy comes in before the macros below (a declaration
// seen after them would be macro-expanded): <malloc.h>, <memory.h>, <intrin.h> (SDL_intrin.h, windows.h).
#include <malloc.h>
#include <memory.h>
#include <intrin.h>
#endif

#ifdef DG_PROFILE_ALLOC

extern volatile int dg_prof_recording;
void *dg_prof_malloc(size_t n, const char *file, int line);
void *dg_prof_calloc(size_t c, size_t n, const char *file, int line);
void *dg_prof_realloc(void *p, size_t n, const char *file, int line);
void dg_prof_free(void *p);
void dg_prof_memcpy_count(size_t n);

#if defined(_MSC_VER)
static __inline void *dg_prof_memcpy(void *d, const void *s, size_t n) {
#else
static inline void *dg_prof_memcpy(void *d, const void *s, size_t n) {
#endif
    if (dg_prof_recording) dg_prof_memcpy_count(n);
    return (memcpy)(d, s, n);
}

#define malloc(n) dg_prof_malloc((n), __FILE__, __LINE__)
#define calloc(c, n) dg_prof_calloc((c), (n), __FILE__, __LINE__)
#define realloc(p, n) dg_prof_realloc((p), (n), __FILE__, __LINE__)
#define free(p) dg_prof_free(p)
#define memcpy(d, s, n) dg_prof_memcpy((d), (s), (n))

#endif
#endif
