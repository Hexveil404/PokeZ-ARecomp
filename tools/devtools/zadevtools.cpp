// ZA DevTools: a program of its own beside the game (docs/dev-tools.md; F10 in a game started with LARECOMP_DEVTOOLS=1). It reads the game's
// \\.\pipe\zadevtools_out (runtime/ui/devtools.mm: log lines, frame times, captures) and writes commands to
// \\.\pipe\zadevtools_in. Reconnects by itself; what it got stays when the game closes or crashes.
// Build: bash tools/devtools/build.sh. Paths: devtools.ini next to the exe (devtools.ini.example).
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

// ------------------------------------------------------------------ what the game sent
struct LogLine { double t; std::string tag, text; int session; };
struct VkStat { double t, fps; int max_ms, wait_ms; long draws, passes, dispatches; double upload_mb; };
static std::mutex g_lock;
static std::deque<LogLine> g_log;
static std::vector<std::string> g_tags;
static std::deque<VkStat> g_vk;
static std::vector<float> g_ft;          // every frame time since connecting (ms)
static std::vector<double> g_ft_t;       // and when it was (game seconds, as the log lines' times)
// The game's clock seen from here: the log lines carry game seconds; the rest (frame times, watches) is stamped with
// this tool's clock moved by the last line's offset (good to a few ms). A click on any graph's point jumps there.
static double g_offset;
static std::atomic<double> g_jump_t{-1};
static std::atomic<unsigned long long> g_mark_read{~0ull}, g_mark_frame{0}, g_mark_key{0};  // ("M": a repro to make, in the main loop)
static double tool_now() { return (double)SDL_GetTicksNS() / 1e9; }
static double game_now() { return tool_now() + g_offset; }
static std::vector<std::string> g_caps;  // capture folders, newest last
static std::atomic<bool> g_connected{false};
static int g_session;

// F9 frame captures, read back from the log: "===== debug frame N =====", then per draw a "[draw] ..." line, and when
// a pass ends "[pass] <png> <w>x<h> fmt <f> targets <n> layers <l>" (runtime/nvn/vulkan/exec.c) - its draws are the
// [draw] lines since the last pass
struct Pass { std::string png, info; int w, h; std::vector<std::string> draws; };
struct ProbeHit { int draw, target; std::string png, text; };
struct FcBlob { uint32_t kind, slot; uint64_t addr, size; std::vector<uint8_t> data; };
struct FcDraw { uint32_t draw, pass, indexed, mode, inst, gpu, cfmt, dfmt; uint64_t vs, fs, count; std::vector<uint8_t> cg, dg; std::vector<FcBlob> blobs; };
struct FrameCap { unsigned long long frame; double t; std::vector<Pass> passes; std::vector<std::string> pending; std::vector<ProbeHit> probe;
                  std::string fullcap; };
static std::vector<FrameCap> g_frames;

static void add_line(double t, const std::string &s) {
    unsigned long long fn;
    if (sscanf(s.c_str(), "===== debug frame %llu =====", &fn) == 1) g_frames.push_back(FrameCap{fn, t, {}, {}, {}, ""});
    else if (!g_frames.empty() && !s.compare(0, 10, "[fullcap] ")) {  // "[fullcap] frame N: recording every draw to <path>"
        size_t to = s.find(" to ");
        if (to != std::string::npos) g_frames.back().fullcap = s.substr(to + 4);
    }
    else if (!g_frames.empty() && !s.compare(0, 8, "[probe] ")) {  // "[probe] build/probe_F_NN.png: draw N target T ..."
        ProbeHit h;
        char png[256] = "";
        h.draw = h.target = -1;
        sscanf(s.c_str() + 8, "%255[^:]: draw %d target %d", png, &h.draw, &h.target);
        h.png = png;
        size_t c = s.find(": ");
        h.text = c == std::string::npos ? s : s.substr(c + 2);
        g_frames.back().probe.push_back(h);
    }
    else if (!g_frames.empty() && !s.compare(0, 9, "[draw]   ")) {  // (LARECOMP_DEBUG_DATA: streams, constants)
        if (!g_frames.back().pending.empty()) g_frames.back().pending.back() += "\n    " + s.substr(9);
    }
    else if (!g_frames.empty() && !s.compare(0, 7, "[draw] ")) g_frames.back().pending.push_back(s.substr(7));
    else if (!g_frames.empty() && !s.compare(0, 8, "[vscan] ") && !g_frames.back().pending.empty())
        g_frames.back().pending.back() += " || " + s.substr(8);
    else if (!g_frames.empty() && !s.compare(0, 7, "[pass] ")) {
        Pass p;
        char png[300] = "";
        p.w = p.h = 0;
        sscanf(s.c_str() + 7, "%299s %dx%d", png, &p.w, &p.h);
        p.png = png;
        p.info = s.substr(7 + strlen(png));
        p.draws.swap(g_frames.back().pending);
        g_frames.back().passes.push_back(std::move(p));
    }
    LogLine l{t, "", s, g_session};
    if (s.size() > 2 && s[0] == '[') {
        size_t e = s.find(']');
        if (e != std::string::npos && e < 24) l.tag = s.substr(0, e + 1);
    }
    if (!s.compare(0, 7, "[vk] t=")) {  // [vk] t=12.6 frame 599 59.7 fps max 23ms level - (13511 draws, 0 skipped, ...)
        VkStat v = {};
        unsigned long long fr;
        if (sscanf(s.c_str(), "[vk] t=%lf frame %llu %lf fps max %dms", &v.t, &fr, &v.fps, &v.max_ms) == 4) {
            const char *c = s.c_str(), *p;
            if ((p = strchr(c, '('))) sscanf(p + 1, "%ld draws", &v.draws);
            if ((p = strstr(c, "skipped, "))) sscanf(p + 9, "%ld passes, %ld dispatches", &v.passes, &v.dispatches);
            if ((p = strstr(c, "wait "))) sscanf(p + 5, "%dms", &v.wait_ms);
            if ((p = strstr(c, "upload "))) sscanf(p + 7, "%lfMB", &v.upload_mb);
            v.t = t;
            g_vk.push_back(v);
            if (g_vk.size() > 3600) g_vk.pop_front();
        }
    }
    if (!l.tag.empty() && std::find(g_tags.begin(), g_tags.end(), l.tag) == g_tags.end()) g_tags.push_back(l.tag);
    g_log.push_back(std::move(l));
    if (g_log.size() > 200000) g_log.pop_front();
}

static void reader_thread() {
    for (;;) {
        HANDLE h = CreateFileA("\\\\.\\pipe\\zadevtools_out", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
        {
            std::lock_guard<std::mutex> lk(g_lock);
            g_session++;
            g_ft.clear();
            g_ft_t.clear();
            add_line(0, "[tool] ===== connected to the game =====");
        }
        g_connected = true;
        char buf[1 << 16];
        std::string part;
        DWORD n;
        while (ReadFile(h, buf, sizeof buf, &n, NULL) && n) {
            std::lock_guard<std::mutex> lk(g_lock);
            for (DWORD i = 0; i < n; i++) {
                if (buf[i] != '\n') { part += buf[i]; continue; }
                if (part.size() > 2 && part[0] == 'L' && part[1] == ' ') {
                    size_t tab = part.find('\t');
                    double lt = atof(part.c_str() + 2);
                    g_offset = lt - tool_now();
                    add_line(lt, tab == std::string::npos ? "" : part.substr(tab + 1));
                } else if (part.size() > 2 && part[0] == 'F') {
                    g_ft.push_back((float)atof(part.c_str() + 2));
                    g_ft_t.push_back(game_now());
                } else if (part.size() > 2 && part[0] == 'M') {
                    unsigned long long rd = 0, fr = 0, key = 0;
                    int got = sscanf(part.c_str() + 2, "%llu %llu %llu", &rd, &fr, &key);
                    if (got >= 2) g_mark_frame = fr, g_mark_key = got == 3 ? key : rd, g_mark_read = rd;
                } else if (part.size() > 2 && part[0] == 'C') {
                    g_caps.push_back(part.substr(2));
                }
                part.clear();
            }
        }
        CloseHandle(h);
        g_connected = false;
        std::lock_guard<std::mutex> lk(g_lock);
        add_line(g_log.empty() ? 0 : g_log.back().t, "[tool] ===== the game closed (or stopped answering) =====");
    }
}

static void send_command(const char *c) {
    HANDLE h = CreateFileA("\\\\.\\pipe\\zadevtools_in", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string s = std::string(c) + "\n";
    DWORD w;
    WriteFile(h, s.data(), (DWORD)s.size(), &w, NULL);
    CloseHandle(h);
}

// ------------------------------------------------------------------ starting the game
// The tool starts the game itself (and stops exactly that process): a profile = exe, arguments, working folder and
// environment. Paths from the recomp folder (two above this exe). The dev game library is the one the replays use.
struct Profile { std::string name, save, ini; };
static std::vector<Profile> PROFILES;
static std::string g_exe, g_game_dir, g_lib, g_release_lib;  // devtools.ini: exe, game, lib, release_lib
static int g_profile;
static bool g_debug_frames;
static HANDLE g_game;
static DWORD g_game_pid;
static std::string g_root;  // the recomp folder, with a trailing backslash

static std::string read_first_line(const std::string &path) {
    std::string s;
    if (FILE *f = fopen(path.c_str(), "rb")) {
        char b[1024];
        if (fgets(b, sizeof b, f)) s = b;
        fclose(f);
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
static std::string win_path(std::string p) {  // (/c/Users/... from the bash scripts -> C:\Users\...)
    if (p.size() > 2 && p[0] == '/' && p[2] == '/') p = std::string(1, (char)toupper(p[1])) + ":" + p.substr(2);
    for (auto &c : p) if (c == '/') c = '\\';
    return p;
}
static bool game_running() {
    if (!g_game) return false;
    if (WaitForSingleObject(g_game, 0) == WAIT_TIMEOUT) return true;
    CloseHandle(g_game);
    g_game = NULL, g_game_pid = 0;
    return false;
}
static void game_stop() {
    if (!game_running()) return;
    TerminateProcess(g_game, 1);  // (the process this tool started - no other)
    WaitForSingleObject(g_game, 5000);
    CloseHandle(g_game);
    g_game = NULL, g_game_pid = 0;
    std::lock_guard<std::mutex> lk(g_lock);
    add_line(g_log.empty() ? 0 : g_log.back().t, "[tool] ===== game stopped by the tool =====");
}
// A command line run hidden, waited for (copies)
static void run_hidden(const std::string &cmd) {
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    std::string c = "cmd /c \"" + cmd + "\"";
    std::vector<char> v(c.begin(), c.end());
    v.push_back(0);
    if (CreateProcessA(NULL, v.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    }
}
static void copy_dir(const std::string &from, const std::string &to) { run_hidden("xcopy /E /I /Q /Y \"" + from + "\" \"" + to + "\" >NUL"); }
static void copy_file(const std::string &from, const std::string &to) { run_hidden("copy /Y \"" + from + "\" \"" + to + "\" >NUL"); }
static std::string stamp() {
    time_t now = time(NULL);
    char s[32];
    strftime(s, sizeof s, "%Y%m%d-%H%M%S", localtime(&now));
    return s;
}
static void write_text(const std::string &path, const std::string &s) {
    if (FILE *f = fopen(path.c_str(), "wb")) { fputs(s.c_str(), f); fclose(f); }
}

// Every run is a recording (the replay patch in runtime/hle/input.c): testes/sessions/<time>/ keeps the save as it was
// at the start, the settings, the clock, the game library and input.txt (the pad as the game read it, by read). "Mark
// bug here" turns the session into repros/<time>/ with mark.txt (the read and frame the bug was on screen); "Replay to
// the bug" starts a fresh copy of that save with the same clock and plays the recording up to the mark, where the
// controls are the player's again (LARECOMP_REPLAY_UNTIL).
static std::string g_session_dir;
// The game code library: the dev one (the replays', built with make) or the release one (built by the start screen's
// setup like a player's: zig, the giant functions at -O0) - the same game code, the same id, compiled differently
static int g_lib_kind;  // 0 dev, 1 release
// Race test: LARECOMP_SYNC_SUBMIT=1 - every command list waited for on the GPU before the game's CPU side goes on (no
// frame ahead): a bug that never shows then is a CPU / GPU race (the game writing next frame's data while the GPU
// still reads this frame's). Slower.
static bool g_race_test;
// LARECOMP_DEBUG_DATA=1: an F9 frame's draws also log their vertex streams' and constant buffers' first values (the
// vertex constants hold the matrices / bones: a draw drawn in the wrong place shows it there). Bigger logs.
static bool g_draw_data = true;  // (now: F9 = full capture, LARECOMP_FULLCAP - runtime/nvn/vulkan/exec.c fullcap_draw)
static const char *const LIB_KINDS[] = {"DLL: dev", "DLL: release"};
static std::string chosen_lib(const std::string &dev) {
    return g_lib_kind && !g_release_lib.empty() ? g_release_lib : dev;
}
struct Repro { std::string dir, name, note; unsigned long long read, frame, key; };  // (key: where the replay stops)
static std::vector<Repro> g_repros;
static std::string g_replaying;  // (the repro being replayed, else "")

// devtools.ini (next to the exe, not in git: devtools.ini.example): where things are on this PC. "key = value" lines;
// profile = <name> | <save folder> | <settings file>, one per save. Sessions, repros and captures go in work.
static void load_config(const std::string &path, const std::string &dir) {
    g_root = dir + "\\";
    if (FILE *f = fopen(path.c_str(), "r")) {
        char l[2048];
        while (fgets(l, sizeof l, f)) {
            std::string s = l;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            size_t q = s.find('=');
            if (s.empty() || s[0] == '#' || q == std::string::npos) continue;
            std::string k = s.substr(0, q), v = s.substr(q + 1);
            while (!k.empty() && k.back() == ' ') k.pop_back();
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            if (k == "work") g_root = win_path(v) + "\\";
            else if (k == "exe") g_exe = win_path(v);
            else if (k == "game") g_game_dir = win_path(v);
            else if (k == "lib") g_lib = win_path(v);
            else if (k == "release_lib") g_release_lib = win_path(v);
            else if (k == "profile") {
                size_t a = v.find('|'), b = a == std::string::npos ? a : v.find('|', a + 1);
                if (b == std::string::npos) continue;
                auto trim = [](std::string t) { while (!t.empty() && t.back() == ' ') t.pop_back(); while (!t.empty() && t[0] == ' ') t.erase(0, 1); return t; };
                PROFILES.push_back({trim(v.substr(0, a)), trim(v.substr(a + 1, b - a - 1)), trim(v.substr(b + 1))});
            }
        }
        fclose(f);
    }
    if (PROFILES.empty()) PROFILES.push_back({"Work folder", g_root + "save", g_root + "larecomp.ini"});
}

static void launch(const std::string &save, const std::string &ini, const std::string &lib, const std::string &logp,
                   const std::string &label) {
    std::string exe = g_exe, cwd = g_root;
    SetEnvironmentVariableA("LARECOMP_DEVTOOLS", "1");
    SetEnvironmentVariableA("LARECOMP_DATA_ROOT", g_root.substr(0, g_root.size() - 1).c_str());  // (captures, logs and caches in work)
    SetEnvironmentVariableA("SDL_VIDEODRIVER", NULL);  // (the tool's own choice, not the game's)
    SetEnvironmentVariableA("LARECOMP_GAME_LIB", lib.c_str());
    SetEnvironmentVariableA("LARECOMP_SAVE_DIR", save.c_str());
    SetEnvironmentVariableA("LARECOMP_SETTINGS", ini.c_str());
    SetEnvironmentVariableA("LARECOMP_DEBUG_FRAMES", "1");  // (F9: build/pass_<frame>_NNN.png - each capture its own files)
    SetEnvironmentVariableA("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    SetEnvironmentVariableA("LARECOMP_SYNC_SUBMIT", g_race_test ? "1" : NULL);
    SetEnvironmentVariableA("LARECOMP_FULLCAP", g_draw_data ? "1" : NULL);
    SetEnvironmentVariableA("LARECOMP_DUMP_GLSL", g_draw_data ? "1" : NULL);  // (the shaders of a capture: build/glsl/<code address>)  // (the pad drives the game while the panels have the focus)
    char path[32768];
    GetEnvironmentVariableA("PATH", path, sizeof path);
    if (!strstr(path, "msys64\\ucrt64\\bin")) SetEnvironmentVariableA("PATH", ("C:\\msys64\\ucrt64\\bin;" + std::string(path)).c_str());
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE log = CreateFileA(logp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL);
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    std::string cmd = "\"" + exe + "\" \"" + g_game_dir + "\" --play";
    std::vector<char> c(cmd.begin(), cmd.end());
    c.push_back(0);
    bool ok = CreateProcessA(NULL, c.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    std::lock_guard<std::mutex> lk(g_lock);
    if (!ok) {
        add_line(g_log.empty() ? 0 : g_log.back().t, "[tool] can't start the game: error " + std::to_string(GetLastError()));
        return;
    }
    CloseHandle(pi.hThread);
    g_game = pi.hProcess, g_game_pid = pi.dwProcessId;
    add_line(g_log.empty() ? 0 : g_log.back().t, "[tool] ===== started the game: " + label + (g_race_test ? " [race test: GPU sync]" : "") + " (pid " + std::to_string(pi.dwProcessId) +
                                                     ", log " + logp + ") =====");
}

static void game_start() {
    if (game_running()) return;
    const Profile &p = PROFILES[g_profile];
    std::string lib = chosen_lib(g_lib);
    std::string save = win_path(p.save), ini = win_path(p.ini);
    // the session: the save as it is now, the settings, the clock, the library - the recording starts from these
    g_session_dir = g_root + "testes\\sessions\\" + stamp();
    run_hidden("mkdir \"" + g_session_dir + "\"");
    copy_dir(save, g_session_dir + "\\save");
    copy_file(ini, g_session_dir + "\\larecomp.ini");
    std::string clock = std::to_string((long long)time(NULL));
    write_text(g_session_dir + "\\clock.txt", clock);
    write_text(g_session_dir + "\\gamelib.txt", lib);
    write_text(g_session_dir + "\\profile.txt", p.name);
    SetEnvironmentVariableA("LARECOMP_RECORD", (g_session_dir + "\\input.txt").c_str());
    SetEnvironmentVariableA("LARECOMP_CLOCK_START", clock.c_str());
    SetEnvironmentVariableA("LARECOMP_REPLAY", NULL);
    SetEnvironmentVariableA("LARECOMP_REPLAY_UNTIL", NULL);
    g_replaying.clear();
    launch(save, ini, lib, g_session_dir + "\\play.log", p.name);
}

static void load_repros() {
    g_repros.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_root + "repros\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        Repro r;
        r.dir = g_root + "repros\\" + fd.cFileName;
        r.name = fd.cFileName;
        r.read = r.frame = r.key = 0;
        if (FILE *f = fopen((r.dir + "\\mark.txt").c_str(), "rb")) {
            int got = fscanf(f, "read %llu frame %llu key %llu", &r.read, &r.frame, &r.key);
            if (got < 2) r.read = 0;
            if (got < 3) r.key = r.read;  // (older repros: by read)
            fclose(f);
        }
        r.note = read_first_line(r.dir + "\\note.txt");
        g_repros.push_back(r);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(g_repros.begin(), g_repros.end(), [](const Repro &a, const Repro &b) { return a.name > b.name; });
}

// "M <read> <frame>" from the game: the session so far becomes a repro
static void make_repro(unsigned long long read, unsigned long long frame, unsigned long long key) {
    if (g_session_dir.empty()) return;
    std::string d = g_root + "repros\\" + stamp();
    run_hidden("mkdir \"" + d + "\"");
    copy_dir(g_session_dir + "\\save", d + "\\save");
    for (const char *f : {"larecomp.ini", "clock.txt", "gamelib.txt", "profile.txt", "input.txt"})
        copy_file(g_session_dir + "\\" + f, d + "\\" + f);
    write_text(d + "\\mark.txt", "read " + std::to_string(read) + " frame " + std::to_string(frame) + " key " + std::to_string(key) + "\n");
    std::lock_guard<std::mutex> lk(g_lock);
    add_line(g_log.empty() ? 0 : g_log.back().t, "[tool] repro saved: " + d + " (read " + std::to_string(read) + ", frame " +
                                                     std::to_string(frame) + ")");
}

static void replay_repro(const Repro &r) {
    if (game_running()) return;
    std::string run = r.dir + "\\run";  // (a fresh copy each time: the game writes its save)
    run_hidden("rmdir /S /Q \"" + run + "\"");
    copy_dir(r.dir + "\\save", run + "\\save");
    copy_file(r.dir + "\\larecomp.ini", run + "\\larecomp.ini");
    SetEnvironmentVariableA("LARECOMP_RECORD", NULL);
    SetEnvironmentVariableA("LARECOMP_REPLAY", (r.dir + "\\input.txt").c_str());
    SetEnvironmentVariableA("LARECOMP_REPLAY_UNTIL", std::to_string(r.key).c_str());
    SetEnvironmentVariableA("LARECOMP_CLOCK_START", read_first_line(r.dir + "\\clock.txt").c_str());
    g_session_dir.clear();
    g_replaying = r.name;
    launch(run + "\\save", run + "\\larecomp.ini", chosen_lib(read_first_line(r.dir + "\\gamelib.txt")), run + "\\play.log",
           "replay of " + r.name + " up to read " + std::to_string(r.read) + " (" + LIB_KINDS[g_lib_kind] + ")");
}

// ------------------------------------------------------------------ the game inside the tool
// The game's own window becomes a child window of the tool, kept on the Game panel: it renders as always (its own
// Vulkan swapchain), the tool only places it. A click on it gives it the keyboard. Let go (Detach, the tool closing)
// it is a window of its own again.
static HWND g_tool_hwnd, g_game_hwnd;
static bool g_probe_armed;  // ("Probe: click the bug" - the next click on the game panel is the spot)
static RECT g_game_rect;
static bool g_embed = true;
static LONG_PTR g_game_style;

static BOOL CALLBACK find_game(HWND h, LPARAM) {
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_game_pid && IsWindowVisible(h) && !GetParent(h)) { g_game_hwnd = h; return FALSE; }
    return TRUE;
}
static void embed_release() {
    if (!g_game_hwnd || !IsWindow(g_game_hwnd)) { g_game_hwnd = NULL; return; }
    SetParent(g_game_hwnd, NULL);
    SetWindowLongPtrA(g_game_hwnd, GWL_STYLE, g_game_style);
    SetWindowPos(g_game_hwnd, HWND_TOP, 100, 100, 1280, 720, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    g_game_hwnd = NULL;
}
// Every tool frame: the game's window on the Game panel's rectangle (client pixels of the tool), or let go
static void embed_update(int x, int y, int w, int h) {
    if (!g_embed || !game_running()) {
        if (g_game_hwnd) embed_release();
        return;
    }
    if (g_game_hwnd && !IsWindow(g_game_hwnd)) g_game_hwnd = NULL;
    if (!g_game_hwnd) {
        EnumWindows(find_game, 0);
        if (!g_game_hwnd) return;  // (not open yet)
        g_game_style = GetWindowLongPtrA(g_game_hwnd, GWL_STYLE);
        SetWindowLongPtrA(g_game_hwnd, GWL_STYLE, (g_game_style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                                                                     WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_CHILD | WS_VISIBLE);
        SetParent(g_game_hwnd, g_tool_hwnd);
        g_game_rect = RECT{};
    }
    if (w < 16 || h < 16) return;
    RECT r = {x, y, x + w, y + h};
    if (memcmp(&r, &g_game_rect, sizeof r)) {
        g_game_rect = r;
        SetWindowPos(g_game_hwnd, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    // a click on the game's panel: the keyboard to the game (the child shares the tool's input since SetParent)
    static bool was_down;
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    if (down && !was_down && GetForegroundWindow() == g_tool_hwnd) {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(g_tool_hwnd, &p);
        if (p.x >= x && p.x < x + w && p.y >= y && p.y < y + h) {
            if (g_probe_armed) {  // the spot, in thousandths of the game's image
                char c[48];
                snprintf(c, sizeof c, "probe %ld %ld", (long)((p.x - x) * 1000 / w), (long)((p.y - y) * 1000 / h));
                send_command(c);
                g_probe_armed = false;
            } else {
                SetFocus(g_game_hwnd);
            }
        }
    }
    was_down = down;
}

// ------------------------------------------------------------------ images (pass PNGs)
#include <png.h>
#include <map>
static SDL_Renderer *g_ren;
struct Img { SDL_Texture *tex; int w, h; };
static std::map<std::string, Img> g_imgs;
static Img image(const std::string &path) {  // (cached; {NULL} when it can't be read)
    auto it = g_imgs.find(path);
    if (it != g_imgs.end()) return it->second;
    Img im = {NULL, 0, 0};
    png_image pi = {};
    pi.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&pi, path.c_str())) {
        pi.format = PNG_FORMAT_RGBA;
        std::vector<uint8_t> px(PNG_IMAGE_SIZE(pi));
        if (png_image_finish_read(&pi, NULL, px.data(), 0, NULL)) {
            im.tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, pi.width, pi.height);
            if (im.tex) {
                SDL_UpdateTexture(im.tex, NULL, px.data(), pi.width * 4);
                SDL_SetTextureBlendMode(im.tex, SDL_BLENDMODE_NONE);  // (alpha shown as is would hide colour)
                im.w = pi.width, im.h = pi.height;
            }
        }
        png_image_free(&pi);
    }
    g_imgs[path] = im;
    return im;
}

// A graph whose points are moments: hovering says when (snapped to the highest point near the mouse: the spike), a
// click takes the console to that moment
static void plot_t(const char *id, const float *v, const double *t, int n, const char *overlay, float lo, float hi, ImVec2 size) {
    ImGui::PlotLines(id, v, n, 0, overlay, lo, hi, size);
    if (n < 2 || !ImGui::IsItemHovered()) return;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float f = (ImGui::GetIO().MousePos.x - a.x) / std::max(1.0f, b.x - a.x);
    int i = std::min(n - 1, std::max(0, (int)(f * (n - 1) + 0.5f))), best = i;
    int r = std::max(1, n / 150);  // (a few pixels either side)
    for (int k = std::max(0, i - r); k <= std::min(n - 1, i + r); k++) if (v[k] > v[best]) best = k;
    ImGui::SetTooltip("at %.2f s: %.2f\nclick: the console at that moment", t[best], v[best]);
    if (ImGui::IsItemClicked()) g_jump_t = t[best];
}

// ------------------------------------------------------------------ panels
static bool g_trig_on;
static float g_trig_ms = 50;

static void panel_console() {
    static char filter[128];
    static bool paused, follow = true, only_last = true;
    static std::vector<std::string> hidden;
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##filter", "search...", filter, sizeof filter);
    ImGui::SameLine(); ImGui::Checkbox("Pause", &paused);
    ImGui::SameLine(); ImGui::Checkbox("Follow", &follow);
    ImGui::SameLine(); ImGui::Checkbox("This session only", &only_last);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { std::lock_guard<std::mutex> lk(g_lock); g_log.clear(); }
    ImGui::SameLine();
    if (ImGui::Button("Save...")) {
        time_t now = time(NULL);
        char p[96];
        strftime(p, sizeof p, "devtools-log-%Y%m%d-%H%M%S.txt", localtime(&now));
        if (FILE *f = fopen(p, "w")) {
            std::lock_guard<std::mutex> lk(g_lock);
            for (auto &l : g_log) fprintf(f, "%9.3f %s\n", l.t, l.text.c_str());
            fclose(f);
        }
    }
    std::vector<std::string> tags;
    { std::lock_guard<std::mutex> lk(g_lock); tags = g_tags; }
    float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (size_t i = 0; i < tags.size(); i++) {
        bool off = std::find(hidden.begin(), hidden.end(), tags[i]) != hidden.end();
        float w = ImGui::CalcTextSize(tags[i].c_str()).x + 12;
        if (i && ImGui::GetItemRectMax().x + 8 + w < right) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, off ? ImVec4(0.2f, 0.2f, 0.2f, 1) : ImVec4(0.18f, 0.35f, 0.55f, 1));
        if (ImGui::SmallButton(tags[i].c_str())) {
            if (off) hidden.erase(std::remove(hidden.begin(), hidden.end(), tags[i]), hidden.end());
            else hidden.push_back(tags[i]);
        }
        ImGui::PopStyleColor();
    }
    static std::deque<LogLine> frozen;
    static std::vector<const LogLine *> rows;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!paused) frozen = g_log;
    }
    rows.clear();
    for (auto &l : frozen) {
        if (only_last && l.session != g_session) continue;
        if (!l.tag.empty() && std::find(hidden.begin(), hidden.end(), l.tag) != hidden.end()) continue;
        if (filter[0] && l.text.find(filter) == std::string::npos) continue;
        rows.push_back(&l);
    }
    // a graph's point clicked: to its moment, the lines around it lit (not following the end meanwhile)
    static double jumped = -1;
    double jt = g_jump_t.load();
    int jump_row = -1;
    if (jt >= 0 && jt != jumped) {
        jumped = jt, follow = false;
        for (size_t i = 0; i < rows.size(); i++) if (rows[i]->t >= jt - 0.3) { jump_row = (int)i; break; }
        if (jump_row < 0 && !rows.empty()) jump_row = (int)rows.size() - 1;
    }
    if (jt >= 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "at %.2f s", jt);
        ImGui::SameLine();
        if (ImGui::SmallButton("back to the end")) { g_jump_t = -1; jumped = -1; follow = true; }
    }
    ImGui::BeginChild("##lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    float lh = ImGui::GetTextLineHeightWithSpacing();
    if (jump_row >= 0) ImGui::SetScrollY(std::max(0.0f, jump_row * lh - ImGui::GetWindowHeight() / 3));
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const LogLine *l = rows[i];
            if (jt >= 0 && fabs(l->t - jt) < 0.3) {
                ImVec2 c = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(c, ImVec2(c.x + ImGui::GetWindowWidth() * 4, c.y + lh), IM_COL32(110, 85, 20, 140));
            }
            const std::string &s = l->text;
            ImVec4 c = s.find("FATAL") != std::string::npos || s.find("exception") != std::string::npos ||
                               s.find("error") != std::string::npos ? ImVec4(1, 0.45f, 0.4f, 1)
                       : l->tag == "[devtools]" || l->tag == "[tool]" ? ImVec4(0.95f, 0.8f, 0.35f, 1)
                       : l->tag == "[vk]" ? ImVec4(0.6f, 0.8f, 1, 1) : ImVec4(0.85f, 0.85f, 0.85f, 1);
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "%9.2f", l->t);
            ImGui::SameLine();
            ImGui::TextColored(c, "%s", s.c_str());
        }
    if (follow && !paused && jump_row < 0) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

static void panel_perf() {
    std::vector<float> all;
    std::vector<double> allt;
    std::vector<VkStat> vk;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        all = g_ft, allt = g_ft_t;
        vk.assign(g_vk.begin(), g_vk.end());
    }
    int n = (int)std::min<size_t>(std::min(all.size(), allt.size()), 1200);
    std::vector<float> v(all.end() - n, all.end());
    std::vector<double> vt(allt.end() - n, allt.end());
    float avg = 0, worst = 0;
    for (float x : v) avg += x, worst = std::max(worst, x);
    if (n) avg /= n;
    std::vector<float> s = v;
    std::sort(s.begin(), s.end());
    float p99 = n ? s[(size_t)std::min(n - 1, (int)(n * 0.99f))] : 0;
    ImGui::SeparatorText("Frames (the last 20 s)");
    ImGui::Text("avg %.1f ms (%.0f fps)   1%% worst %.1f ms (%.0f fps)   worst %.1f ms", avg, avg > 0 ? 1000 / avg : 0, p99,
                p99 > 0 ? 1000 / p99 : 0, worst);
    plot_t("##ft", v.data(), vt.data(), n, "ms per presented frame (16.7 = 60 fps, 33.3 = 30 fps)", 0, 66.7f,
           ImVec2(-FLT_MIN, 140));
    ImGui::SeparatorText("Drop trigger and captures");
    if (ImGui::Checkbox("Drop trigger", &g_trig_on)) { char c[48]; snprintf(c, sizeof c, "trigger %d %.0f", g_trig_on, g_trig_ms); send_command(c); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("##ms", &g_trig_ms, 20, 500, "frame > %.0f ms") && g_trig_on) {
        char c[48]; snprintf(c, sizeof c, "trigger %d %.0f", g_trig_on, g_trig_ms); send_command(c);
    }
    ImGui::SameLine();
    if (ImGui::Button("Capture now")) send_command("capture");
    ImGui::SameLine();
    if (ImGui::Button("F9 frame")) send_command("f9");
    ImGui::TextDisabled("A capture writes captures/<time>/ next to the game: log.txt, frames.csv (+ F9 passes with LARECOMP_DEBUG_FRAMES).");
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto it = g_caps.rbegin(); it != g_caps.rend() && it - g_caps.rbegin() < 10; ++it) ImGui::BulletText("%s", it->c_str());
    }
    ImGui::SeparatorText("The game every 2 s ([vk] stats)");
    if (vk.empty()) { ImGui::TextDisabled("(none yet: they come every 2 s)"); return; }
    const VkStat &l = vk.back();
    ImGui::Text("%.1f fps, worst frame %d ms, %ld draws, %ld passes, %ld dispatches, upload %.1f MB", l.fps, l.max_ms,
                l.draws, l.passes, l.dispatches, l.upload_mb);
    ImGui::Text("render waited for the game %d ms of 2000 (%.0f%%): %s", l.wait_ms, l.wait_ms / 20.0,
                l.wait_ms > 800 ? "the game code (CPU) is the limit" : "the GPU side is busy");
    std::vector<float> fps, wait, draws;
    std::vector<double> ts;
    for (auto &x : vk) fps.push_back((float)x.fps), wait.push_back(x.wait_ms / 20.0f), draws.push_back((float)x.draws), ts.push_back(x.t);
    float w = ImGui::GetContentRegionAvail().x;
    plot_t("##fps", fps.data(), ts.data(), (int)fps.size(), "fps", 0, 65, ImVec2(w, 100));
    plot_t("##wait", wait.data(), ts.data(), (int)wait.size(), "render waiting for the game, %", 0, 100, ImVec2(w, 100));
    plot_t("##draws", draws.data(), ts.data(), (int)draws.size(), "draws per 2 s", 0, FLT_MAX, ImVec2(w, 100));
}

// ------------------------------------------------------------------ Frame: an F9 capture, browsable
static std::string game_file(const std::string &rel) {  // (a path the game wrote, relative to its working folder)
    std::string p = g_root + rel;
    for (auto &c : p) if (c == '/') c = '\\';
    return p;
}
// "verts N extent E bad B ..." (runtime/nvn/vulkan/exec.c debug_vscan) from a draw's text; false when it has none
struct VScan { unsigned long long verts, bad; double extent; };
static bool vscan_of(const std::string &d, VScan *v) {
    size_t at = d.find(" || verts ");
    if (at == std::string::npos) return false;
    return sscanf(d.c_str() + at + 4, "verts %llu extent %lf bad %llu", &v->verts, &v->extent, &v->bad) == 3;
}
static bool suspect(const VScan &v) { return v.bad > 0 || v.extent > 5000; }

// build/fullcap_<frame>.bin (runtime/nvn/vulkan/exec.c fullcap_draw), read once per capture
static std::map<std::string, std::vector<FcDraw>> g_fullcaps;
static const std::vector<FcDraw> *fullcap_of(const std::string &rel) {
    if (rel.empty()) return NULL;
    auto it = g_fullcaps.find(rel);
    if (it != g_fullcaps.end()) return &it->second;
    std::vector<FcDraw> &v = g_fullcaps[rel];
    FILE *f = fopen(game_file(rel).c_str(), "rb");
    if (!f) return &v;
    char magic[8];
    uint64_t frame;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "ZAFC3", 5) || fread(&frame, 8, 1, f) != 1) { fclose(f); return &v; }
    std::map<std::pair<uint32_t, uint64_t>, std::vector<uint8_t>> seen;  // (kind, address): a block written once, referred to after
    for (;;) {
        uint32_t head[2];
        if (fread(head, 4, 2, f) != 2) break;
        if (head[0] != 0x57415244u) {  // (a dispatch record, 'DISB' / 'DISA': not shown here yet)
            if (fseek(f, (long)head[1], SEEK_CUR)) break;
            continue;
        }
        FcDraw d;
        uint32_t a[2], b[6];
        if (fread(a, 4, 2, f) != 2) break;
        d.draw = a[0], d.pass = a[1];
        fread(&d.vs, 8, 1, f), fread(&d.fs, 8, 1, f), fread(&d.indexed, 4, 1, f), fread(&d.mode, 4, 1, f), fread(&d.count, 8, 1, f);
        if (fread(b, 4, 6, f) != 6) break;
        d.inst = b[0], d.gpu = b[1], d.cfmt = b[2], d.dfmt = b[3];
        size_t g = (size_t)b[4] * b[5] * 16;
        d.cg.resize(g), d.dg.resize(g);
        fread(d.cg.data(), 1, g, f), fread(d.dg.data(), 1, g, f);
        fseek(f, (long)(2 * g), SEEK_CUR);  // (ZAFC3: the grids just before the draw)
        uint32_t nb = 0;
        fread(&nb, 4, 1, f);
        for (uint32_t i = 0; i < nb; i++) {
            FcBlob bl;
            uint32_t len;
            uint64_t hash = 0;
            fread(&bl.kind, 4, 1, f), fread(&bl.slot, 4, 1, f), fread(&bl.addr, 8, 1, f), fread(&bl.size, 8, 1, f), fread(&len, 4, 1, f);
            if (bl.kind != 6 && bl.kind != 7) fread(&hash, 8, 1, f);  // (texture tables carry no hash)
            if (len == 0xFFFFFFFFu) {  // (the same bytes as an earlier draw's block: same kind, address and hash)
                auto it = seen.find({bl.kind, bl.addr ^ hash});
                if (it != seen.end()) bl.data = it->second;
            } else {
                bl.data.resize(len);
                if (len && fread(bl.data.data(), 1, len, f) != len) break;
                seen[{bl.kind, bl.addr ^ hash}] = bl.data;
            }
            d.blobs.push_back(std::move(bl));
        }
        v.push_back(std::move(d));
    }
    fclose(f);
    return &v;
}
// draws that changed grid cell (gx, gy) of their colour or depth target (compared with the draw before in the same pass)
static std::vector<std::pair<int, int>> touched(const std::vector<FcDraw> &fc, int gx, int gy) {  // (index, 1 colour 2 depth 3 both)
    std::vector<std::pair<int, int>> out;
    size_t c = ((size_t)gy * 48 + gx) * 16;
    for (size_t i = 0; i < fc.size(); i++) {
        if (c + 16 > fc[i].cg.size()) continue;
        static const uint8_t zero[16] = {};
        bool first = i == 0 || fc[i - 1].pass != fc[i].pass;
        const uint8_t *pc = first ? zero : &fc[i - 1].cg[c], *pd = first ? zero : &fc[i - 1].dg[c];
        int m = (memcmp(&fc[i].cg[c], pc, 16) ? 1 : 0) | (memcmp(&fc[i].dg[c], pd, 16) ? 2 : 0);
        if (m) out.push_back({(int)i, m});
    }
    return out;
}
static const char *const BLOB_KIND[] = {"?", "vertex cbuf", "fragment cbuf", "vertex storage", "fragment storage", "vertex stream",
                                         "vertex textures", "fragment textures", "indices", "vertex GLSL", "fragment GLSL",
                                         "vertex code", "fragment code", "GPU draw parameters", "vertex layout",
                                         "vertex c1 (constants)", "fragment c1 (constants)"};
struct FcTex { uint32_t index, slot, tic, format; uint64_t handle, object, addr; uint32_t width, height, levels, pad; };  // (exec.c)
static void blob_view(const FcBlob &b) {  // as floats / hex / ints, 4 per row; textures as a table
    if (b.kind == 6 || b.kind == 7) {
        size_t n = b.data.size() / sizeof(FcTex);
        ImGui::BeginChild("##blob", ImVec2(0, 220), ImGuiChildFlags_Borders);
        for (size_t i = 0; i < n; i++) {
            FcTex t;
            memcpy(&t, &b.data[i * sizeof t], sizeof t);
            if (t.object) ImGui::Text("tex %u: slot %d handle %llx pool entry %u -> %ux%u fmt %#x levels %u, data at %llx (object %llx)", t.index,
                                      (int)t.slot, (unsigned long long)t.handle, t.tic, t.width, t.height, t.format, t.levels,
                                      (unsigned long long)t.addr, (unsigned long long)t.object);
            else ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "tex %u: slot %d handle %llx pool entry %u -> NOTHING", t.index, (int)t.slot,
                                    (unsigned long long)t.handle, t.tic);
        }
        ImGui::EndChild();
        return;
    }
    if (b.kind == 9 || b.kind == 10) {  // shader source
        static std::string txt;
        txt.assign((const char *)b.data.data(), b.data.size());
        ImGui::InputTextMultiline("##glsl", (char *)txt.c_str(), txt.size() + 1, ImVec2(-FLT_MIN, 320), ImGuiInputTextFlags_ReadOnly);
        return;
    }
    static int as = 0;
    ImGui::RadioButton("float", &as, 0); ImGui::SameLine(); ImGui::RadioButton("hex", &as, 1); ImGui::SameLine(); ImGui::RadioButton("int", &as, 2);
    ImGui::SameLine();
    ImGui::TextDisabled("%s %u at %llx, %llu bytes (%zu kept)", BLOB_KIND[b.kind < 17 ? b.kind : 0], b.slot, (unsigned long long)b.addr,
                        (unsigned long long)b.size, b.data.size());
    ImGui::BeginChild("##blob", ImVec2(0, 220), ImGuiChildFlags_Borders);
    int rows = (int)(b.data.size() / 16);
    ImGuiListClipper clip;
    clip.Begin(rows);
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            const uint8_t *p = &b.data[r * 16];
            float fv[4]; uint32_t uv[4]; int32_t iv[4];
            memcpy(fv, p, 16), memcpy(uv, p, 16), memcpy(iv, p, 16);
            if (as == 0) ImGui::Text("%5x  %12.5g %12.5g %12.5g %12.5g", r * 16, fv[0], fv[1], fv[2], fv[3]);
            else if (as == 1) ImGui::Text("%5x  %08x %08x %08x %08x", r * 16, uv[0], uv[1], uv[2], uv[3]);
            else ImGui::Text("%5x  %11d %11d %11d %11d", r * 16, iv[0], iv[1], iv[2], iv[3]);
        }
    ImGui::EndChild();
}

static int g_cell_x = -1, g_cell_y = -1;
static unsigned long long g_cell_frame;
static void panel_frame() {
    static int sel_frame = -1, sel_pass = 0, sel_draw = -1;
    static size_t known = 0;
    static float zoom = 1;
    static bool fit = true;
    if (ImGui::Button("Capture frame (F9)")) send_command("f9");
    ImGui::SameLine();
    ImGui::TextDisabled("the game stops for a moment while it writes every pass of one frame (build/pass_*.png in the work folder)");
    FrameCap fc;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_frames.size() != known) { known = g_frames.size(); sel_frame = (int)known - 1, sel_pass = 0, sel_draw = -1; }
        if (sel_frame < 0 || sel_frame >= (int)g_frames.size()) { ImGui::TextDisabled("No capture yet."); return; }
        if (ImGui::BeginCombo("Capture", ("frame " + std::to_string(g_frames[sel_frame].frame)).c_str())) {
            for (int i = (int)g_frames.size() - 1; i >= 0; i--) {
                char l[96];
                snprintf(l, sizeof l, "frame %llu  (%.1f s, %zu passes)", g_frames[i].frame, g_frames[i].t, g_frames[i].passes.size());
                if (ImGui::Selectable(l, i == sel_frame)) sel_frame = i, sel_pass = 0, sel_draw = -1;
            }
            ImGui::EndCombo();
        }
        fc = g_frames[sel_frame];
    }
    if (fc.passes.empty()) { ImGui::TextDisabled("(its passes are still being written...)"); return; }
    if (sel_pass >= (int)fc.passes.size()) sel_pass = 0;
    // suspects: draws whose position data is wild (NaN / huge) or spread over more than 5000 units, worst first
    {
        struct S { int pass, draw; VScan v; };
        std::vector<S> sus;
        for (int pi = 0; pi < (int)fc.passes.size(); pi++)
            for (int di = 0; di < (int)fc.passes[pi].draws.size(); di++) {
                VScan v;
                if (vscan_of(fc.passes[pi].draws[di], &v) && suspect(v)) sus.push_back({pi, di, v});
            }
        std::sort(sus.begin(), sus.end(), [](const S &a, const S &b) { return a.v.bad != b.v.bad ? a.v.bad > b.v.bad : a.v.extent > b.v.extent; });
        char h[96];
        snprintf(h, sizeof h, "Suspects: %zu draws with wild or far-spread positions###sus", sus.size());
        if (ImGui::CollapsingHeader(h, sus.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginChild("##sus", ImVec2(0, std::min(160.0f, 24.0f + 20 * sus.size())), ImGuiChildFlags_Borders);
            for (size_t i = 0; i < sus.size() && i < 200; i++) {
                char l[160];
                snprintf(l, sizeof l, "pass %d draw %d: %llu bad values, extent %.0f, %llu vertices", sus[i].pass, sus[i].draw,
                         sus[i].v.bad, sus[i].v.extent, sus[i].v.verts);
                if (ImGui::Selectable(l, sel_pass == sus[i].pass && sel_draw == sus[i].draw)) sel_pass = sus[i].pass, sel_draw = sus[i].draw;
            }
            if (sus.empty()) ImGui::TextDisabled("(none: the position data is sane - a stretch then comes from the transform)");
            ImGui::EndChild();
        }
    }
    // probe hits: the draws that painted the spot clicked, in order (draw N = the frame's Nth draw)
    if (!fc.probe.empty()) {
        char h[96];
        snprintf(h, sizeof h, "Probe: %zu draws painted the spot###probe", fc.probe.size());
        if (ImGui::CollapsingHeader(h, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginChild("##probe", ImVec2(0, std::min(180.0f, 24.0f + 20 * fc.probe.size())), ImGuiChildFlags_Borders);
            for (size_t i = 0; i < fc.probe.size(); i++) {
                const ProbeHit &ph = fc.probe[i];
                // the frame's Nth draw: which pass / draw of the table
                int left = ph.draw - 1, pi = 0;
                while (pi < (int)fc.passes.size() && left >= (int)fc.passes[pi].draws.size()) left -= (int)fc.passes[pi].draws.size(), pi++;
                char l[200];
                snprintf(l, sizeof l, "%2zu. draw %d (pass %d #%d) target %d: %.120s", i + 1, ph.draw, pi, left, ph.target, ph.text.c_str());
                if (ImGui::Selectable(l, false) && pi < (int)fc.passes.size()) sel_pass = pi, sel_draw = left;
                if (ImGui::IsItemHovered()) {
                    Img im = image(game_file(ph.png));
                    if (im.tex) {
                        ImGui::BeginTooltip();
                        ImGui::Image((ImTextureID)(intptr_t)im.tex, ImVec2(480, 480.0f * im.h / im.w));
                        ImGui::EndTooltip();
                    }
                }
            }
            ImGui::EndChild();
        }
    }
    // full capture: the draws that touched the clicked spot, and the selected draw's data
    const std::vector<FcDraw> *full = fullcap_of(fc.fullcap);
    if (full && !full->empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "Full capture: %zu draws recorded. Click a spot of a pass image below.", full->size());
        if (g_cell_x >= 0 && g_cell_frame == fc.frame) {
            auto hits = touched(*full, g_cell_x, g_cell_y);
            char h[96];
            snprintf(h, sizeof h, "Spot %d,%d (of 48x27): %zu draws touched it###touch", g_cell_x, g_cell_y, hits.size());
            if (ImGui::CollapsingHeader(h, ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::BeginChild("##touch", ImVec2(0, std::min(200.0f, 24.0f + 20 * hits.size())), ImGuiChildFlags_Borders);
                for (auto &ht : hits) {
                    const FcDraw &d = (*full)[ht.first];
                    char l[220];
                    snprintf(l, sizeof l, "draw %u (pass %u) %s  vs %llx fs %llx  count %llu x%u%s%s", d.draw, d.pass,
                             ht.second == 3 ? "colour+depth" : ht.second == 1 ? "colour" : "depth", (unsigned long long)d.vs,
                             (unsigned long long)d.fs, (unsigned long long)d.count, d.inst, d.gpu ? " (GPU params)" : "",
                             d.indexed ? " idx" : "");
                    if (ImGui::Selectable(l, false)) {
                        int left = (int)d.draw - 1, pi = 0;
                        while (pi < (int)fc.passes.size() && left >= (int)fc.passes[pi].draws.size()) left -= (int)fc.passes[pi].draws.size(), pi++;
                        if (pi < (int)fc.passes.size()) sel_pass = pi, sel_draw = left;
                    }
                }
                ImGui::EndChild();
            }
        }
    } else if (!fc.fullcap.empty()) {
        ImGui::TextDisabled("(full capture: %s not readable yet)", fc.fullcap.c_str());
    }
    // passes (left)
    ImGui::BeginChild("##passes", ImVec2(230, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    for (int i = 0; i < (int)fc.passes.size(); i++) {
        const Pass &p = fc.passes[i];
        char l[96];
        snprintf(l, sizeof l, "%3d  %dx%d  %zu draws", i, p.w, p.h, p.draws.size());
        if (ImGui::Selectable(l, i == sel_pass)) sel_pass = i, sel_draw = -1;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", p.png.c_str(), p.info.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // the pass's image (top) and its draws (bottom)
    ImGui::BeginChild("##pass", ImVec2(0, 0));
    const Pass &p = fc.passes[sel_pass];
    ImGui::Text("pass %d: %s%s", sel_pass, p.png.c_str(), p.info.c_str());
    ImGui::Checkbox("Fit", &fit);
    if (!fit) { ImGui::SameLine(); ImGui::SetNextItemWidth(160); ImGui::SliderFloat("zoom", &zoom, 0.1f, 8, "%.2fx", ImGuiSliderFlags_Logarithmic); }
    Img im = image(game_file(p.png));
    float avail_h = ImGui::GetContentRegionAvail().y * 0.6f;
    ImGui::BeginChild("##img", ImVec2(0, avail_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (im.tex) {
        float k = fit ? std::min(ImGui::GetContentRegionAvail().x / im.w, ImGui::GetContentRegionAvail().y / im.h) : zoom;
        ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)(intptr_t)im.tex, ImVec2(im.w * k, im.h * k));
        if (ImGui::IsItemHovered()) {
            ImVec2 m = ImGui::GetIO().MousePos;
            ImGui::SetTooltip("pixel %d, %d%s", (int)((m.x - o.x) / k), (int)((m.y - o.y) / k),
                              fc.fullcap.empty() ? "" : "\nclick: the draws that touched this spot");
            if (ImGui::IsItemClicked() && !fc.fullcap.empty()) {
                g_cell_x = std::min(47, std::max(0, (int)((m.x - o.x) / k / im.w * 48)));
                g_cell_y = std::min(26, std::max(0, (int)((m.y - o.y) / k / im.h * 27)));
                g_cell_frame = fc.frame;
            }
        }
    } else {
        ImGui::TextDisabled("(no image: %s)", game_file(p.png).c_str());
    }
    ImGui::EndChild();
    // draws: "e inst 1 mode 4 count 36 prog vs=... fs=... | vs cb .. fs cb .. | rt 1 WxH/f.. | vp ... | attrs .."
    if (ImGui::BeginTable("##draws", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, ImGui::GetContentRegionAvail().y - (sel_draw >= 0 ? 160 : 0)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("count / inst", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("mode", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("vertex shader", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("fragment shader", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("verts / extent / bad", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn("state");
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)p.draws.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const std::string &d = p.draws[i];
                char kind = 'a';
                unsigned long long inst = 0, mode = 0, count = 0, vs = 0, fs = 0;
                sscanf(d.c_str(), "%c inst %llu mode %llu count %llu prog vs=%llx fs=%llx", &kind, &inst, &mode, &count, &vs, &fs);
                size_t bar1 = d.find('|');
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char id[16];
                snprintf(id, sizeof id, "%d", i);
                if (ImGui::Selectable(id, sel_draw == i, ImGuiSelectableFlags_SpanAllColumns)) sel_draw = i;
                ImGui::TableNextColumn(); ImGui::Text("%llu x%llu%s", count, inst, kind == 'e' ? " idx" : "");
                ImGui::TableNextColumn(); ImGui::Text("%llu", mode);
                ImGui::TableNextColumn(); ImGui::Text("%llx", vs);
                ImGui::TableNextColumn(); ImGui::Text("%llx", fs);
                ImGui::TableNextColumn();
                VScan scan;
                if (vscan_of(d, &scan)) {
                    if (suspect(scan)) ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "%llu / %.0f / %llu", scan.verts, scan.extent, scan.bad);
                    else ImGui::Text("%llu / %.0f / %llu", scan.verts, scan.extent, scan.bad);
                } else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                {
                    size_t sc = std::min(d.find(" || verts "), d.find('\n'));
                    std::string st = bar1 == std::string::npos ? "" : d.substr(bar1 + 2, sc == std::string::npos ? std::string::npos : sc - bar1 - 2);
                    ImGui::TextUnformatted(st.c_str());
                }
            }
        ImGui::EndTable();
    }
    if (sel_draw >= 0 && sel_draw < (int)p.draws.size() && full && !full->empty()) {
        int n = sel_draw;
        for (int pi = 0; pi < sel_pass; pi++) n += (int)fc.passes[pi].draws.size();
        if (n < (int)full->size()) {
            const FcDraw &d = (*full)[n];
            static int sel_blob = 0;
            if (sel_blob >= (int)d.blobs.size()) sel_blob = 0;
            ImGui::Text("Data of draw %u:", d.draw);
            for (int i = 0; i < (int)d.blobs.size(); i++) {
                char l[64];
                snprintf(l, sizeof l, "%s %u##b%d", BLOB_KIND[d.blobs[i].kind < 17 ? d.blobs[i].kind : 0], d.blobs[i].slot, i);
                ImGui::SameLine();
                if (ImGui::SmallButton(l)) sel_blob = i;
            }
            if (!d.blobs.empty()) blob_view(d.blobs[sel_blob]);
        }
    }
    if (sel_draw >= 0 && sel_draw < (int)p.draws.size()) {
        static std::string txt;
        txt = "draw " + std::to_string(sel_draw) + ": " + p.draws[sel_draw];
        ImGui::InputTextMultiline("##draw", (char *)txt.c_str(), txt.size() + 1, ImVec2(-FLT_MIN, 150), ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ Memory: the game's memory (guest = host address)
struct Watch { char name[32]; unsigned long long addr; int type; std::deque<float> hist; std::deque<double> ht; };
static std::vector<Watch> g_watches;
static const char *const TYPES[] = {"u32", "s32", "f32", "u64", "f64", "u8"};
static bool read_mem(unsigned long long a, void *out, size_t n) {
    SIZE_T got = 0;
    return game_running() && ReadProcessMemory(g_game, (LPCVOID)(uintptr_t)a, out, n, &got) && got == n;
}
static double value_of(unsigned long long a, int type, bool *ok) {
    uint8_t b[8] = {};
    *ok = read_mem(a, b, type >= 3 && type <= 4 ? 8 : type == 5 ? 1 : 4);
    switch (type) {
    case 0: { uint32_t v; memcpy(&v, b, 4); return v; }
    case 1: { int32_t v; memcpy(&v, b, 4); return v; }
    case 2: { float v; memcpy(&v, b, 4); return v; }
    case 3: { uint64_t v; memcpy(&v, b, 8); return (double)v; }
    case 4: { double v; memcpy(&v, b, 8); return v; }
    default: return b[0];
    }
}
static void panel_memory() {
    static char at[24] = "7000000000";
    static int rows = 48;
    if (!game_running()) { ImGui::TextDisabled("Start the game first (the tool reads the memory of the game it started)."); return; }
    ImGui::SetNextItemWidth(170);
    ImGui::InputText("address (hex)", at, sizeof at, ImGuiInputTextFlags_CharsHexadecimal);
    unsigned long long base = strtoull(at, NULL, 16) & ~0xFull;
    ImGui::SameLine(); if (ImGui::Button("-0x100")) snprintf(at, sizeof at, "%llx", base - 0x100);
    ImGui::SameLine(); if (ImGui::Button("+0x100")) snprintf(at, sizeof at, "%llx", base + 0x100);
    ImGui::SameLine(); ImGui::TextDisabled("(Z-A: guest addresses are the process's own; main 7000000000, heap 76..)");
    std::vector<uint8_t> buf(rows * 16);
    bool ok = read_mem(base, buf.data(), buf.size());
    ImGui::BeginChild("##hex", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f), ImGuiChildFlags_Borders);
    if (!ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "can't read %llx (not mapped)", base);
    else
        for (int r = 0; r < rows; r++) {
            char line[160];
            int n = snprintf(line, sizeof line, "%010llx ", base + r * 16);
            for (int i = 0; i < 16; i++) n += snprintf(line + n, sizeof line - n, "%s%02x", i % 4 ? " " : "  ", buf[r * 16 + i]);
            n += snprintf(line + n, sizeof line - n, "  ");
            for (int i = 0; i < 16; i++) { uint8_t c = buf[r * 16 + i]; line[n++] = c >= 32 && c < 127 ? (char)c : '.'; }
            line[n] = 0;
            ImGui::TextUnformatted(line);
            if (ImGui::IsItemHovered()) {  // the row as floats / ints
                float f[4]; uint32_t u[4]; uint64_t q[2];
                memcpy(f, &buf[r * 16], 16), memcpy(u, &buf[r * 16], 16), memcpy(q, &buf[r * 16], 16);
                ImGui::SetTooltip("f32: %g  %g  %g  %g\nu32: %u  %u  %u  %u\nu64: %llx  %llx\n(right click: watch the first)",
                                  f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3], (unsigned long long)q[0], (unsigned long long)q[1]);
                if (ImGui::IsMouseClicked(1)) {
                    Watch w = {};
                    snprintf(w.name, sizeof w.name, "w%zu", g_watches.size());
                    w.addr = base + r * 16, w.type = 2;
                    g_watches.push_back(w);
                }
            }
        }
    ImGui::EndChild();
    // watches: a value every frame, its last 600 samples
    ImGui::SeparatorText("Watches");
    static char wa[24] = "", wn[32] = "";
    static int wt = 2;
    ImGui::SetNextItemWidth(120); ImGui::InputTextWithHint("##wn", "name", wn, sizeof wn); ImGui::SameLine();
    ImGui::SetNextItemWidth(150); ImGui::InputTextWithHint("##wa", "address (hex)", wa, sizeof wa, ImGuiInputTextFlags_CharsHexadecimal); ImGui::SameLine();
    ImGui::SetNextItemWidth(70); ImGui::Combo("##wt", &wt, TYPES, 6); ImGui::SameLine();
    if (ImGui::Button("Add watch") && wa[0]) {
        Watch w = {};
        snprintf(w.name, sizeof w.name, "%s", wn[0] ? wn : wa);
        w.addr = strtoull(wa, NULL, 16), w.type = wt;
        g_watches.push_back(w);
    }
    if (ImGui::BeginTable("##w", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("address", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("history");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < g_watches.size(); i++) {
            Watch &w = g_watches[i];
            bool vok;
            double v = value_of(w.addr, w.type, &vok);
            w.hist.push_back((float)v);
            w.ht.push_back(game_now());
            if (w.hist.size() > 600) w.hist.pop_front(), w.ht.pop_front();
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputText("##n", w.name, sizeof w.name);
            ImGui::TableNextColumn(); ImGui::Text("%llx", w.addr);
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::Combo("##t", &w.type, TYPES, 6);
            ImGui::TableNextColumn();
            if (vok) ImGui::Text(w.type == 2 || w.type == 4 ? "%.6g" : "%.0f", v); else ImGui::TextDisabled("unreadable");
            ImGui::TableNextColumn();
            std::vector<float> h(w.hist.begin(), w.hist.end());
            std::vector<double> ht(w.ht.begin(), w.ht.end());
            plot_t("##h", h.data(), ht.data(), (int)h.size(), NULL, FLT_MAX, FLT_MAX, ImVec2(-60, 30));
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { g_watches.erase(g_watches.begin() + i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ Repros: a bug, taken back to
static void panel_repros() {
    static bool loaded;
    if (!loaded) loaded = true, load_repros();
    ImGui::TextWrapped("Every Start records the session (the save at the start, the clock, the pad read by read). When the "
                       "bug is on screen click \"Mark bug here\" (top bar): the session up to that moment is kept here, with an F9 "
                       "capture of that frame. \"Replay to the bug\" starts a fresh copy of that save, plays the recording up to "
                       "the mark and gives you the controls there.");
    ImGui::TextDisabled("(replays can drift a little over long runs: NPCs and the camera are not always identical)");
    if (ImGui::Button("Refresh")) load_repros();
    if (!g_replaying.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "replaying %s", g_replaying.c_str()); }
    if (ImGui::BeginTable("##repros", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("repro", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("mark", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("note");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < g_repros.size(); i++) {
            Repro &r = g_repros[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("read %llu / frame %llu", r.read, r.frame);
            ImGui::TableNextColumn();
            char note[256];
            snprintf(note, sizeof note, "%s", r.note.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##note", "what it shows (giant stretched geometry, ...)", note, sizeof note)) {
                r.note = note;
                write_text(r.dir + "\note.txt", r.note);
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(game_running());
            if (ImGui::SmallButton("Replay to the bug")) replay_repro(r);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Folder")) ShellExecuteA(NULL, "open", r.dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ the window
int main(int argc, char **argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window *win = SDL_CreateWindow("ZA DevTools", 1700, 1000, SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
    g_tool_hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    SetWindowLongPtrA(g_tool_hwnd, GWL_STYLE, GetWindowLongPtrA(g_tool_hwnd, GWL_STYLE) | WS_CLIPCHILDREN);  // (the game on top)
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    g_ren = ren;
    SDL_SetRenderVSync(ren, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    float dpi = SDL_GetWindowDisplayScale(win);
    if (dpi <= 0) dpi = 1;
    if (FILE *f = fopen("C:/Windows/Fonts/segoeui.ttf", "rb")) { fclose(f); io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 16 * dpi); }
    if (FILE *f = fopen("C:/Windows/Fonts/consola.ttf", "rb")) { fclose(f); }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(dpi);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    {   // where things are: devtools.ini next to this exe
        char p[MAX_PATH];
        GetModuleFileNameA(NULL, p, sizeof p);
        std::string s = p;
        s = s.substr(0, s.find_last_of('\\'));
        load_config(s + "\\devtools.ini", s);
    }
    std::thread(reader_thread).detach();
    for (int i = 1; i + 1 < argc; i++)  // --start <profile number>: the game started at once (a shortcut per profile)
        if (!strcmp(argv[i], "--start")) { g_profile = std::min(std::max(atoi(argv[i + 1]), 0), (int)PROFILES.size() - 1); game_start(); }
        else if (!strcmp(argv[i], "--replay")) {  // --replay <repro folder name>: that repro replayed at once
            load_repros();
            for (const Repro &r : g_repros)
                if (r.dir.size() >= strlen(argv[i + 1]) && r.dir.compare(r.dir.size() - strlen(argv[i + 1]), std::string::npos, argv[i + 1]) == 0) { replay_repro(r); break; }
        }
    float split = 0.62f, split_y = 0.66f;
    for (bool run = true; run;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) run = false;
        }
        if (g_mark_read.load() != ~0ull) {  // (the game said "M": the session becomes a repro)
            unsigned long long rd = g_mark_read.exchange(~0ull);
            make_repro(rd, g_mark_frame.load(), g_mark_key.load());
            load_repros();
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        float W = io.DisplaySize.x, H = io.DisplaySize.y, bar = 0;
        if (ImGui::BeginMainMenuBar()) {
            bar = ImGui::GetWindowSize().y;
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 1, 1), "ZA DevTools");
            ImGui::Separator();
            bool running = game_running();
            ImGui::SetNextItemWidth(260);
            ImGui::BeginDisabled(running);
            if (ImGui::BeginCombo("##profile", PROFILES[g_profile].name.c_str())) {
                for (int i = 0; i < (int)PROFILES.size(); i++)
                    if (ImGui::Selectable(PROFILES[i].name.c_str(), i == g_profile)) g_profile = i;
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(120);
            ImGui::Combo("##lib", &g_lib_kind, LIB_KINDS, 2);
            ImGui::Checkbox("Race test (GPU sync)", &g_race_test);
            ImGui::Checkbox("F9: full capture", &g_draw_data);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("F9 records everything of the frame: after each draw a grid of its colour / depth targets, and its\n"
                                  "constants, storage buffers and vertex streams. Frame tab: click any spot of a pass image to list\n"
                                  "the draws that touched it; a draw's data in its details. Slower F9, ~80 MB per capture.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every command list waited for on the GPU: no CPU frame ahead. If a bug never shows with this on,\n"
                                  "it is a race between the game writing the next frame's data and the GPU reading this one's. Slower.");
            ImGui::EndDisabled();
            ImGui::Checkbox("Game inside", &g_embed);
            if (!running) { if (ImGui::Button(" Start ")) game_start(); }
            else {
                if (ImGui::Button(" Stop ")) game_stop();
                ImGui::SameLine();
                if (ImGui::Button(" Restart ")) { game_stop(); game_start(); }
            }
            if (running) {
                ImGui::SameLine();
                ImGui::BeginDisabled(!g_connected || g_session_dir.empty());
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.15f, 1));
                if (ImGui::Button(" Mark bug here ")) send_command("mark");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, g_probe_armed ? ImVec4(0.85f, 0.6f, 0.1f, 1) : ImVec4(0.25f, 0.35f, 0.55f, 1));
                if (ImGui::Button(g_probe_armed ? " Now click the bug on the game " : " Probe: click the bug ")) g_probe_armed = !g_probe_armed;
                ImGui::PopStyleColor();
                ImGui::EndDisabled();
            }
            ImGui::Separator();
            if (g_connected) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), running ? "game running (pid %lu), connected" : "game connected", g_game_pid);
            else if (running) ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1), "game starting (pid %lu)...", g_game_pid);
            else ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "no game: choose a profile and Start");
            ImGui::Separator();
            std::lock_guard<std::mutex> lk(g_lock);
            int n = (int)std::min<size_t>(g_ft.size(), 60);
            float avg = 0;
            for (int i = 0; i < n; i++) avg += g_ft[g_ft.size() - 1 - i];
            if (n) ImGui::Text("%.0f fps (%.1f ms)", 1000 / (avg / n), avg / n);
            ImGui::EndMainMenuBar();
        }
        const ImGuiWindowFlags P = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
        float lw = W * split, gh = (H - bar) * split_y;
        // Game (top left): the game's own window is placed on this panel (embed_update)
        ImGui::SetNextWindowPos(ImVec2(0, bar));
        ImGui::SetNextWindowSize(ImVec2(lw - 3, gh - 3));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin("Game", NULL, P | ImGuiWindowFlags_NoScrollbar)) {
            ImVec2 a = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
            // 16:9 in the panel
            float gw2 = av.x, gh2 = av.x * 9 / 16;
            if (gh2 > av.y) gh2 = av.y, gw2 = av.y * 16 / 9;
            float ox = a.x + (av.x - gw2) / 2, oy = a.y + (av.y - gh2) / 2;
            if (!game_running()) {  // (no game: the two ways in, big)
                static bool loaded;
                if (!loaded) loaded = true, load_repros();
                float bw = 420, bh = 64;
                ImGui::SetCursorScreenPos(ImVec2(a.x + (av.x - bw) / 2, a.y + av.y / 2 - bh - 20));
                if (ImGui::Button((std::string("Start: ") + PROFILES[g_profile].name).c_str(), ImVec2(bw, bh))) game_start();
                ImGui::SetCursorScreenPos(ImVec2(a.x + (av.x - bw) / 2, a.y + av.y / 2 + 20));
                if (!g_repros.empty()) {
                    const Repro &r = g_repros.front();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.15f, 1));
                    std::string l = "Replay the last bug (" + r.name + (r.note.empty() ? "" : ": " + r.note) + ")";
                    if (ImGui::Button(l.c_str(), ImVec2(bw, bh))) replay_repro(r);
                    ImGui::PopStyleColor();
                    ImGui::SetCursorScreenPos(ImVec2(a.x + (av.x - bw) / 2, a.y + av.y / 2 + 20 + bh + 8));
                    ImGui::TextDisabled("(older ones: Tools > Repros)");
                } else {
                    ImGui::TextDisabled("No bug marked yet: Start, and click \"Mark bug here\" when one is on screen.");
                }
            } else if (!g_game_hwnd) {
                ImGui::GetWindowDrawList()->AddText(ImVec2(ox + 10, oy + 10), IM_COL32(220, 200, 90, 255), "the game is starting...");
            }
            int pw, ph, ww, wh;  // (ImGui works in window units, the child window in pixels)
            SDL_GetWindowSizeInPixels(win, &pw, &ph);
            SDL_GetWindowSize(win, &ww, &wh);
            float k = ww > 0 ? (float)pw / ww : 1;
            embed_update((int)(ox * k), (int)(oy * k), (int)(gw2 * k), (int)(gh2 * k));
        }
        ImGui::End();
        ImGui::PopStyleVar();
        // splitters: left | right, game / console
        ImGui::SetNextWindowPos(ImVec2(lw - 3, bar));
        ImGui::SetNextWindowSize(ImVec2(6, H - bar));
        ImGui::Begin("##split", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
        ImGui::InvisibleButton("##drag", ImVec2(6, H - bar));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) split = std::min(0.85f, std::max(0.25f, split + io.MouseDelta.x / W));
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(0, bar + gh - 3));
        ImGui::SetNextWindowSize(ImVec2(lw - 3, 6));
        ImGui::Begin("##split_y", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
        ImGui::InvisibleButton("##dragy", ImVec2(lw - 3, 6));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) split_y = std::min(0.9f, std::max(0.2f, split_y + io.MouseDelta.y / (H - bar)));
        ImGui::End();
        // Console (bottom left)
        ImGui::SetNextWindowPos(ImVec2(0, bar + gh + 3));
        ImGui::SetNextWindowSize(ImVec2(lw - 3, H - bar - gh - 3));
        if (ImGui::Begin("Console", NULL, P)) panel_console();
        ImGui::End();
        // Tools (right)
        ImGui::SetNextWindowPos(ImVec2(lw + 3, bar));
        ImGui::SetNextWindowSize(ImVec2(W - lw - 3, H - bar));
        if (ImGui::Begin("Tools", NULL, P)) {
            if (ImGui::BeginTabBar("##tools")) {
                if (ImGui::BeginTabItem("Performance")) { panel_perf(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Frame")) { panel_frame(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Memory")) { panel_memory(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Repros")) { panel_repros(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 20, 20, 26, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
    }
    embed_release();  // (a game still running lives on as a window of its own)
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
