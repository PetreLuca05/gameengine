#include "engine.h"
#include "jobs.h"
#include "log.h"
#include "profiler.h"
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

#define JOB_QUEUE_SIZE 1024
#define MAX_WORKERS 16

typedef struct Job { JobFn fn, done; void *user; JobCounter *counter; u64 submitted; } Job;   // submitted: prof_ticks (queue latency)

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

static void pf_helper(void *u);

static void run_job(Job *j) {
    bool rec = j->fn != pf_helper;   // parallel_for helpers record themselves (parfor:<fn>)
    if (rec) prof_job_begin((const void *)j->fn, j->submitted, false);
    j->fn(j->user);
    if (rec) prof_job_end();
    if (j->done) {
        SDL_LockMutex(s_jobs.done_mutex);
        if (!queue_push(&s_jobs.completed, *j)) log_error("jobs: completion queue full");
        SDL_UnlockMutex(s_jobs.done_mutex);
    }
    if (j->counter) SDL_AtomicDecRef((SDL_AtomicInt *)&j->counter->pending);
}

static int worker_main(void *arg) {
#ifdef __APPLE__
    // macOS has no affinity API: the QoS class keeps the workers on the performance cores of Apple
    // Silicon (the default class may land them on the efficiency cores; docs/PLATFORM_MACOS.md).
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    char name[32];
    SDL_snprintf(name, sizeof(name), "dg_worker_%d", (int)(intptr_t)arg);
    prof_thread_name(name);
    for (;;) {
        // Bursty frame work: spin briefly for more before sleeping (a condition wake costs
        // tens of microseconds, longer than most chunks).
        for (int spin = 0; spin < 4000 && !s_jobs.quit && *(volatile int *)&s_jobs.pending.count == 0; spin++) SDL_CPUPauseInstruction();
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
        s_jobs.threads[i] = SDL_CreateThread(worker_main, name, (void *)(intptr_t)i);
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
    Job j = { fn, done, user, counter, prof_ticks() };
    prof_count(PC_JOBS, 1);
    if (counter) SDL_AtomicIncRef((SDL_AtomicInt *)&counter->pending);
    SDL_LockMutex(s_jobs.mutex);
    bool ok = queue_push(&s_jobs.pending, j);
    SDL_SignalCondition(s_jobs.cond);
    SDL_UnlockMutex(s_jobs.mutex);
    if (!ok) run_job(&j); // queue full: run inline
}

void jobs_wait(JobCounter *counter) {
    bool waiting = false;   // profiler: time with nothing left to help with (a dependency stall)
    while (SDL_GetAtomicInt((SDL_AtomicInt *)&counter->pending) > 0) {
        SDL_LockMutex(s_jobs.mutex);
        Job j;
        bool got = queue_pop(&s_jobs.pending, &j);
        SDL_UnlockMutex(s_jobs.mutex);
        if (got) {
            if (waiting) { PROF_WAIT_END(); waiting = false; }
            run_job(&j);
        } else {
            if (!waiting && dg_prof_recording_on()) { PROF_WAIT_BEGIN(PW_JOBS_WAIT); waiting = true; }
            SDL_Delay(0);
        }
    }
    if (waiting) PROF_WAIT_END();
}

// ------------------------------------------------------------------ parallel for

typedef struct ParFor {
    JobRangeFn fn;
    void *ctx;
    int count, chunk, chunks;
    SDL_AtomicInt next, done, gen;
} ParFor;
#define PARFOR_SLOTS 16
static ParFor g_pf[PARFOR_SLOTS];
static SDL_AtomicInt g_pf_seq;

static void pf_run(ParFor *p, int gen) {
    for (;;) {
        if (SDL_GetAtomicInt(&p->gen) != gen) return;   // a late helper: this loop is over
        int c = SDL_AddAtomicInt(&p->next, 1);
        if (c >= p->chunks) return;
        int b = c * p->chunk;
        p->fn(p->ctx, b, DG_MIN(p->count, b + p->chunk));
        SDL_AddAtomicInt(&p->done, 1);
    }
}

typedef struct PfHelper { ParFor *p; int gen; } PfHelper;
static PfHelper g_pf_helpers[PARFOR_SLOTS][MAX_WORKERS];
static void pf_helper(void *u) {
    PfHelper *h = (PfHelper *)u;
    prof_job_begin((const void *)h->p->fn, 0, true);
    pf_run(h->p, h->gen);
    prof_job_end();
}

void jobs_parallel_for(int count, int chunk, JobRangeFn fn, void *ctx) {
    if (count <= 0) return;
    chunk = DG_MAX(1, chunk);
    int chunks = (count + chunk - 1) / chunk;
    prof_count(PC_PARFOR, 1);
    if (!g_engine.jobs || s_jobs.worker_count <= 0 || chunks <= 1) { fn(ctx, 0, count); return; }
    int slot = (int)((u32)SDL_AddAtomicInt(&g_pf_seq, 1) % PARFOR_SLOTS);
    ParFor *p = &g_pf[slot];
    int gen = SDL_GetAtomicInt(&p->gen) + 1;
    p->fn = fn;
    p->ctx = ctx;
    p->count = count;
    p->chunk = chunk;
    p->chunks = chunks;
    SDL_SetAtomicInt(&p->next, 0);
    SDL_SetAtomicInt(&p->done, 0);
    SDL_SetAtomicInt(&p->gen, gen);
    int helpers = DG_MIN(s_jobs.worker_count, chunks - 1);
    SDL_LockMutex(s_jobs.mutex);
    for (int k = 0; k < helpers; k++) {
        PfHelper *h = &g_pf_helpers[slot][k];
        h->p = p;
        h->gen = gen;
        Job j = { pf_helper, NULL, h, NULL, 0 };
        queue_push(&s_jobs.pending, j);
    }
    SDL_BroadcastCondition(s_jobs.cond);
    SDL_UnlockMutex(s_jobs.mutex);
    prof_job_begin((const void *)fn, 0, true);
    pf_run(p, gen);
    prof_job_end();
    PROF_WAIT_BEGIN(PW_PARFOR_TAIL);
    while (SDL_GetAtomicInt(&p->done) < chunks) SDL_CPUPauseInstruction();
    PROF_WAIT_END();
    // Helpers that never got a chunk see the generation change and leave.
    SDL_SetAtomicInt(&p->gen, gen + 1);
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
