// Hot reload polls on a job (filewatch.h, docs/perf/FINDINGS.md PF-0045).
#include "filewatch.h"
#include "profiler.h"
#include <SDL3/SDL.h>

static void watch_job(void *u) {
    FileWatch *w = (FileWatch *)u;
    w->result = w->fn(w->user);
}

bool filewatch_poll(FileWatch *w, double now, FileWatchFn fn, void *user, u64 *sig) {
    if (w->pending) {
        if (SDL_GetAtomicInt((SDL_AtomicInt *)&w->counter.pending) > 0) return false;   // still looking
        w->pending = false;
        *sig = w->result;
        return true;
    }
    if (now < w->next) return false;
    w->next = now + 1.0;
    if (prof_fix_enabled(PFX_FILEWATCH_JOB) && jobs_worker_count() > 0) {
        w->fn = fn;
        w->user = user;
        w->pending = true;
        jobs_submit(watch_job, NULL, w, &w->counter);
        return false;
    }
    *sig = fn(user);
    return true;
}

void filewatch_cancel(FileWatch *w) {
    if (!w->pending) return;
    jobs_wait(&w->counter);
    w->pending = false;
}
