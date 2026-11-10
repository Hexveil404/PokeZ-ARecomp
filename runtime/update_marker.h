// Whether a release rebuilds the game code (runtime/ui/launcher.mm update_thread; tools/test_update_marker.c): its
// notes end with "<!-- zarecomp-gamelib: <id> -->" (tools/za/dist.sh writes the line from make gamelib-id), compared
// with this build's GAMELIB_ID (runtime/gamelib.c gamelib_build_id: "<16 hex>", or with "-v2" / "-<build>" after it).
// notes / n: the release's text (its JSON object up to the next release's: the marker isn't escaped there).
// -> 1: another id (the game is compiled again after updating), 0: the same, -1: no marker (releases before it).
#pragma once
#include <ctype.h>
#include <stddef.h>
#include <string.h>

static inline int update_notes_rebuild(const char *notes, size_t n, const char *mine) {
    static const char mark[] = "zarecomp-gamelib:";
    const size_t ml = sizeof mark - 1;
    for (size_t i = 0; i + ml <= n; i++) {
        if (memcmp(notes + i, mark, ml)) continue;
        size_t s = i + ml;
        while (s < n && notes[s] == ' ') s++;
        size_t e = s;
        while (e < n && isxdigit((unsigned char)notes[e])) e++;
        if (e == s) return -1;  // (a marker without an id: as if none)
        size_t mine_len = strcspn(mine, "-");  // (the id alone: "-v2" / "-<build>" belong to this PC)
        return !(e - s == mine_len && !strncmp(notes + s, mine, mine_len));
    }
    return -1;
}
