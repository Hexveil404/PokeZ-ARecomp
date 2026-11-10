// nn::pl shared system fonts. The Switch's system fonts are not part of the game, so every SharedFontType the game
// asks for is filled from, in order of preference:
//  1. LARECOMP_FONT=<file> (.ttf / .otf / .ttc face 0 / .bfttf)
//  2. a dump from the user's own console in fonts/, under the console's own names (plain .ttf versions work too):
//     nintendo_udsg-r_std_003 (Standard, Morisawa UD Shin Go), nintendo_udsg-r_org_zh-cn_003 (Chinese Simplified),
//     nintendo_udsg-r_ext_zh-cn_003 (its Ext), nintendo_udjxh-db_zh-tw_003 (Chinese Traditional),
//     nintendo_udsg-r_ko_003 (Korean), nintendo_ext_003 (NintendoExt button glyphs)
//  3. a substitute of that same language where the OS has one (for Standard in a game played in Chinese or Korean:
//     that language's system font first, then the Japanese gothics - issue #389). A gothic like UD Shin Go is much
//     closer to the game's look than Arial. Without one (a Linux without a CJK font, say) the type serves Standard,
//     which is what a game asking for a language whose font is missing gets on the console too - but then that
//     language's characters are the substitute's, so Chinese / Korean text can show missing glyphs.
//     Standard's Japanese gothics are skipped when the game plays in Russian: Japanese fonts carry Cyrillic as
//     full-width characters (JIS X 0208), every letter a square apart (issue #32) - Arial / DejaVu Sans then.
//  4. Standard only, on Linux and any other platform without a known system font (not macOS / Windows), ahead of
//     the OS's own: BIZ UDPGothic shipped with the app (third_party/fonts/BIZUDPGothic, SIL OFL), Morisawa's free
//     font from the same universal-design gothic family as UD Shin Go. (Skipped for Russian like the other Japanese
//     gothics.)
//  5. Standard only: DejaVu Sans shipped with the app (third_party/fonts/DejaVu, free license), for systems with none
//     of the above (Bazzite / Fedora Atomic have no DejaVu - issue #37).
// On those same platforms a substitute (anything but the console's own files) gets the console font's OS/2 Windows
// metrics, which the game sizes text by (match_switch_metrics): without that every substitute draws about 1.5x too
// large. macOS and Windows keep Hiragino / Yu Gothic exactly as they are, at their current size (OS_FONTS_KNOWN).
// .bfttf is the console's obfuscated TTF: 8-byte header (magic, size) then every u32 XORed with a fixed key (bfttf_decode).
// .ttc collections get face 0 copied out as a standalone font (the game's font engine wants one sfnt).
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../hle.h"
#include "../settings.h"
#include <SDL3/SDL_gamepad.h>
#include "../paths.h"

// macOS and Windows have system fonts known to work well (Hiragino / Yu Gothic): the OS's fonts first, untouched.
// Everywhere else the bundled BIZ UDPGothic serves ahead of them and every substitute gets the console's metrics.
#if defined(__APPLE__) || defined(_WIN32)
#define OS_FONTS_KNOWN 1
#else
#define OS_FONTS_KNOWN 0
#endif

typedef struct { uint64_t addr, size; } Font;
// SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt
enum { F_STD, F_ZH_CN, F_ZH_CN_EXT, F_ZH_TW, F_KO, F_EXT, N_FONT_TYPES };
static Font fonts[N_FONT_TYPES];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len + 4);
    if (fread(p, 1, *len, f) != *len) { free(p); p = NULL; }
    fclose(f);
    return p;
}

// .bfttf -> TTF in place (returns the new length, 0 if it isn't one). Read as little-endian words: the first is
// 0x7F9A0218 ^ key, the second the font's size ^ key, then the font itself, each word XORed with the key. The words
// come in either byte order: swapped (issue #48's files; decoded without the swap they never matched), or in the
// font's own order (the font archives of a firmware dump, e.g. from an emulator's installed firmware). The magic word
// says which.
static size_t bfttf_decode(uint8_t *d, size_t len) {
    static const uint32_t KEY = 0x49621806, MAGIC = 0x7F9A0218;
    if (len < 12) return 0;
    uint32_t w0, w1;
    memcpy(&w0, d, 4);
    memcpy(&w1, d + 4, 4);
    bool swap;
    if ((w0 ^ KEY) == MAGIC) swap = true;
    else if ((w0 ^ __builtin_bswap32(KEY)) == __builtin_bswap32(MAGIC)) swap = false;
    else return 0;
    uint32_t key = swap ? KEY : __builtin_bswap32(KEY);
    uint32_t size = w1 ^ key;
    if (!swap) size = __builtin_bswap32(size);
    size_t words = ((size_t)size + 3) & ~(size_t)3;  // (the last word is padded)
    if (size < 4 || words > len - 8) return 0;
    for (size_t i = 0; i < words; i += 4) {
        uint32_t w;
        memcpy(&w, d + 8 + i, 4);
        w ^= key;
        if (swap) put32(d + i, w);  // (big-endian: the font's own byte order)
        else memcpy(d + i, &w, 4);
    }
    uint32_t ver = be32(d);
    return ver == 0x00010000u || ver == 0x4F54544Fu || ver == 0x74727565u ? size : 0;  // TrueType / 'OTTO' / 'true'
}

// .ttc -> face 0 as its own sfnt (table data copied, offsets rewritten)
static uint8_t *ttc_face0(const uint8_t *f, size_t len, size_t *out_len) {
    if (len < 16 || memcmp(f, "ttcf", 4)) return NULL;
    uint32_t o = be32(f + 12);
    if (o + 12 > len) return NULL;
    uint32_t nt = (uint32_t)f[o + 4] << 8 | f[o + 5];
    size_t total = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) total += (be32(f + o + 12 + 16 * i + 12) + 3) & ~3u;
    uint8_t *out = calloc(1, total);
    memcpy(out, f + o, 12);
    size_t pos = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = f + o + 12 + 16 * i;
        uint32_t off = be32(e + 8), n = be32(e + 12);
        if ((size_t)off + n > len) { free(out); return NULL; }
        memcpy(out + 12 + 16 * i, e, 8);  // tag, checksum
        put32(out + 12 + 16 * i + 8, (uint32_t)pos);
        put32(out + 12 + 16 * i + 12, n);
        memcpy(out + pos, f + off, n);
        pos += (n + 3) & ~3u;
    }
    *out_len = total;
    return out;
}

// Does the font map this character to a glyph? (its cmap: format 4 or 12 subtables, Unicode encodings)
static bool font_has(const uint8_t *f, size_t len, uint32_t cp) {
    if (len < 12) return false;
    uint32_t nt = (uint32_t)f[4] << 8 | f[5], cmap = 0, cmap_len = 0;
    for (uint32_t i = 0; i < nt && 12 + 16 * (i + 1) <= len; i++)
        if (!memcmp(f + 12 + 16 * i, "cmap", 4)) { cmap = be32(f + 12 + 16 * i + 8); cmap_len = be32(f + 12 + 16 * i + 12); }
    if (!cmap || (size_t)cmap + cmap_len > len || cmap_len < 4) return false;
    const uint8_t *c = f + cmap, *end = f + cmap + cmap_len;
    uint32_t ns = (uint32_t)c[2] << 8 | c[3];
    for (uint32_t i = 0; i < ns && c + 4 + 8 * (i + 1) <= end; i++) {
        uint32_t plat = (uint32_t)c[4 + 8 * i] << 8 | c[5 + 8 * i], enc = (uint32_t)c[6 + 8 * i] << 8 | c[7 + 8 * i];
        if (!(plat == 0 || (plat == 3 && (enc == 1 || enc == 10)))) continue;  // (Unicode subtables only)
        const uint8_t *t = c + be32(c + 8 + 8 * i);
        if (t + 4 > end) continue;
        uint32_t fmt = (uint32_t)t[0] << 8 | t[1];
        if (fmt == 4 && cp <= 0xFFFF && t + 14 <= end) {
            uint32_t segs = ((uint32_t)t[6] << 8 | t[7]) / 2;
            const uint8_t *ends = t + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
            if (ranges + 2 * segs > end) continue;
            for (uint32_t k = 0; k < segs; k++) {
                uint32_t e = (uint32_t)ends[2 * k] << 8 | ends[2 * k + 1], st = (uint32_t)starts[2 * k] << 8 | starts[2 * k + 1];
                if (cp > e) continue;
                if (cp < st) break;
                uint32_t ro = (uint32_t)ranges[2 * k] << 8 | ranges[2 * k + 1], d = (uint32_t)deltas[2 * k] << 8 | deltas[2 * k + 1];
                if (!ro) return ((cp + d) & 0xFFFF) != 0;
                const uint8_t *g = ranges + 2 * k + ro + 2 * (cp - st);
                return g + 2 <= end && ((((uint32_t)g[0] << 8 | g[1]) + (g[0] | g[1] ? d : 0)) & 0xFFFF) != 0;
            }
        } else if (fmt == 12 && t + 16 <= end) {
            uint32_t ng = be32(t + 12);
            for (uint32_t k = 0; k < ng && t + 16 + 12 * (k + 1) <= end; k++) {
                const uint8_t *gr = t + 16 + 12 * k;
                if (cp >= be32(gr) && cp <= be32(gr + 4)) return be32(gr + 8) + (cp - be32(gr)) != 0;
            }
        }
    }
    return false;
}

// The characters the game counts on in every font it's given: a space and "□" (U+25A1), its box for a missing
// character - it looks the box's glyph up at startup without checking for none, and crashed on a PC whose Yu Gothic
// lacked it (issue #45). A substitute without them is passed over for the next one.
static bool font_usable(const uint8_t *f, size_t len) { return font_has(f, len, 0x20) && font_has(f, len, 0x25A1); }

// The game's font engine sizes a font by its OS/2 usWinAscent + usWinDescent: 1.5 em for the console's UD Shin Go
// (1158 + 378 of 1024 units) against the usual 1.0 em, so a substitute draws about 1.5x too large. Give it the console
// font's values; its text then matches the console's size (checked against the real font on the file select screen).
static void match_switch_metrics(uint8_t *d, size_t len) {
    if (len < 12) return;
    unsigned ntab = (unsigned)d[4] << 8 | d[5];
    if (12 + 16 * (size_t)ntab > len) return;
    const uint8_t *head = NULL;
    uint8_t *os2 = NULL;
    for (unsigned i = 0; i < ntab; i++) {
        const uint8_t *e = d + 12 + 16 * i;
        uint32_t off = be32(e + 8), tlen = be32(e + 12);
        if (off > len || tlen > len - off) continue;
        if (!memcmp(e, "head", 4) && tlen >= 20) head = d + off;
        if (!memcmp(e, "OS/2", 4) && tlen >= 78) os2 = d + off;
    }
    if (!head || !os2) return;
    unsigned upem = (unsigned)head[18] << 8 | head[19];
    if (!upem) return;
    unsigned asc = (1158 * upem + 512) / 1024, desc = (378 * upem + 512) / 1024;
    os2[74] = (uint8_t)(asc >> 8), os2[75] = (uint8_t)asc;    // usWinAscent
    os2[76] = (uint8_t)(desc >> 8), os2[77] = (uint8_t)desc;  // usWinDescent
}

static int load_one_checked(const char *path, Font *ft, bool check);
static int load_one(const char *path, Font *ft) { return load_one_checked(path, ft, false); }
static int load_one_checked(const char *path, Font *ft, bool check) {
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d) return 0;
    size_t n = strlen(path);
    bool console_font = n > 6 && !strcasecmp(path + n - 6, ".bfttf");
    if (console_font) {
        len = bfttf_decode(d, len);
        if (!len) { LOG("[pl] %s: not a valid .bfttf\n", path); free(d); return 0; }
    } else if (len >= 4 && !memcmp(d, "ttcf", 4)) {
        size_t l2;
        uint8_t *f0 = ttc_face0(d, len, &l2);
        free(d);
        if (!f0) { LOG("[pl] %s: bad font collection\n", path); return 0; }
        d = f0;
        len = l2;
    }
    if (!console_font && !OS_FONTS_KNOWN) match_switch_metrics(d, len);  // (a .ttf of the console's own: already so)
    if (check && !font_usable(d, len)) {
        LOG("[pl] %s: no glyph for a space or \"\u25A1\" (U+25A1) - skipped\n", path);
        free(d);
        return 0;
    }
    void *p = hle_alloc(len + 16);
    memcpy(p, d, len);
    free(d);
    ft->addr = (uint64_t)p;
    ft->size = len;
    LOG("[pl] system font %s (%zu bytes)\n", path, len);
    return 1;
}

static int g_fonts_loaded;
RT_ON_RESET(pl_reset) { g_fonts_loaded = 0; memset(fonts, 0, sizeof fonts); }  // (they were in the HLE heap)

const char *pl_prompt_set(void);  // (below: the button prompt set)

static void load_fonts(void) {
    if (g_fonts_loaded) return;
    g_fonts_loaded = 1;
    const char *env = getenv("LARECOMP_FONT");
    // per type: the console's own file(s), then a substitute of that same language
    static const char *const std[] = {"fonts/nintendo_udsg-r_std_003.bfttf", "fonts/nintendo_udsg-r_std_003.ttf",
                                     "fonts/nintendo_udsgr_std_003.ttf", "fonts/nintendo_udsgr_std_003.bfttf",
                                     "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", "C:/Windows/Fonts/YuGothR.ttc",
                                     "C:/Windows/Fonts/YuGothM.ttc",
                                     "/System/Library/Fonts/Supplemental/Arial.ttf", "C:/Windows/Fonts/arial.ttf",
                                     // Linux: DejaVu Sans (Debian / Ubuntu, Arch, Fedora). Not Noto Sans CJK: CFF
                                     // outlines, which the game's font engine crashes on (it wants TrueType)
                                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                     "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                     "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", NULL};
    static const char *const zh_cn[] = {"fonts/nintendo_udsg-r_org_zh-cn_003.bfttf",
                                        "fonts/nintendo_udsg-r_org_zh-cn_003.ttf",
                                        "/System/Library/Fonts/Hiragino Sans GB.ttc",
                                        "/System/Library/Fonts/Supplemental/Hiragino Sans GB.ttc",
                                        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                                        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                                        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", NULL};
    static const char *const zh_cn_ext[] = {"fonts/nintendo_udsg-r_ext_zh-cn_003.bfttf",
                                            "fonts/nintendo_udsg-r_ext_zh-cn_003.ttf", NULL};
    static const char *const zh_tw[] = {"fonts/nintendo_udjxh-db_zh-tw_003.bfttf",
                                        "fonts/nintendo_udjxh-db_zh-tw_003.ttf", "C:/Windows/Fonts/msjh.ttc",
                                        // (PingFang carries both, but a collection copies only face 0 out: Simplified)
                                        "/System/Library/Fonts/PingFang.ttc",
                                        "/System/Library/Fonts/Supplemental/Songti.ttc",
                                        "/usr/share/fonts/truetype/arphic/uming.ttc", NULL};
    static const char *const ko[] = {"fonts/nintendo_udsg-r_ko_003.bfttf", "fonts/nintendo_udsg-r_ko_003.ttf",
                                     "/System/Library/Fonts/AppleSDGothicNeo.ttc", "C:/Windows/Fonts/malgun.ttf",
                                     "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
                                     "/usr/share/fonts/nanum/NanumGothic.ttf", NULL};
    static const char *const ext[] = {"fonts/nintendo_ext_003.bfttf", "fonts/nintendo_ext_003.ttf", NULL};
    static const char *const *const chains[N_FONT_TYPES] = {std, zh_cn, zh_cn_ext, zh_tw, ko, ext};
    // Standard's substitutes for a game in Chinese / Korean, ahead of the Japanese gothics: that language's own
    // system font. Standard is also what ChineseSimpleExt and NintendoExt fall back to, and the game draws some of
    // its text with them (the mode select's labels), so in these languages a Japanese font there left characters
    // blank - an older Windows 10's Yu Gothic lacks some Simplified Chinese ones (issue #389). The Japanese gothics
    // stay after them for a system without one. (Not macOS for Traditional Chinese: its fonts' first face is
    // Simplified, and Hiragino carries the traditional forms.)
    static const char *const std_zh_cn[] = {"/System/Library/Fonts/Hiragino Sans GB.ttc",
                                            "/System/Library/Fonts/Supplemental/Hiragino Sans GB.ttc",
                                            "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                                            "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                                            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", NULL};
    static const char *const std_zh_tw[] = {"C:/Windows/Fonts/msjh.ttc", "/usr/share/fonts/truetype/arphic/uming.ttc",
                                            NULL};
    static const char *const std_ko[] = {"/System/Library/Fonts/AppleSDGothicNeo.ttc", "C:/Windows/Fonts/malgun.ttf",
                                         "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
                                         "/usr/share/fonts/nanum/NanumGothic.ttf", NULL};
    extern const char *settings_language(void);
    const char *lang = settings_language();
    bool cyrillic = !strcmp(lang, "ru");
    const char *const *std_native = !strcmp(lang, "zh-Hans") ? std_zh_cn
                                    : !strcmp(lang, "zh-Hant") ? std_zh_tw
                                    : !strcmp(lang, "ko")      ? std_ko
                                                               : NULL;
    // a forced font: it is what the player asked for, in any language. Said so when it can't be used (it was
    // passed over silently before).
    if (env && !*env) env = NULL;
    if (env) {
        FILE *f = fopen(env, "rb");
        if (!f) {
            LOG("[pl] LARECOMP_FONT %s: can't open, using the normal fonts\n", env);
            env = NULL;
        } else
            fclose(f);
    }
    char bundled[1100];
    // Xbox / PlayStation / keyboard prompts (setting prompts, or auto by the controller): the button glyphs relabelled
    // (tools/za/xbox_font.py, keyboard_font.py) - Nintendo Ext from that set's file when there is one
    const char *set = pl_prompt_set();
    LOG("[pl] button prompts: %s\n", set ? set : "Nintendo");
    if (set) {
        char path[64];
        snprintf(path, sizeof path, "fonts/nintendo_ext_003_%s.ttf", set);
        load_one(path, &fonts[F_EXT]);
    }
    for (int t = 0; t < N_FONT_TYPES; t++) {
        if (env && !load_one(env, &fonts[t])) {
            LOG("[pl] LARECOMP_FONT %s: not a font the game can use, using the normal fonts\n", env);
            env = NULL;
        }
        for (size_t i = 0; !fonts[t].addr && chains[t][i]; i++) {
            const char *f = chains[t][i];
            bool own = strncmp(f, "fonts/", 6) == 0;  // (the console's own files: as they are)
            // Standard in Chinese / Korean, once the console's own files are passed: that language's system font
            if (t == F_STD && std_native && !own && i && strncmp(chains[t][i - 1], "fonts/", 6) == 0) {
                for (size_t k = 0; !fonts[t].addr && std_native[k]; k++)
                    load_one_checked(std_native[k], &fonts[t], true);
                if (fonts[t].addr) break;
            }
            // Standard, before the OS's fonts where none is known: the bundled BIZ UDPGothic (a Japanese gothic
            // too: not for Russian)
            if (!OS_FONTS_KNOWN && t == F_STD && !own && !cyrillic && i && strncmp(chains[t][i - 1], "fonts/", 6) == 0 &&
                rt_resource("BIZUDPGothic-Regular.ttf", "third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf", bundled, sizeof bundled) &&
                load_one_checked(bundled, &fonts[t], true))
                break;
            if (t == F_STD && cyrillic && (strstr(f, "/System/Library/Fonts/ヒラギノ") || strstr(f, "YuGoth"))) continue;
            load_one_checked(f, &fonts[t], !own);
        }
    }
    if (!fonts[F_STD].addr && rt_resource("DejaVuSans.ttf", "third_party/fonts/DejaVu/DejaVuSans.ttf", bundled, sizeof bundled))
        load_one_checked(bundled, &fonts[F_STD], true);
    if (!fonts[F_STD].addr)  // (only a broken install: the app ships DejaVu Sans)
        FATAL("no font for the game's text: import the Switch fonts in Settings (Game font), or reinstall %s\n",
              g_game_is_la ? "LAS Recomp" : "ZA Recomp");
    static const char *const type_names[N_FONT_TYPES] = {"Standard", "ChineseSimple", "ChineseSimpleExt",
                                                         "ChineseTraditional", "Korean", "NintendoExt"};
    for (int t = 1; t < N_FONT_TYPES; t++)  // (font_for: a type without its own font serves Standard's)
        if (!fonts[t].addr) LOG("[pl] %s: Standard's font\n", type_names[t]);
}

// The prompts' style, chosen once at boot: "keyboard", "xbox", "playstation", "nintendo" (a controller SDL names no
// family for) or NULL (the game's own: no controller, prompts = nintendo). Each set names, for every input, the key
// or button it is bound to now - in that pad's own words (A B X Y LB RB LT RT Menu View; cross circle square triangle
// L1 R1 L2 R2 Options Share), made by tools/za/prompt_textures.py for the current bindings. Names both the button glyph
// font (fonts/nintendo_ext_003_<set>.ttf: tools/za/keyboard_font.py, xbox_font.py) and the game's own UI icons
// relabelled (textures/prompts/<set>: tools/za/prompt_textures.py, runtime/nvn/vulkan/textures.c).
const char *pl_prompt_set(void) {
    static int done;
    static const char *set;
    if (done) return set;
    done = 1;
    // (bind_label_family, not input_pad_family: runtime/services/fs.c asks this before runtime/hle/input.c has opened
    // the controller - it asked SDL, which knows the pad's type already; "auto" used to come out as Nintendo's)
    extern int bind_label_family(void);
    extern int16_t g_bind_pad[];
    bool pad = SDL_HasGamepad();
    int fam = pad ? bind_label_family() : 0;
    extern int settings_on_steam_deck(void);
    bool deck = g_settings.prompts == PROMPTS_STEAMDECK || (g_settings.prompts == PROMPTS_AUTO && fam != 2 && settings_on_steam_deck());  // (a Deck is a pad, also when SDL has not opened its controller yet at boot)
    bool xbox = !deck && (g_settings.prompts == PROMPTS_XBOX || (g_settings.prompts == PROMPTS_AUTO && fam == 1));
    bool ps = g_settings.prompts == PROMPTS_PLAYSTATION || (g_settings.prompts == PROMPTS_AUTO && fam == 2);
    bool keys = g_settings.prompts == PROMPTS_KEYBOARD || (g_settings.prompts == PROMPTS_AUTO && !pad && !deck);
    (void)g_bind_pad;  // (each set is made for the bindings: runtime/ui/launcher.mm remakes them when those change)
    set = keys ? "keyboard" : deck ? "steamdeck" : xbox ? "xbox" : ps ? "playstation" : pad ? "nintendo" : NULL;
    // (one line: which set, from what - #229's "PlayStation icons don't turn on" had no way to tell)
    LOG("[pl] button prompts: %s (setting %d, controller %s, family %d%s)\n", set ? set : "the game's own",
        g_settings.prompts, pad ? "connected" : "none", fam, settings_on_steam_deck() ? ", Steam Deck" : "");
    return set;
}

// SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt
static const Font *font_for(uint64_t type) {
    load_fonts();
    // a type this OS gave us no font for (a Linux without a CJK font, say): Standard, as a missing dump does too
    if (type >= N_FONT_TYPES || !fonts[type].addr) return &fonts[F_STD];
    return &fonts[type];
}

HLE_FUNC("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_get_load_state) { c->x[0] = 1; }
HLE_FUNC("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_get_address) { c->x[0] = font_for(c->x[0])->addr; }
HLE_FUNC("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_get_size) { c->x[0] = font_for(c->x[0])->size; }
