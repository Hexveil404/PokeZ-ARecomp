// Moving the data folder (runtime/paths.c rt_data_move / rt_data_use / rt_data_target), one step per run so a run can
// be killed partway (tools/test_data_move.py drives it). Run from a folder without extracted/, with
// LARECOMP_DATA_HOME=<scratch home>:
//   test_data_move root                     the data folder this start uses (finishes what a cut-off move left)
//   test_data_move target <picked>          what the start screen would offer for that folder
//   test_data_move move <to> [<step>]       move there; <step> (0 copy, 1 check, 2 remove the old): stall partway
//   test_data_move use <to>                 switch to it as it is
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "paths.h"
#include "platform.h"

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

static int g_stall = -1;
static void progress(int step, double f) {
    if (step != g_stall || f < 0.3) return;
    printf("STALL\n");
    fflush(stdout);
    for (;;) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    rt_paths_init();
    char err[600] = "";
    if (!strcmp(argv[1], "root")) {
        printf("ROOT %s\n", rt_data_root());
    } else if (!strcmp(argv[1], "target") && argc > 2) {
        RtDataTarget t;
        rt_data_target(argv[2], &t);
        printf("PATH %s\nHAS_DATA %d\nREFUSE %s\nBYTES %llu\n", t.path, t.has_data, t.refuse, t.bytes);
    } else if (!strcmp(argv[1], "move") && argc > 2) {
        if (argc > 3) g_stall = atoi(argv[3]);
        bool ok = rt_data_move(argv[2], progress, err, sizeof err);
        printf("MOVE %s %s\n", ok ? "ok" : "failed", err);
    } else if (!strcmp(argv[1], "use") && argc > 2) {
        bool ok = rt_data_use(argv[2], err, sizeof err);
        printf("USE %s %s\n", ok ? "ok" : "failed", err);
    } else {
        return 2;
    }
    return 0;
}
// (ZA Recomp's paths.c / platform: the launcher log and the guest write-watch window, unused here)
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
void rt_launcher_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
uint64_t g_watch_lo;
int g_watch_write;
