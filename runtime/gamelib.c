// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

// A library whose .id matches but whose file is damaged: told once, then taken for not built (and moved aside so the
// build writes a fresh one; the build's pieces in .gamebuild are kept)
static bool lib_usable(const char *path) {
    char why[128];
    if (lib_intact(path, why, sizeof why)) return true;
    static char told[1100];
    if (strcmp(told, path)) {
        snprintf(told, sizeof told, "%s", path);
        LOG("[gamelib] %s is damaged (%s): it will be built again\n", path, why);
        char bad[1120];
        snprintf(bad, sizeof bad, "%s.damaged", path);
        remove(bad);
        rename(path, bad);
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
            g_tables = open_lib(c[i], want);
            if (g_tables) LOG("[gamelib] %s (%llu + %llu functions)\n", c[i], (unsigned long long)*g_tables->main_count,
                              (unsigned long long)*g_tables->sdk_count);
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
