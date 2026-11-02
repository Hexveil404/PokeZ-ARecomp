// Guest addresses of the game's code and data, per game version (game_addresses/versions.h). GA(NAME) is the
// selected version's address: game_version_select (runtime/game_addresses.c) picks the table by the main program's
// build ID when the game boots, before any game code runs. Unknown build IDs are refused: there is no fallback table.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameAddrs {
#define GA_ADDR(name, value) uint64_t name;
#include "game_addresses/patches_v1_0_0.h"
#include "game_addresses/runtime_v1_0_0.h"
#undef GA_ADDR
} GameAddrs;

// The selected table (NULL until game_version_select succeeds).
extern const GameAddrs *g_ga;
#define GA(name) (g_ga->name)
// For tables built at compile time: GA_FIELD(NAME) there, GA_AT(field) to read the selected version's address.
#define GA_FIELD(name) offsetof(GameAddrs, name)
#define GA_AT(field) (*(const uint64_t *)((const char *)g_ga + (field)))

typedef struct GameVersion {
    const char *key;       // "v1_0_1": the tables' suffix
    const char *label;     // "1.0.1 / 1.1.0": every release with this main program
    const char *build_id;  // the main program's build ID, 40 upper-case hex digits
    const GameAddrs *addrs;
} GameVersion;

bool game_files_are_la(const char *game_dir);  // (game_addresses.c)
// The version whose main build ID is the first 20 bytes of id (exefs/main 0x40), or NULL.
extern uint8_t g_main_build_id[8];  // the main module's, set at start
const GameVersion *game_version_by_build_id(const uint8_t id[20]);
// The version of a main program file (exefs/main), or NULL (missing, unreadable or unknown).
const GameVersion *game_version_of_main(const char *main_path);
// The version of the game files in game_dir (its exefs/main), or NULL.
const GameVersion *game_version_of(const char *game_dir);
// The release label of the exefs folder exefs_dir: "1.0.1" or "1.1.0" when its main.npdm tells releases that share one
// main program apart (by its SHA-256), else the version's label. Display and the log only. NULL: unknown version.
const char *game_release_label(const char *exefs_dir, char *buf, size_t size);
// Selects the address table for the game files in game_dir and logs the version. false: why says what's wrong.
bool game_version_select(const char *game_dir, char *why, size_t why_size);
// The selected version (NULL before game_version_select).
const GameVersion *game_version(void);

#ifdef __cplusplus
}
#endif
