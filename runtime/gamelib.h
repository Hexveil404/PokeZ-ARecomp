// The recompiled game code: a library built from the player's own game files (`make gen` + `make` in a source
// checkout, the start screen's setup otherwise; runtime/gamebuild.c) and loaded at startup (runtime/gamelib.c).
// Nothing from the game ships with the app.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"

#if defined(_WIN32)
#define GAMELIB_NAME "lasgame.dll"
#define GAMELIB_EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
#define GAMELIB_NAME "lasgame.dylib"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#else
#define GAMELIB_NAME "lasgame.so"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#endif

// GAMELIB_ID (-D, from the Makefile): a hash of the generator and the headers the game code is compiled against. A
// library built with another one is rebuilt, not loaded. A library's own id (its .id file, GameLibTables.id) is
// GAMELIB_ID-<the first 16 hex digits of the game's main build ID>: switching game versions rebuilds it too.
#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif
#define GAMELIB_ABI 2  // (2: a list of modules - Pokémon Legends Arceus has subsdk0 / subsdk1 next to main and sdk)
#define GAMELIB_FULL_ID_SIZE 96

bool gamelib_old_cpu(void);  // runtime/gamelib.c: no AVX2 / FMA / BMI2 - the game code is built for x86-64-v2

// The id a library for the game files in game_dir has (GAMELIB_ID[-v2]-<build ID>); false when exefs/main is unreadable.
static inline bool gamelib_full_id(const char *game_dir, char *out, size_t size) {
    char path[1100];
    unsigned char hdr[0x60];
    snprintf(path, sizeof path, "%s/exefs/main", game_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4);
    fclose(f);
    if (!ok) return false;
    int n = snprintf(out, size, "%s%s-", GAMELIB_ID, gamelib_old_cpu() ? "-v2" : "");  // (an x86-64-v2 build: its own)
    for (int i = 0; i < 8 && n + 2 < (int)size; i++) n += snprintf(out + n, size - n, "%02X", hdr[0x40 + i]);
    return true;
}

// One recompiled module (runtime/loader.c LOADER_MODULES order: main, sdk, then the subsdkN the game has)
typedef struct {
    const char *name;        // "main", "sdk", "subsdk0"...
    const uint64_t *addrs;   // sorted guest addresses
    const GuestFn *fns;      // the functions at them
    const uint64_t *count;
} GameLibModule;

#define GAMELIB_MAX_MODULES 4

typedef struct {
    uint32_t abi;    // GAMELIB_ABI
    const char *id;  // GAMELIB_ID-<build ID> (gamelib_full_id)
    uint32_t nmods;  // main and sdk always, then the subsdkN that were generated
    const GameLibModule *mods;
} GameLibTables;

// The library's one exported function (tools/gamelib_entry.c).
typedef const GameLibTables *(*GameLibEntry)(void);
#define GAMELIB_ENTRY "lasgame_tables"

#ifdef __cplusplus
extern "C" {
#endif

// Loads the game code for the game files in game_dir (once; later calls return the first result). Looks at
// LARECOMP_GAME_LIB, next to the executable (a source checkout's build/), then game_dir. false: why says what's wrong.
bool gamelib_load(const char *game_dir, char *why, size_t why_size);
// The tables of the loaded library (NULL before gamelib_load succeeds).
const GameLibTables *gamelib_tables(void);
// True when a library usable with this build is in one of those places (without loading it).
bool gamelib_present(const char *game_dir);

#ifdef __cplusplus
}
#endif
