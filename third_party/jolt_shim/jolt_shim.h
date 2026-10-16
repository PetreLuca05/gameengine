// Tiny extern "C" helpers for Jolt features the joltc binding does not expose.
#ifndef DG_JOLT_SHIM_H
#define DG_JOLT_SHIM_H
#include "joltc.h"
#ifdef __cplusplus
extern "C" {
#endif

// Returns the Jolt version as major*10000 + minor*100 + patch.
int dgj_version(void);

#ifdef __cplusplus
}
#endif
#endif
