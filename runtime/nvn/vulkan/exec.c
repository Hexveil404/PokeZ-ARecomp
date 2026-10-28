// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef __linux__
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "../../kernel.h"
#include "shaders.h"
#include "vk.h"
#include "record.h"
#include "../renderer.h"
#include "../pace.h"
#include "../recolour.h"
#include "../screen_filter.h"
#include "../shader_prep.h"
#include "../../settings.h"

// LARECOMP_SKIP_WHY=1: skipped draws / dispatches per source line, with the stats
static uint32_t skip_why[4096];
#define stat_skipped_here() (stat_skipped++, skip_why[__LINE__ & 4095]++)
static uint64_t stat_vtx_cached, stat_vtx_host, stat_xfb_sessions;  // vertex stream bytes bound from the vertex cache / from host memory
static uint64_t stat_scene_wait_ns;  // (wait_scene)
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
// (the guest heap for shaders in chunks: with the heap import below)
static uint32_t gc_shift;
static uint32_t gmem_chunk_shift(void);
static VkDescriptorSetLayout gmem_chunk_layout(void);
static void gc_scan_pointers(const uint32_t *w, uint64_t bytes);
static bool gc_lazy;
static int gc_imported;
static uint64_t gc_imported_bytes, stat_gc_misses;
static uint64_t gmem_heap_size(void);
static void gc_frame(void);
static uint64_t stat_upload_bytes;
static uint64_t prof_cb_reused, prof_cb_copied;  // (VK_PROF) lean constant blocks: copies reused, made (cbuf_lean)
static bool cbuf_lean(void);
static uint64_t prof_prog_memo, prof_pipe_memo, prof_key_draws;  // (VK_PROF) key memo answers (get_program), draws
static bool key_memo_on(void);
static uint64_t prof_sets_reused, prof_sets_new;  // (VK_PROF) set reuse (allocations: record.c RecStats)
static bool set_reuse_on(void);
static uint64_t prof_slot_wait;  // (VK_PROF) sub(): ring slot waits (ns; GPU waits, inside "pre" when begin_pass starts a submission)
static uint64_t prof_hook_draws, prof_hook_known;  // (VK_PROF) draws through the texture hook pass, with a known texture
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;  // the GPU timer's timestamps (3 per ring slot: start, end, window image acquired)
static uint64_t prof_win, prof_span, prof_wframes;  // (profiling) the window image's submissions: GPU ns, frame spans, count
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint64_t submit_draws;  // cum_draws at the last submission (backend_execute_batch)
static bool list_fenced;       // the list being run has a FenceSync: its submission ends with it (backend_execute_batch)
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
static char stats_text[2048];
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
// Every call on a command buffer, the descriptor pools and the queue goes through record.c (the recorder thread, or
// at once with it off): this file decides, the recorder calls. submit_seq counts what this thread handed to the
// recorder (what it will have submitted, in this order); rec_submitted() is what the driver has.
#define SUBS VK_SUBS
typedef struct {
    VkCommandBuffer main, upload;       // (allocated once, vk_exec_init: the pool is the recorder's from then on)
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (the GPU timer)
    bool ts_skip;                       // (waits on the window image: not counted, see vk_gpu_total_ns)
    bool ts_win;                        // the window image's submission: a 3rd timestamp after its acquire wait
    uint64_t seq;
    // (its descriptor pools: record.c, per slot)
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static void ss_forget(void);
static uint64_t submit_seq;  // last handed to the recorder (= submitted, with the recorder off)
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;
static int passes_in_sub;  // render passes in the submission being recorded (LARECOMP_CHUNK_PASSES, begin_pass)

// CPU / GPU overlap (as renderer.m: the Metal backend, metal-overlap). The game's frame loop submits a frame, presents
// it, and at once waits on that frame's fence (every SyncWait is on the newest submission), so it builds frame N+1
// only once the GPU has finished frame N: a frame costs CPU + GPU time in series. Here the wait is taken one fence
// late: SyncWait(fence N) waits for fence N-1's submission and returns, and the next SyncWait waits for N. The CPU
// builds frame N+1 while the GPU draws N; at most two frames of GPU work are queued; a fence already complete is never
// waited for. Everything this backend recycles is keyed on the timeline (sub(), vk_transient, the set pools after the
// slot's wait, readbacks and screenshots wait on their own submission), not on the game's fence, so only the game
// thread's wait changes. What the game may touch in that frame while the GPU still reads it: its per-frame vertex /
// index data in the guest heap (bound in place; an engine built to overlap double-buffers it anyway), uniform data is
// copied at translation (guest_copy), GPU results the CPU reads (texture / buffer copies) are flushed synchronously,
// and the storage buffers it rewrites one frame ahead get copies (snap_storage). The timing counters are read a frame
// before their GPU time is known: in this mode an end counter reports the latest completed frame's GPU span
// (cq_resolve keeps it host side and no longer writes the game's memory late - the game may have reused the slot).
// LARECOMP_GPU_OVERLAP=0: off (the wait on the fence's own submission, as before); =1: on.
#define VK_OVERLAP_DEFAULT 1  // on where the write watch can run (overlap_on: off by itself without page write tracking)
static uint64_t deferred_seq;   // (seq_lock) the fence the game was told is done but whose submission wasn't waited for
static uint64_t stat_deferred;  // SyncWaits returned without waiting (stats)
static uint64_t stat_snap_bytes, stat_snap_copies;  // storage copies made (snap_storage; stats, per period)
// The overlap needs the storage write watch (snap_storage: the OS's page write tracking), the only thing that catches
// the game rewriting its light lists with the same bytes while the GPU reads them. Where the OS gives no tracking
// (Linux over a userptr or dma-buf heap - AMD, the Steam Deck -, a kernel before 6.7 without PAGEMAP_SCAN, a
// container without userfaultfd, a Windows window reserved without write watch) the overlap turns itself off: the
// full fence wait as before. LARECOMP_GPU_OVERLAP=1 forces it on anyway (tests; the hash probe stands in for the
// watch then), =0 turns it off.
static int pages_cpu_written(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);  // (the page tables below)
static const char *heap_tracking_reason(void);  // (why the heap has no page write tracking, for the log)
static bool overlap_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_GPU_OVERLAP"), *w = getenv("LARECOMP_OVERLAP_WATCH");
        uint32_t g;
        bool tracked = (!w || atoi(w) != 0) && pages_cpu_written(HEAP_BASE, 4096, 0, &g) >= 0;
        if (e) on = atoi(e) != 0;
        else if (VK_OVERLAP_DEFAULT && !tracked) {
            on = 0;
            LOG("[vk] GPU overlap off: no page write tracking on this system (%s); LARECOMP_GPU_OVERLAP=1 forces it on\n",
                w && atoi(w) == 0 ? "LARECOMP_OVERLAP_WATCH=0" : heap_tracking_reason());
            return on;
        } else on = VK_OVERLAP_DEFAULT;
        LOG("[vk] GPU overlap %s (the game's frame fence waited one frame late; LARECOMP_GPU_OVERLAP=%d: %s%s)\n", on ? "on" : "off",
            !on, on ? "off" : "on", on && !tracked ? "; forced without page write tracking: the hash probe instead of the write watch" : "");
    }
    return on;
}
static _Atomic uint64_t g_last_gpu_frame_ticks;  // (overlap) the latest resolved counter pair's GPU span, in Switch GPU ticks
static const uint64_t *cq_last_out;              // (cq_lock) the counter resolved last and its GPU ticks: a pair may straddle
static uint64_t cq_last_ticks;                   // two submissions
static uint64_t snap_ring_end(void);             // (snap_storage: the copies' ring after the game's heap, for gmem_heap_size)

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v));
    return v;
}

// (LARECOMP_VK_PROF) waits on the GPU by cause: the caller names it in wait_why just before waiting
static __thread const char *wait_why;
static struct { const char *why; uint64_t ns, n; } wait_tab[12];
static pthread_mutex_t wait_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static void wait_account(uint64_t ns) {
    const char *why = wait_why ? wait_why : "other";
    wait_why = NULL;
    pthread_mutex_lock(&wait_tab_lock);
    for (int i = 0; i < 12; i++)
        if (!wait_tab[i].why || wait_tab[i].why == why) { wait_tab[i].why = why; wait_tab[i].ns += ns; wait_tab[i].n++; break; }
    pthread_mutex_unlock(&wait_tab_lock);
}

static void cq_resolve_done(uint64_t done);

void vk_wait_seq(uint64_t seq) {
    if (!seq) { wait_why = NULL; return; }
    if (seq > rec_submitted()) {  // not handed to the driver yet: the recorder first (a timeline value can't be waited for sooner)
        pthread_mutex_lock(&seq_lock);
        uint64_t issued = submit_seq;
        pthread_mutex_unlock(&seq_lock);
        if (seq > issued) {  // (not even queued: a caller's mistake - everything queued so far instead, logged)
            static int n;
            if (n++ < 5) LOG("[vk] wait for submission %llu, only %llu queued\n", (unsigned long long)seq, (unsigned long long)issued);
            seq = issued;
            if (!seq) { wait_why = NULL; return; }
        }
        rec_wait_submitted(seq);
    }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    extern void rq_watch(const char *what, uint64_t arg);
    rq_watch("vkWaitSemaphores (GPU submission)", seq);
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    rq_watch(NULL, 0);
    uint64_t waited = SDL_GetTicksNS() - t0;  // (the game thread's fence waits and the render thread's: atomic sums)
    __atomic_fetch_add(&stat_wait_ns, waited, __ATOMIC_RELAXED);
    __atomic_fetch_add(&cum_wait, waited, __ATOMIC_RELAXED);
    wait_account(waited);
    // Callers recycle command buffers and allocations after this returns; a timeout is not completion.
    if (r == VK_TIMEOUT) FATAL("[vk] GPU did not finish submission %llu within 10 s; stopping before reusing in-flight resources\n",
                              (unsigned long long)seq);
    else if (r == VK_SUCCESS) cq_resolve_done(seq);  // (the game reads its timing counters after waiting)
    else {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

// ---- the game's GPU timing counters (nvnCommandBufferReportCounter, two per frame)
// The game times its GPU work with these and drops to 30 fps when frames look slow. They used to get the host clock
// at translation time, so our command translation counted as GPU time: busy scenes (outdoors, ~400 draws) looked slow
// and the game locked itself at 30 even on fast GPUs, at any resolution. Now each counter also records a GPU
// timestamp at its place in the command stream; once that submission is done (the game waits on its fence before
// reading - on the Switch the GPU writes them too) the GPU time replaces the provisional host value, in the host
// clock's terms (offset calibrated at each resolve: a timestamp can't be later than the moment its submission is seen
// done). LARECOMP_VK_COUNTER=cpu: host clock only; =frozen: one value per presented frame (test); =legacy: the host
// clock in ns instead of GPU ticks, as before (test: reproduces the locked 30).
#define CQ_MAX 64
typedef struct { VkQueryPool pool; uint32_t n; uint64_t seq; uint64_t *out[CQ_MAX]; } CounterQ;
static CounterQ cq[SUBS];
static pthread_mutex_t cq_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t cq_offset;  // host ns - GPU ns (the smallest seen)
// Counter timestamps are in the Switch GPU's ticks (614.4 MHz): the game converts the difference to ns itself
// (x 625 / 384). Writing ns made every GPU time look 1.63x as long.
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * 384 / 625); }
static bool cq_calibrated;
static int counter_mode = -1;  // 0 cpu, 1 frozen, 2 gpu
static uint64_t stat_counters_gpu;

// Pokémon Z-A sizes its dynamic resolution from these counters - and the GPU time they show includes our translation's
// passes, so on any PC it sat at its floor (55-65% of 1080p: pixelated, aliased). Unless the setting dynamic_res =
// game, Z-A's counters show no GPU time (as "frozen"): it renders at full resolution, its frame mode being its own
// (rt_za_frame_cfg). F1 > Dynamic resolution; applies at once.
static int vk_counter_mode(void) {
    static bool env;
    if (counter_mode < 0) {
        const char *e = ENV("LARECOMP_VK_COUNTER");
        env = e != NULL;
        counter_mode = e && !strcmp(e, "cpu") ? 0 : e && !strcmp(e, "frozen") ? 1 : e && !strcmp(e, "legacy") ? 3 : 2;
        if (counter_mode == 2 && !g_vk.props.limits.timestampComputeAndGraphics) counter_mode = 0;
    }
    extern bool g_za_frame_hook;
    if (!env && g_za_frame_hook && !g_settings.game_drs) return 1;
    return counter_mode;
}

// ---- frame pacing (runtime/nvn/pace.c): the game's 30 fps mode when frames don't fit 60, through these counters
static void cq_resolve(int i) {  // (cq_lock held) submission slot i is done: its counters get their GPU times
    CounterQ *q = &cq[i];
    if (!q->n) return;
    uint64_t t[CQ_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, q->pool, 0, q->n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod;
        int64_t cand = (int64_t)plat_now_ns() - (int64_t)((double)t[q->n - 1] * per);
        if (!cq_calibrated || cand < cq_offset) cq_offset = cand, cq_calibrated = true;
        static int log = -1;  // LARECOMP_LOG_COUNTER=1: the host (provisional) and GPU times of each counter
        if (log < 0) log = ENV("LARECOMP_LOG_COUNTER") != NULL;
        for (uint32_t k = 0; k < q->n; k++) {
            uint64_t gpu = ns_to_gpu_ticks((uint64_t)((int64_t)((double)t[k] * per) + cq_offset));
            if (log) LOG("[counter] seq %llu #%u at %llx host %.3f gpu %.3f ms\n", (unsigned long long)q->seq, k,
                         (unsigned long long)(uintptr_t)q->out[k], q->out[k][1] * 625.0 / 384 / 1e6, gpu * 625.0 / 384 / 1e6);
            if (overlap_on()) {
                // kept host side: the pair's span becomes the next end counter's reading (the game has read this pair
                // already - its fence wait returned a frame early - and may have reused the slot for the next frame)
                if (cq_last_out && q->out[k] - cq_last_out == 2 && gpu >= cq_last_ticks && gpu - cq_last_ticks < 614400000ull)
                    atomic_store(&g_last_gpu_frame_ticks, gpu - cq_last_ticks);  // (an end counter 16 bytes after its start; under a second)
                cq_last_out = q->out[k], cq_last_ticks = gpu;
            } else {
                q->out[k][1] = pace_end_counter(k ? q->out[k - 1] : NULL, q->out[k], gpu);
            }
        }
        stat_counters_gpu += q->n;
    }
    q->n = 0;
}

static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done, in submission order
    pthread_mutex_lock(&cq_lock);
    for (;;) {
        int best = -1;
        for (int i = 0; i < SUBS; i++)
            if (cq[i].n && cq[i].seq <= done && (best < 0 || cq[i].seq < cq[best].seq)) best = i;
        if (best < 0) break;
        cq_resolve(best);
    }
    pthread_mutex_unlock(&cq_lock);
}

// ---- GPU timer (dynamic resolution, runtime/nvn/output_scale.c; and LARECOMP_VK_PROF's "gpu"): a timestamp at the
// start and the end of each submission's main command buffer, read when its ring slot comes round again (so a few
// submissions late; the controller averages half a second). Submissions overlap a little (the next one's commands
// may start before the previous one's end): their union is counted, the time the GPU was busy.
// The window image's submission (device.c: the game's image drawn into the window - FSR / scaling - and the menu) waits
// for the image, which can be a vsync's wait, so its start timestamp isn't its work's: it has a 3rd timestamp, written
// after the barrier that waits for the image (vk_gpu_timer_window_start), and its time is counted from there - in a
// total of its own (vk_gpu_split_ns): device.c adds it to what dynamic resolution sees unless
// LARECOMP_DYNRES_COUNT_PRESENT=0. Also per frame: the span from the GPU starting the frame's first game submission
// to the end of its window submission; span - game - window = the GPU idle / doing work no timestamp covers
// (uploads ahead of a submission's main command buffer, waiting for the window image).
static uint64_t gpu_total_ns, gpu_last_end;  // ns counted; (ticks) the latest submission end seen
static uint64_t gpu_win_ns, gpu_span_ns, gpu_frames, span_from;  // window submissions: ns, frame spans' ns, count; (ticks)
static bool gpu_timer_on;

static void gpu_timer_add(const uint64_t *t, bool skip, bool win) {
    double per = g_vk.props.limits.timestampPeriod;
    uint64_t t0 = t[0], t1 = t[1], tw = win ? t[2] : 0;
    if (g_vk.ts_bits < 64) { uint64_t m = (1ull << g_vk.ts_bits) - 1; t0 &= m, t1 &= m, tw &= m; }
    if (t1 < t0 || (t1 - t0) * per > 1e9) { span_from = 0; return; }  // (wrapped, or nonsense)
    if (gpu_last_end > t0 && (gpu_last_end - t0) * per > 1e9) gpu_last_end = 0;  // (the counter wrapped)
    if (span_from > t1 || (span_from && (t1 - span_from) * per > 1e9)) span_from = 0;
    uint64_t prev_end = gpu_last_end;
    uint64_t from = t0 > gpu_last_end ? t0 : gpu_last_end;
    if (t1 > gpu_last_end) gpu_last_end = t1;
    if (win) {  // from after the image was acquired (and the work before it ended), to the end
        if (tw >= t0 && tw <= t1) {
            uint64_t wf = tw > prev_end ? tw : prev_end;
            if (t1 > wf) {
                uint64_t ns = (uint64_t)((double)(t1 - wf) * per);
                __atomic_add_fetch(&gpu_win_ns, ns, __ATOMIC_RELAXED);
                if (prof) prof_win += ns;
            }
        }
        if (span_from && t1 > span_from) {
            uint64_t ns = (uint64_t)((double)(t1 - span_from) * per);
            __atomic_add_fetch(&gpu_span_ns, ns, __ATOMIC_RELAXED);
            __atomic_add_fetch(&gpu_frames, 1, __ATOMIC_RELAXED);
            if (prof) prof_span += ns, prof_wframes++;
        }
        span_from = 0;
        return;
    }
    if (!span_from && !skip) span_from = from;  // the frame's first game submission (from where the GPU got to it)
    if (skip || t1 <= from) return;
    uint64_t ns = (uint64_t)((double)(t1 - from) * per);
    __atomic_add_fetch(&gpu_total_ns, ns, __ATOMIC_RELAXED);
    if (prof) prof_gpu += ns;
}

// GPU time of all counted submissions so far (ns); -1: no GPU timer on this device
int64_t vk_gpu_total_ns(void) { return gpu_timer_on ? (int64_t)__atomic_load_n(&gpu_total_ns, __ATOMIC_RELAXED) : -1; }
// (cumulative) out: game submissions' GPU ns, window submissions' GPU ns, frame spans' ns, frames with a span.
// false: no GPU timer.
bool vk_gpu_split_ns(uint64_t out[4]) {
    if (!gpu_timer_on) return false;
    out[0] = __atomic_load_n(&gpu_total_ns, __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&gpu_win_ns, __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&gpu_span_ns, __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&gpu_frames, __ATOMIC_RELAXED);
    return true;
}
// Frame pacing (pace.c): GPU time so far, the game's submissions and the window image's; -1: no GPU timer.
int64_t pace_gpu_ns(void) {
    uint64_t t[4];
    return vk_gpu_split_ns(t) ? (int64_t)(t[0] + t[1]) : -1;
}
uint64_t vk_cum_wait_ns(void) { return __atomic_load_n(&cum_wait, __ATOMIC_RELAXED); }  // (all vk_wait_seq waits so far)

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        wait_why = "ring slot";
        uint64_t tw = prof ? SDL_GetTicksNS() : 0;
        vk_wait_seq(s->seq);
        if (prof) prof_slot_wait += SDL_GetTicksNS() - tw;
        cq_resolve_done(s->seq);
        if (s->ts) {  // GPU time of that submission
            uint64_t t[3];
            uint32_t nq = s->ts_win ? 3 : 2;
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 3, nq, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                gpu_timer_add(t, s->ts_skip, s->ts_win);
            s->ts = s->ts_skip = s->ts_win = false;
        }
        rec_reset_pools(cur_sub);  // (its sets went with the submission; the recorder resets in order)
        pthread_mutex_lock(&seq_lock);
        s->main_begun = s->upload_begun = false;
        s->seq = submit_seq + 1;
        pthread_mutex_unlock(&seq_lock);
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    rec_BeginCommandBuffer(*cb);
    pthread_mutex_lock(&seq_lock);
    *begun = true;
    pthread_mutex_unlock(&seq_lock);
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        ss_forget();  // (a new command buffer starts with no state)
        if (vk_counter_mode() == 2) {  // this slot's timing counter queries (reset outside any render pass)
            CounterQ *q = &cq[cur_sub];
            if (!q->pool) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                            .queryCount = CQ_MAX};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &q->pool) != VK_SUCCESS) q->pool = VK_NULL_HANDLE;
            }
            pthread_mutex_lock(&cq_lock);
            q->n = 0;  // (resolved when the slot was freed: sub())
            q->seq = s->seq;
            pthread_mutex_unlock(&cq_lock);
            if (q->pool) rec_CmdResetQueryPool(s->main, q->pool, 0, CQ_MAX);
        }
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof_qp) {
            rec_CmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 3, 3);
            rec_CmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 3);
            s->ts = true;
        }
    }
    return s->main;
}

// (device.c vk_frame_begin) the window image's submission: its work starts here, after the barrier that waits for the
// image (ALL_COMMANDS: after it, so not while the GPU waits for the image)
void vk_gpu_timer_window_start(void) {
    Sub *s = sub();
    if (!s->ts || s->ts_win) return;
    rec_CmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 3 + 2);
    s->ts_win = true;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    if (!s->upload_begun) {
        begin_cb(&s->upload, &s->upload_begun);
        // after everything submitted before it: a submission's commands may start while the previous one's still run,
        // and these copies overwrite what those may read (textures being replaced, vertex cache pages)
        vk_full_barrier(s->upload);
    }
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);
void vc_flush(void);
static void xm_copy_back(VkCommandBuffer cb);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
static char gp_label[512][112];
static uint32_t gp_draws[512];
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
#define DL_N 4096  // (a power of two)
static char dl_ring[DL_N][128];
static uint64_t dl_args[DL_N];
static uint32_t dl_snap[DL_N][5];  // (those parameters when the draw was recorded)  // a GPU indirect draw's parameters (guest = host address): read back in the dump
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = ENV("LARECOMP_VK_DEVLOST_TRACE") ? atoi(ENV("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    dl_args[dl_n & (DL_N - 1)] = 0;
    vsnprintf(dl_ring[dl_n++ & (DL_N - 1)], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) {
        const uint32_t *q = (const uint32_t *)(uintptr_t)dl_args[i & (DL_N - 1)];
        if (q) LOG("[vk]   %s (args now: %u %u %u %d %u)\n", dl_ring[i & (DL_N - 1)], q[0], q[1], q[2], (int)q[3], q[4]);
        else LOG("[vk]   %s\n", dl_ring[i & (DL_N - 1)]);
    }
    // VK_NV_device_diagnostic_checkpoints: the last draw / dispatch each pipeline stage reached
    extern uint32_t vk_checkpoints(uint64_t *mark, uint32_t *stage, uint32_t max);
    uint64_t mark[16];
    uint32_t stage[16], n = vk_checkpoints(mark, stage, 16);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t m = mark[i];
        LOG("[vk] checkpoint stage %#x: #%llu %s\n", stage[i], (unsigned long long)m,
            m && m <= dl_n && dl_n - m < DL_N ? dl_ring[(m - 1) & (DL_N - 1)] : "(out of the trace)");
    }
    // the operations from the oldest checkpoint on (in flight when the GPU stopped), with indirect parameters
    uint64_t lo = ~0ull;
    for (uint32_t i = 0; i < n; i++) if (mark[i] && mark[i] < lo) lo = mark[i];
    if (lo != ~0ull && dl_n - lo < DL_N)
        for (uint64_t m = lo; m <= dl_n && m < lo + 48; m++) {
            const uint32_t *q = (const uint32_t *)(uintptr_t)dl_args[(m - 1) & (DL_N - 1)];
            const uint32_t *w = dl_snap[(m - 1) & (DL_N - 1)];
            if (q) LOG("[vk]   #%llu %s (args %llx now: %u %u %u %d %u, recorded: %u %u %u %d %u)\n", (unsigned long long)m,
                       dl_ring[(m - 1) & (DL_N - 1)], (unsigned long long)(uintptr_t)q, q[0], q[1], q[2], (int)q[3], q[4],
                       w[0], w[1], w[2], (int)w[3], w[4]);
            else LOG("[vk]   #%llu %s\n", (unsigned long long)m, dl_ring[(m - 1) & (DL_N - 1)]);
        }
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= 511) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 512};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) rec_CmdResetQueryPool(cb, gp_qp, 0, 512);
    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    uint64_t t[512];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
        }
    }
    gp_n = 0;
}

// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    vk_tex_unpack_finish();  // (the upload copies recorded so far read what it writes)
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    if (s->main_begun) xm_copy_back(s->main);  // (transform feedback output, to the heap)
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    vc_flush();
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        rec_EndCommandBuffer(s->upload);
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    s->ts_skip = wait != VK_NULL_HANDLE;
    if (s->ts_win && !s->ts_skip) s->ts_win = false;  // (not the window image's after all: counted as game work)
    if (s->ts) rec_CmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 3 + 1);
    rec_EndCommandBuffer(s->main);
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    dl_note("submit %llu", (unsigned long long)submit_seq + 1);
    rec_QueueSubmit2(&si, submit_seq + 1);  // (the recorder: in order with the commands; its vkQueueSubmit2 time is in RecStats)
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    cur_sub = (cur_sub + 1) % SUBS;
    pthread_mutex_unlock(&seq_lock);
    passes_in_sub = 0;
    stat_flushes++;
    cum_submits++;
    submit_draws = cum_draws;
    ring_retire();
    vk_collect_dead();
    if (signal) gc_frame();  // (a present: once a frame)
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }

static void overlap_clear(void) {  // (a full wait was made: everything queued so far is done)
    pthread_mutex_lock(&seq_lock);
    deferred_seq = 0;
    pthread_mutex_unlock(&seq_lock);
}

void renderer_flush(void) {
    renderer_submit();
    if (!wait_why) wait_why = "flush";
    vk_wait_seq(submit_seq);
    overlap_clear();
}

// The latest submission the driver has (rqueue.h): with the recorder, behind renderer_issued_seq until it catches up.
uint64_t renderer_submit_seq(void) { return rec_submitted(); }

// The latest submission this backend queued (rqueue.c: an item's work ends with it; renderer_wait_seq waits for the
// recorder to hand it to the driver, then for the GPU)
uint64_t renderer_issued_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    return s;
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = "QueueFinish / sync all";
    vk_wait_seq(s);
    overlap_clear();
}

// (overlap) The game's SyncWait on submission seq: done is the latest complete submission, *deferred the one a previous
// call returned early for (0: none). Returns the submission to wait for now (0: none); updates *deferred. A fence
// that is complete is never waited for; a newer fence than the deferred one waits for the deferred one and takes its
// place; the deferred fence itself, or an older one, is waited for. (tools/test_vulkan_overlap.py)
static uint64_t overlap_defer(uint64_t seq, uint64_t done, uint64_t *deferred) {
    uint64_t prev = *deferred;
    if (seq <= done) {  // complete (in order: a deferred one no newer than it is complete too)
        if (seq >= prev) *deferred = 0;
        return 0;
    }
    if (seq > prev) {  // a newer fence: the deferred one is waited for now, this one at the next call
        *deferred = seq;
        return prev > done ? prev : 0;
    }
    if (seq == prev) *deferred = 0;  // the deferred fence again, or an older one: a real wait on it
    return seq;
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    Sub *s = &subs[cur_sub];
    // A reused slot still has its old begun flags until sub() prepares it. An empty queue fence must refer to
    // the last submission, not a future value that submit's empty-work fast path will never signal.
    bool pending = s->seq == submit_seq + 1 && (s->main_begun || s->upload_begun);
    uint64_t q = submit_seq + (pending ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = seq > s ? "SyncWait (fence not submitted yet)" : "SyncWait";
    if (seq > s) seq = s;  // not queued yet (other thread): everything queued so far (vk_wait_seq: the recorder, then the GPU)
    if (overlap_on()) {  // (see deferred_seq) wait for the fence deferred last time, defer this one
        uint64_t done = vk_completed_seq();  // (the timeline's value: a submission the recorder hasn't made yet isn't done)
        pthread_mutex_lock(&seq_lock);
        uint64_t w = overlap_defer(seq, done, &deferred_seq);
        pthread_mutex_unlock(&seq_lock);
        if (!w) {
            __atomic_fetch_add(&stat_deferred, 1, __ATOMIC_RELAXED);  // (the game thread; stats read on the render thread)
            wait_why = NULL;
            cq_resolve_done(done);  // (the game reads its timing counters now: what is complete so far)
            return;
        }
        seq = w;
    }
    vk_wait_seq(seq);
}

// A descriptor set for the pending submission (record.c: the slot's pools, allocated in batches per layout). With
// the recorder on it is a stand-in, to be written and bound through rec_* only, and only for this submission.
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    sub();
    return rec_alloc_set(cur_sub, layout);
}

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed. Without resizable BAR the device-local host-visible memory is a
// small window (256 MB on a GTX 1060): once it's full, later chunks go to system memory (issue #45: it stopped the
// game with "out of device memory").
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    static bool sysmem;  // (the device-local window ran out: system memory from then on)
    // Without resizable BAR the device-local host-visible window is ~256 MB, shared with other programs and the driver:
    // two 64 MB chunks took 128 of its 132 MB in use while its budget fell to 81 MB (RTX 3080 Ti), and a GTX 1060 ran
    // out (issue #45). A chunk goes there only while the window's use stays under half its budget; past that, system
    // memory (~1 ms more GPU time a frame on a 3080 Ti at 1x). The first chunks, which most frames use, stay fast.
    // LARECOMP_VK_RING_BAR=1: no budget check.
    bool spill = false;
    if (!sysmem && !getenv("LARECOMP_VK_RING_BAR")) {
        uint32_t type;
        if (vmaFindMemoryTypeIndexForBufferInfo(g_vk.vma, &bi, &ai, &type) == VK_SUCCESS) {
            const VkPhysicalDeviceMemoryProperties *mp;
            vmaGetMemoryProperties(g_vk.vma, &mp);
            uint32_t heap = mp->memoryTypes[type].heapIndex;
            VmaBudget bud[VK_MAX_MEMORY_HEAPS];
            vmaGetHeapBudgets(g_vk.vma, bud);
            spill = (mp->memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                    bud[heap].usage + size > bud[heap].budget / 2;
            static bool logged;
            if (spill && !logged) {
                logged = true;
                LOG("[vk] transient ring: device-local window %llu of %llu MB budget in use: new chunks in system memory\n",
                    (unsigned long long)(bud[heap].usage >> 20), (unsigned long long)(bud[heap].budget >> 20));
            }
        }
    }
    if (sysmem || spill || getenv("LARECOMP_VK_RING_SYSMEM")) ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;  // (test: a GPU without resizable BAR)
    VmaAllocationInfo info;
    VkResult r = vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info);
    bool moved = false;
    // (a driver may report the full window as out of host memory too; if memory is really gone, the retry fails as well)
    if ((r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) && ai.usage != VMA_MEMORY_USAGE_AUTO_PREFER_HOST) {
        LOG("[vk] transient ring: device-local memory full after %d chunks, system memory from now on\n", nchunks);
        vk_log_memory("ring chunk failed");
        sysmem = moved = true;
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        r = vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info);
    }
    if (r != VK_SUCCESS) {  // (system memory too: vk_transient waits for a chunk to come free instead - issue #45)
        LOG("[vk] transient ring: no memory for chunk %d (%llu MB, error %d)\n", nchunks, (unsigned long long)(size >> 20), r);
        vk_log_memory("ring chunk failed");
        return -1;
    }
    c->ptr = info.pMappedData;
    c->size = size;
    static bool spilled;  // (the first chunk in system memory: logged too)
    if (!nchunks || moved || (spill && !spilled)) {  // where the ring lives: device-local (resizable BAR) or system memory read across PCIe
        if (spill) spilled = true;
        VkMemoryPropertyFlags f;
        vmaGetMemoryTypeProperties(g_vk.vma, info.memoryType, &f);
        LOG("[vk] transient ring: memory type %u (%s%s%s)\n", info.memoryType, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local " : "system memory ",
            f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "host-visible " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "cached" : "");
    }
    return nchunks++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    if (pick < 0) {  // no memory for another chunk (a GTX 1060 with 6 GB: issue #45): wait for the work already submitted,
        // which frees the chunks only it used, and take one of those (the recording in progress keeps its own)
        vk_wait_seq(submit_seq);
        done = vk_completed_seq();
        for (int i = 0; i < nchunks; i++)
            if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
        if (pick < 0) FATAL("the graphics card ran out of memory for uploads (%llu MB needed, %d chunks in use)\n",
                            (unsigned long long)(size >> 20), nchunks);
        static int nlog;
        if (nlog++ < 5) LOG("[vk] transient ring: reused chunk %d after waiting for the GPU\n", pick);
    }
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_SLOTS };  // (the driver constants: cbuf_info)
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, seq; VkSpan span; uint64_t scanned; } gcopy[GCOPY_TAB];  // (scanned: heap size then, 0 not)
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
// scan: (lazy heap chunks, a uniform buffer of a shader with global memory) the range is also scanned for heap
// pointers (gc_scan_pointers), once per shared copy: its contents can't change while the copy is shared (again if the
// heap grew since: pointers past its end were passed over).
static bool gc_scan_needed(void);
static uint64_t prof_scan_calls, prof_scan_bytes, prof_scan_skipped;  // (VK_PROF, lazy chunks) pointer scans
static VkSpan guest_copy_ex(uint64_t addr, uint64_t size, bool scan) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    bool hit = gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size;
    uint64_t heap = 0;
    if (scan) {
        heap = gmem_heap_size();
        if ((!hit || gcopy[slot].scanned != heap) && gc_scan_needed()) gc_scan_pointers((const uint32_t *)(uintptr_t)addr, size);
        else prof_scan_skipped++;
    }
    if (hit) {
        if (scan) gcopy[slot].scanned = heap;
        return gcopy[slot].span;
    }
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s; gcopy[slot].scanned = heap;
    return s;
}
static VkSpan guest_copy(uint64_t addr, uint64_t size) { return guest_copy_ex(addr, size, false); }

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
// state: what the build sets when done (a draw waiting on it can run it itself: run_queued_now)
typedef struct Job { void (*fn)(void *); void *arg; _Atomic int *state; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;
// when each thread's current job started (0: idle) - wait_built says which one a stalled draw is stuck behind
#define MAX_WORKERS 16
static _Atomic uint64_t worker_t0[MAX_WORKERS], translator_t0;
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

static void *worker(void *a) {
    int me = (int)(intptr_t)a;
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
static Job *queued_job(_Atomic int *state, bool take) {
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
            return j;
        }
        pthread_mutex_unlock(lock);
    }
    return NULL;
}
static bool run_queued_now(_Atomic int *state) {
    Job *j = queued_job(state, true);
    if (!j) return false;
    j->fn(j->arg);
    free(j);
    return true;
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;  // &comp, or NULL with no stage bound
    NvnProgram comp;      // the bound stages: BindProgram(program, stages) replaces just those
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint8_t depth_clamp;  // nvnCommandBufferSetDepthClamp
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} st;

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();
    if (last_t && now - last_t > 50000000) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending\n", (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

// (the slow-frame log, slowframe.h; the game thread) running totals this file keeps anyway
void renderer_slow_counters(uint64_t *submits, uint64_t *gpu_wait_ns, uint64_t *imports) {
    *submits = __atomic_load_n(&cum_submits, __ATOMIC_RELAXED);
    *gpu_wait_ns = __atomic_load_n(&cum_wait, __ATOMIC_RELAXED);
    *imports = (uint64_t)__atomic_load_n(&gc_imported, __ATOMIC_RELAXED);
}

// LARECOMP_LOG_SKIPS=1: the skipped draws and dispatches of each stats interval by reason, and the top offenders
// (shader code addresses and content hashes, as in "[shader] slow build" / shader_variants.txt, and how long they
// have been skipped without a break). "building" for tens of seconds is a build that never finishes.
enum { SK_NO_PROGRAM, SK_NO_STAGES, SK_PROG_BUILDING, SK_PROG_FAILED, SK_PIPE_BUILDING, SK_PIPE_FAILED, SK_CS_BUILDING,
       SK_CS_FAILED, SK_TOPOLOGY, SK_DEBUG_SKIP, SK_HIDDEN, SK_N };
static const char *const skip_names[SK_N] = {
    "no program", "program missing a stage", "program building", "program failed", "pipeline building", "pipeline failed",
    "compute building", "compute failed", "topology", "debug skip list", "hidden on purpose"};
static int log_skips = -1;  // (-1: not read yet: the first skip reads it)
static uint64_t skip_n[SK_N];
#define SKIP_TOP 32
static struct { int why; uint64_t a, b, ha, hb, n, first_ns; } skip_top[SKIP_TOP];
static int skip_ntop;
static void skip_note(int why, bool compute) {
    if (log_skips < 0) log_skips = getenv("LARECOMP_LOG_SKIPS") && atoi(getenv("LARECOMP_LOG_SKIPS")) != 0;
    if (!log_skips) return;
    skip_n[why]++;
    const NvnProgram *p = st.program;
    uint64_t a = p ? p->stages[compute ? 5 : 0].code_addr : 0, b = p && !compute ? p->stages[1].code_addr : 0;
    int j = 0;
    while (j < skip_ntop && !(skip_top[j].why == why && skip_top[j].a == a && skip_top[j].b == b)) j++;
    if (j == skip_ntop) {
        if (skip_ntop < SKIP_TOP) skip_ntop++;
        else {  // (full: the entry skipped least this interval goes)
            j = 0;
            for (int i = 1; i < SKIP_TOP; i++) if (skip_top[i].n < skip_top[j].n) j = i;
        }
        skip_top[j].why = why; skip_top[j].a = a; skip_top[j].b = b; skip_top[j].n = 0;
        skip_top[j].ha = p ? shader_stage_hash(&p->stages[compute ? 5 : 0]) : 0;
        skip_top[j].hb = p && !compute ? shader_stage_hash(&p->stages[1]) : 0;
        skip_top[j].first_ns = SDL_GetTicksNS();
    }
    skip_top[j].n++;
}
#define SKIP(why) do { stat_skipped++; if (log_skips) skip_note(why, false); } while (0)
#define SKIP_CS(why) do { stat_skipped++; if (log_skips) skip_note(why, true); } while (0)
static void skip_log(void) {
    if (log_skips <= 0) return;
    uint64_t total = 0;
    for (int i = 0; i < SK_N; i++) total += skip_n[i];
    if (total) {
        LOG("[skips] %llu:", (unsigned long long)total);
        for (int i = 0; i < SK_N; i++) if (skip_n[i]) LOG(" %s %llu", skip_names[i], (unsigned long long)skip_n[i]);
        uint64_t now = SDL_GetTicksNS();
        bool shown[SKIP_TOP] = {0};
        for (int t = 0; t < 4; t++) {  // the top offenders of this interval
            int best = -1;
            for (int i = 0; i < skip_ntop; i++)
                if (skip_top[i].n && !shown[i] && (best < 0 || skip_top[i].n > skip_top[best].n)) best = i;
            if (best < 0) break;
            shown[best] = true;
            bool cs = skip_top[best].why == SK_CS_BUILDING || skip_top[best].why == SK_CS_FAILED;
            LOG("%s %s: %s %llx (%016llx)", t ? "," : " | top", skip_names[skip_top[best].why], cs ? "cs" : "vs",
                (unsigned long long)skip_top[best].a, (unsigned long long)skip_top[best].ha);
            if (!cs) LOG(" fs %llx (%016llx)", (unsigned long long)skip_top[best].b, (unsigned long long)skip_top[best].hb);
            LOG(" x%llu, for %llu s", (unsigned long long)skip_top[best].n,
                (unsigned long long)((now - skip_top[best].first_ns) / 1000000000ull));
        }
        LOG("\n");
    }
    memset(skip_n, 0, sizeof skip_n);
    int k = 0;  // (an offender not skipped this interval is dropped: its "for N s" starts again if it comes back)
    for (int i = 0; i < skip_ntop; i++)
        if (skip_top[i].n) { skip_top[k] = skip_top[i]; skip_top[k++].n = 0; }
    skip_ntop = k;
}

// Builds in flight (programs, graphics / compute pipelines): one still running after 10 s is logged once, with its
// shaders' code addresses. "[shader] slow build" only comes when a build finishes, so one that never does (its draws
// or dispatches skipped for good, compile 0 in the stats) left no trace.
#define BUILDS_MAX 64
static pthread_mutex_t builds_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { const char *what; uint64_t c0, c1, t0; bool logged; } builds[BUILDS_MAX];
static int build_begin(const char *what, uint64_t c0, uint64_t c1) {
    pthread_mutex_lock(&builds_lock);
    int s = 0;
    while (s < BUILDS_MAX && builds[s].what) s++;
    if (s < BUILDS_MAX) { builds[s].what = what; builds[s].c0 = c0; builds[s].c1 = c1; builds[s].t0 = SDL_GetTicksNS(); builds[s].logged = false; }
    pthread_mutex_unlock(&builds_lock);
    return s < BUILDS_MAX ? s : -1;
}
static void build_end(int s) {
    if (s < 0) return;
    pthread_mutex_lock(&builds_lock);
    builds[s].what = NULL;
    pthread_mutex_unlock(&builds_lock);
}
static void builds_check(void) {
    uint64_t now = SDL_GetTicksNS();
    pthread_mutex_lock(&builds_lock);
    for (int s = 0; s < BUILDS_MAX; s++) {
        if (!builds[s].what || builds[s].logged || now - builds[s].t0 < 10000000000ull) continue;
        builds[s].logged = true;
        LOG("[shader] still building after %llu s: %s %llx %llx\n", (unsigned long long)((now - builds[s].t0) / 1000000000ull),
            builds[s].what, (unsigned long long)builds[s].c0, (unsigned long long)builds[s].c1);
    }
    pthread_mutex_unlock(&builds_lock);
}

const char *renderer_stats(void) {
    skip_log();
    builds_check();
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d, fbcopy %llu, upload %.1fMB, "
             "vertices %.0fMB cached %.0fMB host, xfb sessions %llu, scene waits %.0fms, deferred %llu, snap %lluKB/%llu",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0,
             (double)stat_vtx_cached / 1048576.0, (double)stat_vtx_host / 1048576.0, (unsigned long long)stat_xfb_sessions,
             (double)stat_scene_wait_ns / 1e6, (unsigned long long)__atomic_exchange_n(&stat_deferred, 0, __ATOMIC_RELAXED),
             (unsigned long long)(stat_snap_bytes >> 10), (unsigned long long)stat_snap_copies);  // (overlap: SyncWaits returned early; storage copies made)
    stat_scene_wait_ns = 0;
    stat_feedback_copies = stat_upload_bytes = stat_snap_bytes = stat_snap_copies = 0;
    stat_vtx_cached = stat_vtx_host = stat_xfb_sessions = 0;
    if (ENV("LARECOMP_SKIP_WHY")) {
        LOG("[vk] skipped by line:");
        for (int i = 0; i < 4096; i++) if (skip_why[i]) LOG(" %d:%u", i, skip_why[i]);
        LOG("\n");
    }
    memset(skip_why, 0, sizeof skip_why);
    {
        extern void vk_alias_stats(void);
        vk_alias_stats();
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        RecStats rs;  // (the recorder's counters for this interval: record.c)
        rec_stats_take(&rs);
        prof_submit = rs.submit_ns;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        if (gc_shift) {
            snprintf(stats_text + n, sizeof stats_text - n, " | heap chunks %d imported (%llu MB), %llu misses", gc_imported,
                     (unsigned long long)gc_imported_bytes >> 20, (unsigned long long)stat_gc_misses);
            n = strlen(stats_text);
            if (gc_lazy) {
                snprintf(stats_text + n, sizeof stats_text - n, ", pointer scans %llu (%.2f MB), %llu skipped",
                         (unsigned long long)prof_scan_calls, prof_scan_bytes / 1048576.0, (unsigned long long)prof_scan_skipped);
                n = strlen(stats_text);
            }
            prof_scan_calls = prof_scan_bytes = prof_scan_skipped = 0;
        }
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu | counters gpu %llu | pace %s, %llu frames over", (unsigned long long)prof_views_created,
                 (unsigned long long)stat_counters_gpu, pace_is_slow() ? "30" : "60", (unsigned long long)pace_frames_over());
        stat_counters_gpu = 0;
        {
            extern uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n,  " | vcache %llu hit %llu in place %.1f MB copied, %llu queries %.2f ms, %llu pages written",
                     (unsigned long long)prof_vc_hits, (unsigned long long)prof_vc_bypass, prof_vc_copied / 1048576.0,
                     (unsigned long long)prof_vc_queries, prof_vc_scan_ns / 1e6, (unsigned long long)prof_vc_scan_pages);
            prof_vc_hits = prof_vc_bypass = prof_vc_copied = prof_vc_queries = prof_vc_scan_pages = prof_vc_scan_ns = 0;
            extern uint64_t prof_tex_watch_skips, prof_tex_watch_written, prof_tex_watch_unknown, prof_tex_watch_gpu;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | tex unwritten %llu written %llu unknown %llu (GPU pages %llu)",
                     (unsigned long long)prof_tex_watch_skips, (unsigned long long)prof_tex_watch_written,
                     (unsigned long long)prof_tex_watch_unknown, (unsigned long long)prof_tex_watch_gpu);
            prof_tex_watch_skips = prof_tex_watch_written = prof_tex_watch_unknown = prof_tex_watch_gpu = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | const blocks %s: %llu reused %llu copied", cbuf_lean() ? "lean" : "full",
                     (unsigned long long)prof_cb_reused, (unsigned long long)prof_cb_copied);
            prof_cb_reused = prof_cb_copied = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | hook pass %llu draws, %llu with known textures",
                     (unsigned long long)prof_hook_draws, (unsigned long long)prof_hook_known);
            prof_hook_draws = prof_hook_known = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | key memo %s: program %llu pipeline %llu of %llu", key_memo_on() ? "on" : "off",
                     (unsigned long long)prof_prog_memo, (unsigned long long)prof_pipe_memo, (unsigned long long)prof_key_draws);
            prof_prog_memo = prof_pipe_memo = prof_key_draws = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | sets (reuse %s, batch %s, recycle %s): %llu reused %llu written, %llu allocated"
                     " (%llu new from the pools in %llu calls), pool resets %llu %.2f ms%s",
                     set_reuse_on() ? "on" : "off", rec_set_batch() ? "on" : "off", rec_set_recycle() ? "on" : "off",
                     (unsigned long long)prof_sets_reused, (unsigned long long)prof_sets_new, (unsigned long long)rs.set_allocs,
                     (unsigned long long)rs.sets_fresh, (unsigned long long)rs.set_calls, (unsigned long long)rs.pool_resets,
                     rs.pool_reset_ns / 1e6, rec_on() ? " (on the recorder)" : "");
            prof_sets_reused = prof_sets_new = 0;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | slot wait %.1f ms", prof_slot_wait / 1e6);
            prof_slot_wait = 0;
            n = strlen(stats_text);
            // the recorder thread (record.c): packets queued, the translating thread's time queuing them, the recorder's
            // time making the calls, and the waits on it (drain: ImGui / idle waits; seq: GPU waits that had to wait for a
            // submission to be handed to the driver first; present: the next frame's acquire waiting for the last present;
            // full: the ring full)
            snprintf(stats_text + n, sizeof stats_text - n, " | recorder %s: %llu packets (%.1f MB) enqueue %.1f ms, busy %.1f ms (%llu wakes), waits:"
                     " drain %llux %.1f ms, seq %llux %.1f ms, present %llux %.1f ms, full %llux %.1f ms, pipes %llux %.1f ms, dropped %llu",
                     rec_on() ? "on" : "off", (unsigned long long)rs.packets, rs.bytes / 1048576.0, rs.enqueue_ns / 1e6, rs.busy_ns / 1e6,
                     (unsigned long long)rs.wakeups, (unsigned long long)rs.drains, rs.drain_ns / 1e6, (unsigned long long)rs.seq_waits,
                     rs.seq_wait_ns / 1e6, (unsigned long long)rs.present_waits, rs.present_wait_ns / 1e6, (unsigned long long)rs.full_waits,
                     rs.full_ns / 1e6, (unsigned long long)rs.pipe_waits, rs.pipe_wait_ns / 1e6, (unsigned long long)rs.draws_dropped);
        }
        {   // GPU time per frame by part (see gpu_timer_add), CPU waits per frame by place (device.c g_vk_frame_waits)
            static uint64_t lw[4];
            uint64_t dw[4];
            for (int i = 0; i < 4; i++) {  // ([2], [3]: the recorder's)
                uint64_t v = __atomic_load_n(&g_vk_frame_waits[i], __ATOMIC_RELAXED);
                dw[i] = v - lw[i], lw[i] = v;
            }
            double nf = prof_wframes ? (double)prof_wframes : 1, np = dw[3] ? (double)dw[3] : 1;
            double gap = ((double)prof_span - (double)prof_gpu - (double)prof_win) / nf / 1e6;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | gpu/frame game %.1f window %.1f span %.1f gap %.1f ms (%llu frames)"
                     " | cpu wait/frame slot %.1f acquire %.1f present %.1f ms",
                     prof_gpu / nf / 1e6, prof_win / nf / 1e6, prof_span / nf / 1e6, prof_wframes ? gap : 0.0,
                     (unsigned long long)prof_wframes, dw[0] / np / 1e6, dw[1] / np / 1e6, dw[2] / np / 1e6);
            prof_win = prof_span = prof_wframes = 0;
        }
        pthread_mutex_lock(&wait_tab_lock);
        for (int i = 0; i < 12 && wait_tab[i].why; i++) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "%s%s %llux %.0f ms", i ? "," : " | waits:", wait_tab[i].why,
                     (unsigned long long)wait_tab[i].n, wait_tab[i].ns / 1e6);
        }
        memset(wait_tab, 0, sizeof wait_tab);
        pthread_mutex_unlock(&wait_tab_lock);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

static VkPipeline pipe_resolve(void *obj, bool wait);
void vk_exec_init(void) {
    maxwell_disk_cache = true;  // (translate.c: the game's translations kept in cache/maxwell)
    prof = getenv("LARECOMP_VK_PROF") != NULL;
    if (prof) prof_up_ns = 0;
    // the GPU timer (LARECOMP_VK_GPU_TIMER=0: off - dynamic resolution then goes by the frame times)
    const char *gt = getenv("LARECOMP_VK_GPU_TIMER");
    if (g_vk.ts_bits && g_vk.props.limits.timestampComputeAndGraphics && g_vk.props.limits.timestampPeriod > 0 && !(gt && !atoi(gt))) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 3};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    gpu_timer_on = prof_qp != VK_NULL_HANDLE;
    LOG("[vk] GPU timer %s (dynamic resolution goes by %s)\n", gpu_timer_on ? "on" : "off", gpu_timer_on ? "GPU time" : "frame times");
    st_defaults();
    // The heap's chunk / lazy decision now, on this thread, before the render thread and the build workers exist:
    // they read gc_shift / gc_lazy directly (a buffer copy can come before any shader was translated)
    gmem_chunk_shift();
    // The command buffers of every slot, from the pool, before the recorder thread exists: from then on the pool and
    // the buffers are the recorder's (begun, recorded, ended and submitted in packet order)
    for (int i = 0; i < SUBS; i++) {
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cbs));
        subs[i].main = cbs[0];
        subs[i].upload = cbs[1];
    }
    rec_set_pipe_resolver(pipe_resolve);
    rec_init(prof != 0);
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
void renderer_texture_fraction(NvnTexture *t, float *fx, float *fy);  // (textures.c: the part the contents occupy)
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static float tex_fr[2][64][2];  // per bound texture slot: the part of the texture the contents occupy (Dynamic)
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h, pass_layers = 1;
static int debug_pass_no;
// (draw_indirect) the draw being made reads its parameters on the GPU: offset in the guest memory window's buffer
static bool g_ind_gpu;
static VkDeviceSize g_ind_off;
static uint32_t g_ind_stride;
// Transform feedback state (Begin/EndTransformFeedback, the bound buffers) and rasterizer discard
static bool xfb_on, raster_discard;
static uint64_t xfb_addr[4], xfb_size[4];
static uint32_t raw_layer;  // (debug frame: the layer of a layered pass that renderer_save_texture_tex saves)

// A draw's vertex layout. Without dynamic vertex input it is copied into the PipeKey (each layout its own pipeline);
// with it (g_vk.vids) the key leaves it out and draw_ sets it (vkCmdSetVertexInputEXT) when it changes.
typedef struct {
    uint32_t attr_mask, used_streams;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t stride[17], divisor[17];  // divisor 0: per vertex; else per instance
} VtxLayout;

// Redundant-state filter: what draw() last set in the current command buffer, so unchanged pipeline binds, dynamic
// state and vertex / index buffer binds are not recorded again (a draw used to issue ~20 vkCmd* calls; most repeat the
// previous draw's). Forgotten (ss_forget) whenever something else may have recorded state: a new command buffer, a new
// render pass (present, ImGui, post-processing and copies all run between game passes) and the clear pipeline.
// Descriptor sets are always bound (sharing identical sets within a submission was tried: a third matched, but
// comparing them cost more than allocating and writing them on NVIDIA; with dynamic uniform buffers most sets repeat
// their program's previous one, which stage_set keeps: "set reuse").
// LARECOMP_VK_STATE_FILTER=0: every draw sets everything, as before.
enum { SS_PIPE = 1 << 0, SS_DT = 1 << 1, SS_DW = 1 << 2, SS_DOP = 1 << 3, SS_STEN = 1 << 4, SS_SFACE0 = 1 << 5, SS_SFACE1 = 1 << 6,
       SS_BC = 1 << 7, SS_CULL = 1 << 8, SS_FF = 1 << 9, SS_TOPO = 1 << 10, SS_PRE = 1 << 11, SS_DBE = 1 << 12, SS_DB = 1 << 13,
       SS_VP = 1 << 14, SS_SC = 1 << 15, SS_IB = 1 << 16, SS_VI = 1 << 17 };
static struct {
    uint32_t known;  // SS_* bits: the value below is what the command buffer has
    uint32_t vb_known;  // per vertex binding
    const void *pipe;   // the Pipe bound (a building one has no handle yet: WAIT_PIPES)
    VkBool32 dt, dw, sten, pre, dbe;
    VkCompareOp dop;
    uint32_t sface[2][7];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float bc[4], db[3];
    VkCullModeFlags cull;
    VkFrontFace ff;
    VkPrimitiveTopology topo;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkBuffer vb[17]; VkDeviceSize vo[17];
    VtxLayout vi;  // (dynamic vertex input)
} ss;
static int ss_on = -1;
static void ss_forget(void) { ss.known = 0; ss.vb_known = 0; }
static bool ss_enabled(void) {
    if (ss_on < 0) { const char *e = ENV("LARECOMP_VK_STATE_FILTER"); ss_on = !e || atoi(e) != 0; }
    return ss_on;
}
// true: the state must be recorded (and is remembered as known)
static inline bool ss_need(uint32_t bit, bool same) {
    if (ss_on && (ss.known & bit) && same) return false;
    ss.known |= bit;
    return true;
}

static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    rec_CmdEndRendering(vk_cmd());
    in_pass = false;
    if (g_debug_frame && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = ENV("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        renderer_save_texture_tex(st.rt.color[0]->backend, path);
        for (int ci = 1; ci < st.rt.ncolor; ci++) {  // (multiple render targets: the others as _c<i>)
            if (!st.rt.color[ci] || !st.rt.color[ci]->backend) continue;
            char cpath[300];
            snprintf(cpath, sizeof cpath, "%.*s_c%d.png", (int)strlen(path) - 4, path, ci);
            renderer_save_texture_tex(st.rt.color[ci]->backend, cpath);
        }
        TexBackend *tb = st.rt.color[0]->backend;
        if (tb->width == 1 && tb->height == 1 && tb->format == VK_FORMAT_R32G32B32A32_SFLOAT) {  // (exposure: the raw value)
            VkSpan out = vk_readback(16);
            VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            rec_CmdCopyImageToBuffer(cb, tb->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
            renderer_flush();
            const float *f = out.ptr;
            LOG("[vk] %s 1x1 = %g %g %g %g\n", path, f[0], f[1], f[2], f[3]);
        }
        uint32_t lys[3] = {1, pass_layers / 2, pass_layers - 1};  // layered: also layers 1, middle, last
        for (int k = 0; k < 3 && pass_layers > 1; k++) {
            uint32_t ly = lys[k];
            if (k && ly <= lys[k - 1]) continue;
            char lpath[300];
            snprintf(lpath, sizeof lpath, "%.*s_l%u.png", (int)strlen(path) - 4, path, ly);
            raw_layer = ly;
            renderer_save_texture_tex(st.rt.color[0]->backend, lpath);
            raw_layer = 0;
        }
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }
// The part of a level extent `e` a scalable image's contents occupy (Dynamic resolution: drawn at scale `sc` into an
// image allocated at scale `al`, the top-left part); the whole extent otherwise.
static int content_extent(int e, bool scalable, float sc, float al) {
    if (!scalable || sc == al || al <= 0) return e;
    int c = (int)lroundf(e * sc / al);
    return c < 1 ? 1 : c > e ? e : c;
}

static bool heap_whole_userptr(void);
static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    else {
        // Submit the frame in chunks (as Metal, renderer.m begin_pass): the game puts a whole frame in one command
        // list and waits for its fence early in the next frame, so with one submission per list the GPU sat idle
        // while the list was translated, and the game's wait covered translation + GPU time - with the render thread
        // the translation left the game thread but came back as that wait. LARECOMP_CHUNK_PASSES=<n>: a submission
        // every n render passes (0 = one per list). Default 8 with the render thread (RTX 3080 Ti + i9, Field_11C:
        // gpu wait 0.6 -> 0.25 ms, game thread busy 3.4-3.5 ms vs 4.9 without the render thread); without it,
        // off (chunks cost that thread 0.1-0.3 ms there). Also off by default while the whole heap is one userptr
        // import (heap_whole_userptr): amdgpu checks all of its pages again on every submission, ~5 ms each for the
        // 3.2 GB heap, so ten chunks a frame cost ~50 ms (issue #92: RX 6800 XT, 12 fps at the title screen, where
        // 0c26936 - one submission per list - got 29-41).
        static int chunk = -1;
        static bool chunk_env;
        if (chunk < 0) {
            extern bool rq_on(void);
            const char *e = getenv("LARECOMP_CHUNK_PASSES");
            chunk_env = e != NULL;
            chunk = e ? atoi(e) : rq_on() ? 8 : 0;
        }
        int c = chunk;
        if (c > 0 && !chunk_env && heap_whole_userptr()) {  // (decided at boot: asked here, not cached before it)
            static bool logged;
            if (!logged) { logged = true; LOG("[vk] submission chunks off: the guest heap is one userptr import (AMD)\n"); }
            c = 0;
        }
        if (c > 0 && ++passes_in_sub >= c) renderer_submit();
    }
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        // (color: the game's size for the target; drawn: what is rendered - the render scale applied, as the pass's area)
        const TexBackend *tb = t0 ? t0->backend : NULL;
        float dsx = tb && tb->scalable ? tb->sx : 1, dsy = tb && tb->scalable ? tb->sy : 1;
        gp_mark(cb, "pass: %d color %ux%u (drawn %.0fx%.0f) fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0,
                t0 ? t0->height : 0, t0 ? t0->width * dsx : 0, t0 ? t0->height * dsy : 0, t0 ? t0->format : 0,
                st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    uint32_t layers = UINT32_MAX;  // layered rendering: the fewest layers of any attachment
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        uint32_t nly;
        VkImageView v = vk_render_view(t, &st.rt.cview[i], st.rt.has_cview[i], &nly);
        TexBackend *b = t->backend;
        if (!v) continue;
        if (nly < layers) layers = nly;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        pass_color[i] = vk_view_format(b, pass_color[i]);  // (g_vk.lean_usage: the format the view really has)
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    // Bound color slots without a texture / view stay as VK_NULL_HANDLE attachments (legal), but Intel's Windows
    // driver for UHD 630 (Vulkan 1.3.215) crashes in vkCmdBeginRendering reading near a null pointer on the title
    // screen's first clear (issues #8, #46). Empty slots at the end are left out (pipelines and clears follow
    // pass_ncolor); one in the middle, or a pass with no attachment at all, is logged once for the next report.
    while (pass_ncolor && !ca[pass_ncolor - 1].imageView) pass_ncolor--;
    static bool logged_hole;
    for (uint32_t i = 0; i < pass_ncolor && !logged_hole; i++)
        if (!ca[i].imageView) {
            logged_hole = true;
            LOG("[vk] pass with an empty color slot %u of %u (texture %s)\n", i, pass_ncolor, st.rt.color[i] ? "without a view" : "none");
        }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        uint32_t nly;
        VkImageView v = vk_render_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, &nly);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            if (nly < layers) layers = nly;
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) {
        pass_sx = pass_sy = 1; minw = minh = 16;
        static bool logged_none;
        if (!logged_none) logged_none = true, LOG("[vk] pass with no attachments (%d color slots bound)\n", st.rt.ncolor);
    }
    // A pass with depth / stencil but no color attachment gets a throwaway one on Intel (LARECOMP_VK_PLACEHOLDER_COLOR=0 /
    // 1 overrides): the UHD 630's Windows driver (Vulkan 1.3.215) crashes in the vkCmdBeginRendering after such a pass,
    // reading near a null pointer (issues #8, #46). Pipelines and clears follow pass_ncolor / pass_color as usual.
    static int placeholder = -1;
    if (placeholder < 0) {
        const char *e = getenv("LARECOMP_VK_PLACEHOLDER_COLOR");
        placeholder = e ? atoi(e) != 0 : g_vk.props.vendorID == 0x8086;
    }
    bool placed = false;
    if (placeholder && !pass_ncolor && (has_d || has_s)) {
        VkImageView v = vk_dummy_color_view(minw, minh, pass_samples);
        if (v) {
            ca[0] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                                .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE};
            pass_color[0] = VK_FORMAT_R8_UNORM;
            pass_ncolor = 1;
            placed = true;
        }
    }
    pass_w = minw; pass_h = minh;
    pass_layers = layers == UINT32_MAX ? 1 : layers;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = pass_layers,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = pass_ncolor ? ca : NULL,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    // LARECOMP_VK_LOG_PASSES=<n> (default 300 with =1): each of the first n passes before it begins - an Intel crash
    // inside vkCmdBeginRendering (issues #8, #46) ends the log on the pass the driver choked on
    static long log_passes = -1;
    if (log_passes < 0) {
        log_passes = getenv("LARECOMP_VK_LOG_PASSES") ? atol(getenv("LARECOMP_VK_LOG_PASSES")) : 0;
        if (log_passes == 1) log_passes = 300;
    }
    if (log_passes > 0) {
        log_passes--;
        LOG("[vk] pass %ux%u: %u color", minw, minh, pass_ncolor);
        for (uint32_t i = 0; i < pass_ncolor; i++) {
            TexBackend *b = !placed && st.rt.color[i] ? st.rt.color[i]->backend : NULL;
            LOG(" [%u: fmt %d view %s %s", i, pass_color[i], ca[i].imageView ? "yes" : "none", placed ? "placeholder"
                : ca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "clear" : "load");
            // (the image under it: its own format, size, levels / layers, samples; the view's first level and layer)
            if (b) LOG(", image fmt %d %ux%u lv %u ly %u s %u, view lv %u ly %u", b->format, b->width, b->height, b->levels,
                       b->layers, b->samples, st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0,
                       st.rt.has_cview[i] && (st.rt.cview[i].setmask & 2) ? st.rt.cview[i].min_layer : 0);
            LOG("]");
        }
        if (has_d || has_s)
            LOG(", depth %s fmt %d, stencil %s fmt %d (same view %s, layout %d, load op %d / %d)", has_d ? "yes" : "no",
                pass_depth, has_s ? "yes" : "no", pass_stencil, has_d && has_s && da.imageView == sa.imageView ? "yes" : "no",
                has_d ? da.imageLayout : sa.imageLayout, has_d ? da.loadOp : -1, has_s ? sa.loadOp : -1);
        else LOG(", no depth / stencil");
        LOG(", samples %u\n", pass_samples);
    }
    rec_CmdBeginRendering(cb, &ri);
    ss_forget();  // (whatever ran between the passes may have set its own state)
    in_pass = true;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    if (!ss_need(SS_SC, !memcmp(&r, &ss.sc, sizeof r))) return;
    ss.sc = r;
    rec_CmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!ss_need(SS_VP, !memcmp(&vp, &ss.vp, sizeof vp))) return;
    ss.vp = vp;
    rec_CmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])
// The dynamic states the pipelines take: without extended dynamic state 1 / 2 (vk.h VkCompat), their states (DYN[7..14],
// DYN[15..16]) go into the pipelines instead.
// (made once, but by whichever builds come first - Pokémon Z-A's startup shader preparation starts dozens on the
// workers at once: each fills its own copy and stores the same values, then the count. The shared count it counted up
// before ran past the array when two threads filled it together - garbage dynamic states, an NVIDIA driver crash in
// vkCreateGraphicsPipelines about 1 start in 6)
static const VkDynamicState *dyn_states(uint32_t *n) {
    static VkDynamicState d[NDYN];
    static _Atomic uint32_t nd;
    uint32_t c = atomic_load(&nd);
    if (!c) {
        VkDynamicState t[NDYN];
        for (uint32_t i = 0; i < NDYN; i++)
            if (!(g_vkc.no_eds1 && i >= 7 && i <= 14) && !(g_vkc.no_eds2 && i >= 15)) t[c++] = DYN[i];
        memcpy(d, t, sizeof t);
        atomic_store(&nd, c);
    }
    *n = c;
    return d;
}

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    rec_CmdSetDepthBias(cb, 0, 0, 0);
    rec_CmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    rec_CmdSetBlendConstants(cb, bc);
    rec_CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    rec_CmdSetCullMode(cb, VK_CULL_MODE_NONE);
    rec_CmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    rec_CmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    rec_CmdSetDepthTestEnable(cb, VK_FALSE);
    rec_CmdSetDepthWriteEnable(cb, VK_FALSE);
    rec_CmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    rec_CmdSetStencilTestEnable(cb, VK_FALSE);
    rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    rec_CmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

// This AMD driver draws nothing from a pipeline with fixed vertex input while dynamic vertex input is enabled.
// These triangles are generated from gl_VertexIndex and have no attributes, so the dynamic state is an empty layout.
// Without it the movie and the upscaler present uninitialised memory (red noise) while the audio plays.
static void fullscreen_vertex_dyn(VkDynamicState *dyn, uint32_t *n, uint32_t cap) {
    if (g_vk.vids && *n < cap) dyn[(*n)++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
}
static void fullscreen_vertex_bind(VkCommandBuffer cb) {
    if (!g_vk.vids) return;
    VkVertexInputBindingDescription2EXT b = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
    VkVertexInputAttributeDescription2EXT a = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
    rec_CmdSetVertexInputEXT(cb, 0, &b, 0, &a);
    ss_forget();  // the next game draw must set its own layout again
}

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    if (g_vkc.no_eds1) {  // (what clear() sets as dynamic state otherwise)
        ds.depthTestEnable = ds.depthWriteEnable = depth, ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        ds.stencilTestEnable = stencil;
        ds.front = ds.back = (VkStencilOpState){VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                                                VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    uint32_t ndyn;
    const VkDynamicState *dyns = dyn_states(&ndyn);  // (Vulkan 1.2 without extended dynamic state: fewer)
    VkDynamicState dyn[NDYN + 1];
    memcpy(dyn, dyns, ndyn * sizeof *dyn);
    fullscreen_vertex_dyn(dyn, &ndyn, NDYN + 1);
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = ndyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, pass_layers};
        if (n) rec_CmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();  // (its own pipeline and state: the next draw sets everything again)
    dyn_defaults(cb);
    if (depth) { rec_CmdSetDepthTestEnable(cb, VK_TRUE); rec_CmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        rec_CmdSetStencilTestEnable(cb, VK_TRUE);
        rec_CmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        rec_CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        rec_CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    rec_CmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    fullscreen_vertex_bind(cb);
    rec_CmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkShaderModule fs_late;  // (lazy chunks) fs without early fragment tests, for alpha to coverage pipelines; or NULL
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[3];  // [2] (compute: [1]): the heap chunk set (gmem_chunk_layout)
    VkPipelineLayout layout;
    bool vs_uses_vid;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
    uint64_t prep_vh, prep_fh;  // (prepared programs: their code hashes, for LARECOMP_PREP_CHECK)
    ShaderFp fp[2];             // (stale-program detector, shader_prep.h; compute: [0]; touched by the replay only)
    struct PipeMemo *pmemo;     // (render thread) draw_'s pipeline key memo for this program
    bool reuse;                 // set reuse (stage_set): some uniform buffers are dynamic, sets are kept while their contents repeat
    struct SetCache *scache[2]; // (render thread) stage_set's last set per set index
} Prog;

// ---- descriptor set reuse (graphics programs; port of vk-draw-cpu-2 d50bbce)
// Every draw allocated and wrote two descriptor sets (~20 descriptors) although most repeat the previous draw's of
// the same program: the textures and samplers are the same, and the uniform buffers are copies in the same transient
// ring buffer at new offsets. With the uniform buffers that move per draw declared dynamic (their offsets go to
// vkCmdBindDescriptorSets), the set's contents (buffers, ranges, views, samplers) repeat, so stage_set keeps the last
// set of each program and set index and binds it again while what it would write is identical. A kept set belongs to
// the pending submission (its pools are reset with the slot) and to the current view generation; buffers bound in
// sets are destroyed deferred (after the submissions using them), or at a reboot (ds_gen).
// LARECOMP_VK_NO_SET_REUSE=1: no dynamic buffers, a new set per draw, as before.
static int set_reuse = -1;
static uint32_t ds_gen;  // bumped when a buffer bound in sets may be destroyed at once (reboot)
static bool set_reuse_on(void) {
    if (set_reuse < 0) set_reuse = getenv("LARECOMP_VK_NO_SET_REUSE") == NULL;
    return set_reuse;
}
static bool is_ubo(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; }
// Up to the device's limit for the whole pipeline layout (8 or more; NVIDIA 15, AMD 16), the uniform buffers whose
// copies move most: the game's constant buffers (bindings 2-16), the scale table (18), the driver constants (0), the
// tint (19), then the compiler constants (1: one copy per submission). (Build workers: before the program is ready.)
static void mark_dynamic(Prog *p) {
    if (!set_reuse_on()) return;
    uint32_t lim = g_vk.props.limits.maxDescriptorSetUniformBuffersDynamic, n = 0;
    if (lim > 32) lim = 32;
    static const int8_t tier_of[20] = {2, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 1, 3};
    for (int tier = 0; tier <= 4; tier++)
        for (int i = 0; i < p->nbinds && n < lim; i++) {
            VkBindingInfo *b = &p->binds[i];
            if (b->set > 1 || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->binding >= 20 || tier_of[b->binding] != tier) continue;
            b->type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            n++;
        }
    p->reuse = true;
    static _Atomic int logged, logged_left;  // (the limit once; programs left with static buffers: the first 4)
    int left = 0;
    for (int i = 0; i < p->nbinds; i++) left += p->binds[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && p->binds[i].set <= 1;
    if (!atomic_exchange(&logged, 1) || (left && atomic_fetch_add(&logged_left, 1) < 4))
        LOG("[vk] set reuse: %u dynamic uniform buffers allowed; a program with %u dynamic, %d left static\n", lim, n, left);
}

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    bool gm = gc_shift && ((p->vi && p->vi->uses_gmem) || (p->fi && p->fi->uses_gmem));
    if (gm) p->set_layout[nsets++] = gmem_chunk_layout();
    // (lazy chunks, vertex / fragment: the storage buffers' chunks as push constants, shader.h GMEM_DIRECT_MAX)
    VkPushConstantRange gd = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 * GMEM_DIRECT_MAX};
    bool direct = gm && gc_lazy && s1 == VK_SHADER_STAGE_FRAGMENT_BIT;
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout, .pushConstantRangeCount = direct ? 1 : 0,
                                     .pPushConstantRanges = direct ? &gd : NULL};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!ENV("LARECOMP_SAVE_MSL")) return;
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

void slow_note_build(const char *what, uint64_t ns);  // (slowframe.h: the slow-frame log's counts; every build,
void slow_note_cache_hit(void);                       // and the pipelines the driver's cache had)
ShaderInfo *slow_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
#define maxwell_translate slow_translate  // (the translations below: counted and timed for the slow-frame log)
#define SLOW_FEEDBACK(info) VkPipelineCreationFeedback fb_ = {0}; VkPipelineCreationFeedbackCreateInfo fbi_ = \
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, .pNext = (info).pNext, .pPipelineCreationFeedback = &fb_}; \
    (info).pNext = &fbi_
#define SLOW_FEEDBACK_HIT() do { if (fb_.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) slow_note_cache_hit(); } while (0)

// A build the driver takes over a second for is logged with its shaders: draws and dispatches are skipped until it
// is done, which looked like a rendering bug (the chain chomps' compute with the heap in chunks: never ready).
static void slow_build(uint64_t t0, const char *what, uint64_t code0, uint64_t code1) {
    slow_note_build(what, SDL_GetTicksNS() - t0);
    uint64_t ms = (SDL_GetTicksNS() - t0) / 1000000;
    if (ms < 1000) return;
    static int n;
    if (n++ < 40) LOG("[shader] slow build: %s %llx %llx took %llu ms\n", what, (unsigned long long)code0, (unsigned long long)code1, (unsigned long long)ms);
}

// A fragment shader the translator gave early fragment tests (lazy chunks, translate.c glsl_early_tests): removes the
// OpExecutionMode EarlyFragmentTests (in place; true if there was one). Alpha to coverage drops samples after the
// shader, and with early tests their depth / stencil would already be written: those pipelines take this module.
static bool spirv_strip_early_tests(uint32_t *w, size_t *nwords) {
    for (size_t i = 5; i < *nwords;) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
        if (!n || i + n > *nwords) return false;
        if (op == 54) return false;  // OpFunction: past the execution modes
        if (op == 16 && n == 3 && w[i + 2] == 9) {  // OpExecutionMode %main EarlyFragmentTests
            memmove(w + i, w + i + n, (*nwords - i - n) * sizeof *w);
            *nwords -= n;
            return true;
        }
        i += n;
    }
    return false;
}

static void compile_program(void *arg) {
    Prog *p = arg;
    uint64_t t0 = SDL_GetTicksNS();
    int bs = build_begin("modules vs/fs", p->code[0] ? p->code[0] : p->prep_vh, p->code[1] ? p->code[1] : p->prep_fh);
    size_t nv = 0, nf = 0;
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); build_end(bs); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    if (spirv_strip_early_tests(sf, &nf)) p->fs_late = vk_module(sf, nf);
    free(sv);
    free(sf);
    mark_dynamic(p);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_compiles, 1);
    if (!p->vs || !p->fs) LOG("[shader] vs/fs %llx %llx: shader module creation failed\n", (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
    atomic_fetch_add(&builds_done, 1);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
    build_end(bs);
    slow_build(t0, "modules vs/fs", p->code[0], p->code[1]);
}

// The guest memory window that shader global memory, the vertex cache and the texture upload checks cover: the heap
// (SetHeapSize), or for a game on the SDK's virtual address memory, which has no heap (Pokémon Legends Z-A), its
// largest physical memory region (svcMapPhysicalMemory, at most HEAP_MAX). Picked once, at first use: shaders bake
// the base in.
typedef struct { uint64_t base, max; bool phys; } GWin;
static GWin gwin_ = {HEAP_BASE, HEAP_MAX, false};
static atomic_bool gwin_done;
static void gwin_pick(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!atomic_load(&gwin_done)) {
        uint64_t a = 0, sz = kernel_phys_largest(&a);
        const char *force = ENV("LARECOMP_GWIN");  // (debug) <base hex>:<size hex>
        if (force) {
            unsigned long long fb = 0, fs = 0;
            sscanf(force, "%llx:%llx", &fb, &fs);
            gwin_ = (GWin){fb, fs, true};
            plat_commit(fb, fs);  // (the host import needs every page committed)
            LOG("[vk] guest memory window forced: %llx +%llx\n", fb, fs);
            atomic_store(&gwin_done, true);
        } else if (kernel_heap_size()) atomic_store(&gwin_done, true);
        else if (sz >= (64ull << 20)) {
            gwin_ = (GWin){a, sz < HEAP_MAX ? sz : HEAP_MAX, true};
            LOG("[vk] guest memory window: physical memory %llx +%llx (no heap)\n", (unsigned long long)a,
                (unsigned long long)gwin_.max);
            atomic_store(&gwin_done, true);
        }
    }
    pthread_mutex_unlock(&lock);
}
static inline const GWin *gwin(void) {
    if (__builtin_expect(!atomic_load_explicit(&gwin_done, memory_order_acquire), 0)) gwin_pick();
    return &gwin_;
}
#define gwin_base (gwin()->base)
#define gwin_max (gwin()->max)
// bytes of the window the game has set up: the heap size, or the whole physical region
static uint64_t gwin_want(void) { return gwin()->phys ? gwin_.max : kernel_heap_size(); }

static void gmem_window_get(uint64_t *lo, uint64_t *len);  // (the guest-heap import window, below)
// Translates both stages (false: failed, logged). Holds the translator for the whole program.
static bool translate_program_now(Prog *p) {
    uint64_t wl, wn;
    gmem_window_get(&wl, &wn);
    pthread_mutex_lock(&translate_lock);  // (the globals too: the render thread translates as well - run_queued_now)
    maxwell_gmem_base = gwin_base + wl, maxwell_gmem_size = wn;
    maxwell_gmem_chunk_shift = gmem_chunk_shift();
    p->vi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = p->code[1] ? maxwell_translate((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], STAGE_FRAGMENT, &p->fkey)
                           : maxwell_empty_fragment();
    }
    if (ENV("LARECOMP_DUMP_GLSL")) {  // (debug) build/glsl/<code address>.{vert,frag}[.asm]: source and disassembly
        mkdir("build/glsl", 0755);
        for (int sg = 0; sg < 2; sg++) {
            ShaderInfo *si = sg ? p->fi : p->vi;
            if (!p->code[sg]) continue;
            char path[96];
            snprintf(path, sizeof path, "build/glsl/%llx.%s", (unsigned long long)p->code[sg], sg ? "frag" : "vert");
            FILE *f = fopen(path, "wb");
            if (f) { if (si && si->msl) fputs(si->msl, f); fclose(f); }
            strcat(path, ".asm");
            if ((f = fopen(path, "wb"))) {
                maxwell_disassemble((const uint8_t *)(uintptr_t)p->code[sg], p->cmax[sg], sg ? STAGE_FRAGMENT : STAGE_VERTEX, f);
                fclose(f);
            }
        }
    }
    pthread_mutex_unlock(&translate_lock);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return false;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s\n", p->fi->error);
        atomic_store(&p->state, 2);
        return false;
    }
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    return true;
}

static void translate_program(void *arg) {
    Prog *p = arg;
    if (!translate_program_now(p)) return;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    run_job(compile_program, p, &p->state);
}

// The code at a shader's address, in brief: its length (to the end of its constants) and four of its words. Programs
// are looked up by code address, and nothing checked that the code there was still the one they were built from - a
// game can load other shaders where old ones were (upstream LAS Recomp's stale-program detector, c93564d, logs it).
// The fingerprint is part of the lookup: other code, its own program (the same code again finds its translation in
// cache/maxwell); "[shader] code changed" says when it happens.
static uint64_t code_fp(const NvnShaderStage *s) {
    uint32_t len = s->const_offset ? s->const_offset + s->const_size : 0;
    if (!s->code_addr || len < 4) return 0;
    const uint32_t *c = (const uint32_t *)(uintptr_t)s->code_addr;
    uint32_t n = len / 4, w[5] = {len, c[n > 0x20 ? 0x20 : 0], c[n / 3], c[2 * (n / 3)], c[n - 1]};
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 5; i++) { h ^= w[i]; h *= 0x100000001B3ull; }
    return h;
}
// A new program's code: logged when the address held other code before (render thread)
static void note_code(const NvnShaderStage *s, uint64_t fp, const char *stage) {
    static struct { uint64_t addr, fp; } seen[8192];
    if (!s->code_addr) return;
    uint32_t i = (uint32_t)(s->code_addr >> 4) & 8191;
    for (int probe = 0; seen[i].addr && seen[i].addr != s->code_addr; i = (i + 1) & 8191)
        if (++probe == 64) return;  // (full here: not noted)
    static int n;
    if (seen[i].addr && seen[i].fp != fp && n++ < 40)
        LOG("[shader] code changed at %llx (%s): a new program\n", (unsigned long long)s->code_addr, stage);
    seen[i].addr = s->code_addr, seen[i].fp = fp;
}

// ================================================================== prepared objects (shader_prep.h)
// Built before the game from the shipped variant list; found by content (code hashes + keys) at first use, as code
// addresses differ from run to run. Kept for the whole process (reboots included). prep_lock guards the tables.
static pthread_mutex_t prep_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t key; void *obj; } PrepEnt;
static PrepEnt *prep_progs, *prep_pipes, *prep_cs;  // program / pipeline / compute content key -> Prog / Pipe / CsProg
static size_t prep_progs_cap, prep_pipes_cap, prep_cs_cap, prep_progs_n, prep_pipes_n, prep_cs_n;
static _Atomic int prep_pending;

static void *prep_get(PrepEnt *t, size_t cap, uint64_t key) {
    if (!cap) return NULL;
    if (!key) key = 1;
    for (size_t j = key & (cap - 1); t[j].key; j = (j + 1) & (cap - 1))
        if (t[j].key == key) return t[j].obj;
    return NULL;
}
static void prep_put(PrepEnt **t, size_t *cap, size_t *n, uint64_t key, void *obj) {
    if (!key) key = 1;
    if (*n * 2 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        PrepEnt *nt = calloc(nc, sizeof *nt);
        for (size_t i = 0; i < *cap; i++)
            if ((*t)[i].key) {
                size_t j = (*t)[i].key & (nc - 1);
                while (nt[j].key) j = (j + 1) & (nc - 1);
                nt[j] = (*t)[i];
            }
        free(*t);
        *t = nt, *cap = nc;
    }
    size_t j = key & (*cap - 1);
    while ((*t)[j].key && (*t)[j].key != key) j = (j + 1) & (*cap - 1);
    if (!(*t)[j].key) (*n)++;
    (*t)[j] = (PrepEnt){key, obj};
}
#define PREP_GET(tab, key) ({ pthread_mutex_lock(&prep_lock); void *o_ = prep_get(tab, tab##_cap, key); pthread_mutex_unlock(&prep_lock); o_; })
#define PREP_PUT(tab, key, obj) do { pthread_mutex_lock(&prep_lock); prep_put(&tab, &tab##_cap, &tab##_n, key, obj); pthread_mutex_unlock(&prep_lock); } while (0)

static uint64_t prog_content_key(uint64_t vh, uint64_t fh, const ShaderKey *vkey, const ShaderKey *fkey) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t c[2] = {vh, fh};
    const uint8_t *b = (const uint8_t *)c;
    for (size_t i = 0; i < sizeof c; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    b = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    return h;
}

// The caches of programs and pipelines by key (get_program_, draw_): open addressing, doubled at half full. They were
// fixed at 4096 / 8192 entries and never emptied: a long session filled them (Pokémon Z-A, ~29 minutes in, a new
// area's shaders) and the next new key probed forever - the render thread spun, the game froze. (Render thread only.)
typedef struct { uint64_t key; void *val; } KvEnt;
typedef struct { KvEnt *t; uint32_t cap, n; } KvTab;
static KvEnt *kv_find(KvTab *k, uint64_t key) {  // key's entry, or the empty one it would take (key != 0)
    if (!k->cap) {  // (LARECOMP_VK_KV_START=<power of 2> (test): a small start, the growth exercised in any run)
        static uint32_t start;
        if (!start) start = ENV("LARECOMP_VK_KV_START") ? (uint32_t)atoi(ENV("LARECOMP_VK_KV_START")) : 4096;
        if (start < 16 || (start & (start - 1))) start = 4096;  // (the probe masks with cap - 1)
        k->cap = start, k->t = calloc(k->cap, sizeof *k->t);
    }
    uint32_t i = (uint32_t)(key ^ (key >> 31)) & (k->cap - 1);
    while (k->t[i].key && k->t[i].key != key) i = (i + 1) & (k->cap - 1);
    return &k->t[i];
}
static void kv_set(KvTab *k, KvEnt *e, uint64_t key, void *val) {  // e: kv_find's for key, no kv_find of k since
    if (!e->key) {
        if ((k->n + 1) * 2 > k->cap) {
            KvEnt *old = k->t;
            uint32_t oc = k->cap;
            k->cap *= 2, k->t = calloc(k->cap, sizeof *k->t);
            for (uint32_t i = 0; i < oc; i++) if (old[i].key) *kv_find(k, old[i].key) = old[i];
            free(old);
            e = kv_find(k, key);
        }
        k->n++;
    }
    e->key = key, e->val = val;
}
static void kv_clear(KvTab *k) { if (k->t) memset(k->t, 0, k->cap * sizeof *k->t); k->n = 0; }
static KvTab prog_kv;

static void prog_fp_check(Prog *p, NvnProgram *np) {  // (the lookups below are by code address: shader_prep.h)
    shader_fp_check(&p->fp[0], &np->stages[0], "vs");
    shader_fp_check(&p->fp[1], &np->stages[1], "fs");
}

// Key memo: draws mostly repeat the program and pipeline state of a recent draw. draw_ compares the inputs of the
// program lookup with the previous lookup's, and those of the pipeline lookup with the last ones of that program
// (plain compares of everything the keys are made from), and reuses the answer instead of hashing the program key
// byte by byte and filling + hashing the ~700-byte PipeKey. The answers are the objects the tables give. key_gen:
// bumped by vk_exec_reset (tables cleared). LARECOMP_VK_NO_KEY_MEMO=1: off.
static int key_memo = -1;
static uint32_t key_gen = 1;
static bool key_memo_on(void) {
    if (key_memo < 0) key_memo = getenv("LARECOMP_VK_NO_KEY_MEMO") == NULL;
    return key_memo;
}

static Prog *get_program_(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey);
static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    static struct { uint64_t code[2], fp[2]; ShaderKey vkey; uint8_t rt[8]; uint32_t gen; Prog *prog; } m;
    bool on = key_memo_on();
    // (the code fingerprints too: other code at the same address is another program - code_fp)
    uint64_t fp0 = code_fp(&np->stages[0]), fp1 = code_fp(&np->stages[1]);
    if (on && m.prog && m.gen == key_gen && (np->stage_mask & 3) == 3 && m.code[0] == np->stages[0].code_addr &&
        m.code[1] == np->stages[1].code_addr && m.fp[0] == fp0 && m.fp[1] == fp1 && !memcmp(&m.vkey, vkey, sizeof *vkey) &&
        !memcmp(m.rt, fkey->rt_kind, 8)) {
        prof_prog_memo++;
        return m.prog;
    }
    Prog *p = get_program_(np, vkey, fkey);
    if (on && p) {
        m.code[0] = np->stages[0].code_addr; m.code[1] = np->stages[1].code_addr; m.fp[0] = fp0; m.fp[1] = fp1;
        m.vkey = *vkey; memcpy(m.rt, fkey->rt_kind, 8); m.gen = key_gen; m.prog = p;
    }
    return p;
}

static Prog *get_program_(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(np->stage_mask & 1)) return NULL;  // (no fragment stage: depth only - maxwell_empty_fragment)
    uint64_t h = 0xcbf29ce484222325ull, fp[2];
    for (int i = 0; i < 2; i++) {
        uint64_t a = np->stages[i].code_addr;
        fp[i] = code_fp(&np->stages[i]);
        for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; }
        for (int k = 0; k < 8; k++) { h ^= (fp[i] >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; }
    }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    for (int k = 0; k < 4; k++) { h ^= (fkey->stencil_slots >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; }
    for (int k = 0; k < 4; k++) { h ^= (fkey->sint_slots >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    KvEnt *slot = kv_find(&prog_kv, h);
    if (slot->key) {
        prog_fp_check(slot->val, np);
        return slot->val;
    }
    {   // prepared before the game (the same code and keys)
        uint64_t vh = shader_stage_hash(&np->stages[0]), fh = shader_stage_hash(&np->stages[1]);
        uint64_t ck = prog_content_key(vh, fh, vkey, fkey);
        Prog *pp = vh && fh && prep_progs_n ? PREP_GET(prep_progs, ck) : NULL;
        if (pp) {
            kv_set(&prog_kv, slot, h, pp);
            prog_fp_check(pp, np);
            return pp;
        }
        shader_first_visit("program", vh, fh, ck);
    }
    Prog *p = calloc(1, sizeof *p);
    prog_fp_check(p, np);  // (takes the fingerprint)
    kv_set(&prog_kv, slot, h, p);
    note_code(&np->stages[0], fp[0], "vs");
    note_code(&np->stages[1], fp[1], "fs");
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    run_translate(translate_program, p, &p->state);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: {
        static uint8_t told[256];
        if (!told[f & 0xFF]++) LOG("[vk] vertex format %#x unknown: read as RGBA32F\n", f);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// The fixed-function state a draw sets as extended dynamic state (fixed_state): without the extension, part of the
// pipeline key instead (vk.h VkCompat.no_eds1).
typedef struct {
    uint8_t dt, dw, dop, sten, cull, ff, topo, equal_tol;
    uint8_t sop[2][4];  // front / back: fail, pass, depth fail, compare op
} FixedState;

// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill, depth_clamp;
    uint8_t restart;  // (no extended dynamic state 2 only: primitive restart, a strip topology)
    uint32_t write_mask[8];
    FixedState fx;    // (no extended dynamic state only: what draw_ sets as dynamic state otherwise)
} PipeKey;

typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state; } Pipe;  // state: 0 building, 1 ready, 2 failed
// (record.c, at the bind) the pipeline of a Pipe: NULL while it builds (wait: sleeps until it is done) or failed
static VkPipeline pipe_resolve(void *obj, bool wait) {
    Pipe *pp = obj;
    int st_;
    while ((st_ = atomic_load(&pp->state)) == 0) {
        if (!wait) return VK_NULL_HANDLE;
        struct timespec ts = {0, 200000};
        nanosleep(&ts, NULL);
    }
    return st_ == 1 ? pp->pipe : VK_NULL_HANDLE;
}
// (key memo, see get_program) everything pipe_key_fill reads besides the program: the pass's formats and the PipeIn.
// One per program (draws often alternate between a few programs): the last pipeline that program drew with.
struct PipeMemo {
    uint32_t gen, ncolor, samples, channel_mask;
    int nattrib, nstream;
    VkFormat color[8], depth, stencil;
    uint32_t attrib[16], stream[16][2], rt_fmt[8];
    uint8_t blend[8][8], blend_on, a2c, cls, poly_fill, depth_clamp, restart;  // (the last two: draw_'s own key parts)
    Pipe *pp;
    VtxLayout vl;  // the layout pipe_key_fill made from these inputs (dynamic vertex input leaves it out of pp's key)
};
static KvTab pipe_kv;  // (pipelines by key hash: kv_find above)

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// What a pipeline is built from, besides its program and the pass's attachment formats: the draw's state (draw_) or a
// variant record (renderer_prepare)
typedef struct {
    const uint32_t *attrib; int nattrib;
    const uint32_t (*stream)[2]; int nstream;
    const uint32_t *rt_fmt;         // color target texture formats (0: none)
    uint32_t channel_mask;
    uint8_t blend_on;               // color state blend enables
    const uint8_t (*blend)[8];
    uint8_t a2c, topo_class, poly_fill;
} PipeIn;

// vl (optional): the vertex layout, also when the key leaves it out (dynamic vertex input)
static void pipe_key_fill(PipeKey *k, Prog *pr, const PipeIn *in, const VkFormat color[8], uint32_t ncolor, VkFormat depth,
                          VkFormat stencil, uint32_t samples, VtxLayout *vl) {
    memset(k, 0, sizeof *k);
    k->prog = pr;
    k->ncolor = ncolor;
    // (only the pass's own slots: begin_pass leaves the slots past them as the last pass with more targets set them -
    // a depth-only pass's key carried the previous pass's color formats, so prepared pipelines never matched it)
    for (uint32_t i = 0; i < ncolor && i < 8; i++) k->color[i] = color[i];
    k->depth = depth;
    k->stencil = stencil;
    k->samples = samples;
    VtxLayout l_, *l = vl ? vl : &l_;
    memset(l, 0, sizeof *l);
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        l->attr_mask |= 1u << i;
        uint32_t a = i < in->nattrib ? in->attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= in->nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            l->attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            l->attr_bind[i] = ZERO_BINDING;
            l->used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        l->attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        l->attr_off[i] = off;
        l->attr_bind[i] = stream;
        l->used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(l->used_streams & (1u << s))) continue;
        l->stride[s] = s < in->nstream ? in->stream[s][0] : 0;
        l->divisor[s] = s < in->nstream && l->stride[s] ? in->stream[s][1] : 0;
        if (l->divisor[s] > 1 && !g_vk.divisor) l->divisor[s] = 1;
    }
    k->attr_mask = l->attr_mask;  // (the program's: the same with either path)
    if (!g_vk.vids) {             // the layout is part of the pipeline
        memcpy(k->attr_fmt, l->attr_fmt, sizeof k->attr_fmt);
        memcpy(k->attr_off, l->attr_off, sizeof k->attr_off);
        memcpy(k->attr_bind, l->attr_bind, sizeof k->attr_bind);
        memcpy(k->stride, l->stride, sizeof k->stride);
        memcpy(k->divisor, l->divisor, sizeof k->divisor);
        k->used_streams = l->used_streams;
    }
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        if (!in->rt_fmt[i] || k->color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k->write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (in->channel_mask >> (i * 4)) & 0xF : 0;
        if (((in->blend_on >> i) & 1) && !rt_kind(in->rt_fmt[i])) {
            k->blend_on |= (uint8_t)(1u << i);
            memcpy(k->blend[i], in->blend[i], 8);
        }
    }
    k->a2c = in->a2c;
    k->topo_class = in->topo_class;
    k->poly_fill = in->poly_fill;
}

// A pipeline's create info from its key (the structures point into each other and into the key: filled in place)
typedef struct {
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi;
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vs;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[NDYN + 1];  // (+ the vertex layout with dynamic vertex input: draw_ sets it)
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo ri;
    VkGraphicsPipelineCreateInfo pi;
} PipeCI;

static void pipe_ci_fill(PipeCI *c, const PipeKey *k) {
    memset(c, 0, sizeof *c);
    c->stages[0] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"};
    c->stages[1] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->a2c && k->prog->fs_late ? k->prog->fs_late : k->prog->fs, .pName = "main"};
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) c->va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        c->vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) c->vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    c->dvi = (VkPipelineVertexInputDivisorStateCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = c->vd};
    c->vi = (VkPipelineVertexInputStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &c->dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = c->vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = c->va};
    c->ia = (VkPipelineInputAssemblyStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    if (k->restart)  // (a list topology can't take primitive restart: the strip of its class, the draw sets the same)
        c->ia.topology = k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        c->ia.primitiveRestartEnable = VK_TRUE;
    c->vs = (VkPipelineViewportStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    c->rs = (VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1,
                                                 .depthClampEnable = k->depth_clamp ? VK_TRUE : VK_FALSE,
                                                 .depthBiasEnable = g_vkc.no_eds2};
    c->ms = (VkPipelineMultisampleStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    c->ds = (VkPipelineDepthStencilStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (g_vkc.no_eds1) {
        const FixedState *f = &k->fx;
        if (!k->restart) c->ia.topology = (VkPrimitiveTopology)f->topo;  // (restart: its strip, above)
        c->ds.depthTestEnable = f->dt, c->ds.depthWriteEnable = f->dw, c->ds.depthCompareOp = (VkCompareOp)f->dop;
        c->ds.stencilTestEnable = f->sten;
        for (int s = 0; s < 2; s++) {
            VkStencilOpState *o = s ? &c->ds.back : &c->ds.front;
            o->failOp = (VkStencilOp)f->sop[s][0], o->passOp = (VkStencilOp)f->sop[s][1];
            o->depthFailOp = (VkStencilOp)f->sop[s][2], o->compareOp = (VkCompareOp)f->sop[s][3];
        }
        c->rs.cullMode = f->cull, c->rs.frontFace = (VkFrontFace)f->ff;
    }
    for (uint32_t i = 0; i < k->ncolor; i++) {
        c->cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            c->cba[i].blendEnable = VK_TRUE;
            c->cba[i].srcColorBlendFactor = blend_factor(b[1]);
            c->cba[i].dstColorBlendFactor = blend_factor(b[2]);
            c->cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            c->cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            c->cba[i].colorBlendOp = blend_op(b[5]);
            c->cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    c->cb = (VkPipelineColorBlendStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                  .attachmentCount = k->ncolor, .pAttachments = c->cba};
    uint32_t ndyn;
    const VkDynamicState *dyns = dyn_states(&ndyn);  // (Vulkan 1.2 without extended dynamic state: fewer)
    memcpy(c->dyn, dyns, ndyn * sizeof *dyns);
    if (g_vk.vids) c->dyn[ndyn++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    c->dy = (VkPipelineDynamicStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = ndyn, .pDynamicStates = c->dyn};
    c->ri = (VkPipelineRenderingCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                            .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                            .stencilAttachmentFormat = k->stencil};
    c->pi = (VkGraphicsPipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &c->ri, .stageCount = 2,
                                           .pStages = c->stages, .pVertexInputState = g_vk.vids ? NULL : &c->vi,
                                           .pInputAssemblyState = &c->ia, .pViewportState = &c->vs, .pRasterizationState = &c->rs,
                                           .pMultisampleState = &c->ms, .pDepthStencilState = &c->ds, .pColorBlendState = &c->cb,
                                           .pDynamicState = &c->dy, .layout = k->prog->layout};
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    PipeKey *k = &pp->k;
    PipeCI c;
    pipe_ci_fill(&c, k);
    uint64_t t0 = SDL_GetTicksNS();
    int bs = build_begin("pipeline vs/fs", k->prog->code[0] ? k->prog->code[0] : k->prog->prep_vh,
                         k->prog->code[1] ? k->prog->code[1] : k->prog->prep_fh);
    SLOW_FEEDBACK(c.pi);
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &c.pi, NULL, &pp->pipe);
    SLOW_FEEDBACK_HIT();
    build_end(bs);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1);
    atomic_fetch_add(&builds_done, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
    slow_build(t0, "pipeline vs/fs", k->prog->code[0], k->prog->code[1]);
}

// Dynamic vertex input (g_vk.vids): the draw's layout, as build_pipeline would bake it in
static void set_vertex_input(VkCommandBuffer cb, const VtxLayout *l) {
    VkVertexInputAttributeDescription2EXT va[32];
    VkVertexInputBindingDescription2EXT vb[17];
    uint32_t na = 0, nb = 0;
    for (int i = 0; i < 32; i++)
        if (l->attr_mask & (1u << i))
            va[na++] = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                .location = (uint32_t)i, .binding = l->attr_bind[i], .format = l->attr_fmt[i], .offset = l->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(l->used_streams & (1u << s))) continue;
        bool inst = l->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .binding = s, .stride = l->stride[s], .inputRate = inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = inst ? l->divisor[s] : 1};
    }
    rec_CmdSetVertexInputEXT(cb, nb, vb, na, va);
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static int nfeedback;
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    rec_CmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// (debug) LARECOMP_COPY_TRACE=<frame>: draws' attachments / sampled textures, copies and snapshots of that frame ("[ct]")
static bool ct_on(void) {
    static const char *e = (const char *)1;
    if (e == (const char *)1) e = ENV("LARECOMP_COPY_TRACE");
    if (!e) return false;
    static uint64_t memo_f = ~0ull;
    static bool memo;
    uint64_t f = renderer_present_count();
    if (f == memo_f) return memo;
    memo_f = f;
    memo = false;
    for (const char *p = e; *p;) {
        if ((uint64_t)strtoll(p, (char **)&p, 10) == f) { memo = true; break; }
        if (*p == ',') p++; else break;
    }
    if (memo) LOG("[ct] ===== frame %llu\n", (unsigned long long)f);
    return memo;
}
static void ct_draw(Prog *pr, const char *what) {
    LOG("[ct] %s fs %016llx rt", what, (unsigned long long)g_cur_fs_hash);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" c%d=%p/%#x", i, (void *)st.rt.color[i], st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d=%p/%#x%s", (void *)st.rt.depth, st.rt.depth->format, st.ds[0] && st.ds[1] ? "W" : "");
    LOG(" cm %08x vp %.0f %.0f %.0f %.0f sc %d %d %d %d | tex", st.channel_mask, st.viewport[0][0], st.viewport[0][1], st.viewport[0][2],
        st.viewport[0][3], st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < (infos[sg] ? infos[sg]->ntex : 0); i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (t) LOG(" %s%p/%#x", sg ? "" : "v:", (void *)t, t->format);
        }
    LOG("\n");
}

// ================================================================== copy elision (depth copies)
// The game copies its main depth buffer whole, every frame, into textures that are then only sampled while the
// source keeps its depth (the water / transparency depth input), or that get their depth cleared right away
// (stencil kept). At 3x internal resolution each such copy is ~0.15 ms of pure bandwidth. Instead of copying, the
// destination becomes an alias: sampling it (and copying from it) reads the source's image. The alias ends before
// anything changes either side:
//   - the source is about to be overwritten whole (both aspects cleared): the two images swap - the destination
//     keeps the contents without a copy, the source is cleared in the destination's old image;
//   - the destination is about to be overwritten in some aspects (its depth cleared): only the other aspects are
//     copied (stencil, a quarter of the bytes);
//   - anything else (a draw writing depth / stencil, a partial clear, a copy or upload into either, the
//     destination bound as an attachment, a texture freed or reallocated): the real copy is made first.
// Only same-size, same-format, single-level, single-sample depth/stencil images, whole-image copies.
// LARECOMP_VK_COPY_OPT=0: always copy (the old behaviour).
typedef struct { NvnTexture *src, *dst; VkImage simg, dimg; } CopyAlias;
extern uint32_t vk_view_gen;
static CopyAlias aliases[8];
static int naliases;
static uint64_t stat_alias_made, stat_alias_renamed, stat_alias_partial, stat_alias_copied;

static int copy_opt(void) {
    static int on = -1;
    if (on < 0) { const char *e = ENV("LARECOMP_VK_COPY_OPT"); on = e && *e ? atoi(e) : 1; }
    return on;
}

// still valid: both textures keep the images the alias was made with (a resolution change reallocates them)
static bool alias_valid(const CopyAlias *a) {
    const TexBackend *s = a->src->backend, *d = a->dst->backend;
    return s && d && s->image == a->simg && d->image == a->dimg;
}

static void alias_drop(int i) {
    aliases[i] = aliases[--naliases];
    vk_view_gen++;  // (exec.c's per-frame view memo: the destination's views change)
}

// the real copy of the aspects asked for, into the destination's own image
static void alias_copy(CopyAlias *a, VkImageAspectFlags aspects, const char *why) {
    TexBackend *s = a->src->backend, *d = a->dst->backend;
    aspects &= d->aspect;
    if (!aspects) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "copy texture (deferred, %s%s) %ux%u", why, aspects == d->aspect ? "" : ", stencil only", a->dst->width, a->dst->height);
    VkImageCopy c = {.srcSubresource = {aspects, 0, 0, 1}, .dstSubresource = {aspects, 0, 0, 1}, .extent = {d->width, d->height, 1}};
    rec_CmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    if (aspects == d->aspect) stat_alias_copied++; else stat_alias_partial++;
    if (ct_on()) LOG("[ct] alias copy %p -> %p aspects %#x (%s)\n", (void *)a->src, (void *)a->dst, aspects, why);
}

// t is about to be written; full: the aspects of it the write overwrites whole (0: unknown / partial)
static void alias_before_write(NvnTexture *t, VkImageAspectFlags full, const char *why) {
    if (!naliases || !t) return;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (!alias_valid(a)) { alias_drop(i); continue; }
        if (a->dst != t) continue;
        // (LARECOMP_VK_COPY_OPT=2: all aspects even then - for comparing the stencil-only copy's cost)
        alias_copy(a, ((TexBackend *)t->backend)->aspect & (copy_opt() == 2 ? ~0u : ~full), why);
        alias_drop(i);
    }
    NvnTexture *heir = NULL;  // t overwritten whole: one destination can take its image instead of a copy
    if (t->backend && (((TexBackend *)t->backend)->aspect & ~full) == 0)
        for (int i = 0; i < naliases && !heir; i++) if (aliases[i].src == t) heir = aliases[i].dst;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (a->src != t || a->dst == heir) continue;
        alias_copy(a, ((TexBackend *)a->dst->backend)->aspect, why);
        alias_drop(i);
    }
    int rename = -1;
    for (int i = 0; i < naliases; i++) if (heir && aliases[i].src == t && aliases[i].dst == heir) rename = i;
    if (rename >= 0) {
        CopyAlias *a = &aliases[rename];
        TexBackend *s = a->src->backend, *d = a->dst->backend;
        vk_end_pass();
        st.rt_dirty = true;
        VkImage im = s->image; s->image = d->image; d->image = im;
        VmaAllocation m = s->mem; s->mem = d->mem; d->mem = m;
        vk_views_forget(s);
        vk_views_forget(d);
        stat_alias_renamed++;
        if (ct_on()) LOG("[ct] alias rename %p -> %p (%s)\n", (void *)a->src, (void *)a->dst, why);
        alias_drop(rename);
    }
}

// the texture whose image holds t's contents (t itself unless it is an alias)
static NvnTexture *alias_read(NvnTexture *t) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].dst == t) {
            if (alias_valid(&aliases[i])) return aliases[i].src;
            alias_drop(i);
            return t;
        }
    return t;
}

// copy_texture: a whole-image copy that can be an alias instead (true: done)
static bool alias_make(NvnTexture *src, NvnTexture *dst, TexBackend *s, TexBackend *d) {
    if (!copy_opt() || src == dst) return false;
    if (!(s->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return false;
    if (s->format != d->format || s->width != d->width || s->height != d->height || s->aspect != d->aspect || s->levels != 1 ||
        d->levels != 1 || s->layers != 1 || d->layers != 1 || s->depth != 1 || d->depth != 1 || s->samples != 1 || d->samples != 1 ||
        s->base_type != d->base_type || s->presented || d->presented || !s->image || !d->image || !s->gpu_owned || !d->gpu_owned)
        return false;
    // the destination's previous contents (or alias) are replaced whole; a destination that is itself the source of
    // aliases gets those resolved first
    alias_before_write(dst, d->aspect, "copy dst");
    src = alias_read(src);  // (a copy of an alias: of its source)
    s = src->backend;
    if (src == dst) return true;  // (copied back onto itself)
    if (naliases == (int)(sizeof aliases / sizeof aliases[0])) return false;
    aliases[naliases++] = (CopyAlias){src, dst, s->image, d->image};
    vk_view_gen++;
    stat_alias_made++;
    return true;
}

// textures.c: t is being freed - an alias source hands its contents over first
void vk_alias_texture_destroyed(NvnTexture *t) {
    if (naliases) alias_before_write(t, 0, "freed");
}

// textures.c: t is being freed - the executor's state stops naming it. The render targets stay bound across command
// lists, and a freed one was still in st.rt: a later list's pass began on it, which made it a new backend and put the
// freed texture back on the live list (textures.c g_live); the next texture freed then read that entry's garbage -
// a crash in live_remove when the game freed textures walking into a new area of the city.
void vk_exec_texture_destroyed(NvnTexture *t) {
    bool rt = false;
    for (int i = 0; i < 8; i++) if (st.rt.color[i] == t) st.rt.color[i] = NULL, rt = true;
    if (st.rt.depth == t) st.rt.depth = NULL, rt = true;
    if (rt) st.rt_dirty = true;  // (an open pass keeps its views: they are destroyed once the GPU is done)
    for (int i = 0; i < 4; i++) if (feedback_tex[i] == t) feedback_tex[i] = NULL;
    snapshots_invalidate(t);
    for (int i = 0; i < 8; i++) if (snap_tex[i] == t) snap_tex[i] = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) snaps[i].key = NULL;
}
void vk_alias_reset(void) { naliases = 0; }  // (reboot: every texture goes)

// (renderer_stats, once per stats interval) copies made aliases / renamed / partly or fully copied later
void vk_alias_stats(void) {
    static int n;
    if (!stat_alias_made || (n++ > 5 && n % 30)) return;
    LOG("[vk] copy alias: %llu made, %llu renamed, %llu stencil-only, %llu copied later (live %d)\n",
        (unsigned long long)stat_alias_made, (unsigned long long)stat_alias_renamed, (unsigned long long)stat_alias_partial,
        (unsigned long long)stat_alias_copied, naliases);
    stat_alias_made = stat_alias_renamed = stat_alias_partial = stat_alias_copied = 0;
}

// draws: attachments written (or a destination bound at all) end the aliases involved
static void alias_draw_check(void) {
    if (!naliases) return;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) alias_before_write(st.rt.color[i], 0, "color target");
    NvnTexture *d = st.rt.depth;
    if (!d) return;
    bool writes = (st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF));
    bool is_dst = false;
    for (int i = 0; i < naliases; i++) is_dst |= aliases[i].dst == d;
    if (writes || is_dst) alias_before_write(d, 0, writes ? "depth write" : "dst bound");
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = ENV("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            // Drawn into one mip level above the base, reading the texture's other levels: a mip chain being built,
            // no feedback loop - sampled in place. Pokémon Z-A builds its depth pyramid this way (each level from the
            // one above, the whole texture sampled at an explicit level); the snapshot held level 0 only and was
            // reused for the following levels, so most of the pyramid came out empty or stale, and the game's GPU
            // occlusion culling, which reads it, dropped visible objects: tables, statues, trees and stalls vanishing
            // and coming back with the camera.
            if (t == st.rt.depth && st.rt.has_dview && (st.rt.dview.setmask & 1) && st.rt.dview.base_level) continue;
            {
                bool mip = false;
                for (int k = 0; k < st.rt.ncolor; k++)
                    if (t == st.rt.color[k] && st.rt.has_cview[k] && (st.rt.cview[k].setmask & 1) && st.rt.cview[k].base_level) mip = true;
                if (mip) continue;
            }
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = ENV("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                feedback_tex[nfeedback] = t;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            if (ct_on()) LOG("[ct] snapshot %p %ux%u (was %s, si %d)\n", (void *)t, src->width, src->height, si >= 0 ? snap_why[si] : "new", si);
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            rec_CmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// Lean constant blocks: the scale table (66 rows, 1 KB) and the driver constants (up to 2 KB) were zero-filled in
// full and compared in full per stage and draw, although only a few rows / entries carry anything. These build and
// compare only those (the scale table's rows 0..ntex and 65; the driver constants' storage buffer entries below the
// size the shader reads) and write the same bytes as before. LARECOMP_VK_NO_CBUF_LEAN=1: the full blocks, as before.
static int cbuf_lean_v = -1;
static bool cbuf_lean(void) {
    if (cbuf_lean_v < 0) cbuf_lean_v = getenv("LARECOMP_VK_NO_CBUF_LEAN") == NULL;
    return cbuf_lean_v;
}
// The scale table of a graphics stage: rows 0..nt and 65 of rs are set, the rows between are zero (not read from rs).
static VkSpan rscale_dedupe(int sg, const float rs[66][4], int nt) {
    static struct { uint64_t seq; int nt; float r[66][4]; VkSpan span; } last[2];
    uint64_t seq = vk_pending_seq();
    size_t used = (size_t)(nt + 1) * 16;
    if (last[sg].seq == seq && last[sg].nt == nt && !memcmp(last[sg].r, rs, used) && !memcmp(last[sg].r[65], rs[65], 16)) {
        prof_cb_reused++;
        return last[sg].span;
    }
    VkSpan s = vk_transient(66 * 16, 256);
    memcpy(s.ptr, rs, used);
    if (nt < 64) memset((uint8_t *)s.ptr + used, 0, (size_t)(64 - nt) * 16);
    memcpy((uint8_t *)s.ptr + 65 * 16, rs[65], 16);
    last[sg].seq = seq; last[sg].nt = nt; memcpy(last[sg].r, rs, used); memcpy(last[sg].r[65], rs[65], 16); last[sg].span = s;
    prof_cb_copied++;
    return s;
}
// ---- storage buffers the game rewrites one frame ahead (overlap; renderer.m snap_storage)
// The game's tiled light / exposure lists: each frame its CPU memcpys the list for the frame after this one into the
// buffer the frame being drawn reads (7281e33500+32640 and 7281e3bd00+2040, alternating by frame), which the Switch's
// fence made safe. With the fence deferred that copy lands while the GPU still reads the list: screen-aligned tiles
// without shadow, a dark saturated picture when the exposure program (72a8da6000) reads it - worse the slower the GPU.
// Other storage data (the skinning program 72a8da0000's outputs, read in place by the exposure program and by draws
// binding them as vertex data from an offset into the pool: BowWow's body) is only ever written by the GPU and must
// stay where the game put it.
// Which is which is decided per memory range, with no wait and no probe of the programs (renderer.m 660bead). A range
// a draw reads as storage is shader-consumed game data (the lists): COPY for good - every binder of it, the compute
// dispatches that read or modify it too, works on a copy taken at translation, refreshed whenever its data changes
// (hash) and otherwise kept, so the GPU's own modifications stay. The CPU's rewrite can't be told from the data:
// standing still it rewrites the same bytes, and the boxes come from the GPU reading the list while it is rewritten
// (an M6 at 4x in the woods), so no hash decides this case - and on the title screen a probe would have settled the
// lists in place for good. A range only compute slots bind gets a one-frame PROBE on its first use in a level (all
// binders on the copy, the GPU can't touch the original) and its next use looks at the original: changed, the CPU
// rewrites it - COPY; unchanged, a GPU output or static (the skinning outputs, which draws then bind as vertex data
// from an offset into the pool) - IN PLACE for good, probed again when the level changes or after 10 s unused. With
// the write watch below (the default where the heap import allows it) a compute-only range starts in place and is
// copied from the first CPU store into it instead - the probe is the fallback; a copied range is never demoted. Each
// range's decision is logged once ("[snap] ...", the small per-object ranges capped). ~100 KB copied and hashed per
// frame in the field.
// Where the copies live: shaders address storage by guest address through the heap import (one buffer, lazy chunks,
// gfast's copy of the bound ranges), which covers only the heap the game set up (gmem_heap_size; Metal has the whole
// range in one buffer and uses its top). So the ring sits right after the game's heap and gmem_heap_size includes it:
// the one-buffer import takes it in, lazy mode imports its chunk when a copy is first bound (gmem_chunk_bind touches
// the effective addresses), gfast copies from it. Windows commits it (a userptr import needs committed pages).
// The ring is two halves: copies fill one; when it is full the submission pending then is noted for it and the other
// half is taken, after waiting for the submission noted when that half was left - so its freed slots are idle. Live
// copies stay where they are across switches (the allocator walks past them; rs_copy_make), so the GPU's own writes
// into a copy survive; a copy unused for 10 s is released. At ~100 KB a frame that wait is long satisfied; a half
// filling within one submission (not a real case) binds the original for that draw.
// LARECOMP_OVERLAP_SNAP=0: off (everything in place, as before: shows the race).
#define SNAP_HALF (8ull << 20)
#define SNAP_RING (2 * SNAP_HALF)
enum { RS_PROBE, RS_COPY, RS_PLACE };
typedef struct { uint64_t addr, size, copy, pristine, hash, frame; uint32_t gen, level, wgen; uint8_t mode; bool cs_bound, draw_bound; } RangeState;  // (wgen: watch_check; pristine: rs_copy_make)
static RangeState rs_tab[512];
static uint64_t rs_store_frame[512];  // (watch: the frame of the previous CPU write seen on a range's own pages)
static int nrs;
static int snap_state = -1;        // -1 not decided (the heap not set up yet), 0 off, 1 on
static uint64_t snap_base;         // heap offset of the ring (on)
static uint64_t snap_off;          // used bytes of the current half
static int snap_half;              // the half being filled
static uint64_t snap_half_seq[2];  // the submission pending when each half was last left (its copies may be in use until then)
static uint32_t snap_gen = 1;      // (bumped at a half switch: older copies are never bound again)
static uint32_t rs_level_gen = 1;
static Range sb_eff[6][16];        // the current draw's / dispatch's storage table with the copies' addresses (snap_apply)
static bool sb_eff_on;
static inline const Range *sbufs(int stage) { return sb_eff_on ? sb_eff[stage] : st.sbuf[stage]; }

static uint64_t range_hash(uint64_t addr, uint64_t size) {
    const uint64_t *w = (const uint64_t *)(uintptr_t)addr;
    uint64_t h = 1469598103934665603ull, n = size / 8;
    for (uint64_t i = 0; i < n; i++) h = (h ^ w[i]) * 1099511628211ull;
    return h;
}
static void snap_ring_import_tail(void);  // (with the lazy chunks below)
// Decided once the game's heap is set up (SetHeapSize), which is before anything is drawn: the ring follows the heap.
static bool snap_on(void) {
    if (snap_state >= 0) return snap_state;
    uint64_t heap = kernel_heap_size();
    if (!heap) return false;  // (not decided yet)
    const char *e = getenv("LARECOMP_OVERLAP_SNAP");
    snap_state = (!e || atoi(e) != 0) && overlap_on();
    if (snap_state) {
        snap_base = (heap + 0xFFFF) & ~0xFFFFull;  // (minImportedHostPointerAlignment: 64 KB at most)
        if (snap_base + SNAP_RING > HEAP_MAX) {
            snap_state = 0;
            LOG("[vk] storage snapshots off: the game's heap fills the range\n");
        } else {
            plat_commit(HEAP_BASE + snap_base, SNAP_RING);  // (Windows: backed before the import pins it)
            LOG("[vk] storage buffers the CPU rewrites: copies at %llx (%llu MB ring after the game's heap)\n",
                (unsigned long long)(HEAP_BASE + snap_base), (unsigned long long)(SNAP_RING >> 20));
            snap_ring_import_tail();  // (lazy chunks: one imported short of the ring already is imported again over it)
        }
    }
    return snap_state;
}
static uint64_t snap_ring_end(void) { return snap_on() ? snap_base + SNAP_RING : 0; }  // (heap offset; 0: no ring)
static void snap_reset(void) {  // (vk_exec_reset: a reboot - the heap is set up again, its pages replaced)
    snap_state = -1;
    snap_base = snap_off = 0;
    snap_half = 0;
    snap_half_seq[0] = snap_half_seq[1] = 0;
    snap_gen++;
    nrs = 0;
    sb_eff_on = false;
}
static void rs_level_tick(void) {  // (a new level: every range is probed again on its next use)
    extern const char *game_level_name(void);
    static char last[64];
    const char *lv = game_level_name();
    if (lv && strncmp(lv, last, sizeof last - 1)) { snprintf(last, sizeof last, "%s", lv); rs_level_gen++; }
}
static void rs_log(RangeState *e, const char *what, bool cs_slot, uint64_t f) {  // (rare: a new range, a decision)
    static int n;
    if (e->size >= 2040 ? n < 100000 : n++ < 200)  // (the small per-object ranges are many: capped)
        LOG("[snap] %llx+%llu %s (%s bind, frame %llu)\n", (unsigned long long)e->addr, (unsigned long long)e->size, what, cs_slot ? "compute" : "draw", (unsigned long long)f);
}
// A copy lives until its entry remakes it (the original changed) or releases it (unused for 10 s): a half switch no
// longer remakes every copy from its original, which lost the GPU's own writes into a copy - the exposure program's
// into the lists - for a frame (renderer.m 1ea4b16). The allocator moves forward through the current half and walks
// past live copies; a slot freed by a remake (the GPU may still read it) is reused only after the half is left and
// taken again, which waits for the submission pending when it was left - the same guarantee as before.
// The offset in the current half where `need` bytes fit from `at` on, skipping live copies (with their pristine
// images); ~0 when none
static uint64_t rs_ring_fit(uint64_t at, uint64_t need) {
    uint64_t half_lo = HEAP_BASE + snap_base + (uint64_t)snap_half * SNAP_HALF;
    for (bool moved = true; moved && at + need <= SNAP_HALF;) {
        moved = false;
        for (int i = 0; i < nrs; i++) {
            const RangeState *o = &rs_tab[i];
            if (!o->copy || o->copy < half_lo || o->copy >= half_lo + SNAP_HALF) continue;
            uint64_t olo = o->copy - half_lo, ohi = (olo + o->size * (o->pristine ? 2 : 1) + 255) & ~255ull;
            if (at < ohi && at + need > olo) { at = ohi; moved = true; }
        }
    }
    return at + need <= SNAP_HALF ? at : ~0ull;
}
// `need` bytes (256-aligned) of the ring past the live copies: their guest address, or 0 when none can be taken now
// (see the half switch above). A copy being remade is skipped too: the merge in rs_copy_make reads it.
static uint64_t ring_take(uint64_t need) {
    uint64_t at = rs_ring_fit((snap_off + 255) & ~255ull, need);
    if (at == ~0ull) {  // this half is full: the other one, once the GPU is done with what it held
        int other = snap_half ^ 1;
        uint64_t seq = snap_half_seq[other], done = vk_completed_seq();
        if (seq > done) {
            if (seq > renderer_issued_seq()) return 0;  // (still being recorded: can't be waited for - in place this once)
            wait_why = "storage snapshot ring";
            vk_wait_seq(seq);
        }
        snap_half_seq[snap_half] = vk_pending_seq();
        snap_half = other;
        snap_off = 0;
        at = rs_ring_fit(0, need);
        if (at == ~0ull) {  // (the half is all live copies: not in practice - 512 entries, most far smaller)
            static int warned;
            if (warned++ < 3) LOG("[snap] copy ring full: %llu bytes in place\n", (unsigned long long)need);
            return 0;
        }
    }
    snap_off = at + need;
    return HEAP_BASE + snap_base + (uint64_t)snap_half * SNAP_HALF + at;
}
// A copy a compute dispatch binds carries the GPU's own writes into it (the exposure program 72a8da6000 keeps its
// adaptation state in the 2040-byte exposure list it reads and writes; the lists' tile data beside it is the CPU's).
// When the CPU changes the original the copy is remade from it - and the GPU's state went back to whatever the
// original held when the copy was first taken: the screen at half brightness for one frame (the path by the Face
// Shrine; two frames three apart, one per list buffer; renderer.m 69a3524). So such a copy keeps a pristine image of
// what it was copied from right behind it, and a remake merges: a byte the GPU changed (copy != pristine) that the
// CPU didn't (original == pristine) keeps the GPU's value; everything else is the original's, the CPU's write winning
// where both changed (its write is the later one, as in place). Draw-only copies have no GPU state and no image.
// A copy of e's range in the ring: its guest address, or 0 when none can be made now
static uint64_t rs_copy_make(RangeState *e, uint64_t h) {
    bool keep = e->cs_bound;
    uint64_t old = e->copy, old_p = e->pristine;
    uint64_t at = ring_take((e->size * (keep ? 2 : 1) + 255) & ~255ull);
    if (!at) return 0;
    uint8_t *dst = (uint8_t *)(uintptr_t)at;
    const uint8_t *src = (const uint8_t *)(uintptr_t)e->addr;
    memcpy(dst, src, e->size);
    if (keep && old && old_p) {
        const uint8_t *oc = (const uint8_t *)(uintptr_t)old, *op = (const uint8_t *)(uintptr_t)old_p;
        uint64_t kept = 0;
        for (uint64_t i = 0; i < e->size; i++) if (oc[i] != op[i] && src[i] == op[i]) { dst[i] = oc[i]; kept++; }
        if (kept) {
            static int n;
            extern uint64_t renderer_exec_frame(void);
            if (n++ < 20 || getenv("LARECOMP_LOG_BINDS"))
                LOG("[snap] %llx+%llu remade: %llu bytes of the GPU's kept (frame %llu)\n", (unsigned long long)e->addr,
                    (unsigned long long)e->size, (unsigned long long)kept, (unsigned long long)renderer_exec_frame());
        }
    }
    e->copy = at;
    if (keep) { e->pristine = at + e->size; memcpy((void *)(uintptr_t)e->pristine, src, e->size); }
    else e->pristine = 0;
    e->hash = h, e->gen = snap_gen;
    stat_snap_bytes += e->size;
    stat_snap_copies++;
    return e->copy;
}
static void rs_release_unused(uint64_t f) {  // (every 600 frames) copies of entries unbound for 10 s go back to the ring
    static uint64_t last;
    if (f - last < 600) return;
    last = f;
    for (int i = 0; i < nrs; i++) if (rs_tab[i].copy && f - rs_tab[i].frame > 600) rs_tab[i].copy = rs_tab[i].pristine = 0;
}
// Write watch (compute-only ranges; renderer.m cf98fff / c3580a8). The CPU's rewrite of a range can't be seen in its
// bytes (the light lists are rewritten with the same bytes while the view stands still, and the race is the GPU reading
// them mid-rewrite), so a range bound only by compute slots starts in place and the OS's page write tracking tells,
// at its first bind of each frame, whether the CPU wrote any of its pages since the last look: then it is copied from
// then on. The tracking is the vertex cache's and the texture upload checks' (plat_written_pages, the shared page
// generations vc_page_gen): Windows GetWriteWatch over the guest window reserved with MEM_WRITE_WATCH, Linux the
// userfaultfd asynchronous write-protect bits read with PAGEMAP_SCAN. Neither changes a page's protection, and
// neither sees the GPU's own writes (DMA sets no dirty bit): a GPU output stays in place. The Metal backend
// write-protects the pages and catches the store in the fault handler instead; on Windows that (VirtualProtect on
// pages imported with VK_EXT_external_memory_host, NVIDIA) left the GPU reading wrong data - the picture at half
// brightness, grass and HUD digits missing - so the Vulkan backend never changes the protection of imported memory.
// Where the OS gives no tracking (Linux over a userptr or dma-buf heap: the tracking is off there, a kernel without
// PAGEMAP_SCAN, a Windows window reserved without write watch) the one-frame hash probe is the fallback, logged.
// Page granularity (4 KB; 16 KB and 64 KB kernels on arm64 Linux as the kernel reports): a store into a neighbour on
// the same page marks the range too (the lists share a page) - copied, the safe side - except a page shared with a
// small parameter block (watch_check), and a single write (an initialisation) doesn't copy (snap_storage).
// LARECOMP_OVERLAP_WATCH=0: off (compute-only ranges get the one-frame hash probe, as before).
static bool heap_is_userptr(void);  // (with the heap import below)
static int pages_cpu_written(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);  // (the page tables below)
#define WATCH_PAGE 4096ull  // (the page tables' page, VC_PAGE below)
static bool watch_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_OVERLAP_WATCH");
        uint32_t g;
        on = !e || atoi(e) != 0;
        if (on && pages_cpu_written(HEAP_BASE, 4096, 0, &g) < 0) {
            on = 0;
            LOG("[vk] storage write watch off: no page write tracking%s (the hash probe instead)\n", heap_is_userptr() ? " over a userptr heap import" : "");
        } else LOG("[vk] storage write watch %s (compute-bound ranges copied once the OS's page write tracking sees a CPU write; no page protection)\n",
                   on ? "on" : "off (LARECOMP_OVERLAP_WATCH=0)");
    }
    return on;
}
// Did the CPU write any page of the range since the range last looked (e->wgen)? The look stamps the pages (every
// look resets the OS bits, so the generation table carries what other lookers saw). 1 written, 0 not, -1 no tracking.
// A page dirty bit can't tell which range on the page was stored to. A store into a small parameter block (the
// skinning dispatch's 12-byte slot 5, rewritten every frame) says nothing about the buffers beside it: counted as
// theirs, it copied BowWow's skinning output - a GPU output the CPU reads back every frame (his chain) - so the
// dispatch wrote the copy, the CPU read the frozen original, and his body came apart as a companion indoors (Metal
// 1ea4b16). So a range of 256 bytes or more counts only its dirty pages that no watched range under 256 bytes shares;
// the light / exposure lists (32640 / 2040 bytes, one allocation the CPU memcpys) share pages only with each other
// and keep their coverage. A small range counts every dirty page of its own.
static int watch_check(RangeState *e) {
    uint32_t since = e->wgen, now = e->wgen;
    int w = pages_cpu_written(e->addr, e->size, since, &now);
    e->wgen = now;
    if (w != 1 || e->size < 256) return w;
    for (uint64_t plo = e->addr & ~(WATCH_PAGE - 1); plo < e->addr + e->size; plo += WATCH_PAGE) {
        bool small_mate = false;
        for (int k = 0; k < nrs && !small_mate; k++) {
            const RangeState *o = &rs_tab[k];
            if (o != e && o->size < 256 && o->cs_bound && o->addr < plo + WATCH_PAGE && plo < o->addr + o->size) small_mate = true;
        }
        if (small_mate) continue;
        uint32_t pg_now;  // (this page written since the range's last look? The look above stamped it: no OS bits left)
        if (pages_cpu_written(plo, WATCH_PAGE, since, &pg_now) == 1) return 1;  // (a dirty page of its own)
    }
    return 0;  // (only pages shared with small parameter blocks were written: theirs)
}

// The address a binder of [addr, addr+size) uses: the original or its copy (cs_slot: bound by a compute dispatch)
static uint64_t snap_storage(uint64_t addr, uint64_t size, bool cs_slot) {
    if (!snap_on() || !addr || !size || addr < HEAP_BASE || addr - HEAP_BASE + size > snap_base) return addr;
    if (size > SNAP_HALF / 2) {  // (the tile lists grow with the render resolution: ~1.2 MB at 7680x4320; a half holds two of these)
        static int logged;
        if (logged++ < 4) LOG("[vk] storage range %llx+%llu too large for a snapshot: read in place\n", (unsigned long long)addr, (unsigned long long)size);
        return addr;
    }
    if (kernel_heap_size() > snap_base) {  // (the game grew its heap into the ring: off from now on)
        static bool logged;
        if (!logged) { logged = true; LOG("[vk] storage snapshots off: the game's heap grew into the ring\n"); }
        snap_state = 0;
        return addr;
    }
    extern int nvn_pool_flags_at(uint64_t addr);
    int pf = nvn_pool_flags_at(addr);
    if (pf < 0 || (pf & 1)) return addr;  // (CPU_NO_ACCESS: static data, nothing to race with)
    rs_level_tick();
    extern uint64_t renderer_exec_frame(void);
    uint64_t f = renderer_exec_frame();
    rs_release_unused(f);
    // A draw's storage read from the game's per-frame pools (CPU_UNCACHED | GPU_UNCACHED, 0x12: the 64-byte per-object
    // blocks at changing addresses, hundreds a frame) is copied without an entry: a fresh range every frame filled the
    // table in a few minutes, and the table starting over remade every copy from its original - the exposure list's
    // GPU state with it (one dark frame per list buffer, see rs_copy_make). Its slot is free again at once: reused only
    // after the half is left and taken again (the wait in ring_take), as a remade copy's old slot.
    if (!cs_slot && (pf & 0x13) == 0x12 && size <= 65536) {
        uint64_t at = ring_take((size + 255) & ~255ull);
        if (!at) return addr;
        memcpy((void *)(uintptr_t)at, (const void *)(uintptr_t)addr, size);
        stat_snap_bytes += size;
        stat_snap_copies++;
        return at;
    }
    RangeState *e = NULL;
    for (int i = 0; i < nrs; i++) if (rs_tab[i].addr == addr && rs_tab[i].size == size) { e = &rs_tab[i]; break; }
    uint64_t c;
    if (!e) {
        int k = nrs;
        if (nrs == 512) {  // table full: the entry bound longest ago makes room (never the whole table at once)
            k = 0;
            for (int i = 1; i < nrs; i++) if (rs_tab[i].frame < rs_tab[k].frame) k = i;
            rs_log(&rs_tab[k], "evicted (table full, bound longest ago)", cs_slot, f);
        } else nrs++;
        e = &rs_tab[k];
        memset(e, 0, sizeof *e);
        rs_store_frame[e - rs_tab] = 0;
        e->addr = addr, e->size = size, e->mode = RS_PROBE, e->level = rs_level_gen, e->frame = f;
        e->cs_bound = cs_slot;
        if (!cs_slot) e->mode = RS_COPY, e->draw_bound = true;  // a draw's storage read: copied for good (see above)
        else if (watch_on()) {  // a compute-only range: in place; the first CPU write its pages' tracking shows makes it a copy
            e->mode = RS_PLACE;
            watch_check(e);  // (from now: the look stamps its pages)
            rs_log(e, "new: in place, CPU writes tracked", cs_slot, f);
            return addr;
        }
        // the probe frame (or the first copy): every binder this frame takes this copy (frame == f below); a probe's
        // decision is at the next frame's use, when a CPU rewrite can have happened
        c = rs_copy_make(e, range_hash(addr, size));
        if (!c) {  // (no copy possible now: the entry is dropped - not probed yet - and the range is created again next time)
            rs_log(e, "no copy now (the ring's other half is still in flight): in place this once", cs_slot, f);
            if (k != --nrs) { rs_tab[k] = rs_tab[nrs]; rs_store_frame[k] = rs_store_frame[nrs]; }  // (an evicted slot: the last entry moves in)
            return addr;
        }
        rs_log(e, cs_slot ? "new: probe" : "new: copy (a draw's storage read)", cs_slot, f);
        return c;
    }
    if (cs_slot) e->cs_bound = true; else e->draw_bound = true;
    if (e->frame == f) {  // (bound again this frame)
        if (e->mode == RS_PLACE && cs_slot) return addr;
        if (!cs_slot && e->mode != RS_COPY) {  // (a draw's storage read of a range probed / settled in place this frame: copied for good)
            e->mode = RS_COPY;
            rs_log(e, "-> copy (a draw's storage read)", cs_slot, f);
            c = rs_copy_make(e, range_hash(addr, size));
            return c ? c : addr;
        }
        if (e->copy) return e->copy;
        c = rs_copy_make(e, e->hash);
        return c ? c : addr;
    }
    // probe again on a new level or after 10 s unused - only the hash fallback's in-place ranges: a copied range stays
    // copied (a draw's storage read, a CPU store seen), a watched one just gets its pages armed again
    bool again = !watch_on() && e->mode == RS_PLACE && (e->level != rs_level_gen || f - e->frame > 600);
    e->level = rs_level_gen;
    e->frame = f;
    if (!cs_slot && e->mode != RS_COPY) {  // a draw reads it as storage: copied for good (see above)
        e->mode = RS_COPY;
        rs_log(e, "-> copy (a draw's storage read)", cs_slot, f);
        c = rs_copy_make(e, range_hash(addr, size));
        return c ? c : addr;
    }
    if (e->mode == RS_PLACE && e->cs_bound && watch_on()) {  // (tracked: the OS's dirty bits decide, no hash)
        if (watch_check(e) != 1) return addr;
        // A write once (an initialisation: BowWow's skinning buffers at his spawn) stays in place; writes that keep
        // coming (within 8 frames of the previous one) are the CPU rewriting it: copied from now on (renderer.m 1ea4b16)
        int k = (int)(e - rs_tab);
        if (!rs_store_frame[k] || f - rs_store_frame[k] > 8) { rs_store_frame[k] = f; return addr; }
        e->mode = RS_COPY;
        rs_log(e, "-> copy (CPU writes keep coming on its own pages)", cs_slot, f);
        c = rs_copy_make(e, range_hash(addr, size));
        return c ? c : addr;
    }
    if (e->mode == RS_PLACE && !again && e->cs_bound) return addr;
    uint64_t h = range_hash(addr, size);
    if (again && !e->draw_bound) {  // (a draw-read range stays a copy across levels)
        e->mode = RS_PROBE;
        rs_log(e, "probe again (level / unused)", cs_slot, f);
        c = rs_copy_make(e, h);
        return c ? c : addr;
    }
    switch (e->mode) {
    case RS_PLACE:  // (never a compute slot's: a change can only be the CPU's)
        if (h == e->hash) return addr;
        e->mode = RS_COPY;
        rs_log(e, "in place -> copy (changed later)", cs_slot, f);
        break;
    case RS_PROBE:  // the use after the probe: did the CPU touch the original meanwhile?
        if (h == e->hash) { e->mode = RS_PLACE; e->copy = e->pristine = 0; rs_log(e, "probe -> in place (unchanged)", cs_slot, f); return addr; }
        e->mode = RS_COPY;
        rs_log(e, "probe -> copy (the CPU rewrote it)", cs_slot, f);
        break;
    default:  // RS_COPY
        if (h == e->hash && e->copy) return e->copy;  // unchanged: the copy (maybe GPU-written: its state) is current
        break;
    }
    c = rs_copy_make(e, h);
    return c ? c : addr;
}

// (debug) LARECOMP_LOG_BINDS=1: guest memory the GPU reads in place, per frame (bytes bound; distinct ranges once),
// and the ranges of the previous frame whose contents the game rewrote by the time this frame is translated: with
// the frame fence deferred (overlap) the GPU may still read them - the racing data. kind: 0 vertex, 1 index,
// 2 storage (a draw's read), 3 compute slot.
static void bind_note(int kind, uint64_t addr, uint64_t size) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_LOG_BINDS") != NULL;
    if (!on || addr < HEAP_BASE || addr - HEAP_BASE + size > HEAP_MAX || size < 8) return;
    extern int nvn_pool_flags_at(uint64_t addr);
    enum { REC = 8192 };
    static struct { uint64_t addr, size, hash; uint8_t kind; } rec[REC];
    static int nrec;
    static uint64_t frame, all[4], uniq[4], wr[4];
    static uint32_t n[4];
    extern uint64_t renderer_exec_frame(void);
    uint64_t f = renderer_exec_frame();
    if (f != frame) {
        // the previous frame's ranges, rewritten since?
        uint64_t chg[4] = {0}, chgn[4] = {0};
        int shown = 0;
        for (int i = 0; i < nrec; i++) {
            if (range_hash(rec[i].addr, rec[i].size) == rec[i].hash) continue;
            chg[rec[i].kind] += rec[i].size; chgn[rec[i].kind]++;
            if (frame % 120 == 0 && shown++ < 12)
                LOG("[race] frame %llu %s %llx+%llu pool flags %#x\n", (unsigned long long)frame,
                    rec[i].kind == 0 ? "vertex" : rec[i].kind == 1 ? "index" : rec[i].kind == 2 ? "storage" : "compute",
                    (unsigned long long)rec[i].addr, (unsigned long long)rec[i].size, nvn_pool_flags_at(rec[i].addr));
        }
        if (frame % 120 == 0 || chgn[0] + chgn[1] + chgn[2] + chgn[3])
            LOG("[binds] frame %llu: vertex %llu KB (%llu KB distinct, %u binds), index %llu KB (%llu), storage %llu KB (%llu, %u), compute %llu KB (%llu) | in CPU-writable pools: %llu/%llu/%llu/%llu KB | rewritten since: vertex %llu KB/%llu, index %llu KB/%llu, storage %llu KB/%llu, compute %llu KB/%llu\n",
                (unsigned long long)frame, (unsigned long long)(all[0] >> 10), (unsigned long long)(uniq[0] >> 10), n[0], (unsigned long long)(all[1] >> 10), (unsigned long long)(uniq[1] >> 10),
                (unsigned long long)(all[2] >> 10), (unsigned long long)(uniq[2] >> 10), n[2], (unsigned long long)(all[3] >> 10), (unsigned long long)(uniq[3] >> 10),
                (unsigned long long)(wr[0] >> 10), (unsigned long long)(wr[1] >> 10), (unsigned long long)(wr[2] >> 10), (unsigned long long)(wr[3] >> 10),
                (unsigned long long)(chg[0] >> 10), (unsigned long long)chgn[0], (unsigned long long)(chg[1] >> 10), (unsigned long long)chgn[1],
                (unsigned long long)(chg[2] >> 10), (unsigned long long)chgn[2], (unsigned long long)(chg[3] >> 10), (unsigned long long)chgn[3]);
        nrec = 0; memset(all, 0, sizeof all); memset(uniq, 0, sizeof uniq); memset(n, 0, sizeof n); memset(wr, 0, sizeof wr);
        frame = f;
    }
    all[kind] += size; n[kind]++;
    for (int i = 0; i < nrec; i++) if (rec[i].addr == addr && rec[i].size == size) return;
    if (nrec == REC) return;
    rec[nrec].addr = addr; rec[nrec].size = size; rec[nrec].kind = (uint8_t)kind;
    rec[nrec].hash = range_hash(addr, size);
    nrec++;
    uniq[kind] += size;
    int pf = nvn_pool_flags_at(addr);
    if (pf < 0 || !(pf & 1)) wr[kind] += size;  // (distinct bytes in pools the CPU can write: what a snapshot would copy)
}

// Before a draw (stages 0 and 1: the slots the shader reads addresses of, si->ssbo_read) or a dispatch (stage 5: every
// bound slot): the stage's effective storage table, with the copies' addresses where a range is snapshotted. Every
// reader of the table in the draw / dispatch (drv_dedupe, cbuf_info, gmem_direct_pick, gmem_chunk_bind, gfast_prepare,
// vc_mark_sbufs) takes sbufs(stage); st.sbuf itself keeps the game's bindings (rehashed next frame).
static void snap_apply(int stage, uint32_t read_mask) {
    bool on = snap_on();
    for (int i = 0; i < 16; i++)
        if (st.sbuf[stage][i].addr && ((read_mask >> i) & 1)) bind_note(stage == 5 ? 3 : 2, st.sbuf[stage][i].addr, st.sbuf[stage][i].size);
    if (!on) { sb_eff_on = false; return; }
    sb_eff_on = true;
    memcpy(sb_eff[stage], st.sbuf[stage], sizeof sb_eff[stage]);
    for (int i = 0; i < 16; i++) {
        Range *r = &sb_eff[stage][i];
        if (r->addr && ((read_mask >> i) & 1)) r->addr = snap_storage(r->addr, r->size, stage == 5);
    }
}

// The driver constants of a graphics stage (cbuf 0, size bytes): zero but for the stage's storage buffer table at
// base (16 bytes an entry: address low, high, size). Compared by the entries within size.
static VkSpan drv_dedupe(int stage, uint64_t size) {
    static struct { uint64_t seq, size; int stage; Range sb[16]; VkSpan span; } last[2];
    int slot = stage == 1;
    uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
    int ne = size > base ? (int)((size - base + 15) / 16) : 0;
    if (ne > 16) ne = 16;
    uint64_t seq = vk_pending_seq();
    const Range *sb = sbufs(stage);  // (overlap: a snapshotted range's copy)
    if (last[slot].seq == seq && last[slot].size == size && last[slot].stage == stage &&
        !memcmp(last[slot].sb, sb, (size_t)ne * sizeof(Range))) {
        prof_cb_reused++;
        return last[slot].span;
    }
    uint32_t drv[0x800 / 4];
    memset(drv, 0, size);
    for (int i = 0; i < ne; i++) {  // (an entry may run past size: drv is 0x800 bytes, only size of them are copied)
        uint32_t *e = &drv[(base + 16 * i) / 4];
        e[0] = (uint32_t)sb[i].addr;
        e[1] = (uint32_t)(sb[i].addr >> 32);
        e[2] = (uint32_t)sb[i].size;
    }
    VkSpan s = vk_transient_copy(drv, size, 256);
    last[slot].seq = seq; last[slot].size = size; last[slot].stage = stage; last[slot].span = s;
    memcpy(last[slot].sb, sb, (size_t)ne * sizeof(Range));
    prof_cb_copied++;
    return s;
}

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        // The driver constants are made from the stage's buffer bindings (and a dispatch's group size) alone: a draw
        // binding the same ones as the previous draw of its kind, in the same submission, shares that copy. Comparing
        // those few hundred bytes replaced building the 2 KB table and comparing it whole, twice a draw (7% of the
        // render thread, the one that limits the frame rate on slower CPUs).
        uint64_t size = si->cbuf_size[0] > 0x800 ? 0x800 : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (stage != 5 && cbuf_lean()) return ubo_span(drv_dedupe(stage, size), size);
        if (size < 0x500 && stage == 5) size = 0x500;
        const Range *sb = sbufs(stage);  // (overlap: a snapshotted range's copy)
        struct { uint64_t size, sb[16][2], ub[16][2]; uint32_t g[4]; } key;
        memset(&key, 0, sizeof key);
        key.size = size;
        for (int i = 0; i < 16; i++) key.sb[i][0] = sb[i].addr, key.sb[i][1] = sb[i].size;
        if (stage == 5) {
            for (int i = 0; i < 16; i++) key.ub[i][0] = st.ubuf[5][i].addr, key.ub[i][1] = st.ubuf[5][i].size;
            key.g[0] = gx, key.g[1] = gy, key.g[2] = gz;
        }
        static struct { uint64_t seq; __typeof__(key) key; VkSpan span; } last[3];
        int slot = stage == 5 ? 2 : stage == 1 ? 1 : 0;
        uint64_t seq = vk_pending_seq();
        if (last[slot].seq == seq && !memcmp(&last[slot].key, &key, sizeof key)) return ubo_span(last[slot].span, size);
        uint32_t drv[0x800 / 4] = {0};
        if (si->uses_gmem) {  // storage buffers outside the guest memory window: their shader accesses read zero
            static int nlog;
            for (int i = 0; i < 16 && nlog < 30; i++) {
                uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
                if (a && (a < gwin_base || a + sz > gwin_base + gwin_max)) {
                    nlog++;
                    LOG("[vk] storage buffer outside the gmem window: stage %d sb%d %llx+%llx (program %llx)\n", stage, i,
                        (unsigned long long)a, (unsigned long long)sz, (unsigned long long)st.program->stages[stage].code_addr);
                }
            }
        }
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)sb[i].addr; e[1] = (uint32_t)(sb[i].addr >> 32); e[2] = (uint32_t)sb[i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)sb[i].addr;
                e[1] = (uint32_t)(sb[i].addr >> 32);
                e[2] = (uint32_t)sb[i].size;
            }
        }
        VkSpan sp = vk_transient_copy(drv, size, 256);
        last[slot].seq = seq, last[slot].key = key, last[slot].span = sp;
        return ubo_span(sp, size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    // (lazy heap chunks) pointers in it import their chunks: scanned once per shared copy (LARECOMP_VK_NO_SCAN_ONCE=1:
    // at every draw, as before)
    static int scan_once = -1;
    if (scan_once < 0) scan_once = getenv("LARECOMP_VK_NO_SCAN_ONCE") == NULL;
    if (gc_lazy && si->uses_gmem && scan_once) return ubo_span(guest_copy_ex(r.addr, size, true), size);
    if (gc_lazy && si->uses_gmem) gc_scan_pointers((const uint32_t *)(uintptr_t)r.addr, size);
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = gwin_base): the guest memory window imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// LARECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;
// LARECOMP_VK_GMEM_WINDOW=<MB>: import only the top <MB> of the heap instead of all of it. On the Steam Deck the 3.2 GB host-pointer (userptr) import is
// revalidated by amdgpu at every submit: 1.75 ms x 75 submits a frame, 87 % of the render thread (6.7 fps; 21 fps without the import). Z-A's compute
// storage buffers all sit in an 8 MB spot near the heap's end. gmem_lo: heap offset of the import's first byte (0: the whole heap).
static uint64_t gmem_lo, gmem_window;
#define GM_IN(a, sz) (gmem_buf && (a) >= gwin_base + gmem_lo && (a) + (sz) <= gwin_base + gmem_lo + gmem_size)
#define GM_OFF(a) ((a) - gwin_base - gmem_lo)
// The window is anchored at the end of the physical region (gwin_max: constant for the session, so shaders may bake it): [lo, gwin_max).
// Only with a physical-memory window (no heap), else the whole heap as before. The generated shader code computes ga = addr - base, ok = ga < size,
// so translate_program / build_compute set base and size to the window too (the window changed them by lo: text and buildings read the wrong place).
// default: on an integrated GPU (the Steam Deck: the full import cost 87 % of the render thread), 256 MB; LARECOMP_VK_GMEM_WINDOW=0 imports it all
static void gmem_window_init(void) {
    const char *w = ENV("LARECOMP_VK_GMEM_WINDOW");
    gmem_window = w ? strtoull(w, NULL, 10) << 20 : g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 256ull << 20 : 0;
}
static void gmem_window_get(uint64_t *lo, uint64_t *len) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;  // (the translator and the render thread - run_queued_now - both ask)
    pthread_once(&once, gmem_window_init);
    if (!gmem_window || !gwin()->phys || gwin_max <= gmem_window) { *lo = 0; *len = gwin_max; return; }
    *lo = (gwin_max - gmem_window) & ~0xFFFFFull;
    *len = gwin_max - *lo;
}

// Host memory [host, host + size) as a buffer (VK_EXT_external_memory_host; page-aligned). The allocation's result:
// out of memory tells whole_import_probe_oom to go lazy.
static VkResult host_import_ptr(void *host, uint64_t size, VkBuffer *out_buf, VkDeviceMemory *out_mem, bool bda) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &mp) != VK_SUCCESS || !mp.memoryTypeBits)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = host};
    VkMemoryAllocateFlagsInfo fl = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &imp, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = bda ? (void *)&fl : (void *)&imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) return r;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | (bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0)};
    VkBuffer buf;
    r = vkCreateBuffer(g_vk.dev, &bi, NULL, &buf);
    if (r != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return r; }
    r = vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (r != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, buf, NULL); vkFreeMemory(g_vk.dev, mem, NULL); return r; }
    *out_buf = buf;
    *out_mem = mem;
    return VK_SUCCESS;
}
// Heap bytes [off, off + size) as a buffer (a page-aligned range of the mapped heap).
static bool host_import(uint64_t off, uint64_t size, VkBuffer *out_buf, VkDeviceMemory *out_mem, bool bda) {
    return host_import_ptr((void *)(uintptr_t)(HEAP_BASE + off), size, out_buf, out_mem, bda) == VK_SUCCESS;
}

// One import of the whole heap, tried once at the start where it should fit but not by much (device.c
// whole_import_fits: g_vk.heap_probe): HEAP_MAX of scratch host memory imported and used by one submission, then
// freed. True: it ran out of memory (the import or the submission: issue #205's driver took the import and failed
// the first submission), and the heap is imported in lazy chunks instead - decided here, at device creation (vk_exec_init),
// before any shader is translated or the game starts. Costs: the import pins HEAP_MAX of RAM for a moment (well under a
// second). Any other failure: logged, and the heap is imported as one buffer as before.
// LARECOMP_VK_SIM_HEAP_OOM=1: the probe runs, then reports out of memory (tests the fallback on any GPU).
static bool whole_import_probe_oom(void) {
    const uint64_t size = HEAP_MAX, align = 64 << 10;  // (minImportedHostPointerAlignment: 4 KB here, 64 KB at most)
    const char *sim = getenv("LARECOMP_VK_SIM_HEAP_OOM");
    rec_drain();  // (the queue below is the recorder's: idle and empty while this runs)
    uint64_t t0 = plat_now_ns();
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    const char *step = "host memory";
    void *raw = malloc(size + align);
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    if (raw) {
        step = "import";
        r = host_import_ptr((void *)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1)), size, &buf, &mem, false);
    }
    if (r == VK_SUCCESS) {  // a submission that uses it: where the driver makes it resident
        step = "submission";
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qfam};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkCommandBuffer cb = VK_NULL_HANDLE;
        r = vkCreateCommandPool(g_vk.dev, &pi, NULL, &pool);
        if (r == VK_SUCCESS) r = vkCreateFence(g_vk.dev, &fi, NULL, &fence);
        if (r == VK_SUCCESS) {
            VkCommandBufferAllocateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
            r = vkAllocateCommandBuffers(g_vk.dev, &ci, &cb);
        }
        if (r == VK_SUCCESS) {
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            vkBeginCommandBuffer(cb, &bi);
            vkCmdFillBuffer(cb, buf, 0, 256, 0);         // (zeros into zeroed memory: the start and the end of it)
            vkCmdFillBuffer(cb, buf, size - 256, 256, 0);
            r = vkEndCommandBuffer(cb);
        }
        if (r == VK_SUCCESS) {
            VkCommandBufferSubmitInfo cbs = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
            VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbs};
            r = vkQueueSubmit2(g_vk.queue, 1, &si, fence);
            if (r == VK_SUCCESS) r = vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, 10000000000ull);
        }
    }
    if (r == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
    if (fence) vkDestroyFence(g_vk.dev, fence, NULL);
    if (pool) vkDestroyCommandPool(g_vk.dev, pool, NULL);
    if (buf) vkDestroyBuffer(g_vk.dev, buf, NULL);
    if (mem) vkFreeMemory(g_vk.dev, mem, NULL);
    free(raw);
    bool oom = r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY;
    if (r == VK_SUCCESS && sim && atoi(sim)) { oom = true; r = VK_ERROR_OUT_OF_DEVICE_MEMORY; step = "simulated (LARECOMP_VK_SIM_HEAP_OOM)"; }
    unsigned ms = (unsigned)((plat_now_ns() - t0) / 1000000);
    if (oom)
        LOG("[vk] guest heap: one import of %llu MB ran out of memory (%s, error %d, %u ms): falling back to chunks "
            "imported as the game binds them\n", (unsigned long long)(size >> 20), step, r, ms);
    else if (r != VK_SUCCESS)
        LOG("[vk] guest heap: one import of %llu MB failed to test (%s, error %d, %u ms): one buffer as before\n",
            (unsigned long long)(size >> 20), step, r, ms);
    else
        LOG("[vk] guest heap: one import of %llu MB works here (%u ms)\n", (unsigned long long)(size >> 20), ms);
    return oom;
}

// Guest window bytes [lo, lo + size) for shader global memory (gmem_info; the window: gwin - the heap, or Pokémon Z-A's
// physical memory)
static bool gmem_import(uint64_t lo, uint64_t size) {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkResult r = host_import_ptr((void *)(uintptr_t)(gwin_base + lo), size, &buf, &mem, false);
    if (r != VK_SUCCESS) { LOG("[vk] gmem import: %d (%llu MB)\n", r, (unsigned long long)(size >> 20)); return false; }
    if (gmem_buf) vk_defer_buffer(gmem_buf, gmem_mem);  // the heap grew: the pending submission may still use the old import
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    gmem_lo = lo;
    LOG("[vk] guest heap imported for shader global memory: %llu MB from heap offset %llu MB\n", (unsigned long long)(size >> 20), (unsigned long long)(lo >> 20));
    return true;
}

// Linux, AMD (RADV): the import above is a userptr buffer, and amdgpu checks every page of a userptr buffer again on
// each submission - 3.2 GB of heap, ~45 ms of driver time a frame on a ROG Ally X, Steam Deck, Radeon laptops (issues
// #40, #51). Here the heap is instead allocated by the driver (cached system memory, exported as a dma-buf) and that
// memory is mapped at HEAP_BASE before the game starts, so the GPU sees an ordinary buffer. Called from boot (main.c)
// on the main thread before any guest thread exists; a reboot frees it with gmem_buf (vk_exec_reset) and the guest
// window's reset unmaps it, so the next boot gets fresh zeroed memory. Costs: the whole HEAP_MAX is committed once the
// GPU first uses it, and Linux write tracking can't watch this mapping (the heap is then re-checked like on Beta 4).
// Nothing writes the heap before boot (the start screen doesn't use guest memory): the mapping replaces it unseen.
// The mapping: where the driver has VK_EXT_map_memory_placed (RADV, Mesa 24.1+), it maps the memory at HEAP_BASE
// itself, through its own DRM file as for any vkMapMemory. Else the memory is exported as a dma-buf and that is
// mmapped there - which some systems refuse (issue #92: RX 6800 XT, Mesa 25.0.7, "Permission denied"), and then
// the whole heap was a userptr import again.
// Default on for RADV; LARECOMP_VK_HEAP_DMABUF=0 off, =1 on any Linux driver with dma-buf export or placed maps.
// LARECOMP_VK_HEAP_PLACED=0: the dma-buf mapping even where placed maps exist (tests).
#ifdef __linux__
static bool gmem_dmabuf;  // the heap is the driver's memory mapped at HEAP_BASE
static bool gmem_placed;  // ... by the driver (vkMapMemory2KHR, placed): unmapped through it at a reboot

// The driver maps `mem` (HEAP_MAX, one allocation) at HEAP_BASE. NULL, or why not; HEAP_BASE is ordinary guest
// memory again after a failure.
static const char *heap_map_placed(VkDeviceMemory mem) {
#ifndef VK_EXT_map_memory_placed
    (void)mem;
    return "built without VK_EXT_map_memory_placed";  // (g_vk.map_placed is never set then)
#else
    PFN_vkMapMemory2KHR map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(g_vk.dev, "vkMapMemory2KHR");
    PFN_vkUnmapMemory2KHR unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(g_vk.dev, "vkUnmapMemory2KHR");
    if (!map2 || !unmap2) return "no vkMapMemory2KHR";
    // First mapped anywhere, to know the driver can map it at all: a placed map replaces what's at HEAP_BASE before
    // the driver's own mmap can fail
    VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .memory = mem, .offset = 0, .size = VK_WHOLE_SIZE};
    VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem};
    void *p = NULL;
    if (map2(g_vk.dev, &mi, &p) != VK_SUCCESS || !p) return "the driver can't map it";
    unmap2(g_vk.dev, &ui);
    VkMemoryMapPlacedInfoEXT pl = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = (void *)(uintptr_t)HEAP_BASE};
    mi.flags = VK_MEMORY_MAP_PLACED_BIT_EXT;
    mi.pNext = &pl;
    p = NULL;
    VkResult r = map2(g_vk.dev, &mi, &p);
    if (r == VK_SUCCESS && p == (void *)(uintptr_t)HEAP_BASE) return NULL;
    if (r == VK_SUCCESS) unmap2(g_vk.dev, &ui);
    // (the old mapping may be gone: plain zeroed memory again, registered for write tracking like a reboot's)
    if (!plat_reset_guest(HEAP_BASE, HEAP_BASE + HEAP_MAX)) FATAL("cannot restore the guest heap mapping\n");
    return "the driver's placed mapping failed";
#endif
}

// The driver memory heap at a reboot (vk_exec_reset, GPU idle, guest threads stopped): the driver's mapping is left
// as a reserved range (or unmapped), then HEAP_BASE gets fresh zeroed guest memory at once, so no hole is left in the
// guest window until the window's reset.
static void heap_unmap_placed(void) {
    if (!gmem_placed) return;
    PFN_vkUnmapMemory2KHR unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(g_vk.dev, "vkUnmapMemory2KHR");
    VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = gmem_mem,
                               .flags = 0};
#ifdef VK_EXT_map_memory_placed
    if (g_vk.unmap_reserve) ui.flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT;
#endif
    if (unmap2) unmap2(g_vk.dev, &ui);
    gmem_placed = false;
}

static bool guest_heap_dmabuf(void) {
    if (gmem_buf || !g_vk.dev || !(g_vk.dmabuf || g_vk.map_placed) || getenv("LARECOMP_VK_NO_GMEM")) return false;
    const char *e = getenv("LARECOMP_VK_HEAP_DMABUF");
    if (e ? !atoi(e) : !strstr(g_vk.props.deviceName, "RADV")) return false;
    const uint64_t size = HEAP_MAX;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT;  // (cached: the game's CPU work runs in it)
    int type = -1;
    uint64_t largest = 0;  // (the largest heap with a cached host-visible type, for the log when none is big enough)
    for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; i++) {
        if ((mp.memoryTypes[i].propertyFlags & want) != want) continue;
        uint64_t hs = mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size;
        if (hs >= size) type = (int)i;
        else if (hs > largest) largest = hs;
    }
    if (type < 0) {
        if (largest)  // (Steam Deck: a 3072 MB system heap)
            LOG("[vk] heap as driver memory: the driver's cached system memory heap (%llu MB) is smaller than the guest heap "
                "(%llu MB)\n", (unsigned long long)(largest >> 20), (unsigned long long)(size >> 20));
        else LOG("[vk] heap as driver memory: no cached host-visible memory type\n");
        return false;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    const char *pe = getenv("LARECOMP_VK_HEAP_PLACED");
    bool placed = g_vk.map_placed && (!pe || atoi(pe));
    if (!placed && !g_vk.dmabuf) { LOG("[vk] heap as driver memory: no way to map it here\n"); return false; }
    bi.pNext = placed ? NULL : &eb;  // (an exportable buffer only for the dma-buf mapping)
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { LOG("[vk] heap as driver memory: buffer failed\n"); return false; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.dev, buf, &req);
    if (!(req.memoryTypeBits & (1u << type))) {
        LOG("[vk] heap as driver memory: memory type %d not allowed for the buffer\n", type);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        return false;
    }
    if (req.size != size) {  // (the mapping must cover exactly the heap: guest memory follows it)
        LOG("[vk] heap as driver memory: the allocation would be %llu bytes, not %llu\n", (unsigned long long)req.size,
            (unsigned long long)size);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        return false;
    }
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buf};
    VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &ded,
                                     .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = placed ? (void *)&ded : (void *)&ex,
                               .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) { LOG("[vk] heap as driver memory: allocation failed (%d)\n", r); vkDestroyBuffer(g_vk.dev, buf, NULL); return false; }
    const char *why = NULL;
    if (placed) {
        why = vkBindBufferMemory(g_vk.dev, buf, mem, 0) != VK_SUCCESS ? "binding failed" : heap_map_placed(mem);
        if (why) {
            LOG("[vk] heap as driver memory: placed mapping at %llx failed (%s): the heap is shared as before\n",
                (unsigned long long)HEAP_BASE, why);
            vkDestroyBuffer(g_vk.dev, buf, NULL);
            vkFreeMemory(g_vk.dev, mem, NULL);
            return false;
        }
        gmem_placed = true;
        goto mapped;
    }
    PFN_vkGetMemoryFdKHR get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryFdKHR");
    VkMemoryGetFdInfoKHR fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem,
                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    if (!get_fd || get_fd(g_vk.dev, &fi, &fd) != VK_SUCCESS || fd < 0) why = "no dma-buf for it";
    // First mapped anywhere, to know the driver lets it be mapped at all: MAP_FIXED unmaps what's at HEAP_BASE before
    // the driver is asked, so a refusal there would leave the heap unmapped (review of 2c27f3d)
    if (!why) {
        void *probe = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (probe == MAP_FAILED) why = strerror(errno);
        else munmap(probe, size);
        if (probe == MAP_FAILED) LOG("[vk] heap as driver memory: the dma-buf can't be mapped (%s)\n", why);
    }
    if (!why && mmap((void *)(uintptr_t)HEAP_BASE, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) !=
                    (void *)(uintptr_t)HEAP_BASE) {
        why = strerror(errno);
        // (the old mapping is gone: plain zeroed memory again, registered for write tracking like a reboot's)
        if (!plat_reset_guest(HEAP_BASE, HEAP_BASE + size)) FATAL("cannot restore the guest heap mapping\n");
    }
    if (fd >= 0) close(fd);  // (the mapping keeps the dma-buf)
    VkResult br = why ? VK_SUCCESS : vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (!why && br != VK_SUCCESS) {
        why = "binding failed";
        if (!plat_reset_guest(HEAP_BASE, HEAP_BASE + size)) FATAL("cannot restore the guest heap mapping\n");
    }
    if (why) {
        LOG("[vk] heap as driver memory: mapping at %llx failed (%s): the heap is shared as before\n",
            (unsigned long long)HEAP_BASE, why);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        vkFreeMemory(g_vk.dev, mem, NULL);
        return false;
    }
mapped:;
    // the game expects a zeroed heap; the kernel hands out zeroed pages, but 8 spread pages are checked to be sure
    static const uint8_t zero_page[4096];
    for (int i = 0; i < 8; i++)
        if (memcmp((const void *)(uintptr_t)(HEAP_BASE + size / 8 * i), zero_page, sizeof zero_page)) {
            LOG("[vk] heap as driver memory: not zeroed, clearing it\n");
            memset((void *)(uintptr_t)HEAP_BASE, 0, size);
            break;
        }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    gmem_dmabuf = true;
    LOG("[vk] guest heap as driver memory (%s, memory type %d): %llu MB at %llx - all of it is held in system memory "
        "once the GPU first uses it\n", gmem_placed ? "placed map" : "dma-buf", type, (unsigned long long)(size >> 20), (unsigned long long)HEAP_BASE);
    return true;
}
#endif

// The guest heap's GPU memory on Linux: the driver's own (above) where it can be, else host-pointer imports - the
// whole heap (gmem_import) or 16 MB chunks as draws first bind them (gc_lazy: RADV without the dma-buf heap, Steam
// Deck; GPUs whose storage buffers can't span the heap). On Mesa's drivers every such import is a userptr buffer, and
// page write tracking (platform/linux.c: userfaultfd write protection, PAGEMAP_SCAN) changes the protection of pages
// inside them: each tracking query over vertex or texture memory is an MMU-notifier invalidation of the imports it
// overlaps, so the kernel driver waits for the GPU to finish with them and checks their pages again at the next
// submission (amdgpu; the CPU and GPU stop overlapping - Steam Deck, issue #181: 30 fps on Beta 4, 20 with tracking
// over the whole-heap import). Chunks only make the re-check smaller, not the wait. So with any userptr import,
// tracking is off, as on Beta 4 (the vertex cache and the texture checks go without it). NVIDIA's own driver pins
// the pages instead and keeps it. Decided here, at boot, before any query protected a page. LARECOMP_WRITE_TRACKING=1:
// on anyway.
static bool gmem_userptr;  // the heap is imported by host pointer: userptr (vertex cache default, vcache_lookup)
static bool heap_is_userptr(void) { return gmem_userptr; }  // (the storage write watch: off over a userptr import)
static const char *heap_tracking_reason(void) {  // (overlap_on: why there is no page write tracking here)
    if (gmem_userptr) return "the guest heap is a userptr import, which the OS can't track";
#ifdef __linux__
    if (gmem_dmabuf) return "the guest heap is driver memory (dma-buf), which the OS can't track";
    return "no userfaultfd write tracking or PAGEMAP_SCAN (Linux 6.7+)";
#else
    return "the guest window has no write watch";
#endif
}
// The whole heap as one userptr import (not chunks) on AMD: each submission has amdgpu check all of its pages
static bool heap_whole_userptr(void) { return gmem_userptr && !gc_lazy && g_vk.props.vendorID == 0x1002; }

void backend_guest_heap_prepare(void) {
    gmem_userptr = false;
#ifdef __linux__
    guest_heap_dmabuf();
#endif
    if (!g_vk.dev) return;
    gmem_chunk_shift();  // (the chunk / lazy decision, before the first draw binds anything)
#ifdef __linux__
    // driver memory: write tracking can't watch that mapping (its queries over the heap would fail; PAGEMAP_SCAN
    // refuses a range without userfaultfd write protection), so it's off there too
    if (gmem_dmabuf) { plat_write_tracking_off("the guest heap is driver memory"); return; }
    if (!g_vk.host_import || getenv("LARECOMP_VK_NO_GMEM") || g_vk.props.vendorID == 0x10DE) return;
    gmem_userptr = true;
    plat_write_tracking_off(gc_lazy ? "the guest heap is imported in userptr chunks" : "the guest heap is a userptr import");
#endif
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = ENV("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import || gc_lazy;  // (lazy: chunks instead)
    uint64_t want = (gwin_want() + 0xFFFF) & ~0xFFFFull;
    if (want > gwin_max) want = gwin_max;
    uint64_t wlo, wlen;
    gmem_window_get(&wlo, &wlen);
    if (!gmem_window) wlo = 0;
    static int nlog;
    if (!nlog++) LOG("[vk] gmem: window %llu MB, import %s\n", (unsigned long long)(want >> 20), off ? "off" : "on");
    if (!off && want > gmem_lo + gmem_size && !gmem_import(wlo, want - wlo)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// ---- the guest heap for shaders in chunks (shader.h maxwell_gmem_chunk_shift)
// Chunks for two reasons:
// - a GPU whose storage buffers can't span HEAP_MAX (maxStorageBufferRange: 1 GB on Intel): binding one range past
//   that limit is out of spec, and the UHD 630's driver then read garbage beyond 1 GB: dark lighting, black objects
//   (issue #8). Lazy chunks as below (device.c lazy_heap_predicted); where they aren't possible (or with
//   LARECOMP_VK_GMEM_LAZY=0), windows of the one imported heap, one binding each;
// - lazy import (gmem_lazy): on Linux/AMD the import is a userptr buffer and amdgpu checks every page of such a buffer
//   again on each submission - 3.2 GB of heap, ~45 ms of driver time a frame (issues #40, #51). The dma-buf heap
//   (guest_heap_dmabuf) avoids that, but needs a cached host-visible memory type that can hold HEAP_MAX, and APUs
//   have none (Steam Deck: a 3072 MB system heap; Ryzen 3200G: 3172 MB, so the 3220 MB import is also over its GTT
//   budget: issues #40, #92). The GPU itself reads only a little of the heap in place: 73 MB over a four-area tour
//   (vertex and index data 67 MB, storage buffers 9 MB), 15-28 MB a frame, scattered over the whole 3.25 GB (measured
//   on the Metal backend, which binds the same ranges). So each 16 MB chunk is imported on its own when a draw or
//   dispatch first binds a range in it (23 chunks, 368 MB, over that tour), and only the imported chunks exist for the
//   driver to check: a fraction of the pages, under the GTT budget, and still the game's own memory (nothing to copy
//   back). Vertex and index data bind their chunk directly; shaders (LDG/STG) take the chunk's device address and
//   size from a table (GChunks, rewritten when a chunk is imported) and go through a buffer reference after a bounds
//   check: a chunk not imported has size 0, the access reads 0 / drops the store, and the shader counts it in GMiss
//   with the lowest word index hit: such a chunk (a pointer the game keeps in a uniform buffer, which no bind told us
//   about) is imported for the next frame, and the count is in the stats (after a chunk's first use it should stay 0).
//   Buffer device addresses are enabled only where this mode will be used (device.c lazy_heap_predicted): on RADV
//   they put every allocation on the driver's global BO list.
//   Default on RADV when the dma-buf heap doesn't apply, wherever one storage buffer can't span the heap and where the
//   system memory heap (or its budget) can't hold one import of it (8 GB laptops: issue #205), and where one import
//   was tried at the start and ran out of memory (whole_import_probe_oom; LARECOMP_VK_SIM_HEAP_OOM=1 simulates it);
//   LARECOMP_VK_GMEM_LAZY=1 anywhere (tests), =0 off.
// LARECOMP_VK_GMEM_CHUNK_MB=<n> forces chunks of n MB (a power of two, 1 GB at most), =0 one buffer.
#define GC_MAX GMEM_CHUNKS_MAX
static struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint64_t size, addr;  // (addr: the chunk's device address, lazy mode)
} gc[GC_MAX];
static int gc_n;             // chunks over HEAP_MAX
static uint32_t gc_shift;    // 0: one buffer
static bool gc_lazy;         // imports per chunk (else windows of gmem_buf)
static VkBuffer gc_dummy, gc_miss_buf;
static VmaAllocation gc_dummy_mem, gc_miss_mem;
static uint32_t *gc_miss;    // GMiss, host-visible: per chunk, accesses that found no chunk, then the lowest word index
static VkDescriptorSetLayout gc_layout;
static VkDescriptorSet gc_set;
static uint64_t gc_set_seq;
static uint32_t gc_gen, gc_set_gen;  // (gen: bumped when a chunk is imported; the set is rebuilt per submission and gen)
static int gc_scanned;                // chunks imported for a pointer found in a uniform buffer
static uint8_t gc_remembered[GC_MAX]; // chunks shaders reached without a bind (a miss, a pointer): imported again after a reboot
                                      // and, through cache/gmem_chunks.txt, at the next start (a machine misses a chunk once)
static bool gc_file_loaded;
static uint64_t gc_remembered_heap;   // the heap size the remembered chunks were imported at this boot (0: not yet)

// cache/gmem_chunks.txt: one heap byte offset (hex) per line of a chunk shaders reached without a bind. Offsets, not
// chunk numbers: the chunk size depends on the GPU. Loaded once, into gc_remembered; written whenever one is added.
static void gc_remembered_load(void) {
    if (gc_file_loaded) return;
    gc_file_loaded = true;
    FILE *f = fopen("cache/gmem_chunks.txt", "r");
    if (!f) return;
    char line[64];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long off = strtoull(line, NULL, 16);
        int k = (int)(off >> gc_shift);
        if (k >= 0 && k < gc_n && !gc_remembered[k]) { gc_remembered[k] = 1; n++; }
    }
    fclose(f);
    if (n) LOG("[vk] guest heap: %d chunks shaders reached without a bind last time (cache/gmem_chunks.txt): imported at the start\n", n);
}
static void gc_remembered_save(void) {
    mkdir("cache", 0755);
    FILE *f = fopen("cache/gmem_chunks.txt.tmp", "w");
    if (!f) return;
    for (int k = 0; k < gc_n; k++)
        if (gc_remembered[k]) fprintf(f, "%llx\n", (unsigned long long)((uint64_t)k << gc_shift));
    fclose(f);
    remove("cache/gmem_chunks.txt");
    rename("cache/gmem_chunks.txt.tmp", "cache/gmem_chunks.txt");
}
static void gc_remember(int k) {
    if (gc_remembered[k]) return;
    gc_remembered[k] = 1;
    gc_remembered_save();
}
static int gc_imported;
static uint64_t gc_imported_bytes, stat_gc_misses;

static bool gmem_lazy(void) {
    static int lazy = -1;
    if (lazy >= 0) return lazy;
    lazy = g_vk.lazy_heap;
    if (!lazy && g_vk.heap_probe && !getenv("LARECOMP_VK_NO_GMEM")) lazy = whole_import_probe_oom();
    if (lazy && (!g_vk.host_import || !g_vk.bda || !g_vk.ssbo_chunks)) {
        LOG("[vk] lazy heap import: not possible here (%s)\n", !g_vk.host_import ? "no host memory import" : !g_vk.bda ? "no buffer device addresses" : "no stores from fragment / vertex shaders");
        lazy = 0;
    }
    return lazy;
}

// Decided once (pthread_once): the render thread's draws, the translation and pipeline workers and the boot
// (backend_guest_heap_prepare) all ask, and a second caller must not see a half-made decision (shift is 0 until the
// fit loop below ends - a shader translated then would use one buffer while draws bind chunks).
static int gc_shift_decided = -1;
static void gmem_chunk_decide(void);
static pthread_once_t gc_decide_once = PTHREAD_ONCE_INIT;
static uint32_t gmem_chunk_shift(void) {
    pthread_once(&gc_decide_once, gmem_chunk_decide);
    return (uint32_t)gc_shift_decided;
}
static void gmem_chunk_decide(void) {
    int shift;
    // Pokémon Z-A has no heap: its window (gwin: physical memory) is imported as one buffer, as before the chunks -
    // they are laid over HEAP_BASE / HEAP_MAX (data root's extracted/: asked here, the boot may come after this)
    extern bool game_files_are_la(const char *game_dir);
    if (!g_game_is_la || !game_files_are_la("extracted")) {
        gc_lazy = maxwell_gmem_lazy = maxwell_gmem_fs_miss = false;
        gc_shift = 0, gc_n = 0, gc_shift_decided = 0;
        LOG("[vk] guest memory: one import of the game's window (not Link's Awakening: no heap chunks)\n");
        return;
    }
    const char *e = getenv("LARECOMP_VK_GMEM_CHUNK_MB");
    uint64_t lim = g_vk.props.limits.maxStorageBufferRange;
    gc_lazy = gmem_lazy();
    shift = 0;
    if (e) { uint64_t b = strtoull(e, NULL, 10) << 20; if (b) shift = 63 - __builtin_clzll(b); }
    else if (gc_lazy) shift = 24;
    else if (HEAP_MAX > lim && lim) shift = 63 - __builtin_clzll(lim);
    if (shift > 30) shift = 30;  // (1 GB at most: NVIDIA computes a block's length in signed 32 bits, a 2 GB range reads as negative)
    // The table holds GC_MAX chunks at most, and a stage's storage buffers (the windows, gfast, binding 17) its limit
    // (Intel: 200): a chunk size that doesn't fit grows until it does (4 MB asked for -> 16 MB). One buffer only where
    // one buffer is within maxStorageBufferRange.
    int asked = shift, n = 0;
    uint32_t per_stage = g_vk.props.limits.maxPerStageDescriptorStorageBuffers, per_set = g_vk.props.limits.maxDescriptorSetStorageBuffers;
    if (shift && gc_lazy && !g_vk.ssbo_chunks) {
        LOG("[vk] guest heap chunks: not possible here (no stores from fragment / vertex shaders): one buffer\n");
        shift = 0;
    }
    while (shift) {
        n = (int)((HEAP_MAX + (1ull << shift) - 1) >> shift);
        bool fits = n <= GC_MAX && (gc_lazy || ((uint32_t)n + 8 <= per_stage && (uint32_t)n + 1 <= per_set));
        if (fits) break;
        if ((1ull << shift) >= HEAP_MAX) { shift = 0; break; }  // (not even one chunk fits: one buffer)
        shift++;
    }
    if (shift != asked && asked)
        LOG("[vk] guest heap chunks of %llu MB don't fit here (%d chunks at most, %u storage buffers a stage, %u a set): %s\n",
            (unsigned long long)(1ull << asked) >> 20, GC_MAX, per_stage, per_set, shift ? "larger chunks" : "one buffer");
    if (!shift && HEAP_MAX > lim && lim) LOG("[vk] warning: one storage buffer over the heap exceeds this GPU's %llu MB range\n", (unsigned long long)lim >> 20);
    if (!shift) gc_lazy = false;
    gc_shift = (uint32_t)shift;
    gc_n = shift ? n : 0;
    maxwell_gmem_lazy = gc_lazy;
    maxwell_gmem_fs_miss = gc_lazy && getenv("LARECOMP_VK_GMEM_FS_MISS") && atoi(getenv("LARECOMP_VK_GMEM_FS_MISS")) != 0;
    if (maxwell_gmem_fs_miss) LOG("[vk] lazy chunks: every fragment shader counts misses, no early tests (LARECOMP_VK_GMEM_FS_MISS)\n");
    if (shift) LOG("[vk] guest heap bound to shaders in %llu MB %s (storage buffers up to %llu MB here)\n",
                   (unsigned long long)(1ull << shift) >> 20, gc_lazy ? "chunks, imported as the game binds them" : "windows", (unsigned long long)lim >> 20);
    gc_shift_decided = shift;
}

// The heap mapped so far, as the imports see it (SetHeapSize; pages beyond it don't exist yet).
static uint64_t gmem_heap_size(void) {
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    uint64_t ring = snap_ring_end();  // (overlap: the storage copies' ring right after the heap is the GPU's too)
    if (ring > want) want = ring;
    return want > HEAP_MAX ? HEAP_MAX : want;
}

static bool host_import(uint64_t off, uint64_t size, VkBuffer *buf, VkDeviceMemory *mem, bool bda);

// Lazy mode: chunk k imported (again, if the heap grew into it since). False: not mapped yet / the import failed.
static bool gc_import(int k) {
    uint64_t off = (uint64_t)k << gc_shift, heap = gmem_heap_size();
    if (k < 0 || k >= gc_n || off >= heap) return false;
    uint64_t size = heap - off < (1ull << gc_shift) ? heap - off : 1ull << gc_shift;
    if (gc[k].buf && gc[k].size >= size) return true;
    VkBuffer buf;
    VkDeviceMemory mem;
    if (!host_import(off, size, &buf, &mem, true)) {
        static int warned;
        if (warned++ < 8) LOG("[vk] guest heap chunk %d (%llu MB at +%llu MB): import failed\n", k, (unsigned long long)size >> 20, (unsigned long long)off >> 20);
        return false;
    }
    if (gc[k].buf) {  // (the heap grew into the chunk: the pending submission may still use the old import)
        vk_defer_buffer(gc[k].buf, gc[k].mem);
        gc_imported_bytes -= gc[k].size;
    } else {
        gc_imported++;
    }
    VkBufferDeviceAddressInfo ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf};
    gc[k].buf = buf; gc[k].mem = mem; gc[k].size = size;
    gc[k].addr = vkGetBufferDeviceAddress(g_vk.dev, &ai);
    gc_imported_bytes += size;
    gc_gen++;
    return true;
}

// (snap_on, lazy chunks) The chunk holding the end of the game's heap may have been imported before the ring was
// decided, short of it: imported again over the ring (gc_import replaces it; the old import goes once its submissions
// are done). snap_state is set before this runs, so gmem_heap_size's answer includes the ring here.
static void snap_ring_import_tail(void) {
    if (!gc_lazy || !gc_shift) return;
    for (int k = (int)(snap_base >> gc_shift); k <= (int)((snap_base + SNAP_RING - 1) >> gc_shift) && k < gc_n; k++)
        if (gc[k].buf && gc[k].size < gmem_heap_size() - ((uint64_t)k << gc_shift)) gc_import(k);
}

// Pointers the game keeps in its uniform buffers (the lighting pass's light list, the grass's instance tables): the
// chunk a shader reaches that way has no storage buffer bound over it, so it is imported from the uniform buffer's
// contents - which the per-draw copy reads anyway - before the draw. Without this the first frame in such a chunk
// read 0 (chunk 47 at +752 MB at every scene entry, chunk 50 at the first grass cut: 576 misses over a tour). An
// 8-byte-aligned word pair that happens to look like a heap address imports a chunk for nothing: bounded, and logged.
#define GC_SCAN_MAX 48
static int gc_scan_warned;
static void gc_scan_pointers(const uint32_t *w, uint64_t bytes) {
    uint64_t heap = gmem_heap_size();
    prof_scan_calls++;
    prof_scan_bytes += bytes;
    for (uint64_t i = 0; i + 1 < bytes / 4; i += 2) {
        uint64_t v = (uint64_t)w[i] | (uint64_t)w[i + 1] << 32;
        if (v < HEAP_BASE || v - HEAP_BASE >= heap) continue;
        int k = (int)((v - HEAP_BASE) >> gc_shift);
        if (k >= gc_n || gc[k].buf) continue;
        if (gc_scanned >= GC_SCAN_MAX) {
            if (!gc_scan_warned++) LOG("[vk] guest heap: %d chunks imported for pointers in uniform buffers: no more\n", gc_scanned);
            return;
        }
        if (gc_import(k)) {
            gc_scanned++;
            gc_remember(k);
            LOG("[vk] guest heap chunk %d (+%llu MB): a pointer in a uniform buffer (%llx): imported\n", k,
                (unsigned long long)((uint64_t)k << gc_shift) >> 20, (unsigned long long)v);
        }
    }
}

// Whether a pointer scan can still import anything: some chunk of the heap mapped so far isn't imported (and the
// scan's import limit isn't used up). Once every chunk is in, the scans are skipped (guest_copy_ex).
static bool gc_scan_needed(void) {
    static uint64_t c_heap;
    static uint32_t c_gen;
    static int c_scanned = -1, c_warned;
    static bool c_need = true;
    uint64_t heap = gmem_heap_size();
    if (heap != c_heap || gc_gen != c_gen || gc_scanned != c_scanned || gc_scan_warned != c_warned) {
        c_heap = heap; c_gen = gc_gen; c_scanned = gc_scanned; c_warned = gc_scan_warned;
        c_need = false;
        if (gc_scanned < GC_SCAN_MAX || !gc_scan_warned) {
            int n = (int)((heap + (1ull << gc_shift) - 1) >> gc_shift);
            if (n > gc_n) n = gc_n;
            for (int k = 0; k < n && !c_need; k++) c_need = !gc[k].buf;
        }
    }
    return c_need;
}

// A heap range a draw or dispatch binds: its chunks get imported (lazy mode), and the chunk after it as well - the
// game's storage buffers are bound with sizes the shaders read past (the lighting pass's tile list: the misses sat
// 1-9 MB into the chunk after the bound range), and a size of 0 means "the rest of the buffer".
static void gc_touch(uint64_t off, uint64_t size) {
    if (!gc_lazy || off >= HEAP_MAX) return;
    int k0 = (int)(off >> gc_shift), k1 = (int)((off + (size ? size : 1) - 1) >> gc_shift) + 1;
    if (k1 >= gc_n) k1 = gc_n - 1;
    for (int k = k0; k <= k1; k++) if (!gc[k].buf) gc_import(k);
}

// The chunk set's layout: lazy mode the table (GChunks), GMiss and the draw's chunks (GDir<k>); windows one storage
// binding each (shader.h).
// (once: pipeline layouts are made on the build workers, sets on the render thread)
static pthread_once_t gc_layout_once = PTHREAD_ONCE_INIT;
static void gmem_chunk_layout_make(void);
static VkDescriptorSetLayout gmem_chunk_layout(void) {
    pthread_once(&gc_layout_once, gmem_chunk_layout_make);
    return gc_layout;
}
static void gmem_chunk_layout_make(void) {
    gmem_chunk_shift();  // (gc_lazy / gc_n decided)
    VkShaderStageFlags st_ = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    static VkDescriptorSetLayoutBinding lb[GC_MAX];
    uint32_t nb = 0;
    if (gc_lazy) {
        lb[nb++] = (VkDescriptorSetLayoutBinding){GMEM_CHUNK_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, st_, NULL};
        lb[nb++] = (VkDescriptorSetLayoutBinding){GMEM_MISS_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st_, NULL};
        for (int k = 0; k < GMEM_DIRECT_MAX; k++)
            lb[nb++] = (VkDescriptorSetLayoutBinding){GMEM_DIRECT_BINDING + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st_, NULL};
    } else {
        for (int k = 0; k < gc_n; k++) lb[nb++] = (VkDescriptorSetLayoutBinding){GMEM_CHUNK_BINDING + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, st_, NULL};
    }
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = lb};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &gc_layout));
}

// The chunk set for the pending submission: the table of imported chunks, GMiss and the draw's chunks dk[k] (-1: none)
// as GDir<k> (lazy), or the windows. Lazy sets are kept per submission and chunk generation, keyed by dk: a frame has a
// handful of storage buffer chunk combinations, so a draw binds a set it already has (no allocation, no update).
#define GC_SET_CACHE 32
static uint64_t gc_direct_bytes(int k) {  // (GDir<k>'s range: the chunk, within the storage buffer range limit)
    uint64_t lim = g_vk.props.limits.maxStorageBufferRange;
    return lim && gc[k].size > lim ? lim & ~3ull : gc[k].size;
}
static VkDescriptorSet gmem_chunk_set(const int dk[GMEM_DIRECT_MAX]) {
    if (!gc_dummy) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gc_dummy, &gc_dummy_mem, &info));
        memset(info.pMappedData, 0, 4096);
        bi.size = sizeof(uint32_t) * 2 * GC_MAX;  // n[GC_MAX] accesses, w[GC_MAX] lowest word index hit
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gc_miss_buf, &gc_miss_mem, &info));
        gc_miss = info.pMappedData;
        memset(gc_miss, 0, sizeof(uint32_t) * GC_MAX);
        memset(gc_miss + GC_MAX, 0xFF, sizeof(uint32_t) * GC_MAX);
    }
    uint64_t seq = vk_pending_seq();
    // A boot, or a reboot: the chunks shaders reached without a bind last time (this process, or cache/gmem_chunks.txt),
    // before they miss again. Once a boot per heap size (a chunk beyond the heap mapped so far comes when it grows) -
    // not "while nothing is imported": copies and storage buffers import chunks before the first set is built.
    if (gc_lazy && gc_remembered_heap != gmem_heap_size()) {
        gc_remembered_heap = gmem_heap_size();
        gc_remembered_load();
        for (int k = 0; k < gc_n; k++) if (gc_remembered[k] && !gc[k].buf) gc_import(k);
    }
    static struct { VkDescriptorSet set; int dk[GMEM_DIRECT_MAX]; } cache[GC_SET_CACHE];
    static int ncache, next;
    static VkSpan tab_span;  // (the table for this submission and generation, shared by its sets)
    // (windows: also when the heap was imported again since - guest_direct / gfast_prepare re-import as it grows
    // mid-submission, and a set still on the old import read zeros past its end for the rest of the submission)
    static VkBuffer gc_set_heap;
    if (gc_set_seq != seq || gc_set_gen != gc_gen || (!gc_lazy && gc_set_heap != gmem_buf)) { ncache = 0; next = 0; gc_set = VK_NULL_HANDLE; }
    if (!gc_lazy) {
        if (gc_set) return gc_set;
    } else {
        for (int j = 0; j < ncache; j++)
            if (!memcmp(cache[j].dk, dk, sizeof cache[j].dk)) return cache[j].set;
    }
    static VkDescriptorBufferInfo bi[GC_MAX + 2 + GMEM_DIRECT_MAX];
    static VkWriteDescriptorSet w[GC_MAX + 2 + GMEM_DIRECT_MAX];
    VkDescriptorSet set = vk_alloc_set(gmem_chunk_layout());
    uint32_t nw = 0;
    if (gc_lazy) {  // GChunks: uvec4 per chunk = device address lo, hi, words (0: not imported), 0
        if (!ncache) {
            static uint32_t tab[GC_MAX * 4];
            for (int k = 0; k < gc_n; k++) {
                tab[k * 4 + 0] = (uint32_t)gc[k].addr; tab[k * 4 + 1] = (uint32_t)(gc[k].addr >> 32);
                tab[k * 4 + 2] = gc[k].buf ? (uint32_t)(gc[k].size >> 2) : 0; tab[k * 4 + 3] = 0;
            }
            tab_span = vk_transient_copy(tab, sizeof tab, 256);
        }
        bi[0] = (VkDescriptorBufferInfo){tab_span.buf, tab_span.off, sizeof(uint32_t) * GC_MAX * 4};
        bi[1] = (VkDescriptorBufferInfo){gc_miss_buf, 0, sizeof(uint32_t) * 2 * GC_MAX};
        w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = GMEM_CHUNK_BINDING,
                                         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0]};
        w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = GMEM_MISS_BINDING,
                                         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1]};
        for (int j = 0; j < GMEM_DIRECT_MAX; j++) {  // (GDir<j>: the chunk's own import, whole; unused: the zero buffer)
            int k = dk[j];
            bi[2 + j] = k >= 0 && k < gc_n && gc[k].buf ? (VkDescriptorBufferInfo){gc[k].buf, 0, gc_direct_bytes(k)}
                                                       : (VkDescriptorBufferInfo){gc_dummy, 0, 4096};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = GMEM_DIRECT_BINDING + j,
                                             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[2 + j]};
        }
        int slot = ncache < GC_SET_CACHE ? ncache++ : (next++ % GC_SET_CACHE);
        cache[slot].set = set;
        memcpy(cache[slot].dk, dk, sizeof cache[slot].dk);
    } else {  // windows of the one import; one past it is the dummy (reads 0, as the zero buffer before the import)
        VkDescriptorBufferInfo all = gmem_info();
        for (int k = 0; k < gc_n; k++) {
            uint64_t off = (uint64_t)k << gc_shift;
            if (all.buffer == gmem_buf && gmem_buf && off < gmem_size)
                bi[k] = (VkDescriptorBufferInfo){gmem_buf, off, gmem_size - off < (1ull << gc_shift) ? gmem_size - off : 1ull << gc_shift};
            else bi[k] = (VkDescriptorBufferInfo){gc_dummy, 0, 4096};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = GMEM_CHUNK_BINDING + k,
                                             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[k]};
        }
    }
    rec_UpdateDescriptorSets(nw, w);
    gc_set = set; gc_set_seq = seq; gc_set_gen = gc_gen; gc_set_heap = gmem_buf;
    return set;
}

// Lazy mode, a draw: the imported chunks under its vertex / fragment storage buffers, in slot order (shader.h
// GMEM_DIRECT_MAX): gd[] for the shaders' GDirect push constants (the range each GDir<k> covers), dk[] the chunks for
// the set's GDir<k> bindings. A range over two chunks gives both. Accesses there are storage buffer loads, not the table.
// LARECOMP_VK_GMEM_DIRECT=0: none (every access through the table, as before; no shader change).
static void gmem_direct_pick(uint32_t stages, uint32_t gd[GMEM_DIRECT_MAX * 4], int dk[GMEM_DIRECT_MAX]) {
    static int on = -1;
    if (on < 0) {
        on = !getenv("LARECOMP_VK_GMEM_DIRECT") || atoi(getenv("LARECOMP_VK_GMEM_DIRECT")) != 0;
        if (!on) LOG("[vk] lazy chunks: storage buffer chunks through the table too (LARECOMP_VK_GMEM_DIRECT=0)\n");
    }
    memset(gd, 0, sizeof(uint32_t) * GMEM_DIRECT_MAX * 4);
    for (int j = 0; j < GMEM_DIRECT_MAX; j++) dk[j] = -1;
    int n = 0;
    for (int stage = 0; on && stage < 2; stage++) {
        if (!(stages & (1u << stage))) continue;
        for (int i = 0; i < 16 && n < GMEM_DIRECT_MAX; i++) {
            uint64_t a = sbufs(stage)[i].addr, sz = sbufs(stage)[i].size;  // (overlap: a snapshotted range's copy)
            if (a < HEAP_BASE || a - HEAP_BASE >= HEAP_MAX) continue;
            uint64_t off = a - HEAP_BASE;
            int k0 = (int)(off >> gc_shift), k1 = (int)((off + (sz ? sz : 1) - 1) >> gc_shift);
            for (int k = k0; k <= k1 && k < gc_n && n < GMEM_DIRECT_MAX; k++) {
                if (!gc[k].buf) continue;
                int j = 0;
                while (j < n && dk[j] != k) j++;
                if (j < n) continue;
                dk[n] = k;
                gd[n * 4 + 0] = (uint32_t)(((uint64_t)k << gc_shift) >> 2);
                gd[n * 4 + 1] = (uint32_t)(gc_direct_bytes(k) >> 2);
                gd[n * 4 + 2] = (uint32_t)gc[k].addr;
                gd[n * 4 + 3] = (uint32_t)(gc[k].addr >> 32);
                n++;
            }
        }
    }
}

// Binds the chunk set for a draw / dispatch whose shaders (stages: a mask of 1 << stage) use global memory: their storage
// buffers' chunks first.
static void gmem_chunk_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set_index, uint32_t stages) {
    static int log_sb = -1;  // LARECOMP_LOG_GMEM_SBUF=1: each distinct storage buffer bound to a gmem shader, once
    if (log_sb < 0) log_sb = getenv("LARECOMP_LOG_GMEM_SBUF") != NULL;
    for (int stage = 0; stage < 6; stage++) {
        if (!(stages & (1u << stage))) continue;
        for (int i = 0; i < 16; i++) {
            uint64_t a = sbufs(stage)[i].addr, sz = sbufs(stage)[i].size;  // (overlap: a snapshotted range's copy - its chunk)
            if (a < HEAP_BASE || a - HEAP_BASE >= HEAP_MAX) continue;
            if (log_sb) {
                static uint64_t seen[64]; static int nseen;
                uint64_t key = a ^ (sz << 40) ^ ((uint64_t)(stage * 16 + i) << 56);
                int j = 0; while (j < nseen && seen[j] != key) j++;
                if (j == nseen && nseen < 64) { seen[nseen++] = key; LOG("[vk] gmem sbuf stage %d slot %d: %llx + %llx (chunk %d)\n", stage, i, (unsigned long long)a, (unsigned long long)sz, (int)((a - HEAP_BASE) >> gc_shift)); }
            }
            gc_touch(a - HEAP_BASE, sz);
        }
    }
    uint32_t gd[GMEM_DIRECT_MAX * 4];
    int dk[GMEM_DIRECT_MAX] = {-1, -1, -1, -1};
    bool direct = gc_lazy && bp == VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (direct) gmem_direct_pick(stages, gd, dk);
    VkDescriptorSet set = gmem_chunk_set(dk);
    rec_CmdBindDescriptorSets(cb, bp, layout, set_index, 1, &set, 0, NULL);
    if (direct) rec_CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof gd, gd);
}

// Once a frame (the present's submission): a chunk shaders missed is imported for the next frame; the lowest address
// that missed is logged (a pointer the game keeps in a uniform buffer, which no bind told us about).
static void gc_frame(void) {
    if (!gc_shift || !gc_miss || !gc_lazy) return;
    for (int k = 0; k < gc_n; k++) {
        uint32_t m = gc_miss[k], w = gc_miss[GC_MAX + k];
        if (!m) continue;
        gc_miss[k] = 0;
        gc_miss[GC_MAX + k] = 0xFFFFFFFFu;
        stat_gc_misses += m;
        static int nlog;
        if (nlog++ < 40) LOG("[vk] guest heap chunk %d (+%llu MB): %u shader accesses before its import, lowest %llx: imported for the next frame\n", k,
                             (unsigned long long)((uint64_t)k << gc_shift) >> 20, m, (unsigned long long)HEAP_BASE + ((uint64_t)w << 2));
        gc_touch((uint64_t)k << gc_shift, 1);
        gc_remember(k);
    }
}

// In lazy mode the heap-side offsets of a copy are in chunks: one vkCmdCopyBuffer per chunk piece. other = the GPU side.
static void gmem_copy(VkCommandBuffer cb, VkBuffer other, const VkBufferCopy *cp, int n, bool to_heap) {
    if (!gc_lazy) {
        if (to_heap) rec_CmdCopyBuffer(cb, other, gmem_buf, (uint32_t)n, cp);
        else rec_CmdCopyBuffer(cb, gmem_buf, other, (uint32_t)n, cp);
        return;
    }
    for (int i = 0; i < n; i++) {
        uint64_t h = to_heap ? cp[i].dstOffset : cp[i].srcOffset, o = to_heap ? cp[i].srcOffset : cp[i].dstOffset, left = cp[i].size;
        while (left) {
            int k = (int)(h >> gc_shift);
            uint64_t in = h - ((uint64_t)k << gc_shift), piece = (1ull << gc_shift) - in < left ? (1ull << gc_shift) - in : left;
            if (k < gc_n && (gc[k].buf || gc_import(k)) && in + piece <= gc[k].size) {
                VkBufferCopy c = to_heap ? (VkBufferCopy){o, in, piece} : (VkBufferCopy){in, o, piece};
                if (to_heap) rec_CmdCopyBuffer(cb, other, gc[k].buf, 1, &c);
                else rec_CmdCopyBuffer(cb, gc[k].buf, other, 1, &c);
            }
            h += piece; o += piece; left -= piece;
        }
    }
}

// The heap is there for the GPU over this range (one import, or lazy chunks: imported on demand).
static bool gmem_covers(uint64_t off, uint64_t size) {
    if (gc_lazy) {
        if (off + size > gmem_heap_size()) return false;
        gc_touch(off, size);
        for (int k = (int)(off >> gc_shift); k <= (int)((off + size - 1) >> gc_shift); k++) if (!gc[k].buf) return false;
        return true;
    }
    return gmem_buf && off + size <= gmem_size;
}


// Compute dispatches that use global memory: the imported heap is host memory across PCIe on a discrete GPU, and a
// shader storing to it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~8 ms a frame on a GTX 1660
// Super and on an RTX 3080 Ti alike, which the game then waits for on its fence - 49-53 fps there on the 1660). Before
// such a dispatch its bound storage buffers (sorted, merged) are copied into a device-local buffer, the address table
// (binding GMAP_BUFFER_INDEX) tells the shader (translate.c, GLSL compute) where each range sits, loads and stores
// inside them use that copy, and a shader that stores gets the ranges copied back to the heap right after it.
// Accesses outside the table use the heap as before. LARECOMP_VK_GMEM_FAST=0: no copy (empty table: everything goes
// to the heap, the old behaviour).
#define GFAST_SIZE (32ull << 20)
static VkBuffer gfast_buf;
static VmaAllocation gfast_mem;
static uint32_t gmap_tab[(GMAP_MAX + 1) * 4];  // this dispatch's table (stage_set copies it into binding 20)
static VkBufferCopy gfast_cp[GMAP_MAX];       // its copies heap -> gfast_buf
static int gfast_n;
// The copy back after a storing dispatch: only the bound buffers' own bytes, gfast_buf -> heap. The merged ranges
// above also span alignment padding and the gaps between buffers - other data, which the CPU (a frame ahead) or
// earlier GPU work may rewrite while the dispatch runs: copied back, the stale bytes clobbered it (Pokémon Z-A: an
// indirect draw's instance count became an old float, 2 billion instances, device lost).
static VkBufferCopy gfast_back[16];
static int gfast_back_n;
// Even the exact bytes clobbered the CPU's writes: the game fills an indirect draw's index count / first index on the
// CPU while the culling dispatch writes its instance count, both in one buffer. A frame ahead, the CPU wrote a new
// mesh's counts there while the GPU worked on its copy; the copy back restored the old mesh's (Pokémon Z-A: ~12% of
// the culled draws used another mesh's index range - sheets and beams across the sky, the player sunk in a grey slab).
// So a twin of the copy, untouched by the dispatch, goes along, and a merge shader writes back only the words that
// differ from it. LARECOMP_VK_GFAST_MERGE=0: the plain copy back.
static VkBuffer gorig_buf;
static VmaAllocation gorig_mem;
static VkPipeline gmerge_pipe;
static VkPipelineLayout gmerge_layout;
static VkDescriptorSetLayout gmerge_set_layout;
static int gmerge_on = -1;
static bool gfast_stores;  // (set before gfast_prepare: the dispatch stores to global memory)

static bool gmerge_init(void) {
    if (gmerge_on < 0) gmerge_on = !ENV("LARECOMP_VK_GFAST_MERGE") || atoi(ENV("LARECOMP_VK_GFAST_MERGE")) != 0;
    if (!gmerge_on || gmerge_pipe) return gmerge_on && gmerge_pipe;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GFAST_SIZE,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &gorig_buf, &gorig_mem, NULL) != VK_SUCCESS) { gmerge_on = 0; return false; }
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &gmerge_set_layout));
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &gmerge_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pi, NULL, &gmerge_layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = gmerge_layout,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = vk_builtin_shader("gfast_merge.comp"), .pName = "main"}};
    VK_CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &gmerge_pipe));
    return true;
}

static VkDescriptorBufferInfo gfast_info(void) {
    if (!gfast_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GFAST_SIZE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gfast_buf, &gfast_mem, NULL));
    }
    return (VkDescriptorBufferInfo){gfast_buf, 0, GFAST_SIZE};
}

// ---- transform feedback mirror. Pokémon Z-A skins its characters with transform feedback - ~140 sessions a frame,
// their buffers in one ~6 MB arena - and its draws then read the results as vertex streams. Written into the guest
// heap's import (host memory), every vertex was a scattered write across PCIe: 3 ms of GPU time a frame (7 with
// mods/MaxLOD). Instead the sessions write a device-local mirror of the arena (XFB_BUFFER_INDEX), the vertex streams
// inside parts written that way read it (guest_direct), and at each session's end the parts written are copied back
// to the heap (one small DMA transfer), for anything else that reads them there. Ranges outside the mirror's 64 MB
// window go to the heap as before. LARECOMP_VK_XFB_MIRROR=0: off, =1: on. Off by default on AMD GPUs (#22: an RX 9070 XT
// lost the device on an invalid GPU read in exactly these skinning draws, with robust access on and submissions made
// one at a time) - their transform feedback goes to the heap's import as before the mirror.
#define XM_SIZE (64ull << 20)
#define XM_CHUNK 256ull
static VkBuffer xm_buf;
static VmaAllocation xm_mem;
static uint64_t xm_lo;  // guest address of the mirror's first byte (0: not placed yet)
// the frame each chunk was last written by transform feedback (+1; 0: never): only this frame's are read from the
// mirror - the game reuses the arena's memory (other data written by the CPU when areas / outfits change: read from
// the mirror, a stale skinning drew the player black)
static uint32_t xm_written[XM_SIZE / XM_CHUNK];
static VkBufferCopy xm_back[256];  // this submission's written ranges (mirror -> heap)
static int xm_back_n;
static bool xm_draw;  // this draw's transform feedback goes to the mirror

static bool xm_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = ENV("LARECOMP_VK_XFB_MIRROR");
        on = e ? atoi(e) != 0 : g_vk.props.vendorID != 0x1002;
        if (!on && !e) LOG("[vk] transform feedback mirror off on this AMD GPU (LARECOMP_VK_XFB_MIRROR=1: on)\n");
    }
    return on && g_vk.host_import;
}

static bool xm_inside(uint64_t a, uint64_t n) { return xm_lo && a >= xm_lo && a + n <= xm_lo + XM_SIZE; }

static bool xm_all_written(uint64_t a, uint64_t n) {
    if (!n || !xm_inside(a, n)) return false;
    uint32_t now = (uint32_t)vk_frame_counter() + 1;
    for (uint64_t c = (a - xm_lo) / XM_CHUNK; c <= (a + n - 1 - xm_lo) / XM_CHUNK; c++)
        if (xm_written[c] != now) return false;
    return true;
}

// before a transform feedback draw: can its buffers all go to the mirror? (then they're marked written, and queued
// for the copy back)
static bool xm_prepare(void) {
    if (!xm_on() || !gmem_buf) return false;
    if (!xm_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = XM_SIZE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &xm_buf, &xm_mem, NULL) != VK_SUCCESS) { xm_buf = VK_NULL_HANDLE; return false; }
    }
    for (int b = 0; b < 4; b++) {
        if (!xfb_addr[b] || !xfb_size[b]) continue;
        if (!xm_lo) {  // placed around the first session's buffers (the arena is a few MB)
            uint64_t lo = (xfb_addr[b] & ~0xFFFFFull) - (16ull << 20);
            xm_lo = lo < gwin_base ? gwin_base : lo;
            LOG("[vk] transform feedback mirror at %llx (+%llu MB)\n", (unsigned long long)xm_lo, (unsigned long long)(XM_SIZE >> 20));
        }
        if ((xfb_addr[b] & 3) || !xm_inside(xfb_addr[b], xfb_size[b])) return false;
        if (!GM_IN(xfb_addr[b], xfb_size[b])) {  // (also with the whole heap imported: the mirror may reach past its end)
            static int nl;
            if (nl++ < 8) LOG("[gmem] transform feedback buffer %llx is outside the import window\n", (unsigned long long)xfb_addr[b]);
            return false;
        }
    }
    for (int b = 0; b < 4; b++) {
        if (!xfb_addr[b] || !xfb_size[b]) continue;
        uint64_t o = xfb_addr[b] - xm_lo;
        uint32_t now = (uint32_t)vk_frame_counter() + 1;
        for (uint64_t c = o / XM_CHUNK; c <= (o + xfb_size[b] - 1) / XM_CHUNK; c++) xm_written[c] = now;
        int i = 0;
        while (i < xm_back_n && !(xm_back[i].srcOffset == o && xm_back[i].size == xfb_size[b])) i++;
        if (i == xm_back_n) {
            if (xm_back_n == (int)(sizeof xm_back / sizeof *xm_back)) {  // (full: one range over them all)
                uint64_t lo = xm_back[0].srcOffset, hi = lo + xm_back[0].size;
                for (int k = 1; k < xm_back_n; k++) {
                    if (xm_back[k].srcOffset < lo) lo = xm_back[k].srcOffset;
                    if (xm_back[k].srcOffset + xm_back[k].size > hi) hi = xm_back[k].srcOffset + xm_back[k].size;
                }
                xm_back[0] = (VkBufferCopy){lo, xm_lo - gwin_base - gmem_lo + lo, hi - lo};
                xm_back_n = 1;
            }
            xm_back[xm_back_n++] = (VkBufferCopy){o, xm_lo - gwin_base - gmem_lo + o, xfb_size[b]};
        }
    }
    return true;
}

// a vertex stream inside the mirror's transform feedback output: read there
static bool xm_lookup(uint64_t a, uint64_t n, VkBuffer *buf, VkDeviceSize *off) {
    if (!xm_buf || !xm_all_written(a, n)) return false;
    *buf = xm_buf;
    *off = a - xm_lo;
    return true;
}

// the submission's end (outside any pass): what transform feedback wrote, to the heap
static void xm_copy_back(VkCommandBuffer cb) {
    if (!xm_back_n || !xm_buf || !gmem_buf) { xm_back_n = 0; return; }
    vk_full_barrier(cb);
    rec_CmdCopyBuffer(cb, xm_buf, gmem_buf, (uint32_t)xm_back_n, xm_back);
    vk_full_barrier(cb);
    xm_back_n = 0;
}

static void gfast_prepare(VkCommandBuffer cb) {
    static int on = -1;
    // (on an integrated GPU the heap import is not across PCIe: the copy and the table search only cost, 234 -> 97 ms on a Steam Deck)
    if (on < 0) { const char *e = ENV("LARECOMP_VK_GMEM_FAST"); on = e ? atoi(e) != 0 : g_vk.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; }
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
    if (!gc_lazy) gmem_info();  // (the heap import, made on first use)
    gfast_info();
    if (!on || (!gmem_buf && !gc_lazy) || gmem_lo) return;  // (gmem_lo: a windowed import - its offsets are not the heap's)
    struct { uint64_t lo, hi; } r[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {  // window offsets of the bound storage buffers, 256-aligned
        uint64_t a = sbufs(5)[i].addr, sz = sbufs(5)[i].size;  // (overlap: a snapshotted range's copy is what gets copied)
        if (!a || !sz || a < gwin_base || !gmem_covers(a - gwin_base, sz)) continue;
        uint64_t lo = (a - gwin_base) & ~255ull, hi = (a - gwin_base + sz + 255) & ~255ull, top = gc_lazy ? gmem_heap_size() : gmem_size;
        if (hi > top) hi = top;
        int k = n++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }  // (insertion sort by start)
        r[k].lo = lo; r[k].hi = hi;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merge overlapping / nearby ranges (a small gap costs less than another table entry)
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    VkBufferCopy cp[16];
    uint64_t off = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        if (m > GMAP_MAX || off + len > GFAST_SIZE) return;  // (too much: the heap as before)
        cp[i] = (VkBufferCopy){r[i].lo, off, len};
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        off += (len + 255) & ~255ull;
    }
    if (!m) return;
    if (ENV("LARECOMP_LOG_GFAST")) {  // (debug) =<shader code address hex>: that shader's first tables
        static int nlog;
        if (st.program->stages[5].code_addr == strtoull(ENV("LARECOMP_LOG_GFAST"), NULL, 16) && nlog++ < 40) {
            LOG("[gfast] cs %llx: %d ranges, %llu KB:", (unsigned long long)st.program->stages[5].code_addr, m, (unsigned long long)(off >> 10));
            for (int i = 0; i < m; i++) LOG(" %llx+%llx", (unsigned long long)(r[i].lo + gwin_base), (unsigned long long)(r[i].hi - r[i].lo));
            LOG("\n");
        }
    }
    gmem_copy(cb, gfast_buf, cp, m, false);
    vk_full_barrier(cb);
    if (gfast_stores && gmerge_init()) {  // (the twin: see gfast_back)
        VkBufferCopy tw[16];
        for (int i = 0; i < m; i++) tw[i] = (VkBufferCopy){cp[i].dstOffset, cp[i].dstOffset, cp[i].size};
        rec_CmdCopyBuffer(cb, gfast_buf, gorig_buf, (uint32_t)m, tw);
        vk_full_barrier(cb);
    }
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
    memcpy(gfast_cp, cp, (size_t)m * sizeof cp[0]);
    gfast_n = m;
    gfast_back_n = 0;
    for (int i = 0; i < 16; i++) {  // each bound buffer's exact bytes, from wherever its merged range sits
        uint64_t a = st.sbuf[5][i].addr, sz = st.sbuf[5][i].size;
        if (!a || !sz || a < gwin_base || a - gwin_base + sz > gmem_size) continue;
        uint64_t h = a - gwin_base;
        for (int j = 0; j < m; j++)
            if (h >= cp[j].srcOffset && h + sz <= cp[j].srcOffset + cp[j].size) {
                gfast_back[gfast_back_n++] = (VkBufferCopy){cp[j].dstOffset + (h - cp[j].srcOffset), h, sz};
                break;
            }
    }
}

// After a storing dispatch: its ranges back to the heap (before anything else reads them: draws, the CPU after the fence).
static void gfast_finish(VkCommandBuffer cb) {
    if (!gfast_n) return;
    vk_full_barrier(cb);
    bool merged = false;
    if (!gc_lazy && gfast_back_n && gmerge_init()) {  // (chunks: the plain copy, gmem_copy)
        merged = true;
        for (int i = 0; i < gfast_back_n; i++)
            if ((gfast_back[i].srcOffset | gfast_back[i].dstOffset | gfast_back[i].size) & 3 || gfast_back[i].dstOffset / 4 > 0xFFFFFFFFull)
                merged = false;  // (not word-aligned: the plain copy)
    }
    if (merged) {
        VkDescriptorSet set = vk_alloc_set(gmerge_set_layout);
        VkDescriptorBufferInfo bi[3] = {gmem_info(), {gfast_buf, 0, GFAST_SIZE}, {gorig_buf, 0, GFAST_SIZE}};
        VkWriteDescriptorSet w[3];  // (one descriptor a write: record.c)
        for (uint32_t k = 0; k < 3; k++)
            w[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = k,
                                          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[k]};
        rec_UpdateDescriptorSets(3, w);
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gmerge_pipe);
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gmerge_layout, 0, 1, &set, 0, NULL);
        for (int i = 0; i < gfast_back_n; i++) {
            uint32_t pc[3] = {(uint32_t)(gfast_back[i].srcOffset / 4), (uint32_t)(gfast_back[i].dstOffset / 4), (uint32_t)(gfast_back[i].size / 4)};
            rec_CmdPushConstants(cb, gmerge_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            rec_CmdDispatch(cb, (pc[2] + 255) / 256, 1, 1);
        }
    } else if (gfast_back_n) {
        // (overlapping bound buffers copy the same bytes twice - identical data, harmless)
        gmem_copy(cb, gfast_buf, gfast_back, gfast_back_n, true);
    }
    vk_full_barrier(cb);
    gfast_n = 0;
}

// ---- vertex / index cache
// Reading vertex / index data in place from the imported heap crosses PCIe for every vertex fetched on a discrete GPU
// (outdoors at 3x internal resolution on an RTX 3080 Ti: ~0.7-1.9 ms of GPU a frame; more on PCIe 3.0), and copying it
// per draw instead moves about as much onto the CPU. So each range bound for drawing gets a device-local copy, refreshed
// only where the CPU wrote since (the OS's page write tracking, plat_written_pages, asked at submission: vc_flush): one
// sequential GPU copy of the written pages in the submission's upload buffer (which completes before its draws) instead
// of fetches across the bus. Overlapping ranges are separate entries; the per-page generation keeps them all current.
// Pages the GPU writes itself (a compute dispatch's copy-back, gfast_finish) are never cached: draws over them read the
// heap in place as before, and so does everything when the OS gives no write tracking. LARECOMP_VK_VCACHE=0: off.
#define VC_PAGE 4096ull
#define VC_TAB 8192                   // entries (open addressing)
#define VC_MAX_RANGE (64ull << 20)    // larger bound ranges: read in place
#define VC_BUDGET (768ull << 20)      // device-local bytes kept; the least recently used go first
typedef struct { uint64_t lo, hi, used_seq; uint32_t gen, epoch, gpu_until; VkBuffer buf; VmaAllocation mem; bool gpu; } VcEnt;
static VcEnt vc_tab[VC_TAB];
static uint32_t vc_pend[VC_TAB];  // entries the pending submission draws from (vc_flush brings them up to date)
static int vc_npend;
static uint32_t *vc_page_gen;  // per heap page: the generation of its last CPU write seen (vc_gen when observed)
static uint32_t *vc_page_gpu;  // per heap page: the frame the GPU last wrote it (+1; 0: never)
// A page the GPU hasn't written for this many frames is cached again (copied whole first): the game reuses its
// memory, and pages a compute dispatch or transform feedback wrote once stayed read in place for the rest of the
// session - the vertices read across PCIe grew to ~10 GB per 2 s after half an hour (GPU time +25%, 60 fps lost)
#define VC_GPU_HOLD 120u
static bool vc_gpu_expired;  // (an entry turned cacheable again: vc_flush orders its copy after the GPU's writes)
static uint32_t vc_gen = 1, vc_epoch = 1;  // (epoch: bumped when pages turn GPU-written: entries check again)
static uint64_t vc_bytes;
#define VC_DEAD 1024
static struct { VkBuffer b; VmaAllocation m; uint64_t seq; } vc_dead[VC_DEAD];
static int vc_ndead;
static int vc_on = -1;
// (profiler: draws served / read in place, bytes refreshed, write tracking queries, written pages seen, query time)
uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;

static void vc_free_dead(bool all) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < vc_ndead; i++) {
        if (!all && vc_dead[i].seq > done) { vc_dead[k++] = vc_dead[i]; continue; }
        vmaDestroyBuffer(g_vk.vma, vc_dead[i].b, vc_dead[i].m);
    }
    vc_ndead = k;
}

static bool vc_drop(VcEnt *e) {  // (freed once the submissions that may read it are done)
    if (vc_ndead == VC_DEAD) { vk_wait_seq(vk_pending_seq() - 1); vc_free_dead(false); }
    if (vc_ndead == VC_DEAD) return false;  // (all read by the pending submission: keep it)
    vc_dead[vc_ndead].b = e->buf; vc_dead[vc_ndead].m = e->mem; vc_dead[vc_ndead++].seq = e->used_seq + VK_DEAD_MARGIN;
    vc_bytes -= e->hi - e->lo;
    memset(e, 0, sizeof *e);
    return true;
}

// The page tables (vc_page_gen / vc_page_gpu) exist whenever the OS gives write tracking: the vertex cache and the
// texture upload checks (vk_pages_changed) share them. Each write tracking query resets the OS bits, so every query
// stamps the pages it saw written with a new generation, and each user compares against the generation it last saw.
static int pt_state = -1;  // 1: tracking, 0: none
static bool pt_init(void) {
    if (pt_state >= 0) return pt_state;
    uint64_t pg = VC_PAGE, one;
    int n = plat_written_pages(gwin_base, VC_PAGE, &one, 1, &pg);  // (is there write tracking?)
    pt_state = n >= 0 && pg == VC_PAGE;
    if (!pt_state) { LOG("[vk] no page write tracking (%d, page %llu)\n", n, (unsigned long long)pg); return false; }
    vc_page_gen = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_gen);
    vc_page_gpu = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_gpu);
    vc_page_gen[0] = ++vc_gen;  // (the probe took page 0's tracking)
    return true;
}

// Pages outside the window: Pokémon Z-A's textures sit in the alias region (0x74..., its SDK's address regions), not in
// the physical memory window - every check of theirs was "unknown" and sampled 4096 scattered bytes (~5500 a second
// at 50 fps, 12 % of the render thread). The OS tracks writes over the whole guest space, so those pages get
// generations too: a sparse table over [GUEST_LO, GUEST_HI), 2 MB pieces made on first use. (No GPU-written flag:
// shader stores reach the window only, the gmem import.)
#define PG_PIECE 512  // pages a piece
static uint32_t *pg_dir[(GUEST_HI - GUEST_LO) / (PG_PIECE * VC_PAGE)];
static uint32_t *pg_gen(uint64_t page_addr) {
    uint64_t p = (page_addr - GUEST_LO) / VC_PAGE;
    uint32_t **d = &pg_dir[p / PG_PIECE];
    if (!*d) *d = calloc(PG_PIECE, sizeof **d);
    return &(*d)[p % PG_PIECE];
}

static int pages_changed_outside(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (addr < GUEST_LO || addr + size > GUEST_HI) return -1;
    uint64_t lo = addr & ~(VC_PAGE - 1), hi = (addr + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    static uint64_t pages[4096];
    uint64_t pg = VC_PAGE;
    int n = plat_written_pages(lo, hi - lo, pages, 4096, &pg);
    if (n < 0 || pg != VC_PAGE) return -1;
    uint32_t g = ++vc_gen;
    if (n >= 4096) for (uint64_t a = lo; a < hi; a += VC_PAGE) *pg_gen(a) = g;
    else for (int k = 0; k < n; k++) *pg_gen(pages[k]) = g;
    *now = g;
    for (uint64_t a = lo; a < hi; a += VC_PAGE)
        if (*pg_gen(a) > since) return 1;
    return 0;
}

// Texture upload checks (textures.c): may guest memory [addr, addr + size) have changed since generation `since`?
// 0: no CPU write since, 1: written, < 0: unknown - -1 no tracking, -2 pages the GPU writes (gpu_unknown: the upload
// checks; the storage write watch asks about the CPU alone). *now: the generation to pass next time.
static int pages_written(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now, bool gpu_unknown) {
    if (!size || !pt_init()) return -1;
    if (addr < gwin_base || addr - gwin_base + size > gwin_max) {
        static int outside = -1;  // LARECOMP_VK_PAGES_OUTSIDE=0: unknown there, as before
        if (outside < 0) { const char *e = ENV("LARECOMP_VK_PAGES_OUTSIDE"); outside = !e || atoi(e) != 0; }
        int r = outside ? pages_changed_outside(addr, size, since, now) : -1;
        static int nlog;  // LARECOMP_LOG_PAGES=1 (debug): the first ranges outside the tracked window
        if (r < 0 && ENV("LARECOMP_LOG_PAGES") && nlog++ < 12)
            LOG("[pages] %llx+%llx outside the window %llx+%llx: untracked\n", (unsigned long long)addr,
                (unsigned long long)size, (unsigned long long)gwin_base, (unsigned long long)gwin_max);
        return r;
    }
    uint64_t lo = (addr - gwin_base) & ~(VC_PAGE - 1), hi = (addr - gwin_base + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    static uint64_t pages[4096];
    uint64_t pg = VC_PAGE;
    int n = plat_written_pages(gwin_base + lo, hi - lo, pages, 4096, &pg);
    if (n < 0) return -1;
    uint32_t g = ++vc_gen;
    if (n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;
    else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - gwin_base) / VC_PAGE] = g;
    *now = g;
    int changed = 0;
    for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) {
        if (gpu_unknown && vc_page_gpu[p]) return -2;
        if (vc_page_gen[p] > since) changed = 1;
    }
    return changed;
}
int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) { return pages_written(addr, size, since, now, true); }
// The storage write watch (watch_check): CPU writes only; the GPU's own writes to the range (DMA) don't count
static int pages_cpu_written(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) { return pages_written(addr, size, since, now, false); }

// Heap pages [lo, hi) (offsets) written by the GPU: never cached from now on.
static void vc_mark_gpu(uint64_t lo, uint64_t hi) {
    if (!pt_init()) return;
    uint32_t now = (uint32_t)vk_frame_counter() + 1;
    for (uint64_t p = lo / VC_PAGE; p < (hi + VC_PAGE - 1) / VC_PAGE && p < gwin_max / VC_PAGE; p++) vc_page_gpu[p] = now;
    vc_epoch++;
}

// A storing shader (stage 0 / 1 / 5) may write any of its bound storage buffers.
static void vc_mark_sbufs(int stage) {
    for (int i = 0; i < 16; i++) {
        uint64_t a = sbufs(stage)[i].addr, sz = sbufs(stage)[i].size;  // (overlap: the copy, where the shader wrote)
        if (a >= gwin_base && sz && a - gwin_base < gwin_max) vc_mark_gpu(a - gwin_base, a - gwin_base + sz);
    }
}

static bool vcache_lookup(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (vc_on < 0) {
        const char *e = ENV("LARECOMP_VK_VCACHE");
        vc_on = !e || atoi(e) != 0;
        // integrated GPU with a userptr heap (only with LARECOMP_WRITE_TRACKING=1: tracking is off there otherwise): the
        // heap already is the GPU's memory, no bus to save - the copies would only cost. LARECOMP_VK_VCACHE=1: on.
        if (vc_on && !e && gmem_userptr && g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            LOG("[vk] vertex cache off: integrated GPU\n");
            vc_on = 0;
        }
        if (vc_on && !pt_init()) {
            LOG("[vk] vertex cache off: no page write tracking\n");
            vc_on = 0;
        }
    }
    if (!vc_on || (!gmem_buf && !gc_lazy) || gmem_lo || size > VC_MAX_RANGE) return false;
    uint64_t lo = (addr - gwin_base) & ~(VC_PAGE - 1), hi = (addr - gwin_base + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    if (!gmem_covers(lo, hi - lo)) return false;
    uint64_t pend = vk_pending_seq();
    uint64_t h = (lo * 0x9E3779B97F4A7C15ull) ^ (hi * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VC_TAB - 1), free_slot = ~0u, old_slot = ~0u;
    VcEnt *e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VC_TAB - 1)) {
        VcEnt *c = &vc_tab[slot];
        if (c->buf && c->lo == lo && c->hi == hi) { e = c; break; }
        if (!c->buf && free_slot == ~0u) free_slot = slot;
        if (c->buf && c->used_seq < pend && (old_slot == ~0u || c->used_seq < vc_tab[old_slot].used_seq)) old_slot = slot;
    }
    if (!e) {  // a new range: its own device-local buffer (gen 0: copied whole at the submission)
        if (vc_npend == VC_TAB) return false;
        if (vc_ndead) vc_free_dead(false);
        if (free_slot == ~0u) {
            if (old_slot == ~0u || !vc_drop(&vc_tab[old_slot])) return false;
            free_slot = old_slot;
        }
        while (vc_bytes + (hi - lo) > VC_BUDGET) {  // over budget: the least recently used entry goes
            int lru = -1;
            for (int i = 0; i < VC_TAB; i++)
                if (vc_tab[i].buf && vc_tab[i].used_seq < pend && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
            if (lru < 0 || !vc_drop(&vc_tab[lru])) return false;
        }
        e = &vc_tab[free_slot];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = hi - lo,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &e->buf, &e->mem, NULL) != VK_SUCCESS) { memset(e, 0, sizeof *e); return false; }
        e->lo = lo; e->hi = hi;
        vc_bytes += hi - lo;
    }
    uint32_t now = (uint32_t)vk_frame_counter() + 1;
    if (e->epoch != vc_epoch || (e->gpu && now >= e->gpu_until)) {  // (pages turned GPU-written since, or their marks expired)
        bool was = e->gpu;
        uint32_t last = 0;
        for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) if (vc_page_gpu[p] > last) last = vc_page_gpu[p];
        e->gpu = last && now < last + VC_GPU_HOLD;
        e->gpu_until = last + VC_GPU_HOLD;
        e->epoch = vc_epoch;
        if (was && !e->gpu) { e->gen = 0; vc_gpu_expired = true; }  // (the GPU's writes aren't tracked: copied whole)
    }
    if (e->gpu) { prof_vc_bypass++; return false; }
    if (e->used_seq != pend) {  // first use in this submission: brought up to date by vc_flush before it runs
        e->used_seq = pend;
        vc_pend[vc_npend++] = (uint32_t)(e - vc_tab);
    }
    prof_vc_hits++;
    *buf = e->buf;
    *off = addr - gwin_base - lo;
    return true;
}

static int vc_pend_cmp(const void *a, const void *b) {
    uint64_t x = vc_tab[*(const uint32_t *)a].lo, y = vc_tab[*(const uint32_t *)b].lo;
    return x < y ? -1 : x > y;
}

// At submission (vk_submit_extra, before the upload buffer closes): the pages the CPU wrote in the ranges this
// submission draws from - one write tracking query per run of nearby ranges (a query costs about as much as scanning
// ~64 pages: ranges closer than that share one) - and their copies into the cache, ahead of the draws.
void vc_flush(void) {
    if (!vc_npend) return;
    qsort(vc_pend, (size_t)vc_npend, sizeof vc_pend[0], vc_pend_cmp);
    uint32_t g = ++vc_gen;
    static uint64_t pages[4096];
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < vc_npend;) {
        uint64_t lo = vc_tab[vc_pend[i]].lo, hi = vc_tab[vc_pend[i]].hi;
        int j = i + 1;
        for (; j < vc_npend && vc_tab[vc_pend[j]].lo <= hi + 64 * VC_PAGE; j++)
            if (vc_tab[vc_pend[j]].hi > hi) hi = vc_tab[vc_pend[j]].hi;
        uint64_t pg = VC_PAGE;
        int n = plat_written_pages(gwin_base + lo, hi - lo, pages, 4096, &pg);
        prof_vc_queries++;
        if (n < 0 || n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;  // (all)
        else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - gwin_base) / VC_PAGE] = g;
        if (n > 0) prof_vc_scan_pages += (uint64_t)n;
        i = j;
    }
    prof_vc_scan_ns += SDL_GetTicksNS() - t0;
    VkCommandBuffer cb = vk_upload_cmd();
    if (vc_gpu_expired) { vk_full_barrier(cb); vc_gpu_expired = false; }  // (after the earlier submissions' writes)
    for (int i = 0; i < vc_npend; i++) {
        VcEnt *e = &vc_tab[vc_pend[i]];
        VkBufferCopy cp[64];
        int nc = 0;
        for (uint64_t p = e->lo / VC_PAGE; p < e->hi / VC_PAGE; p++) {
            if (e->gen && vc_page_gen[p] <= e->gen) continue;
            uint64_t o = p * VC_PAGE;
            if (nc && cp[nc - 1].srcOffset + cp[nc - 1].size == o) { cp[nc - 1].size += VC_PAGE; continue; }
            if (nc == 64) { nc = 1; cp[0] = (VkBufferCopy){e->lo, 0, e->hi - e->lo}; break; }  // (scattered: the whole range)
            cp[nc++] = (VkBufferCopy){o, o - e->lo, VC_PAGE};
        }
        if (nc) {
            gmem_copy(cb, e->buf, cp, nc, false);
            for (int k = 0; k < nc; k++) prof_vc_copied += cp[k].size;
        }
        e->gen = g;
    }
    vc_npend = 0;
}

// Vertex / index data from the imported guest heap (renderer.m bind_guest reads g_guest_buffer the same way): the
// device-local cache above, else read in place - no per-draw copy, no index scan for the copy size. The game fences its
// CPU writes as on the Switch. Falls back to copies outside the heap or without the import; LARECOMP_VK_COPY_VERTICES=1
// forces copies.

// index: an index buffer. Never read from the transform feedback mirror: transform feedback writes vertex data only, the mirror has no INDEX_BUFFER usage (the validation
// layer: vkCmdBindIndexBuffer on it, 423 times in 15 s), and index data the game wrote into the arena's memory after a session (chunks count as written by 256-byte
// chunk) was read from stale mirror bytes: invisible walls and empty text boxes.
static bool guest_direct(int kind, uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {  // kind: 0 vertex, 1 index (bind_note)
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < gwin_base) return false;
    bind_note(kind, addr, size);
    if (gc_lazy) {
        uint64_t o = addr - HEAP_BASE;
        if (!size || o + size > HEAP_MAX) return false;
        int k = (int)(o >> gc_shift);
        if ((int)((o + size - 1) >> gc_shift) != k || !gmem_covers(o, size)) return false;  // (crossing chunks: copied)
        if (vcache_lookup(addr, size, buf, off)) return true;
        *buf = gc[k].buf;
        *off = o - ((uint64_t)k << gc_shift);
        return true;
    }
    if (addr + size > gwin_base + gmem_lo + gmem_size && gwin_want() != gmem_tried) { gmem_tried = gwin_want(); gmem_info(); }
    if (gmem_lo && xm_lookup(addr, size, buf, off)) { stat_vtx_cached += size; return true; }  // (the mirror needs no import)
    if (!GM_IN(addr, size)) return false;
    if (vcache_lookup(addr, size, buf, off)) { stat_vtx_cached += size; return true; }
    if (kind == 0 && xm_lookup(addr, size, buf, off)) { stat_vtx_cached += size; return true; }
    stat_vtx_host += size;
    *buf = gmem_buf;
    *off = GM_OFF(addr);
    return true;
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    kv_clear(&prog_kv);
    kv_clear(&pipe_kv);
    key_gen++;  // (the key memo: the tables' answers are gone)
    ds_gen++;   // (set reuse: the heap import below is destroyed at once)
    g_cs_n = 0;
    gcopy_gen++;
    if (gmem_buf) {
#ifdef __linux__
        bool placed = gmem_placed;
        heap_unmap_placed();
#endif
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
#ifdef __linux__
        if (placed && !plat_reset_guest(HEAP_BASE, HEAP_BASE + HEAP_MAX)) FATAL("cannot restore the guest heap mapping\n");
        gmem_dmabuf = false;  // (the next boot decides again)
#endif
    }
    gmem_size = gmem_tried = 0;
    gmem_lo = 0;
    xm_lo = 0, xm_back_n = 0;  // (the transform feedback mirror: placed again, nothing written yet)
    memset(xm_written, 0, sizeof xm_written);
    for (int k = 0; k < gc_n; k++) {  // (lazy chunks: their pages are replaced too)
        if (gc[k].buf) { vkDestroyBuffer(g_vk.dev, gc[k].buf, NULL); vkFreeMemory(g_vk.dev, gc[k].mem, NULL); }
        memset(&gc[k], 0, sizeof gc[k]);
    }
    gc_imported = 0; gc_scanned = 0; gc_imported_bytes = 0; gc_gen++;  // (gc_remembered stays: the same heap layout next boot)
    gc_remembered_heap = 0;  // (imported again at the next boot's first chunk set)
    if (gc_miss) { memset(gc_miss, 0, sizeof(uint32_t) * GC_MAX); memset(gc_miss + GC_MAX, 0xFF, sizeof(uint32_t) * GC_MAX); }
    for (int i = 0; i < VC_TAB; i++)  // (the vertex cache: copies of the old pages)
        if (vc_tab[i].buf) vmaDestroyBuffer(g_vk.vma, vc_tab[i].buf, vc_tab[i].mem);
    memset(vc_tab, 0, sizeof vc_tab);
    vc_npend = 0;
    vc_free_dead(true);
    vc_bytes = 0;
    if (vc_page_gpu) memset(vc_page_gpu, 0, gwin_max / VC_PAGE * sizeof *vc_page_gpu);
    vc_epoch++;
    snap_reset();  // (overlap: the storage copies' ring follows the heap, decided again once it is set up)
    st_defaults();
}

// Shadow (sampler*Shadow) slots need a depth view whose format can be compared (VUID-vkCmdDrawIndexed-None-06479):
// the colour dummy or a colour texture there is replaced by a 1x1 depth dummy at 1.0 (lit, textures.c). Each cause is
// logged once (LARECOMP_SHADOW_SLOT_LOG=1: every change of cause / handle, up to 200 lines), with the texture's
// address, size and format, so a run shows why the slot had nothing usable.
static void shadow_slot_check(int stage, const ShaderTexture *st_, int i, uint64_t h, uint32_t tic, const NvnTexEntry *e,
                              NvnTexture *t, VkImageView *view) {
    VkImageView dummy = vk_dummy_view(st_->type);
    if (*view && *view != dummy && t && vk_tex_depth_compare(t)) return;
    enum { HANDLE0, NO_ENTRY, FINALIZED, UNREGISTERED, MULTISAMPLE, FEEDBACK_ARRAY, VIEW_FAILED, TYPE_MISMATCH,
           DEPTH_NO_COMPARE, COLOUR, NCAUSE };
    static const char *names[NCAUSE] = {"handle 0 (nothing bound)", "handle past the texture pool", "texture finalized",
                                        "pool slot not registered", "multisample texture", "feedback snapshot as array",
                                        "view creation failed", "type mismatch", "depth format without compare support",
                                        "colour format"};
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    int why;
    if (!h) why = HANDLE0;
    else if (!e) why = NO_ENTRY;
    else if (!t) why = e->dead_format ? FINALIZED : UNREGISTERED;
    else if (*view == dummy || !*view) {
        bool fb = false;
        for (int k = 0; k < nfeedback; k++) fb |= feedback_tex[k] == e->tex;
        why = b && b->samples > 1 ? MULTISAMPLE : fb && st_->type == TEXTYPE_2D_ARRAY ? FEEDBACK_ARRAY : !*view ? VIEW_FAILED : TYPE_MISMATCH;
    } else why = b && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? DEPTH_NO_COMPARE : COLOUR;
    static int verbose = -1, nverbose;
    static bool logged[NCAUSE];
    static int last_why = -1;
    static uint64_t last_h;
    if (verbose < 0) verbose = getenv("LARECOMP_SHADOW_SLOT_LOG") != NULL;
    bool log = !logged[why] || (verbose && nverbose < 200 && (why != last_why || h != last_h));
    if (log) {
        logged[why] = true;
        if (verbose) nverbose++;
        last_why = why; last_h = h;
        if (t)
            LOG("[vk] shadow slot t%d (stage %d, type %d) f%llu fs %016llx: %s - handle %#llx tic %u, texture %#llx %ux%ux%u "
                "target %u nvn fmt %#x vk fmt %d aspect %#x samples %u: 1x1 depth dummy instead%s\n",
                i, stage, st_->type, (unsigned long long)vk_frame_counter(), (unsigned long long)g_cur_fs_hash, names[why],
                (unsigned long long)h, tic, (unsigned long long)t->addr, t->width, t->height, t->depth, t->target, t->format,
                b ? b->format : 0, b ? b->aspect : 0, b ? b->samples : 0, verbose ? "" : " (once per cause)");
        else
            LOG("[vk] shadow slot t%d (stage %d, type %d) f%llu fs %016llx: %s - handle %#llx tic %u (finalized nvn fmt %#x): "
                "1x1 depth dummy instead%s\n", i, stage, st_->type, (unsigned long long)vk_frame_counter(),
                (unsigned long long)g_cur_fs_hash, names[why], (unsigned long long)h, tic, e && e->dead_format ? e->dead_format - 1 : 0,
                verbose ? "" : " (once per cause)");
    }
    *view = vk_dummy_depth_view(st_->type);
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2], float fr[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    fr[0] = fr[1] = 1;
    NvnTexEntry *ent = NULL;  // (shadow slots: what the view came from)
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = ent = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = ENV("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(naliases ? alias_read(e->tex) : e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            renderer_texture_fraction(e->tex, &fr[0], &fr[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)
                }
        } else if (e->dead_format) {
            nvn_dead_slot_sampled(e, tic);
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) {
        NvnSamplerDesc *sd = &((NvnSamplerDesc *)st.spool->entries)[tsc];
        smp = st_->stencil || st_->sint ? vk_sampler_nearest(sd) : vk_sampler(sd);  // (integer texels: unfiltered)
    }
    {   // (debug) LARECOMP_CS_DUMMY_TEX=<compute code hex>: that compute shader samples 1x1 black textures
        static uint64_t dummy_cs = ~0ull;
        if (dummy_cs == ~0ull) dummy_cs = ENV("LARECOMP_CS_DUMMY_TEX") ? strtoull(ENV("LARECOMP_CS_DUMMY_TEX"), NULL, 16) : 0;
        if (stage == 5 && dummy_cs && st.program && (dummy_cs == 1 || st.program->stages[5].code_addr == dummy_cs))
            view = VK_NULL_HANDLE;  // (1: every compute shader - code addresses change between runs)
    }
    {   // A compute shader reading a scaled depth target with a mip chain at a non-integer scale (1.5x, Match display,
        // Dynamic, the shares) gets a 1x1 dummy: no occluder, so nothing is culled for being hidden. That is Pokémon
        // Z-A's GPU occlusion culling reading its depth pyramid (the scene depth's own mip levels). Scaled by 1.5 the
        // levels don't keep the ratio (135 -> 202, 33 -> 50, 16 -> 25), the culling read the wrong depths and dropped
        // visible objects - lamp posts, hedges, parts of buildings vanishing as the player walked. Integer scales are
        // exact and keep it. The game's frustum and distance culling still run. LARECOMP_FRACTIONAL_OCCLUSION=1: keep it.
        static int keep = -1;
        if (keep < 0) keep = ENV("LARECOMP_FRACTIONAL_OCCLUSION") && atoi(ENV("LARECOMP_FRACTIONAL_OCCLUSION"));
        TexBackend *pb = stage == 5 && view && !keep && ent && ent->tex ? (TexBackend *)ent->tex->backend : NULL;
        if (pb && pb->scalable && pb->levels > 1 && (pb->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
            (pb->sx != floorf(pb->sx) || pb->sy != floorf(pb->sy))) {
            static int nlog;
            if (nlog++ < 3) LOG("[vk] non-integer scale %.2f: occlusion culling's depth pyramid (%ux%u, %u levels) off for this compute shader\n",
                                pb->sx, ent->tex->width, ent->tex->height, pb->levels);
            view = VK_NULL_HANDLE;
        }
    }
    if (st_->shadow)
        shadow_slot_check(stage, st_, i, h, tic, ent, ent && ent->tex ? (naliases ? alias_read(ent->tex) : ent->tex) : NULL, &view);
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(!st_->stencil && !st_->sint);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static float g_mirror_w;  // mirror mode's composite: its render target width (rscale[65].x; 0 otherwise)

// (set reuse) what a set holds, one entry per descriptor written, in stage_set's order (fixed per program)
typedef struct { uint32_t binding, type; uint64_t a, b, c; } SetEnt;
typedef struct SetCache { uint64_t seq; uint32_t view_gen, ds_gen; int n; VkDescriptorSet set; SetEnt e[MAX_BIND]; } SetCache;

// dyn / ndyn: the set's dynamic offsets are appended at dyn[*ndyn] (binding order, as vkCmdBindDescriptorSets takes
// them); NULL for programs without dynamic buffers (compute).
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[RECOLOUR_TINT_FLOATS], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii,
                                 uint32_t *dyn, uint32_t *ndyn) {
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    uint32_t dbind[MAX_BIND], doff[MAX_BIND];
    int n = 0, nd = 0;
    int sg = stage == 1 ? 1 : 0;
    bool lean = cbuf_lean();
    // ([65]: mirror mode, renderer.m; [66..69]: vertex transform feedback buffers) (lean: only rows 0, 1..ntex and 65
    // are set and read - not with transform feedback, whose rows the lean copy leaves out)
    float rs[70][4];
    bool xfb_rows = stage == 0 && xfb_on;
    if (!lean || xfb_rows) memset(rs, 0, sizeof rs);
    rs[0][0] = rs[0][1] = 1; rs[0][2] = rs[0][3] = 0;
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    rs[65][0] = g_mirror_w; rs[65][1] = rs[65][2] = rs[65][3] = 0;
    if (stage == 0 && xfb_on)
        for (int b = 0; b < 4; b++) {
            uint64_t a = xfb_addr[b];
            if (!a || a < gwin_base || a - gwin_base + xfb_size[b] > gwin_max || (a & 3)) continue;
            uint64_t base = xm_draw ? xm_lo : gwin_base + gmem_lo;  // (the transform feedback mirror's offsets, or the import's)
            if (a < base) continue;
            uint32_t u[4] = {(uint32_t)((a - base) >> 2), (uint32_t)(xfb_size[b] >> 2), 1, 0};  // (offset, size in words)
            memcpy(rs[66 + b], u, 16);
        }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2], fr[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) {
                ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1];
                fr[0] = tex_fr[sg][ti][0]; fr[1] = tex_fr[sg][ti][1];
            }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh, fr);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; fr[0] = fr[1] = 1; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
                tex_fr[sg][ti][0] = fr[0]; tex_fr[sg][ti][1] = fr[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (is_ubo(b->type) && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = b->binding == GFAST_BUFFER_INDEX ? gfast_info()
                  : b->binding == XFB_BUFFER_INDEX && xm_draw ? (VkDescriptorBufferInfo){xm_buf, 0, XM_SIZE} : gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || !is_ubo(b->type) || (b->binding != 18 && b->binding != 19 && b->binding != GMAP_BUFFER_INDEX)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == GMAP_BUFFER_INDEX) {  // (compute) this dispatch's fast-copy table: gfast_prepare
            bi[n] = ubo_span(vk_transient_copy(gmap_tab, sizeof gmap_tab, 256), sizeof gmap_tab);
        } else if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) {  // scale x, y, content part x, y
                rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; rs[1 + t][2] = tex_fr[sg][t][0]; rs[1 + t][3] = tex_fr[sg][t][1];
            }
            if (lean && !xfb_rows) bi[n] = ubo_span(rscale_dedupe(sg, (const float (*)[4])rs, si->ntex < 64 ? si->ntex : 64), 66 * 16);
            else bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[RECOLOUR_TINT_FLOATS] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, sizeof zero, 256), sizeof zero);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    // dynamic buffers: the offset moves to the bind (the descriptor keeps buffer and range)
    for (int i = 0; i < n; i++)
        if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            int j = nd++;  // (insertion by binding: the order vkCmdBindDescriptorSets takes the offsets in)
            while (j > 0 && dbind[j - 1] > w[i].dstBinding) { dbind[j] = dbind[j - 1]; doff[j] = doff[j - 1]; j--; }
            dbind[j] = w[i].dstBinding;
            doff[j] = (uint32_t)bi[i].offset;
            bi[i].offset = 0;
        }
    if (nd) {
        if (!dyn || !ndyn) FATAL("stage_set: dynamic buffers without an offset array\n");
        memcpy(dyn + *ndyn, doff, (size_t)nd * sizeof doff[0]);
        *ndyn += (uint32_t)nd;
    }
    extern uint32_t vk_view_gen;
    SetCache *c = NULL;
    SetEnt e[MAX_BIND];
    if (p->reuse && set < 2) {  // the same contents as this program's last set of this index: bind that one again
        if (!p->scache[set]) p->scache[set] = calloc(1, sizeof *p->scache[set]);
        c = p->scache[set];
        memset(e, 0, (size_t)n * sizeof e[0]);  // (padding: compared with memcmp)
        for (int i = 0; i < n; i++) {
            e[i].binding = w[i].dstBinding;
            e[i].type = (uint32_t)w[i].descriptorType;
            if (w[i].pImageInfo) {
                e[i].a = (uint64_t)(uintptr_t)ii[i].sampler; e[i].b = (uint64_t)(uintptr_t)ii[i].imageView; e[i].c = (uint64_t)ii[i].imageLayout;
            } else {
                e[i].a = (uint64_t)(uintptr_t)bi[i].buffer; e[i].b = bi[i].offset; e[i].c = bi[i].range;
            }
        }
        if (c->set && c->seq == vk_pending_seq() && c->view_gen == vk_view_gen && c->ds_gen == ds_gen && c->n == n &&
            !memcmp(c->e, e, (size_t)n * sizeof e[0])) {
            prof_sets_reused++;
            return c->set;
        }
    }
    VkDescriptorSet ds = vk_alloc_set(layout);
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    if (n) rec_UpdateDescriptorSets((uint32_t)n, w);
    prof_sets_new++;
    if (c) {
        c->set = ds; c->seq = vk_pending_seq(); c->view_gen = vk_view_gen; c->ds_gen = ds_gen; c->n = n;
        memcpy(c->e, e, (size_t)n * sizeof e[0]);
    }
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; uint64_t code; } CsProg;

// Compute programs run in 32-lane subgroups, as Maxwell's warps do. The game's compute shaders use ballot / shuffle / vote on a 32-lane warp (Pokémon Z-A's GPU culling
// counts the visible instances of a batch with ballot + a leader's atomicAdd); AMD drivers run compute in 64-lane waves by default (RADV), where those counts are wrong:
// whole parts of a scene (walls, arches, lamps, their shadows) were culled, and the translated shader could hang the GPU. Vulkan 1.3 requires the subgroup size control;
// where the device cannot run 32 lanes (min > 32) nothing changes. LARECOMP_VK_CS_SUBGROUP=0: the driver's choice.
static bool cs_subgroup32(void) {
    static int ok = -1;
    if (ok < 0) {
        const char *e = ENV("LARECOMP_VK_CS_SUBGROUP");
        ok = 0;
        if (g_vkc.api >= VK_API_VERSION_1_3 && !(e && !strcmp(e, "0"))) {
            VkPhysicalDeviceSubgroupSizeControlProperties sgp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
            VkPhysicalDeviceProperties2 pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sgp};
            vkGetPhysicalDeviceProperties2(g_vk.phys, &pp);
            ok = sgp.minSubgroupSize <= 32 && sgp.maxSubgroupSize >= 32 && (sgp.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
            LOG("[vk] compute subgroup sizes %u..%u: %s\n", sgp.minSubgroupSize, sgp.maxSubgroupSize, ok ? "compute programs run in 32 lanes" : "left to the driver");
        }
    }
    return ok;
}

static void build_compute(void *arg) {
    CsProg *cp = arg;
    uint64_t t0 = SDL_GetTicksNS();
    int bs = build_begin("pipeline cs", cp->code, 0);
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); build_end(bs); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    if (!m) {
        LOG("[compute] cs %llx: shader module creation failed\n", (unsigned long long)cp->code);
        atomic_store(&cp->p.state, 2);
        build_end(bs);
        return;
    }
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req32 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
                                                                 .requiredSubgroupSize = 32};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = cs_subgroup32() ? &req32 : NULL,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    SLOW_FEEDBACK(ci);
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    SLOW_FEEDBACK_HIT();
    atomic_fetch_add(&stat_compiles, 1);
    atomic_fetch_add(&builds_done, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    build_end(bs);
    if (r != VK_SUCCESS) LOG("[compute] cs %llx: pipeline failed: %d\n", (unsigned long long)cp->code, r);
    slow_build(t0, "pipeline cs", cp->code, 0);
}

static struct CsEnt { uint64_t addr, fp; CsProg *cp; } g_cs_tab[256];  // compute programs by code address (code_fp)

static uint64_t cs_content_key(uint64_t code_hash, const uint32_t local[3]) {
    uint64_t h = code_hash;
    for (int i = 0; i < 3; i++) { h ^= local[i]; h *= 0x100000001B3ull; }
    return h;
}

static bool translate_compute(CsProg *cp, const uint8_t *code, size_t max, uint64_t addr) {
    uint64_t wl, wn;
    gmem_window_get(&wl, &wn);
    ShaderKey k = {0};
    k.glsl = 1;
    pthread_mutex_lock(&translate_lock);  // (the globals too: the render thread translates as well - run_queued_now)
    maxwell_gmem_base = gwin_base + wl, maxwell_gmem_size = wn;
    maxwell_gmem_chunk_shift = gmem_chunk_shift();
    cp->si = maxwell_translate(code, max, STAGE_COMPUTE, &k);
    if (ENV("LARECOMP_DUMP_GLSL")) {  // (debug) build/glsl/<code address>.comp
        mkdir("build/glsl", 0755);
        char path[96];
        snprintf(path, sizeof path, "build/glsl/%llx.comp", (unsigned long long)addr);
        FILE *f = fopen(path, "wb");
        if (f) { fputs(cp->si->msl ? cp->si->msl : cp->si->error, f); fclose(f); }
    }
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)addr);
        atomic_store(&cp->p.state, 2);
        return false;
    }
    return true;
}

// The table's entry for this code address: the one it had (other code there now: it gets the new program) or a new one
static void cs_tab_put(int at, uint64_t addr, uint64_t fp, CsProg *cp) {
    if (at < 0 && g_cs_n < 256) at = g_cs_n++;
    if (at >= 0) g_cs_tab[at] = (struct CsEnt){addr, fp, cp};
}

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n, at = -1;
    uint64_t fp = code_fp(ss);
    for (int i = 0; i < n && at < 0; i++)
        if (tab[i].addr == ss->code_addr) {
            if (tab[i].fp == fp) return tab[i].cp;
            at = i;  // (other code there now: this entry gets the new program)
        }
    uint32_t local[3];
    for (int i = 0; i < 3; i++) {
        local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!local[i]) local[i] = 1;
    }
    if (shader_rec_recording()) {
        ShaderRec rec = {.kind = 'C', .vs = shader_stage_hash(ss), .local = {local[0], local[1], local[2]}};
        shader_rec_note(&rec);
        shader_seen_code(rec.vs, ss);  // (its code for the next start's preparation: shader_prep.h)
    }
    uint64_t ch = shader_stage_hash(ss);
    CsProg *cp = ch && prep_cs_n ? PREP_GET(prep_cs, cs_content_key(ch, local)) : NULL;  // prepared before the game
    if (cp) {
        cs_tab_put(at, ss->code_addr, fp, cp);
        shader_fp_check(&cp->p.fp[0], ss, "cs");
        return cp;
    }
    shader_first_visit("compute", ch, 0, cs_content_key(ch, local));
    cp = calloc(1, sizeof *cp);
    shader_fp_check(&cp->p.fp[0], ss, "cs");  // (takes the fingerprint)
    cp->code = ss->code_addr;  // (for the build logs)
    note_code(ss, fp, "cs");
    cs_tab_put(at, ss->code_addr, fp, cp);
    memcpy(cp->local, local, sizeof local);
    if (!translate_compute(cp, (const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, ss->code_addr))
        return cp;
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp, &cp->p.state);
    return cp;
}

// ================================================================== shader preparation (shader_prep.h)
// Each record: translated here (one thread, the translator isn't re-entrant), compiled and built on the worker
// threads (run_job), kept in the prep_* tables for the game.
typedef struct { void (*fn)(void *); void *arg; } PrepJob;
static void prep_job_run(void *a) {
    PrepJob *j = a;
    j->fn(j->arg);
    free(j);
    prep_pending--;
}
static void prep_job(void (*fn)(void *), void *arg) {
    PrepJob *j = malloc(sizeof *j);
    j->fn = fn, j->arg = arg;
    prep_pending++;
    run_job(prep_job_run, j, NULL);
}

static _Atomic int prep_pipe_dups;  // records whose pipeline another record already made
// (Pokémon Z-A: only once the game has set up its physical memory window - the translations bake its base and size
// in, and before that it's Link's Awakening's heap: the start screen's preparation would have made every program with
// the wrong one. Z-A's runs at its start instead, shader_prep.c shader_prep_boot)
bool game_files_are_la(const char *game_dir);  // (runtime/game_addresses.c)
bool renderer_prepare_begin(void) { return g_vk.dev != VK_NULL_HANDLE && (gwin()->phys || game_files_are_la("extracted")); }
int renderer_prepare_pending(void) { return prep_pending; }
static void gpl_probe(void);
static void gpl_probe_note(Pipe *pp);
void renderer_prepare_end(void) {
    vk_pipeline_cache_save();  // (otherwise only kept at exit)
    static bool logged;        // (the counts: once per process, the background preparation runs once)
    if (!logged) {
        logged = true;
        pthread_mutex_lock(&prep_lock);
        size_t n = prep_pipes_n;
        pthread_mutex_unlock(&prep_lock);
        LOG("[vk] prepared %zu pipelines (%d records shared one, vertex input %s)\n", n, atomic_load(&prep_pipe_dups),
            g_vk.vids ? "dynamic" : "in each pipeline");
        if (g_vk.gpl_probe) gpl_probe();
    }
}

typedef struct { Pipe *pp; ShaderRec r; } PrepPipe;
static void prep_pipeline(void *arg) {
    PrepPipe *a = arg;
    Prog *pr = a->pp->k.prog;
    // its program's compile was queued before (the queue is first in, first out): wait for it to finish
    while (atomic_load(&pr->state) == 0) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
    if (atomic_load(&pr->state) != 1) { atomic_store(&a->pp->state, 2); free(a); return; }
    const ShaderRec *r = &a->r;
    // the pass's attachment formats as begin_pass makes them for these targets
    VkFormat color[8] = {0}, depth = VK_FORMAT_UNDEFINED, stencil = VK_FORMAT_UNDEFINED;
    uint32_t ncolor = r->ncolor;
    for (uint32_t i = 0; i < ncolor && i < 8; i++) {
        if (!r->color[i]) continue;
        VkFormat b = vk_image_format(r->color[i], NULL);
        color[i] = r->cview[i] && vk_format(r->color[i]) == b ? vk_format(r->cview[i]) : b;
    }
    while (ncolor && !r->color[ncolor - 1]) ncolor--;
    if (r->depth && renderer_format_is_depth(r->depth)) {
        VkImageAspectFlags asp;
        VkFormat b = vk_image_format(r->depth, &asp);
        if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) depth = b;
        if (asp & VK_IMAGE_ASPECT_STENCIL_BIT) stencil = b;
    }
    const PipeIn in = {r->attrib, r->nattrib, r->stream, r->nstream, r->color, r->channel_mask, r->blend_on, r->blend,
                       r->a2c, r->topo, r->poly_fill};
    pipe_key_fill(&a->pp->k, pr, &in, color, ncolor, depth, stencil, r->samples ? r->samples : 1, NULL);
    uint64_t ph = hash_words(0xcbf29ce484222325ull, &a->pp->k, sizeof a->pp->k);
    if (!ph) ph = 1;
    // Another record makes the same pipeline (with dynamic vertex input: every record differing only in its vertex
    // layout): built once. Claimed before the build, so records on other worker threads at the same time see it too
    // (the game skips a draw whose prepared pipeline is still building or failed, as with one it builds itself).
    pthread_mutex_lock(&prep_lock);
    bool dup = prep_get(prep_pipes, prep_pipes_cap, ph) != NULL;
    if (!dup) prep_put(&prep_pipes, &prep_pipes_cap, &prep_pipes_n, ph, a->pp);
    pthread_mutex_unlock(&prep_lock);
    if (dup) { atomic_fetch_add(&prep_pipe_dups, 1); free(a->pp); free(a); return; }
    build_pipeline(a->pp);
    if (g_vk.gpl_probe) gpl_probe_note(a->pp);
    free(a);
}

bool renderer_prepare(const ShaderRec *r, const ShaderBlob *vsb, const ShaderBlob *fsb) {
    if (r->kind == 'C') {
        uint64_t ck = cs_content_key(r->vs, r->local);
        if (PREP_GET(prep_cs, ck)) return true;
        CsProg *cp = calloc(1, sizeof *cp);
        cp->code = r->vs;  // (prepared: its content hash, for the build logs)
        memcpy(cp->local, r->local, sizeof cp->local);
        if (!translate_compute(cp, vsb->code, vsb->const_offset ? vsb->const_offset : 0x10000, 0)) return false;
        PREP_PUT(prep_cs, ck, cp);
        prep_job(build_compute, cp);
        return true;
    }
    // the keys the draw path makes for this state (draw_)
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = (r->flags & SR_FLIP_Y) != 0;
    vkey.depth_minus_one = (r->flags & SR_DEPTH_MINUS_ONE) != 0;
    vkey.flip_x = fkey.flip_x = (r->flags & SR_FLIP_X) != 0;
    for (int i = 0; i < r->nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(r->attrib[i] & 0xFF);
    for (int i = 0; i < r->ncolor && i < 8; i++) if (r->color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(r->color[i]);
    uint64_t ck = prog_content_key(r->vs, r->fs, &vkey, &fkey);
    Prog *p = PREP_GET(prep_progs, ck);
    if (!p) {
        p = calloc(1, sizeof *p);
        p->code[0] = (uintptr_t)vsb->code;
        p->code[1] = (uintptr_t)fsb->code;
        p->cmax[0] = vsb->const_offset ? vsb->const_offset : 0x10000;
        p->cmax[1] = fsb->const_offset ? fsb->const_offset : 0x10000;
        p->vkey = vkey;
        p->fkey = fkey;
        p->prep_vh = r->vs, p->prep_fh = r->fs;
        bool ok = translate_program_now(p);
        p->code[0] = p->code[1] = 0;  // (the game files' copies are gone after this call)
        PREP_PUT(prep_progs, ck, p);  // (failed ones too: the game then skips them as it would its own)
        if (!ok) return false;
        prep_job(compile_program, p);
    }
    if (atomic_load(&p->state) == 2) return false;
    PrepPipe *a = calloc(1, sizeof *a);
    a->pp = calloc(1, sizeof *a->pp);
    a->pp->k.prog = p;
    a->r = *r;
    prep_job(prep_pipeline, a);
    return true;
}

// ================================================================== graphics pipeline library probe (measurement only)
// LARECOMP_VK_GPL_PROBE=1 (or =<N>): after shader preparation, the first N prepared pipelines (20; in the order the
// list's records built them) are built again three ways and timed, on the preparing thread, one at a time:
//   full:      one vkCreateGraphicsPipelines, as the game builds them;
//   libraries: the four VK_EXT_graphics_pipeline_library parts (vertex input, pre-rasterization with the vertex shader,
//              fragment shader, fragment output) plus a fast link (no link-time optimization);
//   optimized: the same parts linked again with link-time optimization (what would replace a fast-linked pipeline).
// Every build is cold: no pipeline cache, and a specialization constant no shader reads (a new value per build and
// run) makes each one new to the driver's own caches. The results are destroyed; only [gpl] log lines remain.
// Nothing else uses the extension: the device enables it only with this variable set (device.c).
#ifdef VK_EXT_graphics_pipeline_library
#define GPL_PROBE_MAX 256
static Pipe *gpl_sample[GPL_PROBE_MAX];
static int gpl_nsample;
static void gpl_probe_note(Pipe *pp) {
    if (atomic_load(&pp->state) != 1) return;
    pthread_mutex_lock(&prep_lock);
    if (gpl_nsample < GPL_PROBE_MAX) gpl_sample[gpl_nsample++] = pp;
    pthread_mutex_unlock(&prep_lock);
}

static double gpl_ms_since(uint64_t t0) { return (double)(SDL_GetTicksNS() - t0) / 1e6; }

// One pipeline: [0] full, [1] vertex input, [2] pre-rasterization, [3] fragment shader, [4] fragment output, [5] fast
// link, [6] optimized link (ms). false: a build failed (logged).
static bool gpl_probe_one(const PipeKey *k, uint32_t nonce, double t[7]) {
    PipeCI c;
    pipe_ci_fill(&c, k);
    VkSpecializationMapEntry me = {0x7fff, 0, 4};  // (no shader declares this id: it only makes the build unique)
    VkSpecializationInfo spec = {1, &me, 4, &nonce};
    c.stages[0].pSpecializationInfo = c.stages[1].pSpecializationInfo = &spec;
    VkPipeline full = VK_NULL_HANDLE, lib[4] = {0}, fast = VK_NULL_HANDLE, opt = VK_NULL_HANDLE;
    const char *fail = NULL;
    VkResult r;
    uint64_t t0 = SDL_GetTicksNS();
    r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &c.pi, NULL, &full);
    t[0] = gpl_ms_since(t0);
    if (r != VK_SUCCESS) { fail = "full"; goto out; }
    nonce ^= 0x80000000u;  // (the libraries: new to the caches too, not the full pipeline's shaders again)
    static const VkGraphicsPipelineLibraryFlagsEXT part[4] = {
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    static const char *part_name[4] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};
    for (int i = 0; i < 4; i++) {
        VkGraphicsPipelineLibraryCreateInfoEXT li = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                     .pNext = &c.ri, .flags = part[i]};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
            .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
            .pDynamicState = &c.dy, .layout = c.pi.layout};
        switch (i) {
        case 0: pi.pVertexInputState = c.pi.pVertexInputState; pi.pInputAssemblyState = &c.ia; break;
        case 1: pi.stageCount = 1; pi.pStages = &c.stages[0]; pi.pViewportState = &c.vs; pi.pRasterizationState = &c.rs; break;
        case 2: pi.stageCount = 1; pi.pStages = &c.stages[1]; pi.pDepthStencilState = &c.ds; pi.pMultisampleState = &c.ms; break;
        case 3: pi.pColorBlendState = &c.cb; pi.pMultisampleState = &c.ms; break;
        }
        t0 = SDL_GetTicksNS();
        r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &lib[i]);
        t[1 + i] = gpl_ms_since(t0);
        if (r != VK_SUCCESS) { fail = part_name[i]; goto out; }
    }
    for (int o = 0; o < 2; o++) {
        VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4,
                                             .pLibraries = lib};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
            .flags = o ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0, .layout = c.pi.layout};
        t0 = SDL_GetTicksNS();
        r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, o ? &opt : &fast);
        t[5 + o] = gpl_ms_since(t0);
        if (r != VK_SUCCESS) { fail = o ? "optimized link" : "fast link"; goto out; }
    }
out:
    if (fail) LOG("[gpl] probe: %s build failed (%d)\n", fail, r);
    VkPipeline all[7] = {full, lib[0], lib[1], lib[2], lib[3], fast, opt};
    for (int i = 0; i < 7; i++) if (all[i]) vkDestroyPipeline(g_vk.dev, all[i], NULL);
    return !fail;
}

static void gpl_probe(void) {
    const char *e = getenv("LARECOMP_VK_GPL_PROBE");
    int want = e && atoi(e) > 1 ? atoi(e) : 20;
    if (want > GPL_PROBE_MAX) want = GPL_PROBE_MAX;
    pthread_mutex_lock(&prep_lock);
    int n = gpl_nsample < want ? gpl_nsample : want;
    pthread_mutex_unlock(&prep_lock);
    if (!n) { LOG("[gpl] probe: no prepared pipelines to measure\n"); return; }
    LOG("[gpl] probe: building %d prepared pipelines three ways (cold, one at a time; vertex input %s)\n", n,
        g_vk.vids ? "dynamic" : "in each pipeline");
    double sum[7] = {0}, mx[7] = {0};
    int ok = 0;
    uint32_t nonce = (uint32_t)SDL_GetTicksNS() & 0x7fffffffu;
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < n; i++) {
        const PipeKey *k = &gpl_sample[i]->k;
        double t[7] = {0};
        if (!gpl_probe_one(k, nonce + (uint32_t)i, t)) continue;
        ok++;
        for (int j = 0; j < 7; j++) { sum[j] += t[j]; if (t[j] > mx[j]) mx[j] = t[j]; }
        LOG("[gpl] %2d vs/fs %016llx %016llx: full %.1f ms; libraries %.1f + %.1f + %.1f + %.1f ms, fast link %.2f ms; "
            "optimized link %.1f ms\n", i, (unsigned long long)k->prog->prep_vh, (unsigned long long)k->prog->prep_fh,
            t[0], t[1], t[2], t[3], t[4], t[5], t[6]);
    }
    if (!ok) { LOG("[gpl] probe: every build failed\n"); return; }
    double libs = sum[1] + sum[2] + sum[3] + sum[4];
    LOG("[gpl] probe summary, %d pipelines (%.0f ms in all): full %.0f ms (avg %.1f, max %.1f); "
        "libraries %.0f ms (vertex input %.1f, pre-rasterization %.0f, fragment shader %.0f, fragment output %.1f) "
        "+ fast link %.1f ms (avg %.2f, max %.2f); optimized link %.0f ms (avg %.1f, max %.1f)\n",
        ok, gpl_ms_since(t0), sum[0], sum[0] / ok, mx[0], libs, sum[1], sum[2], sum[3], sum[4], sum[5], sum[5] / ok, mx[5],
        sum[6], sum[6] / ok, mx[6]);
}
#else
static void gpl_probe_note(Pipe *pp) { (void)pp; }
static void gpl_probe(void) {}
#endif

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp || atomic_load(&cp->p.state) != 1) { SKIP_CS(!cp ? SK_NO_STAGES : atomic_load(&cp->p.state) == 2 ? SK_CS_FAILED : SK_CS_BUILDING); return; }
    {   // LARECOMP_SKIP_CS=<code address hex> (debug): skip that compute shader's dispatches, to see what it does
        static uint64_t skip = ~0ull;
        if (skip == ~0ull) { const char *e = getenv("LARECOMP_SKIP_CS"); skip = e ? strtoull(e, NULL, 16) : 0; }
        if (skip && st.program->stages[5].code_addr == skip) { SKIP_CS(SK_DEBUG_SKIP); return; }
    }
    if (cp->si->uses_gmem && ENV("LARECOMP_LOG_CS_SBUF")) {  // (debug) the storage buffers bound to gmem dispatches
        static uint64_t seen_cs[16];
        static int seen_n[16];
        int k = 0;
        while (k < 15 && seen_cs[k] && seen_cs[k] != st.program->stages[5].code_addr) k++;
        seen_cs[k] = st.program->stages[5].code_addr;
        if (seen_n[k]++ < 24) {
            LOG("[cs] %llx %ux%ux%u:", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
            for (int i = 0; i < 16; i++)
                if (st.sbuf[5][i].addr) LOG(" sb%d %llx+%llx", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
            LOG("\n");
        }
    }
    snap_apply(5, 0xFFFF);  // (overlap) every bound slot: the probe / copy decision per range, before gfast and the sets read the table
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u) cs %llx gmem %d", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    if (dl_on > 0) vk_checkpoint(cb, dl_n);
    nfeedback = 0;
    if (ct_on()) {
        LOG("[ct] dispatch cs %llx %ux%ux%u | tex", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < cp->si->ntex; i++) {
            NvnTexture *t = shader_texture_object(5, &cp->si->tex[i]);
            if (t) LOG(" %s%p/%#x", cp->si->tex[i].is_image ? "I:" : "", (void *)t, t->format);
        }
        LOG("\n");
    }
    if (ENV("LARECOMP_LOG_CS_TEX")) {  // (debug) each compute shader's textures, once
        static uint64_t seen[64];
        static int nseen;
        uint64_t code = st.program->stages[5].code_addr;
        bool have = false;
        for (int i = 0; i < nseen; i++) have |= seen[i] == code;
        if (!have && nseen < 64) {
            seen[nseen++] = code;
            for (int i = 0; i < cp->si->ntex; i++) {
                NvnTexture *t = shader_texture_object(5, &cp->si->tex[i]);
                LOG("[cs-tex] %llx tex%d: %p %ux%u fmt %#x levels %u target %u addr %llx\n", (unsigned long long)code, i, (void *)t,
                    t ? t->width : 0, t ? t->height : 0, t ? t->format : 0, t ? t->levels : 0, t ? t->target : 0,
                    (unsigned long long)(t ? t->addr : 0));
            }
        }
    }
    gfast_stores = cp->si->uses_gmem_store;
    if (cp->si->uses_gmem_map) gfast_prepare(cb);
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL, NULL, NULL);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    if (gc_shift && cp->si->uses_gmem) gmem_chunk_bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, GMEM_CHUNK_SET(STAGE_COMPUTE), 1u << STAGE_COMPUTE);
    rec_CmdDispatch(cb, gx, gy, gz);
    if (cp->si->uses_gmem_map && cp->si->uses_gmem_store) gfast_finish(cb);
    if (cp->si->uses_gmem_store) vc_mark_sbufs(5);
    stat_dispatches++;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

// Texture handle slots of a stage bound in NVN depth/stencil mode STENCIL or with an unsigned integer format
// (ShaderKey.stencil_slots: usamplers), and with a signed integer format (*sint: isamplers). Maxwell's fetches give
// those formats' bits; a float sampler on them is invalid in Vulkan (VUID-vkCmdDraw-format-07753) - NVIDIA returned the
// bits anyway, other drivers needn't.
// Each entry's answer (0 neither, 1 usampler, 2 isampler) is kept per texture pool entry for one submitted command list
// (exec_gen: the game rewrites its pool between submissions, not within one): every draw went through all 32 slots of
// its stages, reading each bound texture's NvnTexture and backend - two cache misses a slot, 5.5 % of the render
// thread at 60 fps. LARECOMP_VK_NO_SLOT_MEMO=1: computed each time.
static uint32_t exec_gen;  // (backend_execute: one per submitted command list)
static int slot_kind(const NvnTexEntry *e) {
    if (!renderer_format_has_stencil(e->tex->format)) return vk_view_int_kind(e->tex, &e->view, e->has_view);
    return (e->has_view && (e->view.setmask & 16) ? e->view.ds_mode : e->tex->ds_mode) == 1;
}
static uint32_t stencil_slots(int stage, uint32_t *sint) {
    uint32_t m = 0;
    *sint = 0;
    if (!st.tpool) return 0;
    static int nomemo = -1;
    if (nomemo < 0) nomemo = ENV("LARECOMP_VK_NO_SLOT_MEMO") != NULL;
    static struct { const NvnDescPool *pool; uint32_t tic, gen; int kind; } memo[4096];
    for (int s = 0; s < 32; s++) {
        uint64_t h = st.tex[stage][s];
        uint32_t tic = h & 0xFFFFF;
        if (!h || tic >= (uint32_t)st.tpool->count) continue;
        int k;
        __typeof__(memo[0]) *c = &memo[tic & 4095];
        if (!nomemo && c->pool == st.tpool && c->tic == tic && c->gen == exec_gen) k = c->kind;
        else {
            NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
            k = e->tex ? slot_kind(e) : 0;
            *c = (__typeof__(*c)){st.tpool, tic, exec_gen, k};
        }
        if (k == 1) m |= 1u << s;
        if (k == 2) *sint |= 1u << s;
    }
    return m;
}


// (investigation) the programs drawn with transform feedback on: their control sections to build/xfb_<vs code>.bin
static void xfb_note(void) {
    static uint64_t seen[64];
    static int nseen;
    if (!st.program || nseen >= 64) return;
    uint64_t vs = st.program->stages[0].code_addr;
    for (int i = 0; i < nseen; i++) if (seen[i] == vs) return;
    seen[nseen++] = vs;
    LOG("[xfb] program vs %llx fs %llx discard %d buffers %llx+%llx %llx+%llx %llx+%llx %llx+%llx\n", (unsigned long long)vs,
        (unsigned long long)st.program->stages[1].code_addr, raster_discard, (unsigned long long)xfb_addr[0],
        (unsigned long long)xfb_size[0], (unsigned long long)xfb_addr[1], (unsigned long long)xfb_size[1],
        (unsigned long long)xfb_addr[2], (unsigned long long)xfb_size[2], (unsigned long long)xfb_addr[3],
        (unsigned long long)xfb_size[3]);
    if (ENV("LARECOMP_DUMP_XFB") && st.program->stages[0].control) {
        char path[96];
        snprintf(path, sizeof path, "build/xfb_%llx.bin", (unsigned long long)vs);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(st.program->stages[0].control, 1, 0x800, f); fclose(f); }
    }
}

// The draw's state as a variant record (shader_prep.h; as renderer.m: both backends record the same lines)
static void rec_from_state(ShaderRec *r, bool flip_x, uint32_t mode) {
    memset(r, 0, sizeof *r);
    r->kind = 'G';
    r->vs = shader_stage_hash(&st.program->stages[0]);
    r->fs = shader_stage_hash(&st.program->stages[1]);
    r->flags = (nvn_window_origin() == 0 ? SR_FLIP_Y : 0) | (nvn_depth_mode() == 0 ? SR_DEPTH_MINUS_ONE : 0) | (flip_x ? SR_FLIP_X : 0);
    r->nattrib = (uint8_t)(st.nattrib < 16 ? st.nattrib : 16);
    memcpy(r->attrib, st.attrib, sizeof r->attrib[0] * r->nattrib);
    r->nstream = (uint8_t)(st.nstream < 16 ? st.nstream : 16);
    memcpy(r->stream, st.stream, sizeof r->stream[0] * r->nstream);
    r->ncolor = (uint8_t)(st.rt.ncolor < 8 ? st.rt.ncolor : 8);
    r->samples = 1;
    for (int i = 0; i < r->ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        if (!t) continue;
        r->color[i] = t->format;
        if (st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4)) r->cview[i] = st.rt.cview[i].format;
        if (t->samples > 1) r->samples = t->samples;
        r->channel_mask |= st.channel_mask & (0xFu << (i * 4));
        if (((st.color_state[0] >> i) & 1) && !rt_kind(t->format)) {
            r->blend_on |= (uint8_t)(1u << i);
            memcpy(r->blend[i], st.blend[i], 8);
        }
    }
    if (st.rt.depth) {
        r->depth = st.rt.depth->format;
        if (!r->ncolor && st.rt.depth->samples > 1) r->samples = st.rt.depth->samples;
    }
    r->a2c = st.ms[0xC] != 0;
    r->topo = shader_topo_class(mode);
    r->poly_fill = st.poly[2] != 1;
}

// LARECOMP_PREP_CHECK=1 (diagnostics): a pipeline the game builds although its shaders were prepared - logs the
// fields its key differs in from the closest prepared pipeline of the same shaders (and the draw's variant record)
static bool prep_check_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_PREP_CHECK") && atoi(getenv("LARECOMP_PREP_CHECK"));
    return v;
}

// Fields of two pipeline keys that differ: their number, and (out) "field: prepared / draw" for each
static int pk_diff(const PipeKey *a, const PipeKey *b, char *out, size_t n) {
    int d = 0;
    size_t o = 0;
    if (out && n) out[0] = 0;
#define PKD(cond, ...) do { if (cond) { d++; if (out && o < n) o += (size_t)snprintf(out + o, n - o, __VA_ARGS__); } } while (0)
    PKD(a->prog != b->prog, " program %p/%p (ready %d/%d);", (void *)a->prog, (void *)b->prog, a->prog ? (int)a->prog->state : -1,
        b->prog ? (int)b->prog->state : -1);
    PKD(a->ncolor != b->ncolor, " ncolor %u/%u;", a->ncolor, b->ncolor);
    for (int i = 0; i < 8; i++) PKD(a->color[i] != b->color[i], " color%d %d/%d;", i, a->color[i], b->color[i]);
    PKD(a->depth != b->depth, " depth %d/%d;", a->depth, b->depth);
    PKD(a->stencil != b->stencil, " stencil %d/%d;", a->stencil, b->stencil);
    PKD(a->samples != b->samples, " samples %u/%u;", a->samples, b->samples);
    PKD(a->attr_mask != b->attr_mask, " attr_mask %#x/%#x;", a->attr_mask, b->attr_mask);
    for (int i = 0; i < 32; i++) {
        PKD(a->attr_fmt[i] != b->attr_fmt[i], " attr%d fmt %u/%u;", i, a->attr_fmt[i], b->attr_fmt[i]);
        PKD(a->attr_off[i] != b->attr_off[i], " attr%d off %u/%u;", i, a->attr_off[i], b->attr_off[i]);
        PKD(a->attr_bind[i] != b->attr_bind[i], " attr%d bind %u/%u;", i, a->attr_bind[i], b->attr_bind[i]);
    }
    PKD(a->used_streams != b->used_streams, " streams %#x/%#x;", a->used_streams, b->used_streams);
    for (int s = 0; s < 17; s++) {
        PKD(a->stride[s] != b->stride[s], " stride%d %u/%u;", s, a->stride[s], b->stride[s]);
        PKD(a->divisor[s] != b->divisor[s], " divisor%d %u/%u;", s, a->divisor[s], b->divisor[s]);
    }
    for (int i = 0; i < 8; i++) {
        PKD(memcmp(a->blend[i], b->blend[i], 8), " blend%d differs;", i);
        PKD(a->write_mask[i] != b->write_mask[i], " write_mask%d %#x/%#x;", i, a->write_mask[i], b->write_mask[i]);
    }
    PKD(a->blend_on != b->blend_on, " blend_on %#x/%#x;", a->blend_on, b->blend_on);
    PKD(a->a2c != b->a2c, " a2c %u/%u;", a->a2c, b->a2c);
    PKD(a->topo_class != b->topo_class, " topo %u/%u;", a->topo_class, b->topo_class);
    PKD(a->poly_fill != b->poly_fill, " fill %u/%u;", a->poly_fill, b->poly_fill);
    if (!d && memcmp(a, b, sizeof *a)) {  // (no field differs: other bytes - padding)
        const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
        for (size_t i = 0; i < sizeof *a; i++) PKD(x[i] != y[i], " byte %zu %02x/%02x;", i, x[i], y[i]);
    }
#undef PKD
    return d;
}

static void prep_check(const PipeKey *k, uint64_t vh, uint64_t fh, const ShaderRec *rec) {
    char line[2048], diff[2048] = "";
    shader_rec_format(rec, line, sizeof line);
    int nprep = 0, best_d = 1 << 30, progs = 0;
    pthread_mutex_lock(&prep_lock);
    for (size_t i = 0; i < prep_progs_cap; i++) {
        Prog *q = prep_progs[i].obj;
        if (prep_progs[i].key && q && q->prep_vh == vh && q->prep_fh == fh) progs++;
    }
    const Pipe *best = NULL;
    for (size_t i = 0; i < prep_pipes_cap; i++) {
        const Pipe *p = prep_pipes[i].obj;
        if (!prep_pipes[i].key || !p || p->k.prog->prep_vh != vh || p->k.prog->prep_fh != fh) continue;
        nprep++;
        int d = pk_diff(&p->k, k, NULL, 0);
        if (d < best_d) best_d = d, best = p;
    }
    if (best) pk_diff(&best->k, k, diff, sizeof diff);
    pthread_mutex_unlock(&prep_lock);
    LOG("[prepcheck] %016llx %016llx: program %s (%d prepared with these shaders), %d prepared pipelines, closest "
        "differs (prepared/draw) in:%s\n[prepcheck]   draw: %s\n", (unsigned long long)vh, (unsigned long long)fh,
        k->prog->prep_vh ? "prepared" : "not prepared", progs, nprep, best ? diff : " (none)", line);
}

static void draw_(CmdRec *r, bool indexed);

// The draw's depth / stencil test and rasterizer state as Vulkan values (draw_ sets them as dynamic state, or they go
// into the pipeline key: FixedState)
static void fixed_state(FixedState *f, bool flip_x, bool flip_y, bool native, VkPrimitiveTopology topo) {
    memset(f, 0, sizeof *f);
    static int nodt = -1;
    if (nodt < 0) nodt = ENV("LARECOMP_NO_DEPTH_TEST") != NULL;
    f->dt = st.ds[0] && !nodt && pass_depth;
    f->dw = f->dt && st.ds[1];
    VkCompareOp dop = f->dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = ENV("LARECOMP_VK_EQUAL_TOL") != NULL;
    f->equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (f->equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    f->dop = (uint8_t)dop;
    f->sten = st.ds[3] && pass_stencil;
    uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
    for (int s = 0; s < 2; s++) {
        uint32_t o = s ? ops >> 12 : ops;
        if (!f->sten) { f->sop[s][0] = f->sop[s][1] = f->sop[s][2] = VK_STENCIL_OP_KEEP; f->sop[s][3] = VK_COMPARE_OP_ALWAYS; continue; }
        f->sop[s][0] = (uint8_t)stencil_op(o & 0xF), f->sop[s][1] = (uint8_t)stencil_op((o >> 8) & 0xF);
        f->sop[s][2] = (uint8_t)stencil_op((o >> 4) & 0xF), f->sop[s][3] = (uint8_t)compare_fn(s ? st.ds[4] >> 4 : st.ds[4] & 0xF);
    }
    uint8_t cull = flip_x ? 0 : st.poly[0];  // (mirror mode's composite: one full-screen quad, either winding)
    f->cull = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = ENV("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = ((st.poly[1] == 1) != flip_y) != (flipw != 0);
    f->ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    f->topo = (uint8_t)(native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
}

// A draw of a handful of vertices - a full-screen pass: post-processing, a menu's backdrop, the composite into the
// window image - whose shader or pipeline is still being built is waited for (a few ms, once) rather than skipped:
// skipping one of those left the whole frame black (Pokémon Z-A: a black flash as a menu opened or closed). Not an
// effect's (blended() / scene_quad(), below): it waits as a scene draw.
// Scene draws wait a little too: a pipeline already in the caches (cache/vk_pipelines.bin, the driver's, the SPIR-V
// one) is built in 1-2 ms, but skipping the draw took the object off the screen for a whole frame - every time a
// scene came into view, and at different places each run, as the builds' timing varies (trees, buildings, tables
// blinking or going black for a frame in a recorded play). Up to 4 ms a draw, 6 ms a frame
// (LARECOMP_VK_SCENE_WAIT_MS, 0: skip at once); past that it's skipped as before - shaders never built before (a
// moment's pop-in the first time instead of a hitch).
// One that isn't built within 2 s isn't waited for again (it was 2 s per frame: a black screen at 0.5 fps on a
// player's PC as a battle began), and the log says what the shader threads were doing. A full-screen draw runs its
// own build if it's still queued (run_queued_now): it waits for that build, not for every one queued before it.
static bool wait_scene(_Atomic int *state) {
    if (atomic_load(state) == 0) queued_job(state, false);  // (its build next: skipped for a frame or two, not until
                                                             // the whole queue is done; 2: failed, nothing queued)
    static double budget_ms = -1;
    if (budget_ms < 0) budget_ms = ENV("LARECOMP_VK_SCENE_WAIT_MS") ? atof(ENV("LARECOMP_VK_SCENE_WAIT_MS")) : 6;
    static uint64_t frame = ~0ull, spent_ns;
    uint64_t f = renderer_present_count();
    if (f != frame) frame = f, spent_ns = 0;
    uint64_t left = (uint64_t)(budget_ms * 1e6) > spent_ns ? (uint64_t)(budget_ms * 1e6) - spent_ns : 0;
    if (left > 4000000ull) left = 4000000ull;
    if (!left) return false;
    uint64_t t0 = SDL_GetTicksNS(), now = t0;
    while (atomic_load(state) == 0 && (now = SDL_GetTicksNS()) - t0 < left) SDL_DelayPrecise(100000);
    spent_ns += now - t0;
    stat_scene_wait_ns += now - t0;
    return atomic_load(state) == 1;
}
// Not a full-screen pass, though a handful of vertices: Z-A's move effects are such quads; waited for like a full-screen
// pass, each new one stopped the frame for its build, one after another (a battle: 200-1200 ms frames, the other
// builds' threads idle). Skipped, an effect is missing for a frame, never a black frame:
// - blended into every colour target: drawn over what is there (not with ONE / ZERO: that blend is a plain write);
// - a quad (4-6 vertices) tested against the scene's depth: in the world, behind what is in front of it (in a recorded
//   battle most of the wait: sparks, decals, masks; the full-screen ones are triangles of 3, or not depth tested).
static bool scene_quad(const CmdRec *r) {
    return r->x[2] > 3 && st.ds[0] && st.rt.depth && compare_fn(st.ds[2]) != VK_COMPARE_OP_ALWAYS;
}
static bool blended(void) {
    int n = 0;
    for (int i = 0; i < st.rt.ncolor; i++) {
        if (!st.rt.color[i]) continue;
        if (!((st.color_state[0] >> i) & 1) || rt_kind(st.rt.color[i]->format) ||
            blend_factor(st.blend[i][2]) == VK_BLEND_FACTOR_ZERO) return false;
        n++;
    }
    return n > 0;
}
static bool wait_built(_Atomic int *state, const CmdRec *r, bool indexed, bool pipe) {
    if (atomic_load(state) == 1) return true;
    static int wait_all = -1;  // LARECOMP_VK_WAIT_ALL_SMALL=1: every draw of up to 6 vertices waits, as before
    if (wait_all < 0) wait_all = ENV("LARECOMP_VK_WAIT_ALL_SMALL") != NULL;
    if (r->x[2] > 6 || (!wait_all && (blended() || scene_quad(r)))) return wait_scene(state);
    static _Atomic int *gave_up[64];
    static int ngave_up;
    for (int i = 0; i < ngave_up; i++) if (gave_up[i] == state) return false;
    uint64_t t0 = SDL_GetTicksNS();
    while (atomic_load(state) == 0 && SDL_GetTicksNS() - t0 < 2000000000ull)
        if (!run_queued_now(state)) SDL_Delay(1);  // (a program: its translation, then its compile)
    uint64_t now = SDL_GetTicksNS();
    static int n;  // (from 5 ms: the first 20 of any length were all at startup, under 2 ms)
    if (now - t0 >= 5000000ull && n++ < 60) {  // (what the draw is: which draws are worth a wait)
        const NvnTexture *t = st.rt.ncolor ? st.rt.color[0] : NULL;
        LOG("[vk] full-screen draw waited %.1f ms for its %s (%llu vertices x%u, blend %#x, depth test %d, %d targets "
            "%ux%u fmt %#x, viewport %.0fx%.0f, vs %llx fs %llx)\n", (now - t0) / 1e6, pipe ? "pipeline" : "shaders",
            (unsigned long long)r->x[2], (uint32_t)r->x[indexed ? 6 : 4], st.color_state[0], st.ds[0] && st.rt.depth,
            st.rt.ncolor, t ? t->width : 0, t ? t->height : 0, t ? t->format : 0, st.viewport[0][2], st.viewport[0][3],
            (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr);
    }
    if (atomic_load(state) == 0) {
        if (ngave_up < 64) gave_up[ngave_up++] = state;
        uint64_t tt = atomic_load(&translator_t0), longest = 0;
        int busy = 0;
        for (int i = 0; i < nworkers; i++) {
            uint64_t w = atomic_load(&worker_t0[i]);
            if (w) busy++, longest = now - w > longest ? now - w : longest;
        }
        LOG("[vk] full-screen draw (vs %llx fs %llx): its shader / pipeline isn't built after 2 s - not waited for again; "
            "translator %s %.1f s, %d of %d compile threads busy (longest %.1f s)\n",
            st.program ? (unsigned long long)st.program->stages[0].code_addr : 0ull,
            st.program ? (unsigned long long)st.program->stages[1].code_addr : 0ull, tt ? "busy for" : "idle",
            tt ? (now - tt) / 1e9 : 0.0, busy, nworkers, longest / 1e9);
    }
    return atomic_load(state) == 1;
}
static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    if (xfb_on) {
        xfb_note();
        // the transform feedback buffers are GPU-written: never served from the vertex cache's CPU-side copies (later
        // draws read them as vertex streams - Z-A's characters take their per-instance data from them; a stale copy
        // froze their animation and stretched their meshes)
        for (int b = 0; b < 4; b++)
            if (xfb_addr[b] >= gwin_base && xfb_size[b] && xfb_addr[b] - gwin_base < gwin_max)
                vc_mark_gpu(xfb_addr[b] - gwin_base, xfb_addr[b] - gwin_base + xfb_size[b]);
        xm_draw = xm_prepare();
        static int nlog;
        if (nlog++ < 24)
            LOG("[xfb] draw indexed %d mode %llu first %llx count %llu | buffers %llx+%llx %llx+%llx %llx+%llx\n", indexed,
                (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2], (unsigned long long)xfb_addr[0],
                (unsigned long long)xfb_size[0], (unsigned long long)xfb_addr[1], (unsigned long long)xfb_size[1],
                (unsigned long long)xfb_addr[2], (unsigned long long)xfb_size[2]);
    }
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
static uint64_t prof_tp_;
static void draw_(CmdRec *r, bool indexed) {
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { SKIP(SK_NO_PROGRAM); return; }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    vkey.stencil_slots = stencil_slots(0, &vkey.sint_slots);
    fkey.stencil_slots = stencil_slots(1, &fkey.sint_slots);
    if (xfb_on && st.program->stages[0].control) {  // the program's transform feedback layout (its control section)
        const uint8_t *ctl = st.program->stages[0].control;
        for (int b = 0; b < 4; b++) {
            vkey.xfb_count[b] = (uint8_t)*(const uint32_t *)(ctl + 0x4DC + 0x80 * b);
            vkey.xfb_stride[b] = *(const uint16_t *)(ctl + 0x6DC + 2 * b);
        }
    }
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (!pr || !wait_built(&pr->state, r, indexed, false)) {
        SKIP(!pr ? SK_NO_STAGES : atomic_load(&pr->state) == 2 ? SK_PROG_FAILED : SK_PROG_BUILDING);
        return;
    }
    {   // Stretched-geometry guard: a vertex shader reading attributes past a one-attribute layout (position only) is
        // drawing with another draw's layout - its mesh spreads across the sky (Z-A: 36-byte vertices read as 12).
        // Skipped until the cause is found; LARECOMP_SKIP_BAD_LAYOUT=0 draws them.
        static int on = -1, fault = -1;
        if (on < 0) on = !ENV("LARECOMP_SKIP_BAD_LAYOUT") || atoi(ENV("LARECOMP_SKIP_BAD_LAYOUT")) != 0;
        // (test) LARECOMP_TEST_BAD_LAYOUT=<n>: every n-th draw of a mesh with interleaved vertices (stride >= 24) gets
        // the bug's layout (position only, stride 12) - the stretching on demand, to test this guard
        if (fault < 0) fault = ENV("LARECOMP_TEST_BAD_LAYOUT") ? atoi(ENV("LARECOMP_TEST_BAD_LAYOUT")) : 0;
        static uint64_t nfault;
        if (fault > 0 && st.nattrib > 1 && st.nstream && st.stream[0][0] >= 24 && (pr->vi->attr_mask & ~1u) &&
            (st.attrib[0] & 0xFF) == 0x22 && (st.attrib[0] >> 8 & 0x1F) == 0 && ++nfault % (uint64_t)fault == 0) {
            st.nattrib = 1;
            st.stream[0][0] = 12;
        }
        if (on && st.nattrib <= 1 && (pr->vi->attr_mask & ~1u)) {
            static uint64_t nskip;
            if (nskip++ < 20) LOG("[vtxstate] skipped draw: vs %llx reads attribs %#x, %d bound (vb0 %llx)\n",
                                  (unsigned long long)st.program->stages[0].code_addr, pr->vi->attr_mask, st.nattrib,
                                  (unsigned long long)st.vbuf[0].addr);
            SKIP(SK_HIDDEN);
            return;
        }
    }
    g_mirror_w = 0;
    {   // mirror mode (runtime/hle/mirror.c; as renderer.m): the scene's composite into the window image is mirrored
        extern bool mirror_draw(bool composite);
        extern bool mirror_scene_size(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h);
        bool composite = false;
        if (st.rt.ncolor && st.rt.color[0] && renderer_texture_presented(st.rt.color[0]) && !(st.color_state[0] & 1) &&
            r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
            st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height && pr->fi->ntex) {
            NvnTexture *t0 = shader_texture_object(1, &pr->fi->tex[0]);
            composite = t0 && mirror_scene_size(t0->width, t0->height, st.rt.color[0]->width, st.rt.color[0]->height);
            extern void scene_size_log(bool composite, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h);
            if (t0) scene_size_log(composite, t0->width, t0->height, st.rt.color[0]->width, st.rt.color[0]->height);
        }
        if (mirror_draw(composite)) {
            vkey.flip_x = fkey.flip_x = true;
            pr = get_program(st.program, &vkey, &fkey);
            if (!pr || atomic_load(&pr->state) != 1) { SKIP(!pr ? SK_NO_STAGES : atomic_load(&pr->state) == 2 ? SK_PROG_FAILED : SK_PROG_BUILDING); return; }
            g_mirror_w = (float)st.rt.color[0]->width;
        }
    }
    snap_apply(0, pr->vi->ssbo_read);  // (overlap) the storage slots the shaders read: a copy where the CPU rewrites the range
    snap_apply(1, pr->fi->ssbo_read);
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = ENV("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { SKIP(SK_DEBUG_SKIP); return; }
        extern bool pot_level_active(void);
        if (last_hash == 0xa2177dbb72197bd9ull && pot_level_active()) { SKIP(SK_HIDDEN); return; }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
        extern void widescreen_backdrop_seen(uint64_t hash);
        if (last_hash) widescreen_backdrop_seen(last_hash);
        extern void widescreen_scene_ubo(const float *u, uint64_t size);  // (the camera's view-projection)
        if (st.ubuf[0][5].addr) widescreen_scene_ubo((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size);
    }
    if (g_debug_frame) debug_draw(pr, r, indexed);
    if (ct_on()) ct_draw(pr, "draw");
    alias_draw_check();
    resolve_feedback(pr);
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    begin_pass();
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { SKIP(SK_TOPOLOGY); return; }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    uint32_t rt_fmt[8] = {0};
    for (int i = 0; i < st.rt.ncolor && i < 8; i++) rt_fmt[i] = st.rt.color[i] ? st.rt.color[i]->format : 0;
    const PipeIn in = {st.attrib, st.nattrib, (const uint32_t (*)[2])st.stream, st.nstream, rt_fmt, st.channel_mask,
                       st.color_state[0], (const uint8_t (*)[8])st.blend, st.ms[0xC] != 0, cls, st.poly[2] != 1};
    // the key's parts draw_ adds to pipe_key_fill's: the depth clamp (in depth-only passes - shadow maps: the casters past
    // the light's planes - and in passes without depth - the UI: its quads ask for it, and some lie outside the depth
    // range, the item-count window after using a candy stayed hidden; not in colour passes with depth: Vulkan's clamp
    // also turns off near clipping, and a big mesh with triangles behind the camera spread across the screen as huge
    // dark sheets; but in colour passes onto a square depth target - Pokémon Z-A's sun shadow maps, 1024x1024 x 3
    // cascades drawn with an 8-bit colour target beside: unclamped, the buildings past the nearest cascade's light planes
    // were clipped from it and their shadows cut off at a line that moved with the player, #43), primitive restart and
    // the depth / stencil state in the pipeline on Vulkan 1.2 drivers
    static int noclamp = -1, clamp_color = -1;  // LARECOMP_VK_NO_DEPTH_CLAMP=1: ignored; LARECOMP_VK_CLAMP_COLOR=1: in every pass
    if (noclamp < 0) noclamp = ENV("LARECOMP_VK_NO_DEPTH_CLAMP") != NULL, clamp_color = ENV("LARECOMP_VK_CLAMP_COLOR") != NULL;
    bool square_depth = st.rt.depth && st.rt.depth->width == st.rt.depth->height;
    uint8_t dclamp = st.depth_clamp && !noclamp && (!pass_ncolor || !pass_depth || square_depth || clamp_color);
    uint8_t restart = g_vkc.no_eds2 && st.prim_restart && native && (mode == 3 || mode == 5);  // (as SS_PRE below)
    FixedState fx;
    fixed_state(&fx, vkey.flip_x, vkey.flip_y, native, topo);
    bool memo_on = key_memo_on() && !g_vkc.no_eds1;  // (no extended dynamic state: the whole fixed state is in the key)
    if (memo_on && !pr->pmemo) pr->pmemo = calloc(1, sizeof *pr->pmemo);
    struct PipeMemo *pm = memo_on ? pr->pmemo : NULL;
    int nat = st.nattrib < 0 ? 0 : st.nattrib > 16 ? 16 : st.nattrib, nst = st.nstream < 0 ? 0 : st.nstream > 16 ? 16 : st.nstream;
    uint32_t npc = pass_ncolor < 8 ? pass_ncolor : 8;
    prof_key_draws++;
    PipeKey k;
    VtxLayout vl;  // the draw's vertex layout (on a memo hit: the memo's, made from the same inputs)
    Pipe *pp;
    if (pm && pm->pp && pm->gen == key_gen && pm->ncolor == pass_ncolor && pm->samples == pass_samples && pm->depth == pass_depth &&
        pm->stencil == pass_stencil && pm->nattrib == st.nattrib && pm->nstream == st.nstream && pm->channel_mask == in.channel_mask &&
        pm->blend_on == in.blend_on && pm->a2c == in.a2c && pm->cls == in.topo_class && pm->poly_fill == in.poly_fill &&
        pm->depth_clamp == dclamp && pm->restart == restart &&
        !memcmp(pm->color, pass_color, npc * sizeof pass_color[0]) && !memcmp(pm->rt_fmt, rt_fmt, sizeof rt_fmt) &&
        !memcmp(pm->attrib, st.attrib, (size_t)nat * 4) && !memcmp(pm->stream, st.stream, (size_t)nst * 8) &&
        !memcmp(pm->blend, st.blend, sizeof pm->blend)) {
        prof_pipe_memo++;
        pp = pm->pp;
        vl = pm->vl;  // (the key leaves the layout out with dynamic vertex input: kept with the memo)
    } else {
        pipe_key_fill(&k, pr, &in, pass_color, pass_ncolor, pass_depth, pass_stencil, pass_samples, &vl);
        k.depth_clamp = dclamp, k.restart = restart;
        if (g_vkc.no_eds1) k.fx = fx;
        uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
        if (!ph) ph = 1;
        KvEnt *pslot = kv_find(&pipe_kv, ph);
        pp = pslot->val;
        if (!pp) {
            if (shader_rec_recording()) {
                ShaderRec rec;
                rec_from_state(&rec, vkey.flip_x, mode);
                shader_rec_note(&rec);
                shader_seen_code(rec.vs, &st.program->stages[0]);  // (their code for the next start's preparation)
                shader_seen_code(rec.fs, &st.program->stages[1]);
            }
            pp = prep_pipes_n ? PREP_GET(prep_pipes, ph) : NULL;  // prepared before the game
            if (pp && memcmp(&pp->k, &k, sizeof k)) pp = NULL;
            if (!pp) {
                uint64_t vh = shader_stage_hash(&st.program->stages[0]), fh = shader_stage_hash(&st.program->stages[1]);
                shader_first_visit("pipeline", vh, fh, ph);
                if (prep_check_on()) {
                    ShaderRec rec;
                    rec_from_state(&rec, vkey.flip_x, mode);
                    prep_check(&k, vh, fh, &rec);
                }
                pp = calloc(1, sizeof *pp);
                pp->k = k;
                run_job(build_pipeline, pp, &pp->state);
            }
            kv_set(&pipe_kv, pslot, ph, pp);
        }
        if (pm) {
            pm->gen = key_gen; pm->ncolor = pass_ncolor; pm->samples = pass_samples; pm->depth = pass_depth; pm->stencil = pass_stencil;
            pm->nattrib = st.nattrib; pm->nstream = st.nstream; pm->channel_mask = in.channel_mask; pm->blend_on = in.blend_on;
            pm->a2c = in.a2c; pm->cls = in.topo_class; pm->poly_fill = in.poly_fill;
            pm->depth_clamp = dclamp; pm->restart = restart;
            memcpy(pm->color, pass_color, sizeof pm->color); memcpy(pm->rt_fmt, rt_fmt, sizeof pm->rt_fmt);
            memcpy(pm->attrib, st.attrib, sizeof pm->attrib); memcpy(pm->stream, st.stream, sizeof pm->stream);
            memcpy(pm->blend, st.blend, sizeof pm->blend);
            pm->pp = pp;
            pm->vl = vl;
        }
    }
    {   // a pipeline still building: the draw is skipped (as on Metal) - unless the recorder waits for it at the bind
        // (LARECOMP_VK_RECORDER_WAIT_PIPES=1, record.c: pipe_resolve), or it is about to be ready (wait_built: one in
        // the driver's cache, up to 4 ms); one that failed is always skipped
        int ps = atomic_load(&pp->state);
        if (ps == 2) { SKIP(SK_PIPE_FAILED); return; }
        if (ps != 1 && !rec_wait_pipes() && !wait_built(&pp->state, r, indexed, true)) { SKIP(SK_PIPE_BUILDING); return; }
    }
    ss_enabled();
    if (ss_need(SS_PIPE, ss.pipe == pp)) { ss.pipe = pp; rec_CmdBindPipelineObj(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp); }
    if (g_vk.vids && ss_need(SS_VI, !memcmp(&ss.vi, &vl, sizeof vl))) { ss.vi = vl; set_vertex_input(cb, &vl); }
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic; without extended dynamic state, the parts in the pipeline: fixed_state)
    VkBool32 dt = fx.dt, dw = fx.dw;
    if (ss_need(SS_DT, ss.dt == dt)) { ss.dt = dt; rec_CmdSetDepthTestEnable(cb, dt); }
    if (ss_need(SS_DW, ss.dw == dw)) { ss.dw = dw; rec_CmdSetDepthWriteEnable(cb, dw); }
    VkCompareOp dop = (VkCompareOp)fx.dop;
    bool equal_tol = fx.equal_tol;
    if (ss_need(SS_DOP, ss.dop == dop)) { ss.dop = dop; rec_CmdSetDepthCompareOp(cb, dop); }
    bool sten = fx.sten;
    if (ss_need(SS_STEN, ss.sten == (VkBool32)sten)) { ss.sten = sten; rec_CmdSetStencilTestEnable(cb, sten); }
    {  // (stencil off too: the pipelines take the op as dynamic state, so each command buffer must set it before its first draw)
        for (int f = 0; f < 2; f++) {
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            uint32_t v[7] = {fx.sop[f][0], fx.sop[f][1], fx.sop[f][2], fx.sop[f][3], st.stencil_vmask[f] & 0xFF,
                             st.stencil_mask[f] & 0xFF, st.stencil_ref[f]};
            if (!sten) memcpy(v, (uint32_t[7]){VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0}, sizeof v);
            if (!ss_need(f ? SS_SFACE1 : SS_SFACE0, !memcmp(v, ss.sface[f], sizeof v))) continue;
            memcpy(ss.sface[f], v, sizeof v);
            rec_CmdSetStencilOp(cb, face, (VkStencilOp)v[0], (VkStencilOp)v[1], (VkStencilOp)v[2], (VkCompareOp)v[3]);
            rec_CmdSetStencilCompareMask(cb, face, v[4]);
            rec_CmdSetStencilWriteMask(cb, face, v[5]);
            rec_CmdSetStencilReference(cb, face, v[6]);
        }
    }
    if (ss_need(SS_BC, !memcmp(ss.bc, st.blend_color, sizeof ss.bc))) { memcpy(ss.bc, st.blend_color, sizeof ss.bc); rec_CmdSetBlendConstants(cb, st.blend_color); }

    // ---- rasterizer
    VkCullModeFlags cm = fx.cull;
    if (ss_need(SS_CULL, ss.cull == cm)) { ss.cull = cm; rec_CmdSetCullMode(cb, cm); }
    VkFrontFace ff = (VkFrontFace)fx.ff;
    if (ss_need(SS_FF, ss.ff == ff)) { ss.ff = ff; rec_CmdSetFrontFace(cb, ff); }
    VkPrimitiveTopology tp = (VkPrimitiveTopology)fx.topo;
    if (ss_need(SS_TOPO, ss.topo == tp)) { ss.topo = tp; rec_CmdSetPrimitiveTopology(cb, tp); }
    VkBool32 pre = st.prim_restart && native && (mode == 3 || mode == 5);
    if (ss_need(SS_PRE, ss.pre == pre)) { ss.pre = pre; rec_CmdSetPrimitiveRestartEnable(cb, pre); }
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = ENV("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        if (ss_need(SS_DBE, ss.dbe == (VkBool32)on)) { ss.dbe = on; rec_CmdSetDepthBiasEnable(cb, on); }
        if (ss_need(SS_DB, !memcmp(ss.db, f, sizeof f))) { memcpy(ss.db, f, sizeof f); rec_CmdSetDepthBias(cb, f[1], f[2], f[0]); }
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    // The hooks below look for known textures (texpatch ids, TexBackend.tp > 0) among the fragment textures: one pass
    // finds the texture objects and whether any is known; the loops only run when one is (most draws have none).
    // LARECOMP_VK_NO_HOOK_PASS=1: every loop looks its textures up, as before.
    static int hook_pass = -1;
    if (hook_pass < 0) hook_pass = getenv("LARECOMP_VK_NO_HOOK_PASS") == NULL;
    NvnTexture *ftex[64];
    bool fknown = true;
    if (hook_pass) {
        fknown = false;
        for (int i = 0; i < pr->fi->ntex; i++) {
            ftex[i] = shader_texture_object(1, &pr->fi->tex[i]);
            const TexBackend *b = ftex[i] ? (const TexBackend *)ftex[i]->backend : NULL;
            if (b && b->tp > 0) fknown = true;
        }
        prof_hook_draws++;
        if (fknown) prof_hook_known++;
    }
#define FTEX(i) (hook_pass ? ftex[i] : shader_texture_object(1, &pr->fi->tex[i]))
    {   // the Items page's cursor brackets while the cursor is on the companion card (runtime/hle/pets.c)
        extern bool pets_hide_item_cursor(void);
        extern bool texpatch_is_item_cursor(int id);
        if (pets_hide_item_cursor() && fknown)
            for (int i = 0; i < pr->fi->ntex; i++) {
                NvnTexture *t = FTEX(i);
                const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
                if (b && b->tp > 0 && texpatch_is_item_cursor(b->tp - 1)) { SKIP(SK_HIDDEN); return; }
            }
    }
    float tint[RECOLOUR_TINT_FLOATS] = {0};
    int recol[2] = {0, 0};  // Cosmetics recolour textures' classes (runtime/nvn/recolour.c)
    if (pr->fi->uses_tint) {
        for (int i = 0; i < pr->fi->ntex && fknown; i++) {
            NvnTexture *t = FTEX(i);
            const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
            if (b && b->tp > 0) recolour_add(recol, recolour_class_of(b->tp - 1));
        }
        recolour_tint_draw(recol, tint);  // (the highlights below win)
        extern bool pot_tint(const float *m, float tint[8], uint64_t mesh);
        extern bool pot_glow_active(void);
        // (uniform buffer 2 = the model matrix, hardware cbuf 5 - only when the vertex shader reads it: a binding left
        // from an earlier draw is another object's matrix)
        if (((pr->vi->cbuf_mask >> 5) & 1) && st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && pot_glow_active())
            pot_tint((const float *)(uintptr_t)st.ubuf[0][2].addr, tint, st.vbuf[0].addr);
        // the gold Escape Bottle (runtime/hle/escape.c): draws sampling the bottle's item icon / model map
        // and the HUD hearts (heart colour, Cosmetics tab)
        extern int texpatch_bottle_kind(int id);
        extern int texpatch_heart_kind(int id);
        extern void texpatch_heart_tint(int kind, float tint[8]);
        int bottle = 0, heart = 0;
        for (int i = 0; i < pr->fi->ntex && fknown; i++) {
            NvnTexture *t = FTEX(i);
            const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
            if (b && b->tp > 0 && !bottle) bottle = texpatch_bottle_kind(b->tp - 1);
            if (b && b->tp > 0 && !heart) heart = texpatch_heart_kind(b->tp - 1);
        }
        if (heart) texpatch_heart_tint(heart, tint);
        if (bottle) {
            extern void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]);
            int u = bottle == 3 || bottle == 6 ? 3 : 0;  // (fairy / Piece of Power models: their bone matrices - souls.c, triforce.c)
            escape_bottle_tint(bottle, (const float *)(uintptr_t)st.ubuf[0][u].addr, (uint32_t)st.ubuf[0][u].size, tint);
        }
    }
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
    uint32_t dyn[2 * MAX_BIND], ndyn = 0;  // (set reuse: the sets' dynamic offsets, vertex then fragment)
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL, dyn, &ndyn);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2], fr[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh, fr);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
        tex_fr[1][i][0] = fr[0]; tex_fr[1][i][1] = fr[1];
        // the Hint Sheet (runtime/hle/hint_sheet.c): the dungeon map's icon in place of the first bottle's
        // (tp read after texture_info: its upload check may have identified the texture just now)
        NvnTexture *t = FTEX(i);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        extern int texpatch_bottle_kind(int id);
        if (b && b->tp > 0 && texpatch_bottle_kind(b->tp - 1) == 1) {
            extern bool hint_sheet_icon_draw(const float *ui, uint32_t ui_size);
            extern NvnTexture *texpatch_hint_icon(void);
            NvnTexture *ht;
            if (hint_sheet_icon_draw((const float *)(uintptr_t)st.ubuf[0][0].addr, (uint32_t)st.ubuf[0][0].size) &&
                (ht = texpatch_hint_icon())) {
                static NvnTextureView none;
                fii[i].imageView = vk_view(ht, &none, false, false, pr->fi->tex[i].type);
            }
        }
    }
#undef FTEX
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        extern bool widescreen_ui_pane_partial(const float *u, uint64_t size);  // (a smaller layout pane: boxed UI)
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad &&
                               !widescreen_ui_pane_partial((const float *)(uintptr_t)st.ubuf[0][0].addr, st.ubuf[0][0].size);
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        extern uint64_t renderer_exec_frame(void);  // (the frame being translated)
        if (reads_scene) wide_frame = renderer_exec_frame();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_exec_frame());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // camera-attached effects sized for 16:9 are widened with the view (runtime/hle/widescreen.c)
        extern float widescreen_effect_stretch(uint64_t fs_hash);
        float k = widescreen_effect_stretch(g_cur_fs_hash);
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii, dyn, &ndyn);
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, ndyn, ndyn ? dyn : NULL);
    if (gc_shift && (pr->vi->uses_gmem || pr->fi->uses_gmem))
        gmem_chunk_bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, GMEM_CHUNK_SET(STAGE_FRAGMENT),
                        (pr->vi->uses_gmem ? 1u << STAGE_VERTEX : 0) | (pr->fi->uses_gmem ? 1u << STAGE_FRAGMENT : 0));
    if (pr->vi->uses_gmem_store) vc_mark_sbufs(0);
    if (pr->fi->uses_gmem_store) vc_mark_sbufs(1);

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    bool ind_ok = !indexed && native;  // (g_ind_gpu: the parameters can be the GPU's - first index etc. as in Vulkan)
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            ind_ok = true;
            if (iaddr % isz == 0 && guest_direct(1, iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
                else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index && ss_need(SS_IB, ss.ib == ib.buf && ss.io == ib.off && ss.it == itype_vk)) {
        ss.ib = ib.buf; ss.io = ib.off; ss.it = itype_vk;
        rec_CmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(vl.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && guest_direct(0, v.addr, v.size, &dbuf, &doff)) {
                if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == dbuf && ss.vo[s] == doff) continue;
                ss.vb[s] = dbuf; ss.vo[s] = doff; ss.vb_known |= 1u << s;
                rec_CmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) { const uint16_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != (uint16_t)scan_restart) max_index = q[i]; }
                else { const uint32_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != scan_restart) max_index = q[i]; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = vl.stride[s], need;
            if (!stride) need = 256;
            else if (vl.divisor[s] && g_ind_gpu) need = v.size ? v.size : 1u << 20;  // (instances only the GPU knows: all)
            else if (vl.divisor[s]) need = ((uint64_t)base_inst + inst + vl.divisor[s] - 1) / vl.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == off) continue;
        ss.vb[s] = sp.buf; ss.vo[s] = off; ss.vb_known |= 1u << s;
        rec_CmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    // Instanced draws with item spots among their instances (bushsanity: bushes are drawn in batches, each instance's
    // model matrix in vertex storage buffer 0, indexed by instance id): drawn as runs of instances, gold runs tinted
    // (as renderer.m; gl_InstanceIndex includes the base instance, so a run [b + k, b + k + n) draws exactly those).
    // Only for a vertex shader that reads storage buffer 0 (its binding stays from the last draw that set it).
    enum { MAX_RUNS = 64 };
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];
    bool run_gold[MAX_RUNS];
    int nruns = 0;
    if (pr->fi->uses_tint && (pr->vi->ssbo_read & 1) && st.sbuf[0][0].addr) {
        extern bool pot_glow_active(void);
        extern bool pot_instance_gold(const float *m);
        uint64_t recs = st.sbuf[0][0].size / 64;
        if (pot_glow_active() && (uint64_t)base_inst + inst <= recs) {
            const float *m = (const float *)(uintptr_t)st.sbuf[0][0].addr;
            for (uint32_t i = 0; i < inst; i++) {
                bool g = pot_instance_gold(m + (size_t)(base_inst + i) * 16);
                if (nruns && run_gold[nruns - 1] == g) { run_n[nruns - 1]++; continue; }
                if (nruns == MAX_RUNS) { nruns = 0; break; }  // (too fragmented: draw as is)
                run_at[nruns] = i; run_n[nruns] = 1; run_gold[nruns] = g; nruns++;
            }
            if (nruns == 1 && !run_gold[0]) nruns = 0;
        }
    }
    if (!nruns) { run_at[0] = 0; run_n[0] = inst; nruns = 1; run_gold[0] = false; }
    for (int ri = 0; ri < nruns; ri++) {
        if (nruns > 1 || run_gold[0]) {  // this run's tint: a fragment set of its own
            float rtint[RECOLOUR_TINT_FLOATS] = {0};
            extern void pot_gold_tint(float tint[8]);
            if (run_gold[ri]) pot_gold_tint(rtint);
            else recolour_tint_draw(recol, rtint);
            uint32_t rdyn[MAX_BIND], nrdyn = 0;
            VkDescriptorSet rset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, rtint, 0, 0, 0, fii, rdyn, &nrdyn);
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 1, 1, &rset, nrdyn, nrdyn ? rdyn : NULL);
        }
        if (raster_discard) {  // (rasterizer discard: the vertex stage runs - transform feedback - and nothing is drawn)
            VkRect2D none = {{0, 0}, {0, 0}};
            rec_CmdSetScissor(cb, 0, 1, &none);
            ss.known &= ~SS_SC;
        }
        if (g_ind_gpu && ind_ok && nruns == 1) {  // (indirect: instance count etc. written by the GPU - culling)
            rec_CmdDrawIndirect(cb, gmem_buf, g_ind_off, 1, g_ind_stride, indexed);
        } else if (!use_index) rec_CmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);
        else rec_CmdDrawIndexed(cb, icount, run_n[ri], 0, base_vertex, base_inst + run_at[ri]);
    }
    dl_note("draw vs %llx fs %llx%s xfb %d", (unsigned long long)st.program->stages[0].code_addr,
            (unsigned long long)st.program->stages[1].code_addr, g_ind_gpu && ind_ok ? " indirect" : "", xfb_on);
    if (g_ind_gpu && ind_ok && dl_on > 0) {
        dl_args[(dl_n - 1) & (DL_N - 1)] = gwin_base + gmem_lo + g_ind_off;
        memcpy(dl_snap[(dl_n - 1) & (DL_N - 1)], (const void *)(uintptr_t)(gwin_base + gmem_lo + g_ind_off), 20);
    }
    if (dl_on > 0) vk_checkpoint(cb, dl_n);
    stat_draws++;
    cum_draws++;
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x offset %x %g %g %g\n",
        st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0],
        st.ds[1], st.ds[2], st.poly[0], st.poly[1], st.channel_mask, st.poly[3], st.poly_offset[0], st.poly_offset[1],
        st.poly_offset[2]);
    // the vertex layout - stretched geometry is a mesh read with another layout: attribute <i>:f<format>/s<stream>+<offset>
    // (those the shader reads), stream <s>:<address>+<size>/<stride>
    LOG("[draw]   layout %d attribs:", st.nattrib);
    for (int i = 0; i < st.nattrib && i < 16; i++)
        if (pr->vi->attr_mask >> i & 1) LOG(" %d:f%02x/s%u+%u", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
    LOG(" | streams");
    for (int s = 0; s < st.nstream && s < 16; s++)
        if (st.vbuf[s].addr) LOG(" %d:%llx+%llx/%u", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size, st.stream[s][0]);
    LOG("\n");
    if (ENV("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {
            if (!((pr->fi->cbuf_mask >> c) & 1) || !st.ubuf[1][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c].addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)st.ubuf[1][c].addr, (unsigned)st.ubuf[1][c].size);
            for (int k = 0; k < 96 && k * 4 < (int)st.ubuf[1][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {  // vertex cbufs (bone palettes...) and storage buffer 0
            if (!((pr->vi->cbuf_mask >> c) & 1) || c < 3 || !st.ubuf[0][c - 3].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[0][c - 3].addr;
            LOG("[draw]   vcb%d %llx size %u:", c, (unsigned long long)st.ubuf[0][c - 3].addr, (unsigned)st.ubuf[0][c - 3].size);
            for (int k = 0; k < 24 && k * 4 < (int)st.ubuf[0][c - 3].size; k++) LOG(" %.5g", u[k]);
            LOG("\n");
        }
        if (st.sbuf[0][0].addr) {
            const uint32_t *w = (const uint32_t *)(uintptr_t)st.sbuf[0][0].addr;
            LOG("[draw]   vsb0 %llx size %llu:", (unsigned long long)st.sbuf[0][0].addr, (unsigned long long)st.sbuf[0][0].size);
            for (int k = 0; k < 16; k++) LOG(" %08x", w[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "");
        if (t->format == 0x1 && t->width >= 256 && t->addr) {  // (R8 atlases, e.g. a glyph cache: guest bytes vs the GPU copy)
            const uint8_t *g = (const uint8_t *)(uintptr_t)t->addr;
            uint64_t n = (uint64_t)t->width * t->height, nz = 0;
            for (uint64_t k = 0; k < n; k++) nz += g[k] != 0;
            static int ndump;
            char path[64];
            snprintf(path, sizeof path, "build/tex_r8_%02d.png", ndump++);
            LOG("[draw]   tex%d guest %llx: %llu of %llu bytes nonzero -> %s\n", i, (unsigned long long)t->addr,
                (unsigned long long)nz, (unsigned long long)n, path);
            if (b) renderer_save_texture_tex((TexBackend *)b, path);
        }
        // (LARECOMP_DEBUG_TEX: the first 64 sampled textures of 256+ pixels and the first 32 sampled render targets, once each)
        static NvnTexture *saved[96];
        static int nsaved, nsaved_rt, dump = -1;
        if (dump < 0) dump = ENV("LARECOMP_DEBUG_TEX") != NULL;
        bool rt = b && b->scalable;
        if (dump && b && t->width >= 256 && t->height >= 64 && (rt ? nsaved_rt < 32 : nsaved < 64) &&
            !(b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) {
            bool seen = false;
            for (int k = 0; k < nsaved + nsaved_rt; k++) seen |= saved[k] == t;
            if (!seen) {
                char path[96];
                snprintf(path, sizeof path, "build/%s_%02d_f%x_%ux%u.png", rt ? "rt" : "tex", rt ? nsaved_rt : nsaved, t->format,
                         t->width, t->height);
                saved[nsaved + nsaved_rt] = t;
                if (rt) nsaved_rt++; else nsaved++;
                LOG("[draw]   tex%d -> %s (guest %llx, target %u)\n", i, path, (unsigned long long)t->addr, t->target);
                renderer_save_texture_tex((TexBackend *)b, path);
            }
        }
    }
}

// An indexed draw with parameters in GPU memory whose indices run past its vertex buffer (Z-A: the index count is
// another mesh's, so the draw reads on into the next allocation - matrices, material floats - and its triangles
// stretch across the screen as sheets and beams). Checked on the parameters as the CPU sees them (the culling
// compute writes only the instance count) and skipped; LARECOMP_IB_GUARD=0 draws them.
static bool ib_overrun(CmdRec *r, const uint32_t *p, uint64_t pa) {
    static int on = -1;
    if (on < 0) on = !ENV("LARECOMP_IB_GUARD") || atoi(ENV("LARECOMP_IB_GUARD")) != 0;
    if (!on || !st.nattrib || !(st.attrib[0] & 0xFF)) return false;
    uint32_t s = st.attrib[0] >> 8 & 0x1F, off = st.attrib[0] >> 13;
    if (s >= (uint32_t)st.nstream || !st.vbuf[s].size || st.stream[s][1] || !st.stream[s][0]) return false;
    uint32_t itype = (uint32_t)r->x[1], isz = itype == 0 ? 1 : itype == 1 ? 2 : 4;
    uint64_t count = p[0], first = p[2], ib = r->x[2];
    if (!count || count > (1u << 22) || !ib || ib < GUEST_LO) return false;
    uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu, mx = 0;
    const uint8_t *ip = (const uint8_t *)(uintptr_t)(ib + first * isz);
    for (uint64_t i = 0; i < count; i++) {
        uint32_t v = isz == 1 ? ip[i] : isz == 2 ? ((const uint16_t *)ip)[i] : ((const uint32_t *)ip)[i];
        if (v != (restart & (isz == 1 ? 0xFFu : isz == 2 ? 0xFFFFu : 0xFFFFFFFFu)) && v > mx) mx = v;
    }
    int64_t last = (int64_t)mx + (int32_t)p[3];
    uint64_t stride = st.stream[s][0];
    if (last < 0 || (uint64_t)last * stride + off + 4 <= st.vbuf[s].size) return false;
    static int nlog;
    if (nlog++ < 60) {
        LOG("[ibguard] frame %llu: skipped draw: args %llx = count %u inst %u first %u base %d binst %u | "
            "ib %llx max index %u -> vertex %lld x stride %llu > vbuf %llx+%llu (%llu vertices)\n",
            (unsigned long long)renderer_present_count(), (unsigned long long)pa, p[0], p[1], p[2], (int)p[3], p[4], (unsigned long long)ib, mx, (long long)last,
            (unsigned long long)stride, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size,
            (unsigned long long)(st.vbuf[s].size / stride));
    }
    stat_skipped_here();
    return true;
}
// Indirect draws: the parameters (GL / Vulkan layout: arrays {count, instances, first, base instance}, elements
// {count, instances, first index, base vertex, base instance}) read from guest memory when the list runs, each made a
// direct draw. Arguments: DrawArraysIndirect (mode, indirect), DrawElementsIndirect (mode, index type, index buffer,
// indirect), MultiDrawArraysIndirectCount (mode, indirect, count buffer, max draws, stride), MultiDrawElementsIndirect-
// Count (mode, index type, index buffer, indirect, count buffer, max draws, stride). (Parameters a compute pass of the
// same submission writes are not ready yet here: they come from the previous one.)
static void draw_indirect(CmdRec *r) {
    bool elements = r->op == CMD_DRAW_ELEMENTS_INDIRECT || r->op == CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT;
    bool multi = r->op == CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT || r->op == CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT;
    int k = elements ? 3 : 1;  // the indirect buffer's argument
    uint64_t ind = r->x[k];
    uint32_t n = 1, stride = elements ? 20 : 16;
    if (multi) {
        uint64_t cnt = r->x[k + 1];
        n = (uint32_t)r->x[k + 2];
        if (cnt && cnt >= GUEST_LO) { uint32_t c = *(const uint32_t *)(uintptr_t)cnt; if (c < n) n = c; }
        if (r->x[k + 3]) stride = (uint32_t)r->x[k + 3];
    }
    static int nlog;
    static uint32_t seen_ops;
    if (nlog < 8 || !(seen_ops & (1u << (r->op & 31)))) {
        nlog++;
        seen_ops |= 1u << (r->op & 31);
        LOG("[vk] indirect draw op %u: %u draw(s) at %llx stride %u (count buffer %llx max %llu)\n", r->op, n, (unsigned long long)ind,
            stride, (unsigned long long)(multi ? r->x[k + 1] : 0), (unsigned long long)(multi ? r->x[k + 2] : 0));
    }
    if (!ind || ind < GUEST_LO || n > 4096) return;
    CmdRec d;
    memset(&d, 0, sizeof d);
    d.op = elements ? CMD_DRAW_ELEMENTS : CMD_DRAW_ARRAYS;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t *p = (const uint32_t *)(uintptr_t)(ind + (uint64_t)i * stride);
        static int nargs;
        if (nargs < 400 && ENV("LARECOMP_LOG_INDIRECT")) {
            nargs++;
            LOG("[vk] indirect %llx: %u %u %u %u %u\n", (unsigned long long)(ind + (uint64_t)i * stride), p[0], p[1], p[2], p[3],
                elements ? p[4] : 0);
        }
        uint64_t pa = ind + (uint64_t)i * stride;
        gmem_info();  // (the import, made on first use)
        bool gpu = GM_IN(pa, 20) && !(pa & 3);
        if (gpu) {
            // The GPU fills in parts of the parameters (the instance count: Pokémon Z-A's culling compute), so the
            // draw reads them there; the CPU side covers the index range from the buffer's start (the first index
            // applies on the GPU) with one instance
            static int gpu_args_log = -1;  // (debug) LARECOMP_LOG_GPU_ARGS=<n>: n parameter sets as the GPU sees them
            if (gpu_args_log < 0) gpu_args_log = ENV("LARECOMP_LOG_GPU_ARGS") ? atoi(ENV("LARECOMP_LOG_GPU_ARGS")) : 0;
            if (gpu_args_log > 0 && renderer_present_count() > 600) {
                gpu_args_log--;
                vk_end_pass();
                VkSpan rb = vk_readback(32);
                VkBufferCopy bc = {GM_OFF(pa), rb.off, 20};
                vk_full_barrier(vk_cmd());
                rec_CmdCopyBuffer(vk_cmd(), gmem_buf, rb.buf, 1, &bc);
                renderer_flush();
                const uint32_t *q = rb.ptr;
                LOG("[vk] gpu args %llx: count %u instances %u first %u base %d base inst %u (cpu: %u %u)\n", (unsigned long long)pa,
                    q[0], q[1], q[2], (int)q[3], q[4], p[0], p[1]);
            }
            if (elements && ib_overrun(r, p, pa)) continue;
            g_ind_gpu = true; g_ind_off = GM_OFF(pa); g_ind_stride = stride;
            d.x[0] = r->x[0];
            if (elements) {
                d.x[1] = r->x[1]; d.x[2] = p[0] + p[2]; d.x[3] = r->x[2];
                d.x[4] = (uint64_t)(int64_t)(int32_t)p[3]; d.x[5] = p[4]; d.x[6] = 1;
            } else {
                d.x[1] = 0; d.x[2] = p[0] + p[2]; d.x[3] = p[3]; d.x[4] = 1;
            }
            if (d.x[2]) draw(&d, elements);
            g_ind_gpu = false;
            continue;
        }
        if (!p[0] || !p[1]) continue;  // (no vertices or no instances)
        d.x[0] = r->x[0];  // mode
        if (elements) {  // x: mode, index type, count, index buffer, base vertex, base instance, instances
            d.x[1] = r->x[1]; d.x[2] = p[0];
            d.x[3] = r->x[2] + (uint64_t)p[2] * (r->x[1] == 0 ? 1 : r->x[1] == 1 ? 2 : 4);  // first index (NVN index type 0/1/2: 8/16/32-bit)
            d.x[4] = (uint64_t)(int64_t)(int32_t)p[3]; d.x[5] = p[4]; d.x[6] = p[1];
        } else {  // x: mode, first, count, base instance, instances
            d.x[1] = p[2]; d.x[2] = p[0]; d.x[3] = p[3]; d.x[4] = p[1];
        }
        draw(&d, elements);
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    alias_before_write(t, 0, "upload");
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    rec_CmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < GUEST_LO) return;
    t = alias_read(t);
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)lroundf(t->width * tsx), (int32_t)lroundf(t->height * tsy), 1}},  // (the contents)
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        rec_CmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    rec_CmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    wait_why = "texture -> buffer readback";
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

// Scaled depth copies (the game's depth pyramid) are blits, but a depth format needn't support being a blit destination:
// Intel's drivers don't for D32_SFLOAT_S8_UINT (validation on a UHD 770; a suspect in the UHD 630's crash after a
// depth pass, issues #8, #46). There the depth goes the long way round with transfers only: to a buffer, into an
// unsigned-integer colour image with the same bits per texel, blitted (nearest: bits kept) into a second one at the
// new size, back to the buffer, and into the depth image. The regions are copy_texture's, already cut to the
// contents' extent (Dynamic resolution's sub-rect, issue #191's content_extent) and kept inside their levels.
// The scratch buffer and images are sized by the source and destination *levels* (not the regions, which follow the
// render scale), so a changing scale doesn't regrow them; outgrown ones are destroyed once the submissions that may
// use them are done. All of it (and depth_blit_ok's table) belongs to the translating thread: the render thread, or
// the game's GPU thread with LARECOMP_RENDER_THREAD=0 - copy_texture runs nowhere else.
static bool depth_blit_ok(VkFormat f) {
    static VkFormat known[8];
    static bool ok[8];
    static int n;
    for (int i = 0; i < n; i++) if (known[i] == f) return ok[i];
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &fp);
    bool r = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (n < 8) { known[n] = f; ok[n++] = r; }
    if (!r) LOG("[vk] depth format %d can't be blitted here: scaled depth copies go through a colour image\n", f);
    return r;
}

typedef struct { VkImage img; VmaAllocation mem; uint32_t w, h; } ScratchImage;

static bool scratch_image(ScratchImage *si, uint32_t w, uint32_t h, VkFormat fmt) {
    if (si->img && si->w >= w && si->h >= h) return true;
    if (si->w > w) w = si->w;
    if (si->h > h) h = si->h;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) return false;
    if (si->img) vk_defer_image(si->img, si->mem);  // (no views: never sampled or rendered to)
    vk_image_init_layout(vk_upload_cmd(), img, VK_IMAGE_ASPECT_COLOR_BIT);
    *si = (ScratchImage){img, mem, w, h};
    return true;
}

static void depth_blit_via_color(VkCommandBuffer cb, TexBackend *s, TexBackend *d, uint32_t slv, uint32_t sly, int sx, int sy,
                                 int sw, int sh, uint32_t dlv, uint32_t dly, int dx, int dy, int dw, int dh) {
    if (s->samples != 1 || d->samples != 1) return;  // (buffer copies and blits are single-sample only, as before)
    // bits per texel of the depth aspect in a buffer: 4 (D32 / X8_D24 variants) or 2 (D16)
    uint32_t bpp = s->format == VK_FORMAT_D16_UNORM || s->format == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4;
    VkFormat cf = bpp == 2 ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_UINT;
    static VkBuffer buf;
    static VmaAllocation buf_mem;
    static VkDeviceSize buf_size;
    static struct { VkBuffer buf; VmaAllocation mem; uint64_t seq; } old[8];  // outgrown buffers, until their work is done
    static ScratchImage a[2], b[2];  // per colour format (R16 / R32)
    uint64_t done = vk_completed_seq();
    for (int i = 0; i < 8; i++)
        if (old[i].buf && old[i].seq <= done) { vmaDestroyBuffer(g_vk.vma, old[i].buf, old[i].mem); old[i].buf = VK_NULL_HANDLE; }
    uint32_t slw = level_extent(s->width, slv), slh = level_extent(s->height, slv);
    uint32_t dlw = level_extent(d->width, dlv), dlh = level_extent(d->height, dlv);
    VkDeviceSize need = (VkDeviceSize)(slw * slh > dlw * dlh ? slw * slh : dlw * dlh) * bpp;
    if (need > buf_size) {
        if (need < buf_size * 2) need = buf_size * 2;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer nb;
        VmaAllocation nm;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &nb, &nm, NULL) != VK_SUCCESS) return;
        if (buf) {
            int i = 0;
            while (i < 8 && old[i].buf) i++;
            if (i < 8) old[i].buf = buf, old[i].mem = buf_mem, old[i].seq = vk_pending_seq();
            // (else kept, never freed: 8 outgrown in flight at once, with each at least twice the last - not seen)
        }
        buf = nb; buf_mem = nm; buf_size = need;
    }
    int k = bpp == 2 ? 0 : 1;
    if (!scratch_image(&a[k], slw, slh, cf) || !scratch_image(&b[k], dlw, dlh, cf)) return;
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, slv, sly, 1}, .imageOffset = {sx, sy, 0},
                           .imageExtent = {(uint32_t)sw, (uint32_t)sh, 1}};
    rec_CmdCopyImageToBuffer(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    vk_full_barrier(cb);
    c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = (VkOffset3D){0, 0, 0};
    rec_CmdCopyBufferToImage(cb, buf, a[k].img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {sw, sh, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {dw, dh, 1}}};
    rec_CmdBlitImage(cb, a[k].img, VK_IMAGE_LAYOUT_GENERAL, b[k].img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    vk_full_barrier(cb);
    c.imageExtent = (VkExtent3D){(uint32_t)dw, (uint32_t)dh, 1};
    rec_CmdCopyImageToBuffer(cb, b[k].img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    vk_full_barrier(cb);
    c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT, dlv, dly, 1};
    c.imageOffset = (VkOffset3D){dx, dy, 0};
    rec_CmdCopyBufferToImage(cb, buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)lroundf(st_->width * ssx), sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)lroundf(st_->height * ssy);
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)lroundf(dt->width * dsx), dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)lroundf(dt->height * dsy);
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the contents' extent in the level, and every region stays inside its level: scaled
        // allocations (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies /
        // blits are undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's
        // users). Dynamic resolution draws into the top-left part of the allocation: the rest holds older frames, and
        // a whole-image copy of it into a texture of another scale (the save slot's screenshot, 192x108) took those
        // along (the frame small in a corner, stale frames around it)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        int scw = content_extent(slw, s->scalable, s->sx, s->ax), sch = content_extent(slh, s->scalable, s->sy, s->ay);
        int dcw = content_extent(dlw, d->scalable, d->sx, d->ax), dch = content_extent(dlh, d->scalable, d->sy, d->ay);
        bool same_part = slw == dlw && slh == dlh && scw == dcw && sch == dch;  // (contents in the same part of equal images)
        if (!sr[3]) sw = scw - sx0;
        if (!sr[4]) sh = sch - sy0;
        if (!dr[3]) dw = dcw - dx0;
        if (!dr[4]) dh = dch - dy0;
        if (same_part && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && sw == scw && sh == sch && dw == dcw && dh == dch)
            sw = dw = slw, sh = dh = slh;  // (the same part of images of one size: the whole image, which may alias)
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame || ct_on())
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    if (copy_opt() && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && sw == (int)s->width && sh == (int)s->height && dw == (int)d->width &&
        dh == (int)d->height && slv == 0 && dlv == 0 && sly == 0 && dly == 0 && alias_make(st_, dt, s, d)) {
        if (ct_on()) LOG("[ct] alias made %p -> %p\n", (void *)st_, (void *)dt);
        return;
    }
    alias_before_write(dt, 0, "copy dst");
    if (naliases) s = alias_read(st_)->backend;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        rec_CmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        rec_CmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    static int via_color = -1;  // LARECOMP_VK_DEPTH_BLIT_VIA_COLOR=1: always (tests on GPUs that can blit depth)
    if (via_color < 0) via_color = getenv("LARECOMP_VK_DEPTH_BLIT_VIA_COLOR") ? atoi(getenv("LARECOMP_VK_DEPTH_BLIT_VIA_COLOR")) : 0;
    if (depth && (sa & da & VK_IMAGE_ASPECT_DEPTH_BIT) && (via_color || !depth_blit_ok(d->format) || !depth_blit_ok(s->format))) {
        dl_note("  depth blit via colour %p lv%u (%d,%d %dx%d) -> %p lv%u (%d,%d %dx%d)", (void *)s->image, slv, sx0, sy0, sw, sh,
                (void *)d->image, dlv, dx0, dy0, dw, dh);
        depth_blit_via_color(cb, s, d, slv, sly, sx0, sy0, sw, sh, dlv, dly, dx0, dy0, dw, dh);
        return;
    }
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    rec_CmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

// ---- decoded video into its texture (textures.c renderer_upload_nv12 / upload_pending, render thread): the NV12
// planes go into two images (R8, R8G8: reused while the size stays), and a fullscreen draw converts them into the
// texture through a UNORM view (the bytes as the RGBA upload wrote them: no sRGB encoding). Recorded into the upload
// command buffer, ahead of the frame that samples the texture.
static bool nv12_plane(TexBackend *p, VkFormat f, uint32_t w, uint32_t h, VkCommandBuffer cb) {
    if (p->image) vk_views_forget(p);  // (a new size: the old image is left to the driver's teardown - one per movie size)
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *p = (TexBackend){0};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &p->image, &p->mem, NULL) != VK_SUCCESS) { p->image = VK_NULL_HANDLE; return false; }
    vk_image_init_layout(cb, p->image, VK_IMAGE_ASPECT_COLOR_BIT);
    p->format = f; p->width = w; p->height = h; p->depth = 1; p->levels = p->layers = p->samples = 1;
    p->aspect = VK_IMAGE_ASPECT_COLOR_BIT; p->base_type = VK_IMAGE_VIEW_TYPE_2D; p->gpu_owned = true;
    return true;
}

void vk_convert_nv12(TexBackend *b, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    static TexBackend py, puv;
    static VkDescriptorSetLayout dsl;
    static VkPipelineLayout layout;
    static VkPipeline pipes[2];  // RGBA8, BGRA8 targets
    VkCommandBuffer cb = vk_upload_cmd();
    bool bgra = b->format == VK_FORMAT_B8G8R8A8_UNORM || b->format == VK_FORMAT_B8G8R8A8_SRGB;
    VkFormat raw = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    if (!layout) {
        VkDescriptorSetLayoutBinding bd[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                              {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bd};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &dsl));
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &layout));
        LOG("[movie] video frames converted to RGBA on the GPU\n");
    }
    if (!pipes[bgra]) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = vk_builtin_shader("nv12.vert"), .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = vk_builtin_shader("nv12.frag"), .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                                .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
        VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                                   .pAttachments = &cba};
        VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        uint32_t ndyn = 2;
        fullscreen_vertex_dyn(dyn, &ndyn, 3);
        VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = ndyn,
                                               .pDynamicStates = dyn};
        VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &raw};
        VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                           .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                           .pViewportState = &vs, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                           .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = layout};
        if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pipes[bgra]) != VK_SUCCESS) {
            LOG("[movie] NV12 pipeline failed\n");
            pipes[bgra] = VK_NULL_HANDLE;
            return;
        }
    }
    if (py.width != w || py.height != h || !py.image)
        if (!nv12_plane(&py, VK_FORMAT_R8_UNORM, w, h, cb) || !nv12_plane(&puv, VK_FORMAT_R8G8_UNORM, w / 2, h / 2, cb)) return;
    VkComponentMapping ident = {0};  // (VK_COMPONENT_SWIZZLE_IDENTITY)
    VkImageView vy = vk_image_view(&py, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8_UNORM, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView vuv = vk_image_view(&puv, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8_UNORM, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView vt = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, raw, 0, 1, 0, 1, ident, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!vy || !vuv || !vt) return;
    VkSpan s = vk_transient_copy(nv12, (uint64_t)uv_off + (uint64_t)stride * (h / 2), 16);
    VkBufferImageCopy c[2] = {
        {.bufferOffset = s.off, .bufferRowLength = stride, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {w, h, 1}},
        {.bufferOffset = s.off + uv_off, .bufferRowLength = stride / 2, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {w / 2, h / 2, 1}}};
    vk_full_barrier(cb);
    rec_CmdCopyBufferToImage(cb, s.buf, py.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c[0]);
    rec_CmdCopyBufferToImage(cb, s.buf, puv.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c[1]);
    vk_full_barrier(cb);
    VkDescriptorSet set = vk_alloc_set(dsl);
    VkDescriptorImageInfo ii[2] = {{vk_default_sampler(true), vy, VK_IMAGE_LAYOUT_GENERAL},
                                   {vk_default_sampler(true), vuv, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1]}};
    rec_UpdateDescriptorSets(2, wr);
    uint32_t rw = w < b->width ? w : b->width, rh = h < b->height ? h : b->height;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = vt,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {rw, rh}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[bgra]);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    VkViewport vp = {0, 0, (float)rw, (float)rh, 0, 1};
    VkRect2D sc = {{0, 0}, {rw, rh}};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    fullscreen_vertex_bind(cb);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
}

// keep_alpha: texcopy.frag (the texture's alpha) instead of present.frag (opaque)
static VkPipeline present_pipeline_ex(VkFormat fmt, bool keep_alpha) {
    static struct { VkFormat f; bool a; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].f == fmt && cache[i].a == keep_alpha) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        // (vertex: flip + crop, 32 bytes; fragment: the screen filter, 16 more)
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(keep_alpha ? "texcopy.frag" : "present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    uint32_t ndyn = 2;
    fullscreen_vertex_dyn(dyn, &ndyn, 3);
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = ndyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].a = keep_alpha; cache[i].p = p; break; }
    return p;
}
static VkPipeline present_pipeline(VkFormat fmt) { return present_pipeline_ex(fmt, false); }

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, raw_layer, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX, or a box downscale when it is larger. Each pass: a fullscreen triangle into an RGBA8
// image (gamma-encoded, like the input); up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

// kind: 0 a plain post pass; 1 SMAA edges also writing stencil 1 where they draw, 2 SMAA weights only where the
// stencil is 1 (g_vk.smaa_stencil; dsfmt: the stencil format)
static VkPipeline post_pipeline_kind(const char *fs, int kind, VkFormat dsfmt) {
    static struct { const char *fs; int kind; VkFormat f; VkPipeline p; } cache[16];
    for (int i = 0; i < 16; i++) if (cache[i].p && cache[i].fs == fs && cache[i].kind == kind && cache[i].f == dsfmt) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    uint32_t ndyn = 2;
    fullscreen_vertex_dyn(dyn, &ndyn, 3);
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = ndyn, .pDynamicStates = dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (kind == 1 || kind == 2) {
        ri.stencilAttachmentFormat = dsfmt;
        VkStencilOpState so = {.failOp = VK_STENCIL_OP_KEEP, .passOp = kind == 1 ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP,
                               .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = kind == 1 ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_EQUAL,
                               .compareMask = 0xFF, .writeMask = kind == 1 ? 0xFF : 0, .reference = 1};
        ds.stencilTestEnable = VK_TRUE;
        ds.front = ds.back = so;
    }
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 16; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].kind = kind; cache[i].f = dsfmt; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS, 5: box downscale.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[6];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        rec_CmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
// post_area_w / _h (> 0): only that top-left part of dst (Dynamic resolution: the contents' part)
static uint32_t post_area_w, post_area_h;
// (g_vk.smaa_stencil) a stencil image at w x h for SMAA's edge mask (kept until the size changes; NULL: none usable)
static TexBackend *smaa_stencil_image(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    static TexBackend img;
    static bool unusable;
    TexBackend *d = &img;
    if (unusable) return NULL;
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        static const VkFormat cand[3] = {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
        VkFormat f = VK_FORMAT_UNDEFINED;
        for (int i = 0; i < 3 && !f; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(g_vk.phys, cand[i], &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) f = cand[i];
        }
        if (!f) { unusable = true; LOG("[vk] SMAA stencil: no stencil format, off\n"); return NULL; }
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL) != VK_SUCCESS) { d->image = VK_NULL_HANDLE; return NULL; }
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1; d->samples = 1;
        d->aspect = f == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(cb, d->image, d->aspect);
        LOG("[vk] SMAA stencil image %ux%u format %d\n", w, h, f);
    }
    return d;
}

static void post_pass_ex(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                         bool clear, TexBackend *sten, int kind);
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    post_pass_ex(cb, fs, srcs, n, dst, pc, clear, NULL, 0);
}
// sten / kind (g_vk.smaa_stencil): kind 1 clears sten and writes 1 where the pass draws, kind 2 draws only there
static void post_pass_ex(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                         bool clear, TexBackend *sten, int kind) {
    if (!sten) kind = 0;
    uint32_t aw = post_area_w && post_area_w < dst->width ? post_area_w : dst->width;
    uint32_t ah = post_area_h && post_area_h < dst->height ? post_area_h : dst->height;
    // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    VkPipeline pipe = post_pipeline_kind(fs, kind, sten ? sten->format : VK_FORMAT_UNDEFINED);
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {aw, ah}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    VkRenderingAttachmentInfo sa = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (kind) {
        static const VkComponentMapping id = {0};
        sa.imageView = vk_image_view(sten, VK_IMAGE_VIEW_TYPE_2D, sten->format, 0, 1, 0, 1, id, sten->aspect);
        sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        sa.loadOp = kind == 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        sa.storeOp = kind == 1 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ri.pStencilAttachment = &sa;
    }
    rec_CmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    rec_UpdateDescriptorSets((uint32_t)n, w);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {aw, ah}};  // (the viewport stays the whole image: gl_FragCoord is the pixel)
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    rec_CmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    fullscreen_vertex_bind(cb);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, float fx, float fy, double ow_d, double oh_d, bool *upscaled) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    uint32_t cw = (uint32_t)lroundf(b->width * fx), ch = (uint32_t)lroundf(b->height * fy);  // (the contents' size)
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > cw + 1 || oh > ch + 1);  // (an upscale only)
    *upscaled = fsr;
    // Internal resolution above the window's: the present's one bilinear tap reads at most 2 x 2 source texels per
    // window pixel - all of them at 2x, 1 in 9 at 3x (its centre: no anti-aliasing at all), 4 in 16 at 4x - so the
    // higher resolutions looked worse than 2x. A box filter over each window pixel's whole footprint instead.
    bool down = !fsr && ow && oh && (ow + 1 < cw || oh + 1 < ch);
    *upscaled = fsr || down;  // (the result fills the window: no contents' scale left for the present)
    if (!smaa && !fsr && !down) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        // Dynamic resolution: only the contents' part, and 64 pixels past it (the edge searches reach 2 x 16 pixels, so
        // the contents' pixels never read past it) - the window step's cost then scales with the factor like the game's
        post_area_w = cw + 64, post_area_h = ch + 64;
        // (g_vk.smaa_stencil, integrated GPUs) the weights only where the edge pass found edges (its shader discards
        // the others), as SMAA's reference integration does with a stencil
        TexBackend *sten = g_vk.smaa_stencil ? smaa_stencil_image(cb, b->width, b->height) : NULL;
        VkImageView in[3] = {v};
        post_pass_ex(cb, "smaa_edge.frag", in, 1, edges, m, true, sten, 1);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass_ex(cb, "smaa_weight.frag", wi, 3, weights, m, true, sten, 2);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        post_area_w = post_area_h = 0;
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)cw, ih = (float)ch;  // (EASU fetches texels of the contents: a larger texture needs no uv scale)
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    if (down) {
        float pd[8] = {(float)cw / ow, (float)ch / oh, (float)cw, (float)ch};  // (the contents: Dynamic resolution)
        TexBackend *d = post_image(5, ow, oh);
        VkImageView di[3] = {v};
        post_pass(cb, "boxdown.frag", di, 1, d, pd, false);
        v = post_view(d);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1, pdn = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs || down != pdn)
            LOG("[post] %ux%u%s%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "",
                down ? " box" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr; pdn = down;
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    // Dynamic: the image occupies the top-left part of the texture; FSR maps that part to its whole output, otherwise
    // the present samples the part
    float fx = b->scalable && b->ax > 0 ? b->sx / b->ax : 1, fy = b->scalable && b->ay > 0 ? b->sy / b->ay : 1;
    bool upscaled = false;
    VkImageView src = post_process(cb, b, fx, fy, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh, &upscaled);
    float pvc[8];
    memcpy(pvc, pv, sizeof pvc);
    if (!upscaled) { pvc[4] *= fx; pvc[5] *= fy; pvc[6] *= fx; pvc[7] *= fy; }
    pv = pvc;
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_UpdateDescriptorSets(1, &w);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    float pc[12];
    memcpy(pc, pv, 32);
    screen_filter_params(pc + 8);
    rec_CmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
    fullscreen_vertex_bind(cb);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    rec_CmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
// Only the part its contents occupy (Dynamic resolution: the top-left part; the rest holds older frames).
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    float fx = (float)content_extent((int)b->width, b->scalable, b->sx, b->ax) / (float)b->width;
    float fy = (float)content_extent((int)b->height, b->scalable, b->sy, b->ay) / (float)b->height;
    crop[0] *= fx; crop[1] *= fy; crop[2] *= fx; crop[3] *= fy;
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    // (an sRGB texture sampled decodes to linear: written back through an sRGB target, the PNG holds its own bytes -
    // what a texture dump / HD pack wants. RGBA8 sRGB images are read raw already: raw_view.)
    VkFormat of = vk_format_is_srgb(b->format) && b->format != VK_FORMAT_R8G8B8A8_SRGB && b->format != VK_FORMAT_B8G8R8A8_SRGB
                ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = of,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = of, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline_ex(of, strstr(path, "textures/") != NULL);  // (dumps keep their alpha)
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_UpdateDescriptorSets(1, &wr);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    float pv[12] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    if (strstr(path, "frame_")) screen_filter_params(pv + 8);  // (shots of the game image: as shown)
    rec_CmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pv);
    fullscreen_vertex_bind(cb);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: {  // (program, NVNshaderStageBits): a game may bind its stages from several programs
            NvnProgram *p = (NvnProgram *)(uintptr_t)r->x[0];
            uint32_t m = (uint32_t)r->x[1] & 0x3F;
            if (!m) m = 0x3F;
            if (ENV("LARECOMP_LOG_BINDPROG")) {
                static int n;
                if (n++ < 40) LOG("[bindprog] %p stages %#x (program has %#x)\n", (void *)p, m, p ? p->stage_mask : 0);
            }
            for (int i = 0; i < 6; i++) {
                if (!(m >> i & 1)) continue;
                if (p && (p->stage_mask >> i & 1)) {
                    st.comp.stages[i] = p->stages[i];
                    st.comp.stage_mask |= 1u << i;
                } else {
                    memset(&st.comp.stages[i], 0, sizeof st.comp.stages[i]);
                    st.comp.stage_mask &= ~(1u << i);
                }
            }
            st.program = st.comp.stage_mask ? &st.comp : NULL;
            break;
        }
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = ENV("LARECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_DEPTH_CLAMP: st.depth_clamp = r->x[0] & 1; break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (ENV("LARECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x\n", (unsigned long long)renderer_present_count(), index,
                    st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2]);
            if (ct_on() && index < st.rt.ncolor && st.rt.color[index])
                LOG("[ct] clear-color %p mask %#x sc %d %d %d %d\n", (void *)st.rt.color[index], (uint32_t)r->x[2], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            if (index < st.rt.ncolor && st.rt.color[index]) alias_before_write(st.rt.color[index], 0, "clear");
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            alias_before_write(t, 0, "clear");
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (ct_on() && st.rt.depth)
                LOG("[ct] clear-depth %p d%d s%d sc %d %d %d %d\n", (void *)st.rt.depth, r->x[0] != 0, r->x[2] != 0, st.scissor[0][0],
                    st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            if (st.rt.depth && naliases) {  // (copy aliases: which aspects of the depth target this clear replaces whole)
                NvnTexture *d = st.rt.depth;
                bool whole = st.scissor[0][0] <= 0 && st.scissor[0][1] <= 0 && (int64_t)st.scissor[0][0] + st.scissor[0][2] >= (int64_t)d->width &&
                             (int64_t)st.scissor[0][1] + st.scissor[0][3] >= (int64_t)d->height &&
                             !(st.rt.has_dview && (((st.rt.dview.setmask & 1) && st.rt.dview.base_level) ||
                                                   ((st.rt.dview.setmask & 2) && st.rt.dview.min_layer)));
                for (int i = 0; i < st.rt.ncolor; i++)  // (the clear covers the smallest attachment only)
                    if (st.rt.color[i] && (st.rt.color[i]->width < d->width || st.rt.color[i]->height < d->height)) whole = false;
                VkImageAspectFlags full = 0;
                if (whole && r->x[0] != 0) full |= VK_IMAGE_ASPECT_DEPTH_BIT;
                if (whole && renderer_format_has_stencil(d->format) && (r->x[2] & 0xFF) == 0xFF) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (!renderer_format_has_stencil(d->format)) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (ct_on())
                    LOG("[ct] alias clear %p full %#x (whole %d, x0 %llu x1 %#llx x2 %#llx, dview %d/%#x, ncolor %d)\n", (void *)d, full, whole,
                        (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2], st.rt.has_dview,
                        st.rt.dview.setmask, st.rt.ncolor);
                alias_before_write(d, full, "clear");
            }
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_BEGIN_XFB: xfb_on = true; stat_xfb_sessions++; break;
        case CMD_END_XFB: {
            xfb_on = false;
            // the mirror's output, to the heap right away: shaders read it there as storage (e.g. the characters'
            // shading - left for the submission's end, cutscenes drew them black and scenery vanished)
            if (xm_back_n) { vk_end_pass(); xm_copy_back(vk_cmd()); }
            static int nlog = -1;  // (debug) LARECOMP_LOG_XFB_DATA=<n>: the first entries of n sessions' buffers, as written
            if (nlog < 0) nlog = ENV("LARECOMP_LOG_XFB_DATA") ? atoi(ENV("LARECOMP_LOG_XFB_DATA")) : 0;
            if (nlog > 0 && renderer_present_count() > 600 && gmem_buf) {
                nlog--;
                vk_end_pass();
                for (int b = 0; b < 3; b++) {
                    uint64_t a = xfb_addr[b];
                    if (!a || !GM_IN(a, 64)) continue;
                    VkSpan rb = vk_readback(64);
                    VkBufferCopy bc = {GM_OFF(a), rb.off, 64};
                    vk_full_barrier(vk_cmd());
                    rec_CmdCopyBuffer(vk_cmd(), gmem_buf, rb.buf, 1, &bc);
                    renderer_flush();
                    const float *f = rb.ptr;
                    const uint32_t *u = rb.ptr;
                    LOG("[xfb] buffer %d %llx: %g %g %g %g %g %g | %08x %08x %08x %08x\n", b, (unsigned long long)a, f[0], f[1], f[2], f[3], f[4],
                        f[5], u[0], u[1], u[2], u[3]);
                }
            }
            break;
        }
        case CMD_BIND_XFB_BUFFER:
            if (r->x[0] < 4) { xfb_addr[r->x[0]] = r->x[1]; xfb_size[r->x[0]] = r->x[2]; }
            break;
        case CMD_BIND_XFB_BUFFERS: {
            const uint64_t *b = x;
            for (uint64_t i = 0; i < r->x[1] && i < 4 && r->x[0] + i < 4; i++) {
                xfb_addr[r->x[0] + i] = b[2 * i];
                xfb_size[r->x[0] + i] = b[2 * i + 1];
            }
            break;
        }
        case CMD_SET_RASTERIZER_DISCARD: raster_discard = r->x[0] & 1; break;
        case CMD_DRAW_XFB: {
            static int nlog;
            if (nlog++ < 4) LOG("[xfb] DrawTransformFeedback mode %llu control %llx\n", (unsigned long long)r->x[0], (unsigned long long)r->x[1]);
            break;
        }
        case CMD_DRAW_ARRAYS_INDIRECT: case CMD_DRAW_ELEMENTS_INDIRECT:
        case CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT: case CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT:
            draw_indirect(r);
            break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = ENV("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            list_fenced = true;
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow: the
            // host clock now, replaced by the GPU's time at this point once the submission is done (cq_resolve).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            int mode = vk_counter_mode();
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (mode == 1 && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = ns_to_gpu_ticks(plat_now_ns()); }
            if (!out) break;
            out[0] = 0;
            // SAMPLES_PASSED (occlusion queries: Pokémon Z-A tests its objects with point draws and skips those that
            // pass no samples): no occlusion queries here, so a count that always grows - every object visible
            static uint64_t samples;
            if (type == 1) out[0] = samples += 1u << 20;
            out[1] = mode == 1 ? frame_ts : mode == 3 ? SDL_GetTicksNS() : ns_to_gpu_ticks(plat_now_ns());  // (3 legacy: ns, the old bug)
            static uint64_t *prev_out;  // (frame pacing: the pair's start)
            // (overlap) the game reads this pair before its GPU work is done: the end counter reports the latest
            // completed pair's GPU span after this pair's start (the frame before, as a frame-late measurement)
            if (mode == 2 && overlap_on() && prev_out && out - prev_out == 2) out[1] = prev_out[1] + atomic_load(&g_last_gpu_frame_ticks);
            if (mode == 0 || mode == 2) out[1] = pace_end_counter(prev_out, out, out[1]);
            prev_out = out;
            {   // (test) LARECOMP_TEST_COUNTER_DELAY_MS=<ms>: stall the host after a frame's first counter - a slow CPU
                static double delay = -1;
                static uint64_t delayed_frame = ~0ull;
                if (delay < 0) delay = ENV("LARECOMP_TEST_COUNTER_DELAY_MS") ? atof(ENV("LARECOMP_TEST_COUNTER_DELAY_MS")) : 0;
                if (delay > 0 && vk_frame_counter() != delayed_frame) {
                    delayed_frame = vk_frame_counter();
                    SDL_DelayPrecise((Uint64)(delay * 1e6));
                }
            }
            if (mode == 2 && type == 0) {  // (type 0: timestamp)
                VkCommandBuffer cb = vk_cmd();  // (first: may begin the buffer, which resets the slot's queries)
                CounterQ *q = &cq[cur_sub];
                pthread_mutex_lock(&cq_lock);
                if (q->pool && q->n < CQ_MAX) {
                    rec_CmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->pool, q->n);
                    q->out[q->n++] = out;
                }
                pthread_mutex_unlock(&cq_lock);
            }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            wait_why = "buffer -> buffer copy";
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += r->size;
    }
}

// submit: end the submission after this list (else it goes on into the next list's)
static void execute_one(uint64_t handle, bool submit) {
    extern void plat_sample_focus(void);  // (runtime/platform.h: LARECOMP_SAMPLE_EXEC samples this thread)
    plat_sample_focus();
    exec_gen++;
    uint64_t t0 = SDL_GetTicksNS();
    exec_list(handle, 0);
    uint64_t dt = SDL_GetTicksNS() - t0;
    cum_exec += dt;
    if (prof) prof_exec += dt;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = ENV("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    static long batch_draws = -1;
    if (batch_draws < 0) batch_draws = ENV("LARECOMP_VK_BATCH_DRAWS") ? atol(ENV("LARECOMP_VK_BATCH_DRAWS")) : 400;
    if (sync_submit) renderer_flush();
    else if (submit || list_fenced || cum_draws - submit_draws >= (uint64_t)batch_draws) renderer_submit();
    else if (xm_back_n) { vk_end_pass(); xm_copy_back(vk_cmd()); }  // (as a submission's end does: a later list may read it)
    list_fenced = false;
}

void backend_execute(uint64_t handle) { execute_one(handle, true); }

// The game hands its command lists over a frame at a time (Pokémon Z-A: ~100 in one nvnQueueSubmitCommands): they go
// to the GPU in a few submissions instead of one each. ~80 submissions a frame cost ~20 % of the render thread
// (vkQueueSubmit, a new command buffer and its state, the vertex cache's write-tracking query, a ring slot each).
// A submission still ends after a list with a fence in it (a waiter on another thread must not find it pending, as
// before), after LARECOMP_VK_BATCH_DRAWS draws (default 400: the GPU starts on the frame while the rest is
// translated), and with the call. LARECOMP_VK_SUBMIT_EACH=1: one submission per list, as before.
void backend_execute_batch(const uint64_t *handles, int n) {
    static int each = -1;
    if (each < 0) each = ENV("LARECOMP_VK_SUBMIT_EACH") != NULL;
    for (int i = 0; i < n; i++) execute_one(handles[i], each || i == n - 1);
}
