// The start screen's "does this update compile the game again?" (runtime/update_marker.h, used by
// runtime/ui/launcher.mm update_thread on GitHub's release JSON): the marker tools/za/dist.sh puts at the end of a
// release's notes, compared with this build's game-code id.
#include <assert.h>
#include <stdio.h>
#include "../runtime/update_marker.h"

static int check(const char *notes, const char *mine) { return update_notes_rebuild(notes, strlen(notes), mine); }

int main(void) {
    const char *mine = "5d34c746198b5a7f";
    // the same id: no rebuild; another: a rebuild
    assert(check("\"body\": \"## Fixes\\n- ...\\n<!-- zarecomp-gamelib: 5d34c746198b5a7f -->\"", mine) == 0);
    assert(check("\"body\": \"notes <!-- zarecomp-gamelib: 0c4a48fe98dcc3e0 -->\"", mine) == 1);
    // this PC's id carries more after a '-' (old CPUs: -v2; the full id: -<build>): the id alone is compared
    assert(check("<!-- zarecomp-gamelib: 5d34c746198b5a7f -->", "5d34c746198b5a7f-v2-8A1C86C437394B69") == 0);
    assert(check("<!-- zarecomp-gamelib: 5d34c746198b5a7f -->", "0c4a48fe98dcc3e0-8A1C86C437394B69") == 1);
    // no marker (a release from before it), or one without an id: unknown
    assert(check("\"body\": \"## v0.1.20\\nfixes\"", mine) == -1);
    assert(check("<!-- zarecomp-gamelib: -->", mine) == -1);
    assert(check("", mine) == -1);
    // spacing and a shorter / longer id
    assert(check("zarecomp-gamelib:5d34c746198b5a7f", mine) == 0);
    assert(check("zarecomp-gamelib:   5d34c746198b5a7f   -->", mine) == 0);
    assert(check("zarecomp-gamelib: 5d34c746198b5a7", mine) == 1);
    assert(check("zarecomp-gamelib: 5d34c746198b5a7f0", mine) == 1);
    // only the text given: a marker past n (the next release's object) isn't this release's
    const char *two = "\"tag_name\": \"v0.1.22\" notes\"tag_name\": \"v0.1.21\" <!-- zarecomp-gamelib: 0c4a48fe98dcc3e0 -->";
    assert(update_notes_rebuild(two, (size_t)(strstr(two, "\"tag_name\": \"v0.1.21\"") - two), mine) == -1);
    puts("PASS: update notes' game-code marker (same, other, none, spacing, this PC's suffixes, release bounds)");
    return 0;
}
