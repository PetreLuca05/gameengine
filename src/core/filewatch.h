// Hot reload polls off the main thread (docs/perf/FINDINGS.md PF-0045): a watch's signature (file
// stats, directory listings: a hitch of a frame every second on Windows) is computed on a job once a
// second and taken by the main thread at a later frame; the owner compares and reloads as before.
#ifndef DG_FILEWATCH_H
#define DG_FILEWATCH_H

#include "base.h"
#include "jobs.h"

// Runs on a worker: only file stats / listings and reads of state the main thread does not change
// while the watch is armed.
typedef u64 (*FileWatchFn)(void *user);

typedef struct FileWatch {
    FileWatchFn fn;
    void *user;
    u64 result;
    double next;          // the time of the next look
    JobCounter counter;
    bool pending;
} FileWatch;

// Main thread, every frame (now: seconds, any clock): true with *sig when a signature taken once a
// second is ready (computed in place while PF-0045 is off or without workers).
bool filewatch_poll(FileWatch *w, double now, FileWatchFn fn, void *user, u64 *sig);
// Waits for a look in flight (before the watched state goes away).
void filewatch_cancel(FileWatch *w);

#endif
