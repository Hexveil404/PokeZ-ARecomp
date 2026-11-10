// Run the production host filesystem with scratch files; unrelated game paths must not be used.
#pragma once
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include "../runtime/hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#ifdef _WIN32
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _read(fd, buf, (unsigned)n);
}
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _write(fd, buf, (unsigned)n);
}
#endif
static int sync_error, write_interrupts, write_zero;
static size_t write_limit, written;
static int64_t write_fail_after = -1;
static int test_sync(int fd) {
    if (sync_error) { errno = EIO; return -1; }
    return fsync(fd);
}
static ssize_t test_write(int fd, const void *p, size_t n, off_t off) {
    if (write_interrupts) { write_interrupts--; errno = EINTR; return -1; }
    if (write_zero) return 0;
    if (write_fail_after >= 0) {
        if (written >= (size_t)write_fail_after) { errno = ENOSPC; return -1; }
        if (n > (size_t)write_fail_after - written) n = (size_t)write_fail_after - written;
    }
    if (write_limit && n > write_limit) n = write_limit;
    ssize_t result = pwrite(fd, p, n, off);
    if (result > 0) written += (size_t)result;
    return result;
}
#define fsync test_sync
#define pwrite test_write
#include "../runtime/services/fs.c"
#undef pwrite
#undef fsync
const char *g_romfs_dir;
int rt_romfs_files;
const char *rando_seed_dir(void) { return NULL; }
bool romfs_layout(const char *p, RomfsLayout *l, char *why, size_t n) { abort(); }
bool rando_romfs_override(const char *p, char *out, size_t n) { abort(); }
void romfs_file_read_hook(const char *p) { abort(); }
void enemy_random_filter(const char *p, uint64_t off, uint8_t *d, uint64_t n) {}
void rt_fatal_msg(const char *fmt, ...) { abort(); }
void rt_fatal_trace(void) { abort(); }
void save_editor_file_read(const char *p, uint64_t a, int64_t o, int64_t n) {}
void pace_hitch(const char *why) {}  // (runtime/nvn/pace.c: a save commit is a stall)
static int notices;  // save-failed warnings shown (fs.c tells the player once per run)
void plat_notice(const char *text) { notices++; }
// (ZA Recomp's fs.c: mods from the settings, host open / rename with Windows sharing, the button prompts' romfs patch,
// read timings)
#include "../runtime/settings.h"
#include <fcntl.h>
#include <time.h>
Settings g_settings;
#include "../runtime/input_bind.h"
int16_t g_bind_pad[BIND_PAD_COUNT];
int g_game_is_la = 1;
// (fs.c's Oodle-error report: page state and the running threads - not known here)
bool plat_page_state(uint64_t addr, char *out, size_t n) { (void)addr; (void)out; (void)n; return false; }
int kernel_live_threads(void) { return 0; }
// (fs.c's Oodle retry, rt_za_oodle_call: only game code built with the hook calls it - never here)
GuestFn dispatch_lookup(uint64_t addr) { (void)addr; return NULL; }
bool rt_game_data_bad;
int rt_mods_on;
void settings_load(void) {}
bool settings_list_has(const char *list, const char *name) { return false; }
const char *pl_prompt_set(void) { return NULL; }
#ifdef _WIN32  /* (as runtime/platform/windows.c: shared for delete, so an open save can be renamed over - the autosave fix) */
#include <windows.h>
#include <io.h>
static void test_errno_win(DWORD e) { errno = e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? EEXIST : e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES; }
int plat_open_file(const char *path, int flags) {
    DWORD access = (flags & O_RDWR) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD disp = !(flags & O_CREAT) ? OPEN_EXISTING : (flags & O_EXCL) ? CREATE_NEW : OPEN_ALWAYS;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { test_errno_win(GetLastError()); return -1; }
    int fd = _open_osfhandle((intptr_t)h, ((flags & O_RDWR) ? _O_RDWR : _O_RDONLY) | _O_BINARY);
    if (fd < 0) CloseHandle(h);
    return fd;
}
int plat_rename(const char *from, const char *to) {
    if (MoveFileExA(from, to, 0)) return 0;
    test_errno_win(GetLastError());
    return -1;
}
#else
int plat_open_file(const char *path, int flags) { return open(path, flags, 0644); }
int plat_rename(const char *from, const char *to) { return rename(from, to); }
#endif
uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
#include "../runtime/game_addresses.h"
static uint8_t live_save[0x6000];
static GameAddrs test_ga = {  // the 1.0.0 table, with the live save in host memory
#define GA_ADDR(name, value) .name = UINT64_C(value),
#include "../runtime/game_addresses/patches_v1_0_0.h"
#include "../runtime/game_addresses/runtime_v1_0_0.h"
#undef GA_ADDR
};
const GameAddrs *g_ga = &test_ga;
__attribute__((constructor)) static void test_ga_init(void) { test_ga.LIVE_SAVE = (uintptr_t)live_save; }
#include "../runtime/hle/flags_ext.c"
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    Service *s = calloc(1, sizeof *s); s->name = name; s->handler = handler; s->state = state; return s;
}
void ipc_return_object(IpcResp *r, Service *s) { r->objects[r->nobjects++] = s; }
static void put_file(const char *p, unsigned byte, size_t n) {
    FILE *f = fopen(p, "wb"); assert(f);
    while (n--) assert(fputc(byte, f) != EOF);
    assert(!fclose(f));
}
static int first_byte(const char *p) {
    FILE *f = fopen(p, "rb"); assert(f); int b = fgetc(f); fclose(f); return b;
}
static IpcResp open_file(Service *s, const char *p, uint32_t mode) {
    IpcReq q = {.cmd=8, .data=(const uint8_t *)&mode, .nx=1}; q.x[0].addr = (uintptr_t)p;
    IpcResp r = {0}; filesystem(s, &q, &r); return r;
}
static void close_file(Service *s) { s->on_close(s); assert(!s->state); free(s); }
static uint32_t write_file(Service *s, const void *p, size_t n, unsigned option) {
    uint64_t args[3] = {option, 0, n}; IpcReq q = {.cmd=1, .data=(const uint8_t *)args, .na=1};
    q.a[0] = (IpcBuf){(uintptr_t)p, n}; IpcResp r = {0}; file(s, &q, &r); return r.result;
}
static uint32_t commit_mount(Service *s) {
    IpcReq q = {.cmd=10}; IpcResp r = {0}; filesystem(s, &q, &r); return r.result;
}
