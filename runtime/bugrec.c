// Player bug recordings: see bugrec.h
#include "bugrec.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// ---- The zip: stored (no compression - a save is 6 MB, the rest text, and a GitHub attachment may be 25 MB), so
// no library: local headers, the central directory and its end record. Names relative to the folder, '/' between.
// The bug report's names are ours and ASCII (bug-reports/<stamp>/..., relative to the data root): plain stdio.
static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    uint32_t c = ~0u;
    while (n--) c = tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) b->p = realloc(b->p, b->cap = (b->n + n) * 2 + 4096);
    memcpy(b->p + b->n, d, n), b->n += n;
}
static void put16(Buf *b, uint32_t v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(b, x, 2); }
static void put32(Buf *b, uint32_t v) { put16(b, v & 0xFFFF), put16(b, v >> 16); }
typedef struct { FILE *out; Buf cen; uint32_t off, count; bool ok; } Zip;
static void zip_file(Zip *z, const char *path, const char *name) {
    FILE *f = fopen(path, "rb");
    if (!f) { z->ok = false; return; }
    Buf d = {0};
    uint8_t chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f))) put(&d, chunk, got);
    fclose(f);
    struct stat st;
    struct tm *t = stat(path, &st) == 0 ? localtime(&st.st_mtime) : NULL;
    uint32_t dtime = t ? (uint32_t)(t->tm_hour << 11 | t->tm_min << 5 | t->tm_sec / 2) : 0;
    uint32_t ddate = t && t->tm_year >= 80 ? (uint32_t)((t->tm_year - 80) << 9 | (t->tm_mon + 1) << 5 | t->tm_mday) : 0x21;
    uint32_t crc = crc32_of(d.p, d.n), len = (uint32_t)strlen(name);
    Buf h = {0};
    put32(&h, 0x04034b50), put16(&h, 20), put16(&h, 0), put16(&h, 0), put16(&h, dtime), put16(&h, ddate);
    put32(&h, crc), put32(&h, (uint32_t)d.n), put32(&h, (uint32_t)d.n), put16(&h, len), put16(&h, 0), put(&h, name, len);
    if (fwrite(h.p, 1, h.n, z->out) != h.n || (d.n && fwrite(d.p, 1, d.n, z->out) != d.n)) z->ok = false;
    Buf *c = &z->cen;
    put32(c, 0x02014b50), put16(c, 20), put16(c, 20), put16(c, 0), put16(c, 0), put16(c, dtime), put16(c, ddate);
    put32(c, crc), put32(c, (uint32_t)d.n), put32(c, (uint32_t)d.n), put16(c, len), put16(c, 0), put16(c, 0);
    put16(c, 0), put16(c, 0), put32(c, 0), put32(c, z->off), put(c, name, len);
    z->off += (uint32_t)(h.n + d.n), z->count++;
    free(h.p), free(d.p);
}
static bool zip_tree(Zip *z, const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (!d) return false;
    for (struct dirent *e; z->ok && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[1100], name[600];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(name, sizeof name, "%s%s", prefix, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) { z->ok = false; break; }
        if (S_ISDIR(st.st_mode)) {
            char sub[610];
            snprintf(sub, sizeof sub, "%s/", name);
            if (!zip_tree(z, path, sub)) z->ok = false;
        } else zip_file(z, path, name);
    }
    closedir(d);
    return z->ok;
}
bool bugrec_zip_dir(const char *dir, const char *zip_path) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", zip_path);
    DIR *probe = opendir(dir);  // (a missing folder: no file made at all)
    if (!probe) return false;
    closedir(probe);
    Zip z = {.out = fopen(tmp, "wb"), .ok = true};
    if (!z.out) return false;
    zip_tree(&z, dir, "");
    if (z.ok) {
        uint32_t cen_off = z.off, cen_size = (uint32_t)z.cen.n;
        put32(&z.cen, 0x06054b50), put16(&z.cen, 0), put16(&z.cen, 0), put16(&z.cen, z.count), put16(&z.cen, z.count);
        put32(&z.cen, cen_size), put32(&z.cen, cen_off), put16(&z.cen, 0);
        if (fwrite(z.cen.p, 1, z.cen.n, z.out) != z.cen.n) z.ok = false;
    }
    if (fclose(z.out) != 0) z.ok = false;
    free(z.cen.p);
    if (z.ok) remove(zip_path);  // (a good zip already there stays unless this one is complete)
    if (!z.ok || rename(tmp, zip_path) != 0) { remove(tmp); return false; }
    return true;
}

#ifndef BUGREC_ZIP_ONLY  // (local-tests/bugrec_zip_test.c builds the zip alone)
// ---- The session. "Record for a bug report" (the start screen; LARECOMP_BUG_RECORD=1 for test runs): at Play the
// save and settings are copied into bug-reports/<stamp>/ and the recorder (runtime/hle/input.c LARECOMP_RECORD, its
// .pace from framepace.c; the clock from LARECOMP_CLOCK_START) writes there - the folder is a ZA DevTools repro's
// layout, so tools/za/bug-replay.sh and DevTools replay it as it is. Packed into bug-reports/<stamp>.zip when the
// window closes, or at the next start after a crash or a kill (bugrec_pack_pending: the recording is flushed as it goes).
#include "platform.h"
#include "rt.h"
#include "settings.h"
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

bool g_bugrec_want;      // (the start screen's switch: this Play is recorded)
static char g_dir[64];   // (recording: bug-reports/<stamp>; "" otherwise)
// (any Play in this process, recorded or not: the recorder, its pacing and the game clock read their variables once -
// a second Play's recording would have no input, so the switch waits for a restart)
static bool g_used;
static int g_marks;

// <folder>/lock, held while this process records: a second ZA Recomp starting meanwhile (bugrec_pack_pending) leaves
// a live recording alone. Windows: opened unshared and deleted on close - gone with the process, even after a crash;
// POSIX: flock, released with the process (the file stays, unlocked).
#ifdef _WIN32
static HANDLE g_lock = INVALID_HANDLE_VALUE;
static void lock_take(const char *dir) {
    char p[300];
    snprintf(p, sizeof p, "%s/lock", dir);
    g_lock = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
}
static void lock_drop(void) {
    if (g_lock != INVALID_HANDLE_VALUE) CloseHandle(g_lock), g_lock = INVALID_HANDLE_VALUE;
}
static bool lock_held(const char *dir) {  // (by a running process)
    char p[300];
    snprintf(p, sizeof p, "%s/lock", dir);
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return false; }  // (a stale one: deleted with that close)
    return GetLastError() == ERROR_SHARING_VIOLATION;
}
#else
static int g_lock = -1;
static void lock_take(const char *dir) {
    char p[300];
    snprintf(p, sizeof p, "%s/lock", dir);
    if ((g_lock = open(p, O_CREAT | O_RDWR, 0644)) >= 0 && flock(g_lock, LOCK_EX | LOCK_NB) != 0) close(g_lock), g_lock = -1;
}
static void lock_drop(void) {
    if (g_lock < 0) return;
    char p[300];
    snprintf(p, sizeof p, "%s/lock", g_dir);
    unlink(p), close(g_lock), g_lock = -1;
}
static bool lock_held(const char *dir) {
    char p[300];
    snprintf(p, sizeof p, "%s/lock", dir);
    int fd = open(p, O_RDWR);
    if (fd < 0) return false;
    bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    if (!held) unlink(p);  // (a crashed session's: not part of the report)
    close(fd);
    return held;
}
#endif

bool bugrec_can_start(void) { return !g_used; }
bool bugrec_active(void) { return g_dir[0] != 0; }

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    bool ok = in && out;
    char buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in))) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    return ok;
}
static void copy_tree(const char *from, const char *to) {  // (a save folder: its files, and any folders in it)
    MKDIR(to);
    DIR *d = opendir(from);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[1100], t[1100];
        struct stat st;
        snprintf(s, sizeof s, "%s/%s", from, e->d_name), snprintf(t, sizeof t, "%s/%s", to, e->d_name);
        if (stat(s, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) copy_tree(s, t);
        else if (!copy_file(s, t)) LOG("[bugrec] couldn't copy %s\n", s);
    }
    if (d) closedir(d);
}
static void write_text(const char *dir, const char *name, const char *text) {
    char p[300];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) fputs(text, f), fclose(f);
}
static void put_env(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1); else unsetenv(k);
#endif
}

void bugrec_start(const char *root) {
    const char *e = getenv("LARECOMP_BUG_RECORD");
    if (e && *e) g_bugrec_want = atoi(e) != 0;
    if (g_used) return;
    g_used = true;
    if (!g_bugrec_want) return;
    g_bugrec_want = false, g_marks = 0;
    time_t now = time(NULL);
    char stamp[32], text[1400], p[300];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    MKDIR("bug-reports");
    snprintf(g_dir, sizeof g_dir, "bug-reports/%s", stamp);
    if (MKDIR(g_dir) != 0) { LOG("[bugrec] can't make %s: not recording\n", g_dir); g_dir[0] = 0; return; }
    lock_take(g_dir);
    snprintf(p, sizeof p, "%s/save", g_dir);
    copy_tree("save/save_0", p);  // (no save yet: an empty folder - the replay starts a new game, as the player did)
    snprintf(p, sizeof p, "%s/larecomp.ini", g_dir);
    copy_file("larecomp.ini", p);
    snprintf(text, sizeof text, "%lld\n", (long long)now);
    write_text(g_dir, "clock.txt", text);
    write_text(g_dir, "gamelib.txt", "\n");  // (DevTools' "which library": the release's own)
    // the mods by name only (mods are not ours to bundle): the replay warns when its own differ
    char mods[2048] = "";
    DIR *d = opendir("mods");
    for (struct dirent *m; d && (m = readdir(d));)
        if (m->d_name[0] != '.' && strlen(mods) + strlen(m->d_name) + 2 < sizeof mods) strcat(mods, m->d_name), strcat(mods, "\n");
    if (d) closedir(d);
    write_text(g_dir, "mods.txt", mods);
    uint8_t id[8] = {0};
    snprintf(text, sizeof text, "%s/exefs/main", root);
    FILE *f = fopen(text, "rb");
    if (f) { fseek(f, 0x40, SEEK_SET); if (fread(id, 1, 8, f) != 8) memset(id, 0, 8); fclose(f); }
    extern const char *rt_version(void);  // (runtime/ui/launcher.mm)
    snprintf(text, sizeof text, "version %s\ngame %02x%02x%02x%02x%02x%02x%02x%02x\nstarted %s\n", rt_version(), id[0], id[1],
             id[2], id[3], id[4], id[5], id[6], id[7], stamp);
    write_text(g_dir, "info.txt", text);
    extern const char *rt_log_file(void);  // (runtime/platform/*.c)
    snprintf(text, sizeof text, "%s\n", rt_log_file());
    write_text(g_dir, "log_path.txt", text);
    snprintf(p, sizeof p, "%s/input.txt", g_dir);
    put_env("LARECOMP_RECORD", p);
    snprintf(text, sizeof text, "%lld", (long long)now);
    put_env("LARECOMP_CLOCK_START", text);
    LOG("[bugrec] recording to %s\n", g_dir);
}

void bugrec_mark(void) {
    if (!bugrec_active()) return;
    extern uint64_t g_input_reads, g_replay_key, renderer_present_count(void);
    char line[128], p[300];
    snprintf(line, sizeof line, "read %llu frame %llu key %llu\n", (unsigned long long)g_input_reads,
             (unsigned long long)renderer_present_count(), (unsigned long long)g_replay_key);
    snprintf(p, sizeof p, "%s/marks.txt", g_dir);
    FILE *f = fopen(p, "ab");
    if (f) fputs(line, f), fclose(f);
    if (!g_marks++) write_text(g_dir, "mark.txt", line);  // (the first: where DevTools and bug-replay.sh stop)
    LOG("[bugrec] mark %d: %s", g_marks, line);
    extern void menu_notice(const char *text, int seconds);  // (runtime/ui/menu.mm)
    menu_notice(UI_TR("Bug marked", "Bug marcado"), 3);
}

// The folder (with the session's log) into <folder>.zip; the folder goes once the zip is made. log_path.txt (the log's
// full path: the player's user folder) stays out of the zip - it is put back if packing fails, for the next start.
static void pack(const char *dir, const char *log) {
    char p[300], zip[300], lp[300], text[1100];
    if (log && *log) {
        snprintf(p, sizeof p, "%s/play.log", dir);
        if (!copy_file(log, p)) LOG("[bugrec] couldn't copy the log %s\n", log);
    }
    snprintf(lp, sizeof lp, "%s/log_path.txt", dir);
    remove(lp);
    snprintf(zip, sizeof zip, "%s.zip", dir);
    if (!bugrec_zip_dir(dir, zip)) {
        snprintf(text, sizeof text, "%s\n", log ? log : "");
        write_text(dir, "log_path.txt", text);
        LOG("[bugrec] couldn't pack %s: the folder is kept\n", dir);
        return;
    }
    plat_remove_tree(dir);
    FILE *f = fopen("bug-reports/new.txt", "wb");  // (the start screen says where it is: launcher.mm)
    if (f) fputs(zip, f), fclose(f);
    LOG("[bugrec] packed %s\n", zip);
}

void bugrec_finish(void) {
    if (!bugrec_active()) return;
    extern void input_record_stop(void), pace_record_stop(void);
    input_record_stop(), pace_record_stop();
    put_env("LARECOMP_RECORD", "");  // (and the clock: a relaunch from the start screen - mods, updates - inherits them)
    put_env("LARECOMP_CLOCK_START", "");
    lock_drop();
    fflush(stderr);
    extern const char *rt_log_file(void);
    pack(g_dir, rt_log_file());
    g_dir[0] = 0;
}

// A replay (LARECOMP_REPLAY) of a bug report recorded by another version: the game code, settings layout and timing
// differ, so it would wander off - refused, naming both (LARECOMP_REPLAY_ANY_VERSION=1 tries anyway). A recording
// without info.txt (replay-record.sh, ZA DevTools sessions) isn't checked.
void bugrec_check_replay(void) {
    const char *r = getenv("LARECOMP_REPLAY");
    if (!r || !*r || getenv("LARECOMP_REPLAY_ANY_VERSION")) return;
    char p[1100], line[200];
    snprintf(p, sizeof p, "%s", r);
    char *slash = strrchr(p, '/'), *bs = strrchr(p, '\\');
    if (bs > slash) slash = bs;
    snprintf(slash ? slash + 1 : p, sizeof p - (size_t)(slash ? slash + 1 - p : 0), "info.txt");
    FILE *f = fopen(p, "rb");
    if (!f) return;
    extern const char *rt_version(void);
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "version ", 8) || !strcmp(line + 8, rt_version())) continue;
        LOG("[replay] recorded with %s, this is %s: replays need the same version (LARECOMP_REPLAY_ANY_VERSION=1 tries "
            "anyway)\n", line + 8, rt_version());
        fclose(f);
        exit(2);
    }
    fclose(f);
}

// A session that never got to its window close (a crash, a kill): packed now, before the start rotates its log away
// (runtime/main.c) - its log_path.txt names it. Only our unpacked recordings have log_path.txt (it stays out of the
// zips): a folder the player made or extracted there is left alone, and so is one another running ZA Recomp is
// recording into (its lock).
void bugrec_pack_pending(void) {
    DIR *d = opendir("bug-reports");
    for (struct dirent *e; d && (e = readdir(d));) {
        char dir[300], log[1100];
        struct stat st;
        snprintf(dir, sizeof dir, "bug-reports/%s", e->d_name);
        if (e->d_name[0] == '.' || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        snprintf(log, sizeof log, "%s/log_path.txt", dir);
        FILE *f = fopen(log, "rb");
        if (!f || lock_held(dir)) { if (f) fclose(f); continue; }
        if (!fgets(log, sizeof log, f)) log[0] = 0;
        fclose(f);
        log[strcspn(log, "\r\n")] = 0;
        LOG("[bugrec] %s was not packed (the game crashed or was closed): packing it now\n", dir);
        pack(dir, log);
    }
    if (d) closedir(d);
}
#endif
