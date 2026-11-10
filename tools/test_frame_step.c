// Constant game speed (runtime/hle/framepace.c) against simulated frames, as the game's frame clock calls it each frame:
// rt_za_frame_dt (the frame's duration -> the world's step), rt_za_frame_cfg (60 / 30 fps mode), then
// rt_za_frame_step for each of its 11 clock channels. #179 #188:
//  - below 60 fps (the mode flipping frame by frame) channel 0 - the player's - gets the world's ticks (channel 1's):
//    v0.1.20 held one back at every switch into the 30 fps mode and never gave it back (16% slow at ~47 fps);
//  - the first 30 fps frame after a 60 fps one still gives channel 0 one tick (two left the player stuck after a roll);
//  - game code without the per-channel hook never runs the per-frame mode (1.0.0 / 2.0.0 before the recompile).
// Built by tools/test_frame_step.py; no game data.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/hle/framepace.c"

// ---- the host around it
static uint64_t now_ns = 1000000000ull;
uint64_t plat_now_ns(void) { return now_ns; }
void plat_sleep_until_ns(uint64_t t) { if (t > now_ns) now_ns = t; }
volatile uint64_t g_vsync_count;
bool g_fps60_fell_back;
Settings g_settings;
uint64_t renderer_present_count(void) { return 0; }
uint64_t renderer_shader_builds(void) { return 0; }
void menu_notice(const char *text, int seconds) { (void)text; (void)seconds; }

static int frames_30;  // (frames run in the game's 30 fps mode)
// One game frame of ms milliseconds: the frame clock's three calls; ticks[] += what each channel was handed (+1: a
// channel's step n is n + 1 ticks). hook: the game code has the per-channel call (FRAME_STEP).
static void frame(double ms, bool hook, uint64_t ticks[11], int *first_30_ch0) {
    static bool prev30;
    now_ns += (uint64_t)(ms * 1e6);
    g_vsync_count += (uint64_t)(ms / 16.6667 + 0.5);
    rt_za_frame_dt(16666667);
    int32_t mode = rt_za_frame_cfg(1);  // (the game's configured 30 fps mode: 1)
    bool in30 = mode == 1;
    frames_30 += in30;
    for (uint32_t ch = 0; ch < 11; ch++) {
        uint32_t game_step = in30 ? 1 : 0;  // (what the game itself hands a channel in that mode)
        uint32_t s = hook ? rt_za_frame_step(game_step, ch) : game_step;
        ticks[ch] += s + 1;
        if (ch == 0 && in30 && !prev30 && first_30_ch0) *first_30_ch0 = s + 1 > *first_30_ch0 ? (int)(s + 1) : *first_30_ch0;
    }
    prev30 = in30;
}

int main(int argc, char **argv) {
    setenv("LARECOMP_FPS", "60", 1);
    g_settings.frame_dt = true, g_settings.fps60 = true, g_settings.fps60_lock = true;
    int which = argc > 1 ? atoi(argv[1]) : 1;
    uint64_t t[11] = {0};
    int first30 = 0;
    if (which == 1) {  // ~45 fps for 3000 frames with the hook: the mode flips, channel 0 keeps the world's pace
        for (int i = 0; i < 3000; i++) frame(22.2, true, t, &first30);
        double drift = (double)t[0] / (double)t[1], tps = (double)t[1] / 66.6;  // (3000 x 22.2 ms of game time)
        printf("hook, ~45 fps: %d of 3000 frames in the 30 fps mode; ticks channel 0 %llu, channel 1 %llu (%.3f); the "
               "first 30 fps frame gives channel 0 %d tick(s); world %.1f ticks/s\n", frames_30, (unsigned long long)t[0],
               (unsigned long long)t[1], drift, first30, tps);
        assert(frames_30 > 300);               // (the per-frame mode ran)
        assert(drift > 0.97 && drift < 1.03);  // (v0.1.20's rule: ~0.84)
        assert(first30 == 1);                  // (two ticks there: the stuck-after-a-roll)
        assert(tps > 55 && tps < 65);          // (the world at full speed)
    } else if (which == 2) {  // game code without the hook: the per-frame mode never runs
        for (int i = 0; i < 600; i++) frame(22.2, false, t, NULL);
        printf("no hook, ~45 fps: %d of 600 frames in the 30 fps mode\n", frames_30);
        assert(frames_30 <= 2);  // (the first frames, before the hook's absence is known)
    } else if (which == 3) {  // 60 fps: nothing changes - one tick a frame everywhere, never the 30 fps mode
        for (int i = 0; i < 600; i++) frame(16.6667, true, t, NULL);
        printf("hook, 60 fps: %d frames in the 30 fps mode; ticks channel 0 %llu, channel 1 %llu\n", frames_30,
               (unsigned long long)t[0], (unsigned long long)t[1]);
        assert(frames_30 <= 5 && t[0] == t[1]);
    }
    printf("ok\n");
    return 0;
}
