// A small Horizon kernel: handles, threads, synchronization, memory, and the SVC dispatcher.
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hle.h"
#include "kernel.h"
#ifdef _WIN32
#include "platform/windows_threads.h"
#endif

// ------------------------------------------------------------------ results
#undef R_OK
#define R_OK 0
#define R_TIMEOUT 0xEA01
#define R_CANCELLED 0xEC01
#define R_INVALID_HANDLE 0xE401
#define R_INVALID_STATE 0xFA01
#define R_INVALID_ADDRESS 0xCC01
#define R_NOT_FOUND 0xF201
#define R_INVALID_ENUM 0xF001
#define R_OUT_OF_RESOURCE 0xD001
#define R_LIMIT_REACHED 0x10801
#define R_INVALID_CURRENT_MEMORY 0xD401

#define PSEUDO_THREAD 0xFFFF8000u
#define PSEUDO_PROCESS 0xFFFF8001u
#define WAITERS_BIT 0x40000000u

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static __thread KThread *t_cur;

// ------------------------------------------------------------------ handle table
#define MAX_HANDLES 4096
static KObject *handles[MAX_HANDLES];

static uint32_t handle_alloc(KObject *o) {
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (!handles[i]) {
            handles[i] = o;
            o->refs++;
            return 0x100 + i;
        }
    }
    FATAL("out of handles\n");
}

KObject *handle_get(uint32_t h) {
    if (h == PSEUDO_THREAD) return &t_cur->obj;
    if (h < 0x101 || h >= 0x100 + MAX_HANDLES) return NULL;
    return handles[h - 0x100];
}

uint32_t kernel_new_handle(KObject *o) {
    pthread_mutex_lock(&g_klock);
    uint32_t h = handle_alloc(o);
    pthread_mutex_unlock(&g_klock);
    return h;
}

static void handle_close(uint32_t h) {
    KObject *o = handle_get(h);
    if (!o || h == PSEUDO_THREAD) return;
    handles[h - 0x100] = NULL;
    o->refs--;
}

// Threads blocked in WaitSynchronization; object signals wake only these (caller holds g_klock).
static KThread *g_syncwaiters;

static void wake(KThread *t) { pthread_cond_signal(&t->cond); }

void kernel_signal(KObject *o) {
    // caller holds g_klock
    o->signaled = true;
    for (KThread *t = g_syncwaiters; t; t = t->snext) wake(t);
}

// ------------------------------------------------------------------ time

uint64_t rt_ticks(void) {
    // 19.2 MHz system tick
    uint64_t ns = plat_now_ns();
    static double scale = -1;
    if (scale < 0) scale = getenv("LARECOMP_TICK_SCALE") ? atof(getenv("LARECOMP_TICK_SCALE")) : 1.0;
    if (scale != 1.0) ns = (uint64_t)(ns * scale);
    return (uint64_t)((__uint128_t)ns * 192 / 10000);
}

static void deadline(struct timespec *ts, int64_t timeout_ns) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ns / 1000000000;
    ts->tv_nsec += timeout_ns % 1000000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

// Sleep on the current thread's condition. Returns false on timeout. Caller holds g_klock.
static pthread_cond_t g_nothread_cond = PTHREAD_COND_INITIALIZER;
// Time this thread spent blocked in the kernel (waits, sleeps): idle, not work. Frame pacing (nvn/pace.c)
// takes the presenting thread's: a frame's busy time is its period minus this.
__thread uint64_t g_blocked_ns;

static bool kwait(int64_t timeout_ns, const struct timespec *ts) {
    pthread_cond_t *cv = t_cur ? &t_cur->cond : &g_nothread_cond;
    uint64_t t0 = plat_now_ns();
    bool r = true;
    if (timeout_ns < 0) pthread_cond_wait(cv, &g_klock);
    else r = pthread_cond_timedwait(cv, &g_klock, ts) == 0;
    g_blocked_ns += plat_now_ns() - t0;
    return r;
}

// ------------------------------------------------------------------ memory map
typedef struct { uint64_t addr, size; uint32_t type, perm; } Region;
static Region regions[1024];
static int nregions;
// The table's lock: the game maps memory and asks what is mapped from several threads at once (an insert's memmove
// under a concurrent one lost regions; a QueryMemory during it read a wrong answer). Its own lock, not g_klock: a
// shared memory's on_map callback may take that one.
static pthread_mutex_t g_mlock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t heap_size;
static uint64_t tls_next = TLS_BASE;
uint64_t kernel_heap_size(void) { return heap_size; }  // bytes from HEAP_BASE set by SetHeapSize
// The largest physical memory region (svcMapPhysicalMemory): where a game on the SDK's virtual address memory keeps
// what others keep in their heap. Its size (0: none), *addr its start.
uint64_t kernel_phys_largest(uint64_t *addr) {
    uint64_t best = 0;
    pthread_mutex_lock(&g_mlock);
    for (int i = 0; i < nregions; i++)
        if (regions[i].type == 7 && regions[i].size > best) { best = regions[i].size; *addr = regions[i].addr; }
    pthread_mutex_unlock(&g_mlock);
    return best;
}
// Writable guest data regions (heap, mapped/physical memory, module .data): for debug/runtime memory scans.
int kernel_data_regions(uint64_t *addr, uint64_t *size, int max) {
    int n = 0;
    pthread_mutex_lock(&g_mlock);
    for (int i = 0; i < nregions && n < max; i++)
        if ((regions[i].perm & 2) && regions[i].type != 0xB && regions[i].type != 0xC) {
            addr[n] = regions[i].addr; size[n] = regions[i].size; n++;
        }
    pthread_mutex_unlock(&g_mlock);
    return n;
}

static void region_add(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    if (nregions == 1024) FATAL("too many regions\n");
    plat_commit(addr, size);  // back it before the OS writes there (Windows: file reads into the heap)
    int i = nregions++;
    while (i > 0 && regions[i - 1].addr > addr) { regions[i] = regions[i - 1]; i--; }
    regions[i] = (Region){addr, size, type, perm};
}

// The SDK picks a random free address, then maps there: another thread can take it in between. The console refuses the
// second one (InvalidCurrentMemory) and the SDK picks again; taken as it came, two stacks, or a stack and hid's shared
// memory, shared their pages: the odd start that stopped in nn::hid::GetNpadJoyAssignment over a value only a stack
// could have put there. (g_mlock held)
static bool region_taken(uint64_t addr, uint64_t size, const char *what) {
    for (int i = 0; i < nregions; i++)
        if (regions[i].addr < addr + size && addr < regions[i].addr + regions[i].size) {
            static int nlog;
            if (nlog++ < 20)
                LOG("[kernel] %s %llx+%llx refused: taken by %llx+%llx (type %x)\n", what, (unsigned long long)addr,
                    (unsigned long long)size, (unsigned long long)regions[i].addr, (unsigned long long)regions[i].size, regions[i].type);
            return true;
        }
    return false;
}

static void region_remove(uint64_t addr) {
    for (int i = 0; i < nregions; i++) {
        if (regions[i].addr == addr) {
            memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof(Region));
            nregions--;
            return;
        }
    }
}

// Physical memory (svcMapPhysicalMemory, the SDK's virtual address memory): a range may cover pages mapped before,
// which keep their contents - only the gaps are new (zero: the window's pages are fresh until unmapped). Adjacent
// ranges merge into one region (the SDK maps many small ones).
// Application memory (GetInfo 6 / 21) and what the process uses of it (GetInfo 7 / 22): the heap, physical memory
// (svcMapPhysicalMemory) and ~64 MB for the code, stacks and kernel objects. As on hardware, a physical mapping
// beyond it fails with LimitReached - Pokémon Z-A maps all the free memory as one pool, then keeps asking for 4 MB
// pieces until refused. Off by default (LARECOMP_PHYS_LIMIT=1): with the limit the game keeps fewer characters / props
// loaded than on Ryujinx with its own (smaller) baseline - the 64 MB estimate is not what the hardware counts.
#define APP_MEMORY 0xCD500000ull
static uint64_t phys_mapped;
static int phys_limit = -1;  // LARECOMP_PHYS_LIMIT=1: count physical memory as used and refuse mappings past APP_MEMORY
static uint64_t used_memory(void) {
    if (phys_limit < 0) phys_limit = getenv("LARECOMP_PHYS_LIMIT") != NULL;
    return heap_size + (phys_limit ? phys_mapped : 0) + 0x4000000ull;
}

// bytes of [addr, addr + size) not mapped yet (physical memory regions)
static uint64_t phys_new_bytes(uint64_t addr, uint64_t size) {
    uint64_t end = addr + size, cur = addr, n = 0;
    for (int i = 0; i < nregions && cur < end; i++) {
        Region *r = &regions[i];
        if (r->type != 7 || r->addr + r->size <= cur || r->addr >= end) continue;
        if (r->addr > cur) n += r->addr - cur;
        cur = r->addr + r->size;
    }
    if (cur < end) n += end - cur;
    return n;
}

static void phys_map(uint64_t addr, uint64_t size) {
    phys_mapped += phys_new_bytes(addr, size);
    uint64_t end = addr + size, cur = addr, gaps[64][2];
    int ngaps = 0;
    for (int i = 0; i < nregions && cur < end; i++) {
        Region *r = &regions[i];
        if (r->type != 7 || r->addr + r->size <= cur || r->addr >= end) continue;
        if (r->addr > cur && ngaps < 64) { gaps[ngaps][0] = cur; gaps[ngaps][1] = r->addr - cur; ngaps++; }
        cur = r->addr + r->size;
    }
    if (cur < end && ngaps < 64) { gaps[ngaps][0] = cur; gaps[ngaps][1] = end - cur; ngaps++; }
    for (int i = 0; i < ngaps; i++) region_add(gaps[i][0], gaps[i][1], 7, 3);
    for (int i = 0; i + 1 < nregions;) {
        Region *a = &regions[i], *b = &regions[i + 1];
        if (a->type == 7 && b->type == 7 && a->addr + a->size == b->addr) {
            a->size += b->size;
            memmove(b, b + 1, (nregions - i - 2) * sizeof(Region));
            nregions--;
        } else i++;
    }
}

// Unmapped pages are released and read as zero when mapped again; regions are trimmed or split.
static void phys_unmap(uint64_t addr, uint64_t size) {
    uint64_t end = addr + size;
    uint64_t was = size - phys_new_bytes(addr, size);  // (the mapped part of the range)
    phys_mapped = was > phys_mapped ? 0 : phys_mapped - was;
    for (int i = 0; i < nregions; i++) {
        Region r = regions[i];
        if (r.type != 7 || r.addr + r.size <= addr || r.addr >= end) continue;
        region_remove(r.addr);
        if (r.addr < addr) region_add(r.addr, addr - r.addr, 7, 3);
        if (r.addr + r.size > end) region_add(end, r.addr + r.size - end, 7, 3);
        i = -1;  // (the array changed: rescan; trimmed pieces no longer overlap)
    }
#ifdef __linux__
    plat_reset_guest(addr, end);
#else
    memset((void *)(uintptr_t)addr, 0, size);
#endif
}

static void query_memory(uint64_t addr, uint64_t out) {
    uint64_t prev_end = 0;
    uint64_t info[5] = {0};  // addr, size, type|attr<<32, perm|ipc<<32, device|pad<<32
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        if (addr >= r->addr && addr < r->addr + r->size) {
            info[0] = r->addr;
            info[1] = r->size;
            info[2] = r->type;
            info[3] = r->perm;
            memcpy((void *)out, info, 0x28);
            return;
        }
        if (addr < r->addr) {
            info[0] = prev_end;
            info[1] = r->addr - prev_end;
            memcpy((void *)out, info, 0x28);
            return;
        }
        prev_end = r->addr + r->size;
    }
    info[0] = prev_end;
    info[1] = (1ull << 39) - prev_end;
    memcpy((void *)out, info, 0x28);
}

// ------------------------------------------------------------------ threads
static uint64_t next_thread_id = 0x50;

// ------------------------------------------------------------------ in-place reboot
// Recompiled guest code can't be killed from outside, so a reboot (runtime/main.c) sets g_rt_unwind, wakes every
// waiting thread, and each guest thread longjmps back to thread_main at its next safe point: rt_svc (entry and
// exit), an HLE call (runtime/dispatch.c), or a kernel wait (which returns early while g_rt_unwind is set). The
// frames dropped are recompiled code (its state lives in guest memory, which is wiped next) and HLE functions;
// none of those hold a host lock across a call into the guest, so nothing stays locked. svcExitThread ends a
// thread the same way.
volatile int g_rt_unwind;
static KThread *g_threads;  // every thread of this boot (g_klock)

void rt_unwind_point(void) {
    if (__builtin_expect(g_rt_unwind, 0) && t_cur) longjmp(t_cur->unwind, 1);
}

static void *thread_main(void *arg) {
    KThread *t = arg;
    t_cur = t;
    plat_thread_guest(t->prio);
    if (t->is_main) plat_thread_perf_core(PLAT_PERF_GAME);  // (the game's frame runs here)
    crash_handler_install();
    rt_set_fpcr(0);
    if (!setjmp(t->unwind)) guest_call(&t->cpu, t->entry);
    pthread_mutex_lock(&g_klock);
    t->finished = true;
    kernel_signal(&t->obj);
    pthread_mutex_unlock(&g_klock);
    t_cur = NULL;
    return NULL;
}

static KThread *thread_new(uint64_t entry, uint64_t arg, uint64_t stack_top, int prio, int core) {
    KThread *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cond, NULL);
    t->obj.type = KOBJ_THREAD;
    t->entry = entry;
    t->prio = prio;
    t->core = core < 0 ? 0 : core;
    t->cpu.x[0] = arg;
    t->cpu.sp = stack_top & ~15ull;
    t->cpu.excl_addr = ~0ull;
    t->cpu.thread = t;
    pthread_mutex_lock(&g_klock);  // (threads create threads at once: two must never get the same id / TLS)
    t->id = next_thread_id++;
    t->tlr = tls_next;
    tls_next += 0x200;
    t->cpu.tpidrro = t->tlr;
    t->anext = g_threads;
    g_threads = t;
    pthread_mutex_unlock(&g_klock);
    return t;
}

static void thread_start(KThread *t) {
#ifdef _WIN32
    t->host = guest_thread_create(thread_main, t, 256u << 20);
    if (!t->host) FATAL("Could not create a game thread.\n");
    t->started = true;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256ull << 20);  // guest recursion runs on the host stack
    t->started = true;
    pthread_create(&t->host, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
#endif
}

KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack_size, int prio) {
    uint64_t stack = STACK_BASE + STACK_SIZE - ((stack_size + 0xFFFF) & ~0xFFFFull);
    region_add(stack, stack_size, 0xB, 3);
    KThread *t = thread_new(entry, 0, stack + stack_size, prio, 0);
    t->is_main = true;
    t->handle = kernel_new_handle(&t->obj);
    return t;
}

static KThread *g_main_thread;  // (g_klock) for hang_watch
static void *hang_watch(void *arg);

void kernel_run_main(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4) {
    t->cpu.x[0] = x0; t->cpu.x[1] = x1; t->cpu.x[2] = x2; t->cpu.x[3] = x3; t->cpu.x[4] = x4;
    thread_start(t);  // the host main thread goes on to run the window event loop
    pthread_mutex_lock(&g_klock);
    g_main_thread = t;
    pthread_mutex_unlock(&g_klock);
    static bool watching;
    const char *e = getenv("LARECOMP_HANG_DUMP");
    uint64_t s = e ? strtoull(e, NULL, 10) : 30;
    if (!watching && s) {
        watching = true;
        pthread_t w;
        if (!pthread_create(&w, NULL, hang_watch, (void *)(uintptr_t)s)) pthread_detach(w);
    }
}

int kernel_live_threads(void) {  // (the "[mem]" log line: guest threads started and not finished)
    int n = 0;
    pthread_mutex_lock(&g_klock);
    for (KThread *t = g_threads; t; t = t->anext) n += t->started && !t->finished;
    pthread_mutex_unlock(&g_klock);
    return n;
}

uint32_t kernel_current_thread_handle(void) { return t_cur ? t_cur->handle : 0; }
Cpu *kernel_current_cpu(void) { return t_cur ? &t_cur->cpu : NULL; }
KThread *kernel_current_thread(void) { return t_cur; }
uint64_t kernel_current_thread_id(void) { return t_cur ? t_cur->id : 0; }

// ------------------------------------------------------------------ mutex / condvar (Horizon semantics)
// Mutex word: owner handle | WAITERS_BIT. Waiting threads sleep on the global cond and are handed ownership.

static uint32_t *u32p(uint64_t a) { return (uint32_t *)(uintptr_t)a; }

// Try to take mutex `m` for `tag`, or mark it contended. Returns true if acquired. Uses CAS so a
// concurrent user-space unlock (owner storing 0) is never overwritten.
static bool mutex_acquire_or_mark(uint64_t m, uint32_t tag) {
    for (;;) {
        uint32_t v = __atomic_load_n(u32p(m), __ATOMIC_SEQ_CST);
        if (v == 0) {
            if (__atomic_compare_exchange_n(u32p(m), &v, tag, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
        } else {
            if ((v & WAITERS_BIT) ||
                __atomic_compare_exchange_n(u32p(m), &v, v | WAITERS_BIT, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                return false;
        }
    }
}

// Pick a thread waiting on mutex `addr` and give it ownership. Caller holds g_klock.
static void mutex_release(uint64_t addr) {
    KThread *next = NULL;
    int count = 0;
    for (KThread *t = g_waitlist; t; t = t->wnext) {
        if (t->wait_kind == WAIT_MUTEX && t->wait_addr == addr) {
            if (!next || t->prio < next->prio) next = t;
            count++;
        }
    }
    if (!next) {
        __atomic_store_n(u32p(addr), 0, __ATOMIC_SEQ_CST);
        return;
    }
    __atomic_store_n(u32p(addr), next->handle | (count > 1 ? WAITERS_BIT : 0), __ATOMIC_SEQ_CST);
    next->wait_kind = WAIT_NONE;
    next->wait_result = R_OK;
    wake(next);
}

KThread *g_waitlist;
uint64_t g_program_id = 0x01006BB00C6F0000ull;
int g_game_is_la = 1;
uint64_t g_system_resource_size;
static uint64_t g_user_exception_ctx;  // GetInfo 20 (in the HLE heap: gone with it on a reboot)
RT_ON_RESET(user_exception_ctx_reset) { g_user_exception_ctx = 0; }

// At the end: the wakers take the most urgent waiter and, among equals, the first found - the longest waiting, as
// Horizon (first in, first out within a priority). Added at the head, the latest waiter won every tie.
static void waitlist_add(KThread *t) {
    KThread **p = &g_waitlist;
    while (*p) p = &(*p)->wnext;
    t->wnext = NULL;
    *p = t;
}
static void waitlist_remove(KThread *t) {
    for (KThread **p = &g_waitlist; *p; p = &(*p)->wnext)
        if (*p == t) { *p = t->wnext; return; }
}

// Block until wait_kind is cleared by a waker, timeout, or a reboot. Caller holds g_klock.
// CancelSynchronization doesn't end these waits (mutex, condition variable, address arbiter), as on Horizon: its flag
// is for the thread's next WaitSynchronization only. Ending them used the flag up there (a cancel meant to wake a
// thread out of a WaitSynchronization could be lost), and the SDK's untimed condition variable wait treats any
// result but success as fatal.
static uint32_t block_current(int64_t timeout_ns) {
    KThread *t = t_cur;
    struct timespec ts;
    if (timeout_ns >= 0) deadline(&ts, timeout_ns);
    t->wait_since = plat_now_ns();
    waitlist_add(t);
    while (t->wait_kind != WAIT_NONE) {
        if (g_rt_unwind) {  // reboot: give up the wait (the thread unwinds when its svc returns)
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_CANCELLED;
            break;
        }
        if (!kwait(timeout_ns, &ts)) {
            if (t->wait_kind == WAIT_NONE) break;  // (woken just as the time ran out: the waker's result stands)
            t->wait_kind = WAIT_NONE;
            t->wait_result = R_TIMEOUT;
            break;
        }
    }
    waitlist_remove(t);
    return t->wait_result;
}

// (diagnostics) the last KTRACE_N condition variable (untimed waits) / mutex kernel calls, lock-free, for the [hang] dump: what each
// thread did with which key / mutex word, and the words before and after. LARECOMP_KTRACE=0 turns it off.
enum { KT_WAIT, KT_WAIT_END, KT_SIGNAL, KT_LOCK, KT_LOCK_END, KT_UNLOCK };
#define KTRACE_N 256
typedef struct { uint64_t ns, tid, addr, addr2; uint32_t w0, w1, w2, arg, kind; } KTrace;
static KTrace ktrace[KTRACE_N];
static uint64_t ktrace_n;  // (__atomic builtins)
static int ktrace_on = -1;
static void kt(uint32_t kind, uint64_t addr, uint64_t addr2, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t arg) {
    if (ktrace_on < 0) ktrace_on = !getenv("LARECOMP_KTRACE") || atoi(getenv("LARECOMP_KTRACE")) != 0;
    if (!ktrace_on) return;
    KTrace *e = &ktrace[__atomic_fetch_add(&ktrace_n, 1, __ATOMIC_RELAXED) % KTRACE_N];
    *e = (KTrace){plat_now_ns(), t_cur ? t_cur->id : 0, addr, addr2, w0, w1, w2, arg, kind};
}
static uint32_t rdw(uint64_t a) { return a ? __atomic_load_n(u32p(a), __ATOMIC_SEQ_CST) : 0; }
// Keys used with timed waits (periodic pollers, e.g. a 1 ms timer thread) would fill the ring within a second: their
// waits and signals are left out (g_klock).
static uint64_t kt_timed[16];
static bool kt_timed_key(uint64_t key, bool add) {
    for (int i = 0; i < 16; i++) {
        if (kt_timed[i] == key) return true;
        if (!kt_timed[i] && add) { kt_timed[i] = key; return true; }
    }
    return false;
}

static uint32_t svc_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag) {
    pthread_mutex_lock(&g_klock);
    uint32_t v = __atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    kt(KT_LOCK, addr, 0, v, 0, owner, tag);
    if (v != (owner | WAITERS_BIT)) {
        pthread_mutex_unlock(&g_klock);
        return R_OK;
    }
    KThread *t = t_cur;
    t->wait_kind = WAIT_MUTEX;
    t->wait_addr = addr;
    t->wait_tag = tag;
    uint32_t r = block_current(-1);
    kt(KT_LOCK_END, addr, 0, rdw(addr), 0, 0, r);
    pthread_mutex_unlock(&g_klock);
    return r == R_CANCELLED ? R_OK : r;
}

static uint32_t svc_arbitrate_unlock(uint64_t addr) {
    pthread_mutex_lock(&g_klock);
    uint32_t before = rdw(addr);
    mutex_release(addr);
    kt(KT_UNLOCK, addr, 0, before, rdw(addr), 0, 0);
    pthread_mutex_unlock(&g_klock);
    return R_OK;
}

static uint32_t svc_wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout) {
    static long late_us = -1;  // (test) LARECOMP_DEBUG_CV_LATE_WAIT=<us>: every condition variable wait starts that late
    if (late_us < 0) late_us = getenv("LARECOMP_DEBUG_CV_LATE_WAIT") ? atol(getenv("LARECOMP_DEBUG_CV_LATE_WAIT")) : 0;
    if (late_us > 0) usleep((useconds_t)late_us);  // (forces notify-before-wait races in the game's code: hle/delegate_null.c)
    pthread_mutex_lock(&g_klock);
    KThread *t = t_cur;
    uint32_t mbefore = rdw(mutex), kbefore = rdw(key);
    // The key first, then the mutex (as the kernel does): nn::os Signal / Broadcast skip the svc while the key is 0,
    // and once the mutex is free another core can take it, change the state and signal before we sleep
    __atomic_store_n(u32p(key), 1, __ATOMIC_SEQ_CST);
    mutex_release(mutex);
    bool traced = !kt_timed_key(key, timeout >= 0);
    if (traced) kt(KT_WAIT, key, mutex, kbefore, mbefore, rdw(mutex), 0xffffffffu);
    t->wait_kind = WAIT_CONDVAR;
    t->wait_addr = key;
    t->wait_mutex = mutex;
    t->wait_tag = tag;
    uint32_t r = block_current(timeout);
    if (r != R_OK && t->wait_addr == mutex) {
        // signaled (moved to the mutex's waiters) before the timeout: the kernel hands it the mutex as usual
        r = R_OK;
        if ((LD32(mutex) & ~WAITERS_BIT) != tag && !mutex_acquire_or_mark(mutex, tag)) {
            t->wait_kind = WAIT_MUTEX;
            block_current(-1);
        }
    }
    // A timeout (or cancel) still waiting on the key returns without the mutex: as on hardware, the SDK takes it
    // again itself (nn::os TimedWait calls Enter on 0xea01 - reacquiring here deadlocked it on its own mutex: issue
    // #19's thread dump, a controller thread's timer-event wait)
    if (traced) kt(KT_WAIT_END, key, mutex, rdw(key), rdw(mutex), 0, r);
    pthread_mutex_unlock(&g_klock);
    return r;
}

static void svc_signal_process_wide_key(uint64_t key, int32_t count) {
    pthread_mutex_lock(&g_klock);
    uint32_t kbefore = rdw(key);
    int woken = 0;
    bool any_left = false;
    for (;;) {
        if (count > 0 && woken >= count) break;
        KThread *best = NULL;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key && (!best || t->prio < best->prio)) best = t;
        if (!best) break;
        woken++;
        // the woken thread must reacquire its mutex
        uint64_t m = best->wait_mutex;
        best->wait_result = R_OK;
        if (mutex_acquire_or_mark(m, best->wait_tag)) {
            best->wait_kind = WAIT_NONE;
            wake(best);
        } else {
            best->wait_kind = WAIT_MUTEX;
            best->wait_addr = m;
        }
    }
    for (KThread *t = g_waitlist; t; t = t->wnext)
        if (t->wait_kind == WAIT_CONDVAR && t->wait_addr == key) any_left = true;
    if (!any_left) __atomic_store_n(u32p(key), 0, __ATOMIC_SEQ_CST);
    if (!kt_timed_key(key, false)) kt(KT_SIGNAL, key, 0, kbefore, rdw(key), (uint32_t)woken, (uint32_t)count);
    pthread_mutex_unlock(&g_klock);
}

// ------------------------------------------------------------------ address arbiter
enum { ARB_WAIT_LESS = 0, ARB_DEC_WAIT_LESS = 1, ARB_WAIT_EQ = 2 };
enum { ARB_SIGNAL = 0, ARB_SIGNAL_INC_EQ = 1, ARB_SIGNAL_MOD_EQ = 2 };

static uint32_t svc_wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout) {
    pthread_mutex_lock(&g_klock);
    // (the guest changes these words with its own atomics, outside g_klock: read-modify-writes are CASes, as in the
    // kernel - a plain load + store lost guest updates made in between)
    int32_t cur = (int32_t)__atomic_load_n(u32p(addr), __ATOMIC_SEQ_CST);
    bool wait;
    if (type == ARB_WAIT_LESS) wait = cur < value;
    else if (type == ARB_DEC_WAIT_LESS) {
        // Atomically, as Horizon: the game changes these words in user space (exclusive load / store, no kernel
        // lock), and a plain store of the value read before overwrote such a change - a lost signal.
        uint32_t v = (uint32_t)cur;
        while ((wait = (int32_t)v < value) &&
               !__atomic_compare_exchange_n(u32p(addr), &v, v - 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
    } else if (type == ARB_WAIT_EQ) wait = cur == value;
    else { pthread_mutex_unlock(&g_klock); return R_INVALID_ENUM; }
    if (!wait) { pthread_mutex_unlock(&g_klock); return R_INVALID_STATE; }
    if (timeout == 0) { pthread_mutex_unlock(&g_klock); return R_TIMEOUT; }
    t_cur->wait_kind = WAIT_ARBITER;
    t_cur->wait_addr = addr;
    uint32_t r = block_current(timeout);
    pthread_mutex_unlock(&g_klock);
    return r;
}

static int arbiter_wake(uint64_t addr, int32_t count) {  // (the most urgent waiters first, as Horizon)
    int woken = 0;
    while (count <= 0 || woken < count) {
        KThread *best = NULL;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr && (!best || t->prio < best->prio)) best = t;
        if (!best) break;
        best->wait_kind = WAIT_NONE;
        best->wait_result = R_OK;
        wake(best);
        woken++;
    }
    return woken;
}

// The word at addr from `expected` to `desired` if it still holds `expected` (user space may change it any time).
static bool arbiter_update(uint64_t addr, int32_t expected, int32_t desired) {
    uint32_t e = (uint32_t)expected;
    return __atomic_compare_exchange_n(u32p(addr), &e, (uint32_t)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static uint32_t svc_signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    pthread_mutex_lock(&g_klock);
    uint32_t r = R_OK;
    if (type == ARB_SIGNAL) {
        arbiter_wake(addr, count);
    } else if (type == ARB_SIGNAL_INC_EQ) {
        if (!arbiter_update(addr, value, value + 1)) r = R_INVALID_STATE;
        else arbiter_wake(addr, count);
    } else if (type == ARB_SIGNAL_MOD_EQ) {
        int waiting = 0;
        for (KThread *t = g_waitlist; t; t = t->wnext)
            if (t->wait_kind == WAIT_ARBITER && t->wait_addr == addr) waiting++;
        int32_t nv;
        if (count <= 0) nv = waiting > 0 ? value - 2 : value + 1;
        else nv = waiting == 0 ? value + 1 : (waiting <= count ? value - 1 : value);
        if (!arbiter_update(addr, value, nv)) r = R_INVALID_STATE;
        else arbiter_wake(addr, count);
    } else {
        r = R_INVALID_ENUM;
    }
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ wait synchronization

static uint32_t svc_wait_synchronization(uint64_t handles_ptr, int32_t count, int64_t timeout, int32_t *index) {
    pthread_mutex_lock(&g_klock);
    struct timespec ts;
    if (timeout > 0) deadline(&ts, timeout);
    KThread *t = t_cur;
    uint32_t *hs = (uint32_t *)(uintptr_t)handles_ptr;
    uint32_t r;
    if (ENV("LARECOMP_VSYNC_BT")) {  // (debug) who waits for the display's vsync event: the game's frame loop
        extern uint32_t vi_vsync_handle(void);
        static int shown;
        for (int i = 0; i < count && shown < 2; i++)
            if (hs[i] && hs[i] == vi_vsync_handle()) {
                shown++;
                LOG("[vsync-bt] wait on vsync (%d handles, timeout %lld) lr %llx:\n", count, (long long)timeout,
                    (unsigned long long)(t ? t->cpu.x[30] : 0));
                plat_print_backtrace();
            }
    }
    for (;;) {
        for (int i = 0; i < count; i++) {
            KObject *o = handle_get(hs[i]);
            if (!o) { r = R_INVALID_HANDLE; goto out; }
            if (o->signaled) { *index = i; r = R_OK; goto out; }
        }
        if (timeout == 0) { r = R_TIMEOUT; goto out; }  // (before the cancel: a poll leaves it for the next real wait)
        if (t->cancel) { t->cancel = false; r = R_CANCELLED; goto out; }
        if (g_rt_unwind) { r = R_CANCELLED; goto out; }
        t->in_sync_wait = true;
        t->wait_since = plat_now_ns();
        t->sync_handles = handles_ptr, t->sync_count = count;
        t->snext = g_syncwaiters;
        g_syncwaiters = t;
        bool ok = kwait(timeout < 0 ? -1 : timeout, &ts);
        for (KThread **p = &g_syncwaiters; *p; p = &(*p)->snext)
            if (*p == t) { *p = t->snext; break; }
        t->in_sync_wait = false;
        if (!ok) { r = R_TIMEOUT; goto out; }
    }
out:
    pthread_mutex_unlock(&g_klock);
    return r;
}

// ------------------------------------------------------------------ debug output
static void debug_string(uint64_t p, uint64_t len) {
    fprintf(stderr, "[guest] %.*s", (int)len, (const char *)(uintptr_t)p);
    if (len && ((const char *)(uintptr_t)p)[len - 1] != '\n') fputc('\n', stderr);
}

// ------------------------------------------------------------------ SVC dispatch
static int trace_svc = -1;

static void svc_impl(Cpu *c, uint32_t imm, uint64_t pc);

static _Atomic uint64_t svc_wait_ns[32][0x80], svc_calls[32][0x80];
static int prof_svc = -1;

static void svc_entry(Cpu *c, uint32_t imm, uint64_t pc);
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    rt_unwind_point();
    svc_entry(c, imm, pc);
    rt_unwind_point();
}

static void svc_entry(Cpu *c, uint32_t imm, uint64_t pc) {
    if (prof_svc < 0) prof_svc = ENV("LARECOMP_PROF_SVC") != NULL;
    if (prof_svc && imm < 0x80) {
        uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3], lr = c->x[30];
        uint32_t h0 = imm == 0x18 && a2 ? *(uint32_t *)(uintptr_t)a1 : 0;
        uint64_t t0 = plat_now_ns();
        svc_impl(c, imm, pc);
        uint64_t dt = plat_now_ns() - t0;
        static _Atomic int nslow;
        if (((t_cur && t_cur->id == 80 && (imm == 0x18 || imm == 0x34)) || (dt > 10000000 && imm != 0x0b && imm != 0x1c)) && nslow < 20000) {
            nslow++;
            LOG("[slow] t%llu svc%02x %llums a=%llx,%llx,%llx,%llx h0=%x lr=%llx\n", (unsigned long long)(t_cur ? t_cur->id : 0), imm,
                (unsigned long long)(dt / 1000000), (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, h0, (unsigned long long)lr);
        }
        unsigned tid = t_cur ? (unsigned)(t_cur->id & 31) : 31;
        svc_wait_ns[tid][imm] += dt;
        svc_calls[tid][imm]++;
        static _Atomic uint64_t last;
        uint64_t now = plat_now_ns();
        if (now - last > 2000000000ull) {
            last = now;
            for (int t = 0; t < 32; t++) {
                int any = 0;
                for (int i = 0; i < 0x80; i++)
                    if (svc_calls[t][i]) {
                        if (!any) { LOG("[svcprof] t%d", t); any = 1; }
                        LOG(" %02x:%llu/%llums", i, (unsigned long long)svc_calls[t][i], (unsigned long long)(svc_wait_ns[t][i] / 1000000));
                        svc_calls[t][i] = 0; svc_wait_ns[t][i] = 0;
                    }
                if (any) LOG("\n");
            }
        }
        return;
    }
    if (trace_svc < 0) trace_svc = getenv("LARECOMP_TRACE_SVC") != NULL;
    if (!trace_svc) return svc_impl(c, imm, pc);
    uint64_t in[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};
    svc_impl(c, imm, pc);
    LOG("[svc] %02x(%llx, %llx, %llx, %llx) -> %llx %llx %llx  @%llx\n", imm, in[0], in[1], in[2], in[3], c->x[0],
        c->x[1], c->x[2], pc);
}

static void svc_impl(Cpu *c, uint32_t imm, uint64_t pc) {
    uint64_t *x = c->x;
    switch (imm) {
    case 0x01: {  // SetHeapSize
        uint64_t size = x[1];
        if (size > HEAP_MAX) { x[0] = R_OUT_OF_RESOURCE; break; }
        pthread_mutex_lock(&g_mlock);
        heap_size = size;
        region_remove(HEAP_BASE);
        if (size) region_add(HEAP_BASE, size, 5, 3);
        pthread_mutex_unlock(&g_mlock);
        x[0] = R_OK;
        x[1] = HEAP_BASE;
        break;
    }
    case 0x02: case 0x03: x[0] = R_OK; break;  // SetMemoryPermission / SetMemoryAttribute
    case 0x04:  // MapMemory(dst, src, size): src is inaccessible while mapped, so copying is equivalent
        pthread_mutex_lock(&g_mlock);
        if (region_taken(x[0], x[2], "MapMemory")) {
            pthread_mutex_unlock(&g_mlock);
            x[0] = R_INVALID_CURRENT_MEMORY;
            break;
        }
        region_add(x[0], x[2], 0xB, 3);
        pthread_mutex_unlock(&g_mlock);
        memcpy((void *)x[0], (void *)x[1], x[2]);
        x[0] = R_OK;
        break;
    case 0x05:  // UnmapMemory(dst, src, size): copy back
        memcpy((void *)x[1], (void *)x[0], x[2]);
        pthread_mutex_lock(&g_mlock);
        region_remove(x[0]);
        pthread_mutex_unlock(&g_mlock);
        x[0] = R_OK;
        break;
    case 0x06:  // QueryMemory(out_info, _, addr)
        pthread_mutex_lock(&g_mlock);
        query_memory(x[2], x[0]);
        pthread_mutex_unlock(&g_mlock);
        x[0] = R_OK;
        x[1] = 0;
        break;
    case 0x07: LOG("ExitProcess\n"); exit(0);
    case 0x08: {  // CreateThread(_, entry, arg, stack_top, prio, core)
        KThread *t = thread_new(x[1], x[2], x[3], (int)x[4], (int)(int32_t)x[5]);
        t->handle = kernel_new_handle(&t->obj);
        LOG("[kernel] CreateThread entry=%llx stack=%llx prio=%d -> %x\n", x[1], x[3], (int)x[4], t->handle);
        x[0] = R_OK;
        x[1] = t->handle;
        break;
    }
    case 0x09: {  // StartThread
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KOBJ_THREAD) { x[0] = R_INVALID_HANDLE; break; }
        thread_start((KThread *)o);
        x[0] = R_OK;
        break;
    }
    case 0x0A: longjmp(t_cur->unwind, 1);  // ExitThread: back to thread_main, which signals the thread object
    case 0x0B: {  // SleepThread (in slices of at most 50 ms: a reboot doesn't wait out a long sleep)
        int64_t ns = (int64_t)x[0];
        if (ns <= 0) { sched_yield(); break; }
        for (uint64_t end = plat_now_ns() + (uint64_t)ns, now; !g_rt_unwind && (now = plat_now_ns()) < end;) {
            uint64_t d = end - now < 50000000 ? end - now : 50000000;
            struct timespec ts = {(time_t)(d / 1000000000), (long)(d % 1000000000)};
            nanosleep(&ts, NULL);
            g_blocked_ns += plat_now_ns() - now;
        }
        break;
    }
    case 0x0C: {  // GetThreadPriority(_, handle)
        KObject *o = handle_get((uint32_t)x[1]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? (uint64_t)((KThread *)o)->prio : 0;
        break;
    }
    case 0x0D: {  // SetThreadPriority
        KObject *o = handle_get((uint32_t)x[0]);
        if (o) ((KThread *)o)->prio = (int)x[1];
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        break;
    }
    case 0x0E: {  // GetThreadCoreMask(_, _, handle)
        KObject *o = handle_get((uint32_t)x[2]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? (uint64_t)((KThread *)o)->core : 0;
        x[2] = 0xF;
        break;
    }
    case 0x0F: x[0] = R_OK; break;  // SetThreadCoreMask
    case 0x10: x[0] = t_cur->core; break;  // GetCurrentProcessorNumber
    case 0x11: case 0x12: case 0x17: {  // SignalEvent / ClearEvent / ResetSignal
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o) x[0] = R_INVALID_HANDLE;
        else {
            KObject *ev = o->type == KOBJ_EVENT ? o : NULL;
            if (!ev) x[0] = R_INVALID_HANDLE;
            else {
                if (imm == 0x11) kernel_signal(ev);
                else if (imm == 0x17 && !ev->signaled) { x[0] = R_INVALID_STATE; pthread_mutex_unlock(&g_klock); break; }
                else ev->signaled = false;
                x[0] = R_OK;
            }
        }
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x13: {  // MapSharedMemory(handle, addr, size, perm)
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KOBJ_SHMEM) { x[0] = R_INVALID_HANDLE; break; }
        KSharedMem *s = (KSharedMem *)o;
        pthread_mutex_lock(&g_mlock);
        if (region_taken(x[1], x[2], "MapSharedMemory")) {
            pthread_mutex_unlock(&g_mlock);
            x[0] = R_INVALID_CURRENT_MEMORY;
            break;
        }
        region_add(x[1], x[2], 6, 3);
        pthread_mutex_unlock(&g_mlock);
        memset((void *)x[1], 0, x[2]);
        s->guest_addr = x[1];  // HLE services write straight into the guest's mapping
        if (s->on_map) s->on_map(s);
        x[0] = R_OK;
        break;
    }
    case 0x14:  // UnmapSharedMemory
        pthread_mutex_lock(&g_mlock);
        region_remove(x[1]);
        pthread_mutex_unlock(&g_mlock);
        x[0] = R_OK;
        break;
    case 0x15: {  // CreateTransferMemory(_, addr, size, perm)
        KTransferMem *t = calloc(1, sizeof *t);
        t->obj.type = KOBJ_TMEM;
        t->addr = x[1];
        t->size = x[2];
        x[0] = R_OK;
        x[1] = kernel_new_handle(&t->obj);
        break;
    }
    case 0x16:  // CloseHandle
        pthread_mutex_lock(&g_klock);
        handle_close((uint32_t)x[0]);
        pthread_mutex_unlock(&g_klock);
        x[0] = R_OK;
        break;
    case 0x18: {  // WaitSynchronization(_, handles, count, timeout)
        int32_t idx = -1;
        x[0] = svc_wait_synchronization(x[1], (int32_t)x[2], (int64_t)x[3], &idx);
        x[1] = (uint32_t)idx;
        break;
    }
    case 0x19: {  // CancelSynchronization
        pthread_mutex_lock(&g_klock);
        KObject *o = handle_get((uint32_t)x[0]);
        if (o && o->type == KOBJ_THREAD) {
            ((KThread *)o)->cancel = true;
            wake((KThread *)o);
            x[0] = R_OK;
        } else x[0] = R_INVALID_HANDLE;
        pthread_mutex_unlock(&g_klock);
        break;
    }
    case 0x1A: x[0] = svc_arbitrate_lock((uint32_t)x[0], x[1], (uint32_t)x[2]); break;
    case 0x1B: x[0] = svc_arbitrate_unlock(x[0]); break;
    case 0x1C: x[0] = svc_wait_process_wide_key(x[0], x[1], (uint32_t)x[2], (int64_t)x[3]); break;
    case 0x1D: svc_signal_process_wide_key(x[0], (int32_t)x[1]); x[0] = R_OK; break;
    case 0x1E: x[0] = rt_ticks(); break;
    case 0x1F: {  // ConnectToNamedPort(_, name)
        uint32_t h = ipc_connect_named_port((const char *)(uintptr_t)x[1]);
        x[0] = h ? R_OK : R_NOT_FOUND;
        x[1] = h;
        break;
    }
    case 0x21: x[0] = ipc_handle_request(c, (uint32_t)x[0]); break;  // SendSyncRequest
    case 0x22: {  // SendSyncRequestWithUserBuffer(buf, size, handle)
        uint64_t saved = c->tpidrro;
        c->tpidrro = x[0];
        x[0] = ipc_handle_request(c, (uint32_t)x[2]);
        c->tpidrro = saved;
        break;
    }
    case 0x24: x[0] = R_OK; x[1] = 0x81; break;  // GetProcessId
    case 0x25: {  // GetThreadId(_, handle)
        KObject *o = handle_get((uint32_t)x[1]);
        x[0] = o ? R_OK : R_INVALID_HANDLE;
        x[1] = o ? ((KThread *)o)->id : 0;
        break;
    }
    case 0x26:  // Break
        if (x[0] & 0x80000000u) { LOG("[kernel] Break notification %llx\n", x[0]); x[0] = R_OK; break; }
        if (x[2] == 4 && x[1] >= GUEST_LO && x[1] < GUEST_HI) {  // an abort: info is the failed Result (module, description)
            uint32_t rc = LD32(x[1]);  // (no info pointer: reading address 0 crashed here, hiding the abort's own report)
            LOG("[kernel] abort on result 0x%x (%u-%u)\n", rc, 2000 + (rc & 0x1FF), (rc >> 9) & 0x1FFF);
        }
        {   // the guest's call chain (frame records: [fp] = caller's fp, [fp + 8] = return address)
            uint64_t fp = c->x[29];
            for (int i = 0; i < 16 && fp >= GUEST_LO && fp < GUEST_HI && !(fp & 7); i++, fp = LD64(fp))
                LOG("  guest #%d %llx\n", i, (unsigned long long)LD64(fp + 8));
        }
        FATAL("svcBreak(reason=%llx, info=%llx, size=%llx) at %llx\n", x[0], x[1], x[2], pc);
    case 0x27: debug_string(x[0], x[1]); x[0] = R_OK; break;
    case 0x29: {  // GetInfo(_, id0, handle, id1)
        uint32_t id0 = (uint32_t)x[1];
        uint64_t v = 0;
        uint32_t r = R_OK;
        switch (id0) {
        case 0: v = 0xF; break;
        case 1: v = 0xFFFFFFFFFFFFFFFFull; break;
        case 2: v = ALIAS_BASE; break;
        case 3: v = ALIAS_SIZE; break;
        case 4: v = HEAP_BASE; break;
        case 5: v = HEAP_MAX; break;
        case 6: case 21: v = APP_MEMORY; break;  // application memory (3.2 GB, as on hardware)
        case 7: case 22: v = used_memory(); break;
        case 8: v = 0; break;
        case 9: v = 0; break;
        case 11: v = ((uint64_t)arc4random() << 32) | arc4random(); break;
        case 12: v = GUEST_LO; break;
        case 13: v = GUEST_HI - GUEST_LO; break;
        case 14: v = STACK_BASE; break;
        case 15: v = STACK_SIZE; break;
        case 16: v = g_system_resource_size; break;
        case 17: v = 0; break;
        case 18: v = g_program_id; break;
        case 20: {  // UserExceptionContextAddress: a page of the process (nn::os::SetDyingMessageRegion writes it)
            if (!g_user_exception_ctx) g_user_exception_ctx = (uint64_t)(uintptr_t)hle_alloc(0x1000);
            v = g_user_exception_ctx;
            break;
        }
        case 23: v = 1; break;  // IsApplication
        case 26: v = 1; break;  // IsSvcPermitted
        case 27: case 28: v = 0; break;  // IoRegionHint, AliasRegionExtraSize (18.0.0+: SDK 19 games ask)
        default: r = R_INVALID_ENUM; LOG("[kernel] GetInfo unknown id %u/%llu\n", id0, x[3]);
        }
        x[0] = r;
        x[1] = v;
        break;
    }
    case 0x2C:  // MapPhysicalMemory(addr, size)
        if (ENV("LARECOMP_LOG_VAMM")) LOG("[kernel] MapPhysicalMemory %llx +%llx\n", x[0], x[1]);
        pthread_mutex_lock(&g_mlock);
        if (used_memory() && phys_limit && used_memory() + phys_new_bytes(x[0], x[1]) > APP_MEMORY) {
            pthread_mutex_unlock(&g_mlock);
            static int nlog;
            if (nlog++ < 4) LOG("[kernel] MapPhysicalMemory %llx +%llx: LimitReached (used %llu MB)\n", x[0], x[1],
                                (unsigned long long)(used_memory() >> 20));
            x[0] = R_LIMIT_REACHED;
            break;
        }
        phys_map(x[0], x[1]);
        pthread_mutex_unlock(&g_mlock);
        x[0] = R_OK;
        break;
    case 0x2D:  // UnmapPhysicalMemory(addr, size)
        if (ENV("LARECOMP_LOG_VAMM")) LOG("[kernel] UnmapPhysicalMemory %llx +%llx\n", x[0], x[1]);
        pthread_mutex_lock(&g_mlock);
        phys_unmap(x[0], x[1]);
        pthread_mutex_unlock(&g_mlock);
        x[0] = R_OK;
        break;
    case 0x34: x[0] = svc_wait_for_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int64_t)x[3]); break;
    case 0x35: x[0] = svc_signal_to_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int32_t)x[3]); break;
    case 0x45: {  // CreateEvent -> w1 writable, w2 readable
        KObject *ev = calloc(1, sizeof *ev);
        ev->type = KOBJ_EVENT;
        x[0] = R_OK;
        x[1] = kernel_new_handle(ev);
        x[2] = kernel_new_handle(ev);
        if (ENV("LARECOMP_PROF_SVC")) LOG("[evt] guest w=%llx r=%llx lr=%llx\n", (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[30]);
        break;
    }
    default:
        FATAL("unimplemented svc 0x%x at %llx (x0=%llx x1=%llx x2=%llx)\n", imm, pc, x[0], x[1], x[2]);
    }
}

// Reboot, step 1: stop every guest thread (see rt_unwind_point) and join it. `idle` keeps the window responsive.
void kernel_stop_threads(void (*idle)(void)) {
    g_rt_unwind = 1;
    uint64_t t0 = plat_now_ns(), logged = t0;
    for (;;) {
        int live = 0;
        pthread_mutex_lock(&g_klock);
        for (KThread *t = g_threads; t; t = t->anext)
            if (t->started && !t->finished) { live++; wake(t); }
        pthread_cond_broadcast(&g_nothread_cond);
        pthread_mutex_unlock(&g_klock);
        if (!live) break;
        uint64_t now = plat_now_ns();
        if (now - logged > 3000000000ull) {  // (a thread that never reaches a safe point: who and where)
            logged = now;
            LOG("[reboot] still waiting for %d guest threads after %llums:\n", live, (unsigned long long)((now - t0) / 1000000));
            for (KThread *t = g_threads; t; t = t->anext)
                if (t->started && !t->finished)
                    LOG("[reboot]   thread %llu (entry %llx) pc %llx lr %llx wait %d sync %d\n", (unsigned long long)t->id,
                        (unsigned long long)t->entry, (unsigned long long)t->cpu.pc, (unsigned long long)t->cpu.x[30],
                        t->wait_kind, t->in_sync_wait);
        }
        if (idle) idle();
        usleep(2000);
    }
    int n = 0;
    for (KThread *t = g_threads; t; t = t->anext)
        if (t->started) {
#ifdef _WIN32
            if (!guest_thread_join(t->host)) FATAL("Could not join a game thread.\n");
#else
            pthread_join(t->host, NULL);
#endif
            n++;
        }
    LOG("[reboot] %d guest threads stopped in %llums\n", n, (unsigned long long)((plat_now_ns() - t0) / 1000000));
}

// (diagnostics) every guest thread and what it waits on, with the guest frame chain (x29 / x30 pairs on the guest
// stack: the game function that waits). Doesn't wait for g_klock: a hang may hold it (then says so).
void kernel_dump_threads(const char *prefix) {
    bool locked = false;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&g_klock) == 0); i++) usleep(1000);
    if (!locked) LOG("%s g_klock held for 100+ ms: read without it\n", prefix);
    static const char *const kinds[] = {"-", "mutex", "condvar", "arbiter"};
    extern const char *hle_name_at(uint64_t pc);
    uint64_t now = plat_now_ns();
    for (KThread *t = g_threads; t; t = t->anext) {
        if (!t->started || t->finished) continue;
        bool waiting = t->wait_kind != WAIT_NONE || t->in_sync_wait;
        const char *hle = hle_name_at(t->cpu.pc);  // (pc: the last call through dispatch; an HLE one: its name)
        LOG("%s thread %llu h %x prio %d core %d entry %llx pc %llx%s%s%s lr %llx wait %s addr %llx mutex %llx tag %x%s%s for %llu ms\n",
            prefix, (unsigned long long)t->id, t->handle, t->prio, t->core, (unsigned long long)t->entry,
            (unsigned long long)t->cpu.pc, hle ? " (" : "", hle ? hle : "", hle ? ")" : "", (unsigned long long)t->cpu.x[30],
            (unsigned)t->wait_kind < 4 ? kinds[t->wait_kind] : "?", (unsigned long long)t->wait_addr,
            (unsigned long long)t->wait_mutex, t->wait_tag,
            t->in_sync_wait ? " in WaitSynchronization" : "", t->cancel ? " (cancel pending)" : "",
            waiting ? (unsigned long long)((now - t->wait_since) / 1000000) : 0ull);
        if (t->wait_kind == WAIT_CONDVAR || t->wait_kind == WAIT_MUTEX) {
            uint64_t m = t->wait_kind == WAIT_CONDVAR ? t->wait_mutex : t->wait_addr;
            LOG("%s   mutex word %x%s\n", prefix, *(volatile uint32_t *)(uintptr_t)m,
                t->wait_kind == WAIT_CONDVAR ? "" : " (waiting to be handed it)");
        }
        for (int i = 0; t->in_sync_wait && i < t->sync_count && i < 8; i++) {
            uint32_t h = ((const uint32_t *)(uintptr_t)t->sync_handles)[i];
            KObject *o = h > 0x100 && h - 0x100 < MAX_HANDLES ? handles[h - 0x100] : NULL;
            LOG("%s   handle %x type %d%s\n", prefix, h, o ? (int)o->type : 0, o && o->signaled ? " signaled" : "");
        }
        uint64_t fp = t->cpu.x[29];  // guest frames: [fp] = caller's fp, [fp + 8] = return address
        for (int i = 0; i < 12 && fp >= STACK_BASE && fp < STACK_BASE + STACK_SIZE && !(fp & 15); i++) {
            LOG("%s   frame %d: lr %llx\n", prefix, i, (unsigned long long)*(uint64_t *)(uintptr_t)(fp + 8));
            uint64_t up = *(uint64_t *)(uintptr_t)fp;
            if (up <= fp) break;
            fp = up;
        }
    }
    {   // the kernel trace: oldest first
        static const char *const kn[] = {"cv-wait", "cv-wait-end", "cv-signal", "arb-lock", "arb-lock-end", "arb-unlock"};
        uint64_t n = __atomic_load_n(&ktrace_n, __ATOMIC_RELAXED), first = n > KTRACE_N ? n - KTRACE_N : 0;
        LOG("%s kernel trace, last %llu of %llu (ms before now; words before -> after):\n", prefix,
            (unsigned long long)(n - first), (unsigned long long)n);
        for (uint64_t i = first; i < n; i++) {
            const KTrace *e = &ktrace[i % KTRACE_N];
            long long ago = (long long)(now - e->ns) / 1000000;
            switch (e->kind) {
            case KT_WAIT: LOG("%s  -%lld t%llu cv-wait key %llx (%x) mutex %llx %x -> %x\n", prefix, ago,
                              (unsigned long long)e->tid, (unsigned long long)e->addr, e->w0, (unsigned long long)e->addr2,
                              e->w1, e->w2); break;
            case KT_WAIT_END: LOG("%s  -%lld t%llu cv-wait-end key %llx (%x) mutex %llx (%x) r %x\n", prefix, ago,
                                  (unsigned long long)e->tid, (unsigned long long)e->addr, e->w0, (unsigned long long)e->addr2,
                                  e->w1, e->arg); break;
            case KT_SIGNAL: LOG("%s  -%lld t%llu cv-signal key %llx %x -> %x count %d woke %u\n", prefix, ago,
                                (unsigned long long)e->tid, (unsigned long long)e->addr, e->w0, e->w1, (int)e->arg, e->w2); break;
            default: LOG("%s  -%lld t%llu %s mutex %llx %x -> %x owner %x tag/r %x\n", prefix, ago, (unsigned long long)e->tid,
                         kn[e->kind < 6 ? e->kind : 0], (unsigned long long)e->addr, e->w0, e->w1, e->w2, e->arg);
            }
        }
    }
    if (locked) pthread_mutex_unlock(&g_klock);
}

// The hang dump, one facility for the whole runtime. LARECOMP_HANG_DUMP=<s> (default 30, 0 off), checked every
// second from the main guest thread's start:
// - the main guest thread in one kernel wait that long (startup hangs on Windows with few cores leave no other trace)
// - the renderer's own checks (runtime/nvn/rqueue.c, kernel_hang_hooks): no frame presented twice that long after the
//   GPU device setup, presenting stopped that long, or one noted host wait (GPU fence, queue, nextDrawable /
//   vkAcquire...) that long
// -> "[hang]" lines: why, the renderer's state (render thread queue, what each noted thread waits on), every guest
// thread with its kernel wait and guest frames, and the kernel trace. At most 6 dumps per run.
static bool (*hang_check_fn)(uint64_t limit_ns, char *why, size_t n);
static void (*hang_state_fn)(const char *prefix);
void kernel_hang_hooks(bool (*check)(uint64_t limit_ns, char *why, size_t n), void (*state)(const char *prefix)) {
    __atomic_store_n(&hang_state_fn, state, __ATOMIC_RELEASE);
    __atomic_store_n(&hang_check_fn, check, __ATOMIC_RELEASE);
}
static void *hang_watch(void *arg) {
    uint64_t s = (uint64_t)(uintptr_t)arg, dumped_since = 0;
    for (int dumps = 0; dumps < 6;) {
        sleep(1);
        char why[256];
        int n = 0;
        pthread_mutex_lock(&g_klock);
        KThread *m = g_main_thread;  // (the current boot's: kernel_reset clears it)
        uint64_t since = m ? m->wait_since : 0, now = plat_now_ns();
        bool stuck = m && m->started && !m->finished && (m->wait_kind != WAIT_NONE || m->in_sync_wait) && since < now &&
                     now - since >= s * 1000000000ull && since != dumped_since;
        pthread_mutex_unlock(&g_klock);
        if (stuck) {
            n = snprintf(why, sizeof why, "main guest thread in one kernel wait for %llu s", (unsigned long long)s);
            dumped_since = since;
        }
        bool (*check)(uint64_t, char *, size_t) = __atomic_load_n(&hang_check_fn, __ATOMIC_ACQUIRE);  // (every second: it keeps its own "reported" state)
        if (check && check(s * 1000000000ull, why + n + (n ? 2 : 0), sizeof why - (size_t)n - 2)) {
            if (n) why[n] = ';', why[n + 1] = ' ';
            n = 1;
        }
        if (!n) continue;
        LOG("[hang] ===== %s =====\n", why);
        void (*state)(const char *) = __atomic_load_n(&hang_state_fn, __ATOMIC_ACQUIRE);
        if (state) state("[hang]");
        kernel_dump_threads("[hang]");
        LOG("[hang] =====\n");
        dumps++;
    }
    return NULL;
}

// Reboot, step 2 (threads stopped): a fresh kernel. Kernel objects are leaked rather than freed: host statics
// can still point at some (vi.c's vsync event, signalled by its host timer thread).
void kernel_reset(void) {
    pthread_mutex_lock(&g_klock);
    for (KThread *t = g_threads, *next; t; t = next) {
        next = t->anext;
        pthread_cond_destroy(&t->cond);
        free(t);
    }
    g_threads = NULL;
    g_main_thread = NULL;
    g_waitlist = NULL;
    g_syncwaiters = NULL;
    memset(handles, 0, sizeof handles);
    nregions = 0;
    heap_size = 0;
    tls_next = TLS_BASE;
    next_thread_id = 0x50;
    g_rt_unwind = 0;
    pthread_mutex_unlock(&g_klock);
}

// LARECOMP_WAIT_DUMP=<seconds>: every so often, what each guest thread waits on (hangs: who holds what)
static void *wait_dump_main(void *arg) {
    unsigned iv = (unsigned)(uintptr_t)arg;
    for (;;) {
        sleep(iv);
        pthread_mutex_lock(&g_klock);
        LOG("[wait] ---- threads\n");
        static const char *kinds[] = {"run", "mutex", "condvar", "arbiter"};
        for (KThread *t = g_threads; t; t = t->anext) {
            if (t->finished) continue;
            const char *k = t->in_sync_wait ? "sync" : kinds[t->wait_kind];
            uint64_t a = t->wait_addr;
            LOG("[wait] %x prio %d %-7s addr %llx (=%x) mutex %llx (=%x) tag %x  pc %llx lr %llx\n", t->handle, t->prio, k,
                (unsigned long long)a, a ? LD32(a) : 0, (unsigned long long)t->wait_mutex,
                t->wait_mutex ? LD32(t->wait_mutex) : 0, t->wait_tag, (unsigned long long)t->cpu.pc,
                (unsigned long long)t->cpu.x[30]);
            if (t->in_sync_wait) {
                char hl[128] = "";
                for (int i = 0, n = 0; i < t->sync_n && i < 16; i++)
                    n += snprintf(hl + n, sizeof hl - n, " %x", t->sync_hs[i]);
                LOG("[wait]     handles%s\n", hl);
            }
        }
        pthread_mutex_unlock(&g_klock);
        extern void audio_wait_dump(void);
        audio_wait_dump();
    }
    return NULL;
}

void kernel_init(void) {
    if (ENV("LARECOMP_WAIT_DUMP")) {
        pthread_t th;
        pthread_create(&th, NULL, wait_dump_main, (void *)(uintptr_t)atoi(ENV("LARECOMP_WAIT_DUMP")));
        pthread_detach(th);
    }
    // Reserve the heap, alias, stack, and TLS regions.
    region_add(TLS_BASE, TLS_SIZE, 0xC, 3);
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        region_add(m->base + m->text_off, m->ro_off - m->text_off, 3, 5);
        region_add(m->base + m->ro_off, m->data_off - m->ro_off, 3, 1);
        region_add(m->base + m->data_off, m->size - m->data_off, 4, 3);
    }
}

KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) {
    KObject *ev = calloc(1, sizeof *ev);
    ev->type = KOBJ_EVENT;
    *w = kernel_new_handle(ev);
    *r = kernel_new_handle(ev);
    if (ENV("LARECOMP_PROF_SVC")) { const char *s = plat_symbol((uint64_t)(uintptr_t)__builtin_return_address(0), NULL); LOG("[evt] hle w=%x r=%x from %s\n", *w, *r, s ? s : "?"); }
    return ev;
}

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle) {
    KSharedMem *s = calloc(1, sizeof *s);
    s->obj.type = KOBJ_SHMEM;
    s->size = size;
    s->on_map = on_map;
    *handle = kernel_new_handle(&s->obj);
    return s;
}
