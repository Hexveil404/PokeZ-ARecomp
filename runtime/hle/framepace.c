// Frame pacing: the game's speed doesn't depend on the frame rate.
// The game advances its world each frame by its frame period (16.67 ms) times the swap interval its frame timer
// (f_700000c910) reads from the system object - normally 1. When a frame takes longer than a vsync (GPU-bound at
// 32:9, a hitch), the next frame starts a vsync later but still advanced only 16.67 ms: slow motion (30 fps = half
// speed). tools/patches.py FRAME_STEPS_SITE routes that read here: we return how many vsyncs really passed since
// the last frame, so the world advances by the time that passed. The game's own waiting is unaffected (it reads
// the interval nowhere else). Shared by the macOS and Windows builds.
// FINDING (2026-10-09): forcing 2 steps did NOT change Link's walking speed - gameplay is a true fixed step per
// frame; this timer feeds something else. Kept for research, off by default. (2026-10-18: the read is the game's
// dynamic resolution's frame budget - tools/patches.py GAME_DYNRES_SITE, now off, so it no longer runs at all.)
//   LARECOMP_FRAMEPACE=1     hand the timer the vsyncs elapsed (default: the game's own value)
//   LARECOMP_FRAME_STEPS_MAX maximum steps per frame (default 4: after a long stall the game doesn't jump ahead)
//   LARECOMP_FORCE_STEPS=<n> (test) always n
#include <stdlib.h>

#include "../hle.h"

extern volatile uint64_t g_vsync_count;  // runtime/hle/vi.c
uint64_t g_frame_steps_late;             // (stats) frames that advanced more than one step

uint32_t rt_frame_steps(uint32_t interval) {
    static int on = -1, max_steps, forced;
    if (on < 0) {
        const char *e = ENV("LARECOMP_FRAMEPACE");
        on = e ? atoi(e) != 0 : 0;  // off by default: the world (Link's movement) doesn't use this step (tested)
        e = ENV("LARECOMP_FRAME_STEPS_MAX");
        max_steps = e ? atoi(e) : 4;
        if (max_steps < 1) max_steps = 1;
        e = ENV("LARECOMP_FORCE_STEPS");
        forced = e ? atoi(e) : 0;
    }
    static int ncall;
    if (ENV("LARECOMP_LOG_FRAMEPACE") && ncall++ % 120 == 0) LOG("[framepace] call %d interval %u vsync %llu\n", ncall, interval, (unsigned long long)g_vsync_count);
    if (forced > 0) return (uint32_t)forced;
    static uint64_t last;
    uint64_t now = __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED);
    uint64_t steps = last ? now - last : 1;
    last = now;
    if (!on) return interval;
    if (steps < interval) steps = interval;  // (the game's own 30 fps mode: it waits that many vsyncs anyway)
    if (steps < 1) steps = 1;
    if (steps > (uint64_t)max_steps) steps = (uint64_t)max_steps;
    if (steps > 1) g_frame_steps_late++;
    return (uint32_t)steps;
}

// Pokémon Z-A's frame mode (tools/patches.py ZA_BUILDS): its configured vsyncs per frame - 1 (1: 30 fps). With
// the fps60 setting (or LARECOMP_FPS=60) 0: it presents every vsync and steps its world 1/60 s a frame. A frame
// that takes longer still steps 1/60 s: below 60 fps the whole game moves in slow motion (a player's log: 35 fps
// = 58% speed; the CPU-side game code couldn't keep up, an RTX 4060 waiting half the time). So the setting backs off
// by itself: presented below FPS60_MIN for FPS60_SLOW_S seconds in a row (after FPS60_WARM_S to settle - shaders
// building right after the switch), the game goes back to 30 fps for the rest of the session and the overlay says so
// (menu_notice). Choosing 60 again in F1 tries again. LARECOMP_FPS=30: as is; LARECOMP_FPS=60 or F1's "60 fps (always)"
// (fps60_lock): 60, no backing off - for players who'd rather have some slow motion than 30 fps.
// Seconds spent building shaders count neither way (#22: an RX 9070 XT with a cold pipeline cache had three 1-2 s
// shader stalls in a battle and went back to 30 for the session, holding 60 otherwise).
#include "../settings.h"
#include "../platform.h"
#define FPS60_MIN 50
#define FPS60_SLOW_S 5
#define FPS60_WARM_S 5
bool g_za_frame_hook;    // (the game is Pokémon Z-A: exec.c leaves its GPU timing counters alone)
bool g_fps60_fell_back;  // (60 fps chosen, but this session went back to 30: runtime/ui/menu.mm says so)
static bool fps60_holds(void) {
    extern uint64_t renderer_present_count(void);
    extern void menu_notice(const char *text, int seconds);
    static int last_setting = -1, warm, slow;
    static uint64_t t0, p0;
    int setting = g_settings.fps60 | g_settings.fps60_lock << 1;
    if (last_setting != setting) {  // (chosen again in F1: a new try)
        last_setting = setting;
        g_fps60_fell_back = false, warm = slow = 0, t0 = 0;
    }
    if (!g_settings.fps60 || g_fps60_fell_back) return false;
    if (g_settings.fps60_lock) return true;
    uint64_t now = plat_now_ns(), pc = renderer_present_count();
    if (!t0) { t0 = now, p0 = pc; return true; }
    if (now - t0 < 1000000000ull) return true;
    double fps = (double)(pc - p0) * 1e9 / (double)(now - t0);
    t0 = now, p0 = pc;
    if (warm < FPS60_WARM_S) { warm++; return true; }
    static double min = -1;  // LARECOMP_FPS60_MIN: another floor (tests: 59 makes a busy scene back off)
    if (min < 0) min = ENV("LARECOMP_FPS60_MIN") ? atof(ENV("LARECOMP_FPS60_MIN")) : FPS60_MIN;
    extern uint64_t renderer_shader_builds(void);  // (the renderer: builds done, top bit = one being built now)
    static uint64_t b0;
    uint64_t b = renderer_shader_builds();
    bool building = (b >> 63) || (b & ~(1ull << 63)) - (b0 & ~(1ull << 63)) >= 2;  // (one now, or a few this second)
    b0 = b;
    if (!building) slow = fps < min ? slow + 1 : 0;
    if (slow < FPS60_SLOW_S) return true;
    g_fps60_fell_back = true;
    LOG("[pace] 60 fps not held (%.0f fps for %d s): back to 30 fps - below 60 the game moves in slow motion\n", fps, slow);
    menu_notice("60 fps turned off: this PC didn't keep 60 fps, and below that the game moves in slow motion. "
                "Back to 30 fps (choose 60 again in F1 to retry).", 8);
    return false;
}
// ---- Pokémon Z-A: the world step decoupled from the frame rate (tools/za_patches.py FRAME_DT)
// The game's frame step (the frame clock, at its `ldr x8, [x1, #8]`) is the number of 1/60 s vsyncs the frame
// spanned, read from a duration: 16.7 ms -> 0 (one tick), 33.3 -> 1 (two ticks), ... At 57 fps a 17.5 ms frame still
// counts one tick, so the world runs at ~95 % speed: slow motion everywhere below the chosen rate. And the duration
// it hands us isn't its frame period at all: measured at a steady 60 fps it is ~8.5 ms (its CPU-busy time), so its
// own catch-up never fires here. tools/za_patches.py routes that read here: the duration we report comes from a
// fixed-timestep accumulator of the real time between calls - the step is the whole vsyncs owed, the fraction kept
// for the frames after - so the simulation tracks wall-clock time at any frame rate (and still lands exactly on 60 /
// 30). F1 > Constant game speed (the frame_dt setting, on by default; LARECOMP_FRAME_DT=0 / 1 overrides it at load):
// off hands the game its own duration, as before.
//   LARECOMP_FRAME_DT_FORCE=<n> (test) always hand the game n (its step is n + 1 ticks)
//   LARECOMP_FRAME_DT_LOG=1    the first 20 calls and every 60th: the time measured and the step handed on
static uint32_t za_step_n;  // (the game thread) this frame's accumulator step (rt_za_frame_step hands it on)
static bool za_dyn30, za_dyn30_prev;  // (rt_za_frame_cfg: this frame / the one before ran in the game's 30 fps mode)
uint64_t rt_za_frame_dt(uint64_t real_ns) {
    static uint64_t acc, last_ns;
    static bool was_on;
    if (!g_settings.frame_dt) { was_on = false, za_step_n = 0; return real_ns; }
    uint64_t now = plat_now_ns();
    if (!was_on) { was_on = true, last_ns = now, acc = 0; }  // (turned on: nothing owed from before)
    uint64_t dt = now - last_ns;
    last_ns = now;
    // A stall over 100 ms (a load, a shader build) doesn't jump the world: the excess is dropped, not kept - kept, it
    // fast-forwarded the world at 4 steps a frame after every load
    if (dt > 100000000ull) dt = 100000000ull;
    const uint64_t step = 16666667ull;  // (the game's fixed step: 1/60 s)
    // The game's own bands, by its strict `>` thresholds (16,666,667 / 33,333,366 / 50,000,000 / 66,666,667 ns): a
    // duration of band[n] reads as n
    static const uint64_t band[5] = {0, 16666668ull, 33333367ull, 50000001ull, 66666668ull};
    static int force = -2;
    if (force == -2) { const char *e = ENV("LARECOMP_FRAME_DT_FORCE"); force = e ? atoi(e) : -1; }
    uint64_t n;
    if (force >= 0) n = (uint64_t)(force > 4 ? 4 : force);
    else {
        acc += dt;
        uint64_t owed = acc / step;  // whole 1/60 s vsyncs owed since the last call
        acc -= owed * step;          // (the remainder is under one step)
        // The subsystems' step is this value + 1 (their "0" is one tick: the 30 fps mode's configured 1 gives two), so
        // the vsyncs owed less one: 60 fps -> 0 (one tick), 40 fps -> 0, 1, 0, 1 (1.5: full speed), 30 fps -> 1
        n = owed ? owed - 1 : 0;
        if (n > 4) n = 4;  // (the game's own most: a hitch steps at most 4)
    }
    static int log = -1;
    if (log < 0) log = ENV("LARECOMP_FRAME_DT_LOG") != NULL;
    if (log) {
        static int nc;
        if (nc++ < 20 || nc % 60 == 0)
            LOG("[framedt] call %d dt %llu ns (game %llu) -> step %llu (%s)\n", nc, (unsigned long long)dt,
                (unsigned long long)real_ns, (unsigned long long)n, force >= 0 ? "forced" : "accumulated");
    }
    za_step_n = (uint32_t)n;
    return band[n];
}

// The step each clock channel is handed (the same function's update loop, `mov w2, w20`: tools/za_patches.py
// FRAME_STEP, 2.0.2). The duration above only reaches the game in its 30 fps mode: in its 60 fps mode the frame clock
// takes its configured step (0) and ignores the measured one, so the world stepped 1/60 s a frame and slowed down
// below 60 fps. Here the channels get max(the game's step, the accumulator's), the frame clock's own choice and mode
// untouched (forcing its measured-duration flag instead flipped its 60 / 30 controller and doubled the speed).
// The loop's "subsystems" are the game's clock channels (11 in 2.0.2): each update call only stores its channel's
// step, and what reads a channel runs later. Channel 0 keeps the game's own step in its 60 fps mode: given two ticks
// there - which that mode never hands a channel - it left the player stuck after a roll over changing ground height,
// roll / crouch / walk refused for good and turning still allowed (Steam Deck at ~48 fps). Found by bisecting the
// channels on a save that got stuck within a minute: channel 0 alone on one tick - never stuck, the world at full
// speed; channel 0 alone on two ticks - stuck, and slow (the world's speed is in the other channels). The 30 fps mode
// hands every channel two ticks itself and keeps the catch-up on all of them. Channel 0 is the index the bisect found
// (the channels' order isn't known from the code - the count is logged once, "[pace] clock channels: 11").
//   LARECOMP_FRAME_STEP=0              the game's step on every channel (the 60 fps mode slows down below 60 again)
//   LARECOMP_FRAME_STEP_KEEP=<i>,<j>.. the channels that keep the game's step (default 0; "none": every one takes the
//                                      accumulator's)
static bool keeps_game_step(uint32_t channel) {
    static int init;
    static uint64_t mask = 1;  // (channel 0)
    if (!init) {
        init = 1;
        const char *e = ENV("LARECOMP_FRAME_STEP_KEEP");
        if (e && *e) {  // (set but empty: the default)
            mask = 0;
            for (const char *p = e; *p;) {
                char *end;
                unsigned long v = strtoul(p, &end, 10);
                if (end == p) break;
                if (v < 64) mask |= 1ull << v;
                p = *end ? end + 1 : end;
            }
            LOG("[pace] clock channels keeping the game's step: %s\n", e);
        }
    }
    return channel < 64 && (mask >> channel & 1);
}
uint32_t rt_za_frame_step(uint32_t game_step, uint32_t channel) {
    static int on = -1;
    if (on < 0) { const char *e = ENV("LARECOMP_FRAME_STEP"); on = !e || atoi(e) != 0; }
    static uint32_t last_channel;
    static bool counted;
    if (!counted && channel == 0 && last_channel > 0) counted = true, LOG("[pace] clock channels: %u\n", last_channel + 1);
    last_channel = channel;
    if (!on || !g_settings.frame_dt || (game_step == 0 && keeps_game_step(channel))) return game_step;
    // The per-frame mode (rt_za_frame_cfg, below) brought the stuck-after-a-roll back: the player's code takes the
    // game's frame mode a frame late, so the first frame run in the 30 fps mode after a 60 fps one handed channel 0 two
    // ticks while that code still went by the 60 fps mode - the mismatch above. That frame keeps channel 0 on one tick
    // (LARECOMP_DYN_CH0=2, the default). Tested on the Deck: never stuck, the banners still clear. =0: two ticks there
    // (stuck again); =1: one tick on every frame in the 30 fps mode
    static int ch0 = -1;
    if (ch0 < 0) ch0 = ENV("LARECOMP_DYN_CH0") ? atoi(ENV("LARECOMP_DYN_CH0")) : 2;
    if (channel == 0 && keeps_game_step(0) && za_dyn30 && (ch0 == 1 || (ch0 == 2 && !za_dyn30_prev))) return 0;
    return za_step_n > game_step ? za_step_n : game_step;
}

// (60 fps with frame_dt; LARECOMP_FRAME_MODE_DYN, default 1) A frame the accumulator says is behind (rt_za_frame_dt's
// step 1+, read earlier in the same function) runs in the game's own 30 fps mode, the rest in its 60 fps mode. The
// per-channel catch-up (rt_za_frame_step) alone gave channel 1 - the world's speed - two ticks while the game believed
// it was in its 60 fps mode, and its UI timelines then never finished: the ambush's "ADVANTAGE / Now's your chance!"
// stayed on screen, the Victory banner cut off without its fade (Steam Deck at ~47 fps: channel 1 by bisect; the game's
// own 30 fps mode, two ticks on every channel, showed both right). In its 30 fps mode the game hands every channel
// max(1, the step) itself, as it does on the Switch. Its present interval of 2 on those frames is held at 1
// (g_za_mode_dyn: runtime/nvn/objects.c present_pace) - the accumulator, not the interval, keeps the speed.
// LARECOMP_FRAME_MODE_DYN=0: off (the 60 fps mode on every frame); =2 keeps the game's interval on those frames (a
// frame that moved the world two ticks stays on screen two vsyncs; no visible difference on the Deck).
bool g_za_mode_dyn, g_za_mode_dyn_interval;
int32_t rt_za_frame_cfg(int32_t cfg) {
    g_za_frame_hook = true;
    const char *e = ENV("LARECOMP_FPS");
    bool on = e ? atoi(e) >= 60 : fps60_holds();
    static int dyn = -1;
    if (dyn < 0) dyn = ENV("LARECOMP_FRAME_MODE_DYN") ? atoi(ENV("LARECOMP_FRAME_MODE_DYN")) : 1;
    g_za_mode_dyn = dyn && on && g_settings.frame_dt && cfg > 0;
    g_za_mode_dyn_interval = g_za_mode_dyn && dyn == 2;
    static int logged = -1;
    if (logged != (int)on + 2 * g_za_mode_dyn) {
        logged = (int)on + 2 * g_za_mode_dyn;
        LOG("[pace] game frame mode %d -> %d (%s)\n", cfg, on ? 0 : cfg,
            g_za_mode_dyn ? (g_za_mode_dyn_interval ? "60 fps, its 30 fps mode and interval on frames that are behind"
                                                    : "60 fps, its 30 fps mode on frames that are behind")
                          : on ? "60 fps" : "as is");
    }
    bool behind = g_za_mode_dyn && za_step_n >= 1;
    za_dyn30_prev = za_dyn30, za_dyn30 = behind;
    if (behind) {
        static uint64_t n30;
        if (++n30 % 600 == 1) LOG("[pace] frames in the game's 30 fps mode (behind): %llu so far\n", (unsigned long long)n30);
    }
    int32_t mode = behind ? cfg : on ? 0 : cfg;
    {   // the game's own time, in vsyncs: each frame it steps max(mode + 1, the vsyncs that really passed) - a replay
        // keyed by this lands its inputs on the same moment of the game on a faster or slower PC (runtime/hle/input.c)
        extern uint64_t g_game_vsyncs;
        static uint64_t last;
        uint64_t now = __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED), passed = last ? now - last : 0;
        uint64_t step = (uint64_t)mode + 1;
        if (passed > step) step = passed > 8 ? 8 : passed;  // (a long stall: the game caps its step too)
        last = now;
        g_game_vsyncs += step;
    }
    return mode;
}
uint64_t g_game_vsyncs;  // (see rt_za_frame_cfg)
