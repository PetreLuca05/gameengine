// The allocator counters behind tests/audio_alloc_hook.h: an allocation while mixer_in_render() is
// true (the audio thread) is counted; test_synth asserts the count stays 0.
#include "audio_alloc_hook.h"
#undef malloc
#undef calloc
#undef realloc
#undef free
#include "audio/mixer.h"

int g_audio_render_allocs;

void *test_hook_malloc(size_t n) { g_audio_render_allocs += mixer_in_render(); return malloc(n); }
void *test_hook_calloc(size_t c, size_t n) { g_audio_render_allocs += mixer_in_render(); return calloc(c, n); }
void *test_hook_realloc(void *p, size_t n) { g_audio_render_allocs += mixer_in_render(); return realloc(p, n); }
void test_hook_free(void *p) { g_audio_render_allocs += mixer_in_render() && p; free(p); }
