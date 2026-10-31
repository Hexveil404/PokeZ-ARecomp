// The game's side of ZA DevTools (docs/dev-tools.md), off unless LARECOMP_DEVTOOLS=1. The tool is a
// program of its own (tools/devtools/zadevtools.cpp -> zadevtools.exe, its own window): F10 starts it. This side
// only gathers and serves, over two named pipes (this machine only):
//   \\.\pipe\zadevtools_out  game -> tool: "L <seconds>\t<log line>" for every stderr line (the backlog first),
//                            "F <ms>" for every presented frame, "C <dir>" when a capture was written
//   \\.\pipe\zadevtools_in   tool -> game: "capture", "trigger <0|1> <ms>", "f9"
// The log is taken from stderr itself (a pipe in front of it, the lines passed on to where stderr went), so nothing
// in the runtime changes for it; a later freopen of stderr (the release's logs/play.log) is noticed and taken again.
// A capture (the tool's button, or the drop trigger: a frame slower than N ms) writes captures/<time>/: log.txt (the
// last 3000 lines), frames.csv (the last 20 s of frame times) and asks for an F9 frame capture.
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <string>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

extern "C" volatile int g_debug_frame_request;
extern "C" uint64_t g_input_reads;
extern "C" uint64_t g_replay_key;                // (the recording's clock: the game's time, or the read count)               // runtime/hle/input.c (the replay's clock: the game's reads of the pad)
extern "C" uint64_t renderer_present_count(void);
extern "C" int g_probe_x, g_probe_y;             // runtime/nvn/vulkan/exec.c (the probe's spot, thousandths)  // runtime/hle/input.c (F9: the next frame's passes, build/pass_*.png)

#ifdef _WIN32
// ------------------------------------------------------------------ the log
struct LogLine { double t; std::string text; };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::deque<LogLine> g_log;     // (the backlog a tool gets when it connects)
static const size_t LOG_MAX = 20000;
static std::string g_outq;            // bytes waiting for the tool
static std::atomic<bool> g_client{false};
static uint64_t g_t0;
static int g_out = -1;
static intptr_t g_pipe_handle;
static bool g_on;  // LARECOMP_DEVTOOLS is set

static double now_s() { return (double)(SDL_GetTicksNS() - g_t0) / 1e9; }

static void send_locked(const std::string &s) {  // (g_lock held)
    if (g_client.load() && g_outq.size() < (8u << 20)) g_outq += s;
}

static void add_line(const std::string &s) {
    double t = now_s();
    g_log.push_back({t, s});
    if (g_log.size() > LOG_MAX) g_log.pop_front();
    char h[32];
    snprintf(h, sizeof h, "L %.3f\t", t);
    send_locked(h + s + "\n");
}

static void *reader(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[8192];
    std::string part;
    for (;;) {
        int n = _read(fd, buf, sizeof buf);
        if (n <= 0) break;
        if (g_out >= 0) _write(g_out, buf, n);  // (where the log went before: the file / console stays complete)
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') { add_line(part); part.clear(); }
            else if (buf[i] != '\r') part += buf[i];
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void capture_install(void) {
    int f[2];
    if (_pipe(f, 1 << 16, _O_BINARY) != 0) return;
    fflush(stderr);
    int out = _dup(2);
    if (out < 0 || _dup2(f[1], 2) != 0) return;
    g_out = out;
    g_pipe_handle = _get_osfhandle(2);
    setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t t;
    pthread_create(&t, NULL, reader, (void *)(intptr_t)f[0]);
    pthread_detach(t);
}

// ------------------------------------------------------------------ frame times, captures
static const int FT_N = 1200;
static float g_ft[FT_N];
static int g_ft_pos;
static uint64_t g_ft_count, g_last_ns;
static std::atomic<bool> g_trig_on{false};
static std::atomic<float> g_trig_ms{50};
static double g_trig_last = -100;

static void write_capture(float ms) {
    time_t now = time(NULL);
    char name[64], dir[96], p[160];
    strftime(name, sizeof name, "%Y%m%d-%H%M%S", localtime(&now));
    SDL_CreateDirectory("captures");
    snprintf(dir, sizeof dir, "captures/%s", name);
    SDL_CreateDirectory(dir);
    snprintf(p, sizeof p, "%s/log.txt", dir);
    pthread_mutex_lock(&g_lock);
    if (FILE *f = fopen(p, "w")) {
        size_t from = g_log.size() > 3000 ? g_log.size() - 3000 : 0;
        for (size_t i = from; i < g_log.size(); i++) fprintf(f, "%9.3f %s\n", g_log[i].t, g_log[i].text.c_str());
        fclose(f);
    }
    pthread_mutex_unlock(&g_lock);
    snprintf(p, sizeof p, "%s/frames.csv", dir);
    if (FILE *f = fopen(p, "w")) {
        fprintf(f, "frame,ms\n");
        int n = (int)std::min<uint64_t>(g_ft_count, FT_N);
        for (int i = 0; i < n; i++) fprintf(f, "%d,%.3f\n", i, g_ft[(g_ft_pos - n + i + FT_N) % FT_N]);
        fclose(f);
    }
    g_debug_frame_request = 1;  // (F9: dumped when LARECOMP_DEBUG_FRAMES is set)
    fprintf(stderr, "[devtools] frame of %.0f ms: captured to %s\n", ms, dir);
    pthread_mutex_lock(&g_lock);
    send_locked(std::string("C ") + dir + "\n");
    pthread_mutex_unlock(&g_lock);
}

// Every presented frame (runtime/ui/menu.mm menu_render)
extern "C" void devtools_frame(void) {
    if (!g_on) return;
    if (_get_osfhandle(2) != g_pipe_handle) capture_install();  // (stderr reopened since: runtime/main.c logs/play.log)
    uint64_t now = SDL_GetTicksNS();
    if (g_last_ns) {
        float ms = (float)(now - g_last_ns) / 1e6f;
        g_ft[g_ft_pos] = ms;
        g_ft_pos = (g_ft_pos + 1) % FT_N;
        g_ft_count++;
        if (g_client.load()) {
            char m[32];
            snprintf(m, sizeof m, "F %.2f\n", ms);
            pthread_mutex_lock(&g_lock);
            send_locked(m);
            pthread_mutex_unlock(&g_lock);
        }
        double t = now_s();
        if (g_trig_on.load() && ms > g_trig_ms.load() && t - g_trig_last > 5) {  // (5 s apart: one capture per drop)
            g_trig_last = t;
            write_capture(ms);
        }
    }
    g_last_ns = now;
}

// ------------------------------------------------------------------ the pipes
static void *serve_out(void *) {
    for (;;) {
        HANDLE h = CreateNamedPipeA("\\\\.\\pipe\\zadevtools_out", PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1,
                                    1 << 20, 0, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { SDL_Delay(1000); continue; }
        if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(h); continue; }
        pthread_mutex_lock(&g_lock);  // the backlog, then the stream
        g_outq.clear();
        g_client = true;
        for (auto &l : g_log) {
            char hd[32];
            snprintf(hd, sizeof hd, "L %.3f\t", l.t);
            g_outq += hd + l.text + "\n";
        }
        pthread_mutex_unlock(&g_lock);
        for (;;) {
            std::string chunk;
            pthread_mutex_lock(&g_lock);
            chunk.swap(g_outq);
            pthread_mutex_unlock(&g_lock);
            if (chunk.empty()) { SDL_Delay(15); continue; }
            DWORD w;
            if (!WriteFile(h, chunk.data(), (DWORD)chunk.size(), &w, NULL)) break;  // (the tool closed)
        }
        g_client = false;
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
    return NULL;
}
static void command(const char *c) {
    int on;
    float ms;
    if (!strcmp(c, "capture")) write_capture(0);
    else if (!strcmp(c, "f9")) g_debug_frame_request = 1;
    else if (sscanf(c, "probe %d %f", &on, &ms) == 2) {  // "probe <x> <y>" (thousandths): which draws paint that spot
        g_probe_x = on, g_probe_y = (int)ms;
        g_debug_frame_request = 1;
        fprintf(stderr, "[devtools] probe at %d,%d (thousandths): the next frame's draws that paint it\n", g_probe_x, g_probe_y);
    }
    else if (!strcmp(c, "mark")) {  // "the bug is on screen now": the read / frame a replay can be taken back to
        unsigned long long rd = g_input_reads, fr = renderer_present_count(), key = g_replay_key;
        g_debug_frame_request = 1;  // (and the frame itself, F9)
        fprintf(stderr, "[devtools] mark: read %llu, frame %llu, clock %llu\n", rd, fr, key);
        char m[96];
        snprintf(m, sizeof m, "M %llu %llu %llu\n", rd, fr, key);
        pthread_mutex_lock(&g_lock);
        send_locked(m);
        pthread_mutex_unlock(&g_lock);
    }
    else if (sscanf(c, "trigger %d %f", &on, &ms) == 2) {
        g_trig_on = on != 0, g_trig_ms = ms;
        fprintf(stderr, "[devtools] drop trigger %s (%.0f ms)\n", on ? "on" : "off", ms);
    }
}
static void *serve_in(void *) {
    for (;;) {
        HANDLE h = CreateNamedPipeA("\\\\.\\pipe\\zadevtools_in", PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 0,
                                    1 << 16, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { SDL_Delay(1000); continue; }
        if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(h); continue; }
        char buf[512];
        std::string part;
        DWORD n;
        while (ReadFile(h, buf, sizeof buf, &n, NULL) && n) {
            for (DWORD i = 0; i < n; i++) {
                if (buf[i] == '\n') { command(part.c_str()); part.clear(); }
                else part += buf[i];
            }
        }
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
    return NULL;
}

// (off unless asked for: no stderr capture, no pipes, nothing per frame in a player's run)
__attribute__((constructor)) static void devtools_boot(void) {
    g_on = getenv("LARECOMP_DEVTOOLS") != NULL;
    if (!g_on) return;
    g_t0 = SDL_GetTicksNS();
    capture_install();
    pthread_t t;
    pthread_create(&t, NULL, serve_out, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, serve_in, NULL);
    pthread_detach(t);
}

// F10 (runtime/ui/menu.mm, with LARECOMP_DEVTOOLS=1): the tool, started beside the game (ZADEVTOOLS: its exe, else
// zadevtools.exe next to the game's)
extern "C" void devtools_toggle(void) {
    const char *exe = getenv("ZADEVTOOLS");
    char path[MAX_PATH];
    if (!exe) {
        GetModuleFileNameA(NULL, path, sizeof path);
        char *s = strrchr(path, '\\');
        if (s) snprintf(s + 1, sizeof path - (s + 1 - path), "zadevtools.exe");
        exe = path;
    }
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    char cmd[600];
    snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
        fprintf(stderr, "[devtools] started the tool\n");
    } else {
        fprintf(stderr, "[devtools] can't start the tool (error %lu): set ZADEVTOOLS\n", GetLastError());
    }
}
#else
extern "C" void devtools_frame(void) {}
extern "C" void devtools_toggle(void) {}
#endif

// (the in-game window is gone: the tool has its own)
extern "C" bool devtools_open(void) { return false; }
extern "C" bool devtools_typing(void) { return false; }
extern "C" void devtools_draw(float, float, float) {}
extern "C" bool devtools_view(double, double, double *, double *, double *, double *) { return false; }
