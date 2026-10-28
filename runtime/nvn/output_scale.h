// Output resolution (runtime/nvn/output_scale.c), shared by the Metal and Vulkan backends.
#pragma once
#include <stdbool.h>

typedef struct {
    double sx, sy;             // render scale (relative to the game's 1920x1080 sizes): what a frame is drawn at
    double ax, ay;             // allocation scale: the render targets' size (Dynamic renders into a part of them)
    double shape;              // window aspect / 16:9 when the camera is widened, else 1
    double vx, vy, vw, vh;     // where the image is drawn in the window (pixels)
    double lines;              // the frame's lines per window line of its image (below 1: drawn smaller, upscaled)
} OutScale;

// dw x dh: the window's drawable size in pixels; now: seconds; GPU time of the last frame: game_ms the game's (< 0:
// unknown), win_ms the window step's - the game's image drawn into the window (upscale, SMAA) and the menu (< 0:
// unknown / not counted)
void output_scale(double dw, double dh, bool reshape_allowed, double now, double game_ms, double win_ms, OutScale *o);
double output_scale_dynamic_factor(void);
double output_scale_dynamic_hold_fps(void);
// (diagnostics) "[scale] frame f drawable WxH -> render scale sx x sy" whenever the render scale or the window changes
void output_scale_log(unsigned long long frame, double dw, double dh, const OutScale *o);  // the frame rate Dynamic holds (60 out of reach), else 0

// (Vulkan) where a frame's time went, noted once per frame before output_scale(); LARECOMP_LOG_DYNRES's [dynres] line
// shows the per-frame averages. ms over the frame: game = the game's submissions' GPU time, win = the window image's
// (from after its acquire wait), span = GPU time from the frame's first game work to its window submission's end
// (over `frames` frames that ended in it), CPU waits: frame slot's fence, acquire, present, other vk_wait_seq waits.
typedef struct {
    double game, win, span, frames, w_slot, w_acquire, w_present, w_other;
    bool win_counted;  // the window's GPU time is in what dynamic resolution goes by
} DynSplit;
void output_scale_note_split(const DynSplit *s);
// (Metal) the frame's command buffers' GPU spans summed, overlaps and all (what dynamic resolution went by before it
// counted their union): shown in LARECOMP_LOG_DYNRES's [dynres] line
void output_scale_note_sum(double ms);
