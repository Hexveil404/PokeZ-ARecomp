// Where a mod's romfs / exefs is in the layouts mods come in (runtime/services/mod_layout.c): mods/<name>/romfs, the
// Switch's atmosphere/contents/<title id>/romfs, Ryujinx' contents/<title id>/romfs or <title id>/romfs, and the same
// one folder down (a zip's "<Name>/romfs"). The start screen's installer moves a mod's romfs to mods/<name>/romfs; these
// are for mods copied into mods/ by hand.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// out: the mod's romfs folder (mod_dir + the path inside it, as on disk); false when there is none.
bool mod_romfs_dir(const char *mod_dir, char *out, size_t n);
// out: the mod's exefs folder, at the same places (romfs -> exefs); false when there is none.
bool mod_exefs_dir(const char *mod_dir, char *out, size_t n);
// The mod changes the game's code: an exefs with a subsdk0-9 or a .pchtxt in it (a main.npdm alone is not code).
bool mod_has_code(const char *mod_dir);
bool mod_is_subsdk(const char *name);  // exefs/subsdk0-9: a code mod's program (any case)
// A .pchtxt's @enabled byte patches (tools/pchtxt.py's rules) written into image, an NSO's loaded image of size bytes;
// those below keep_below are left alone (main's .text: the lifted code already has them). The bytes written, or -1
// with nothing written when a line isn't a byte patch or a patch is outside the image.
int pchtxt_apply(const char *text, uint8_t *image, uint64_t size, uint64_t keep_below);

#ifdef __cplusplus
}
#endif
