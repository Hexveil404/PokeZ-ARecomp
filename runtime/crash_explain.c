// A fatal error, explained to the player: the crash dialog (plat_fatal_notice) says what went wrong in plain words
// and what to do about it; the technical lines (FATAL / signal, backtrace, registers) stay in the log for bug reports.
// Known causes are recognised from what the runtime knows when it stops: the fatal line, where the crash was (the
// game code, LAS Recomp itself, the graphics driver), and the game files.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "platform.h"
#include "rt.h"

static char g_line[512];   // the first fatal line (FATAL message / "signal 11 accessing 0x10")
static uint64_t g_pc;      // a crash's host pc (0: a FATAL)
bool rt_game_data_bad;
int rt_mods_on;
int rt_gpu_bad_code;
int rt_romfs_files = -1;   // the game's own RomFS files (runtime/services/fs.c; -1: not read yet)
enum { ROMFS_FILES = 3836 };  // (every supported version: runtime/ui/launcher.mm counts the same)

void rt_fatal_msg(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL: %s", b);
    if (!g_line[0]) snprintf(g_line, sizeof g_line, "%s", b);
}

void rt_crash_fault(const char *what, uint64_t fa, uint64_t pc) {
    if (!g_line[0]) snprintf(g_line, sizeof g_line, "%s accessing %#llx", what, (unsigned long long)fa);
    if (!g_pc) g_pc = pc;
}

static bool has(const char *s) { return strstr(g_line, s) != NULL; }

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c: inside the game code?

static bool contains_nocase(const char *hay, const char *needle) {  // (no strcasestr on Windows)
    for (; *hay; hay++) {
        size_t i = 0;
        while (needle[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (!needle[i]) return true;
    }
    return false;
}

// The graphics driver's own code (the crash was inside it, not in LAS Recomp): Vulkan drivers on Windows / Linux,
// Apple's Metal driver
static bool in_gpu_driver(uint64_t pc) {
    char m[512];
    if (!pc || !plat_module_name(pc, m, sizeof m)) return false;
    static const char *const drivers[] = {"nvoglv", "nvgpucomp", "nvwgf", "libnvidia", "amdvlk", "amdxc", "atiumd",
                                          "atig6", "igvk", "igc64", "igd", "libvulkan_radeon", "libvulkan_intel",
                                          "libvulkan_nouveau", "libvulkan_lvp", "libvulkan_virtio", "AGXMetal",
                                          "MTLCompiler", "libLLVM", "radeonsi"};
    for (size_t i = 0; i < sizeof drivers / sizeof *drivers; i++)
        if (contains_nocase(m, drivers[i])) return true;
    return false;
}

// (device.c vk_compile_crash_recover; LARECOMP_TEST_COMPILE_CRASH: the test's crash is in build_pipeline itself)
bool rt_compile_crash_at(uint64_t pc) { return in_gpu_driver(pc) || getenv("LARECOMP_TEST_COMPILE_CRASH"); }

void rt_crash_explain(char *out, size_t n, const char *log_path) {
    static const char *const report = "Please report it as a GitHub issue and attach the log file below. "
                                      "Your progress up to your last save or autosave is safe.";
    const char *app = g_game_is_la ? "LAS Recomp" : "ZA Recomp";
    char head[160], what[600];
    extern void vk_compile_crash_note(void) __attribute__((weak));  // (Vulkan: was this thread compiling a pipeline?)
    if (vk_compile_crash_note && g_pc && rt_compile_crash_at(g_pc)) vk_compile_crash_note();
    // (the file count is Link's Awakening's: Pokémon Z-A's RomFS has its own, and every crash read as missing files)
    if (g_game_is_la && rt_romfs_files >= 0 && rt_romfs_files < ROMFS_FILES) {
        snprintf(head, sizeof head, "Your game files are incomplete");
        snprintf(what, sizeof what, "%s found %d of the game's %d files, and the game stopped when it needed "
                 "one that's missing. Copy your game files again so none are missing, then set them up again on the "
                 "start screen (Game Files).", app, rt_romfs_files, (int)ROMFS_FILES);
    } else if (has("doesn't fit its language")) {
        snprintf(head, sizeof head, "Your game files' text has been changed");
        snprintf(what, sizeof what, "The game's text for this language comes from another language (a translation "
                 "patch), and the game stopped at a question it can't show that way. Set up unmodified game files on "
                 "the start screen (Game Files). To play in another language, pick it in Settings (Language).");
    } else if (has("built for another version")) {
        snprintf(head, sizeof head, "The game code was built for other game files or another version of %s", app);
        snprintf(what, sizeof what, "Start %s again: the start screen rebuilds the game code for these game "
                 "files by itself.", app);
    } else if (has("unsupported version of the game")) {
        snprintf(head, sizeof head, "Wrong version of the game");
        snprintf(what, sizeof what, "%s needs version %s of the game. Set up those game files on the start screen "
                 "(Game Files).", app, g_game_is_la ? "1.0.0, 1.0.1 or 1.1.0" : "1.0.0, 2.0.0 or 2.0.2");
    } else if (has("no font for the game's text")) {
        snprintf(head, sizeof head, "No font for the game's text");
        snprintf(what, sizeof what, "Import the Switch fonts in Settings (Game font), or reinstall %s - it "
                 "comes with a font of its own.", app);
    } else if (has("get_terminate") || has("uncaught C++ exception")) {  // (the game threw: recompiled code can't
                                                                         // unwind; runtime/hle/warp.c rt_cxa_throw)
        snprintf(head, sizeof head, "The game ran into an error it couldn't recover from");
        snprintf(what, sizeof what, "This is a bug in %s, not a problem with your game files or PC. %s", app, report);
    } else if (rt_gpu_bad_code < 0) {
        snprintf(head, sizeof head, "The graphics driver's shader compiler crashed");
        snprintf(what, sizeof what, "It crashed on one of the game's shaders. %s now knows which one and will skip it, so one "
                 "effect may be missing. Start the game again: nothing is lost, up to your last save or autosave.", app);
    } else if (rt_gpu_bad_code) {  // (before the Vulkan messages: the device loss that follows says nothing useful)
        snprintf(head, sizeof head, "The graphics driver produced bad code for the GPU");
        snprintf(what, sizeof what, rt_gpu_bad_code == 1
                 ? "%s has deleted the driver's shader cache, which may be why. Start the game again: it stutters for a while "
                   "as shaders are built again, nothing else changes. If it comes back, updating your graphics driver can help."
                 : "%s will now build shaders without the driver's optimizer, which avoids this kind of driver bug. Start the "
                   "game again: it may run slower until you update your graphics driver.", app);
    } else if (has("svcBreak") && rt_game_data_bad) {  // (the same spot every time: nothing another start would change, #89)
        snprintf(head, sizeof head, "Damaged game data");
        snprintf(what, sizeof what, "The game read data from its files that can't be unpacked, so it stopped. %s"
                 "Dump the game again from your Switch and set it up on the start screen (Game Files).",
                 rt_mods_on ? "Turn your mods off first: a mod made for another version of the game can do this. If it still happens: " : "");
    } else if (has("svcBreak")) {  // (the game's own check failed: Pokémon Z-A, rarely, while it starts - a timing race)
        snprintf(head, sizeof head, "The game stopped at one of its own checks");
        snprintf(what, sizeof what, "Start it again: this happens now and then while the game starts, and the next "
                 "start is normally fine. If it keeps happening at the same place, it's a bug in %s. %s", app, report);
    } else if (in_gpu_driver(g_pc)) {
        snprintf(head, sizeof head, "Your graphics driver crashed");
        snprintf(what, sizeof what, "Update your graphics driver (NVIDIA, AMD or Intel) and try again. If it keeps "
                 "happening, please report it with the log file below.");
    } else if (has("VK_") || has("Vulkan")) {
        snprintf(head, sizeof head, "Graphics driver error");
        snprintf(what, sizeof what, "%s needs a graphics driver with Vulkan 1.3: install the latest driver for "
                 "your GPU (NVIDIA, AMD or Intel). If it keeps happening, please report it with the log file below.", app);
    } else if (has("out of memory") || has("cannot allocate") || has("alloc failed")) {
        snprintf(head, sizeof head, "Out of memory");
        snprintf(what, sizeof what, "Close other programs and try again.");
    } else if (g_pc && rt_host_fn_name(g_pc, NULL)) {
        snprintf(head, sizeof head, "The game crashed");
        snprintf(what, sizeof what, "This is a bug in %s, not a problem with your game files or PC. %s", app, report);
    } else {
        snprintf(head, sizeof head, "%s stopped because of an error", app);
        snprintf(what, sizeof what, "%s", report);
    }
    // (mods replace the game's files, and #85 / #98 stopped with them on and not without them)
    if (rt_mods_on && !rt_game_data_bad && (has("svcBreak") || (g_pc && rt_host_fn_name(g_pc, NULL)))) {
        size_t l = strlen(what);
        snprintf(what + l, sizeof what - l, " You have mods on: if this keeps happening, turn them off and see whether it stops.");
    }
    rt_launcher_log("[crash] %s (%s)\n", head, g_line);
    if (log_path && *log_path) snprintf(out, n, "%s\n\n%s\n\nLog file:\n%s", head, what, log_path);
    else snprintf(out, n, "%s\n\n%s", head, what);
}
