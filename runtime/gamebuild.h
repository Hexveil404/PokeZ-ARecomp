// Builds the game code library (runtime/gamelib.h) on the player's machine, from their own game files: the generator
// (tools/gen.py; shipped frozen as lasgen/) turns exefs/main and exefs/sdk into C, the bundled compiler (zig/) builds
// that into <game_dir>/lasgame.*. The compiled pieces are kept in <game_dir>/.gamebuild: an update compiles only the
// pieces whose generated C changed.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // now and then, from the calling thread: how far the whole build is (0..1) and what it's doing
    void (*progress)(double fraction, const char *step);
    int jobs;  // compiler processes at once (>= 1)
    bool (*cancelled)(void);  // optional, polled on this thread; completed objects stay reusable
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log).
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);
// After a failed gamebuild_run: a helper was killed by a signal (e.g. out of memory); why then says so, for the player.
bool gamebuild_killed(void);
// The generator and the compiler are there (shipped builds; a source checkout with tools/venv and clang).
bool gamebuild_available(void);
// A code mod's NSO (exefs/subsdk0-9) lifted for guest address base and built into its own library (runtime/modlib.h):
// out_dll its path (cache/mods/...); kept when its id matches. game_dir: the game files (their exefs/main).
// o: progress and cancel, as gamebuild_run's (NULL: none; o->jobs: the compiles at once, 0: one per core).
// Cancelled: false, why "cancelled".
bool modbuild_run(const char *game_dir, const char *nso, uint64_t base, char *out_dll, size_t n,
                  const GameBuildOpts *o, char *why, size_t why_size);
// Its library is built and current: no build needed.
bool modbuild_ready(const char *nso, uint64_t base);
// An x86-64 CPU without AVX2 / FMA / BMI2: the game code (x86-64-v3) is built for x86-64-v2 here instead, under its own
// id. The id a library must have on this machine: GAMELIB_ID, + "-v2" then (runtime/gamelib.c; declared here, not in
// gamelib.h - its contents are part of GAMELIB_ID, and a new id would rebuild every player's game code).
bool gamelib_old_cpu(void);
const char *gamelib_id(void);

#ifdef __cplusplus
}
#endif
