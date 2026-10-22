// Minimal SPIR-V reflection (pure, no Vulkan): the descriptor bindings a compute shader declares in
// one set. Used by the vendored FidelityFX FSR 2 passes (post_fsr2.c), whose binding lists differ
// per pass and per permutation: the descriptor set layout is built from what the SPIR-V declares.
// tests/test_spv_reflect.c.
#ifndef DG_SPV_REFLECT_H
#define DG_SPV_REFLECT_H

#include "core/base.h"

typedef enum SpvDescKind {
    SPV_DESC_SAMPLER,           // sampler
    SPV_DESC_SAMPLED_IMAGE,     // texture2D (separate image)
    SPV_DESC_COMBINED,          // sampler2D
    SPV_DESC_STORAGE_IMAGE,     // image2D / uimage2D
    SPV_DESC_UNIFORM_BUFFER,    // uniform block
    SPV_DESC_STORAGE_BUFFER,    // buffer block
} SpvDescKind;

typedef struct SpvBinding {
    u32 binding;
    SpvDescKind kind;
} SpvBinding;

// The bindings of descriptor set `set`, sorted by binding. Returns the count (at most max), -1 when
// the code is not SPIR-V.
int spv_reflect_bindings(const u32 *code, size_t words, u32 set, SpvBinding *out, int max);

#endif
