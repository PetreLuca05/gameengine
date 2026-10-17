// Tiny extern "C" helpers for Jolt features the joltc binding does not expose.
#ifndef DG_JOLT_SHIM_H
#define DG_JOLT_SHIM_H
#include "joltc.h"
#ifdef __cplusplus
extern "C" {
#endif

// Returns the Jolt version as major*10000 + minor*100 + patch.
int dgj_version(void);

// Worst-case bytes one contact constraint takes in PhysicsSystem::Update's temp buffer
// (ContactConstraintManager::cMaxConstraintSize): the step allocates maxContactConstraints of them.
uint32_t dgj_contact_constraint_bytes(void);

// A temp allocator for JPH_PhysicsSystem_Update2: a fixed block of `size` bytes (NULL when it cannot be
// allocated), malloc past it; a failed malloc aborts with a [jolt] trace instead of returning null.
JPH_TempAllocator *dgj_temp_allocator_create(uint32_t size);
void dgj_temp_allocator_destroy(JPH_TempAllocator *a);
// Allocations that did not fit the fixed block so far.
uint32_t dgj_temp_allocator_fallbacks(const JPH_TempAllocator *a);

#ifdef __cplusplus
}
#endif
#endif
