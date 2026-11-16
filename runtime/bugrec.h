// Player bug recordings (runtime/bugrec.c): a session recorded for a bug report and packed into one zip.
#pragma once
#include <stdbool.h>

// Every file under dir (sub-folders too) into a stored zip at zip_path; false, and no zip, on any error
bool bugrec_zip_dir(const char *dir, const char *zip_path);

extern bool g_bugrec_want;          // the start screen's "Record for a bug report" (LARECOMP_BUG_RECORD=1: test runs)
bool bugrec_can_start(void);        // false once a session ran in this process (its game time doesn't restart)
bool bugrec_active(void);           // recording now
void bugrec_start(const char *root);  // at Play, before the game boots (root: the game files, for the build ID)
void bugrec_mark(void);             // F8 / F1 "Mark the bug now": the replay stops at the first
void bugrec_finish(void);           // the window closing, or back to the title: the zip
void bugrec_pack_pending(void);     // at start: a crashed / killed session's zip
void bugrec_check_replay(void);     // at start: a bug report replayed on another version is refused (exit 2)
