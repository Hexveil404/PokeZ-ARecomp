// The recorder thread: see record.h.
//
// The render thread translated the game's command lists and made every Vulkan call itself. On a slow CPU (Steam
// Deck) the driver's share of those calls - building the GPU commands behind vkCmd*, allocating and writing
// descriptor sets, the submissions - is a quarter to a third of that thread's frame, and it depends on nothing the
// thread decides next. So the translating thread now writes each call's arguments into a packet in a ring, in the
// order it made them, and this thread makes the real calls in that order while the translator goes on with the next
// draw. A packet holds everything the call needs, by value: the recorder never reads the translator's state or the
// game's memory.
//
// Shared state and how it is protected (T = the translating thread, R = the recorder):
//   ring bytes          written by T before `head` is published (release), read by R after it loads `head`
//                       (acquire); reused by T only past `tail` (R's release store after each packet, T's acquire)
//   head / tail         _Atomic; the only hand-off (see above). `wpos` (T's unpublished end) is T's alone
//   wake-up             R sets `asleep` under `mu` and re-checks `head` before waiting on cv_work (seq_cst stores and
//                       loads on both sides: no lost wake-up); T signals only when it sees `asleep`
//   submitted           _Atomic, stored by R after vkQueueSubmit2 returns (release); anyone reads it. T's own count of
//                       what it queued is exec.c's submit_seq (seq_lock)
//   presents_done       _Atomic, stored by R after vkQueuePresentKHR; present_recreate: _Atomic flag R sets, T clears
//   cv_done             R broadcasts under `mu` after each submission / present packet and whenever it catches up:
//                       waiters for a submission, a present count, a drain or ring room sleep on it (timed, so a late
//                       broadcast costs at most a tick)
//   command buffers     allocated once, before R starts (exec.c vk_exec_init); from then on only R records, begins,
//   and the pool        ends and submits them. T calls into one directly only after rec_drain (ImGui, device.c)
//   descriptor pools    `pools[VK_SUBS]` and the batch tables: R's alone while the recorder is on (allocations and
//                       resets are packets); T's alone when off. The slot to use travels in the packet
//   stand-in sets       `rset[]` is written by R (the real set, at the alloc packet) and read by R (bind / update
//                       packets); T only hands out indices (`rset_next`, T's alone). An index is reused only
//                       RSETS allocations later, by which time every packet that named it was recorded, since
//                       packets run in order
//   the queue           R's alone (submissions, presents); T uses it only after rec_drain (vkDeviceWaitIdle,
//                       the heap import probe), when R is idle and nothing is queued
//   Vulkan objects in   images, views, buffers, samplers, pipelines, layouts, query pools: created by T (or the build
//   packets             workers) before the packet that names them; destroyed by T only once vkWaitSemaphores /
//                       vk_completed_seq says the submission that could use them is done - which implies R
//                       submitted it, which implies R recorded every packet before that submission (packets run
//                       in order, the timeline is signalled by the submission). So "destroy once the GPU is done"
//                       stays correct with the recorder in between
//   the transient ring  exec.c's chunks: written by T (the data) and reused by T once their submission completed
//                       (the same argument); R only passes buffer handles and offsets through
//   stats               RecStats: T's fields plain (one writer), R's _Atomic relaxed adds; read by T
//   pipeline objects    (WAIT_PIPES) R reads Pipe.state / .pipe through the resolver: _Atomic state, the handle
//                       written by the build worker before the state's release store
#include "record.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../platform.h"
#include "../rqueue.h"

// ================================================================== the ring
typedef void (*PktFn)(const void *args);
typedef struct { PktFn fn; uint32_t size; uint32_t pad; } PktHdr;  // fn NULL: skip to the ring's start
#define PKT_ALIGN 16
#define PUBLISH_EVERY (4u << 10)  // bytes queued before T publishes them (and at every submission / present / wait)

static uint8_t *ring;
static uint64_t ring_cap;  // (a power of two)
static _Atomic uint64_t head;  // (T) published end
static _Atomic uint64_t tail;  // (R) recorded end
static uint64_t wpos, published;  // (T) written end, the end last published
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_work = PTHREAD_COND_INITIALIZER, cv_done = PTHREAD_COND_INITIALIZER;
static _Atomic int asleep;
static _Atomic uint64_t submitted, presents_done;
static _Atomic bool quitting;  // (rec_quit) packets are dropped instead of run
static _Atomic int present_recreate;
static uint64_t presents_queued;  // (T)
static int on = -1;               // 1: the thread records, 0: calls at once, -1: not started
static pthread_t rec_thread;
static bool prof;
static RecStats st_t;             // T's counters
static _Atomic uint64_t r_busy_ns, r_wakeups, r_submit_ns, r_set_allocs, r_set_calls, r_pipe_waits, r_pipe_wait_ns, r_dropped;
static _Atomic uint64_t r_sets_fresh, r_pool_resets, r_pool_reset_ns;  // (R, or T without the recorder) pools: sets new, resets
static _Atomic uint64_t a_seq_waits, a_seq_wait_ns, a_present_waits, a_present_wait_ns;  // (waits from any thread)
extern __thread uint64_t g_blocked_ns;  // (kernel.c: the presenting thread's idle time, for the frame pacer)
static RecPipeResolve pipe_resolve;
static int wait_pipes = -1;

bool rec_on(void) { return on > 0; }
bool rec_wait_pipes(void) {
    if (wait_pipes < 0) wait_pipes = getenv("LARECOMP_VK_RECORDER_WAIT_PIPES") && atoi(getenv("LARECOMP_VK_RECORDER_WAIT_PIPES")) != 0;
    return wait_pipes;
}
void rec_set_pipe_resolver(RecPipeResolve f) { pipe_resolve = f; }

#define NOW() plat_now_ns()
#define ADD(v, x) atomic_fetch_add_explicit(&(v), (x), memory_order_relaxed)

static void timed_wait(pthread_cond_t *cv, uint64_t ns) {  // (mu held)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(ns % 1000000000ull);
    ts.tv_sec += (time_t)(ns / 1000000000ull);
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(cv, &mu, &ts);
}

// T: what it wrote so far is the recorder's to run
static void publish(void) {
    if (published == wpos) return;
    published = wpos;
    atomic_store_explicit(&head, wpos, memory_order_seq_cst);
    if (atomic_load_explicit(&asleep, memory_order_seq_cst)) {
        pthread_mutex_lock(&mu);
        pthread_cond_signal(&cv_work);
        pthread_mutex_unlock(&mu);
    }
}

// T: room for `need` bytes at wpos (the recorder frees room as it records; a full ring means it is behind by its
// whole size, 16 MB of packets - a long way, so the wait is rare)
static void wait_room(uint64_t need) {
    if (wpos + need - atomic_load_explicit(&tail, memory_order_acquire) <= ring_cap) return;
    publish();
    uint64_t t0 = NOW();
    rq_watch("the recorder (ring full)", wpos);
    pthread_mutex_lock(&mu);
    while (wpos + need - atomic_load_explicit(&tail, memory_order_acquire) > ring_cap) timed_wait(&cv_done, 1000000);
    pthread_mutex_unlock(&mu);
    rq_watch(NULL, 0);
    st_t.full_waits++;
    st_t.full_ns += NOW() - t0;
}

// T: a packet for fn with `args` bytes of arguments (+ `extra` bytes of arrays after them, each caller aligns its
// own); returns the arguments' place. Committed with commit() once filled.
static void *push(PktFn fn, size_t args, size_t extra) {
    uint64_t need = (sizeof(PktHdr) + ((args + PKT_ALIGN - 1) & ~(size_t)(PKT_ALIGN - 1)) + extra + PKT_ALIGN - 1) & ~(uint64_t)(PKT_ALIGN - 1);
    if (need > ring_cap / 4) FATAL("[vk] recorder: a packet of %llu bytes does not fit the ring\n", (unsigned long long)need);
    uint64_t at = wpos & (ring_cap - 1);
    if (at + need > ring_cap) {  // no room before the end: a skip packet, the real one at the start
        uint64_t skip = ring_cap - at;
        wait_room(skip);
        PktHdr *s = (PktHdr *)(ring + at);
        s->fn = NULL;
        s->size = (uint32_t)skip;
        wpos += skip;
        at = 0;
    }
    wait_room(need);
    PktHdr *h = (PktHdr *)(ring + at);
    h->fn = fn;
    h->size = (uint32_t)need;
    wpos += need;
    st_t.packets++;
    st_t.bytes += need;
    return h + 1;
}
static void commit(void) {
    if (wpos - published >= PUBLISH_EVERY) publish();
}
// (the arrays of a packet: after its arguments, 16-aligned)
#define AL16(n) (((n) + 15) & ~(size_t)15)
static inline void *after(const void *args, size_t args_size) { return (uint8_t *)args + AL16(args_size); }

// ---- the thread
static void *run(void *arg) {
    (void)arg;
    plat_thread_perf_core(PLAT_PERF_RENDER);  // (Windows hybrid CPUs: the performance cores, as the render thread)
    rq_watch(NULL, 0);                        // (the hang dump knows this thread)
    uint64_t t = 0;
    for (;;) {
        uint64_t h = atomic_load_explicit(&head, memory_order_acquire);
        if (h == t) {
            pthread_mutex_lock(&mu);
            atomic_store_explicit(&asleep, 1, memory_order_seq_cst);
            while ((h = atomic_load_explicit(&head, memory_order_seq_cst)) == t) pthread_cond_wait(&cv_work, &mu);
            atomic_store_explicit(&asleep, 0, memory_order_relaxed);
            pthread_mutex_unlock(&mu);
            ADD(r_wakeups, 1);
        }
        uint64_t t0 = prof ? NOW() : 0;
        while (t != h) {
            const PktHdr *p = (const PktHdr *)(ring + (t & (ring_cap - 1)));
            if (p->fn && !atomic_load_explicit(&quitting, memory_order_relaxed)) p->fn(p + 1);  // (rec_quit: dropped)
            t += p->size;
            atomic_store_explicit(&tail, t, memory_order_release);
        }
        if (prof) ADD(r_busy_ns, NOW() - t0);
        pthread_mutex_lock(&mu);  // caught up: drains and waiters
        pthread_cond_broadcast(&cv_done);
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static void stats_init(void);
void rec_init(bool prof_on) {
    if (on >= 0) return;
    prof = prof_on;
    const char *e = getenv("LARECOMP_VK_RECORDER");
    int want = !e || atoi(e) != 0;
    // ARM's Mali drivers: the GPU was lost a few seconds into the title screen with the recorder thread on (Mali-G610,
    // #358) and ran fine with it off; LARECOMP_VK_RECORDER=1 still turns it on there
    bool mali = g_vk.props.vendorID == 0x13B5;
    if (mali && !e) want = 0;
    on = 0;
    if (want) {
        const char *mb = getenv("LARECOMP_VK_RECORDER_MB");
        uint64_t cap = (uint64_t)(mb && atoi(mb) > 0 ? atoi(mb) : 16) << 20;
        while (cap & (cap - 1)) cap &= cap - 1;  // (a power of two)
        ring = malloc(cap);
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1 << 20);
        if (!ring || pthread_create(&th, &attr, run, NULL) != 0) {
            LOG("[vk] recorder thread: could not start (%s): Vulkan calls on the render thread\n", ring ? "thread" : "memory");
            free(ring);
            ring = NULL;
        } else {
            ring_cap = cap;
            rec_thread = th;
            pthread_detach(th);
            on = 1;
        }
        pthread_attr_destroy(&attr);
    }
    stats_init();
    LOG("[vk] recorder thread %s%s\n", on ? "on" : "off", on && rec_wait_pipes() ? " (draws wait for their pipelines)" :
        mali && !e ? " (Mali driver)" : "");
}

// ================================================================== waits
// A wait on the recorder this long (a quarter second) is logged: the recorder was stuck in a driver call (a slow
// vkQueueSubmit2 / vkQueuePresentKHR is logged too, just before) or the whole process stalled
static void slow_wait(const char *what, uint64_t ns) {
    if (ns <= 250000000) return;
    static _Atomic int n;
    if (atomic_fetch_add_explicit(&n, 1, memory_order_relaxed) < 20) LOG("[vk] recorder: a wait for %s took %llu ms\n", what, (unsigned long long)(ns / 1000000));
}

uint64_t rec_submitted(void) { return atomic_load_explicit(&submitted, memory_order_acquire); }
uint64_t rec_presents_queued(void) { return presents_queued; }

void rec_wait_submitted(uint64_t seq) {
    if (on <= 0 || rec_submitted() >= seq) return;
    uint64_t t0 = NOW();
    rq_watch("the recorder (submission)", seq);
    pthread_mutex_lock(&mu);
    while (rec_submitted() < seq) timed_wait(&cv_done, 2000000);
    pthread_mutex_unlock(&mu);
    rq_watch(NULL, 0);
    uint64_t dt = NOW() - t0;
    ADD(a_seq_waits, 1);
    ADD(a_seq_wait_ns, dt);
    slow_wait("a submission", dt);
}

void rec_wait_presents(uint64_t n) {
    if (on <= 0 || atomic_load_explicit(&presents_done, memory_order_acquire) >= n) return;
    uint64_t t0 = NOW();
    rq_watch("the recorder (present)", n);
    pthread_mutex_lock(&mu);
    while (atomic_load_explicit(&presents_done, memory_order_acquire) < n) timed_wait(&cv_done, 2000000);
    pthread_mutex_unlock(&mu);
    rq_watch(NULL, 0);
    uint64_t dt = NOW() - t0;
    ADD(a_present_waits, 1);
    ADD(a_present_wait_ns, dt);
    slow_wait("the last present", dt);
}

static void drain_to(uint64_t upto, uint64_t *n, uint64_t *ns) {
    uint64_t at = atomic_load_explicit(&tail, memory_order_acquire);
    if (at >= upto) return;
    uint64_t t0 = NOW();
    rq_watch("the recorder (drain)", upto);
    pthread_mutex_lock(&mu);
    while (atomic_load_explicit(&tail, memory_order_acquire) < upto) timed_wait(&cv_done, 2000000);
    pthread_mutex_unlock(&mu);
    rq_watch(NULL, 0);
    uint64_t dt = NOW() - t0;
    (*n)++;
    *ns += dt;
    if (dt > 250000000) {  // (a backlog of packets, or the recorder stuck in one driver call: see slow_wait)
        static int k;
        if (k++ < 20) LOG("[vk] recorder: a drain took %llu ms (%llu KB of packets behind)\n", (unsigned long long)(dt / 1000000),
                          (unsigned long long)((upto - at) / 1024));
    }
}
void rec_drain(void) {
    if (on <= 0) return;
    publish();
    drain_to(wpos, &st_t.drains, &st_t.drain_ns);
}
void rec_drain_published(void) {
    if (on <= 0 || pthread_equal(pthread_self(), rec_thread)) return;  // (an exit from the recorder itself: nothing to wait for)
    uint64_t upto = atomic_load_explicit(&head, memory_order_acquire), t0 = NOW();
    pthread_mutex_lock(&mu);  // (bounded: at exit a recorder stuck in the driver must not keep the process alive)
    while (atomic_load_explicit(&tail, memory_order_acquire) < upto && NOW() - t0 < 5000000000ull) timed_wait(&cv_done, 2000000);
    pthread_mutex_unlock(&mu);
}

void rec_quit(void) {
    if (on <= 0) return;
    atomic_store(&quitting, true);
    rec_drain_published();  // (the packet it is in finishes; the rest are skipped)
}

// ================================================================== stats
static void stats_init(void) { memset(&st_t, 0, sizeof st_t); }
void rec_stats_take(RecStats *o) {
    *o = st_t;
    memset(&st_t, 0, sizeof st_t);
    o->busy_ns = atomic_exchange_explicit(&r_busy_ns, 0, memory_order_relaxed);
    o->wakeups = atomic_exchange_explicit(&r_wakeups, 0, memory_order_relaxed);
    o->submit_ns = atomic_exchange_explicit(&r_submit_ns, 0, memory_order_relaxed);
    o->set_allocs = atomic_exchange_explicit(&r_set_allocs, 0, memory_order_relaxed);
    o->set_calls = atomic_exchange_explicit(&r_set_calls, 0, memory_order_relaxed);
    o->sets_fresh = atomic_exchange_explicit(&r_sets_fresh, 0, memory_order_relaxed);
    o->pool_resets = atomic_exchange_explicit(&r_pool_resets, 0, memory_order_relaxed);
    o->pool_reset_ns = atomic_exchange_explicit(&r_pool_reset_ns, 0, memory_order_relaxed);
    o->pipe_waits = atomic_exchange_explicit(&r_pipe_waits, 0, memory_order_relaxed);
    o->pipe_wait_ns = atomic_exchange_explicit(&r_pipe_wait_ns, 0, memory_order_relaxed);
    o->draws_dropped = atomic_exchange_explicit(&r_dropped, 0, memory_order_relaxed);
    o->seq_waits = atomic_exchange_explicit(&a_seq_waits, 0, memory_order_relaxed);
    o->seq_wait_ns = atomic_exchange_explicit(&a_seq_wait_ns, 0, memory_order_relaxed);
    o->present_waits = atomic_exchange_explicit(&a_present_waits, 0, memory_order_relaxed);
    o->present_wait_ns = atomic_exchange_explicit(&a_present_wait_ns, 0, memory_order_relaxed);
}

// (T) time spent queuing: around every rec_* that queues, when profiling
#define ENQ_T0() uint64_t enq_t0_ = prof ? NOW() : 0
#define ENQ_END() do { if (prof) st_t.enqueue_ns += NOW() - enq_t0_; } while (0)

// ================================================================== descriptor pools (per submission slot)
// Sets are allocated in batches per layout (LARECOMP_VK_NO_SET_BATCH=1: one per call, as before): a layout's batch
// grows with the sets it holds so far (1, 1, 2, 4 ... 32 a call), so at most about half goes unused, at most 31 sets
// once it allocates 32 at a time.
// Set recycling (LARECOMP_VK_NO_SET_RECYCLE=1: off): resetting a slot's pools at every submission cost the Intel UHD
// 770's Windows driver ~0.2 ms each (6-7 a frame). Once the slot's previous submission is done (rec_reset_pools comes
// after the slot's GPU wait, in order with the commands), its sets are handed out again instead (every set is written
// in full before it is bound, as a new one), and the pools are reset only when the stock outgrew the slot's first
// pool, a set had to be allocated outside it (table full), or the table is 3/4 full. Without recycling, the pools are
// reset only if a set was allocated since the last reset.
#define STOCK_TAB 1024
#define SET_BATCH 32
// A layout's sets in a slot: sets[0..count) allocated from the slot's pools, [0..next) handed out this submission
struct SetStock { VkDescriptorSetLayout layout; uint32_t next, count, cap; VkDescriptorSet *sets; };
typedef struct {
    VkDescriptorPool pools[16];
    int npools, cur_pool;
    struct SetStock *stock;  // the slot's sets per layout (NULL: none yet)
    uint16_t stock_used[STOCK_TAB];  // (its entries in use)
    int nstock_used;
    bool sets_allocated, sets_untracked;  // since the pools' last reset: any set allocated, any outside the stock
} Pools;
static Pools pools[VK_SUBS];
static int set_batch = -1, set_recycle = -1;

static bool alloc_sets(Pools *s, VkDescriptorSetLayout layout, uint32_t count, VkDescriptorSet *out) {
    VkDescriptorSetLayout ls[SET_BATCH];
    for (uint32_t i = 0; i < count; i++) ls[i] = layout;
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) {
                if (count > 1) return false;  // (the caller asks for one)
                FATAL("descriptor pools exhausted\n");
            }
            // exec.c's set reuse moves most uniform buffers to the dynamic type: the pool then holds 8192 static + 16384
            // dynamic instead of 32768 static. Without it (LARECOMP_VK_NO_SET_REUSE=1), the pool is as in beta_5: no
            // dynamic descriptors (a pool's size costs on some drivers: the Intel UHD 770's Windows driver spent ~0.4 ms
            // more a frame with 32768 more of them). A pool that runs out of a type is followed by another.
            static int dynp = -1;
            if (dynp < 0) dynp = getenv("LARECOMP_VK_NO_SET_REUSE") == NULL;  // (as exec.c set_reuse_on)
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, dynp ? 8192 : 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16384}};  // (exec.c stage_set: set reuse)
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = dynp ? 7 : 6, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = count, .pSetLayouts = ls};
        ADD(r_set_calls, 1);
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, out);
        if (r == VK_SUCCESS) { s->sets_allocated = true; ADD(r_sets_fresh, count); return true; }
        s->cur_pool++;
    }
}

static VkDescriptorSet pools_alloc(Pools *s, VkDescriptorSetLayout layout) {
    if (set_batch < 0) set_batch = getenv("LARECOMP_VK_NO_SET_BATCH") == NULL;
    if (set_recycle < 0) set_recycle = getenv("LARECOMP_VK_NO_SET_RECYCLE") == NULL;
    ADD(r_set_allocs, 1);
    struct SetStock *e = NULL;
    if (set_batch || set_recycle) {
        if (!s->stock) s->stock = calloc(STOCK_TAB, sizeof *s->stock);
        uintptr_t h = (uintptr_t)layout;
        h ^= h >> 17;
        h *= 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0, j = (uint32_t)(h >> 40) & (STOCK_TAB - 1); i < STOCK_TAB; i++, j = (j + 1) & (STOCK_TAB - 1))
            if (s->stock[j].layout == layout || !s->stock[j].layout) { e = &s->stock[j]; break; }
    }
    if (e) {
        if (!e->layout) {
            e->layout = layout; e->next = e->count = 0;
            s->stock_used[s->nstock_used++] = (uint16_t)(e - s->stock);
        }
        if (e->next < e->count) return e->sets[e->next++];
        uint32_t want = !set_batch ? 1 : e->count < 1 ? 1 : e->count > SET_BATCH ? SET_BATCH : e->count;
        if (e->count + want > e->cap) {
            uint32_t nc = e->cap ? e->cap * 2 : 32;
            while (nc < e->count + want) nc *= 2;
            e->sets = realloc(e->sets, nc * sizeof *e->sets);
            e->cap = nc;
        }
        if (!alloc_sets(s, layout, want, e->sets + e->count)) {  // (pools exhausted for a batch: one)
            want = 1;
            alloc_sets(s, layout, 1, e->sets + e->count);
        }
        e->count += want;
        return e->sets[e->next++];
    }
    s->sets_untracked = true;  // (table full, or neither batches nor recycling)
    VkDescriptorSet set;
    alloc_sets(s, layout, 1, &set);
    return set;
}

bool rec_set_batch(void) {
    if (set_batch < 0) set_batch = getenv("LARECOMP_VK_NO_SET_BATCH") == NULL;
    return set_batch;
}

bool rec_set_recycle(void) {
    if (set_recycle < 0) set_recycle = getenv("LARECOMP_VK_NO_SET_RECYCLE") == NULL;
    return set_recycle;
}

static void pools_reset(Pools *s) {  // (the slot's previous submission is done)
    if (set_recycle < 0) set_recycle = getenv("LARECOMP_VK_NO_SET_RECYCLE") == NULL;
    bool full = !set_recycle || s->cur_pool > 0 || s->sets_untracked || s->nstock_used > STOCK_TAB * 3 / 4;
    if (!full) {
        for (int i = 0; i < s->nstock_used; i++) s->stock[s->stock_used[i]].next = 0;
        return;
    }
    if (s->sets_allocated) {
        uint64_t tr = prof ? NOW() : 0;
        int np = s->cur_pool < s->npools ? s->cur_pool + 1 : s->npools;  // (the pools sets came from: up to cur_pool)
        for (int i = 0; i < np; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        ADD(r_pool_resets, (uint64_t)np);
        if (prof) ADD(r_pool_reset_ns, NOW() - tr);
    }
    s->cur_pool = 0;
    s->sets_allocated = s->sets_untracked = false;
    for (int i = 0; i < s->nstock_used; i++) {  // (their sets went with the pools; the arrays are kept)
        struct SetStock *e = &s->stock[s->stock_used[i]];
        e->layout = VK_NULL_HANDLE; e->next = e->count = 0;
    }
    s->nstock_used = 0;
}

// ---- stand-ins: a handle that names a slot of rset[], filled by the recorder at the allocation packet
#define RSETS (1u << 18)  // (more than a slot's pools can hold: 16 x 8192)
#define RSET_TAG 0xD5ED000000000000ull  // (no pointer and no small integer has these top bits)
static VkDescriptorSet rset[RSETS];  // (R)
static uint32_t rset_next;           // (T)
static inline bool is_standin(VkDescriptorSet s) { return ((uint64_t)(uintptr_t)s & 0xFFFF000000000000ull) == RSET_TAG; }
static inline VkDescriptorSet resolve_set(VkDescriptorSet s) {
    return is_standin(s) ? rset[(uint32_t)(uintptr_t)s & (RSETS - 1)] : s;
}

typedef struct { VkDescriptorSetLayout layout; uint32_t slot, index; } AllocSetArgs;
static void alloc_set_run(const void *p) {
    const AllocSetArgs *a = p;
    rset[a->index & (RSETS - 1)] = pools_alloc(&pools[a->slot], a->layout);
}
VkDescriptorSet rec_alloc_set(int slot, VkDescriptorSetLayout layout) {
    if (on <= 0) return pools_alloc(&pools[slot], layout);
    ENQ_T0();
    uint32_t i = rset_next++;
    AllocSetArgs *a = push(alloc_set_run, sizeof *a, 0);
    *a = (AllocSetArgs){layout, (uint32_t)slot, i};
    commit();
    ENQ_END();
    return (VkDescriptorSet)(uintptr_t)(RSET_TAG | i);
}

static void reset_pools_run(const void *p) { pools_reset(&pools[*(const uint32_t *)p]); }
void rec_reset_pools(int slot) {
    if (on <= 0) { pools_reset(&pools[slot]); return; }
    ENQ_T0();
    *(uint32_t *)push(reset_pools_run, sizeof(uint32_t), 0) = (uint32_t)slot;
    commit();
    ENQ_END();
}

static bool write_is_image(const VkWriteDescriptorSet *w) {
    switch (w->descriptorType) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return true;
    default: return false;
    }
}
typedef union { VkDescriptorBufferInfo b; VkDescriptorImageInfo i; } WriteInfo;
typedef struct { uint32_t n; } UpdateArgs;  // then: n VkWriteDescriptorSet, then n WriteInfo
static void update_sets_run(const void *p) {
    const UpdateArgs *a = p;
    VkWriteDescriptorSet *w = after(a, sizeof *a);
    const WriteInfo *inf = after(w, a->n * sizeof *w);
    for (uint32_t i = 0; i < a->n; i++) {
        w[i].dstSet = resolve_set(w[i].dstSet);
        if (write_is_image(&w[i])) w[i].pImageInfo = &inf[i].i;
        else w[i].pBufferInfo = &inf[i].b;
    }
    vkUpdateDescriptorSets(g_vk.dev, a->n, w, 0, NULL);
}
void rec_UpdateDescriptorSets(uint32_t n, const VkWriteDescriptorSet *w) {
    if (on <= 0) { vkUpdateDescriptorSets(g_vk.dev, n, w, 0, NULL); return; }
    ENQ_T0();
    UpdateArgs *a = push(update_sets_run, sizeof *a, AL16(n * sizeof *w) + n * sizeof(WriteInfo));
    a->n = n;
    VkWriteDescriptorSet *pw = after(a, sizeof *a);
    WriteInfo *inf = after(pw, n * sizeof *pw);
    for (uint32_t i = 0; i < n; i++) {
        if (w[i].descriptorCount != 1 || w[i].pNext) FATAL("[vk] recorder: a descriptor write of %u descriptors\n", w[i].descriptorCount);
        pw[i] = w[i];
        if (write_is_image(&w[i])) inf[i].i = *w[i].pImageInfo;
        else inf[i].b = *w[i].pBufferInfo;
        pw[i].pImageInfo = NULL; pw[i].pBufferInfo = NULL; pw[i].pTexelBufferView = NULL;  // (set again when run)
    }
    commit();
    ENQ_END();
}

// ================================================================== command buffers, submissions, presents
static void begin_cb_run(const void *p) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*(const VkCommandBuffer *)p, &bi));
}
void rec_BeginCommandBuffer(VkCommandBuffer cb) {
    if (on <= 0) { begin_cb_run(&cb); return; }
    ENQ_T0();
    *(VkCommandBuffer *)push(begin_cb_run, sizeof cb, 0) = cb;
    commit();
    ENQ_END();
}
static void end_cb_run(const void *p) { VK_CHECK(vkEndCommandBuffer(*(const VkCommandBuffer *)p)); }
void rec_EndCommandBuffer(VkCommandBuffer cb) {
    if (on <= 0) { end_cb_run(&cb); return; }
    ENQ_T0();
    *(VkCommandBuffer *)push(end_cb_run, sizeof cb, 0) = cb;
    commit();
    ENQ_END();
}

typedef struct {
    VkCommandBufferSubmitInfo cbs[2];
    VkSemaphoreSubmitInfo wait, sig[2];
    uint32_t ncb, nwait, nsig;
    uint64_t seq;
} SubmitArgs;
static void submit_run(const void *p) {
    const SubmitArgs *a = p;
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = a->ncb, .pCommandBufferInfos = a->cbs,
                        .signalSemaphoreInfoCount = a->nsig, .pSignalSemaphoreInfos = a->sig,
                        .waitSemaphoreInfoCount = a->nwait, .pWaitSemaphoreInfos = &a->wait};
    uint64_t t0 = NOW();
    rq_watch("vkQueueSubmit2", a->seq);  // (a hang dump names it)
    VkResult r = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    rq_watch(NULL, 0);
    uint64_t dt = NOW() - t0;
    if (prof) ADD(r_submit_ns, dt);
    if (dt > 50000000) {  // (a driver stall: a slow drain or wait on the recorder shows where it was)
        static int n;
        if (n++ < 20) LOG("[vk] vkQueueSubmit2 of submission %llu took %llu ms\n", (unsigned long long)a->seq, (unsigned long long)(dt / 1000000));
    }
    if (r == VK_ERROR_DEVICE_LOST) { extern void vk_devlost_dump(void); vk_devlost_dump(); }
    VK_CHECK(r);
    atomic_store_explicit(&submitted, a->seq, memory_order_release);
    if (on > 0) {
        pthread_mutex_lock(&mu);
        pthread_cond_broadcast(&cv_done);
        pthread_mutex_unlock(&mu);
    }
}
void rec_QueueSubmit2(const VkSubmitInfo2 *si, uint64_t seq) {
    SubmitArgs a = {.ncb = si->commandBufferInfoCount, .nwait = si->waitSemaphoreInfoCount, .nsig = si->signalSemaphoreInfoCount, .seq = seq};
    if (a.ncb > 2 || a.nwait > 1 || a.nsig > 2 || si->pNext) FATAL("[vk] recorder: a submission it can't carry\n");
    for (uint32_t i = 0; i < a.ncb; i++) a.cbs[i] = si->pCommandBufferInfos[i];
    if (a.nwait) a.wait = si->pWaitSemaphoreInfos[0];
    for (uint32_t i = 0; i < a.nsig; i++) a.sig[i] = si->pSignalSemaphoreInfos[i];
    if (on <= 0) { submit_run(&a); return; }
    ENQ_T0();
    *(SubmitArgs *)push(submit_run, sizeof a, 0) = a;
    publish();  // (whoever waits for the submission must not wait for the next batch)
    ENQ_END();
}

typedef struct { VkSemaphore wait; VkSwapchainKHR swapchain; uint32_t image; uint64_t id; } PresentArgs;
void vk_present_watch(uint64_t id);  // (device.c)
static void present_run(const void *p) {
    const PresentArgs *a = p;
    VkPresentIdKHR pid = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &a->id};
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = a->id ? &pid : NULL,
                           .waitSemaphoreCount = 1, .pWaitSemaphores = &a->wait,
                           .swapchainCount = 1, .pSwapchains = &a->swapchain, .pImageIndices = &a->image};
    uint64_t t0 = NOW();
    rq_watch("vkQueuePresentKHR", a->image);
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    rq_watch(NULL, 0);
    uint64_t dt = NOW() - t0;
    __atomic_fetch_add(&g_vk_frame_waits[2], dt, __ATOMIC_RELAXED);
    if (dt > 250000000) {  // (FIFO waits for the display, a few ms; this long is a stall in the driver or the compositor)
        static int n;
        if (n++ < 20) LOG("[vk] vkQueuePresentKHR took %llu ms\n", (unsigned long long)(dt / 1000000));
    }
    __atomic_fetch_add(&g_vk_frame_waits[3], 1, __ATOMIC_RELAXED);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) atomic_store_explicit(&present_recreate, 1, memory_order_release);
    else if (r == VK_SUCCESS) { if (a->id) vk_present_watch(a->id); }
    else {
        static int n;
        if (n++ < 10) LOG("[vk] present failed: %d\n", r);
        if (r == VK_ERROR_DEVICE_LOST) { vk_device_fault_report(); FATAL("vkQueuePresentKHR: device lost\n"); }
    }
    atomic_fetch_add_explicit(&presents_done, 1, memory_order_release);
    if (on > 0) {
        pthread_mutex_lock(&mu);
        pthread_cond_broadcast(&cv_done);
        pthread_mutex_unlock(&mu);
    }
}
void rec_QueuePresentKHR(VkSemaphore wait, VkSwapchainKHR swapchain, uint32_t image, uint64_t present_id) {
    PresentArgs a = {wait, swapchain, image, present_id};
    presents_queued++;
    if (on <= 0) {  // (the wait is this thread's: idle for the pacer, as before)
        uint64_t t0 = NOW();
        present_run(&a);
        g_blocked_ns += NOW() - t0;
        return;
    }
    ENQ_T0();
    *(PresentArgs *)push(present_run, sizeof a, 0) = a;
    publish();
    ENQ_END();
}
bool rec_present_recreate(void) { return atomic_exchange_explicit(&present_recreate, 0, memory_order_acq_rel) != 0; }

// ================================================================== commands
// Scalar arguments: a struct of the command buffer and the values, made by a macro from the parameter list.
#define CMD_ARGS(name, ...) typedef struct { VkCommandBuffer cb; __VA_ARGS__ } name##Args;
#define CMD_QUEUE(name, ...) \
    do { if (on <= 0) { vk##name(__VA_ARGS__); return; } ENQ_T0(); name##Args *a_ = push(name##_run, sizeof *a_, 0); \
         *a_ = (name##Args){__VA_ARGS__}; commit(); ENQ_END(); } while (0)

CMD_ARGS(CmdDispatch, uint32_t x, y, z;)
static void CmdDispatch_run(const void *p) { const CmdDispatchArgs *a = p; vkCmdDispatch(a->cb, a->x, a->y, a->z); }
void rec_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) { CMD_QUEUE(CmdDispatch, cb, x, y, z); }

CMD_ARGS(CmdBindIndexBuffer, VkBuffer buf; VkDeviceSize off; VkIndexType type;)
static void CmdBindIndexBuffer_run(const void *p) { const CmdBindIndexBufferArgs *a = p; vkCmdBindIndexBuffer(a->cb, a->buf, a->off, a->type); }
void rec_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type) { CMD_QUEUE(CmdBindIndexBuffer, cb, buf, off, type); }

CMD_ARGS(CmdEndRendering, char pad_;)
static void CmdEndRendering_run(const void *p) { vkCmdEndRendering(((const CmdEndRenderingArgs *)p)->cb); }
void rec_CmdEndRendering(VkCommandBuffer cb) {
    if (on <= 0) { vkCmdEndRendering(cb); return; }
    ENQ_T0();
    ((CmdEndRenderingArgs *)push(CmdEndRendering_run, sizeof(CmdEndRenderingArgs), 0))->cb = cb;
    commit();
    ENQ_END();
}

CMD_ARGS(CmdSetDepthBias, float c, k, s;)
static void CmdSetDepthBias_run(const void *p) { const CmdSetDepthBiasArgs *a = p; vkCmdSetDepthBias(a->cb, a->c, a->k, a->s); }
void rec_CmdSetDepthBias(VkCommandBuffer cb, float c, float k, float s) { CMD_QUEUE(CmdSetDepthBias, cb, c, k, s); }

#define CMD_BOOL(name) \
    CMD_ARGS(name, VkBool32 v;) \
    static void name##_run(const void *p) { const name##Args *a = p; vk##name(a->cb, a->v); } \
    void rec_##name(VkCommandBuffer cb, VkBool32 v) { CMD_QUEUE(name, cb, v); }
CMD_BOOL(CmdSetDepthBiasEnable)
CMD_BOOL(CmdSetDepthTestEnable)
CMD_BOOL(CmdSetDepthWriteEnable)
CMD_BOOL(CmdSetStencilTestEnable)
CMD_BOOL(CmdSetPrimitiveRestartEnable)

#define CMD_ENUM(name, T) \
    CMD_ARGS(name, T v;) \
    static void name##_run(const void *p) { const name##Args *a = p; vk##name(a->cb, a->v); } \
    void rec_##name(VkCommandBuffer cb, T v) { CMD_QUEUE(name, cb, v); }
CMD_ENUM(CmdSetDepthCompareOp, VkCompareOp)
CMD_ENUM(CmdSetCullMode, VkCullModeFlags)
CMD_ENUM(CmdSetFrontFace, VkFrontFace)
CMD_ENUM(CmdSetPrimitiveTopology, VkPrimitiveTopology)

#define CMD_FACE_U32(name) \
    CMD_ARGS(name, VkStencilFaceFlags face; uint32_t v;) \
    static void name##_run(const void *p) { const name##Args *a = p; vk##name(a->cb, a->face, a->v); } \
    void rec_##name(VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t v) { CMD_QUEUE(name, cb, face, v); }
CMD_FACE_U32(CmdSetStencilCompareMask)
CMD_FACE_U32(CmdSetStencilWriteMask)
CMD_FACE_U32(CmdSetStencilReference)

CMD_ARGS(CmdSetStencilOp, VkStencilFaceFlags face; VkStencilOp fail, pass, dfail; VkCompareOp cmp;)
static void CmdSetStencilOp_run(const void *p) {
    const CmdSetStencilOpArgs *a = p;
    vkCmdSetStencilOp(a->cb, a->face, a->fail, a->pass, a->dfail, a->cmp);
}
void rec_CmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags face, VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail, VkCompareOp cmp) {
    CMD_QUEUE(CmdSetStencilOp, cb, face, fail, pass, dfail, cmp);
}

CMD_ARGS(CmdResetQueryPool, VkQueryPool pool; uint32_t first, n;)
static void CmdResetQueryPool_run(const void *p) { const CmdResetQueryPoolArgs *a = p; vkCmdResetQueryPool(a->cb, a->pool, a->first, a->n); }
void rec_CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t n) { CMD_QUEUE(CmdResetQueryPool, cb, pool, first, n); }

CMD_ARGS(CmdWriteTimestamp2, VkPipelineStageFlags2 stage; VkQueryPool pool; uint32_t query;)
static void CmdWriteTimestamp2_run(const void *p) { const CmdWriteTimestamp2Args *a = p; vkCmdWriteTimestamp2(a->cb, a->stage, a->pool, a->query); }
void rec_CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    CMD_QUEUE(CmdWriteTimestamp2, cb, stage, pool, query);
}

CMD_ARGS(CmdSetBlendConstants, float c[4];)
static void CmdSetBlendConstants_run(const void *p) { const CmdSetBlendConstantsArgs *a = p; vkCmdSetBlendConstants(a->cb, a->c); }
void rec_CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (on <= 0) { vkCmdSetBlendConstants(cb, c); return; }
    ENQ_T0();
    CmdSetBlendConstantsArgs *a = push(CmdSetBlendConstants_run, sizeof *a, 0);
    a->cb = cb;
    memcpy(a->c, c, sizeof a->c);
    commit();
    ENQ_END();
}

// ---- pipelines and draws
// A bind through the resolver (exec.c's Pipe): with WAIT_PIPES a pipeline still building is waited for here, so the
// translating thread never skips a draw for it; one whose build failed binds nothing, and the draws until the next
// graphics bind are dropped (as the translating thread drops them when it skips).
static bool unbound;  // (R; T when the recorder is off: the same thread then)
CMD_ARGS(CmdBindPipeline, VkPipelineBindPoint bp; VkPipeline p; void *obj;)
static void CmdBindPipeline_run(const void *p) {
    const CmdBindPipelineArgs *a = p;
    VkPipeline pipe = a->p;
    if (a->obj) {
        pipe = pipe_resolve(a->obj, false);
        if (!pipe && rec_wait_pipes()) {
            uint64_t t0 = NOW();
            pipe = pipe_resolve(a->obj, true);
            ADD(r_pipe_waits, 1);
            ADD(r_pipe_wait_ns, NOW() - t0);
        }
    }
    if (a->bp == VK_PIPELINE_BIND_POINT_GRAPHICS) unbound = !pipe;
    if (pipe) vkCmdBindPipeline(a->cb, a->bp, pipe);
}
void rec_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    CmdBindPipelineArgs a = {cb, bp, p, NULL};
    if (on <= 0) { CmdBindPipeline_run(&a); return; }
    ENQ_T0();
    *(CmdBindPipelineArgs *)push(CmdBindPipeline_run, sizeof a, 0) = a;
    commit();
    ENQ_END();
}
void rec_CmdBindPipelineObj(VkCommandBuffer cb, VkPipelineBindPoint bp, void *obj) {
    CmdBindPipelineArgs a = {cb, bp, VK_NULL_HANDLE, obj};
    if (on <= 0) { CmdBindPipeline_run(&a); return; }
    ENQ_T0();
    *(CmdBindPipelineArgs *)push(CmdBindPipeline_run, sizeof a, 0) = a;
    commit();
    ENQ_END();
}
static bool draw_dropped(void) {
    if (!unbound) return false;
    if (!atomic_fetch_add_explicit(&r_dropped, 1, memory_order_relaxed)) LOG("[vk] recorder: a pipeline has no result yet or failed to build: its draws are dropped\n");
    return true;
}

CMD_ARGS(CmdDraw, uint32_t count, inst, first, first_inst;)
static void CmdDraw_run(const void *p) {
    const CmdDrawArgs *a = p;
    if (!draw_dropped()) vkCmdDraw(a->cb, a->count, a->inst, a->first, a->first_inst);
}
void rec_CmdDraw(VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, uint32_t first_inst) {
    CmdDrawArgs a = {cb, count, inst, first, first_inst};
    if (on <= 0) { CmdDraw_run(&a); return; }
    ENQ_T0();
    *(CmdDrawArgs *)push(CmdDraw_run, sizeof a, 0) = a;
    commit();
    ENQ_END();
}
// (Pokémon Z-A: draws whose parameters its culling compute wrote, read by the GPU from the guest heap - exec.c draw_)
CMD_ARGS(CmdDrawIndirect, VkBuffer buf; VkDeviceSize off; uint32_t n, stride; bool indexed;)
static void CmdDrawIndirect_run(const void *p) {
    const CmdDrawIndirectArgs *a = p;
    if (draw_dropped()) return;
    if (a->indexed) vkCmdDrawIndexedIndirect(a->cb, a->buf, a->off, a->n, a->stride);
    else vkCmdDrawIndirect(a->cb, a->buf, a->off, a->n, a->stride);
}
void rec_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t n, uint32_t stride, bool indexed) {
    CmdDrawIndirectArgs a = {cb, buf, off, n, stride, indexed};
    if (on <= 0) { CmdDrawIndirect_run(&a); return; }
    ENQ_T0();
    *(CmdDrawIndirectArgs *)push(CmdDrawIndirect_run, sizeof a, 0) = a;
    commit();
    ENQ_END();
}
CMD_ARGS(CmdDrawIndexed, uint32_t count, inst, first; int32_t base; uint32_t first_inst;)
static void CmdDrawIndexed_run(const void *p) {
    const CmdDrawIndexedArgs *a = p;
    if (!draw_dropped()) vkCmdDrawIndexed(a->cb, a->count, a->inst, a->first, a->base, a->first_inst);
}
void rec_CmdDrawIndexed(VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, int32_t base, uint32_t first_inst) {
    CmdDrawIndexedArgs a = {cb, count, inst, first, base, first_inst};
    if (on <= 0) { CmdDrawIndexed_run(&a); return; }
    ENQ_T0();
    *(CmdDrawIndexedArgs *)push(CmdDrawIndexed_run, sizeof a, 0) = a;
    commit();
    ENQ_END();
}

// ---- arrays: the packet's arguments, then the array(s) after them
CMD_ARGS(CmdBindVertexBuffers, uint32_t first, n;)  // then VkBuffer[n], VkDeviceSize[n]
static void CmdBindVertexBuffers_run(const void *p) {
    const CmdBindVertexBuffersArgs *a = p;
    const VkBuffer *b = after(a, sizeof *a);
    const VkDeviceSize *o = after(b, a->n * sizeof *b);
    vkCmdBindVertexBuffers(a->cb, a->first, a->n, b, o);
}
void rec_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs, const VkDeviceSize *offs) {
    if (on <= 0) { vkCmdBindVertexBuffers(cb, first, n, bufs, offs); return; }
    ENQ_T0();
    CmdBindVertexBuffersArgs *a = push(CmdBindVertexBuffers_run, sizeof *a, AL16(n * sizeof *bufs) + n * sizeof *offs);
    *a = (CmdBindVertexBuffersArgs){cb, first, n};
    VkBuffer *b = after(a, sizeof *a);
    memcpy(b, bufs, n * sizeof *bufs);
    memcpy(after(b, n * sizeof *b), offs, n * sizeof *offs);
    commit();
    ENQ_END();
}

CMD_ARGS(CmdBindDescriptorSets, VkPipelineBindPoint bp; VkPipelineLayout layout; uint32_t first, n, ndyn;)  // then sets[n], dyn[ndyn]
static void CmdBindDescriptorSets_run(const void *p) {
    const CmdBindDescriptorSetsArgs *a = p;
    VkDescriptorSet *s = after(a, sizeof *a);
    const uint32_t *d = after(s, a->n * sizeof *s);
    for (uint32_t i = 0; i < a->n; i++) s[i] = resolve_set(s[i]);
    vkCmdBindDescriptorSets(a->cb, a->bp, a->layout, a->first, a->n, s, a->ndyn, a->ndyn ? d : NULL);
}
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                               const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn) {
    if (on <= 0) { vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, ndyn, dyn); return; }
    ENQ_T0();
    CmdBindDescriptorSetsArgs *a = push(CmdBindDescriptorSets_run, sizeof *a, AL16(n * sizeof *sets) + ndyn * sizeof *dyn);
    *a = (CmdBindDescriptorSetsArgs){cb, bp, layout, first, n, ndyn};
    VkDescriptorSet *s = after(a, sizeof *a);
    memcpy(s, sets, n * sizeof *sets);
    if (ndyn) memcpy(after(s, n * sizeof *s), dyn, ndyn * sizeof *dyn);
    commit();
    ENQ_END();
}

CMD_ARGS(CmdPushConstants, VkPipelineLayout layout; VkShaderStageFlags stages; uint32_t off, size;)  // then the bytes
static void CmdPushConstants_run(const void *p) {
    const CmdPushConstantsArgs *a = p;
    vkCmdPushConstants(a->cb, a->layout, a->stages, a->off, a->size, after(a, sizeof *a));
}
void rec_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off, uint32_t size, const void *data) {
    if (on <= 0) { vkCmdPushConstants(cb, layout, stages, off, size, data); return; }
    ENQ_T0();
    CmdPushConstantsArgs *a = push(CmdPushConstants_run, sizeof *a, size);
    *a = (CmdPushConstantsArgs){cb, layout, stages, off, size};
    memcpy(after(a, sizeof *a), data, size);
    commit();
    ENQ_END();
}

// (device.c's overlay: secondary command buffers the presenting thread recorded and ended before queuing this; their pool
// is not touched again until the GPU finished the submission that executes them)
CMD_ARGS(CmdExecuteCommands, uint32_t n;)  // then VkCommandBuffer[n]
static void CmdExecuteCommands_run(const void *p) {
    const CmdExecuteCommandsArgs *a = p;
    vkCmdExecuteCommands(a->cb, a->n, after(a, sizeof *a));
}
void rec_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs) {
    if (on <= 0) { vkCmdExecuteCommands(cb, n, cbs); return; }
    ENQ_T0();
    CmdExecuteCommandsArgs *a = push(CmdExecuteCommands_run, sizeof *a, n * sizeof *cbs);
    *a = (CmdExecuteCommandsArgs){cb, n};
    memcpy(after(a, sizeof *a), cbs, n * sizeof *cbs);
    commit();
    ENQ_END();
}

CMD_ARGS(CmdSetViewport, uint32_t first, n;)  // then VkViewport[n]
static void CmdSetViewport_run(const void *p) { const CmdSetViewportArgs *a = p; vkCmdSetViewport(a->cb, a->first, a->n, after(a, sizeof *a)); }
void rec_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    if (on <= 0) { vkCmdSetViewport(cb, first, n, v); return; }
    ENQ_T0();
    CmdSetViewportArgs *a = push(CmdSetViewport_run, sizeof *a, n * sizeof *v);
    *a = (CmdSetViewportArgs){cb, first, n};
    memcpy(after(a, sizeof *a), v, n * sizeof *v);
    commit();
    ENQ_END();
}
CMD_ARGS(CmdSetScissor, uint32_t first, n;)  // then VkRect2D[n]
static void CmdSetScissor_run(const void *p) { const CmdSetScissorArgs *a = p; vkCmdSetScissor(a->cb, a->first, a->n, after(a, sizeof *a)); }
void rec_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r) {
    if (on <= 0) { vkCmdSetScissor(cb, first, n, r); return; }
    ENQ_T0();
    CmdSetScissorArgs *a = push(CmdSetScissor_run, sizeof *a, n * sizeof *r);
    *a = (CmdSetScissorArgs){cb, first, n};
    memcpy(after(a, sizeof *a), r, n * sizeof *r);
    commit();
    ENQ_END();
}

// (VK_EXT_vertex_input_dynamic_state, exec.c set_vertex_input: through g_vk.set_vertex_input; pNext must be NULL)
CMD_ARGS(CmdSetVertexInputEXT, uint32_t nb, na;)  // then VkVertexInputBindingDescription2EXT[nb], VkVertexInputAttributeDescription2EXT[na]
static void CmdSetVertexInputEXT_run(const void *p) {
    const CmdSetVertexInputEXTArgs *a = p;
    const VkVertexInputBindingDescription2EXT *b = after(a, sizeof *a);
    const VkVertexInputAttributeDescription2EXT *v = after(b, a->nb * sizeof *b);
    g_vk.set_vertex_input(a->cb, a->nb, b, a->na, v);
}
void rec_CmdSetVertexInputEXT(VkCommandBuffer cb, uint32_t nb, const VkVertexInputBindingDescription2EXT *b, uint32_t na,
                              const VkVertexInputAttributeDescription2EXT *v) {
    if (on <= 0) { g_vk.set_vertex_input(cb, nb, b, na, v); return; }
    ENQ_T0();
    CmdSetVertexInputEXTArgs *a = push(CmdSetVertexInputEXT_run, sizeof *a, AL16(nb * sizeof *b) + na * sizeof *v);
    *a = (CmdSetVertexInputEXTArgs){cb, nb, na};
    VkVertexInputBindingDescription2EXT *bd = after(a, sizeof *a);
    memcpy(bd, b, nb * sizeof *b);
    memcpy(after(bd, nb * sizeof *bd), v, na * sizeof *v);
    commit();
    ENQ_END();
}

CMD_ARGS(CmdClearAttachments, uint32_t na, nr;)  // then VkClearAttachment[na], VkClearRect[nr]
static void CmdClearAttachments_run(const void *p) {
    const CmdClearAttachmentsArgs *a = p;
    const VkClearAttachment *at = after(a, sizeof *a);
    vkCmdClearAttachments(a->cb, a->na, at, a->nr, after(at, a->na * sizeof *at));
}
void rec_CmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *at, uint32_t nr, const VkClearRect *r) {
    if (on <= 0) { vkCmdClearAttachments(cb, na, at, nr, r); return; }
    ENQ_T0();
    CmdClearAttachmentsArgs *a = push(CmdClearAttachments_run, sizeof *a, AL16(na * sizeof *at) + nr * sizeof *r);
    *a = (CmdClearAttachmentsArgs){cb, na, nr};
    VkClearAttachment *pa = after(a, sizeof *a);
    memcpy(pa, at, na * sizeof *at);
    memcpy(after(pa, na * sizeof *pa), r, nr * sizeof *r);
    commit();
    ENQ_END();
}

// Image clears, copies, blits, resolves: two handles, two layouts, a clear value, a count of regions and a filter;
// the regions follow.
typedef struct {
    VkCommandBuffer cb;
    uint64_t h1, h2;
    VkImageLayout l1, l2;
    uint32_t n;
    VkFilter filter;
    union { VkClearColorValue color; VkClearDepthStencilValue ds; } clear;
} XferArgs;
#define XFER_REGIONS(a) after((a), sizeof *(a))
static void copy_buffer_run(const void *p) { const XferArgs *a = p; vkCmdCopyBuffer(a->cb, (VkBuffer)a->h1, (VkBuffer)a->h2, a->n, XFER_REGIONS(a)); }
static void copy_image_run(const void *p) {
    const XferArgs *a = p;
    vkCmdCopyImage(a->cb, (VkImage)a->h1, a->l1, (VkImage)a->h2, a->l2, a->n, XFER_REGIONS(a));
}
static void blit_image_run(const void *p) {
    const XferArgs *a = p;
    vkCmdBlitImage(a->cb, (VkImage)a->h1, a->l1, (VkImage)a->h2, a->l2, a->n, XFER_REGIONS(a), a->filter);
}
static void resolve_image_run(const void *p) {
    const XferArgs *a = p;
    vkCmdResolveImage(a->cb, (VkImage)a->h1, a->l1, (VkImage)a->h2, a->l2, a->n, XFER_REGIONS(a));
}
static void buffer_to_image_run(const void *p) {
    const XferArgs *a = p;
    vkCmdCopyBufferToImage(a->cb, (VkBuffer)a->h1, (VkImage)a->h2, a->l2, a->n, XFER_REGIONS(a));
}
static void image_to_buffer_run(const void *p) {
    const XferArgs *a = p;
    vkCmdCopyImageToBuffer(a->cb, (VkImage)a->h1, a->l1, (VkBuffer)a->h2, a->n, XFER_REGIONS(a));
}
static void clear_color_run(const void *p) {
    const XferArgs *a = p;
    vkCmdClearColorImage(a->cb, (VkImage)a->h1, a->l1, &a->clear.color, a->n, XFER_REGIONS(a));
}
static void clear_ds_run(const void *p) {
    const XferArgs *a = p;
    vkCmdClearDepthStencilImage(a->cb, (VkImage)a->h1, a->l1, &a->clear.ds, a->n, XFER_REGIONS(a));
}
// queues fn with the arguments and n regions of `size` bytes each (the recorder on)
static void xfer(PktFn fn, const XferArgs *x, const void *regions, size_t size) {
    ENQ_T0();
    XferArgs *a = push(fn, sizeof *a, x->n * size);
    *a = *x;
    memcpy(XFER_REGIONS(a), regions, x->n * size);
    commit();
    ENQ_END();
}
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    if (on <= 0) { vkCmdCopyBuffer(cb, src, dst, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .n = n};
    xfer(copy_buffer_run, &x, r, sizeof *r);
}
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    if (on <= 0) { vkCmdCopyImage(cb, src, sl, dst, dl, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l1 = sl, .l2 = dl, .n = n};
    xfer(copy_image_run, &x, r, sizeof *r);
}
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    if (on <= 0) { vkCmdBlitImage(cb, src, sl, dst, dl, n, r, f); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l1 = sl, .l2 = dl, .n = n, .filter = f};
    xfer(blit_image_run, &x, r, sizeof *r);
}
void rec_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    if (on <= 0) { vkCmdResolveImage(cb, src, sl, dst, dl, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l1 = sl, .l2 = dl, .n = n};
    xfer(resolve_image_run, &x, r, sizeof *r);
}
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *r) {
    if (on <= 0) { vkCmdCopyBufferToImage(cb, src, dst, dl, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l2 = dl, .n = n};
    xfer(buffer_to_image_run, &x, r, sizeof *r);
}
void rec_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) {
    if (on <= 0) { vkCmdCopyImageToBuffer(cb, src, sl, dst, n, r); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)src, .h2 = (uint64_t)dst, .l1 = sl, .n = n};
    xfer(image_to_buffer_run, &x, r, sizeof *r);
}
void rec_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *rg) {
    if (on <= 0) { vkCmdClearColorImage(cb, img, l, c, n, rg); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)img, .l1 = l, .n = n, .clear.color = *c};
    xfer(clear_color_run, &x, rg, sizeof *rg);
}
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n,
                                   const VkImageSubresourceRange *rg) {
    if (on <= 0) { vkCmdClearDepthStencilImage(cb, img, l, c, n, rg); return; }
    XferArgs x = {.cb = cb, .h1 = (uint64_t)img, .l1 = l, .n = n, .clear.ds = *c};
    xfer(clear_ds_run, &x, rg, sizeof *rg);
}

// ---- barriers: the dependency with its three barrier arrays after it (nothing chained: pNext must be NULL)
CMD_ARGS(CmdPipelineBarrier2, VkDependencyInfo di;)  // then memory[], buffer[], image[] barriers
static void CmdPipelineBarrier2_run(const void *p) {
    const CmdPipelineBarrier2Args *a = p;
    VkDependencyInfo di = a->di;
    const VkMemoryBarrier2 *m = after(a, sizeof *a);
    const VkBufferMemoryBarrier2 *b = after(m, di.memoryBarrierCount * sizeof *m);
    const VkImageMemoryBarrier2 *im = after(b, di.bufferMemoryBarrierCount * sizeof *b);
    di.pMemoryBarriers = di.memoryBarrierCount ? m : NULL;
    di.pBufferMemoryBarriers = di.bufferMemoryBarrierCount ? b : NULL;
    di.pImageMemoryBarriers = di.imageMemoryBarrierCount ? im : NULL;
    vkCmdPipelineBarrier2(a->cb, &di);
}
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    if (on <= 0) { vkCmdPipelineBarrier2(cb, di); return; }
    if (di->pNext) FATAL("[vk] recorder: a chained dependency\n");
    for (uint32_t i = 0; i < di->memoryBarrierCount; i++) if (di->pMemoryBarriers[i].pNext) FATAL("[vk] recorder: a chained barrier\n");
    for (uint32_t i = 0; i < di->bufferMemoryBarrierCount; i++) if (di->pBufferMemoryBarriers[i].pNext) FATAL("[vk] recorder: a chained barrier\n");
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) if (di->pImageMemoryBarriers[i].pNext) FATAL("[vk] recorder: a chained barrier\n");
    ENQ_T0();
    size_t sm = di->memoryBarrierCount * sizeof(VkMemoryBarrier2), sb = di->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2);
    size_t si = di->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2);
    CmdPipelineBarrier2Args *a = push(CmdPipelineBarrier2_run, sizeof *a, AL16(sm) + AL16(sb) + si);
    a->cb = cb;
    a->di = *di;
    a->di.pMemoryBarriers = NULL; a->di.pBufferMemoryBarriers = NULL; a->di.pImageMemoryBarriers = NULL;
    uint8_t *m = after(a, sizeof *a);
    if (sm) memcpy(m, di->pMemoryBarriers, sm);
    uint8_t *b = after(m, sm);
    if (sb) memcpy(b, di->pBufferMemoryBarriers, sb);
    if (si) memcpy(after(b, sb), di->pImageMemoryBarriers, si);
    commit();
    ENQ_END();
}

// ---- rendering: the info, its colour attachments, then the depth and stencil ones (nothing chained)
CMD_ARGS(CmdBeginRendering, VkRenderingInfo ri; VkRenderingAttachmentInfo depth, stencil; uint8_t has_depth, has_stencil;)  // then colour[]
static void CmdBeginRendering_run(const void *p) {
    const CmdBeginRenderingArgs *a = p;
    VkRenderingInfo ri = a->ri;
    ri.pColorAttachments = ri.colorAttachmentCount ? after(a, sizeof *a) : NULL;
    ri.pDepthAttachment = a->has_depth ? &a->depth : NULL;
    ri.pStencilAttachment = a->has_stencil ? &a->stencil : NULL;
    vkCmdBeginRendering(a->cb, &ri);
}
void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    if (on <= 0) { vkCmdBeginRendering(cb, ri); return; }
    if (ri->pNext) FATAL("[vk] recorder: a chained rendering info\n");
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) if (ri->pColorAttachments[i].pNext) FATAL("[vk] recorder: a chained attachment\n");
    if ((ri->pDepthAttachment && ri->pDepthAttachment->pNext) || (ri->pStencilAttachment && ri->pStencilAttachment->pNext))
        FATAL("[vk] recorder: a chained attachment\n");
    ENQ_T0();
    size_t sc = ri->colorAttachmentCount * sizeof(VkRenderingAttachmentInfo);
    CmdBeginRenderingArgs *a = push(CmdBeginRendering_run, sizeof *a, sc);
    a->cb = cb;
    a->ri = *ri;
    a->ri.pColorAttachments = NULL; a->ri.pDepthAttachment = NULL; a->ri.pStencilAttachment = NULL;
    a->has_depth = ri->pDepthAttachment != NULL;
    a->has_stencil = ri->pStencilAttachment != NULL;
    if (a->has_depth) a->depth = *ri->pDepthAttachment;
    if (a->has_stencil) a->stencil = *ri->pStencilAttachment;
    if (sc) memcpy(after(a, sizeof *a), ri->pColorAttachments, sc);
    commit();
    ENQ_END();
}
