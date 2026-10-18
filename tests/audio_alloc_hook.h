// Force-included into test_synth / test_audio_env (every source of those targets): routes malloc /
// calloc / realloc / free through counters, so the tests can assert that mixer_render (the audio
// thread) never allocates. tests/test_synth.c defines the hooks.
#ifndef DG_AUDIO_ALLOC_HOOK_H
#define DG_AUDIO_ALLOC_HOOK_H
// Included before anything else: the POSIX clock for the tests' timing.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdlib.h>
#include <string.h>

void *test_hook_malloc(size_t n);
void *test_hook_calloc(size_t c, size_t n);
void *test_hook_realloc(void *p, size_t n);
void test_hook_free(void *p);

#ifndef DG_ALLOC_HOOK_IMPL
#define malloc(n) test_hook_malloc(n)
#define calloc(c, n) test_hook_calloc(c, n)
#define realloc(p, n) test_hook_realloc(p, n)
#define free(p) test_hook_free(p)
#endif

#endif
