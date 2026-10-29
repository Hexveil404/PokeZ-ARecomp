// Host OS layer: everything the runtime needs that differs between macOS and Windows.
// Implementations: runtime/platform/macos.c, runtime/platform/windows.c (the Makefile picks one).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#elif defined(__linux__)
#include "platform/linux_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- guest address space
// Claim [lo, hi) for the guest at exactly that address, readable/writable (pages may be backed lazily).
bool plat_reserve_guest(uint64_t lo, uint64_t hi);
// Make sure [addr, addr+size) inside the guest window is backed (before the OS itself writes there, e.g.
// file reads); a no-op where the whole window is backed from the start.
void plat_commit(uint64_t addr, uint64_t size);
// In-place reboot: every page of [lo, hi) back to zero (and unbacked), the window still claimed.
bool plat_reset_guest(uint64_t lo, uint64_t hi);
// Pages of [addr, addr+size) in the guest window the CPU wrote since the last call over them (write tracking: Windows
// MEM_WRITE_WATCH, Linux 6.7+ userfaultfd + PAGEMAP_SCAN), their addresses into pages[] (at most max), and the tracking
// reset for the range. Returns the count (max: maybe more), or -1 where there is no tracking (macOS, older Linux).
// Page size: *page.
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);
// Memory in use (the "[mem]" log line): the process's private commit, and the part of it inside the guest window.
// False where it isn't measured (macOS, Linux).
bool plat_mem_usage(uint64_t *process, uint64_t *guest);
// (profiling, LARECOMP_SAMPLE_EXEC: Windows) the calling thread is the one to sample; a no-op elsewhere
void plat_sample_focus(void);
#ifdef __linux__
void plat_write_tracking_off(const char *why);  // tracking off from now on (unless LARECOMP_WRITE_TRACKING=1)
#endif
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_RW = 3 };
int plat_protect(uint64_t addr, uint64_t size, int prot);  // 0 on success
#define PLAT_PAGE 0x4000ull  // largest page size we deal with (macOS 16K; multiple of Windows' 4K)

// ---- time
uint64_t plat_now_ns(void);                // monotonic
void plat_sleep_until_ns(uint64_t t);      // precise wait for plat_now_ns() >= t
void plat_thread_realtime(uint64_t period_ns);  // periodic timing thread (vsync): don't coalesce its timers
int plat_thread_realtime_check(uint64_t period_ns);  // (the calling thread, after a late wait) 0 the policy holds, 1 it was
                                                     // gone and is set again, -1 can't tell; only macOS looks
int plat_cpu_count(void);  // logical cores this process may run on (its affinity mask); 0 if unknown
void plat_thread_guest(int prio);  // a guest thread's host scheduling class from its Switch priority (lower = more urgent)
uint64_t plat_thread_cpu_ns(void);  // the calling thread's CPU time so far (user + system; diagnostics)
// The calling thread is one of the frame's critical threads: on Windows, no power throttling (EcoQoS) for it and, on a
// hybrid CPU, a preference for the performance cores (selected CPU sets; LARECOMP_PCORE=0: off). Elsewhere a no-op.
enum { PLAT_PERF_GAME, PLAT_PERF_RENDER, PLAT_PERF_VSYNC };
#ifdef _WIN32
void plat_thread_perf_core(int role);
#else
static inline void plat_thread_perf_core(int role) { (void)role; }
#endif

// ---- process
void plat_init(void);                                // first thing in main()
// The machine, for the log's first "[info]" line (runtime/main.c): nothing that identifies the player (no user name,
// no paths). Fields left empty / 0 when unknown.
typedef struct {
    char os[128];          // "macOS 15.4 (arm64)", "SteamOS 3.6.19 (x86_64, kernel 6.5.0-valve22)"
    char cpu[128];         // "Apple M3 Max"
    char cores_detail[48]; // optional, after the core count: "12P + 4E"
    int cores, threads;    // physical cores, hardware threads
    uint64_t ram;          // bytes
    const char *package;   // ".app", "AppImage", "zip"; NULL: a build run from its folder
} PlatSysInfo;
void plat_system_info(PlatSysInfo *si);
bool plat_exe_path(char *buf, size_t size);
void plat_os_name(char *buf, size_t size);  // "Windows 10.0.19045", "Ubuntu 24.04 LTS (6.8.0)", "macOS 15.1" (the log)
void plat_exec(char *const argv[]);  // replace the process (NULL = same command line); returns only on failure
bool plat_relaunch(void);  // (Windows) start this program again, detached; the caller then exits
#if defined(_WIN32) || defined(__linux__)
void plat_terminate(void);  // end the process now, skipping DLL teardown (exit status 0)
#endif

// Child processes (seed generator, game-file extraction). stdout + stderr go to log_path (NULL = /dev/null).
typedef intptr_t PlatProc;  // 0 = failed to start
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log);
int plat_proc_wait(PlatProc p, bool block);  // exit code, or -1 while still running (block = false)
#define PLAT_PROC_SIGNALED 256               // + N: ended by signal N instead of exiting (not on Windows)
void plat_proc_kill(PlatProc p);             // end it and what it started (then it's gone: no wait)
bool plat_remove_tree(const char *dir);      // rm -rf
// A game file opened the way the console's file systems allow: it can be renamed or deleted while open (Windows:
// shared delete access). flags: O_RDONLY / O_RDWR, O_CREAT, O_EXCL; errno set on failure. Rename: fails if the target
// exists (EEXIST), as the console's does.
int plat_open_file(const char *path, int flags);
int plat_rename(const char *from, const char *to);
// dir is synced by OneDrive (Windows: under a OneDrive folder, or a cloud file); false on other systems
bool plat_onedrive_synced(const char *dir);
#ifdef _WIN32
#define PLAT_EXE_SUFFIX ".exe"
#define PLAT_VENV_PYTHON "venv/Scripts/python.exe"  // relative to the venv's parent
#else
#define PLAT_EXE_SUFFIX ""
#define PLAT_VENV_PYTHON "venv/bin/python"
#endif

// ---- diagnostics
// Name of the host function containing pc (guest functions are f_<addr>), NULL if unknown.
const char *plat_symbol(uint64_t pc, uint64_t *offset);
bool plat_module_name(uint64_t pc, char *out, size_t n);  // the file of the module (exe / library / driver) holding pc
void plat_print_backtrace(void);
void plat_fatal_notice(void);  // a fatal error is about to end the process: tell the user (Windows release: message box)
// Something the player should know that doesn't stop the game (a save that failed): the same dialog as
// plat_fatal_notice, as a warning, from its own thread so the game keeps running. Like it, only shown when started as
// the app (a terminal / test run has the log).
void plat_notice(const char *text);
void plat_crash_handler_install(void);  // per thread where the OS needs it (alternate signal stack)
#ifdef _WIN32
void plat_dump_threads(void);  // into the log: where every other thread is (a stuck game)
void plat_run_begin(void);     // the last run ended without a word: what Windows recorded, into the log
#endif
// What the screen shows in a window (Windows: a hash of 32x18 samples of the desktop over its client area, while it
// is the foreground window; false: can't tell)
bool plat_window_screen_hash(void *hwnd, uint32_t *out);
// Called by the crash handler (runtime/dispatch.c): true = fault handled (watchpoint), resume.
bool rt_fault_watch(uint64_t fault_addr, uint64_t host_pc);
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
