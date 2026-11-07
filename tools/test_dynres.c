// Dynamic resolution's controller (runtime/nvn/output_scale.c) driven by a simulated GPU: no game files, no window.
// Build + run: tools/test_dynres.py (prints each case's factor path; exits non-zero when a case misbehaves).
// The GPU: the game's part G = (a + b f^2) x scene x noise at factor f (a: what doesn't scale with the resolution), the
// window step W (upscale / SMAA / menu: the window's size only). A frame takes G + W of GPU; with vsync and the
// frames waiting for the display (fifo, as on Windows' Intel driver) it lasts the next multiple of 16.7 ms, with
// mailbox max(16.7, G + W). Cases from an Intel UHD 770 (Field, Dynamic): G and W as measured at the factor it sat at.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/settings.h"
#include "../runtime/nvn/output_scale.h"

Settings g_settings = {.res_mode = RES_DYNAMIC};
bool g_gpu_integrated = true;
void pace_hitch(const char *why) { (void)why; }  // (output_scale.c's render scale changes: nvn/pace.c)
void pace_settings_changed(const char *what) { (void)what; }  // (output_scale.c's setting watch: nvn/pace.c)

static unsigned g_rng = 1;
static double rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return ((g_rng >> 8) & 0xffff) / 65535.0; }

typedef struct {
    const char *name;
    double a, b, w;     // G = a + b f^2, W
    int fifo;           // frames wait for the display
    double light_at;    // > 0: the scene gets lighter (x0.6) from then on (s)
    double secs;
    double wobble;      // the scene's load swings +-this over 20 s (0: 3%; frame to frame +-2%: the floor's 30.6 fps)
    double heavy_at, heavy, heavy_until;  // > 0: G x heavy from heavy_at (s) until heavy_until (0: the end)
    double wander;      // > 0: the scene's load wanders at random (walking around: +-this typical, over a few seconds)
    double lines;       // the window's height (0: 2160, 4K); the floor depends on it (1080: 0.50, 2160: 1/3)
    double clock_slow;  // > 0: the GPU clocks down while mostly idle (an iGPU): its times x up to this, at under 60% busy
    double resize_at, lines2;  // > 0: the window's height becomes lines2 then (fullscreen <-> windowed): G's scaling part
                               // and W follow the window's area
} Case;

typedef struct {
    double f_end, fps_end, hold_end;  // hold_end: the frame rate the controller says it holds (0: none)
    int changes_last60, hold_changes;  // hold_changes: the held rate changed (30 <-> 20 <-> 60), the whole run
    int changes_settled;  // factor changes after the first 20 s
    double back30_t;      // (heavy_until) when it held 30 again after the heavy stretch (0: never)
    double f_min_last60, f_max_last60;
    double hold_t;  // when the held rate last changed (s)
    // (resize_at) after the window's size changed: the factor 1.5 s later, its changes in the next 8 s, frames slower than
    // the frame rate before the change in the next 5 s, steps down and holds ended in the next 15 s (a swing)
    double f_after;
    int changes_after, slow_after, downs_after;
} Result;

static int near30(double h) { return fabs(h - 30) < 0.5; }
static Result run(const Case *c) {
    g_rng = 7;
    double now = 1.0, f = output_scale_dynamic_factor(), g_prev = -1, w_prev = -1;
    double frames_end = 0, t_end = 0, last_f = f, f_min = 1, f_max = 0, last_hold = 0, hold_t = 0;
    int changes = 0, hold_changes = 0, settled = 0;
    double back30 = 0;
    double walk = 0;  // (wander: the load's random part, log scale)
    double clk = 1;   // (clock_slow: the GPU's times x this - 1 at full clock)
    double f_after = -1, p_before = 0;
    int changes_after = 0, slow_after = 0, downs_after = 0;
    printf("%s\n  t=0s %.2f", c->name, f);
    int col = 0;
    while (now < c->secs) {
        OutScale o;
        double lines0 = c->lines > 0 ? c->lines : 2160;
        bool resized = c->resize_at > 0 && now > c->resize_at;
        double lines = resized ? c->lines2 : lines0, area = (lines / lines0) * (lines / lines0);
        output_scale(lines * 16 / 9, lines, false, now, g_prev, w_prev, &o);
        f = output_scale_dynamic_factor();
        if (resized && f_after < 0 && now > c->resize_at + 1.5) f_after = f;
        if (f != last_f) {
            printf(" | %.0fs %.2f", now, f);
            if (++col % 8 == 0) printf("\n ");
            if (now > c->secs - 60) changes++;
            if (now > 20) settled++;
            if (resized && now < c->resize_at + 8) changes_after++;
            if (resized && now > c->resize_at + 0.1 && now < c->resize_at + 15 && f < last_f) downs_after++;  // (not the change at the switch)
            last_f = f;
        }
        double hold = output_scale_dynamic_hold_fps();
        if (hold != last_hold) {
            printf(" | %.0fs hold %.0f", now, hold);
            if (++col % 8 == 0) printf("\n ");
            if (c->resize_at > 0 && now > c->resize_at && now < c->resize_at + 15 && hold == 0) downs_after++;
            hold_changes++, last_hold = hold, hold_t = now;
            if (c->heavy_until > 0 && now > c->heavy_until && near30(hold) && !back30) back30 = now;
        }
        if (now > c->secs - 60) { f_min = fmin(f_min, f); f_max = fmax(f_max, f); }
        double scene = (1 + (c->wobble > 0 ? c->wobble : 0.03) * sin(now * 2 * M_PI / 20)) * (c->light_at > 0 && now > c->light_at ? 0.6 : 1.0);
        if (c->heavy_at > 0 && now > c->heavy_at && (c->heavy_until <= 0 || now < c->heavy_until)) scene *= c->heavy;
        if (c->wander > 0) {  // mean-reverting over ~3 s, typical size c->wander
            double gauss = (rnd() + rnd() + rnd() + rnd() - 2) * 1.732, dt = 1 / 60.0;
            walk += -walk / 3.0 * dt + c->wander * sqrt(2 / 3.0) * sqrt(dt) * gauss;
            scene *= exp(walk);
        }
        double g = (c->a + c->b * f * f * area) * scene * (0.98 + 0.04 * rnd()) * clk, w = c->w * area * (0.99 + 0.02 * rnd()) * clk;
        double gw = g + w, p = c->fifo ? ceil(gw / 16.667 - 1e-9) * 16.667 : gw;
        if (c->clock_slow > 0) {  // under 60% busy: the clock drifts down (times up), over 80%: back up, over ~1 s
            double busy = gw / fmax(p, 16.667), target = busy < 0.6 ? c->clock_slow : busy > 0.8 ? 1.0 : clk;
            clk += (target - clk) * fmin(1.0, p / 1000.0);
        }
        if (p < 16.667) p = 16.667;
        if (c->resize_at > 0 && now > c->resize_at - 3 && now <= c->resize_at) p_before = fmax(p_before, p);
        if (resized && now < c->resize_at + 5 && p > p_before + 1) slow_after++;
        g_prev = g, w_prev = w;
        now += p / 1000;
        if (now > c->secs - 10) frames_end++, t_end += p / 1000;
    }
    Result r = {f, frames_end / t_end, last_hold, changes, hold_changes, f_min, f_max, hold_t, settled, back30, f_after,
                changes_after, slow_after, downs_after};
    printf("\n  end: factor %.2f, %.1f fps over the last 10 s (holding %.0f), %d change%s in the last 60 s (%.2f-%.2f), %d after 20 s\n",
           r.f_end, r.fps_end, r.hold_end, changes, changes == 1 ? "" : "s", f_min, f_max, settled);
    if (c->resize_at > 0)
        printf("  window %.0f -> %.0f lines at %.0f s: factor %.2f 1.5 s later, %d change%s in 8 s, %d slower frame%s in 5 s, "
               "%d step%s down / hold%s ended in 15 s\n", c->lines > 0 ? c->lines : 2160, c->lines2, c->resize_at, f_after,
               changes_after, changes_after == 1 ? "" : "s", slow_after, slow_after == 1 ? "" : "s", downs_after,
               downs_after == 1 ? "" : "s", downs_after == 1 ? "" : "s");
    printf("\n");
    return r;
}

static int g_fail;
static int near(double a, double b) { return fabs(a - b) < 0.5; }
static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n\n", what); g_fail = 1; }
}

int main(int argc, char **argv) {
    // (each case is its own process: the controller's state is static) argv[1]: the case's index
    static const Case cases[] = {
        // 4K window, bilinear: G 16.3 W 0.9 at 0.50 -> 30.6 fps (fifo halving)
        {"4K bilinear (G 16.3 + W 0.9 at 0.50, fifo)", 4.1, 48.8, 0.9, 1, 0, 180},
        // 4K, SMAA + FSR 1: G 13.1 W 20.2 at 0.50 -> 20 fps
        {"4K SMAA + FSR 1 (G 13.1 + W 20.2 at 0.50, fifo)", 3.3, 39.2, 20.2, 1, 0, 180},
        // 1080p, bilinear: G 10.9 W 0.2 at 0.65, hunting 0.65 <-> 0.75 before (the old raise needed G < 10.5, a step
        // down G + W > 14.5): G = 0.3 + 25.1 f^2 (10.9 at 0.65, 14.4 at 0.75), the load swinging +-5%
        {"1080p bilinear (G 10.9 + W 0.2 at 0.65, fifo)", 0.3, 25.1, 0.2, 1, 0, 180, 0.05, 0, 0, 0, 0, 1080},
        // 1080p, SMAA + FSR 1: G 9.1 W 8.1 at 0.50 -> 30 fps (G = 6.5 + 10.4 f^2: a large part that doesn't scale)
        {"1080p SMAA + FSR 1 (G 9.1 + W 8.1 at 0.50, fifo)", 6.5, 10.4, 8.1, 1, 0, 180, 0, 0, 0, 0, 0, 1080},
        // control: the frames don't wait (mailbox): every step down is a faster frame, so down to the floor as before
        {"4K bilinear, mailbox (G 16.3 + W 0.9 at 0.50)", 4.1, 48.8, 0.9, 0, 0, 120},
        // the scene gets lighter (x0.6) at 90 s: 60 fps fits at the floor again
        {"4K bilinear, lighter scene from 90 s (fifo)", 4.1, 48.8, 0.9, 1, 90, 240},
        // pure f^2 GPU (nothing fixed): the most optimistic model for steps down
        {"4K bilinear, G all f^2 (G 16.3 at 0.50, fifo)", 0, 65.2, 0.9, 1, 0, 180},
        // a fast GPU: 4K at 1.0 fits (G 11 + W 1)
        {"fast GPU, 4K (G 11 + W 1 at 1.00)", 2, 9, 1, 1, 0, 60},
        // a busier scene: its load swings +-15%: it follows the scene, the hold stays
        {"4K bilinear, load swinging +-15% (fifo)", 4.1, 48.8, 0.9, 1, 0, 180, 0.15},
        // 1080p bilinear with the load swinging +-15%: steps with the scene, slowly (the raise wait doubles)
        {"1080p bilinear, load swinging +-15% (fifo)", 0.3, 25.1, 0.2, 1, 0, 180, 0.15, 0, 0, 0, 0, 1080},
        // the game's own scaler off (it rendered 1280x720 here before): 1080p at 0.50 G 13.9-14.5 ms, 43.7 fps
        {"1080p bilinear, game scaler off (G 14.2 + W 0.2 at 0.50, fifo)", 3, 44.8, 0.2, 1, 0, 180, 0, 0, 0, 0, 0, 1080},
        // 4K, SMAA + FSR 1 with the game scaler off: G + W 44 ms at 0.50 (no reaction before: at the floor already)
        {"4K SMAA + FSR 1, game scaler off (G 23.8 + W 20.2 at 0.50, fifo)", 6, 71.2, 20.2, 1, 0, 180},
        // a hold whose G grows (UHD 770 4K bilinear: 19.4 -> 27.9 ms at 0.50 after holding 30; G mostly fixed here):
        // x1.44 from 60 s - the floor still fits 30
        {"4K bilinear, G x1.44 from 60 s (G 19.4 at 0.50, fifo)", 10, 37.6, 0.9, 1, 0, 180, 0, 60, 1.44},
        // a slower GPU holding 30 at the floor (G 18.2 there), G x1.8 from 60 s: not even the floor fits 30 any more
        // (frames every 50 ms) - the hold steps down to 20
        {"4K bilinear, held 30, G x1.8 from 60 s (fifo)", 14, 37.6, 0.9, 1, 0, 180, 0, 60, 1.8},
        // the same, x1.8 from 60 s to 150 s: 20, then back to 30 once the floor fits it with room
        {"4K bilinear, held 30, G x1.8 from 60 s to 150 s (fifo)", 14, 37.6, 0.9, 1, 0, 300, 0, 60, 1.8, 150},
        // x1.8 swinging +-15%: 20 fps, no hunting between 20 and 30
        {"4K bilinear, held 30, G x1.8 from 30 s, load swinging +-15% (fifo)", 14, 37.6, 0.9, 1, 0, 240, 0.15, 30, 1.8},
        // walking around, a fast GPU at the edge (5120x1440 class: G 13.6 + W 1 at 1.00, the load wandering +-10%):
        // it hunted here - 45 steps in 6 min before. A few changes at most
        {"edge at 1.00, load wandering +-10% (fifo)", 1.6, 12, 1.0, 1, 0, 360, 0.01, 0, 0, 0, 0.10},
        // the same edge lower down (G 13.2 at 0.80), wandering +-15%
        {"edge at 0.80, load wandering +-15% (fifo)", 1.5, 18.3, 0.8, 1, 0, 360, 0.01, 0, 0, 0, 0.15},
        // UHD 770, 1080p window (floor 0.50: G 10.5 there at full clock), a heavy stretch (x1.6, 100-104 s) pushes it into
        // the 30 fps hold; held, the GPU is half idle and clocks down (times x1.5: G ~15.9, as measured on Windows), so
        // the hold's own measure never shows 60 fitting again (it stayed at 30 for the rest of the run). It must probe 60
        {"1080p iGPU, clock drops while held at 30 (fifo)", 2, 34, 0.3, 1, 0, 300, 0, 100, 1.6, 104, 0, 1080, 1.5},
        // fullscreen <-> windowed (issue #205): the window's size changes at 90 s. Held at 30 in a 1080p screen (SMAA +
        // FSR 1, case 3) to a 1600x900 window: the same render height, the hold goes on - no trip to the floor and back
        {"1080p SMAA + FSR 1 held at 30, to a 900-line window at 90 s (fifo)", 6.5, 10.4, 8.1, 1, 0, 180, 0, 0, 0, 0, 0, 1080,
         0, 90, 900},
        // the same with a heavier window step (W 12 at 1080p): 60 is out of reach in the window too - the hold goes on
        {"1080p heavier window step held at 30, to a 900-line window at 90 s (fifo)", 6.5, 10.4, 12, 1, 0, 180, 0, 0, 0, 0, 0,
         1080, 0, 90, 900},
        // a 1080p window at 60 (case 2's GPU) made fullscreen on a 4K display: the factor takes the same render height
        // at once, the frames stay at 60 (before: 0.7 at 4K, 4x the pixels, until the slow frames stepped it down)
        {"1080p window at 60 to 4K fullscreen at 90 s (fifo)", 0.3, 25.1, 0.2, 1, 0, 180, 0.05, 0, 0, 0, 0, 1080, 0, 90, 2160},
        // 4K fullscreen at the floor (case 0) to a 1440-line window: the factor goes up to the same render height at
        // once (before: it stayed at 0.33 there for the raise wait, 720 lines of a 1440 window)
        {"4K at the floor to a 1440-line window at 90 s (fifo)", 4.1, 48.8, 0.9, 1, 0, 180, 0, 0, 0, 0, 0, 2160, 0, 90, 1440},
    };
    int n = sizeof cases / sizeof *cases;
    if (argc < 2) { printf("%d\n", n); return 0; }
    int i = atoi(argv[1]);
    Result r = run(&cases[i]);
    // (the floor is 1/3 since the game's scaler is off: the cases that held 30 at 0.50 reach 60 under it)
    switch (i) {
    // (the floor: 1/3 at 4K, 0.50 in a 1080p window - 540 lines)
    case 0: check(r.f_end >= 0.35 && r.fps_end > 58 && r.changes_last60 == 0, "4K bilinear: 60 under 0.50"); break;
    case 1: check(r.fps_end > 28 && r.changes_last60 <= 2, "4K FSR: 30 fps at the floor"); break;
    case 2: check(r.fps_end > 58 && r.changes_last60 == 0 && r.f_end >= 0.65, "1080p bilinear: settle at 60"); break;
    // (a 1080p window's floor is 0.50 now, 960x540: 60 is out of reach there, so it holds 30 - much sharper)
    case 3: check(near(r.hold_end, 30) && r.fps_end > 29 && r.changes_last60 <= 2, "1080p FSR: 30, sharper"); break;
    case 4: check(r.fps_end > 58 && r.f_end <= 0.45, "mailbox: 60 under 0.50"); break;
    case 5: check(r.fps_end > 58, "lighter scene: back to 60"); break;
    case 6: check(r.f_end >= 0.35 && r.fps_end > 58 && r.changes_last60 == 0, "4K bilinear f^2: 60 under 0.50"); break;
    case 7: check(r.f_end == 1.0 && r.fps_end > 58, "fast GPU: full resolution"); break;
    case 8: check(r.fps_end > 58 && r.changes_last60 <= 2, "4K bilinear +-15%: 60, steady"); break;
    case 9: check(r.fps_end > 58 && r.changes_last60 <= 2, "1080p bilinear +-15%: 60, few steps"); break;
    case 10: check(r.fps_end > 58 && r.changes_last60 == 0, "1080p game scaler off: 60"); break;
    case 11: check(near(r.hold_end, 20) && r.fps_end > 19 && r.changes_last60 <= 2 && r.hold_changes <= 2, "4K FSR 44 ms: hold 20"); break;
    case 12: check(near(r.hold_end, 30) && r.fps_end > 28 && r.changes_last60 <= 2, "G x1.44: hold 30 at a lower factor"); break;
    case 13: check(near(r.hold_end, 20) && r.fps_end > 19 && r.changes_last60 <= 2 && r.hold_changes <= 5, "G x2.2: hold steps down to 20"); break;
    case 14: check(near(r.hold_end, 30) && r.fps_end > 28 && r.changes_last60 <= 2 && r.back30_t > 0 && r.back30_t < 154,
                   "G x1.8 then back: 20, then 30 within a few seconds"); break;
    case 15: check(near(r.hold_end, 20) && r.fps_end > 19 && r.hold_changes <= 5, "G x2.2 +-15%: 20, no hunting"); break;
    case 16: check(r.changes_settled <= 4 && r.fps_end > 58, "edge at 1.00 wandering: a few changes in 6 min"); break;
    case 17: check(r.changes_settled <= 8 && r.fps_end > 58, "edge at 0.80 wandering: a few changes in 6 min"); break;
    case 18: check(r.fps_end > 58 && r.hold_end == 0 && r.hold_t < 150, "iGPU clock lock-in: back to 60 after the heavy stretch"); break;
    // (held 30 in a smaller window: the hold goes on - or 60 fits at the window's floor now, found by a probe)
    case 19: check(r.f_after >= 0.55 && r.slow_after == 0 && (near(r.hold_end, 30) ? r.changes_after <= 1 : r.fps_end > 58),
                   "held 30, to a smaller window: no swing"); break;
    case 20: check(r.downs_after == 0 && r.f_after >= 0.55 && r.slow_after == 0 && near(r.hold_end, 30),
                   "held 30 with 60 out of reach, to a smaller window: the hold goes on"); break;
    case 21: check(r.f_after <= 0.4 && r.slow_after <= 3 && r.fps_end > 58 && r.downs_after == 0,
                   "1080p window to 4K: the same render height, still 60"); break;
    case 22: check(r.f_after >= 0.5 && r.fps_end > 58, "4K floor to a 1440 window: the same render height at once"); break;
    }
    // a hold is real: the frames come at least at the rate it says
    if (r.hold_end > 0) check(r.fps_end >= 0.95 * r.hold_end, "the held frame rate is met");
    return g_fail;
}
// (ZA Recomp's output_scale.c: Pokémon Z-A's handheld image - Link's Awakening here)
#include <stdbool.h>
bool g_za_frame_hook;
bool am_handheld(void) { return false; }
