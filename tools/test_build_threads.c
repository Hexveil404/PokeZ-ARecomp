// Build threads (runtime/nvn/vulkan/build_threads.h) on their own, no GPU (#251, #93):
//  1. a build a waiting draw takes out of the queue (run_queued_now) runs on a thread of its own, never on the draw's
//     (the render thread): the driver's compiler crashing there can't be survived - #251's crash dialog;
//  2. that thread's build "crashing" (as the crash handler does: build_thread_leave, then the thread stays put for good)
//     leaves the draw's thread going, with the build failed;
//  3. the render thread itself is no build thread (build_thread_leave: false - the crash report, not a silent hang);
//  4. a worker crashing gets a new worker in its slot: every queued build still gets built.
// Build: clang -std=gnu11 -O1 -pthread -Iruntime tools/test_build_threads.c -o build/test-build-threads
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t SDL_GetTicksNS(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int SDL_GetNumLogicalCPUCores(void) { return 4; }
static int plat_cpu_count(void) { return 8; }  // (8 - 4: 4 workers)
#define ENV(x) getenv(x)
#include "nvn/vulkan/build_threads.h"

static void sleep_ms(int ms) { struct timespec t = {0, ms * 1000000L}; nanosleep(&t, NULL); }
static bool wait_for(_Atomic int *state, int want, int ms) {
    for (int i = 0; i < ms && atomic_load(state) != want; i++) sleep_ms(1);
    return atomic_load(state) == want;
}

// Every worker held on a gate, so a build queued after them stays queued
static _Atomic int gate_open, held;
static void hold(void *a) { (void)a; atomic_fetch_add(&held, 1); while (!atomic_load(&gate_open)) sleep_ms(1); }

typedef struct { _Atomic int *state; pthread_t ran_on; bool crash; } Build;
static void build(void *a) {
    Build *b = a;
    b->ran_on = pthread_self();
    if (b->crash) {  // (the driver's compiler crashing: the crash handler's part)
        bool left = build_thread_leave();
        atomic_store(b->state, left ? 2 : 3);  // (2: failed, as vk_compile_abandon marks it; 3: couldn't leave)
        if (left) for (;;) sleep_ms(1000);      // (the thread stays where it is, for good)
        return;
    }
    atomic_store(b->state, 1);
}

int main(void) {
    pthread_t render = pthread_self();
    // 3. this thread (the render thread's role) is no build thread
    assert(!build_thread_leave());

    for (int i = 0; i < 4; i++) run_job(hold, NULL, NULL);
    for (int i = 0; i < 2000 && atomic_load(&held) < 4; i++) sleep_ms(1);
    assert(atomic_load(&nworkers) == 4 && atomic_load(&held) == 4);

    // 1. a waiting draw's build: off the render thread
    _Atomic int s1 = 0;
    Build b1 = {&s1, 0, false};
    run_job(build, &b1, &s1);
    assert(run_queued_now(&s1));  // (it was queued: taken out)
    assert(wait_for(&s1, 1, 2000));
    assert(!pthread_equal(b1.ran_on, render));
    assert(!run_queued_now(&s1));  // (no longer queued)
    printf("urgent build: ran on a thread of its own, not the render thread\n");

    // 2. that build crashing: the render thread goes on, the build failed
    _Atomic int s2 = 0;
    Build b2 = {&s2, 0, true};
    run_job(build, &b2, &s2);
    assert(run_queued_now(&s2));
    assert(wait_for(&s2, 2, 2000));  // (3 would be: the thread couldn't leave - the crash dialog)
    assert(!pthread_equal(b2.ran_on, render));
    printf("urgent build crashing: the thread left, the build failed, the render thread went on\n");

    // 4. a worker crashing: its slot gets a new worker, later builds still built
    atomic_store(&gate_open, 1);
    _Atomic int s3 = 0;
    Build b3 = {&s3, 0, true};
    run_job(build, &b3, &s3);
    assert(wait_for(&s3, 2, 2000));
    _Atomic int later[16];
    Build lb[16];
    for (int i = 0; i < 16; i++) {
        atomic_store(&later[i], 0);
        lb[i] = (Build){&later[i], 0, false};
        run_job(build, &lb[i], &later[i]);
    }
    for (int i = 0; i < 16; i++) assert(wait_for(&later[i], 1, 2000));
    printf("worker crashing: replaced, 16 later builds built\n");
    printf("ok\n");
    return 0;
}
