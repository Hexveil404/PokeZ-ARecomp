// A code mod's library: a mod's NSO (exefs/subsdk0-9, exlaunch / Hakkun) lifted by tools/gen.py like the game's own
// modules, into a library of its own (cache/mods/<id>.dll, runtime/gamebuild.c modbuild_run), so that adding or
// changing a mod never builds the game code again. Its entry (tools/modlib_entry.c) hands over its function table.
// Included by the mod library too: only types and constants are shared with it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#define MODLIB_ABI 1
#define MODLIB_ENTRY "lasmod_tables"
#if defined(_WIN32)
#define MODLIB_EXT ".dll"
#elif defined(__APPLE__)
#define MODLIB_EXT ".dylib"
#else
#define MODLIB_EXT ".so"
#endif
// Mods sit right after main, as rtld places subsdk modules on the Switch: within the +-128 MB of a `b` from every
// function they hook (Hakkun writes nothing else). Room: SDK_BASE - main's end (30.5 MB on Z-A 2.0.2).
#define MAX_CODE_MODS 4
#define MOD_ALIGN 0x200000ull

typedef struct {
    uint32_t abi;            // MODLIB_ABI
    const char *id;          // its library id (modlib_id)
    const uint64_t *addrs;   // sorted guest addresses
    const GuestFn *fns;      // the functions at them
    const uint64_t *count;
    uint64_t base;           // the guest address it was lifted for
} ModLibTables;
typedef const ModLibTables *(*ModLibEntry)(void);

#ifdef __cplusplus
extern "C" {
#endif

// The guest address of the k-th code mod (0-based, in load order): after main's image (main_size bytes at MAIN_BASE)
// and the mods before it, each aligned to MOD_ALIGN; 0 when it would end past SDK_BASE.
uint64_t mod_base(uint64_t main_size, int k, const uint64_t *sizes);
// An NSO's image size as the loader maps it (segments + bss, 16 KB aligned); 0 if unreadable.
uint64_t nso_image_size(const char *path);

#ifdef __cplusplus
}
#endif
