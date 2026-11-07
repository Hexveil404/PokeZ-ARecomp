// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3c0 / +0x3c4
// initial text offset / length (UTF-16) in the work buffer at +0x4d0. The answer is UTF-8 only when the game asks
// for it (KeyboardConfig isUseUtf8, +0x3bd: Link's Awakening), else UTF-16: Pokémon Z-A read our UTF-8 bytes two at
// a time, "William" showing as CJK characters ("Wi" = U+6957).
static size_t utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n + 1 < cap) {
        uint32_t cp = *p++;
        int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (more) cp &= 0x3F >> more;
        for (; more && (*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3F);
        if (more || (cp >= 0x80 && cp < 0xA0)) cp = '?';  // (truncated sequence, or a stray continuation byte)
        if (cp >= 0x10000) {
            if (n + 2 >= cap) break;
            cp -= 0x10000; out[n++] = (uint16_t)(0xD800 | cp >> 10); out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF)); }
        else out[n++] = (uint16_t)cp;
    }
    out[n] = 0;
    return n;
}

HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    char initial[96] = "";
    for (uint32_t i = 0; wb && i < n && i < sizeof initial - 1; i++) {
        uint16_t ch = *(const uint16_t *)(uintptr_t)(wb + off + 2 * i);
        initial[i] = ch < 0x80 ? (char)ch : '?';
    }
    char text[96];
    // (research aid) LARECOMP_SWKBD_ANSWER=<text>: answered without the overlay (hidden test runs: the link code)
    const char *answer = getenv("LARECOMP_SWKBD_ANSWER");
    int ok = answer ? (snprintf(text, sizeof text, "%s", answer), 1) : menu_keyboard_run(initial, max_len, text, sizeof text);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    bool utf8 = *(const uint8_t *)(uintptr_t)(arg + 0x3bd) != 0;
    LOG("[swkbd] name entry (max %d, initial \"%s\", %s): %s \"%s\"\n", max_len, initial, utf8 ? "UTF-8" : "UTF-16",
        ok ? "entered" : "cancelled", text);
    if (ptr && size) {
        memset((void *)(uintptr_t)ptr, 0, size);
        if (utf8) snprintf((char *)(uintptr_t)ptr, size, "%s", text);
        else utf8_to_utf16(text, (uint16_t *)(uintptr_t)ptr, size / 2);
    }
    c->x[0] = 0;
}
