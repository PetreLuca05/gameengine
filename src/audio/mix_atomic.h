// Minimal acquire / release atomics for the mixer's single-producer / single-consumer queue and the
// voice states it publishes (no SDL, so the pure mixer links into the unit tests).
#ifndef DG_MIX_ATOMIC_H
#define DG_MIX_ATOMIC_H

#include "core/base.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
typedef volatile long mix_atomic;
DG_INLINE long mix_load(const mix_atomic *a) { long v = *a; _ReadWriteBarrier(); return v; }
DG_INLINE void mix_store(mix_atomic *a, long v) { _ReadWriteBarrier(); _InterlockedExchange(a, v); }
#else
typedef long mix_atomic;
DG_INLINE long mix_load(const mix_atomic *a) { return __atomic_load_n(a, __ATOMIC_ACQUIRE); }
DG_INLINE void mix_store(mix_atomic *a, long v) { __atomic_store_n(a, v, __ATOMIC_RELEASE); }
#endif

#endif
