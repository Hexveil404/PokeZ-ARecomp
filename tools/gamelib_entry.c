// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden.
#include "gamelib.h"

#define MOD_TABLES(m) extern const uint64_t m##_fn_addrs[], m##_fn_count; extern const GuestFn m##_fn_ptrs[];
// subsdkN: only some games have them (Pokémon Legends Arceus: subsdk0 = the movie library, subsdk1 = NEX); weak, so a
// library generated without them still links and leaves them out
#define SUB_TABLES(m) \
    extern const uint64_t m##_fn_addrs[] __attribute__((weak)), m##_fn_count __attribute__((weak)); \
    extern const GuestFn m##_fn_ptrs[] __attribute__((weak));
MOD_TABLES(main)
MOD_TABLES(sdk)
SUB_TABLES(subsdk0)
SUB_TABLES(subsdk1)

GAMELIB_EXPORT const GameLibTables *lasgame_tables(void) {
    static GameLibModule mods[GAMELIB_MAX_MODULES] = {{"main", main_fn_addrs, main_fn_ptrs, &main_fn_count},
                                                      {"sdk", sdk_fn_addrs, sdk_fn_ptrs, &sdk_fn_count}};
    static GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, 2, mods};
    if (t.nmods == 2) {
        if (&subsdk0_fn_count) mods[t.nmods++] = (GameLibModule){"subsdk0", subsdk0_fn_addrs, subsdk0_fn_ptrs, &subsdk0_fn_count};
        if (&subsdk1_fn_count) mods[t.nmods++] = (GameLibModule){"subsdk1", subsdk1_fn_addrs, subsdk1_fn_ptrs, &subsdk1_fn_count};
    }
    return &t;
}
