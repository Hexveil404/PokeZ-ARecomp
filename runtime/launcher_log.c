// logs/launcher.log: what the start screen did, kept across starts. logs/play.log only exists once the game is set up and
// is rewritten at every start (an update's relaunch alone takes the previous one away), and helpers' own logs sit in
// hidden folders, so a failed setup or build could leave nothing to send (#99). Here: appended, opened and closed per line
// (a crash loses nothing), the old part moved to launcher-anterior.log past 1 MB. Only events: system, paths, helpers that
// failed or didn't start, what the start screen showed, crashes. rt_launcher_log also writes the line to stderr.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "rt.h"

static char g_path[1200];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void put(const char *line) {
    if (!g_path[0]) return;
    pthread_mutex_lock(&g_lock);
    FILE *f = fopen(g_path, "ab");
    if (f) {
        char at[32] = "";
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        if (t) strftime(at, sizeof at, "%H:%M:%S ", t);
        fputs(at, f);
        fputs(line, f);
        if (line[0] && line[strlen(line) - 1] != '\n') fputc('\n', f);
        fclose(f);
    }
    pthread_mutex_unlock(&g_lock);
}

void launcher_log_file(const char *fmt, ...) {
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    put(line);
}

void rt_launcher_log(const char *fmt, ...) {
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stderr);
    put(line);
}

// (the data root is the working directory by now: runtime/paths.c)
void launcher_log_open(void) {
    char dir[1024];
    if (!getcwd(dir, sizeof dir)) return;
#ifdef _WIN32
    CreateDirectoryA("logs", NULL);
#else
    mkdir("logs", 0755);
#endif
    char path[1200], old[1200];
    snprintf(path, sizeof path, "%s/logs/launcher.log", dir);
    snprintf(old, sizeof old, "%s/logs/launcher-anterior.log", dir);
    struct stat sb;
    if (!stat(path, &sb) && sb.st_size > 1024 * 1024) {
        remove(old);
        rename(path, old);
    }
    snprintf(g_path, sizeof g_path, "%s", path);
    char date[40] = "";
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t) strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", t);
    launcher_log_file("======== start %s, data folder %s\n", date, dir);
#ifdef _WIN32
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, sizeof exe);
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExA(dir, &avail, NULL, NULL))
        launcher_log_file("program %s, %.1f GB free on the data folder's drive\n", exe, (double)avail.QuadPart / (1024.0 * 1024.0 * 1024.0));
#endif
}
