#include "engine.h"
#include "jobs.h"
#include "log.h"
#include <SDL3/SDL.h>

#define JOB_QUEUE_SIZE 1024
#define MAX_WORKERS 16

typedef struct Job { JobFn fn, done; void *user; JobCounter *counter; } Job;

typedef struct JobQueue {
    Job items[JOB_QUEUE_SIZE];
    int head, tail, count;
} JobQueue;

typedef struct JobSystem {
    SDL_Thread *threads[MAX_WORKERS];
    int worker_count;
    SDL_Mutex *mutex;
    SDL_Condition *cond;
    JobQueue pending;
    JobQueue completed;
    SDL_Mutex *done_mutex;
    bool quit;
} JobSystem;
#define s_jobs (*g_engine.jobs)

static bool queue_push(JobQueue *q, Job j) {
    if (q->count >= JOB_QUEUE_SIZE) return false;
    q->items[q->tail] = j;
    q->tail = (q->tail + 1) % JOB_QUEUE_SIZE;
    q->count++;
    return true;
}

static bool queue_pop(JobQueue *q, Job *out) {
    if (q->count == 0) return false;
    *out = q->items[q->head];
    q->head = (q->head + 1) % JOB_QUEUE_SIZE;
    q->count--;
    return true;
}

static void run_job(Job *j) {
    j->fn(j->user);
    if (j->done) {
        SDL_LockMutex(s_jobs.done_mutex);
        if (!queue_push(&s_jobs.completed, *j)) log_error("jobs: completion queue full");
        SDL_UnlockMutex(s_jobs.done_mutex);
    }
    if (j->counter) SDL_AtomicDecRef((SDL_AtomicInt *)&j->counter->pending);
}

static int worker_main(void *arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(s_jobs.mutex);
        while (!s_jobs.quit && s_jobs.pending.count == 0) SDL_WaitCondition(s_jobs.cond, s_jobs.mutex);
        if (s_jobs.quit) { SDL_UnlockMutex(s_jobs.mutex); break; }
        Job j;
        bool got = queue_pop(&s_jobs.pending, &j);
        SDL_UnlockMutex(s_jobs.mutex);
        if (got) run_job(&j);
    }
    return 0;
}

void jobs_init(int worker_count) {
    g_engine.jobs = (JobSystem *)SDL_calloc(1, sizeof(JobSystem));
    if (worker_count <= 0) worker_count = SDL_GetNumLogicalCPUCores() - 1;
    worker_count = DG_CLAMP(worker_count, 1, MAX_WORKERS);
    s_jobs.mutex = SDL_CreateMutex();
    s_jobs.done_mutex = SDL_CreateMutex();
    s_jobs.cond = SDL_CreateCondition();
    s_jobs.worker_count = worker_count;
    for (int i = 0; i < worker_count; i++) {
        char name[32];
        SDL_snprintf(name, sizeof(name), "dg_worker_%d", i);
        s_jobs.threads[i] = SDL_CreateThread(worker_main, name, NULL);
    }
    log_info("jobs: %d worker threads", worker_count);
}

void jobs_shutdown(void) {
    SDL_LockMutex(s_jobs.mutex);
    s_jobs.quit = true;
    SDL_BroadcastCondition(s_jobs.cond);
    SDL_UnlockMutex(s_jobs.mutex);
    for (int i = 0; i < s_jobs.worker_count; i++) SDL_WaitThread(s_jobs.threads[i], NULL);
    jobs_pump_completions(0);
    SDL_DestroyCondition(s_jobs.cond);
    SDL_DestroyMutex(s_jobs.mutex);
    SDL_DestroyMutex(s_jobs.done_mutex);
    SDL_free(g_engine.jobs);
    g_engine.jobs = NULL;
}

int jobs_worker_count(void) { return s_jobs.worker_count; }

void jobs_submit(JobFn fn, JobFn done, void *user, JobCounter *counter) {
    Job j = { fn, done, user, counter };
    if (counter) SDL_AtomicIncRef((SDL_AtomicInt *)&counter->pending);
    SDL_LockMutex(s_jobs.mutex);
    bool ok = queue_push(&s_jobs.pending, j);
    SDL_SignalCondition(s_jobs.cond);
    SDL_UnlockMutex(s_jobs.mutex);
    if (!ok) run_job(&j); // queue full: run inline
}

void jobs_wait(JobCounter *counter) {
    while (SDL_GetAtomicInt((SDL_AtomicInt *)&counter->pending) > 0) {
        SDL_LockMutex(s_jobs.mutex);
        Job j;
        bool got = queue_pop(&s_jobs.pending, &j);
        SDL_UnlockMutex(s_jobs.mutex);
        if (got) run_job(&j);
        else SDL_Delay(0);
    }
}

void jobs_pump_completions(int max) {
    for (int n = 0; max <= 0 || n < max; n++) {
        SDL_LockMutex(s_jobs.done_mutex);
        Job j;
        bool got = queue_pop(&s_jobs.completed, &j);
        SDL_UnlockMutex(s_jobs.done_mutex);
        if (!got) break;
        j.done(j.user);
    }
}
