"""CPU / GPU overlap (exec.c): the deferred frame fence and the storage snapshot state machine. No GPU is needed.

Extracts overlap_defer (the SyncWait decision) and the snap_storage block from runtime/nvn/vulkan/exec.c by their
markers, compiles each with stubs, and checks the sequences the design relies on (LAS-reports/vk_overlap_design.md).
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
defer = source[source.index('static uint64_t overlap_defer('):source.index('\nuint64_t renderer_fence_seq(')]
snap = source[source.index('#define SNAP_HALF'):source.index('// (debug) LARECOMP_LOG_BINDS=1:')]
decide = source[source.index('#define VK_OVERLAP_DEFAULT'):source.index('static _Atomic uint64_t g_last_gpu_frame_ticks;')]
assert 'VK_OVERLAP_DEFAULT 1' in decide, 'the default is on where the write watch is available (Beta 5)'
assert 'static void snap_ring_import_tail(void);' in snap

defer_harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
''' + defer + r'''
// The game's frame loop: SyncWait on its frames' fences, each a new submission; `done` is what the GPU completed,
// pres the latest two presents' submissions (newest first), h the latest two fences waited on.
static uint64_t pres[2], h[2];
static void present(uint64_t s) { pres[1] = pres[0], pres[0] = s; }
int main(void) {
    // before any present, one fence a call: the fence two waits back bounds the queue
    assert(overlap_defer(1, 0, pres, h) == 0 && h[0] == 1);
    assert(overlap_defer(2, 0, pres, h) == 0 && h[0] == 2 && h[1] == 1);
    assert(overlap_defer(3, 0, pres, h) == 1);
    assert(overlap_defer(4, 0, pres, h) == 2);
    // the same fence again: a real wait; a complete one: none
    assert(overlap_defer(4, 0, pres, h) == 4);
    assert(overlap_defer(4, 4, pres, h) == 0);
    // Pokémon Z-A: two fences a frame, A mid-frame and B after the present (P). Frame n: A = 10n+1, B = 10n+2,
    // P = 10n+3. B never waits for its own frame's A, with the present translated before the wait or after it, and
    // whatever the GPU completed: each wait is for the previous frame's present at most.
    for (int late = 0; late < 2; late++)
        for (uint64_t gpu_lag = 0; gpu_lag < 3; gpu_lag++) {
            pres[0] = pres[1] = h[0] = h[1] = 0;
            for (uint64_t n = 1; n < 30; n++) {
                uint64_t A = 10 * n + 1, B = 10 * n + 2, P = 10 * n + 3, prevP = 10 * (n - 1) + 3;
                uint64_t done = n > gpu_lag ? 10 * (n - gpu_lag) - 7 : 0;  // (frame n - gpu_lag + 1's A done or not)
                uint64_t w = overlap_defer(A, done, pres, h);
                assert(w <= prevP && w < A);
                if (!late) present(P);
                w = overlap_defer(B, done, pres, h);
                assert(w != A && w <= prevP);  // (the bug: the second fence waited for the first)
                if (n > 2 && done < prevP) assert(w == prevP);
                if (late) present(P);
            }
        }
    // at most two frames queued: with a GPU that never finishes, each frame's waits are for the frame before
    pres[0] = pres[1] = h[0] = h[1] = 0;
    uint64_t done = 0;
    for (uint64_t n = 1; n < 30; n++) {
        uint64_t A = 10 * n + 1, B = 10 * n + 2, P = 10 * n + 3, prevP = 10 * (n - 1) + 3;
        uint64_t w = overlap_defer(A, done, pres, h);
        if (n > 1) assert(w == prevP);
        if (w) done = w;
        present(P);
        assert(overlap_defer(B, done, pres, h) == 0);  // (the previous frame: done at the first wait)
    }
    // a fence of an earlier frame (older than the latest two presents): a real wait on it
    assert(overlap_defer(10 * 25 + 1, 0, pres, h) == 10 * 25 + 1);
    // loading: fences with no presents between them - bounded by the fence two waits back
    uint64_t last = pres[0];
    assert(overlap_defer(last + 1, last, pres, h) == 0);
    assert(overlap_defer(last + 2, last, pres, h) == 0);
    assert(overlap_defer(last + 3, last, pres, h) == last + 1);
    assert(overlap_defer(last + 4, last, pres, h) == last + 2);
    puts("PASS: overlap_defer - one frame late (Z-A's two fences a frame: never its own frame's), presents translated before / after the wait, no presents (two fences back), repeats, earlier frames, two in flight");
}
'''

# overlap_on: the switch, the default, and the automatic off without page write tracking
decide_harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HEAP_BASE 0x7240000000ull
#define LOG(...) fprintf(stderr, __VA_ARGS__)
static int tracking = 1;
static int pages_cpu_written(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    assert(addr == HEAP_BASE && size == 4096 && !since); *now = 1; return tracking ? 0 : -1;
}
static const char *heap_tracking_reason(void) { return "the guest heap is driver memory (dma-buf), which the OS can't track"; }
// (F1 > Compatibility, settings.h: Auto here; the GPU vendor: not AMD - its own default is off, tested below)
enum { GPU_AUTO, GPU_ON, GPU_OFF, GPU_OVERLAP = 0, GPU_COMPAT_N = 8 };
static struct { int gpu_compat[GPU_COMPAT_N]; } g_settings;
static struct { struct { uint32_t vendorID; } props; } g_vk;
''' + decide + r'''
static bool xm_on(void) { return false; }  // (the transform feedback mirror: not part of this test)
int main(int argc, char **argv) {
    assert(argc == 3);
    tracking = !strcmp(argv[1], "tracked") || !strcmp(argv[1], "amd");
    if (!strcmp(argv[1], "amd")) g_vk.props.vendorID = 0x1002;
    bool want = !strcmp(argv[2], "on");
    assert(overlap_on() == want && overlap_on() == want);  // (decided once)
    printf("PASS: overlap %s, tracking %s, LARECOMP_GPU_OVERLAP=%s, LARECOMP_OVERLAP_WATCH=%s\n", want ? "on" : "off", tracking ? "yes" : "no",
           getenv("LARECOMP_GPU_OVERLAP") ? getenv("LARECOMP_GPU_OVERLAP") : "(unset)",
           getenv("LARECOMP_OVERLAP_WATCH") ? getenv("LARECOMP_OVERLAP_WATCH") : "(unset)");
}
'''

snap_harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct { uint64_t addr, size; } Range;
static struct { Range sbuf[6][16]; } st;
static uint8_t *heap_mem;
#define HEAP_MAX (256ull << 20)
#define HEAP_BASE ((uint64_t)(uintptr_t)heap_mem)
// The guest memory window (exec.c gwin_pick): the heap here, no physical-memory window (Pokémon Z-A's) and no ring of its own
typedef struct { uint64_t base, max, ring; bool phys; } GWin;
static GWin gwin_;
static const GWin *gwin(void) { gwin_.base = HEAP_BASE, gwin_.max = HEAP_MAX; return &gwin_; }
#define gwin_base (gwin()->base)
#define gwin_max (gwin()->max)
#define GWIN_RING (64ull << 20)
#define LOG(...) fprintf(stderr, __VA_ARGS__)
static uint64_t heap_size = 48ull << 20;  // (the test ranges below sit under 24 MB; the ring follows at 48 MB)
static uint64_t kernel_heap_size(void) { return heap_size; }
static unsigned commits;
static void plat_commit(uint64_t addr, uint64_t size) { assert(addr == HEAP_BASE + heap_size && size == (64ull << 20)); commits++; }
// The OS's page write tracking (exec.c pages_cpu_written over plat_written_pages): 4 KB dirty bits the test sets
// as "the CPU wrote here", reset by a look, and the shared page generations a look stamps
static int track_off;  // (no tracking: -1)
static uint8_t page_dirty[HEAP_MAX / 4096];
static uint32_t page_gen[HEAP_MAX / 4096], pgen = 1;
static unsigned looks;
static void cpu_wrote(uint64_t addr) { page_dirty[(addr - HEAP_BASE) / 4096] = 1; }
static int pages_cpu_written(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (track_off) return -1;
    assert(addr >= HEAP_BASE && addr - HEAP_BASE + size <= HEAP_MAX);
    looks++;
    uint64_t lo = (addr - HEAP_BASE) / 4096, hi = (addr - HEAP_BASE + size + 4095) / 4096;
    uint32_t g = ++pgen;
    int w = 0;
    for (uint64_t p = lo; p < hi; p++) {
        if (page_dirty[p]) { page_dirty[p] = 0; page_gen[p] = g; }
        if (page_gen[p] > since) w = 1;
    }
    *now = g;
    return w;
}
static bool userptr;
static bool overlap_on(void) { return true; }
static uint64_t completed, pending = 1, issued, waited;
static uint64_t vk_completed_seq(void) { return completed; }
static uint64_t vk_pending_seq(void) { return pending; }
static uint64_t renderer_issued_seq(void) { return issued; }
static const char *wait_why;
static void vk_wait_seq(uint64_t seq) { assert(seq <= issued); waited = seq; completed = seq; }
static int pool_flags = 4;  // CPU_CACHED
int nvn_pool_flags_at(uint64_t addr) { (void)addr; return pool_flags; }
static uint64_t frame = 10;
uint64_t renderer_exec_frame(void) { return frame; }
static const char *level = "Field";
const char *game_level_name(void) { return level; }
static uint64_t stat_snap_bytes, stat_snap_copies;
''' + snap + r'''
static void snap_ring_import_tail(void) {}
static bool heap_is_userptr(void) { return userptr; }
static uint64_t ring_lo(void) { return HEAP_BASE + snap_base; }
static bool in_ring(uint64_t a) { return a >= ring_lo() && a < ring_lo() + SNAP_RING; }
static bool in_half(uint64_t a, int h) { return a >= ring_lo() + h * SNAP_HALF && a < ring_lo() + (h + 1) * SNAP_HALF; }

// The write watch (LARECOMP_OVERLAP_WATCH unset): compute-only ranges in place, copied once the OS's page write
// tracking shows a CPU write on their pages; no page protection anywhere
static void watch_tests(void) {
    assert(snap_on() && watch_on());
    uint64_t A = HEAP_BASE + 0x1000, B = HEAP_BASE + 0x20000, B2 = HEAP_BASE + 0x20800, C = HEAP_BASE + 0x40000, D = HEAP_BASE + 0x60000;
    // W1. a compute-only range starts in place; its pages are looked at (stamped) once, and once per frame after
    unsigned l = looks;
    assert(snap_storage(A, 32640, true) == A && rs_tab[0].mode == RS_PLACE && rs_tab[0].wgen && looks == l + 1);
    assert(snap_storage(A, 32640, true) == A && looks == l + 1);  // (again this frame: no look)
    frame++;
    assert(snap_storage(A, 32640, true) == A && looks == l + 2);  // no CPU write: in place
    // W2. the CPU writes into it: one write is noted (an initialisation stays in place); writes that keep coming
    //     (within 8 frames) make it a copy for good, kept while unchanged
    cpu_wrote(A + 100);
    frame++;
    assert(snap_storage(A, 32640, true) == A && rs_tab[0].mode == RS_PLACE);
    cpu_wrote(A + 200);
    frame++;
    uint64_t a1 = snap_storage(A, 32640, true);
    assert(a1 != A && in_ring(a1) && rs_tab[0].mode == RS_COPY && !memcmp((void *)(uintptr_t)a1, heap_mem + 0x1000, 32640));
    frame++;
    assert(snap_storage(A, 32640, true) == a1);
    // W3. a level change or 10 s unused never demotes a copied range; a data change refreshes it
    level = "Dungeon";
    frame += 601;  // (unused 10 s: its copy is released and made again - still a copy)
    uint64_t a1b = snap_storage(A, 32640, true);
    assert(a1b != A && in_ring(a1b) && rs_tab[0].mode == RS_COPY);
    heap_mem[0x1000 + 3] ^= 1;
    frame++;
    uint64_t a2 = snap_storage(A, 32640, true);
    assert(a2 != a1b && rs_tab[0].mode == RS_COPY && !memcmp((void *)(uintptr_t)a2, heap_mem + 0x1000, 32640));
    // W4. an in-place tracked range stays in place over a level change (no hash probe, no copy); a GPU write (no
    //     dirty bit) changes nothing
    assert(snap_storage(C, 2048, true) == C && rs_tab[1].mode == RS_PLACE);
    heap_mem[0x40000 + 9] = 0x42;  // (the GPU wrote: the tracking doesn't see it)
    level = "Beach";
    frame += 601;
    assert(snap_storage(C, 2048, true) == C && rs_tab[1].mode == RS_PLACE);
    // W5. two ranges on one page (the light lists): a CPU write into one marks both
    frame++;
    assert(snap_storage(B, 2048, true) == B && snap_storage(B2, 2048, true) == B2);
    cpu_wrote(B2 + 16);
    frame++;
    assert(snap_storage(B, 2048, true) == B && snap_storage(B2, 2048, true) == B2);  // (noted)
    cpu_wrote(B2 + 32);
    frame++;
    uint64_t b1 = snap_storage(B, 2048, true), b2 = snap_storage(B2, 2048, true);
    assert(b1 != B && b2 != B2 && rs_tab[2].mode == RS_COPY && rs_tab[3].mode == RS_COPY);
    // W6. a write before the range is first seen is consumed by the first look: not a reason to copy
    cpu_wrote(D + 8);
    frame++;
    assert(snap_storage(D, 2048, true) == D && rs_tab[4].mode == RS_PLACE);
    frame++;
    assert(snap_storage(D, 2048, true) == D && rs_tab[4].mode == RS_PLACE);
    // W7. a draw's storage read of a tracked in-place range: a copy for good, as without the watch
    uint64_t E = HEAP_BASE + 0x80000;
    frame++;
    assert(snap_storage(E, 2048, true) == E);
    frame++;
    uint64_t e1 = snap_storage(E, 2048, false);
    assert(e1 != E && rs_tab[5].mode == RS_COPY && rs_tab[5].draw_bound);
    frame++;
    assert(snap_storage(E, 2048, true) == e1);
    // W8. another looker (the vertex cache) consuming the OS bits first doesn't hide the write: the generations carry it
    uint64_t F = HEAP_BASE + 0xA0000;
    frame++;
    assert(snap_storage(F, 2048, true) == F);
    uint32_t g;
    for (int i = 0; i < 2; i++) {  // (written in two frames, someone else looking first each time)
        cpu_wrote(F + 4);
        assert(pages_cpu_written(F, 4096, 0, &g) == 1);  // (the bits are reset, the page stamped)
        frame++;
        uint64_t fc = snap_storage(F, 2048, true);
        assert(i == 0 ? fc == F : (fc != F && rs_tab[6].mode == RS_COPY));
    }
    // W9. BowWow's skinning output (22048 bytes, a GPU output the CPU reads back) whose last page holds the dispatch's
    //     12-byte parameter blocks, which the CPU rewrites every frame: the output stays in place, the params are copied
    uint64_t S = HEAP_BASE + 0xC0000, P = S + 22048 + 0x40;  // (P on S's last page)
    frame++;
    assert(snap_storage(S, 22048, true) == S && snap_storage(P, 12, true) == P);
    for (int i = 0; i < 4; i++) {
        cpu_wrote(P);
        frame++;
        uint64_t sp = snap_storage(P, 12, true);
        assert(snap_storage(S, 22048, true) == S && rs_tab[7].mode == RS_PLACE);  // (never the output)
        if (i >= 1) assert(sp != P && rs_tab[8].mode == RS_COPY);              // (the params: written every frame)
    }
    cpu_wrote(S + 8);  // a write on one of the output's own pages, then another: that is the CPU's - copied
    frame++;
    assert(snap_storage(S, 22048, true) == S);
    cpu_wrote(S + 8);
    frame++;
    assert(snap_storage(S, 22048, true) != S && rs_tab[7].mode == RS_COPY);
    // W10. a single write (an initialisation) leaves a range in place, also after many frames
    uint64_t I = HEAP_BASE + 0x100000;
    frame++;
    assert(snap_storage(I, 4096, true) == I);
    cpu_wrote(I);
    frame++;
    assert(snap_storage(I, 4096, true) == I);
    for (int i = 0; i < 20; i++) { frame++; assert(snap_storage(I, 4096, true) == I && rs_tab[9].mode == RS_PLACE); }
    cpu_wrote(I);  // (a lone write 21 frames later: still not a rewrite)
    frame++;
    assert(snap_storage(I, 4096, true) == I && rs_tab[9].mode == RS_PLACE);
    puts("PASS: write watch - in place + tracked, CPU writes that keep coming make a copy for good (a single one doesn't), no demotion on level / idle, GPU writes ignored, a shared page marks both, a page shared with a small parameter block doesn't, a prior write is consumed, draw reads copy, other lookers");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    heap_mem = _aligned_malloc(HEAP_MAX, 0x10000);  // (MinGW has no posix_memalign)
    if (heap_mem) memset(heap_mem, 0, HEAP_MAX);
#else
    if (posix_memalign((void **)&heap_mem, 0x10000, HEAP_MAX)) heap_mem = NULL;  // (aligned like the real heap base)
    else memset(heap_mem, 0, HEAP_MAX);
#endif
    assert(heap_mem);
    if (argc > 1 && !strcmp(argv[1], "watch")) { watch_tests(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "notrack")) {  // no OS write tracking (a userptr / dma-buf heap on Linux, no write watch): the probe instead
        track_off = 1; userptr = true;
        assert(snap_on() && !watch_on());
        uint64_t A = HEAP_BASE + 0x1000;
        assert(snap_storage(A, 32640, true) != A && rs_tab[0].mode == RS_PROBE);
        puts("PASS: no page write tracking - the watch is off, compute-only ranges get the hash probe");
        return 0;
    }
    uint64_t A = HEAP_BASE + 0x1000, B = HEAP_BASE + 0x20000, C = HEAP_BASE + 0x40000;
    uint8_t *a = heap_mem + 0x1000, *b = heap_mem + 0x20000;
    memset(a, 1, 32640); memset(b, 2, 2048); memset(heap_mem + 0x40000, 3, 4096);
    // decided on first use: the ring follows the heap (64 KB aligned), committed once
    assert(snap_ring_end() == heap_size + SNAP_RING && commits == 1 && snap_on());
    // 1. the probe: a first use binds a copy with the same bytes
    uint64_t c1 = snap_storage(A, 32640, true);
    assert(c1 != A && in_ring(c1) && !memcmp((void *)(uintptr_t)c1, a, 32640) && nrs == 1 && stat_snap_copies == 1);
    assert(snap_storage(A, 32640, true) == c1);  // (bound again this frame: the same copy)
    // 2. unchanged at the next use: IN PLACE from then on (the GPU-only skinning outputs)
    frame++;
    assert(snap_storage(A, 32640, true) == A && rs_tab[0].mode == RS_PLACE);
    frame++;
    a[100] = 9;  // (the GPU writes it on the Switch; here the probe said the CPU doesn't: stays in place, no hash)
    assert(snap_storage(A, 32640, true) == A);
    // 3. the light list: rewritten between frames -> COPY, a fresh copy whenever its data changes
    uint64_t c2 = snap_storage(B, 2048, true);
    assert(c2 != B && in_ring(c2) && rs_tab[1].mode == RS_PROBE);
    frame++;
    b[7] = 0x55;
    uint64_t c3 = snap_storage(B, 2048, true);
    assert(c3 != B && c3 != c2 && rs_tab[1].mode == RS_COPY && ((uint8_t *)(uintptr_t)c3)[7] == 0x55);
    assert(snap_storage(B, 2048, false) == c3);  // (a draw reading it this frame: the same copy)
    frame++;
    assert(snap_storage(B, 2048, true) == c3);   // unchanged: the copy (maybe GPU-modified) stays
    frame++;
    b[8] = 0x66;
    uint64_t c4 = snap_storage(B, 2048, true);
    assert(c4 != c3 && ((uint8_t *)(uintptr_t)c4)[8] == 0x66);
    // 4. a range a draw reads as storage: a copy for good from its first use (no probe: the hash can't see the CPU
    //    rewriting the same bytes), kept while unchanged, refreshed when changed; a compute bind takes the same copy
    uint64_t c5 = snap_storage(C, 4096, false);
    assert(c5 != C && rs_tab[2].mode == RS_COPY && rs_tab[2].draw_bound);
    assert(snap_storage(C, 4096, true) == c5);
    frame++;
    assert(snap_storage(C, 4096, false) == c5 && snap_storage(C, 4096, true) == c5);
    frame++;
    heap_mem[0x40000 + 5] = 7;
    uint64_t c6 = snap_storage(C, 4096, false);
    assert(c6 != C && c6 != c5 && rs_tab[2].mode == RS_COPY);
    level = "Beach";  // a new level: a draw-read range is not probed again, it stays a copy
    frame++;
    assert(snap_storage(C, 4096, true) == c6 && rs_tab[2].mode == RS_COPY);
    level = "Field";
    // 4b. first seen in a compute-only frame (the title screen): probed, settled in place; later read by a draw:
    //     copied for good, and the compute binds after that use the same copy even though the data never changes
    uint64_t H = HEAP_BASE + 0x60000;
    memset(heap_mem + 0x60000, 8, 2048);
    frame++;
    assert(snap_storage(H, 2048, true) != H && rs_tab[3].mode == RS_PROBE);
    frame++;
    assert(snap_storage(H, 2048, true) == H && rs_tab[3].mode == RS_PLACE);
    frame++;
    assert(snap_storage(H, 2048, true) == H);  // (a compute bind, first in the frame: still in place)
    uint64_t h1 = snap_storage(H, 2048, false);  // the draw, same frame: a copy from now on
    assert(h1 != H && in_ring(h1) && rs_tab[3].mode == RS_COPY && rs_tab[3].draw_bound);
    frame++;
    assert(snap_storage(H, 2048, true) == h1 && snap_storage(H, 2048, false) == h1);
    frame += 601;  // unused for 10 s: still not probed again (its copy was released: a new one, still a copy)
    uint64_t h2 = snap_storage(H, 2048, true);
    assert(h2 != H && in_ring(h2) && rs_tab[3].mode == RS_COPY && !memcmp((void *)(uintptr_t)h2, heap_mem + 0x60000, 2048));
    // 4c. in place from a compute-only probe, then a draw in a later frame (not the same one): copied for good
    uint64_t I = HEAP_BASE + 0x70000;
    frame++;
    assert(snap_storage(I, 2048, true) != I);
    frame++;
    assert(snap_storage(I, 2048, true) == I && rs_tab[4].mode == RS_PLACE);
    frame++;
    uint64_t i1 = snap_storage(I, 2048, false);
    assert(i1 != I && rs_tab[4].mode == RS_COPY);
    frame++;
    assert(snap_storage(I, 2048, true) == i1);
    // 5. a new level: probed again
    frame++;
    level = "Dungeon";
    assert(snap_storage(A, 32640, true) != A && rs_tab[0].mode == RS_PROBE);
    frame++;
    assert(snap_storage(A, 32640, true) == A && rs_tab[0].mode == RS_PLACE);
    // 6. unused for 10 s: probed again
    frame += 601;
    assert(snap_storage(A, 32640, true) != A && rs_tab[0].mode == RS_PROBE);
    // 7. not snapshotted: static pools, ranges in or past the ring, too large, outside the heap
    pool_flags = 1;
    assert(snap_storage(B, 2048, true) == B);
    pool_flags = 4;
    assert(snap_storage(ring_lo(), 2048, true) == ring_lo());
    assert(snap_storage(HEAP_BASE + heap_size - 1024, 2048, true) == HEAP_BASE + heap_size - 1024);
    assert(snap_storage(A, SNAP_HALF / 2 + 256, true) == A);
    assert(snap_storage(A + 0x4000, SNAP_HALF / 2, true) != A + 0x4000);  // (the largest allowed: a half holds two, one bound by compute)
    assert(snap_storage(HEAP_BASE - 4096, 2048, true) == HEAP_BASE - 4096);
    // 8. the halves: 1 MB copies (512 KB ranges and their pristine images) fill half 0; the switch to half 1 needs no wait (never used); a live copy stays where
    //    it is across the switch (the GPU's writes into it survive); the switch back waits for the submission pending
    //    when half 0 was left and reuses only freed slots, walking past the live copies
    frame++;
    uint64_t big = SNAP_HALF / 16;  // (a compute-bound copy takes twice that: the copy and its pristine image)
    uint8_t *d = heap_mem + (1ull << 20);
    uint64_t D = HEAP_BASE + (1ull << 20);
    memset(d, 4, big);
    snap_off = 0; snap_half = 0; snap_gen++;  // (a clean half 0: the earlier cases' copies released)
    for (int i = 0; i < nrs; i++) rs_tab[i].copy = 0;
    uint64_t keep = snap_storage(D, big, true);  // a COPY candidate: probe copy at the start of half 0
    assert(in_half(keep, 0));
    frame++;
    d[0] = 5;
    uint64_t keep2 = snap_storage(D, big, true);  // changed: COPY mode, a copy in half 0
    assert(in_half(keep2, 0) && rs_tab[nrs - 1].mode == RS_COPY);
    issued = 5; pending = 6; completed = 5; waited = 0;
    for (int i = 0; i < 6; i++) {  // 6 more fill half 0 (8 x 1 MB)
        uint64_t E = HEAP_BASE + (2ull << 20) + (uint64_t)i * 4096;
        assert(in_half(snap_storage(E, big, true), 0));
    }
    uint64_t F = HEAP_BASE + (3ull << 20);
    uint64_t f1 = snap_storage(F, big, true);  // the 9th: half 1, no wait, half 0's users noted as the pending submission
    assert(in_half(f1, 1) && !waited && snap_half == 1 && snap_half_seq[0] == 6);
    frame++;
    // the old COPY-mode entry, unchanged: its live copy in half 0 is bound again (not remade from the original)
    uint64_t keep3 = snap_storage(D, big, true);
    assert(keep3 == keep2 && in_half(keep3, 0));
    for (int i = 0; i < 7; i++) {  // 7 more fill half 1 (F + 7 x 1 MB)
        uint64_t E = HEAP_BASE + (5ull << 20) + (uint64_t)i * 4096;
        assert(in_half(snap_storage(E, big, true), 1));
    }
    // half 1 is full. Switching back needs submission 6 done: it is only being recorded (issued 5): the bind falls
    // back to the original for this one draw, nothing waits, nothing is overwritten
    frame++;
    uint64_t G = HEAP_BASE + (7ull << 20);
    assert(snap_storage(G, big, true) == G && snap_half == 1 && !waited);
    // once submitted (issued 7, still not complete): the switch waits for 6, then takes half 0
    issued = 7; pending = 8;
    uint64_t g1 = snap_storage(G, big, true);  // (half 0's only free slot: the probe copy D left behind; the live copies stay)
    assert(in_half(g1, 0) && g1 == keep && waited == 6 && snap_half == 0 && snap_half_seq[1] == 8);
    assert(rs_tab[nrs - 1].copy == g1 && !memcmp((void *)(uintptr_t)keep2, d, big));  // (D's live copy untouched)
    // 8b. the per-frame pools (0x12): a draw's storage read is copied without a table entry; a compute bind of one
    //     gets an entry, as before
    issued = pending = completed = 9;
    snap_off = 0; snap_half = 0;  // (a clean half 0: case 8's copies released)
    for (int i = 0; i < nrs; i++) rs_tab[i].copy = rs_tab[i].pristine = 0;
    frame++;
    int n0 = nrs;
    pool_flags = 0x12;
    uint8_t *p12 = heap_mem + (6ull << 20);
    uint64_t P = HEAP_BASE + (6ull << 20);
    memset(p12, 7, 64);
    uint64_t pc = snap_storage(P, 64, false);
    assert(pc != P && nrs == n0 && !memcmp((void *)(uintptr_t)pc, p12, 64));
    assert(snap_storage(P + 256, 64, true) != P + 256 && nrs == n0 + 1);
    pool_flags = 4;
    // 8c. a compute copy's remake keeps the GPU's writes into it where the CPU didn't write (the exposure list's state)
    frame++;
    uint8_t *x = heap_mem + (6ull << 20) + 65536;
    uint64_t X = HEAP_BASE + (6ull << 20) + 65536;
    memset(x, 1, 2048);
    assert(snap_storage(X, 2048, true) != X);  // probe
    frame++;
    x[100] = 2;
    uint64_t x1 = snap_storage(X, 2048, true);  // the CPU rewrote it: a copy, with its pristine image behind it
    RangeState *ex = &rs_tab[nrs - 1];
    assert(ex->addr == X && ex->mode == RS_COPY && ex->pristine == x1 + 2048);
    ((uint8_t *)(uintptr_t)x1)[10] = 9;  // the GPU writes bytes 10 and 20
    ((uint8_t *)(uintptr_t)x1)[20] = 9;
    frame++;
    x[20] = 3, x[30] = 4;  // the CPU writes bytes 20 and 30
    uint64_t x2 = snap_storage(X, 2048, true);
    const uint8_t *m2 = (const uint8_t *)(uintptr_t)x2;
    assert(x2 != x1 && m2[10] == 9 && m2[20] == 3 && m2[30] == 4 && m2[100] == 2 && m2[0] == 1);
    assert(!memcmp((void *)(uintptr_t)ex->pristine, x, 2048));
    // 8d. a full table evicts the entry bound longest ago (never the whole table)
    frame++;
    for (int i = 0; nrs < RS_MAX; i++) assert(snap_storage(HEAP_BASE + (6ull << 20) + 131072 + (uint64_t)i * 256, 256, true));
    uint64_t a0 = rs_tab[0].addr;
    rs_tab[3].frame = 0;
    frame++;
    uint64_t Z = HEAP_BASE + (6ull << 20) + 65536 + 4096;
    assert(snap_storage(Z, 256, true) != Z && nrs == RS_MAX && rs_tab[3].addr == Z && rs_tab[0].addr == a0);
    // 9. the game grows its heap into the ring: off, everything in place from then on
    heap_size = snap_base + 4096;
    assert(snap_storage(A, 32640, true) == A && snap_state == 0 && snap_ring_end() == 0);
    // 10. a reboot decides again (heap not set up yet: not decided; then on)
    snap_reset();
    heap_size = 0;
    assert(!snap_on() && snap_state == -1);
    heap_size = 8ull << 20;
    assert(snap_on() && snap_base == (8ull << 20) && commits == 2 && nrs == 0);
    puts("PASS: snapshots - probe / in place / copy, level and idle re-probes, exclusions, the two halves and their waits, per-frame pool reads without entries, GPU writes kept across a remake, eviction, heap growth, reboot");
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-overlap-') as temporary:
    temporary = Path(temporary)
    for name, harness in (('defer', defer_harness), ('decide', decide_harness), ('snap', snap_harness)):
        cfile = temporary / f'{name}.c'
        cfile.write_text(harness)
        exe = temporary / (f'{name}.exe' if os.name == 'nt' else name)
        subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
                        str(cfile), '-o', str(exe)], check=True)
        env = dict(os.environ)
        env.pop('LARECOMP_OVERLAP_WATCH', None)
        if name == 'decide':  # default on with tracking, off without (unless forced), =0 off
            env.pop('LARECOMP_GPU_OVERLAP', None)
            subprocess.run([str(exe), 'tracked', 'on'], check=True, timeout=10, env=env)
            subprocess.run([str(exe), 'untracked', 'off'], check=True, timeout=10, env=env)
            subprocess.run([str(exe), 'untracked', 'on'], check=True, timeout=10, env=dict(env, LARECOMP_GPU_OVERLAP='1'))
            subprocess.run([str(exe), 'tracked', 'off'], check=True, timeout=10, env=dict(env, LARECOMP_GPU_OVERLAP='0'))
            subprocess.run([str(exe), 'tracked', 'off'], check=True, timeout=10, env=dict(env, LARECOMP_OVERLAP_WATCH='0'))
            subprocess.run([str(exe), 'amd', 'off'], check=True, timeout=10, env=env)  # (AMD: off by default, #39)
            subprocess.run([str(exe), 'amd', 'on'], check=True, timeout=10, env=dict(env, LARECOMP_GPU_OVERLAP='1'))
            continue
        if name == 'snap':  # the hash probe (watch off), the write watch, no tracking
            subprocess.run([str(exe)], check=True, timeout=10, env=dict(env, LARECOMP_OVERLAP_WATCH='0'))
            subprocess.run([str(exe), 'watch'], check=True, timeout=10, env=env)
            subprocess.run([str(exe), 'notrack'], check=True, timeout=10, env=env)
        else:
            subprocess.run([str(exe)], check=True, timeout=10, env=env)
