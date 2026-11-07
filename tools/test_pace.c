// Frame pacing (runtime/nvn/pace.c) replayed against simulated frames: no game files, no window.
// Build + run: tools/test_pace.py (prints each case's holds and returns; exits non-zero when a case misbehaves).
// A frame: the game thread works cpu ms (twice that in the game's 30 fps mode: its second world step), the GPU
// takes gpu ms (pipelined: the frame is ready at the later of the two), then it is shown at the next vsync (60 Hz). Held,
// pace.c's own cadence waits out the second vsync. Time is simulated (plat_now_ns / plat_sleep_until_ns), so a case
// of minutes runs in milliseconds.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/pace.h"

// ---- what pace.c reads
#define VSYNC_NS 16666667ull
static uint64_t t_ns = 1000000000ull, cpu_ns, gpu_total_ns;
__thread uint64_t g_blocked_ns, g_rq_wait_ns, g_rq_lock_ns, g_submit_ns, g_gpu_wait_ns, g_present_ns, g_hooks_ns;
volatile uint64_t g_vsync_count, g_vsync_ns;
uint64_t pace_test_game_mgr;
static uint8_t game_mgr[0x500];
static uint64_t zone;
static void vsyncs(void) { g_vsync_count = t_ns / VSYNC_NS, g_vsync_ns = g_vsync_count * VSYNC_NS; }
uint64_t plat_now_ns(void) { return t_ns; }
void plat_sleep_until_ns(uint64_t t) { if (t > t_ns) t_ns = t; vsyncs(); }
uint64_t plat_thread_cpu_ns(void) { return cpu_ns; }
uint64_t rq_render_cpu_ns(void) { return 0; }
int64_t pace_gpu_ns(void) { return (int64_t)gpu_total_ns; }
int game_in_play(void) { return 1; }
bool world_on_screen(void) { return true; }
const char *game_level_name(void) { return "Field"; }
uint64_t game_camera_zone(void) { return zone; }
void slow_frame_present(uint64_t now, uint64_t gcpu) { (void)now, (void)gcpu; }

// ---- cases
// cpu: the game thread's CPU time; wait: its waits that count as busy (for the render thread, the GPU's previous
// frame: the play log's machine was ~13 ms busy a frame with 3-6 ms of CPU); stall: a one-off busy stall (ms)
typedef struct { double cpu, gpu, stall, wait; } Frame;
typedef struct {
    const char *name;
    double secs;
    void (*frame)(double t, int n, Frame *f);  // this frame's work (t: seconds in, n: frame number)
    int (*check)(int holds, int backs, double held_s, double last_change_t, bool held_end, double secs);
} Case;

static uint64_t rng = 9;
static double noise(double amp) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return ((rng >> 33) / (double)(1ull << 31) * 2 - 1) * amp; }

// The play log's machine (M3 Max at 5120x1440, MetalFX): game CPU 3-6 ms, GPU 10-13 ms a frame. Every ~6 s a
// hitch, then frames shown a vsync late in a burst (the GPU catching up): a door jump (signalled, 165 ms frame),
// a save commit (signalled, no long frame), a ~1 s texture hash stall, a 50 ms stall nothing signals (an item
// spawn somewhere else, a shader), and a resolution step.
static int burst_left;
static void user_hitches(double t, int n, Frame *f) {
    f->cpu = 4.5 + noise(1.5), f->gpu = 11.5 + noise(1.5), f->stall = 0, f->wait = 8.5;
    static int last_k = -1;
    int k = (int)(t / 6.0);
    if (t > 3 && k != last_k) {
        last_k = k;
        switch (k % 5) {
        case 0: pace_hitch("a door jump"); f->stall = 165; break;
        case 1: pace_hitch("a save commit"); break;
        case 2: f->stall = 1000; break;  // (texture hashes: a 1 s frame, itself a hitch)
        case 3: f->stall = 50; break;    // (nothing signals it: only the spread rule keeps it out)
        case 4: pace_hitch("a render scale change"); break;
        }
        burst_left = 14;
    }
    if (burst_left > 0) burst_left--, f->gpu = 24 + noise(4);
}
// The same machine held at 30 by a real heavy stretch first (GPU 20 ms for 4 s), then the 10-13 ms of the log:
// back to 60 (the old 10 ms limit kept it at 30 here).
static void user_release(double t, int n, Frame *f) {
    f->cpu = 4.5 + noise(1.5), f->stall = 0, f->wait = 8.5;
    f->gpu = t < 4 ? 20 + noise(1) : 12 + noise(1);
    zone = t < 4 ? 1 : 2;
}
// #310: a GPU-bound machine at 20 ms a frame (shown late every frame, short busy time): held, and stays held.
static void gpu20(double t, int n, Frame *f) { f->cpu = 4 + noise(1), f->gpu = 20 + noise(1), f->stall = 0; }
// The same with hitches every 2 s (door jumps, saves): still held.
static void gpu20_hitches(double t, int n, Frame *f) {
    gpu20(t, n, f);
    static int last_k = -1;
    int k = (int)(t / 2.0);
    if (k != last_k) last_k = k, pace_hitch(k & 1 ? "a save commit" : "a door jump"), f->stall = k % 3 ? 0 : 150;
}
// #40: a CPU-bound machine at 20 ms busy a frame: held, and stays held.
static void cpu20(double t, int n, Frame *f) { f->cpu = 20 + noise(2), f->gpu = 8 + noise(1), f->stall = 0; }
// A machine at the edge: GPU 12 ms in quiet areas, 18 ms in heavy ones, 8 s each. May try 60 once, then must
// settle at 30 (backing off: the 10 ms limit) without flipping.
static void edge(double t, int n, Frame *f) {
    int heavy = (int)(t / 8.0) & 1;
    zone = (uint64_t)(t / 8.0) + 1;
    f->cpu = 4.5 + noise(1), f->gpu = (heavy ? 18 : 12) + noise(1), f->stall = 0, f->wait = 8;
}
// Backoff built up first: held at GPU 20 ms (zone 1), back to 60 at GPU 9 ms (zone 2), held again within 20 s (zone 3:
// backing off, the 10 ms GPU limit). user_res_change: at 25 s the user lowers the resolution (the setting, and the
// render scale change it makes), GPU 11.5 ms after: back at 60 within a few seconds (it stayed at 30 outdoors: 11.5 ms
// is over the backoff's 10 ms). dynres_steps: Dynamic's own steps instead (a render scale change every 2 s, GPU 18 /
// 16 ms), then 11.5 ms from 25 s: the backoff stays, and so does the hold.
static void backoff_phases(double t, Frame *f) {
    f->cpu = 4.5 + noise(1), f->stall = 0, f->wait = 8;
    f->gpu = (t < 4 ? 20 : t < 10 ? 9 : 20) + noise(1);
    zone = t < 4 ? 1 : t < 10 ? 2 : 3;
}
static void user_res_change(double t, int n, Frame *f) {
    backoff_phases(t, f);
    static bool done;
    if (t >= 25 && !done) done = true, pace_settings_changed("resolution"), pace_hitch("a render scale change"), f->stall = 120;
    if (t >= 25) f->gpu = 11.5 + noise(1);
}
static void dynres_steps(double t, int n, Frame *f) {
    backoff_phases(t, f);
    static int last_k = -1;
    int k = (int)(t / 2.0);
    if (t >= 14 && t < 25) {
        if (k != last_k) last_k = k, pace_hitch("a render scale change");
        f->gpu = (k & 1 ? 16 : 18) + noise(1);
    }
    if (t >= 25) f->gpu = 11.5 + noise(1);
}

// Fits 60 with no hitches: never held.
static void steady(double t, int n, Frame *f) { f->cpu = 5 + noise(1), f->gpu = 11 + noise(1.5), f->stall = 0, f->wait = 8; }

static int never_held(int holds, int backs, double held_s, double lc, bool end, double secs) { return holds == 0; }
static int held_after(int holds, int backs, double held_s, double lc, bool end, double secs) {
    return holds == 1 && backs == 0 && end && held_s > secs - 5;  // (held within a few seconds and for the rest)
}
static int released(int holds, int backs, double held_s, double lc, bool end, double secs) {
    return holds == 1 && backs == 1 && !end && lc < 12;
}
static int settles(int holds, int backs, double held_s, double lc, bool end, double secs) {
    return holds <= 2 && backs <= 1 && end && lc < secs / 2;  // (at most one try at 60, then 30 for good)
}

static int res_released(int holds, int backs, double held_s, double lc, bool end, double secs) {
    return holds == 2 && backs == 2 && !end && lc >= 25 && lc < 28;  // (back at 60 within a few seconds of the change)
}
static int backoff_kept(int holds, int backs, double held_s, double lc, bool end, double secs) {
    return holds == 2 && backs == 1 && end && lc < 25;  // (held again, and still held after the steps and the light part)
}

static const Case cases[] = {
    {"steady: GPU 11 ms, CPU 5 ms (fits 60)", 60, steady, never_held},
    {"play log hitches: GPU 10-13 ms, door jump / save / texture hash / unsignalled stall / scale step + late bursts",
     120, user_hitches, never_held},
    {"play log release: held by 4 s at GPU 20 ms, then GPU 12 ms", 30, user_release, released},
    {"#310: GPU 20 ms a frame, sustained", 60, gpu20, held_after},
    {"#310 with hitches: GPU 20 ms, a door jump or save every 2 s", 60, gpu20_hitches, held_after},
    {"#40: CPU 20 ms busy a frame, sustained", 60, cpu20, held_after},
    {"edge: GPU 12 ms quiet / 18 ms heavy, 8 s each", 120, edge, settles},
    {"user resolution change: held backing off at GPU 20 ms, then the setting lowered to GPU 11.5 ms", 40, user_res_change,
     res_released},
    {"Dynamic's own steps: held backing off, scale steps at GPU 16-18 ms, then GPU 11.5 ms (backoff kept)", 45,
     dynres_steps, backoff_kept},
};

static int run(const Case *c) {
    memset(game_mgr, 0, sizeof game_mgr);
    game_mgr[0x4e3] = 1;  // (the game allows its 30 fps mode)
    pace_test_game_mgr = (uint64_t)(uintptr_t)game_mgr;
    rng = 9, zone = 0;
    vsyncs();
    int holds = 0, backs = 0, n = 0;
    double start = t_ns / 1e9, held_since = -1, held_s = 0, last_change = 0;
    bool was = false, game30 = false;
    uint64_t last_shown = t_ns;
    printf("%s\n ", c->name);
    while (t_ns / 1e9 - start < c->secs) {
        double t = t_ns / 1e9 - start;
        Frame f = {0};
        c->frame(t, n++, &f);
        bool held = game_mgr[0x4e2] != 0;
        // (held, or the game's own 30 fps mode after a frame that missed a vsync: its second world step)
        double cpu = f.cpu * (held || game30 ? 2 : 1) + f.stall;
        uint64_t t0 = t_ns;
        cpu_ns += (uint64_t)(cpu * 1e6);
        if (!held && !game30) cpu += f.wait;  // (at 30 the GPU's last frame is long done: logged held, 3-6 ms busy)
        t_ns += (uint64_t)(cpu * 1e6);
        uint64_t ready = t0 + (uint64_t)((f.gpu > cpu ? f.gpu : cpu) * 1e6);
        gpu_total_ns += (uint64_t)(f.gpu * 1e6);
        uint64_t shown = (ready / VSYNC_NS + 1) * VSYNC_NS;  // (the next vsync: waited for idle)
        g_blocked_ns += shown - t_ns;
        game30 = shown - last_shown > VSYNC_NS + VSYNC_NS / 2;
        t_ns = last_shown = shown;
        vsyncs();
        pace_frame();
        bool now_held = pace_is_slow();
        if (now_held != was) {
            printf(" %s at %.1fs", now_held ? "held" : "back", t), last_change = t;
            if (now_held) holds++, held_since = t;
            else backs++, held_s += t - held_since;
            was = now_held;
        }
    }
    if (was) held_s += c->secs - held_since;
    int ok = c->check(holds, backs, held_s, last_change, was, c->secs);
    printf("%s\n  %s: %d hold%s, %d return%s to 60, %.0f s held of %.0f\n", holds + backs ? "" : " (no change)",
           ok ? "ok" : "FAILED", holds, holds == 1 ? "" : "s", backs, backs == 1 ? "" : "s", held_s, c->secs);
    return ok;
}

int main(int argc, char **argv) {
    int ncases = (int)(sizeof cases / sizeof *cases);
    if (argc < 2) { printf("%d\n", ncases); return 0; }  // (the runner: each case in its own process - pace.c's state)
    int i = atoi(argv[1]);
    if (i < 0 || i >= ncases) return 2;
    return run(&cases[i]) ? 0 : 1;
}
// (ZA Recomp's pace.c asks which game runs: Link's Awakening here)
int g_game_is_la = 1;
