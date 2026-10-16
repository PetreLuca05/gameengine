#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L   // fseeko / off_t under strict C11
#endif
#include "platform.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

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

double platform_time(void) {
    static u64 start, freq;
    if (!freq) { freq = SDL_GetPerformanceFrequency(); start = SDL_GetPerformanceCounter(); }
    return (double)(SDL_GetPerformanceCounter() - start) / (double)freq;
}

void platform_sleep_ms(u32 ms) { SDL_Delay(ms); }
