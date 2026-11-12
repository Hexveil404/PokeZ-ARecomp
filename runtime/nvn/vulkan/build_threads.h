// Build threads for shaders and pipelines (runtime/nvn/vulkan/exec.c includes this; tools/test_build_threads.c tests it
// alone, #251): the workers' queue, the translator's, and the urgent builds a waiting draw hands to a thread of its own.
// Needs from its includer: SDL_GetTicksNS, SDL_GetNumLogicalCPUCores, plat_cpu_count, ENV.
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
// state: what the build sets when done (a draw waiting on it can run it itself: run_queued_now)
typedef struct Job { void (*fn)(void *); void *arg; _Atomic int *state; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;
// when each thread's current job started (0: idle) - wait_built says which one a stalled draw is stuck behind
#define MAX_WORKERS 16
static _Atomic uint64_t worker_t0[MAX_WORKERS + 1], translator_t0;  // (+1: the urgent builds, run_queued_now)
static _Atomic int nworkers;
static _Atomic uint64_t builds_done;  // (shaders / pipelines built, all session: renderer_shader_builds)
// Shader work for runtime/hle/framepace.c (seconds spent building shaders don't count against 60 fps): the builds
// done so far, top bit set while one is being built
uint64_t renderer_shader_builds(void) {
    uint64_t n = atomic_load(&builds_done);
    if (atomic_load(&translator_t0)) n |= 1ull << 63;
    for (int i = 0; i < atomic_load(&nworkers); i++) if (atomic_load(&worker_t0[i])) n |= 1ull << 63;
    return n;
}

static __thread int t_worker = -1;   // (this thread's worker slot; -1: not a worker. vk_compile_abandon)
static __thread bool t_prep_job;     // (a shader preparation job: counted in prep_pending)
static __thread _Atomic int *t_build_state;  // (the Pipe being built: its state)
static __thread int t_build_slot;
static void *worker(void *a) {
    int me = (int)(intptr_t)a;
    t_worker = me;
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (!jobs_head) pthread_cond_wait(&jobs_cond, &jobs_lock);
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        pthread_mutex_unlock(&jobs_lock);
        atomic_store(&worker_t0[me], SDL_GetTicksNS());
        j->fn(j->arg);
        atomic_store(&worker_t0[me], 0);
        free(j);
    }
    return NULL;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = ENV("LARECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation: one dedicated thread (the translator isn't re-entrant - translate.c's RSC2 string ring - and
// translating on the game thread cost up to ~90 ms per frame on entering an area). Compiles go on to the workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;  // (compute shaders translate on the game thread)
static Job *tjobs_head, *tjobs_tail;
static pthread_mutex_t tjobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tjobs_cond = PTHREAD_COND_INITIALIZER;
static void *translator(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&tjobs_lock);
        while (!tjobs_head) pthread_cond_wait(&tjobs_cond, &tjobs_lock);
        Job *j = tjobs_head;
        tjobs_head = j->next;
        if (!tjobs_head) tjobs_tail = NULL;
        pthread_mutex_unlock(&tjobs_lock);
        atomic_store(&translator_t0, SDL_GetTicksNS());
        j->fn(j->arg);
        atomic_store(&translator_t0, 0);
        free(j);
    }
    return NULL;
}
static bool sync_shaders(void);
static void run_translate(void (*fn)(void *), void *arg, _Atomic int *state) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) { started = true; pthread_t t; pthread_create(&t, NULL, translator, NULL); }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->state = state; j->next = NULL;
    pthread_mutex_lock(&tjobs_lock);
    if (tjobs_tail) tjobs_tail->next = j; else tjobs_head = j;
    tjobs_tail = j;
    pthread_cond_signal(&tjobs_cond);
    pthread_mutex_unlock(&tjobs_lock);
}

static void run_job(void (*fn)(void *), void *arg, _Atomic int *state) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) {
        started = true;
        // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
        int n = plat_cpu_count();  // (the process's affinity mask: a process held to a few cores, few workers)
        if (n <= 0) n = SDL_GetNumLogicalCPUCores();
        n -= 4;
        if (n < 4) n = 4;
        if (n > MAX_WORKERS) n = MAX_WORKERS;
        nworkers = n;
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, (void *)(intptr_t)i); }
    }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->state = state; j->next = NULL;
    pthread_mutex_lock(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

// A draw that has to wait for its build (wait_built) takes it out of the queues - the translator's, then the
// workers' - and runs it on its own thread: queued behind the dozens a new area or effect brings, it waited for all
// of them (over a second, a frozen frame). false: not queued (running, or done).
// The queued job building state, taken out of its queue (take) or moved to its front, for the next free thread: a draw
// skipped for it (wait_scene) gets it in a build or two, not after every build queued before it.
static Job *queued_job(_Atomic int *state, bool take, int *queue) {
    for (int q = 0; q < 2; q++) {
        pthread_mutex_t *lock = q ? &jobs_lock : &tjobs_lock;
        Job **head = q ? &jobs_head : &tjobs_head, **tail = q ? &jobs_tail : &tjobs_tail, *prev = NULL;
        pthread_mutex_lock(lock);
        for (Job **pj = head; *pj; prev = *pj, pj = &(*pj)->next) {
            Job *j = *pj;
            if (j->state != state) continue;
            if (take || pj != head) {
                *pj = j->next;
                if (*tail == j) *tail = prev;
                if (!take) j->next = *head, *head = j;
            }
            pthread_mutex_unlock(lock);
            if (queue) *queue = q;
            return j;
        }
        pthread_mutex_unlock(lock);
    }
    return NULL;
}
// A build taken out of the workers' queue runs on a thread of its own, not the render thread: the driver's compiler can
// crash on a pipeline (#93, #251: AMD R7 360, 22.6.1, amdvlk64.dll+885a8 on a full-screen pass's pipeline), and a build
// thread is left where it is while the game goes on without that pipeline (vk_compile_crash_recover) - the render
// thread can't be, so the crash dialog came instead, a run after run with one more pipeline listed each time. The
// draw waits on the build's state as before (wait_built polls it), so it costs a thread start (~0.1 ms). Translations
// (the first queue) are ours, not the driver's: still run here.
static void *urgent_build(void *a) {
    Job *j = a;
    t_worker = MAX_WORKERS;  // (vk_compile_abandon: a build thread, not one to start again)
    atomic_store(&worker_t0[MAX_WORKERS], SDL_GetTicksNS());
    j->fn(j->arg);
    atomic_store(&worker_t0[MAX_WORKERS], 0);
    free(j);
    return NULL;
}
static bool run_queued_now(_Atomic int *state) {
    int q = 0;
    Job *j = queued_job(state, true, &q);
    if (!j) return false;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    bool own = q == 1 && pthread_create(&t, &at, urgent_build, j) == 0;
    pthread_attr_destroy(&at);
    if (!own) { j->fn(j->arg); free(j); }  // (a translation, or no thread to be had)
    return true;
}


// A build thread whose build the driver's compiler crashed in leaves for good (vk_compile_abandon, from the crash
// handler, on that thread): a worker's slot gets a new worker, an urgent build's nothing. false: not a build thread
// (the render thread, the translator...) - it can't be left behind.
static bool build_thread_leave(void) {
    if (t_worker < 0) return false;
    atomic_store(&worker_t0[t_worker], 0);
    pthread_t t;
    if (t_worker < MAX_WORKERS) pthread_create(&t, NULL, worker, (void *)(intptr_t)t_worker);
    t_worker = -1;
    return true;
}
