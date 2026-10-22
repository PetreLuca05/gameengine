// SPIR-V reflection tests (src/render/spv_reflect.c): the descriptor bindings of one set.
//   fixture   a compute shader with a sampler, a separate texture, a combined sampler, two storage
//             images, a uniform block and a storage buffer in sets 0 / 1: kinds, bindings, order
//   fsr2      the vendored FidelityFX FSR 2 passes as built (bin/shaders/fsr2, when present): set 0
//             holds the two samplers, set 1 the resources of each pass (post_fsr2.c builds its
//             descriptor set layouts from this)
//   garbage   not SPIR-V: -1
#include "render/spv_reflect.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "data/spv_reflect_fixture.h"

static int g_fail, g_checks;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static void test_fixture(void) {
    SpvBinding b[16];
    size_t words = sizeof(k_test_spv) / sizeof(k_test_spv[0]);
    int n = spv_reflect_bindings(k_test_spv, words, 1, b, 16);
    CHECK(n == 6, "set 1: %d bindings", n);
    static const struct { u32 binding; SpvDescKind kind; } want[6] = {
        { 0, SPV_DESC_SAMPLED_IMAGE }, { 2, SPV_DESC_COMBINED }, { 4, SPV_DESC_STORAGE_IMAGE },
        { 5, SPV_DESC_STORAGE_IMAGE }, { 7, SPV_DESC_UNIFORM_BUFFER }, { 9, SPV_DESC_STORAGE_BUFFER },
    };
    for (int i = 0; i < 6 && i < n; i++) CHECK(b[i].binding == want[i].binding && b[i].kind == want[i].kind, "set 1 [%d]: binding %u kind %d", i, b[i].binding, (int)b[i].kind);
    n = spv_reflect_bindings(k_test_spv, words, 0, b, 16);
    CHECK(n == 1 && b[0].binding == 0 && b[0].kind == SPV_DESC_SAMPLER, "set 0: %d bindings", n);
    CHECK(spv_reflect_bindings(k_test_spv, words, 2, b, 16) == 0, "set 2 is empty");
    CHECK(spv_reflect_bindings(k_test_spv, words, 1, b, 3) == 3, "max respected");
}

static u32 *read_spv(const char *path, size_t *words) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u32 *code = size > 0 && size % 4 == 0 ? (u32 *)malloc((size_t)size) : NULL;
    if (code && fread(code, 1, (size_t)size, f) != (size_t)size) { free(code); code = NULL; }
    fclose(f);
    *words = code ? (size_t)size / 4 : 0;
    return code;
}

static void test_fsr2(void) {
    // pass, set 1 binding count, a storage image binding, the FSR 2 constant block's binding
    static const struct { const char *name; int count; u32 storage, cb; } passes[] = {
        { "compute_luminance_pyramid", 7, 2, 5 }, { "reconstruct_previous_depth", 12, 5, 12 },
        { "depth_clip", 14, 12, 14 },               { "lock", 4, 1, 3 },
        { "accumulate", 17, 13, 18 },
    };
    int found = 0;
    for (int p = 0; p < (int)(sizeof(passes) / sizeof(passes[0])); p++) {
        char path[256];
        snprintf(path, sizeof(path), "shaders/fsr2/%s.comp.spv", passes[p].name);
        size_t words = 0;
        u32 *code = read_spv(path, &words);
        if (!code) continue;
        found++;
        SpvBinding b[32];
        int n = spv_reflect_bindings(code, words, 1, b, 32);
        CHECK(n == passes[p].count, "%s: %d bindings in set 1 (want %d)", passes[p].name, n, passes[p].count);
        bool storage = false, cb = false;
        for (int i = 0; i < n; i++) {
            if (b[i].binding == passes[p].storage) storage = b[i].kind == SPV_DESC_STORAGE_IMAGE;
            if (b[i].binding == passes[p].cb) cb = b[i].kind == SPV_DESC_UNIFORM_BUFFER;
        }
        CHECK(storage && cb, "%s: storage image %u / constant block %u", passes[p].name, passes[p].storage, passes[p].cb);
        n = spv_reflect_bindings(code, words, 0, b, 32);
        CHECK(n == 2 && b[0].kind == SPV_DESC_SAMPLER && b[1].kind == SPV_DESC_SAMPLER, "%s: set 0 = %d (the two samplers)", passes[p].name, n);
        free(code);
    }
    printf("fsr2: %d passes reflected%s\n", found, found ? "" : " (bin/shaders/fsr2 not found, skipped)");
}

static void test_garbage(void) {
    u32 junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    SpvBinding b[4];
    CHECK(spv_reflect_bindings(junk, 8, 0, b, 4) == -1, "not SPIR-V");
    CHECK(spv_reflect_bindings(k_test_spv, 3, 0, b, 4) == -1, "truncated header");
    // a stream cut in the middle of an instruction: what was parsed is reported, nothing read past the end
    size_t words = sizeof(k_test_spv) / sizeof(k_test_spv[0]);
    CHECK(spv_reflect_bindings(k_test_spv, words / 2, 1, b, 4) >= 0, "cut stream");
}

int main(void) {
    test_fixture();
    test_fsr2();
    test_garbage();
    printf("spv_reflect: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
