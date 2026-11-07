// Game-files delete guard (runtime/paths.c rt_remove_game_file(s), plat_remove_tree): the start screen's "Delete the
// game files" never deletes through a symlink / junction, nor anything outside <data root>/extracted, and a plain
// setup still deletes its own files. Scratch folders only: tools/test_delete_guard.py <empty scratch dir>.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "paths.h"
#include "platform.h"  // (Windows: win_compat.h's mkdir)

// (the platform file's crash handling, never reached here)
void rt_crash_explain(char *out, size_t n, const char *log_path) { (void)log_path; if (n) out[0] = 0; }
bool rt_fault_watch(uint64_t a, uint64_t pc) { (void)a, (void)pc; return false; }
uint64_t g_fault_lr;  // (dispatch.c's: the platform handler stores the faulting link register here)
void rt_fault_report(const char *what, uint64_t a, uint64_t pc, uint64_t lr) {
    (void)a, (void)pc, (void)lr;
    fprintf(stderr, "fault: %s\n", what);
    abort();
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) { (void)pc, (void)offset; return NULL; }

static int failures;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } \
    } while (0)

static bool there(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}
static void put(const char *p) {
    FILE *f = fopen(p, "wb");
    if (!f) { fprintf(stderr, "can't write %s\n", p); exit(2); }
    fputs("dummy", f);
    fclose(f);
}
static void dir(const char *p) { mkdir(p, 0755); }

// A folder link (POSIX symlink; Windows junction, which needs no privilege)
static bool link_dir(const char *target, const char *link) {
#ifdef _WIN32
    char cmd[2400], t[1024], l[1024];
    if (!_fullpath(t, target, sizeof t) || !_fullpath(l, link, sizeof l)) return false;
    for (char *c = t; *c; c++) if (*c == '/') *c = '\\';
    for (char *c = l; *c; c++) if (*c == '/') *c = '\\';
    snprintf(cmd, sizeof cmd, "cmd /c mklink /J \"%s\" \"%s\" >NUL", l, t);
    return system(cmd) == 0 && there(link);
#else
    char t[1024];
    if (!realpath(target, t)) return false;
    return symlink(t, link) == 0;
#endif
}

// A fake game-files folder: the entries the setup makes, with dummy contents
static void fake_game_files(const char *d) {
    char p[1100];
    dir(d);
    snprintf(p, sizeof p, "%s/exefs", d), dir(p);
    snprintf(p, sizeof p, "%s/exefs/main", d), put(p);
    snprintf(p, sizeof p, "%s/romfs", d), dir(p);
    snprintf(p, sizeof p, "%s/romfs/data.bin", d), put(p);
    snprintf(p, sizeof p, "%s/romfs.bin", d), put(p);
}

int main(int argc, char **argv) {
    if (argc != 2 || chdir(argv[1]) != 0) { fprintf(stderr, "usage: test_delete_guard <empty scratch dir>\n"); return 2; }
    // other/: files that are not the app's (stand-in for a main checkout's extracted/); root/: the data folder
    fake_game_files("other");
    dir("other/keep"), put("other/keep/precious");
    dir("root");
    char root[1024];
    if (!getcwd(root, sizeof root)) return 2;
    strcat(root, "/root");
#ifdef _WIN32
    _putenv_s("LARECOMP_DATA_ROOT", root);
#else
    setenv("LARECOMP_DATA_ROOT", root, 1);
#endif
    rt_paths_init();  // chdir to root/
    const char *O = "../other";

    // 1. extracted/ is a link to someone else's game files: nothing deleted
    CHECK(link_dir(O, "extracted"));
    CHECK(!rt_remove_game_file("extracted", "romfs.bin"));
    CHECK(!rt_remove_game_files("extracted"));
    CHECK(there("../other/romfs.bin") && there("../other/romfs/data.bin") && there("../other/exefs/main"));
    CHECK(there("extracted"));
#ifdef _WIN32
    RemoveDirectoryA("extracted");  // (the junction only)
#else
    unlink("extracted");
#endif

    // 2. a folder outside the data root, given directly: refused
    char other_abs[1100];
    snprintf(other_abs, sizeof other_abs, "%s/../other", root);
    CHECK(!rt_remove_game_files(other_abs));
    CHECK(there("../other/romfs.bin") && there("../other/romfs/data.bin"));

    // 3. a real extracted/ whose romfs is a link out, and whose exefs holds a link out: the links go, never their targets
    dir("extracted");
    CHECK(link_dir("../other/romfs", "extracted/romfs"));
    dir("extracted/exefs");
    put("extracted/exefs/main");
    CHECK(link_dir("../other/keep", "extracted/exefs/inner"));
    CHECK(!rt_remove_game_file("extracted", "romfs"));  // (a link: refused)
    CHECK(there("../other/romfs/data.bin"));
    CHECK(rt_remove_game_file("extracted", "exefs"));   // (removes the link inside, not what it points to)
    CHECK(!there("extracted/exefs"));
    CHECK(there("../other/keep/precious"));
#ifdef _WIN32
    RemoveDirectoryA("extracted/romfs");
#else
    unlink("extracted/romfs");
#endif
    rmdir("extracted");

    // 4. reached through a linked parent folder (resolves outside the data root): refused
    dir("../outside");
    fake_game_files("../outside/extracted");
    CHECK(link_dir("../outside", "via"));
    CHECK(!rt_remove_game_files("via/extracted"));
    CHECK(there("../outside/extracted/romfs.bin") && there("../outside/extracted/romfs/data.bin"));

    // 5. a plain setup (no links): its own game files are deleted as before; anything else in there stays
    fake_game_files("extracted");
    put("extracted/mine.txt");
    CHECK(rt_remove_game_file("extracted", "romfs.bin"));
    CHECK(!there("extracted/romfs.bin"));
    CHECK(!rt_remove_game_file("extracted", "mine.txt"));  // (not app-created)
    CHECK(rt_remove_game_files("extracted"));
    CHECK(!there("extracted/romfs") && !there("extracted/exefs") && there("extracted/mine.txt"));
    remove("extracted/mine.txt");
    fake_game_files("extracted");
    CHECK(rt_remove_game_files("extracted"));
    CHECK(!there("extracted"));  // (emptied, so the folder goes too)

    // nothing outside root/ was touched
    CHECK(there("../other/romfs.bin") && there("../other/romfs/data.bin") && there("../other/exefs/main") &&
          there("../other/keep/precious") && there("../outside/extracted/exefs/main"));
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    printf("delete guard: all checks passed\n");
    return 0;
}
// (ZA Recomp's paths.c / platform: the launcher log and the guest write-watch window, unused here)
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
void rt_launcher_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
uint64_t g_watch_lo;
int g_watch_write;
