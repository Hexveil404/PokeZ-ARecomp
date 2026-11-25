// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "gamebuild.h"
#include "paths.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];

bool gamelib_old_cpu(void) {
#if defined(__x86_64__)
    const char *e = getenv("LARECOMP_OLD_CPU");  // (testing it on a newer CPU)
    if (e && *e == '1') return true;
    __builtin_cpu_init();
    return !(__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2"));
#else
    return false;
#endif
}

// The library's id file (<library>.id, written next to it by the build): its id (gamelib_full_id), read without
// loading it.
static bool id_matches(const char *lib, const char *want) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    return !strcmp(id, want);
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// Whether the library file is whole: its header and the end of its last section are there (#332: lasgame.dll left
// damaged on the disk - a crash or power loss while something wrote it, a disk or antivirus fault - failed to load
// with error 193 at every start, though its .id file still matched; now the start screen builds it again, which links
// the kept pieces in .gamebuild in a minute or two). The file's own bytes only: the OS loader still has the last word.
static bool lib_intact(const char *path, char *why, size_t why_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, why_size, "can't be read"); return false; }
    unsigned char h[4096];
    size_t n = fread(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
    long long size = ftell(f);
    fclose(f);
    bool all_zero = true;
    for (size_t i = 0; i < n && all_zero; i++) all_zero = !h[i];
    if (n < 64 || all_zero) { snprintf(why, why_size, "%s (%lld bytes)", all_zero ? "all zero" : "too short", size); return false; }
#ifdef _WIN32
    if (h[0] != 'M' || h[1] != 'Z') { snprintf(why, why_size, "no MZ header"); return false; }
    uint32_t pe = *(uint32_t *)(h + 0x3C);
    if (pe > n - 24 || memcmp(h + pe, "PE\0\0", 4)) { snprintf(why, why_size, "no PE header"); return false; }
    uint16_t nsec = *(uint16_t *)(h + pe + 6), opt = *(uint16_t *)(h + pe + 20);
    size_t sec = pe + 24 + opt;
    if (sec + 40ull * nsec > n) { snprintf(why, why_size, "section table cut"); return false; }
    long long end = 0;
    for (int i = 0; i < nsec; i++) {
        const unsigned char *s = h + sec + 40 * i;
        long long e = (long long)*(uint32_t *)(s + 20) + *(uint32_t *)(s + 16);  // PointerToRawData + SizeOfRawData
        if (e > end) end = e;
    }
    if (size < end) { snprintf(why, why_size, "cut short: %lld of %lld bytes", size, end); return false; }
#elif defined(__APPLE__)
    uint32_t m = *(uint32_t *)h;
    if (m != 0xFEEDFACFu && m != 0xCAFEBABEu && m != 0xBEBAFECAu) { snprintf(why, why_size, "no Mach-O header"); return false; }
#else
    if (memcmp(h, "\x7f" "ELF", 4)) { snprintf(why, why_size, "no ELF header"); return false; }
    uint64_t shoff = *(uint64_t *)(h + 0x28);
    uint16_t shentsize = *(uint16_t *)(h + 0x3A), shnum = *(uint16_t *)(h + 0x3C);
    long long end = (long long)shoff + (long long)shentsize * shnum;
    if (size < end) { snprintf(why, why_size, "cut short: %lld of %lld bytes", size, end); return false; }
#endif
    return true;
}

// The library's fingerprint, <library>.sum: its size and a hash of each of 64 regions of the whole file, written after
// its first good load and dropped by each build (build_cache.cpp). #332 came back on v0.1.26 with a library the header /
// section checks passed - damaged inside - so a start now also hashes the file (8 bytes a step: ~0.1 s for 340 MB once
// it's in the file cache, and the load reads it anyway): one that changed since is damaged, and the log says how (size,
// how many regions and where, when the file was last written: what happened to it then).
#define SUM_N 64
static bool lib_sample(const char *path, long long *size, uint32_t h[SUM_N], long long *mtime) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct stat sb;
    *mtime = stat(path, &sb) == 0 ? (long long)sb.st_mtime : 0;
    *size = stat(path, &sb) == 0 ? (long long)sb.st_size : 0;
    enum { CH = 1 << 16 };
    long long region = ((*size + SUM_N - 1) / SUM_N + CH - 1) / CH * CH;  // (a whole number of reads per region)
    if (region < CH) region = CH;
    static uint64_t buf[CH / 8];
    uint64_t x[SUM_N];
    for (int i = 0; i < SUM_N; i++) x[i] = 14695981039346656037ull;
    long long at = 0;
    for (size_t n; (n = fread(buf, 1, CH, f)) > 0; at += (long long)n) {
        int r = (int)(at / region);
        if (r >= SUM_N) r = SUM_N - 1;
        uint64_t v = x[r];
        size_t w = n / 8;
        for (size_t k = 0; k < w; k++) v = (v ^ buf[k]) * 1099511628211ull;
        for (size_t k = w * 8; k < n; k++) v = (v ^ ((unsigned char *)buf)[k]) * 1099511628211ull;
        x[r] = v;
    }
    fclose(f);
    for (int i = 0; i < SUM_N; i++) h[i] = (uint32_t)(x[i] ^ x[i] >> 32);
    return *size > 0;
}
static void lib_sum_write(const char *path) {
    char sp[1120];
    snprintf(sp, sizeof sp, "%s.sum", path);
    if (exists(sp)) return;  // (kept: the first good load's)
    long long size, mt;
    uint32_t h[SUM_N];
    if (!lib_sample(path, &size, h, &mt)) return;
    FILE *f = fopen(sp, "wb");
    if (!f) return;
    fprintf(f, "LASSUM1 %lld %lld", size, mt);
    for (int i = 0; i < SUM_N; i++) fprintf(f, " %08x", h[i]);
    fputc('\n', f);
    fclose(f);
}
// false (and why): the library differs from its fingerprint; true when it matches or has none yet
static bool lib_sum_ok(const char *path, char *why, size_t why_size) {
    char sp[1120];
    snprintf(sp, sizeof sp, "%s.sum", path);
    FILE *f = fopen(sp, "rb");
    if (!f) return true;
    long long size0 = -1, mt0 = 0;
    uint32_t h0[SUM_N];
    int n = fscanf(f, "LASSUM1 %lld %lld", &size0, &mt0) == 2 ? 0 : -1;
    for (int i = 0; n == 0 && i < SUM_N; i++) if (fscanf(f, " %x", &h0[i]) != 1) n = -1;
    fclose(f);
    if (n < 0) return true;  // (unreadable: no verdict)
    long long size, mt;
    uint32_t h[SUM_N];
    if (!lib_sample(path, &size, h, &mt)) return true;
    int diff = 0, first = -1;
    for (int i = 0; i < SUM_N; i++) if (h[i] != h0[i]) diff++, first = first < 0 ? i : first;
    if (size == size0 && !diff) return true;
    time_t t = (time_t)mt;
    struct tm *tm = localtime(&t);
    char when[32] = "?";
    if (tm) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", tm);
    snprintf(why, why_size, "changed since its first good start: size %lld -> %lld, %d of %d regions differ (the first %d%% in), "
             "file last written %s%s", size0, size, diff, SUM_N, first < 0 ? 0 : first * 100 / (SUM_N - 1), when,
             mt != mt0 ? "" : ", the same time as then");
    return false;
}

#ifdef _WIN32
// #332 (third round): a freshly built lasgame.dll failed with error 193 at its very first start, on a new install on
// another drive (H:\, i3-7100) - before any fingerprint existed, and with a header, section table and length the
// checks above passed. What the file checks can't see, Windows' own image mapping can: the start screen now maps the
// library as an image (LOAD_LIBRARY_AS_IMAGE_RESOURCE - the step that fails with 193, nothing of it runs), and on a
// failure says why in the log - the loader's header rules checked one by one (pe_rules), the drive (lib_drive) - and
// tries a copy on another drive (%LOCALAPPDATA%\ZA Recomp\gamelib): one that maps there is used from there (a
// folder or drive Windows won't run programs from), one that fails there too is damaged and built again.

// The loader's rules for a 64-bit PE image header: the first one broken, or "" when none is
static void pe_rules(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(out, n, "can't be read"); return; }
    unsigned char h[4096];
    size_t got = fread(h, 1, sizeof h, f);
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    fclose(f);
    if (got < 512 || h[0] != 'M' || h[1] != 'Z') { snprintf(out, n, "no MZ header"); return; }
    uint32_t pe = *(uint32_t *)(h + 0x3C);
    if (pe > got - 264 || memcmp(h + pe, "PE\0\0", 4)) { snprintf(out, n, "no PE header at %u", pe); return; }
    const unsigned char *fh = h + pe + 4, *oh = fh + 20;
    uint16_t machine = *(uint16_t *)fh, nsec = *(uint16_t *)(fh + 2), optsz = *(uint16_t *)(fh + 16), chr = *(uint16_t *)(fh + 18);
    uint16_t magic = *(uint16_t *)oh;
    uint32_t salign = *(uint32_t *)(oh + 32), falign = *(uint32_t *)(oh + 36), image = *(uint32_t *)(oh + 56),
             headers = *(uint32_t *)(oh + 60);
    uint64_t base = *(uint64_t *)(oh + 24);
#define PE_BAD(...) do { snprintf(out, n, __VA_ARGS__); return; } while (0)
    if (machine != 0x8664) PE_BAD("machine %#x, not x64", machine);
    if (magic != 0x20b) PE_BAD("optional header magic %#x, not PE32+", magic);
    if (!(chr & 0x2000) || !(chr & 0x2)) PE_BAD("characteristics %#x: not an executable DLL", chr);
    if (optsz < 240) PE_BAD("optional header %u bytes", optsz);
    if (!falign || (falign & (falign - 1)) || falign < 512 || falign > 65536) PE_BAD("file alignment %u", falign);
    if (!salign || (salign & (salign - 1)) || salign < falign) PE_BAD("section alignment %u (file %u)", salign, falign);
    if (base & 0xFFFF) PE_BAD("image base %#llx not 64 KB aligned", (unsigned long long)base);
    if (!headers || headers > image || headers > size) PE_BAD("headers %u bytes (image %u, file %lld)", headers, image, size);
    size_t sec = pe + 24 + optsz;
    if (!nsec || sec + 40ull * nsec > got) PE_BAD("%u sections: table cut", nsec);
    uint32_t next = (headers + salign - 1) & ~(salign - 1);
    for (int i = 0; i < nsec; i++) {
        const unsigned char *t = h + sec + 40 * i;
        uint32_t vsize = *(uint32_t *)(t + 8), va = *(uint32_t *)(t + 12), rsize = *(uint32_t *)(t + 16), raw = *(uint32_t *)(t + 20);
        char nm[9] = {0};
        memcpy(nm, t, 8);
        if (va != next) PE_BAD("section %s at %#x, the last one ended at %#x", nm, va, next);
        if (raw % falign || (rsize && (long long)raw + rsize > size)) PE_BAD("section %s raw %#x+%#x (file %lld)", nm, raw, rsize, size);
        uint32_t span = vsize > rsize ? vsize : rsize;
        next = (va + span + salign - 1) & ~(salign - 1);
    }
    if (image != next) PE_BAD("image size %#x, the sections end at %#x", image, next);
#undef PE_BAD
}

// The drive a path is on: type, file system, free space (for the log)
static void lib_drive(const char *path, char *out, size_t n) {
    char vol[MAX_PATH] = "?", fsname[64] = "?", full[MAX_PATH];
    DWORD serial = 0, maxlen = 0, flags = 0;
    if (!GetFullPathNameA(path, sizeof full, full, NULL)) snprintf(full, sizeof full, "%s", path);
    GetVolumePathNameA(full, vol, sizeof vol);
    GetVolumeInformationA(vol, NULL, 0, &serial, &maxlen, &flags, fsname, sizeof fsname);
    UINT t = GetDriveTypeA(vol);
    const char *ts = t == DRIVE_FIXED ? "fixed" : t == DRIVE_REMOVABLE ? "removable" : t == DRIVE_REMOTE ? "network"
                   : t == DRIVE_RAMDISK ? "RAM disk" : t == DRIVE_CDROM ? "optical" : "unknown";
    ULARGE_INTEGER fr = {0}, tot = {0};
    GetDiskFreeSpaceExA(vol, &fr, &tot, NULL);
    DWORD attr = GetFileAttributesA(full);
    bool a = attr != INVALID_FILE_ATTRIBUTES;
    snprintf(out, n, "drive %s %s %s, %.1f of %.1f GB free%s%s%s", vol, ts, fsname, fr.QuadPart / 1e9, tot.QuadPart / 1e9,
             a && (attr & FILE_ATTRIBUTE_COMPRESSED) ? ", file compressed" : "", a && (attr & FILE_ATTRIBUTE_SPARSE_FILE) ? ", file sparse" : "",
             a && (attr & FILE_ATTRIBUTE_OFFLINE) ? ", file offline" : "");
}

// 0 when Windows maps the file as a program image, else its error (193: not a valid program)
static DWORD lib_maps(const char *path) {
    char full[MAX_PATH];
    DWORD fn = GetFullPathNameA(path, sizeof full, full, NULL);
    HMODULE h = LoadLibraryExA(fn && fn < sizeof full ? full : path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) { DWORD e = GetLastError(); return e ? e : 1; }
    FreeLibrary(h);
    return 0;
}

// The copy on another drive (%LOCALAPPDATA%\ZA Recomp\gamelib\<name>), made or refreshed now: false when it can't be
static bool lib_alt(const char *path, char *alt, size_t n) {
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base) return false;
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\ZA Recomp", base);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, sizeof dir, "%s\\ZA Recomp\\gamelib", base);
    CreateDirectoryA(dir, NULL);
    const char *name = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (!name || (bs && bs > name)) name = bs;
    snprintf(alt, n, "%s\\%s", dir, name ? name + 1 : path);
    struct stat a, b;
    if (stat(path, &a) != 0) return false;
    if (stat(alt, &b) == 0 && a.st_size == b.st_size && a.st_mtime == b.st_mtime) return true;  // (CopyFile keeps the time)
    if (!CopyFileA(path, alt, FALSE)) {
        LOG("[gamelib] can't copy %s to %s (error %lu)\n", path, alt, GetLastError());
        return false;
    }
    char id[1100], altid[1100];  // (its .id beside it; the library's own id decides at load anyway)
    snprintf(id, sizeof id, "%s.id", path);
    snprintf(altid, sizeof altid, "%s.id", alt);
    CopyFileA(id, altid, FALSE);
    return true;
}
static char g_alt_for[1100], g_alt[1100];  // (a library that maps only from its copy: lib_image_ok -> gamelib_load)

// Whether Windows takes the library as a program image here; when not, the log says why and its copy elsewhere is
// tried. Remembered per file (size, time): the start screen asks often.
static bool lib_image_ok(const char *path, char *why, size_t why_size) {
    static char memo_path[1100];
    static long long memo_size, memo_time;
    static bool memo_ok;
    struct stat sb;
    if (stat(path, &sb) != 0) return true;  // (lib_intact told it already)
    if (!strcmp(memo_path, path) && memo_size == (long long)sb.st_size && memo_time == (long long)sb.st_mtime) return memo_ok;
    snprintf(memo_path, sizeof memo_path, "%s", path);
    memo_size = (long long)sb.st_size, memo_time = (long long)sb.st_mtime;
    memo_ok = true;
    DWORD e = lib_maps(path);
    const char *t = getenv("LARECOMP_GAMELIB_ALT");  // (test: =1 takes the library here as one Windows won't map)
    if (!e && t && *t == '1') e = ERROR_BAD_EXE_FORMAT;
    if (!e) return true;
    char rules[200], drive[200], alt[1100];
    pe_rules(path, rules, sizeof rules);
    lib_drive(path, drive, sizeof drive);
    LOG("[gamelib] Windows won't map %s as a program (error %lu); header: %s; %s\n", path, e, rules[0] ? rules : "follows the rules", drive);
    if (lib_alt(path, alt, sizeof alt)) {
        DWORD e2 = lib_maps(alt);
        char d2[200];
        lib_drive(alt, d2, sizeof d2);
        LOG("[gamelib] its copy %s (%s): %s\n", alt, d2,
            e2 ? "doesn't map either - the file itself is damaged" : "maps - the game code is loaded from there");
        if (!e2) {
            snprintf(g_alt_for, sizeof g_alt_for, "%s", path);
            snprintf(g_alt, sizeof g_alt, "%s", alt);
            return true;
        }
        remove(alt);
        char altid[1100];
        snprintf(altid, sizeof altid, "%s.id", alt);
        remove(altid);
    }
    memo_ok = false;
    snprintf(why, why_size, "Windows won't load it as a program (error %lu%s%s)", e, rules[0] ? ": " : "", rules);
    return false;
}
#endif

// A library whose .id matches but whose file is damaged: told once, then taken for not built (and moved aside so the
// build writes a fresh one; the build's pieces in .gamebuild are kept)
static bool lib_usable(const char *path) {
    char why[256];
    if (lib_intact(path, why, sizeof why) && lib_sum_ok(path, why, sizeof why)
#ifdef _WIN32
        && lib_image_ok(path, why, sizeof why)
#endif
    )
        return true;
    static char told[1100];
    if (strcmp(told, path)) {
        snprintf(told, sizeof told, "%s", path);
        LOG("[gamelib] %s is damaged (%s): it will be built again\n", path, why);
        char bad[1120];
        snprintf(bad, sizeof bad, "%s.damaged", path);
        remove(bad);
        rename(path, bad);
        snprintf(bad, sizeof bad, "%s.sum", path);  // (the next build's library gets its own)
        remove(bad);
    }
    return false;
}

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
    const char *env = getenv("LARECOMP_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);
    char exe_side[1024];
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
    return n;
}

// This build's game-code id (the Makefile's GAMELIB_ID): the start screen compares it with a newer release's (its notes'
// "<!-- zarecomp-gamelib: <id> -->", runtime/ui/launcher.mm update_thread) to say whether updating rebuilds the game
// code (tools/za/dist.sh writes the notes' line from make gamelib-id).
const char *gamelib_build_id(void) { return GAMELIB_ID; }

// (a library already loaded - back on the start screen after playing - counts only while it is still the one wanted:
// the on mods' .pchtxt patches are in the id, and a set turned on or off since needs another; gamelib_stale)
bool gamelib_present(const char *game_dir) {
    char want[GAMELIB_FULL_ID_SIZE];
    if (!game_dir || !gamelib_full_id(game_dir, want, sizeof want)) return g_tables != NULL;
    if (g_tables) return !strcmp(g_tables->id, want);
    char c[3][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i], want) && lib_usable(c[i])) return true;
    return false;
}

bool gamelib_stale(const char *game_dir) {
    char want[GAMELIB_FULL_ID_SIZE];
    return g_tables && game_dir && gamelib_full_id(game_dir, want, sizeof want) && strcmp(g_tables->id, want);
}

static const GameLibTables *open_lib(const char *path, const char *want) {
#ifdef _WIN32
    // (a full path: LoadLibrary takes a relative one with a folder in it from the program's folder first - a data
    // folder elsewhere (RecompLauncher's, LARECOMP_DATA_ROOT) next to a program folder with its own extracted/ loaded
    // that copy's library: "built for another version")
    char full[MAX_PATH];
    DWORD fn = GetFullPathNameA(path, sizeof full, full, NULL);
    if (fn && fn < sizeof full) path = full;
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, want)) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char want[GAMELIB_FULL_ID_SIZE] = "";
        if (!game_dir || !gamelib_full_id(game_dir, want, sizeof want)) {
            snprintf(g_error, sizeof g_error, "the game files' exefs/main is missing or unreadable");
            if (why) snprintf(why, why_size, "%s", g_error);
            return false;
        }
        char c[3][1100];
        int n = candidates(game_dir, c);
        const char *env = getenv("LARECOMP_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            if (!exists(c[i])) continue;
            bool given = i == 0 && env && *env;  // LARECOMP_GAME_LIB: loaded as given, the library's own id decides
            if (!given && !id_matches(c[i], want)) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            if (!given && !lib_usable(c[i])) {
                snprintf(g_error, sizeof g_error, "%s was damaged on the disk: open ZA Recomp again and the start "
                         "screen builds it again", c[i]);
                continue;
            }
#ifdef _WIN32
            if (!strcmp(g_alt_for, c[i])) snprintf(g_error, sizeof g_error, "%s maps only from its copy", c[i]);
            else
#endif
            g_tables = open_lib(c[i], want);
#ifdef _WIN32
            // (it maps only from its copy elsewhere - lib_image_ok -, or it failed here as not a valid program: the copy)
            if (!g_tables && (!strcmp(g_alt_for, c[i]) || strstr(g_error, "(error 193)"))) {
                char alt[1100], first[256];
                snprintf(first, sizeof first, "%s", g_error);
                bool have = !strcmp(g_alt_for, c[i]) ? snprintf(alt, sizeof alt, "%s", g_alt) > 0 : lib_alt(c[i], alt, sizeof alt);
                if (have) {
                    char rules[200], drive[200];
                    pe_rules(c[i], rules, sizeof rules);
                    lib_drive(c[i], drive, sizeof drive);
                    LOG("[gamelib] %s; header: %s; %s - trying its copy %s\n", first, rules[0] ? rules : "follows the rules", drive, alt);
                    if ((g_tables = open_lib(alt, want))) LOG("[gamelib] the game code runs from %s\n", alt);
                    else LOG("[gamelib] its copy: %s\n", g_error);
                }
            }
#endif
            if (g_tables) lib_sum_write(c[i]);
            if (g_tables) LOG("[gamelib] %s (%llu + %llu functions)\n", c[i], (unsigned long long)*g_tables->main_count,
                              (unsigned long long)*g_tables->sdk_count);
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
