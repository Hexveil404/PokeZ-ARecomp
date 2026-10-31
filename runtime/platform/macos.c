// macOS host layer (runtime/platform.h).
#include <errno.h>
#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "../paths.h"
#include "../platform.h"
#include "posix_log.h"

void rt_crash_explain(char *out, size_t n, const char *log_path);  // runtime/crash_explain.c

static mach_timebase_info_data_t tb;

// Started as the app (Finder, Dock: no terminal): stdout / stderr go to lasrecomp.log in the data folder, with five
// backups (posix_log.h; as on Linux / Windows), and a fatal error is explained in an alert instead of the app
// vanishing. Runs from a terminal or a test script (not the .app's executable) keep their own output.
static char g_log_path[1024];

static void log_to_file_in_app(void) {
    char exe[1024];
    uint32_t sz = sizeof exe;
    if (isatty(STDERR_FILENO) || _NSGetExecutablePath(exe, &sz) != 0 || !strstr(exe, ".app/Contents/MacOS/")) return;
    char dir[900];
    if (!rt_data_root_early(dir, sizeof dir)) return;  // (runtime/paths.h: the data folder, wherever it was moved)
    mkdir(dir, 0755);
    session_log_open(dir, g_log_path, sizeof g_log_path);
}

static void sysctl_str(const char *name, char *out, size_t n) {
    size_t len = n;
    if (sysctlbyname(name, out, &len, NULL, 0) != 0) out[0] = 0;
    else out[n - 1] = 0;
}
static int64_t sysctl_int(const char *name) {
    int64_t v = 0;  // (hw.memsize is 64-bit, the counts 32-bit: little-endian, so either fits)
    size_t len = sizeof v;
    return sysctlbyname(name, &v, &len, NULL, 0) == 0 ? v : 0;
}

void plat_system_info(PlatSysInfo *si) {
    char ver[32], machine[32] = "";
    sysctl_str("kern.osproductversion", ver, sizeof ver);
    struct utsname u;
    if (uname(&u) == 0) snprintf(machine, sizeof machine, "%s", u.machine);
    bool rosetta = sysctl_int("sysctl.proc_translated") == 1;
    snprintf(si->os, sizeof si->os, "macOS %s (%s%s)", ver[0] ? ver : "?", machine, rosetta ? ", Rosetta" : "");
    sysctl_str("machdep.cpu.brand_string", si->cpu, sizeof si->cpu);
    si->cores = (int)sysctl_int("hw.physicalcpu");
    si->threads = (int)sysctl_int("hw.logicalcpu");
    int64_t p = sysctl_int("hw.perflevel0.physicalcpu"), e = sysctl_int("hw.perflevel1.physicalcpu");
    if (p && e) snprintf(si->cores_detail, sizeof si->cores_detail, "%dP + %dE", (int)p, (int)e);
    si->ram = (uint64_t)sysctl_int("hw.memsize");
    char exe[1024];
    uint32_t sz = sizeof exe;
    si->package = _NSGetExecutablePath(exe, &sz) == 0 && strstr(exe, ".app/Contents/MacOS/") ? ".app" : NULL;
}

void plat_init(void) {
    log_to_file_in_app();
    raise_open_file_limit();
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    // Everything readable/writable: macOS pages are 16K but the guest maps at 4K granularity.
    // Untouched pages cost nothing.
    mach_vm_address_t a = lo;
    return mach_vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED) == KERN_SUCCESS;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    mach_vm_address_t a = lo;
    return mach_vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
bool plat_page_state(uint64_t addr, char *out, size_t n) { (void)addr; (void)out; (void)n; return false; }

int plat_written_pages_peek(uint64_t addr, uint64_t size) { (void)addr; (void)size; return -1; }
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

bool plat_mem_usage(uint64_t *process, uint64_t *guest) { (void)process; (void)guest; return false; }
void plat_sample_focus(void) {}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

uint64_t plat_now_ns(void) {
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

uint64_t plat_thread_cpu_ns(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ? (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec : 0;
}

void plat_sleep_until_ns(uint64_t t) {
    if (!tb.denom) mach_timebase_info(&tb);
    mach_wait_until(t * tb.denom / tb.numer);
}

// Real-time (time-constraint) thread: exempt from the timer coalescing macOS applies to
// ordinary threads when the app isn't frontmost, which otherwise stretches 16ms waits to ~100ms.
void plat_thread_realtime(uint64_t period_ns) {
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t period = period_ns * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)period,
        .computation = (uint32_t)(period / 20),
        .constraint = (uint32_t)(period / 4),
        .preemptible = 1,
    };
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                                         THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) fprintf(stderr, "[plat] real-time thread policy refused: %d\n", (int)kr);
}
// After a late wait: is the time-constraint policy still in effect for this thread? The kernel drops it (back to
// timeshare, timers coalesced to ~100 ms when the app isn't frontmost) for a thread that overruns its computation
// budget; set again when found gone. Called only on the late path: no cost otherwise.
int plat_thread_realtime_check(uint64_t period_ns) {
    thread_time_constraint_policy_data_t pol;
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t def = FALSE;
    kern_return_t kr = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol, &count, &def);
    if (kr != KERN_SUCCESS) return -1;
    if (!def) return 0;
    plat_thread_realtime(period_ns);
    return 1;
}

// Guest threads start at the default QoS, and on a Mac with efficiency cores (an M1 Air: 4 + 4) the scheduler may run
// the game's frame-critical threads there - the main thread (priority 44) builds and submits every frame. Priorities
// 44 and more urgent get user-interactive (performance cores first), the rest user-initiated. LARECOMP_THREAD_QOS=0:
// the default class as before.
int plat_cpu_count(void) { return 0; }  // (no affinity masks here: the caller uses the machine's count)

void plat_thread_guest(int prio) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_THREAD_QOS") || atoi(getenv("LARECOMP_THREAD_QOS")) != 0;
    if (on) pthread_set_qos_class_self_np(prio <= 44 ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
}

bool plat_exe_path(char *buf, size_t size) {
    uint32_t len = (uint32_t)size;
    return _NSGetExecutablePath(buf, &len) == 0;
}

#include <sys/sysctl.h>
void plat_os_name(char *buf, size_t size) {
    char v[64] = "";
    size_t n = sizeof v;
    if (sysctlbyname("kern.osproductversion", v, &n, NULL, 0) != 0) v[0] = 0;
    snprintf(buf, size, "macOS %s", v);
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    if (plat_exe_path(path, sizeof path)) execv(path, *_NSGetArgv());
}

const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

bool plat_module_name(uint64_t pc, char *out, size_t n) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) return false;
    snprintf(out, n, "%s", di.dli_fname);
    return true;
}

// Fatal error: what happened in plain words (runtime/crash_explain.c) - an alert when running as the app (the
// terminal shows the log otherwise). CFUserNotification: safe from any thread, no run loop needed.
void plat_fatal_notice(void) {
    char text[1600];
    rt_crash_explain(text, sizeof text, g_log_path);
    if (!g_log_path[0]) return;
    fflush(stderr);
    CFStringRef msg = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, NULL, NULL, NULL, CFSTR("LAS Recomp"), msg,
                                   CFSTR("OK"), NULL, NULL, NULL);
    if (msg) CFRelease(msg);
}

static void *notice_thread(void *text) {
    CFStringRef msg = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    CFUserNotificationDisplayAlert(0, kCFUserNotificationCautionAlertLevel, NULL, NULL, NULL, CFSTR("LAS Recomp"), msg,
                                   CFSTR("OK"), NULL, NULL, NULL);
    if (msg) CFRelease(msg);
    free(text);
    return NULL;
}
void plat_notice(const char *text) {
    if (!g_log_path[0]) return;
    pthread_t t;
    char *copy = strdup(text);
    if (copy && !pthread_create(&t, NULL, notice_thread, copy)) pthread_detach(t);
    else free(copy);
}

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++) fprintf(stderr, "  %s\n", syms[i]);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die - the report
                                                                               // itself aborting looped, 5.7 GB log)
    uint64_t fa = (uint64_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr;
    { extern uint64_t g_fault_lr; g_fault_lr = lr; }
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <fcntl.h>
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? PLAT_PROC_SIGNALED + WTERMSIG(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path) == 0 || errno == ENOENT ? 0 : -1;  // (gone already: e.g. a ._ file macOS removed with its folder on exFAT)
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

int plat_open_file(const char *path, int flags) { return open(path, flags, 0644); }
int plat_rename(const char *from, const char *to) {
    if (!access(to, F_OK)) { errno = EEXIST; return -1; }
    return rename(from, to);
}
bool plat_onedrive_synced(const char *dir) { (void)dir; return false; }

bool plat_window_screen_hash(void *hwnd, uint32_t *out) { (void)hwnd, (void)out; return false; }  // (Windows only)
