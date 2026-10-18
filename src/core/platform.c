#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L   // fseeko / off_t under strict C11
#endif
#include "platform.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *platform_read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *data = (char *)malloc((size_t)len + 1);
    size_t got = fread(data, 1, (size_t)len, f);
    fclose(f);
    data[got] = 0;
    if (out_size) *out_size = got;
    return data;
}

bool platform_read_file_range(const char *path, u64 offset, void *dst, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
#ifdef _WIN32
    int r = _fseeki64(f, (long long)offset, SEEK_SET);
#else
    int r = fseeko(f, (off_t)offset, SEEK_SET);
#endif
    size_t got = r == 0 ? fread(dst, 1, size, f) : 0;
    fclose(f);
    return got == size;
}

bool platform_write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, size, f);
    fclose(f);
    return w == size;
}

bool platform_file_exists(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info);
}

i64 platform_file_mtime(const char *path) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return 0;
    return (i64)info.modify_time;
}

const char *platform_base_dir(void) {
    const char *b = SDL_GetBasePath();
    return b ? b : "./";
}

const char *platform_pref_dir(void) {
    static char *pref;
    if (!pref) pref = SDL_GetPrefPath("Deadground", "Deadground");
    return pref ? pref : "./";
}

const char *platform_data_path(const char *rel, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "%s%s", platform_base_dir(), rel);
    if (platform_file_exists(buf)) return buf;
#ifdef DG_SOURCE_DIR
    snprintf(buf, buf_size, "%s/%s", DG_SOURCE_DIR, rel);
    if (platform_file_exists(buf)) return buf;
#endif
    snprintf(buf, buf_size, "%s", rel);
    return buf;
}

// The base dir is writable (probed once): false inside a macOS .app (Contents/Resources, signed and
// read-only for a notarized build) and in read-only installs.
static bool base_dir_writable(void) {
    static int state = -1;
    if (state < 0) {
        char probe[1024];
        snprintf(probe, sizeof(probe), "%s.dg_write_probe", platform_base_dir());
        bool bundle = strstr(platform_base_dir(), ".app/Contents/Resources") != NULL;
        state = !bundle && platform_write_file(probe, "", 0);
        if (state) SDL_RemovePath(probe);
    }
    return state == 1;
}

const char *platform_writable_dir(const char *rel, char *buf, size_t buf_size) {
    if (base_dir_writable()) platform_data_path(rel, buf, buf_size);
    else snprintf(buf, buf_size, "%s%s", platform_pref_dir(), rel);
    // create every level (SDL_CreateDirectory makes the parents)
    SDL_CreateDirectory(buf);
    return buf;
}

void platform_preinit(void) {
#ifdef __APPLE__
    // Metal fast math assumes no NaN / Inf: the NaN guards of TAA, exposure and the phone tonemap
    // would compile away. Precise math keeps the image the Windows / Linux drivers produce.
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);
    // The bindless vgeo textures (1152 combined image samplers per stage) need argument buffers.
    setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 0);
    // Pipeline creation on every core (macOS 13.3+): shorter first-launch shader compilation.
    setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1", 0);
#endif
}

double platform_time(void) {
    static u64 start, freq;
    if (!freq) { freq = SDL_GetPerformanceFrequency(); start = SDL_GetPerformanceCounter(); }
    return (double)(SDL_GetPerformanceCounter() - start) / (double)freq;
}

u32 platform_wall_ms(void) {
    // The wall clock at start plus the performance counter: steady within the process, and the
    // same time base in every process (platform_time starts at each process's own launch).
    static double base_ms;
    static bool init;
    if (!init) {
        SDL_Time now = 0;
        SDL_GetCurrentTime(&now);
        base_ms = (double)now / 1e6 - platform_time() * 1000.0;
        init = true;
    }
    return (u32)(u64)(base_ms + platform_time() * 1000.0);
}

void platform_sleep_ms(u32 ms) { SDL_Delay(ms); }
