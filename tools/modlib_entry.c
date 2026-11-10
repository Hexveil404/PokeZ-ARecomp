// Entry of a code mod's library (runtime/modlib.h), compiled into it with the mod's generated code (gen.py, module
// name "mod"): its function table, found by name when the game loads the library. GAMELIB_ID (-D) is the library's
// own id (runtime/gamebuild.c modbuild_run), MODLIB_BASE (-D) the guest address the mod was lifted for.
#include "gamelib.h"
#include "modlib.h"

extern const uint64_t mod_fn_addrs[], mod_fn_count;
extern const GuestFn mod_fn_ptrs[];

GAMELIB_EXPORT const ModLibTables *lasmod_tables(void) {
    static const ModLibTables t = {MODLIB_ABI, GAMELIB_ID, mod_fn_addrs, mod_fn_ptrs, &mod_fn_count, MODLIB_BASE};
    return &t;
}
