// Basic types and helper macros shared by every module.
#ifndef DG_BASE_H
#define DG_BASE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;

#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define DG_MIN(a, b) ((a) < (b) ? (a) : (b))
#define DG_MAX(a, b) ((a) > (b) ? (a) : (b))
#define DG_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define KB(x) ((size_t)(x) << 10)
#define MB(x) ((size_t)(x) << 20)
#define DG_UNUSED(x) (void)(x)

#if defined(_MSC_VER)
#define DG_INLINE static __inline
#define DG_THREAD_LOCAL __declspec(thread)
#else
#define DG_INLINE static inline
#define DG_THREAD_LOCAL _Thread_local
#endif

// Copies at most dst_size-1 chars and always terminates.
DG_INLINE void dg_strcpy(char *dst, size_t dst_size, const char *src) {
    if (!dst_size) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= dst_size) n = dst_size - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

DG_INLINE u32 dg_hash_str(const char *s) {
    u32 h = 2166136261u;
    while (*s) { h ^= (u8)*s++; h *= 16777619u; }
    return h;
}

DG_INLINE u32 dg_hash_u32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

// PCG32 random generator; deterministic given the seed.
typedef struct Rng { u64 state; u64 inc; } Rng;

DG_INLINE u32 rng_next(Rng *r) {
    u64 old = r->state;
    r->state = old * 6364136223846793005ULL + (r->inc | 1);
    u32 xorshifted = (u32)(((old >> 18u) ^ old) >> 27u);
    u32 rot = (u32)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-(i32)rot) & 31));
}
DG_INLINE Rng rng_make(u64 seed) {
    Rng r = { 0, (seed << 1u) | 1u };
    rng_next(&r); r.state += seed; rng_next(&r);
    return r;
}
DG_INLINE float rng_float(Rng *r) { return (rng_next(r) >> 8) * (1.0f / 16777216.0f); }
DG_INLINE float rng_range(Rng *r, float lo, float hi) { return lo + (hi - lo) * rng_float(r); }

#endif
