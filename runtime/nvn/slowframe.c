// Slow-frame log: see slowframe.h.
#include "slowframe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform.h"
#include "../rt.h"

#define ADD(v, n) __atomic_fetch_add(&(v), (n), __ATOMIC_RELAXED)
#define GET(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)

static uint64_t n_pipes, pipe_ns, n_hits, n_progs, prog_ns, n_tr, tr_ns, n_uploads, upload_bytes, n_hashes;

void slow_note_build(const char *what, uint64_t ns) {
    if (!strncmp(what, "pipeline", 8)) ADD(n_pipes, 1), ADD(pipe_ns, ns);
    else ADD(n_progs, 1), ADD(prog_ns, ns);
}
void slow_note_cache_hit(void) { ADD(n_hits, 1); }
void slow_note_upload(uint64_t bytes) { ADD(n_uploads, 1), ADD(upload_bytes, bytes); }
void slow_note_full_hash(void) { ADD(n_hashes, 1); }
ShaderInfo *slow_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key) {
    uint64_t t0 = plat_now_ns();
    ShaderInfo *r = maxwell_translate(code, max_size, stage, key);
    ADD(n_tr, 1), ADD(tr_ns, plat_now_ns() - t0);
    return r;
}

typedef struct {
    uint64_t pipes, pipe_ns, hits, progs, prog_ns, tr, tr_ns, uploads, up_bytes, hashes, imports, submits, wait_ns, rcpu, gcpu;
    uint64_t rqw, blocked, present;  // (the game thread's own: waits for the render thread, vsync / pacer waits, presents)
} Snap;

static void snap(Snap *s, uint64_t gcpu) {
    extern uint64_t rq_render_cpu_ns(void);
    s->pipes = GET(n_pipes), s->pipe_ns = GET(pipe_ns), s->hits = GET(n_hits);
    s->progs = GET(n_progs), s->prog_ns = GET(prog_ns);
    s->tr = GET(n_tr), s->tr_ns = GET(tr_ns);
    s->uploads = GET(n_uploads), s->up_bytes = GET(upload_bytes), s->hashes = GET(n_hashes);
    renderer_slow_counters(&s->submits, &s->wait_ns, &s->imports);
    s->rcpu = rq_render_cpu_ns();
    s->gcpu = gcpu;
    extern __thread uint64_t g_rq_wait_ns, g_blocked_ns, g_present_ns;  // (rqueue.c, kernel.c: this thread's)
    s->rqw = g_rq_wait_ns, s->blocked = g_blocked_ns, s->present = g_present_ns;
}

static uint64_t d(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }  // (a backend total reset at a reboot: 0)

#define LOAD_NS 2000000000ull     // a level change: its next 2 s are loading / fading in (measured: up to ~1 s)
#define WINDOW_NS 60000000000ull  // the rate limit's period
#define LINES_PER_WINDOW 20

// The level now, compared with the one seen last: a change starts a load window. False: not in play (title, file
// select, the start screen, a reboot) - loading as well.
static bool level_check(uint64_t now, uint64_t *changed_at, char *name, size_t n) {
    extern const char *game_level_name(void);
    extern int game_in_play(void);
    static char last[64];
    const char *lv = game_level_name();
    if (strncmp(lv, last, sizeof last - 1)) {
        snprintf(last, sizeof last, "%s", lv);
        *changed_at = now;
    }
    if (name) snprintf(name, n, "%s", last);
    return !g_game_is_la || game_in_play() != 0;  // (another game: no level to tell loads by - every slow frame is logged)
}

void slow_frame_present(uint64_t now, uint64_t gcpu) {
    static int64_t limit = -1;  // ns (0: off)
    if (limit < 0) {
        const char *e = getenv("LARECOMP_SLOW_FRAME_MS");
        limit = (e ? atoll(e) : 50) * 1000000ll;
        if (limit < 0) limit = 0;
    }
    if (!limit) return;
    static uint64_t last, frames, changed_at, win_start, win_lines, win_busy, win_load;
    static Snap prev;
    Snap cur;
    snap(&cur, gcpu);
    uint64_t gap = last ? now - last : 0;
    last = now;
    frames++;
    if (!(frames & 15)) level_check(now, &changed_at, NULL, 0);  // (sampled: a level change starts a load window)
    if (win_start && now - win_start >= WINDOW_NS) {
        if (win_busy || win_load) {
            char s[160] = "";
            if (win_busy) snprintf(s, sizeof s, "%llu past the limit of %d lines", (unsigned long long)win_busy, LINES_PER_WINDOW);
            if (win_load)
                snprintf(s + strlen(s), sizeof s - strlen(s), "%s%llu during loads", win_busy ? ", " : "", (unsigned long long)win_load);
            LOG("[slow] %llu more slow frame%s in the last minute, not logged (%s)\n", (unsigned long long)(win_busy + win_load),
                win_busy + win_load == 1 ? "" : "s", s);
        }
        win_start = win_lines = win_busy = win_load = 0;
    }
    if (gap > (uint64_t)limit) {
        char lv[64];
        bool play = level_check(now, &changed_at, lv, sizeof lv);
        if (!win_start) win_start = now;
        if (!play || now - changed_at < LOAD_NS) win_load++;
        else if (win_lines >= LINES_PER_WINDOW) win_busy++;
        else {
            win_lines++;
            extern uint64_t renderer_present_count(void);
            char b[640];
            int n = snprintf(b, sizeof b, "[slow] frame %llu %llu ms in %s:", (unsigned long long)renderer_present_count(),
                             (unsigned long long)(gap / 1000000), lv);
            const char *sep = " ";
#define PART(...) do { if (n < (int)sizeof b) n += snprintf(b + n, sizeof b - n, __VA_ARGS__); sep = ", "; } while (0)
            uint64_t v = d(cur.pipes, prev.pipes), hits = d(cur.hits, prev.hits);
            if (v) {
                PART("%s%llu pipeline%s built (%.0f ms", sep, (unsigned long long)v, v == 1 ? "" : "s", d(cur.pipe_ns, prev.pipe_ns) / 1e6);
                if (hits) PART(", %llu from cache", (unsigned long long)hits);
                PART(")");
            }
            if ((v = d(cur.progs, prev.progs)))
                PART("%s%llu program%s compiled (%.0f ms)", sep, (unsigned long long)v, v == 1 ? "" : "s", d(cur.prog_ns, prev.prog_ns) / 1e6);
            if ((v = d(cur.tr, prev.tr)))
                PART("%s%llu shader%s translated (%.0f ms)", sep, (unsigned long long)v, v == 1 ? "" : "s", d(cur.tr_ns, prev.tr_ns) / 1e6);
            if ((v = d(cur.uploads, prev.uploads))) {
                uint64_t by = d(cur.up_bytes, prev.up_bytes);
                if (by < 1048576) PART("%s%llu texture upload%s %llu KB", sep, (unsigned long long)v, v == 1 ? "" : "s", (unsigned long long)(by + 1023) / 1024);
                else PART("%s%llu texture upload%s %.1f MB", sep, (unsigned long long)v, v == 1 ? "" : "s", by / 1048576.0);
            }
            if ((v = d(cur.hashes, prev.hashes))) PART("%s%llu full texture hash%s", sep, (unsigned long long)v, v == 1 ? "" : "es");
            if ((v = d(cur.imports, prev.imports))) PART("%s%llu heap import%s", sep, (unsigned long long)v, v == 1 ? "" : "s");
            v = d(cur.submits, prev.submits);
            PART("%s%llu submission%s", sep, (unsigned long long)v, v == 1 ? "" : "s");
            PART("%sgpu wait %.0f ms", sep, d(cur.wait_ns, prev.wait_ns) / 1e6);
            // (render cpu 0: no render thread - that work is the game thread's)
            if (prev.rcpu) PART("%srender cpu %.0f ms", sep, d(cur.rcpu, prev.rcpu) / 1e6);
            PART("%sgame cpu %.0f ms", sep, d(cur.gcpu, prev.gcpu) / 1e6);
            // where the rest went: this thread waiting for the render thread, for a vsync / the pacer, in presents;
            // and the longest wait the render thread itself reported (rqueue.c rq_watch: nextDrawable, a GPU wait)
            PART("%squeue wait %.0f ms", sep, d(cur.rqw, prev.rqw) / 1e6);
            PART("%svsync/pace wait %.0f ms", sep, d(cur.blocked, prev.blocked) / 1e6);
            if (d(cur.present, prev.present) >= 1000000) PART("%spresent %.0f ms", sep, d(cur.present, prev.present) / 1e6);
            extern uint64_t rq_render_longest(const char **what);
            const char *rw = NULL;
            uint64_t rwn = rq_render_longest(&rw);
            if (rw && rwn >= 5000000) PART("%srender thread longest: %s %.0f ms", sep, rw, rwn / 1e6);
#undef PART
            LOG("%s\n", b);
            // (held up by the GPU - this thread waiting on it or on the render thread, which waits on it - with little
            // CPU work: the next frame's GPU time per pass, Vulkan: exec.c vk_gpuprof_request)
            extern void vk_gpuprof_request(void) __attribute__((weak));
            uint64_t held = d(cur.wait_ns, prev.wait_ns) + d(cur.rqw, prev.rqw);
            if (vk_gpuprof_request && held * 2 > gap && !d(cur.pipes, prev.pipes) && !d(cur.progs, prev.progs))
                vk_gpuprof_request();
        }
    }
    prev = cur;
}
