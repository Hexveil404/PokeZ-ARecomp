// Runtime-internal declarations.
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "cpu.h"
#include "platform.h"

// One write per line: stderr is unbuffered (main.c / platform: crash logs aren't lost), and Windows' CRT then wrote a
// formatted line as dozens of separate WriteFile calls - the 2-second stats line cost ~40 ms, a hitch every 2 s.
static inline void __attribute__((format(printf, 1, 2))) rt_log(const char *fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof b) { fwrite(b, 1, (size_t)n, stderr); return; }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);  // (longer: as before)
    va_end(ap);
}
#define LOG(...) rt_log(__VA_ARGS__)
#define FATAL(...) (rt_fatal_msg(__VA_ARGS__), rt_fatal_trace())  // (runtime/crash_explain.c keeps the line)

// ---- address space layout (guest addresses == host addresses)
// macOS reserves 0x2_0000_0000-0x70_0000_0000, and guest addresses must fit in 39 bits, so the
// guest lives in a 12.5 GB window at 0x70_0000_0000 claimed at startup (see main.c).
#define GUEST_LO     0x7000000000ull
#ifdef __APPLE__
#define GUEST_HI     0x7320000000ull
#else  // room for an SDK's virtual address memory too (Pokémon Legends Z-A reserves 12 GB; MAP_NORESERVE)
#define GUEST_HI     0x7800000000ull
#endif
#define MAIN_BASE    0x7000000000ull
#define SDK_BASE     0x7008000000ull   // (128 MB for main: Pokémon Legends Z-A's image is 0x5f7a000)
#define HLE_BASE     0x7080000000ull   // fake addresses for host-implemented functions
#define HLE_SIZE     0x0000100000ull
#define TLS_BASE     0x7090000000ull
#define TLS_SIZE     0x0000100000ull
#define HLE_HEAP     0x7092000000ull   // guest-visible memory owned by host code
#define HLE_HEAP_SIZE 0x000E000000ull
#ifdef __APPLE__
#define ALIAS_BASE   0x7100000000ull   // alias region (svcMapMemory targets)
#define ALIAS_SIZE   0x0100000000ull
#else  // alias region (svcMapMemory targets; an SDK's virtual address memory reserves its address regions here)
#define ALIAS_BASE   0x7400000000ull
#define ALIAS_SIZE   0x0400000000ull   // 16 GB, up to GUEST_HI
#endif
#define HEAP_BASE    0x7240000000ull
#define HEAP_MAX     0x00D0000000ull   // 3.25 GB

// ---- the game (kernel.c; set from exefs/main.npdm by main.c)
extern uint64_t g_program_id;  // GetInfo 18, Link's Awakening's by default
extern uint64_t g_system_resource_size;  // GetInfo 16 (META+0x14): non-zero turns on the SDK's virtual address memory
extern int g_game_is_la;  // it is Link's Awakening: the fixed-address hooks of runtime/hle may run

// ---- loader.c
typedef struct Module {
    const char *name;
    uint64_t base, size;
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint64_t dynamic, init, fini, init_array, init_array_sz, fini_array, fini_array_sz;
    uint64_t symtab, strtab, nsyms;
} Module;

extern Module g_modules[4];
extern int g_nmodules;
void loader_load(const char *exefs_dir);  // (again after a reboot: modules are reloaded into wiped memory)
uint64_t loader_find_symbol(const char *name);
const char *loader_symbol_at(uint64_t addr, uint64_t *offset);

// ---- dispatch.c
typedef void (*HostFn)(Cpu *c);
void dispatch_init(void);
GuestFn dispatch_lookup(uint64_t addr);
uint64_t hle_register(const char *name, HostFn fn);  // returns fake guest address
HostFn hle_lookup_impl(const char *name);
void hle_override(uint64_t guest_addr, HostFn fn, const char *name);
void rt_fatal_trace(void) __attribute__((noreturn));
// runtime/crash_explain.c: the fatal line / crash spot, and the player's explanation of it (the crash dialog)
void rt_fatal_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_crash_fault(const char *what, uint64_t fa, uint64_t pc);
void rt_crash_explain(char *out, size_t n, const char *log_path);
extern int rt_romfs_files;
// runtime/launcher_log.c: logs/launcher.log, kept across starts (rt_launcher_log: stderr too)
void launcher_log_open(void);
void launcher_log_file(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_launcher_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int rt_gpu_bad_code;     // (invalid instructions on this driver: device.c vk_bad_code_note)
extern bool rt_game_data_bad;  // (the game's decompressor reported damaged data: am.c)
extern int rt_mods_on;         // (mods layered over the game's files: fs.c)
void crash_handler_install(void);  // per thread (alternate signal stack)

// ---- kernel.c
typedef struct KThread KThread;
void kernel_init(void);
void kernel_stop_threads(void (*idle)(void));  // reboot: unwind every guest thread and join it (idle: called while waiting)
void kernel_reset(void);                       // reboot: empty handle table, memory map, thread list
KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack_size, int prio);
void kernel_run_main(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4);
uint32_t kernel_current_thread_handle(void);
Cpu *kernel_current_cpu(void);

// ---- ipc.c
typedef struct Service Service;
int ipc_handle_request(Cpu *c, uint32_t handle);
uint32_t ipc_connect_named_port(const char *name);

// ---- in-place reboot (main.c): switch seeds or go back to the start screen without leaving the process
enum { RT_REBOOT_NONE, RT_REBOOT_GAME, RT_REBOOT_TITLE };  // GAME: boot again at once (new seed); TITLE: start screen
void rt_request_reboot(int kind);  // any thread; the window loop (ui_run) returns it and main() carries it out
int rt_reboot_requested(void);
extern volatile int g_rt_unwind;   // set while guest threads are being stopped (kernel.c)
void rt_unwind_point(void);        // guest threads: unwind to thread_main if a reboot is stopping them

// ---- ui (runtime/nvn/metal.m)
int ui_run(void);  // window loop; returns the reboot kind requested (the window stays open)

// ---- helpers
void guest_call(Cpu *c, uint64_t addr);   // call guest function at addr (saves/restores x30)
uint64_t guest_sym(const char *name);     // cached loader_find_symbol, fatal if missing
void *hle_alloc(uint64_t size);           // zeroed guest-visible memory (never freed)
extern const char *g_romfs_dir;
