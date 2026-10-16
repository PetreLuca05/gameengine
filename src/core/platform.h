// Thin platform layer: files, paths, time.
#ifndef DG_PLATFORM_H
#define DG_PLATFORM_H

#include "base.h"
#include <SDL3/SDL.h>

// Returns malloc'd, NUL-terminated contents (caller frees) or NULL.
char *platform_read_file(const char *path, size_t *out_size);
// Reads size bytes at offset into dst (false if the file is shorter). Thread-safe (own handle).
bool platform_read_file_range(const char *path, u64 offset, void *dst, size_t size);
bool platform_write_file(const char *path, const void *data, size_t size);
bool platform_file_exists(const char *path);
i64 platform_file_mtime(const char *path); // 0 if missing

// Resolves a data-relative path ("data/items.json") against the install dir,
// falling back to the source tree in dev builds.
const char *platform_data_path(const char *rel, char *buf, size_t buf_size);
const char *platform_pref_dir(void);   // writable user dir, ends with separator
const char *platform_base_dir(void);   // exe dir, ends with separator

double platform_time(void);            // seconds since start, high resolution
void platform_sleep_ms(u32 ms);

#endif
