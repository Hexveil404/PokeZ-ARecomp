// Windows host layer (runtime/platform.h).
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <timeapi.h>
#include <io.h>
#include <psapi.h>
#include <wchar.h>
#include <direct.h>
#include <ctype.h>

#include "../platform.h"

void rt_crash_explain(char *out, size_t n, const char *log_path);  // runtime/crash_explain.c
void rt_launcher_log(const char *fmt, ...);  // runtime/launcher_log.c

// ---- guest address space
// The window is reserved up front and committed per region (plat_commit from the loader / kernel). Anything
// the guest touches outside a committed region is committed on its first access by the exception handler
// below, so the window behaves like macOS's all-RW reservation. The OS itself (ReadFile into a guest buffer)
// does not fault: such buffers must be inside committed regions.
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    // No VirtualQuery first, unless a watch is armed: it measures the run of like pages from `a` - the window's whole
    // committed part, GBs - and took ~15 ms (Pokémon Z-A: every first touch of a chunk while a battle loads, seconds
    // of them). The window is one reservation, never freed, its pages reserved or committed read-write: a second
    // commit is a no-op (the race below: the access is retried). Only a page protected on purpose (LARECOMP_WATCH)
    // must stay as it is - a commit makes it read-write again.
    extern uint64_t g_watch_lo;
    if (!g_watch_lo) return VirtualAlloc((void *)(uintptr_t)(a & ~(COMMIT_CHUNK - 1)), COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return false;
    // Two threads touching the same uncommitted chunk: the second faulted before the first committed it and now finds
    // it committed - retry the access. (A page protected on purpose, plat_protect / LARECOMP_WATCH, isn't plain
    // read-write and still goes on to the fault watch.) Fix from letsgo-recomp.
    if (mi.State == MEM_COMMIT) return (mi.Protect & 0xFF) == PAGE_READWRITE && !(mi.Protect & PAGE_GUARD);
    if (mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    return VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static bool g_report_crashes;

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    // (not for an execute fault: on a read-write page, retrying it would fault again forever)
    if (er->ExceptionInformation[0] != EXCEPTION_EXECUTE_FAULT && lazy_commit(fa)) return EXCEPTION_CONTINUE_EXECUTION;
    extern int g_watch_write;
    g_watch_write = (int)er->ExceptionInformation[0];
    if (g_report_crashes && rt_fault_watch(fa, ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The window keeps write tracking (MEM_WRITE_WATCH: the Vulkan renderer's vertex / index cache re-copies only pages the
// CPU wrote, runtime/nvn/vulkan/exec.c). Without it (refused): no tracking, the renderer reads guest memory in place.
static bool g_write_watch;

// (the reservation failed: main.c starts the process again) what holds part of the range, for the new process's log
// (main.c, LARECOMP_REEXEC_WHY: this one can't log yet). Windows places heaps, stacks and DLLs' allocations at random
// addresses in the lower terabyte (high-entropy ASLR), so now and then one lands in the guest's 12.5 GB.
static void note_guest_conflict(uint64_t lo, uint64_t hi) {
    char why[200];
    snprintf(why, sizeof why, "VirtualAlloc error %lu, nothing found in the range", GetLastError());
    MEMORY_BASIC_INFORMATION mi;
    for (uint64_t a = lo; a < hi && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); ) {
        uint64_t base = (uint64_t)(uintptr_t)mi.BaseAddress, end = base + mi.RegionSize;
        if (mi.State != MEM_FREE) {
            char mod[MAX_PATH] = "";
            if (mi.Type == MEM_IMAGE && !GetModuleFileNameA((HMODULE)mi.AllocationBase, mod, sizeof mod)) mod[0] = 0;
            const char *name = strrchr(mod, '\\');
            snprintf(why, sizeof why, "%s %s at %llx-%llx%s%s",
                     mi.State == MEM_COMMIT ? "committed" : "reserved",
                     mi.Type == MEM_IMAGE ? "module" : mi.Type == MEM_MAPPED ? "mapped file" : "private memory",
                     (unsigned long long)base, (unsigned long long)end, mod[0] ? " " : "", name ? name + 1 : mod);
            break;
        }
        if (end <= a) break;
        a = end;
    }
    _putenv_s("LARECOMP_REEXEC_WHY", why);
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p == (void *)(uintptr_t)lo) g_write_watch = true;
    else p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)lo) {
        note_guest_conflict(lo, hi);
        return false;
    }
    g_lo = lo;
    g_hi = hi;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi || max <= 0) return -1;
    ULONG_PTR n = (ULONG_PTR)max;
    DWORD gran = 4096;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, (SIZE_T)size, (PVOID *)pages, &n, &gran) != 0) return -1;
    *page = gran;
    if (n >= (ULONG_PTR)max) ResetWriteWatch((void *)(uintptr_t)addr, (SIZE_T)size);  // (more than fit: caller takes all)
    return (int)n;
}

bool plat_mem_usage(uint64_t *process, uint64_t *guest) {
    PROCESS_MEMORY_COUNTERS_EX pc = {.cb = sizeof pc};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pc, sizeof pc)) return false;
    *process = pc.PrivateUsage;
    *guest = 0;  // (0: not counted)
    MEMORY_BASIC_INFORMATION mi;
    // The guest's share only with LARECOMP_MEM_SPLIT=1: counting it walks the window's regions, and VirtualQuery over
    // GBs of them held up the other threads' memory calls - a 63 ms frame each minute, from any thread
    static int split = -1;
    if (split < 0) split = getenv("LARECOMP_MEM_SPLIT") != NULL;
    for (uint64_t a = g_lo; split && a < g_hi && VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); a = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize)
        if (mi.State == MEM_COMMIT) *guest += mi.RegionSize;
    static int map_once;  // LARECOMP_MEM_MAP=1 (debug): the largest private allocations outside the guest window, once
    if (!map_once++ && getenv("LARECOMP_MEM_MAP")) {
        struct { uint64_t base, size; } top[16] = {{0}};
        uint64_t cur = 0, sz = 0;
        for (uint64_t a = 0x10000; VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi); a = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
            uint64_t ab = (uint64_t)(uintptr_t)mi.AllocationBase;
            if (ab != cur) {  // (an allocation's regions are consecutive: its total when the next one starts)
                for (int i = 0; i < 16; i++)
                    if (sz > top[i].size) { memmove(&top[i + 1], &top[i], (15 - i) * sizeof top[0]); top[i].base = cur, top[i].size = sz; break; }
                cur = ab, sz = 0;
            }
            if (mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE && (ab < g_lo || ab >= g_hi)) sz += mi.RegionSize;
            if ((uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize < a) break;
        }
        for (int i = 0; i < 16 && top[i].size; i++)
            fprintf(stderr, "[mem] private allocation %llx: %llu MB\n", (unsigned long long)top[i].base, (unsigned long long)(top[i].size >> 20));
    }
    return true;
}
int plat_written_pages_peek(uint64_t addr, uint64_t size) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi) return -1;
    static PVOID pages[4096];
    ULONG_PTR n = 4096;
    DWORD gran = 4096;
    if (GetWriteWatch(0, (void *)(uintptr_t)addr, (SIZE_T)size, pages, &n, &gran) != 0) return -1;
    return (int)n;
}

// Decommit everything: the range stays reserved, and pages come back zeroed when committed again (lazily).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return VirtualFree((void *)(uintptr_t)lo, hi - lo, MEM_DECOMMIT) != 0;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        fprintf(stderr, "[plat] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr,
                (unsigned long long)size, GetLastError());
}

// (gmem_fault_explain, #39 #72) committed? resident? A committed page nothing ever touched has no physical page yet
bool plat_page_state(uint64_t addr, char *out, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    PSAPI_WORKING_SET_EX_INFORMATION ws = {.VirtualAddress = (void *)(uintptr_t)(addr & ~0xFFFull)};
    bool q = VirtualQuery(ws.VirtualAddress, &mi, sizeof mi) != 0;
    bool w = K32QueryWorkingSetEx(GetCurrentProcess(), &ws, sizeof ws) != 0;
    snprintf(out, n, "%s, protection %#lx; %s%s", !q ? "?" : mi.State == MEM_COMMIT ? "committed" : mi.State == MEM_RESERVE ? "reserved only" : "free",
             q ? mi.Protect : 0, !w ? "working set unknown" : ws.VirtualAttributes.Valid ? "resident" : "NOT resident (never touched, or paged out)",
             w && ws.VirtualAttributes.Valid && ws.VirtualAttributes.Locked ? ", locked" : "");
    return true;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time
static LARGE_INTEGER qpf;

uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
}

// The calling thread's CPU time. GetThreadTimes only advances in whole scheduler ticks (15.6 ms): per frame that reads
// 0 or 15.6. QueryThreadCycleTime counts the thread's cycles exactly, in TSC ticks on x64 (invariant TSC), so
// those are converted with the TSC rate measured once against the performance counter (20 ms, at the first call).
// Elsewhere (or if the cycle count fails): GetThreadTimes, kernel + user.
#if defined(__x86_64__) || defined(_M_X64)
#include <intrin.h>
static double tsc_per_ns;
static INIT_ONCE tsc_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK tsc_calibrate(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o, (void)p, (void)c;
    uint64_t q0 = plat_now_ns(), t0 = __rdtsc();
    Sleep(20);
    uint64_t q1 = plat_now_ns(), t1 = __rdtsc();
    if (q1 > q0 && t1 > t0) tsc_per_ns = (double)(t1 - t0) / (double)(q1 - q0);
    return TRUE;
}
#endif
uint64_t plat_thread_cpu_ns(void) {
#if defined(__x86_64__) || defined(_M_X64)
    InitOnceExecuteOnce(&tsc_once, tsc_calibrate, NULL, NULL);
    ULONG64 cyc;
    if (tsc_per_ns > 0 && QueryThreadCycleTime(GetCurrentThread(), &cyc)) return (uint64_t)((double)cyc / tsc_per_ns);
#endif
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    uint64_t kt = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime, ut = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kt + ut) * 100;  // (100 ns units)
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

void plat_sleep_until_ns(uint64_t t) {
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }  // last 0.2 ms: spin (timer slack)
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};  // relative, 100 ns units
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

int plat_cpu_count(void) {
    DWORD_PTR proc = 0, sys = 0;
    return GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) ? __builtin_popcountll((unsigned long long)proc) : 0;
}

void plat_thread_guest(int prio) { (void)prio; }  // (the OS's default scheduling serves the game here)

// ---- performance cores (hybrid CPUs: P-cores + E-cores)
// Windows may run the game, render and vsync threads on efficiency cores, where they run far slower (another Switch
// recomp measured 40%). Each of those threads opts out of power throttling (EcoQoS) and, on a hybrid CPU, selects the
// CPU sets of the faster cores (EfficiencyClass above the lowest). Selected CPU sets are a soft preference, never a
// hard pin, and they are only the cores the process affinity allows: when the affinity already excludes the E-cores
// (or the P-cores), nothing is selected, so an affinity-limited run (e.g. 0xF) schedules exactly as before.
// Not hybrid: no CPU sets. LARECOMP_PCORE=0: none of this (the process-wide opt-out in plat_init stays).
static INIT_ONCE pcore_once = INIT_ONCE_STATIC_INIT;
static bool pcore_off;
static ULONG *pcore_ids;  // the selected CPU set ids (NULL: no selection)
static ULONG pcore_n;
static const char *pcore_how = "";

static BOOL CALLBACK pcore_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o, (void)p, (void)c;
    const char *e = getenv("LARECOMP_PCORE");
    if (e && *e && atoi(e) == 0) {
        pcore_off = true;
        fprintf(stderr, "[pcore] off (LARECOMP_PCORE=0): default scheduling, thread power throttling unchanged\n");
        return TRUE;
    }
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    uint8_t *buf = len ? malloc(len) : NULL;
    if (!buf || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        free(buf);
        pcore_how = " (CPU sets unavailable)";
        fprintf(stderr, "[pcore] CPU set information unavailable (error %lu): power throttling off only\n", GetLastError());
        return TRUE;
    }
    // The process affinity mask covers its primary group only: filter by it when the system has a single group.
    DWORD_PTR proc = 0, sys = 0;
    bool filter = GetActiveProcessorGroupCount() == 1 && GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc;
    int lo = 255, hi = -1, all_p = 0, all_e = 0, ok_p = 0, ok_e = 0, n = 0;
    for (ULONG off = 0; off < len;) {  // pass 1: the efficiency classes
        SYSTEM_CPU_SET_INFORMATION *i = (SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (!i->Size) break;
        if (i->Type == CpuSetInformation) {
            int ec = i->CpuSet.EfficiencyClass;
            if (ec < lo) lo = ec;
            if (ec > hi) hi = ec;
            n++;
        }
        off += i->Size;
    }
    bool hybrid = n && hi > lo;
    if (hybrid) pcore_ids = calloc((size_t)n, sizeof *pcore_ids);
    for (ULONG off = 0; off < len;) {  // pass 2: count, and the P-cores the affinity allows
        SYSTEM_CPU_SET_INFORMATION *i = (SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (!i->Size) break;
        if (i->Type == CpuSetInformation) {
            bool perf = i->CpuSet.EfficiencyClass > lo;
            bool allowed = !filter || (i->CpuSet.Group == 0 && i->CpuSet.LogicalProcessorIndex < 64 &&
                                       (proc >> i->CpuSet.LogicalProcessorIndex) & 1);
            perf ? all_p++ : all_e++;
            if (allowed) perf ? ok_p++ : ok_e++;
            if (allowed && perf && pcore_ids) pcore_ids[pcore_n++] = i->CpuSet.Id;
        }
        off += i->Size;
    }
    free(buf);
    if (!hybrid) {
        pcore_how = " (not hybrid: no CPU sets)";
        fprintf(stderr, "[pcore] CPU not hybrid (%d logical processors, one efficiency class): power throttling off, "
                        "no CPU sets\n", n);
    } else if (!ok_p || !ok_e) {
        free(pcore_ids), pcore_ids = NULL, pcore_n = 0;
        pcore_how = " (affinity decides: no CPU sets)";
        fprintf(stderr, "[pcore] hybrid CPU: %d performance + %d efficiency logical processors; the process affinity "
                        "allows %d + %d: no CPU sets (the affinity decides), power throttling off\n", all_p, all_e, ok_p, ok_e);
    } else {
        pcore_how = " (performance-core CPU sets)";
        fprintf(stderr, "[pcore] hybrid CPU: %d performance + %d efficiency logical processors%s; game/render/vsync "
                        "threads prefer the %d performance ones, power throttling off\n", all_p, all_e,
                filter && (ok_p != all_p || ok_e != all_e) ? " (affinity-limited)" : "", ok_p);
    }
    return TRUE;
}

void plat_thread_perf_core(int role) {
    // Above the game's own threads (all normal priority, dozens - its job workers busy half the time): the frame's
    // threads - the game thread, the render thread and the recorder - lose the CPU to them less often; a preempted
    // render thread missed a refresh, the frame shown twice (Pokémon Z-A on 8 cores: a present ~28 ms late every
    // ~2 s). LARECOMP_THREAD_PRIO=0: normal priority, as before.
    static int prio = -1;
    if (prio < 0) prio = !getenv("LARECOMP_THREAD_PRIO") || atoi(getenv("LARECOMP_THREAD_PRIO")) != 0;
    if (prio) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    InitOnceExecuteOnce(&pcore_once, pcore_init, NULL, NULL);
    if (pcore_off) return;
    THREAD_POWER_THROTTLING_STATE tp = {
        .Version = THREAD_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED,
        .StateMask = 0,  // (controlled, and off: never EcoQoS)
    };
    bool thr = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tp, sizeof tp) != 0;
    bool sets = !pcore_ids || SetThreadSelectedCpuSets(GetCurrentThread(), pcore_ids, pcore_n) != 0;
    static volatile LONG logged;  // (once per role: a reboot's new game thread applies the same, quietly)
    static const char *const names[] = {"game", "render", "vsync"};
    if (role < 0 || role > 2 || (InterlockedOr(&logged, 1 << role) & (1 << role))) return;
    fprintf(stderr, "[pcore] %s thread: power throttling %s%s%s\n", names[role], thr ? "off" : "unchanged (refused)",
            pcore_how, sets ? "" : " (CPU sets refused)");
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}
int plat_thread_realtime_check(uint64_t period_ns) { (void)period_ns; return -1; }

// ---- process
// GUI launches keep their own append-only log; lasrecomp.log names the latest run, with five recent backups.
// A launcher and the game it starts share one session, so another launch cannot truncate either writer.
static char g_log_path[MAX_PATH], g_log_latest[MAX_PATH];
static char g_log_tag[MAX_PATH + 16];
static HANDLE g_log_mutex;

static bool lock_logs(void) {
    DWORD r = g_log_mutex ? WaitForSingleObject(g_log_mutex, 5000) : WAIT_FAILED;
    return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
}

// A shared lock outside the log's data marks it as live, including across the launcher's child processes.
// Pruning takes the exclusive form, so an older session still running keeps its path until the next cleanup.
static bool log_live_lock(HANDLE file, bool exclusive) {
    OVERLAPPED at = {.Offset = 0xffffffffu, .OffsetHigh = 0x7fffffffu};
    return LockFileEx(file, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1, 0, &at);
}

typedef struct { char name[MAX_PATH]; FILETIME created; } SessionLog;
static int newest_log(const void *a, const void *b) {
    const SessionLog *x = a, *y = b;
    int time = CompareFileTime(&y->created, &x->created);
    return time ? time : strcmp(y->name, x->name);
}
static bool session_log_name(const char *name) {  // lasrecomp-backup-<filetime hex>-<pid>.log
    if (strncmp(name, "lasrecomp-backup-", 17)) return false;
    const char *p = name + 17;
    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-') p++;
    return p > name + 17 && !strcmp(p, ".log");
}
static void keep_legacy_log(const char *dir) {
    WIN32_FILE_ATTRIBUTE_DATA old;
    if (!GetFileAttributesExA(g_log_latest, GetFileExInfoStandard, &old) ||
        old.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return;
    // The fallback copy has its own creation time, but still names the original session in its first line.
    char old_tag[MAX_PATH + 16] = {0};
    HANDLE old_file = CreateFileA(g_log_latest, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (old_file != INVALID_HANDLE_VALUE) {
        DWORD got;
        ReadFile(old_file, old_tag, sizeof old_tag - 1, &got, NULL); CloseHandle(old_file);
    }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%slasrecomp-backup-*.log", dir);
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(path, &data);
    bool kept = false;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            char tag[MAX_PATH + 16]; snprintf(tag, sizeof tag, "[session] %s\n", data.cFileName);
            kept |= !strncmp(old_tag, tag, strlen(tag)) || !CompareFileTime(&data.ftCreationTime, &old.ftCreationTime);
        }
        while (!kept && FindNextFileA(find, &data));
        FindClose(find);
    }
    // The first launch after upgrading may only have the old lasrecomp.log; retain that report too.
    if (!kept) {
        unsigned long long stamp = ((unsigned long long)old.ftCreationTime.dwHighDateTime << 32) | old.ftCreationTime.dwLowDateTime;
        if (snprintf(path, sizeof path, "%slasrecomp-backup-%016llx-0.log", dir, stamp) < (int)sizeof path)
            CopyFileA(g_log_latest, path, TRUE);
    }
}
static void prune_logs(const char *dir) {
    char path[MAX_PATH];
    if (snprintf(path, sizeof path, "%slasrecomp-backup-*.log", dir) >= (int)sizeof path) return;
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(path, &data);
    if (find == INVALID_HANDLE_VALUE) return;
    SessionLog *logs = NULL;
    size_t n = 0;
    do {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || !session_log_name(data.cFileName)) continue;
        SessionLog *next = realloc(logs, (n + 1) * sizeof *logs);
        if (!next) { free(logs); FindClose(find); return; }
        logs = next;
        snprintf(logs[n].name, sizeof logs[n].name, "%s", data.cFileName);
        logs[n++].created = data.ftCreationTime;
    } while (FindNextFileA(find, &data));
    FindClose(find);
    if (n > 1) qsort(logs, n, sizeof *logs, newest_log);
    for (size_t i = 5; i < n; i++) {
        if (snprintf(path, sizeof path, "%s%s", dir, logs[i].name) >= (int)sizeof path) continue;
        HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        if (log_live_lock(f, true)) DeleteFileA(path);
        CloseHandle(f);
    }
    free(logs);
}

// NTFS can give the newest session both names, keeping lasrecomp.log current even after a crash. On filesystems
// without hard links, copy on exit instead. The unique session header identifies who owns the latest report;
// creation times can collide for simultaneous launches. The startup mutex also covers this replacement.
static void publish_log(void) {
    if (!g_log_path[0] || !lock_logs()) return;
    fflush(stdout); fflush(stderr);
    HANDLE latest = CreateFileA(g_log_latest, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BY_HANDLE_FILE_INFORMATION current, session;
    HANDLE source = (HANDLE)_get_osfhandle(_fileno(stderr));
    char tag[sizeof g_log_tag]; DWORD got = 0;
    bool copy = latest != INVALID_HANDLE_VALUE && ReadFile(latest, tag, strlen(g_log_tag), &got, NULL) &&
                got == strlen(g_log_tag) && !memcmp(tag, g_log_tag, got) && GetFileInformationByHandle(latest, &current) &&
                GetFileInformationByHandle(source, &session) &&
                (current.dwVolumeSerialNumber != session.dwVolumeSerialNumber || current.nFileIndexHigh != session.nFileIndexHigh ||
                 current.nFileIndexLow != session.nFileIndexLow);
    if (latest != INVALID_HANDLE_VALUE) CloseHandle(latest);
    if (copy) CopyFileA(g_log_path, g_log_latest, FALSE);
    ReleaseMutex(g_log_mutex);
}

static void log_to_file_without_console(void) {
    // An inherited pipe/file (tools, terminal) is already somewhere useful. Only the GUI launch needs redirecting.
    // A relaunched game must still join its GUI session and publish its fallback copy on exit.
    const char *inherited = getenv("LASRECOMP_LOG_FILE");
    bool fresh = !inherited || !*inherited;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (fresh && err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) return;
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (!n || n >= sizeof dir) return;
    char *slash = strrchr(dir, '\\');
    if (!slash) return;
    slash[1] = 0;
    if (snprintf(g_log_latest, sizeof g_log_latest, "%slasrecomp.log", dir) >= (int)sizeof g_log_latest) return;
    unsigned long long hash = 14695981039346656037ull;
    for (const unsigned char *p = (const unsigned char *)g_log_latest; *p; p++) hash = (hash ^ *p) * 1099511628211ull;
    char mutex[64]; snprintf(mutex, sizeof mutex, "Local\\LASRecomp.logs.%016llx", hash);
    g_log_mutex = CreateMutexA(NULL, FALSE, mutex);
    if (!lock_logs()) return;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    unsigned long long stamp = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    int used = fresh ? snprintf(g_log_path, sizeof g_log_path, "%slasrecomp-backup-%016llx-%lu.log", dir, stamp,
                               (unsigned long)GetCurrentProcessId())
                     : snprintf(g_log_path, sizeof g_log_path, "%s", inherited);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (used >= 0 && (size_t)used < sizeof g_log_path)
        file = CreateFileA(g_log_path, GENERIC_READ | FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           fresh ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !log_live_lock(file, false)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        g_log_path[0] = 0; ReleaseMutex(g_log_mutex); return;
    }
    const char *name = strrchr(g_log_path, '\\');
    snprintf(g_log_tag, sizeof g_log_tag, "[session] %s\n", name ? name + 1 : g_log_path);
    // A GUI-subsystem process with no inherited handles starts with invalid CRT stream descriptors.
    // Give both streams real descriptors before duplicating the shared-delete log handle into them.
    if (!freopen("NUL", "ab", stderr) || !freopen("NUL", "ab", stdout)) {
        CloseHandle(file); g_log_path[0] = 0; ReleaseMutex(g_log_mutex); return;
    }
    int fd = _open_osfhandle((intptr_t)file, _O_WRONLY | _O_APPEND | _O_BINARY);
    if (fd < 0 || _dup2(fd, _fileno(stderr)) != 0) {
        if (fd < 0) CloseHandle(file); else _close(fd);
        g_log_path[0] = 0; ReleaseMutex(g_log_mutex); return;
    }
    _close(fd);
    setvbuf(stderr, NULL, _IONBF, 0);
    _dup2(_fileno(stderr), _fileno(stdout));
    _putenv_s("LASRECOMP_LOG_FILE", g_log_path);
    if (fresh) {
        fputs(g_log_tag, stderr);
        keep_legacy_log(dir);
        char next[MAX_PATH];
        // A shorter name than the session path, which was already checked above.
        snprintf(next, sizeof next, "%slasrecomp-current-%016llx-%lu.tmp", dir, stamp, (unsigned long)GetCurrentProcessId());
        bool staged = CreateHardLinkA(next, g_log_path, NULL) != 0;
        if (!staged) {
            // Keep the session header in the placeholder too; the full copy at exit preserves it.
            HANDLE marker = CreateFileA(next, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (marker != INVALID_HANDLE_VALUE) {
                DWORD written;
                staged = WriteFile(marker, g_log_tag, strlen(g_log_tag), &written, NULL) && written == strlen(g_log_tag);
                CloseHandle(marker);
                if (!staged) DeleteFileA(next);
            }
        }
        if (staged) {
            MoveFileExA(next, g_log_latest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            DeleteFileA(next);
        }
        prune_logs(dir);
    }
    atexit(publish_log);
    ReleaseMutex(g_log_mutex);
}

// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2), or on the player's machine for
// x86-64-v2 when the CPU is older (gamelib_old_cpu): say so instead of crashing with an illegal instruction on a CPU
// without even that.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt")) return;
    MessageBoxA(NULL, "ZA Recomp needs a CPU with SSE4.2 (Intel Core i3/i5/i7 / 2008 or newer, AMD Bulldozer / 2011 or "
                "newer).", "ZA Recomp", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

// Fatal error (rt_fatal_trace): what happened in plain words (runtime/crash_explain.c) in a message box, when there is
// no console; the technical lines stay in the log.
static void run_end(void);
void plat_fatal_notice(void) {
    run_end();
    publish_log();
    char text[1600];
    rt_crash_explain(text, sizeof text, g_log_path);
    if (!g_log_path[0]) return;
    fflush(stderr);
    MessageBoxA(NULL, text, "ZA Recomp", MB_ICONERROR | MB_OK);
}

// (profiling) LARECOMP_SAMPLE=<delay s>,<duration s>: after the delay, a thread samples the instruction pointer of
// every other thread of the process each ~1 ms (suspend, read the context, resume) for the duration, then writes
// build/samples.txt: "<thread id> <rip> <count>" lines (map the addresses with tools/za/samples.py and nm).
// LARECOMP_SAMPLE_CALLER=<lo hex>,<hi hex>: samples inside that range (a leaf function such as memcmp) also count the
// return address on top of the stack, under thread id | 0x80000000 - who calls it.
#include <tlhelp32.h>
#define SAMP_TAB (1u << 20)
static struct { uint64_t rip; uint32_t tid, n; } *samp;
static void samp_add(uint64_t rip, uint32_t tid) {
    uint64_t k = (rip * 0x9E3779B97F4A7C15ull) ^ tid;
    uint32_t slot = (uint32_t)(k >> 40) & (SAMP_TAB - 1);
    for (int probe = 0; probe < 64; probe++, slot = (slot + 1) & (SAMP_TAB - 1)) {
        if (!samp[slot].n) { samp[slot].rip = rip; samp[slot].tid = tid; samp[slot].n = 1; return; }
        if (samp[slot].rip == rip && samp[slot].tid == tid) { samp[slot].n++; return; }
    }
}
// LARECOMP_SAMPLE_EXEC=1 (with LARECOMP_SAMPLE): only the thread running the game's command lists (plat_sample_focus,
// from runtime/nvn/vulkan/exec.c), ~1 kHz, each sample its whole call stack (RtlVirtualUnwind) -> build/stacks.txt:
// "M <base> <size> <module path>" lines, then one "S <rip> <caller> <caller's caller> ..." line per sample
// (tools/za/stacks.py: self and inclusive time per function). LARECOMP_SAMPLE_EXEC=all: every thread instead, a stack
// only when it runs (not waiting inside ntdll / KernelBase / win32u) - who does the work while the game is held up.
static volatile DWORD g_focus_tid;
void plat_sample_focus(void) { if (!g_focus_tid) g_focus_tid = GetCurrentThreadId(); }
#define STK_DEPTH 24
static DWORD WINAPI stack_sampler(double dur) {
    bool all = !strcmp(getenv("LARECOMP_SAMPLE_EXEC"), "all");
    HANDLE hs[256];
    int nh = 0;
    if (all) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {.dwSize = sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok && nh < 256; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
                if ((hs[nh] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID))) nh++;
        CloseHandle(snap);
    } else {
        while (!g_focus_tid) Sleep(10);
        if ((hs[0] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_focus_tid))) nh = 1;
    }
    if (!nh) return 0;
    uint64_t wait_lo[3] = {0}, wait_hi[3] = {0};  // (all: the modules a waiting thread sits in)
    static const char *const wait_mods[3] = {"ntdll.dll", "KernelBase.dll", "win32u.dll"};
    for (int i = 0; i < 3; i++) {
        MODULEINFO mi;
        HMODULE m = GetModuleHandleA(wait_mods[i]);
        if (m && K32GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi))
            wait_lo[i] = (uint64_t)(uintptr_t)mi.lpBaseOfDll, wait_hi[i] = wait_lo[i] + mi.SizeOfImage;
    }
    size_t cap = (size_t)(dur * 1100) * (all ? 8 : 1) + 64, n = 0;
    uint64_t (*stk)[STK_DEPTH] = calloc(cap, sizeof *stk);
    ULONGLONG end = GetTickCount64() + (ULONGLONG)(dur * 1000);
    while (GetTickCount64() < end && n < cap) {
        for (int t = 0; t < nh && n < cap; t++) {
            HANDLE h = hs[t];
            CONTEXT c = {.ContextFlags = CONTEXT_FULL};
            if (SuspendThread(h) == (DWORD)-1) continue;
            bool waiting = false;
            if (GetThreadContext(h, &c)) {  // (unwound while it is suspended: its stack can't change)
                for (int i = 0; all && i < 3; i++) waiting |= c.Rip >= wait_lo[i] && c.Rip < wait_hi[i];
                for (int d = 0; !waiting && d < STK_DEPTH && c.Rip; d++) {
                    stk[n][d] = c.Rip;
                    DWORD64 base;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                    if (!fe) { c.Rip = *(const DWORD64 *)(uintptr_t)c.Rsp; c.Rsp += 8; continue; }  // (a leaf: no frame)
                    void *hd;
                    DWORD64 ef;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &ef, NULL);
                }
                if (!waiting) n++;
            }
            ResumeThread(h);
        }
        Sleep(1);
    }
    for (int t = 0; t < nh; t++) CloseHandle(hs[t]);
    FILE *f = fopen("build/stacks.txt", "w");
    if (f) {
        HMODULE mods[512];
        DWORD need = 0;
        if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
            for (DWORD i = 0; i < need / sizeof *mods && i < 512; i++) {
                MODULEINFO mi;
                char path[MAX_PATH];
                if (K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi) && GetModuleFileNameA(mods[i], path, sizeof path))
                    fprintf(f, "M %llx %lx %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll, mi.SizeOfImage, path);
            }
        for (size_t i = 0; i < n; i++) {
            fputc('S', f);
            for (int d = 0; d < STK_DEPTH && stk[i][d]; d++) fprintf(f, " %llx", (unsigned long long)stk[i][d]);
            fputc('\n', f);
        }
        fclose(f);
    }
    fprintf(stderr, "[sample] %llu stacks of thread %lu -> build/stacks.txt\n", (unsigned long long)n, g_focus_tid);
    return 0;
}

static DWORD WINAPI sampler_main(void *arg) {
    double delay = 0, dur = 10;
    sscanf((const char *)arg, "%lf,%lf", &delay, &dur);
    Sleep((DWORD)(delay * 1000));
    if (getenv("LARECOMP_SAMPLE_EXEC")) return stack_sampler(dur);
    samp = calloc(SAMP_TAB, sizeof *samp);
    unsigned long long caller_lo = 0, caller_hi = 0;
    if (getenv("LARECOMP_SAMPLE_CALLER")) sscanf(getenv("LARECOMP_SAMPLE_CALLER"), "%llx,%llx", &caller_lo, &caller_hi);
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    ULONGLONG end = GetTickCount64() + (ULONGLONG)(dur * 1000);
    uint64_t total = 0;
    while (GetTickCount64() < end) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {.dwSize = sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            CONTEXT ctx = {.ContextFlags = CONTEXT_CONTROL};
            if (SuspendThread(h) != (DWORD)-1) {
                if (GetThreadContext(h, &ctx)) {
                    samp_add(ctx.Rip, te.th32ThreadID);
                    if (ctx.Rip >= caller_lo && ctx.Rip < caller_hi)  // (the thread is suspended: its stack can be read)
                        samp_add(*(const uint64_t *)(uintptr_t)ctx.Rsp, te.th32ThreadID | 0x80000000u);
                    total++;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        Sleep(1);
    }
    FILE *f = fopen("build/samples.txt", "w");
    if (f) {
        for (uint32_t i = 0; i < SAMP_TAB; i++)
            if (samp[i].n) fprintf(f, "%u %llx %u\n", samp[i].tid, (unsigned long long)samp[i].rip, samp[i].n);
        fclose(f);
    }
    HMODULE game = GetModuleHandleA("lasgame.dll"), exe = GetModuleHandleA(NULL);
    fprintf(stderr, "[sample] %llu samples -> build/samples.txt (lasgame.dll at %p, exe at %p)\n", (unsigned long long)total,
            (void *)game, (void *)exe);
    return 0;
}

static DWORD WINAPI notice_thread(LPVOID text) {
    MessageBoxA(NULL, text, "LAS Recomp", MB_ICONWARNING | MB_OK);
    free(text);
    return 0;
}
void plat_notice(const char *text) {
    if (!g_log_path[0]) return;
    char *copy = _strdup(text);
    HANDLE t = copy ? CreateThread(NULL, 0, notice_thread, copy, 0, NULL) : NULL;
    if (t) CloseHandle(t);
    else free(copy);
}

bool plat_module_name(uint64_t pc, char *out, size_t n) {
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return false;
    return GetModuleFileNameA(m, out, (DWORD)n) > 0;
}

// RtlGetVersion (ntdll): the real version - GetVersionEx answers with the manifest's idea of it
typedef LONG(WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
void plat_system_info(PlatSysInfo *si) {
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RtlGetVersionFn rtl = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (!rtl || rtl(&v) != 0) v.dwMajorVersion = 0;
    SYSTEM_INFO sys;
    GetNativeSystemInfo(&sys);
    const char *arch = sys.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                     : sys.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "?";
    const char *name = v.dwMajorVersion != 10 ? "Windows" : v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    if (v.dwMajorVersion)  // (Windows 11 still says 10.0: build 22000 and later)
        snprintf(si->os, sizeof si->os, "%s (%lu.%lu.%lu, %s)", name, v.dwMajorVersion, v.dwMinorVersion,
                 v.dwBuildNumber, arch);
    else snprintf(si->os, sizeof si->os, "Windows (%s)", arch);
    // the CPU's name: the registry's (what Task Manager shows), else the CPUID brand string
    DWORD len = sizeof si->cpu;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, NULL, si->cpu, &len) != ERROR_SUCCESS) {
        si->cpu[0] = 0;
#if defined(__x86_64__) || defined(__i386__)
        unsigned r[12];
        for (unsigned i = 0; i < 3; i++)
            __asm__ volatile("cpuid" : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3]) : "a"(0x80000002u + i), "c"(0));
        memcpy(si->cpu, r, sizeof r);
        si->cpu[sizeof r] = 0;
#endif
    }
    char *c = si->cpu;  // (both pad the name with spaces)
    while (*c == ' ') c++;
    memmove(si->cpu, c, strlen(c) + 1);
    for (size_t l = strlen(si->cpu); l && si->cpu[l - 1] == ' '; l--) si->cpu[l - 1] = 0;
    // cores: one RelationProcessorCore record each; threads: every processor group's
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &bytes);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info = bytes ? malloc(bytes) : NULL;
    if (info && GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes))
        for (char *p = (char *)info; p < (char *)info + bytes; p += ((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)p)->Size)
            si->cores++;
    free(info);
    si->threads = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    if (GlobalMemoryStatusEx(&m)) si->ram = m.ullTotalPhys;
#ifdef LARECOMP_ZIP_BUILD
    si->package = "zip";  // (make dist: the release zip)
#endif
}

void plat_init(void) {
    // abort() (rt_fatal_trace, after the crash dialog) without the C runtime's own "Microsoft Visual C++ Runtime
    // Library - abort() has been called" box and the Windows error report: the player sees only LAS Recomp's dialog
#ifdef _WRITE_ABORT_MSG  // (MinGW's stdlib.h declares it with these flags)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    log_to_file_without_console();
    if (getenv("LARECOMP_SAMPLE")) CreateThread(NULL, 0, sampler_main, getenv("LARECOMP_SAMPLE"), 0, NULL);
    require_cpu();
    _fmode = _O_BINARY;  // fopen/open default to binary (the runtime's "r"/"w" files are binary or LF text)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);  // 1 ms Sleep / condition-variable timeouts (default 15.6 ms)
    // Opt out of EcoQoS and of Windows 11 ignoring the timer resolution while the window is occluded.
    PROCESS_POWER_THROTTLING_STATE pt = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
        .StateMask = 0,
    };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

bool plat_exe_path(char *buf, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    return n > 0 && n < size;
}

// RtlGetVersion: the real version (GetVersionEx answers what the manifest declares). Build 22000+ is Windows 11.
void plat_os_name(char *buf, size_t size) {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn f = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    if (!f || f(&v) != 0) { snprintf(buf, size, "Windows"); return; }
    snprintf(buf, size, "Windows %s %lu.%lu.%lu", v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "11" : "10",
             v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
}

// Quote one argument the way CommandLineToArgvW / the CRT parse it back.
static void append_arg(char *out, size_t cap, const char *a) {
    size_t n = strlen(out);
    bool q = !*a || strpbrk(a, " \t\"") != NULL;
    if (n && n + 1 < cap) out[n++] = ' ';
    if (q && n + 1 < cap) out[n++] = '"';
    for (const char *p = a; *p; p++) {
        int bs = 0;
        while (*p == '\\') { bs++; p++; }
        int reps = !*p ? (q ? 2 * bs : bs) : *p == '"' ? 2 * bs + 1 : bs;
        for (int i = 0; i < reps && n + 1 < cap; i++) out[n++] = '\\';
        if (!*p) break;
        if (n + 1 < cap) out[n++] = *p;
    }
    if (q && n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

// No exec() on Windows: start the new process with the same console / standard handles, wait for it, and exit with
// its exit code. (Exiting at once ended the process a launcher, Steam or a test script started about a second in,
// with code 0, while the game ran on in another process.)
// A new run of this program (same exe path - the file there now - and command line), not waited for and not tied to
// this process: the start screen's updater starts it and then ends (plat_exec would keep this window up meanwhile).
bool plat_relaunch(void) {
    static wchar_t exe[32768];
    if (!GetModuleFileNameW(NULL, exe, 32768)) return false;
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    fflush(stdout);
    fflush(stderr);
    if (!CreateProcessW(exe, GetCommandLineW(), NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi) &&
        !CreateProcessW(exe, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void plat_exec(char *const argv[]) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    BOOL ok;
    fflush(stdout);
    fflush(stderr);
    if (!argv) {
        ok = CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    } else {
        static char cmd[32768];
        cmd[0] = 0;
        for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
        static wchar_t wcmd[32768];
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    if (!ok) return;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = 1;
    CloseHandle(pi.hProcess);
    run_end();  // (an end the log tells; the new process marks its own run)
    ExitProcess(code);
}

// ---- diagnostics
// DLL frames are named module+offset; frames in our own executable are left to the caller, which maps
// them to guest functions (runtime/dispatch.c) - mingw builds carry DWARF, not PDB symbols.
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[MAX_PATH];
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, sizeof path)) return NULL;
    const char *b = strrchr(path, '\\');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)m;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    USHORT n = RtlCaptureStackBackTrace(0, 64, bt, NULL);
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = plat_symbol(pc, &off);
        if (!s) s = rt_host_fn_name(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx lasrecomp+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(pc - exe));
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint64_t fa = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    char what[48];
    snprintf(what, sizeof what, "exception %08lx", er->ExceptionCode);
    // x64 has no link register: report the return address on top of the stack (valid at function entry).
    uint64_t lr = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)ep->ContextRecord->Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT)
        lr = *(uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
    // The driver's compiler crashed on a build worker: the game goes on without that pipeline, this thread stays here (#93)
    extern bool vk_compile_crash_recover(uint64_t pc, const char *what, uint64_t fa) __attribute__((weak));
    if (vk_compile_crash_recover && vk_compile_crash_recover(ep->ContextRecord->Rip, what, fa)) for (;;) Sleep(INFINITE);
    run_end();  // (the log has the report: not an end without a word)
    rt_fault_report(what, fa, ep->ContextRecord->Rip, lr);
}

void plat_crash_handler_install(void) {
    ULONG guarantee = 64 << 10;  // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    if (g_report_crashes) return;
    g_report_crashes = true;
    SetUnhandledExceptionFilter(unhandled_filter);
}

// ---- a stuck game, a silent end (#38: play.log just stopped - frozen and closed from the Task Manager, or ended by
// Windows without our crash report: heap corruption, a fail-fast)
// plat_dump_threads (runtime/nvn/vulkan/device.c hang_watch: no frame for a while): every other thread's place, into
// the log - its instruction and the code addresses on its stack (a scan, not an unwind: the stack is read with
// ReadProcessMemory, which can't fault, and nothing that could take a lock runs while a thread is suspended).
// Frames in the game code are named by their guest function (rt_host_fn_name).
void plat_dump_threads(void) {
    HMODULE exe = GetModuleHandleA(NULL), game = GetModuleHandleA("lasgame.dll");
    typedef HRESULT(WINAPI * GetDescFn)(HANDLE, PWSTR *);
    GetDescFn get_desc = (GetDescFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {.dwSize = sizeof te};
    static uint64_t stack[4096];  // (32 KB of each stack: the frames nearest the top)
    int threads = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!h) continue;
        CONTEXT ctx = {.ContextFlags = CONTEXT_CONTROL};
        size_t words = 0;
        bool got = false;
        if (SuspendThread(h) != (DWORD)-1) {
            if ((got = GetThreadContext(h, &ctx)))  // (a page at a time, up to the first that can't be read: the top)
                for (uint64_t a = ctx.Rsp & ~7ull; words < 4096;) {
                    size_t chunk = (size_t)((4096 - (a & 4095)) / 8);
                    if (chunk > 4096 - words) chunk = 4096 - words;
                    SIZE_T n = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a, stack + words, chunk * 8, &n) ||
                        n != chunk * 8)
                        break;
                    words += chunk, a += chunk * 8;
                }
            ResumeThread(h);
        }
        char name[64] = "";
        PWSTR desc = NULL;
        if (get_desc && SUCCEEDED(get_desc(h, &desc)) && desc) {
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof name, NULL, NULL);
            LocalFree(desc);
        }
        CloseHandle(h);
        if (!got) continue;
        threads++;
        char line[1024];
        int len = snprintf(line, sizeof line, "[hang] thread %lu%s%s%s:", te.th32ThreadID, *name ? " (" : "", name,
                           *name ? ")" : "");
        int shown = 0;
        for (size_t i = 0; i <= words && shown < 10 && len < (int)sizeof line - 96; i++) {
            uint64_t pc = i == 0 ? ctx.Rip : stack[i - 1];
            if (pc >= g_lo && pc < g_hi) continue;  // (guest memory: never code, and VirtualQuery is slow on it)
            HMODULE m = NULL;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)(uintptr_t)pc, &m))
                continue;
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)(uintptr_t)pc, &mi, sizeof mi) ||
                !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                continue;
            uint64_t off = 0;
            const char *s = m == exe || m == game ? rt_host_fn_name(pc, &off) : plat_symbol(pc, &off);
            if (s) len += snprintf(line + len, sizeof line - len, " %s+%llx", s, (unsigned long long)off);
            else len += snprintf(line + len, sizeof line - len, " %s+%llx", m == game ? "lasgame" : "lasrecomp",
                                 (unsigned long long)(pc - (uint64_t)(uintptr_t)m));
            shown++;
        }
        fprintf(stderr, "%s\n", line);
    }
    CloseHandle(snap);
    fprintf(stderr, "[hang] %d threads\n", threads);
    fflush(stderr);
}

// The last run's end: cache/running holds a run's process id from its start (plat_run_begin) until it ends in a way
// the log tells (a closed window, back to the start screen, our crash report). Still there at the next start: that
// run ended without a word, and what Windows' event log recorded about it (Application Error / Hang: the exception and
// the module) goes into this log.
static void run_end(void) { remove("cache/running"); }

static void last_run_report(unsigned long pid) {
    typedef HANDLE(WINAPI * QueryFn)(HANDLE, LPCWSTR, LPCWSTR, DWORD);
    typedef BOOL(WINAPI * NextFn)(HANDLE, DWORD, HANDLE *, DWORD, DWORD, DWORD *);
    typedef BOOL(WINAPI * RenderFn)(HANDLE, HANDLE, DWORD, DWORD, PVOID, DWORD *, DWORD *);
    typedef BOOL(WINAPI * CloseFn)(HANDLE);
    HMODULE w = LoadLibraryA("wevtapi.dll");
    QueryFn query = w ? (QueryFn)(void *)GetProcAddress(w, "EvtQuery") : NULL;
    NextFn next = w ? (NextFn)(void *)GetProcAddress(w, "EvtNext") : NULL;
    RenderFn render = w ? (RenderFn)(void *)GetProcAddress(w, "EvtRender") : NULL;
    CloseFn evt_close = w ? (CloseFn)(void *)GetProcAddress(w, "EvtClose") : NULL;
    bool found = false;
    HANDLE q = query && next && render && evt_close
        ? query(NULL, L"Application",
                L"*[System[(EventID=1000 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= 604800000]]]",
                0x1 | 0x200)  // (EvtQueryChannelPath | EvtQueryReverseDirection: the newest first)
        : NULL;
    static wchar_t xml[16384];
    static char s[32768];
    char want[32];
    snprintf(want, sizeof want, ">0x%lx<", pid);  // (<Data Name='ProcessId'>0x1a2b</Data>)
    HANDLE ev;
    DWORD got;
    for (int seen = 0; q && !found && seen < 200 && next(q, 1, &ev, 1000, 0, &got) && got; seen++) {
        DWORD used = 0, props = 0;
        if (render(NULL, ev, 1 /* EvtRenderEventXml */, sizeof xml - 2, xml, &used, &props)) {
            WideCharToMultiByte(CP_UTF8, 0, xml, -1, s, sizeof s, NULL, NULL);
            for (char *c = s; *c; c++) *c = (char)tolower((unsigned char)*c);
            if (strstr(s, "lasrecomp") && strstr(s, want)) {
                found = true;
                char out[1024];
                int n = snprintf(out, sizeof out, "[system] the last run (process %lu) ended without a report; Windows "
                                 "recorded %s", pid, strstr(s, "<eventid>1002<") ? "it not responding (a hang)" : "a crash");
                static const char *const keys[] = {"modulename", "exceptioncode", "faultingoffset"};
                for (int k = 0; k < 3; k++) {
                    char tag[48];
                    snprintf(tag, sizeof tag, "name='%s'>", keys[k]);
                    const char *v = strstr(s, tag);
                    if (!v) continue;
                    v += strlen(tag);
                    n += snprintf(out + n, sizeof out - n, ", %s %.*s", keys[k], (int)strcspn(v, "<"), v);
                }
                fprintf(stderr, "%s\n", out);
            }
        }
        evt_close(ev);
    }
    if (q) evt_close(q);
    if (!found)
        fprintf(stderr, "[system] the last run (process %lu) ended without a report: closed from the Task Manager, a "
                        "power loss, or ended by Windows (nothing on it in Windows' event log)\n", pid);
}

void plat_run_begin(void) {
    _mkdir("cache");
    FILE *f = fopen("cache/running", "rb");
    unsigned long last = 0;
    if (f) {
        if (fscanf(f, "%lu", &last) != 1) last = 0;
        fclose(f);
        if (last) last_run_report(last);
    }
    if ((f = fopen("cache/running", "wb"))) fprintf(f, "%lu\n", GetCurrentProcessId()), fclose(f);
    atexit(run_end);
}

// ---- POSIX shims (win_compat.h)
// Guest pages are committed on the CPU's first touch (vectored_handler), but ReadFile / WriteFile on a guest buffer
// never fault: on a page only reserved they stop part way. A file read into fresh guest memory then came back short,
// the rest of the buffer keeping old bytes (Z-A: meshes loaded only in part - indices running into stale data, the
// stretched sheets and beams). The range is committed first.
static void commit_guest_range(uint64_t a, uint64_t n) {
    if (!n || a + n <= g_lo || a >= g_hi) return;
    uint64_t end = a + n < g_hi ? a + n : g_hi;
    if (a < g_lo) a = g_lo;
    extern uint64_t g_watch_lo;  // (one commit, not a VirtualQuery per region: lazy_commit says why - ~3 ms a file read)
    if (!g_watch_lo) { plat_commit(a, end - a); return; }
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return;
        uint64_t rend = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_RESERVE) {
            uint64_t lo = a & ~(COMMIT_CHUNK - 1), hi = ((rend < end ? rend : end) + COMMIT_CHUNK - 1) & ~(COMMIT_CHUNK - 1);
            if (hi > rend) hi = rend;
            if (!VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_COMMIT, PAGE_READWRITE))
                VirtualAlloc((void *)(uintptr_t)(a & ~0xFFFull), ((rend < end ? rend : end) - (a & ~0xFFFull) + 0xFFF) & ~0xFFFull,
                             MEM_COMMIT, PAGE_READWRITE);
        }
        a = rend;
    }
}
static ssize_t prw(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    static int no_commit = -1;  // (test) LARECOMP_NO_READ_COMMIT=1: the old behaviour
    if (no_commit < 0) no_commit = getenv("LARECOMP_NO_READ_COMMIT") && atoi(getenv("LARECOMP_NO_READ_COMMIT"));
    if (!no_commit) commit_guest_range((uint64_t)(uintptr_t)buf, n);
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        BOOL ok = write ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov)
                        : ReadFile(h, (char *)buf + done, chunk, &got, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            static int nlog;
            if (nlog++ < 40)
                fprintf(stderr, "[plat] %s of %#llx bytes at file offset %#llx into %p failed after %#llx: error %lu\n",
                        write ? "write" : "read", (unsigned long long)n, (unsigned long long)off, buf, (unsigned long long)done, err);
            return done ? (ssize_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, true); }

// ---- child processes
typedef struct { HANDLE process, job; } WinProcess;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    static __thread char cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
    static __thread wchar_t wcmd[32768], wlog[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
    MultiByteToWideChar(CP_UTF8, 0, log_path ? log_path : "NUL", -1, wlog, MAX_PATH);
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(wlog, append_log ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, append_log ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) return 0;
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = NULL, .hStdOutput = log,
                       .hStdError = log};
    PROCESS_INFORMATION pi;
    // CreateProcess doesn't resolve relative paths written with '/' (e.g. a helper under tools/): pass the program
    // as a full path, like posix_spawn takes it as given
    static __thread wchar_t wprog[MAX_PATH], wfull[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, wprog, MAX_PATH);
    for (wchar_t *p = wprog; *p; p++) if (*p == L'/') *p = L'\\';
    const wchar_t *app = GetFullPathNameW(wprog, MAX_PATH, wfull, NULL) ? wfull : NULL;
    BOOL ok = CreateProcessW(app, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (!ok) rt_launcher_log("[plat] could not start %s (error %lu)\n", argv[0], GetLastError());
    CloseHandle(log);
    if (!ok) return 0;
    // A separate job per helper: cancelling one compiler also stops its children immediately,
    // without killing the other compilation jobs. Handles close on app exit as well.
    WinProcess *child = calloc(1, sizeof *child);
    if (!child) {
        TerminateProcess(pi.hProcess, 255);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0;
    }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
        !AssignProcessToJobObject(job, pi.hProcess)) {
        // A restricted parent job can prevent assignment. Still run the helper, as before jobs were added;
        // cancellation can stop this process only in that case, not its descendants.
        fprintf(stderr, "[plat] could not supervise %s (error %lu); starting without a job\n", argv[0], GetLastError());
        if (job) CloseHandle(job);
        job = NULL;
    }
    child->process = pi.hProcess, child->job = job;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return (PlatProc)child;
}

void plat_proc_kill(PlatProc p) {
    WinProcess *child = (WinProcess *)p;
    if (child->job) TerminateJobObject(child->job, 255);
    else TerminateProcess(child->process, 255);
    WaitForSingleObject(child->process, INFINITE);
    CloseHandle(child->process);
    if (child->job) CloseHandle(child->job);
    free(child);
}

int plat_proc_wait(PlatProc p, bool block) {
    WinProcess *child = (WinProcess *)p;
    DWORD w = WaitForSingleObject(child->process, block ? INFINITE : 0);
    if (w == WAIT_TIMEOUT) return -1;
    DWORD code = 255;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(child->process, &code);
    CloseHandle(child->process);
    if (child->job) CloseHandle(child->job);
    free(child);
    // (a crash's NTSTATUS, e.g. 0xC0000409, is not "still running" (-1): the caller gets 255, the log keeps the real one -
    // a helper that dies before writing anything leaves only this: 0xC0000135 / 0xC0000142 can't load, 0xC0000005 crashed,
    // 0xC0000022 access denied, usually an antivirus; #99 had only "exit status 255" and an empty log)
    if (code > 255) rt_launcher_log("[plat] a helper process ended with status 0x%08lX\n", (unsigned long)code);
    return code > 255 ? 255 : (int)code;
}

// rm -rf that never goes through a link: a junction or symlink inside is removed itself (RemoveDirectory /
// DeleteFile on the reparse point), never what it points to. Cloud placeholders (OneDrive: also reparse points, but
// not redirects) are ordinary files and folders. w: a \\?\ path in a 32768-wide buffer, len its length; attr and
// tag: its FindFirstFile attributes and reparse tag.
static bool cloud_placeholder(DWORD tag) { return (tag & 0xFFFF0FFF) == 0x9000001A; }  // IO_REPARSE_TAG_CLOUD(_1.._F)
static bool remove_tree_w(wchar_t *w, size_t len, DWORD attr, DWORD tag) {
    bool dir = attr & FILE_ATTRIBUTE_DIRECTORY;
    if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) && !cloud_placeholder(tag))  // junction / symlink: the link only
        return dir ? RemoveDirectoryW(w) != 0 : DeleteFileW(w) != 0;
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(w, attr & ~FILE_ATTRIBUTE_READONLY);
    if (!dir) return DeleteFileW(w) != 0;
    bool ok = true;
    if (len + 3 >= 32768) return false;
    wcscpy(w + len, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    w[len] = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            size_t k = wcslen(fd.cFileName);
            if (len + 1 + k >= 32768) { ok = false; continue; }
            w[len] = L'\\';
            wcscpy(w + len + 1, fd.cFileName);
            ok &= remove_tree_w(w, len + 1 + k, fd.dwFileAttributes,
                                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0);
            w[len] = 0;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(w) != 0 && ok;
}

bool plat_remove_tree(const char *dir) {
    static __thread wchar_t rel[MAX_PATH * 4], w[32768];
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, rel, MAX_PATH * 4) <= 1) return false;
    for (wchar_t *p = rel; *p; p++) if (*p == L'/') *p = L'\\';
    DWORD n = GetFullPathNameW(rel, 32768 - 8, w + 4, NULL);  // (absolute, then \\?\ for long paths)
    if (!n || n >= 32768 - 8) return false;
    size_t len;
    if (w[4] == L'\\' && w[5] == L'\\') {  // \\server\share -> \\?\UNC\server\share
        memmove(w + 8, w + 6, (n - 1) * sizeof *w);
        memcpy(w, L"\\\\?\\UNC\\", 8 * sizeof *w);
        len = n + 6;
    } else {
        memcpy(w, L"\\\\?\\", 4 * sizeof *w);
        len = n + 4;
    }
    while (len > 7 && w[len - 1] == L'\\') len--;  // (no trailing separator: "\\*" is appended)
    w[len] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return remove_tree_w(w, len, fd.dwFileAttributes,
                         (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0);
}

// ---- OneDrive
// A folder's final path (links and junctions resolved), without the \\?\ prefix, backslashes, no trailing one
static bool final_path(const wchar_t *p, wchar_t *out, DWORD n) {
    HANDLE h = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD k = 0;
    if (h != INVALID_HANDLE_VALUE) {
        k = GetFinalPathNameByHandleW(h, out, n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(h);
    }
    if (!k || k >= n) k = GetFullPathNameW(p, n, out, NULL);  // (can't be opened: as written)
    if (!k || k >= n) return false;
    if (!wcsncmp(out, L"\\\\?\\UNC\\", 8)) memmove(out + 2, out + 8, (wcslen(out + 8) + 1) * sizeof *out);
    else if (!wcsncmp(out, L"\\\\?\\", 4)) memmove(out, out + 4, (wcslen(out + 4) + 1) * sizeof *out);
    for (wchar_t *c = out; *c; c++) if (*c == L'/') *c = L'\\';
    for (size_t l = wcslen(out); l > 3 && out[l - 1] == L'\\';) out[--l] = 0;
    return true;
}

#ifndef FILE_ATTRIBUTE_PINNED
#define FILE_ATTRIBUTE_PINNED 0x00080000
#define FILE_ATTRIBUTE_UNPINNED 0x00100000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

// Under one of the OneDrive folders the environment names (personal, work / school), or a cloud file itself
bool plat_onedrive_synced(const char *dir) {
    wchar_t w[MAX_PATH], d[1024], r[1024], v[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, MAX_PATH) <= 1 || !final_path(w, d, 1024)) return false;
    DWORD a = GetFileAttributesW(d);
    if (a != INVALID_FILE_ATTRIBUTES &&
        (a & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_PINNED | FILE_ATTRIBUTE_UNPINNED)))
        return true;
    static const wchar_t *const vars[] = {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"};
    for (int i = 0; i < 3; i++) {
        DWORD k = GetEnvironmentVariableW(vars[i], v, 1024);
        if (!k || k >= 1024 || !final_path(v, r, 1024)) continue;
        size_t l = wcslen(r);
        if (r[l - 1] == L'\\') l--;  // (a drive's root: "D:\")
        if (!_wcsnicmp(d, r, l) && (!d[l] || d[l] == L'\\')) return true;
    }
    return false;
}

// End the process at once, skipping DLL teardown (from an atexit handler, after flushing). Exit status 0.
void plat_terminate(void) {  // (exit()'s last handler, runtime/nvn/vulkan/device.c: the others registered after
    run_end();                 // ours never run - so the run's end is marked here)
    publish_log();
    TerminateProcess(GetCurrentProcess(), 0);
}

// main.c moved stderr to logs/play.log (Pokémon Z-A): the crash dialog names that file and it becomes lasrecomp.log at
// the end. The session file (lasrecomp-backup-*.log) keeps only the lines before the move - players attached it from
// the dialog and sent 231 null bytes (#15, #39)
void rt_log_moved(const char *path) {
    char full[MAX_PATH];
    DWORD n = GetFullPathNameA(path, sizeof full, full, NULL);
    if (!g_log_path[0] || !n || n >= sizeof full || !lock_logs()) return;
    snprintf(g_log_path, sizeof g_log_path, "%s", full);
    ReleaseMutex(g_log_mutex);
}

// stderr still the console (no redirect: started by a double click) - main.c then logs to a file
int rt_stderr_is_console(void) {
    DWORD t = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
    return t == FILE_TYPE_CHAR || t == FILE_TYPE_UNKNOWN;
}

static void set_errno_win(DWORD e) {
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
          : e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? EEXIST
          : e == ERROR_DIR_NOT_EMPTY ? ENOTEMPTY : EACCES;
}

int plat_open_file(const char *path, int flags) {
    DWORD access = (flags & O_RDWR) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD disp = !(flags & O_CREAT) ? OPEN_EXISTING : (flags & O_EXCL) ? CREATE_NEW : OPEN_ALWAYS;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { set_errno_win(GetLastError()); return -1; }
    int fd = _open_osfhandle((intptr_t)h, ((flags & O_RDWR) ? _O_RDWR : _O_RDONLY) | _O_BINARY);
    if (fd < 0) CloseHandle(h);
    return fd;
}

int plat_rename(const char *from, const char *to) {
    if (MoveFileExA(from, to, 0)) return 0;  // (no MOVEFILE_REPLACE_EXISTING: the console's rename refuses too)
    set_errno_win(GetLastError());
    return -1;
}

// A hash of what the screen shows in the window's client area (32x18 samples of the composed desktop), while it is the
// foreground window: runtime/nvn/vulkan/device.c screen_watch tells a window that stopped showing the game's frames.
bool plat_window_screen_hash(void *hwnd, uint32_t *out) {
    HWND w = (HWND)hwnd;
    if (!w || GetForegroundWindow() != w || IsIconic(w)) return false;
    RECT rc;
    if (!GetClientRect(w, &rc) || rc.right < 64 || rc.bottom < 36) return false;
    POINT o = {0, 0};
    ClientToScreen(w, &o);
    HDC sdc = GetDC(NULL);
    if (!sdc) return false;
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = 32, .biHeight = -18, .biPlanes = 1,
                                   .biBitCount = 32, .biCompression = BI_RGB}};
    void *bits = NULL;
    HBITMAP bm = mdc ? CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    bool ok = false;
    if (bm) {
        HGDIOBJ old = SelectObject(mdc, bm);
        SetStretchBltMode(mdc, COLORONCOLOR);
        ok = StretchBlt(mdc, 0, 0, 32, 18, sdc, o.x, o.y, rc.right, rc.bottom, SRCCOPY) != 0;
        GdiFlush();
        if (ok) {
            uint32_t h = 2166136261u;
            const uint8_t *p = bits;
            for (int i = 0; i < 32 * 18 * 4; i++) h = (h ^ p[i]) * 16777619u;
            *out = h;
        }
        SelectObject(mdc, old);
        DeleteObject(bm);
    }
    if (mdc) DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    return ok;
}
