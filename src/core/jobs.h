// Basic thread pool. Jobs run on worker threads; an optional completion
// callback runs on the main thread inside jobs_pump_completions().
#ifndef DG_JOBS_H
#define DG_JOBS_H

#include "base.h"

typedef void (*JobFn)(void *user);

typedef struct JobCounter { volatile int pending; } JobCounter;

void jobs_init(int worker_count); // <=0 picks cores-1
void jobs_shutdown(void);
int jobs_worker_count(void);
bool jobs_on_worker(void);             // the calling thread is a worker (a job)

// Runs fn(user) on a worker. If done is non-NULL it runs on the main thread later.
// counter (optional) is incremented now and decremented when fn finishes.
void jobs_submit(JobFn fn, JobFn done, void *user, JobCounter *counter);
// The same for work the frame waits for: workers take it before the background jobs (PF-0043).
void jobs_submit_frame(JobFn fn, JobFn done, void *user, JobCounter *counter);
void jobs_wait(JobCounter *counter);   // helps run jobs while waiting
void jobs_pump_completions(int max);   // main thread; max<=0 means all

// fn(ctx, begin, end) over [0, count) in chunks, on the workers and the calling thread; returns
// when every chunk is done. Chunks are claimed in order but may run concurrently: fn must only
// write per-index outputs (results are then independent of the thread count). A job may call it
// too (the classic culling job, PF-0041): the caller runs chunks until none is left and then waits
// only for chunks already running, so it cannot wait on queued work; at most 16 loops in flight.
typedef void (*JobRangeFn)(void *ctx, int begin, int end);
void jobs_parallel_for(int count, int chunk, JobRangeFn fn, void *ctx);

#endif
