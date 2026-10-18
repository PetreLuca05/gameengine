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
// A directory the game writes into (caches, HDR screenshots): `rel` (ends with '/') under the data
// path (build tree, install dir) when that is writable, else under the pref dir (a signed macOS .app
// bundle, a read-only install: docs/PLATFORM_MACOS.md). Created when missing.
const char *platform_writable_dir(const char *rel, char *buf, size_t buf_size);

// Process setup before SDL / Vulkan (main): the MoltenVK configuration on macOS (environment
// defaults the user can still override; docs/PLATFORM_MACOS.md "MoltenVK configuration").
void platform_preinit(void);

double platform_time(void);            // seconds since start, high resolution
u32 platform_wall_ms(void);            // wall clock in ms (wraps): comparable between processes on one machine
void platform_sleep_ms(u32 ms);

#endif
