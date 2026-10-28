// Start screen, shown before the game boots (user 2026-10-08, styled after Dusklight's launcher): PLAY (with
// ‹ Vanilla / seed › picker), SETTINGS, QUIT, game-files status at the lower left. Nothing of the game ships with
// LAS Recomp: on first launch the user picks the folder with their own game files (v1.0.0), we check the main
// module's build id, copy the files and build the game's data image; PLAY stays disabled until that worked. The background is a frame of the game's own title movie (from the user's files) once extracted.
//
// Skipped with --play, LARECOMP_NO_LAUNCHER=1, or any scripted test run (LARECOMP_INPUT): tests boot straight in.
// The in-game menu's "Title screen" comes back here without leaving the process (runtime/main.c reboots the game).
#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <atomic>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner)
#include "option_rows.h"
#include "osk.h"
#include "pad_nav.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
#endif
#include "imgui_impl_sdl3.h"

extern "C" {
#include "platform.h"
#include "paths.h"
#include "gamebuild.h"
#include "gamelib.h"
#include "romfs_build.h"
#include "settings.h"
#include "input_bind.h"
bool controls_ui_took_back(void);  // ui/controls_ui.mm: B went back a step on Configure inputs
void input_mouse_release(SDL_Window *w);   // runtime/hle/input.c
void ui_window_init(void);
SDL_Window *ui_window(void);
#ifdef __APPLE__
void *ui_metal_layer(void);
void *ui_metal_device(void);
#else
// runtime/nvn/vulkan: window frames and ImGui plumbing
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
#endif
}
#ifdef __APPLE__
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// (test) BGRA texture -> PNG (the game renderer's saver isn't up yet before the game boots)
static void save_png(id<MTLTexture> tex, const char *path) {
    size_t w = tex.width, h = tex.height;
    std::vector<uint8_t> px(w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {  // (NULL when the file can't be created, e.g. its folder isn't there: CFRelease(NULL) traps)
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    } else fprintf(stderr, "[launcher] can't write %s\n", path);
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}
#endif

static const uint8_t BUILD_ID[8] = {0xae, 0x16, 0xf7, 0x1e, 0x00, 0x2a, 0xf8, 0xcb};  // Link's Awakening v1.0.0 main
// Pokémon Legends Z-A's main builds (tools/patches.py ZA_BUILDS): set up from the player's .xci / .nsp and keys by the
// bundled tools/zaimport (no extracted folder), no romfs/ copy - the game reads romfs.bin (+ aoc/ for the DLC)
static const struct { uint8_t id[8]; const char *version; } ZA_BUILDS[] = {
    {{0x72, 0x22, 0xe1, 0x3e, 0xcf, 0x6a, 0xdb, 0x32}, "1.0.0"},
    {{0x8a, 0x1c, 0x86, 0xc4, 0x37, 0x39, 0x4b, 0x69}, "2.0.0"},
    {{0xb1, 0xf1, 0x2f, 0xd9, 0x19, 0xea, 0xe8, 0x6a}, "2.0.2"},
};
// the version of a main module's build id: NULL = not one we know
static const char *build_version(const uint8_t id[8]) {
    if (!memcmp(id, BUILD_ID, 8)) return "1.0.0";
    for (auto &b : ZA_BUILDS)
        if (!memcmp(id, b.id, 8)) return b.version;
    return NULL;
}
static bool build_is_za(const uint8_t id[8]) { return build_version(id) && memcmp(id, BUILD_ID, 8); }
// The release tag this build is (also names the release files; Makefile / tools/bundle_macos.sh / make_dmg.sh read
// it): ZA Recomp's v<major>.<minor>.<patch> (Hexveil404/PokeZ-ARecomp releases).
#define LARECOMP_VERSION "v0.1.10"
extern "C" const char *rt_version(void) { return LARECOMP_VERSION; }  // (runtime/main.c: the log's first line)
extern "C" const char *larecomp_version(void) { return LARECOMP_VERSION; }  // (the shader caches' tag)

// ------------------------------------------------------------------ language
// The start screen speaks English or Portuguese: settings ui_lang (the PT | EN switch in its top bar), auto = the
// system's language (Portuguese when it is, else English). TR("English", "Português") picks the line. Logs stay English.
// The F1 menu follows the same setting (UI_TR, runtime/settings.h).
static bool g_pt;
#define TR(en, pt) (g_pt ? (pt) : (en))
static void lang_update(void) { g_pt = settings_ui_portuguese(); }  // (runtime/settings.c: the F1 menu speaks it too)

// ------------------------------------------------------------------ update check
// The start screen asks GitHub for the project's releases once per launch (a background curl, which macOS and
// Windows 10+ ship) and offers the best one if it outranks this build: any v<x.y.z> above every beta, betas by
// number; other tags are ignored. Offline / no curl / anything unexpected: nothing is shown.
#define RELEASES_URL "https://api.github.com/repos/Hexveil404/PokeZ-ARecomp/releases?per_page=100"
#define RELEASE_PAGE "https://github.com/Hexveil404/PokeZ-ARecomp/releases/tag/"

static long long tag_rank(const std::string &t) {
    int n, a, b, c;
    char end;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1 && n > 0) return n;
    if (sscanf(t.c_str(), "v%d.%d.%d%c", &a, &b, &c, &end) == 3 && a >= 0 && b >= 0 && c >= 0 && b < 4096 && c < 4096)
        return (1LL << 48) + ((long long)a << 24) + ((long long)b << 12) + c;
    return -1;
}

// "Beta 2" / "1.0.0" (with_word: "Version 1.0.0")
static std::string tag_label(const std::string &t, bool with_word) {
    int n;
    char end;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1) return "Beta " + std::to_string(n);
    std::string v = t.size() > 1 && t[0] == 'v' ? t.substr(1) : t;
    return with_word ? "Version " + v : v;
}

static std::string g_update_tag;  // a newer release's tag (g_update_lock)
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;

static void *update_thread(void *) {
    std::string body;
    if (const char *t = getenv("LARECOMP_UPDATE_TAGS")) {  // (test) "beta_2,v1.0.0": these releases, no network
        for (const char *p = t; *p;) {
            const char *c = strchr(p, ',');
            body += "\"tag_name\": \"" + std::string(p, c ? c - p : strlen(p)) + "\"\n";
            p = c ? c + 1 : p + strlen(p);
        }
    } else {
#ifdef _WIN32
        const char *sr = getenv("SystemRoot");
        std::string curl = std::string(sr ? sr : "C:\\Windows") + "\\System32\\curl.exe";
#else
        std::string curl = "/usr/bin/curl";
#endif
        std::string out = std::string(rt_data_root()) + "/cache/releases.json";
        mkdir((std::string(rt_data_root()) + "/cache").c_str(), 0755);
        const char *av[] = {curl.c_str(), "-fsSL", "--max-time", "15", "-H", "Accept: application/vnd.github+json",
                            "-A", "LASRecomp/" LARECOMP_VERSION, RELEASES_URL, NULL};
        PlatProc p = plat_spawn(av, out.c_str(), false);
        if (!p || plat_proc_wait(p, true) != 0) { remove(out.c_str()); return NULL; }
        if (FILE *f = fopen(out.c_str(), "rb")) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && body.size() < (8u << 20)) body.append(buf, n);
            fclose(f);
        }
        remove(out.c_str());
    }
    std::string best;
    long long best_rank = tag_rank(LARECOMP_VERSION);
    for (size_t at = 0; (at = body.find("\"tag_name\"", at)) != std::string::npos;) {
        size_t q1 = body.find('"', body.find(':', at) + 1), q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
        if (tag_rank(tag) > best_rank) best_rank = tag_rank(tag), best = tag;
        at = q2;
    }
    if (!best.empty()) fprintf(stderr, "[launcher] update available: %s (this build %s)\n", best.c_str(), LARECOMP_VERSION);
    pthread_mutex_lock(&g_update_lock);
    g_update_tag = best;
    pthread_mutex_unlock(&g_update_lock);
    return NULL;
}

// ------------------------------------------------------------------ game files
static std::string g_root;  // extracted game: <root>/exefs, <root>/romfs

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// The main module's build id (false: no such file)
static bool read_build_id(const std::string &main_path, uint8_t id[8]) {
    FILE *f = fopen(main_path.c_str(), "rb");
    if (!f) return false;
    memset(id, 0, 8);
    fseek(f, 0x40, SEEK_SET);
    size_t n = fread(id, 1, 8, f);
    fclose(f);
    return n == 8;
}

// The copied game files alone ("" = there), without the game code built from them
static std::string check_copied_files(const std::string &root) {
    uint8_t id[8];
    if (!read_build_id(root + "/exefs/main", id)) return "No game files";
    if (!build_version(id)) return "Wrong game version";
    // (the game data: romfs.bin - Pokémon Z-A's packages, read by zaimport - or the copied romfs/ folder, which
    // runtime/services/fs.c serves itself)
    if (!file_exists(root + "/romfs.bin") && !file_exists(root + (build_is_za(id) ? "/romfs" : "/romfs/region_common")))
        return "Game data missing";
    return "";
}

// "Nintendo Switch · v2.0.0" under the status line: the set-up files' version
static std::string game_version(const std::string &root) {
    uint8_t id[8];
    const char *v = read_build_id(root + "/exefs/main", id) ? build_version(id) : NULL;
    return v ? v : "?";
}

// "" = ready to play, else what is wrong
static std::string check_game_files(const std::string &root) {
    std::string why = check_copied_files(root);
    if (why.empty() && !gamelib_present(root.c_str())) return "Game code not built yet";
    return why;
}

// ------------------------------------------------------------------ setup (background thread)
// The player brings their own game files: a folder holding exefs/ and romfs/ (or Pokémon Z-A's .xci / .nsp packages,
// read by zaimport into exefs/ and romfs.bin). They're copied into the data root (APFS clones / hard links where the
// file system allows); the game reads romfs/ in place (runtime/services/fs.c).
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_src;           // the picked folder
static std::string g_job;           // current setup step / result for the status line
static std::string g_job_hint;      // a failure: what to do about it (shown under it)
static bool g_busy, g_job_failed;
static bool g_confirm_delete;       // game-files page: "Delete the game files?" shown

static void set_job(const std::string &s, bool failed = false, const std::string &hint = "") {
    pthread_mutex_lock(&g_lock);
    g_job = s;
    g_job_failed = failed;
    g_job_hint = hint;
    pthread_mutex_unlock(&g_lock);
}

// Setup failures: a short headline and what to do next (also LARECOMP_LAUNCHER_SHOT=<f>:fail<1-8>, screenshots). The
// job line keeps the English (the log, tests); the start screen shows fail_head / fail_hint in its language.
enum { FAIL_NOT_FOUND = 1, FAIL_UNUSABLE, FAIL_COPY, FAIL_VERSION, FAIL_MISSING, FAIL_BUILD, FAIL_KEYS, FAIL_IMPORT };
static int g_fail;                 // the last failure's kind (0: none)
static std::string g_fail_detail;  // (from zaimport or the check: technical, as it came)
static const char *fail_head(int k, bool pt) {
    static const char *const en[] = {"", "No game files found in that folder", "Those files can't be used",
                                     "Setup didn't finish", "Wrong version of the game", "Game files incomplete",
                                     "The game code didn't build", "prod.keys not found",
                                     "The game couldn't be read from your files"};
    static const char *const ptb[] = {"", "Nenhum arquivo do jogo nessa pasta", "Esses arquivos não servem",
                                      "A preparação não terminou", "Versão errada do jogo", "Arquivos do jogo incompletos",
                                      "O código do jogo não compilou", "prod.keys não encontrado",
                                      "Não deu para ler o jogo dos seus arquivos"};
    return k >= 1 && k <= 8 ? (pt ? ptb : en)[k] : "";
}
static const char *fail_hint(int k, bool pt) {
    static const char *const en[] = {
        "",
        "Choose the folder with your Pok\xc3\xa9mon Legends Z-A game extracted (exefs and romfs folders), or with your "
        "dump (.xci / .nsp) and your prod.keys.",
        "Pok\xc3\xa9mon Legends Z-A's base game isn't among those files. Choose the folder with your own dump.",
        "Make sure there's about 12 GB free and try again.",
        "ZA Recomp runs versions 1.0.0, 2.0.0 and 2.0.2 of the game (base game, or base game + the 2.0.0 or 2.0.2 "
        "update).",
        "Some files are missing. Choose the folder again; if it happens again, dump the game again.",
        "Make sure there's about 4 GB free and try again. If it keeps happening, report it on GitHub.",
        "Put the prod.keys (and title.keys) dumped from your own Switch in the folder with the game files. Any file "
        "name works (prod.keys.txt, keys.txt...): it is recognized by what is inside it.",
        "The full reason is in lasrecomp.log next to the program."};
    static const char *const ptb[] = {
        "",
        "Escolha a pasta com o seu Pokémon Legends Z-A extraído (pastas exefs e romfs), ou com o seu dump (.xci / .nsp) "
        "e o seu prod.keys.",
        "O jogo base de Pokémon Legends Z-A não está entre esses arquivos. Escolha a pasta com o seu próprio dump.",
        "Veja se há uns 12 GB livres e tente de novo.",
        "O ZA Recomp roda as versões 1.0.0, 2.0.0 e 2.0.2 do jogo (jogo base, ou jogo base + atualização 2.0.0 ou 2.0.2).",
        "Faltam arquivos. Escolha a pasta de novo; se acontecer outra vez, faça o dump do jogo de novo.",
        "Veja se há uns 4 GB livres e tente de novo. Se continuar, avise no GitHub.",
        "Coloque o prod.keys (e o title.keys) do seu próprio Switch na pasta dos arquivos do jogo. Qualquer nome serve "
        "(prod.keys.txt, keys.txt...): ele é reconhecido pelo conteúdo.",
        "O motivo completo está no lasrecomp.log, ao lado do programa."};
    return k >= 1 && k <= 8 ? (pt ? ptb : en)[k] : "";
}
static void set_failure(int k, const std::string &detail = "") {
    if (k < 1 || k > 8) return;
    g_fail = k, g_fail_detail = detail;
    set_job(fail_head(k, false), true, detail.empty() ? fail_hint(k, false) : detail + "\n" + fail_hint(k, false));
}

static bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// exefs: the folder with main and main.npdm; romfs: the folder with region_common (Link's Awakening's data root) or
// arc/data.trpfs (Pokémon Z-A's: its packs). Found in the picked folder or a few levels below it (some folder layouts
// nest them).
static void find_game(const std::string &dir, int depth, std::string &exefs, std::string &romfs, bool &unusable) {
    if (depth > 4 || (!exefs.empty() && !romfs.empty())) return;
    if (exefs.empty() && file_exists(dir + "/main") && file_exists(dir + "/main.npdm")) exefs = dir;
    if (romfs.empty() && (is_dir(dir + "/region_common") || file_exists(dir + "/arc/data.trpfs"))) romfs = dir;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] == '.') continue;
        std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : "";
        for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
        if (ext == ".xci" || ext == ".nsp" || ext == ".nca" || ext == ".nsz" || ext == ".xcz") unusable = true;
        if (n != "region_common" && n != "arc" && is_dir(dir + "/" + n)) sub.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(sub.begin(), sub.end());
    for (auto &s : sub) find_game(s, depth + 1, exefs, romfs, unusable);
}

static bool build_id_ok(const std::string &main_path) {
    uint8_t id[8];
    return read_build_id(main_path, id) && build_version(id);
}

// ------------------------------------------------------------------ Z-A: the player's packages -> game files
// Game packages (.xci / .nsp) in the picked folder or a few levels below it
// (compressed: .nsz / .xcz seen - NSZ's compressed packages, not readable here)
static void find_packages(const std::string &dir, int depth, std::vector<std::string> &out, int &compressed) {
    if (depth > 3) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] == '.') continue;
        std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : "";
        for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
        if (is_dir(dir + "/" + n)) sub.push_back(dir + "/" + n);
        else if (ext == ".xci" || ext == ".nsp") out.push_back(dir + "/" + n);
        else if (ext == ".nsz" || ext == ".xcz") compressed++;
    }
    closedir(d);
    std::sort(sub.begin(), sub.end());
    for (auto &s : sub) find_packages(s, depth + 1, out, compressed);
}

// The player's keys files, told apart by what is in them, whatever they are named (prod.keys, prod.keys.txt - Windows'
// hidden extensions -, "prod (1).keys", keys.txt...): a file named *.keys / *.key / *.txt (under 1 MB) is
// KEYS_PROD with a header_key line, KEYS_TITLE when its lines are "<rights id> = <title key>" (32 hex digits each).
enum { KEYS_NONE, KEYS_PROD, KEYS_TITLE };
static bool keys_file_name(std::string n) {
    for (auto &ch : n) ch = (char)tolower((unsigned char)ch);
    auto ends = [&](const char *x) { size_t k = strlen(x); return n.size() > k && !n.compare(n.size() - k, k, x); };
    return ends(".keys") || ends(".key") || ends(".txt");
}
static bool hex32(const std::string &s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!isxdigit((unsigned char)c)) return false;
    return true;
}
static int keys_kind(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (1 << 20)) return KEYS_NONE;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return KEYS_NONE;
    std::string s((size_t)st.st_size, 0);
    s.resize(fread(&s[0], 1, s.size(), f));
    fclose(f);
    for (auto &ch : s) ch = (char)tolower((unsigned char)ch);
    if (s.find("header_key") != std::string::npos) return KEYS_PROD;
    int title = 0;
    size_t at = 0;
    while (at < s.size()) {
        size_t end = s.find('\n', at);
        if (end == std::string::npos) end = s.size();
        std::string line;
        for (size_t i = at; i < end; i++)
            if (!isspace((unsigned char)s[i])) line += s[i];
        size_t eq = line.find('=');
        if (eq != std::string::npos && hex32(line.substr(0, eq)) && hex32(line.substr(eq + 1))) title++;
        at = end + 1;
    }
    return title ? KEYS_TITLE : KEYS_NONE;
}

// A keys file of that kind in dir (prod.keys / title.keys first), "" if none. bad: a keys-named file that is neither
// (for the error: the player sees which file was looked at).
static std::string keys_in_dir(const std::string &dir, int kind, std::string *bad) {
    std::string named = dir + (kind == KEYS_PROD ? "/prod.keys" : "/title.keys");
    if (keys_kind(named) == kind) return named;
    DIR *d = opendir(dir.c_str());
    if (!d) return "";
    std::vector<std::string> names;
    while (struct dirent *e = readdir(d))
        if (e->d_name[0] != '.' && keys_file_name(e->d_name)) names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto &n : names) {
        int k = keys_kind(dir + "/" + n);
        if (k == kind) return dir + "/" + n;
        if (k == KEYS_NONE && bad && bad->empty()) *bad = n;
    }
    return "";
}

// The player's prod.keys: in the picked folder (or a few levels below it), next to the app (keys/), or where emulators
// keep them
static std::string find_keys(const std::string &src, std::string *bad) {
    std::vector<std::string> stack = {src, src + "/keys"};
    for (int depth = 0; depth < 3; depth++) {
        std::vector<std::string> next;
        for (auto &dir : stack) {
            std::string k = keys_in_dir(dir, KEYS_PROD, bad);
            if (!k.empty()) return k;
            DIR *d = opendir(dir.c_str());
            if (!d) continue;
            while (struct dirent *e = readdir(d)) {
                std::string n = e->d_name;
                if (n[0] != '.' && is_dir(dir + "/" + n)) next.push_back(dir + "/" + n);
            }
            closedir(d);
        }
        std::sort(next.begin(), next.end());
        stack.swap(next);
    }
    std::vector<std::string> dirs = {src + "/..", "keys", "."};
    if (const char *a = getenv("APPDATA")) {
        dirs.push_back(std::string(a) + "/Ryujinx/system");
        dirs.push_back(std::string(a) + "/yuzu/keys");
    }
    if (const char *h = getenv("USERPROFILE")) dirs.push_back(std::string(h) + "/.switch");
    if (const char *h = getenv("HOME")) dirs.push_back(std::string(h) + "/.switch");
    for (auto &d : dirs) {
        std::string k = keys_in_dir(d, KEYS_PROD, NULL);
        if (!k.empty()) return k;
    }
    return "";
}

// title.keys (only needed for packages without their tickets): next to prod.keys or in the picked folder
static std::string find_title_keys(const std::string &src, const std::string &prod) {
    std::string dir = prod.substr(0, prod.find_last_of("/\\"));
    std::string k = keys_in_dir(dir, KEYS_TITLE, NULL);
    return k.empty() ? keys_in_dir(src, KEYS_TITLE, NULL) : k;
}

static std::atomic<unsigned long long> g_done, g_total;  // (progress: below)
static bool g_import_mode;                               // Z-A packages: the bar is the import, then the build

// zaimport (tools/zaimport, shipped as zaimport/zaimport.exe) writes <root>/exefs, romfs.bin, aoc/; its progress
// lines ("progress <done> <total>") go to the bar, its last line is the reason when it fails
static bool import_packages(const std::vector<std::string> &pkgs, const std::string &keys, const std::string &title_keys,
                            std::string &err) {
    char tool[1100];
    if (!rt_resource("zaimport/zaimport" PLAT_EXE_SUFFIX, "build/zaimport/zaimport" PLAT_EXE_SUFFIX,
                     tool, sizeof tool)) {
        err = "the game file importer is missing from this copy of ZA Recomp";
        return false;
    }
    std::string log = g_root + "/import.log";
    std::vector<const char *> argv = {tool};
    if (!title_keys.empty()) argv.push_back("--title-keys"), argv.push_back(title_keys.c_str());
    argv.push_back(keys.c_str());
    argv.push_back(g_root.c_str());
    for (auto &p : pkgs) argv.push_back(p.c_str());
    argv.push_back(NULL);
    PlatProc p = plat_spawn(argv.data(), log.c_str(), false);
    if (!p) { err = "the game file importer didn't start"; return false; }
    int r;
    char last[512] = "";
    for (;;) {
        r = plat_proc_wait(p, false);
        FILE *f = fopen(log.c_str(), "rb");
        if (f) {  // the newest progress line
            char buf[4096];
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
            size_t k = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[k] = 0;
            while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
            char *nl = strrchr(buf, '\n');
            snprintf(last, sizeof last, "%s", nl ? nl + 1 : buf);
            unsigned long long d, t;
            const char *pl = NULL;
            for (const char *q = strstr(buf, "progress "); q; q = strstr(q + 1, "progress ")) pl = q;
            if (pl && sscanf(pl, "progress %llu %llu", &d, &t) == 2) g_total = t, g_done = d;
        }
        if (r >= 0) break;
        SDL_Delay(100);
    }
    fprintf(stderr, "[launcher] zaimport: exit %d, %s\n", r, last);
    if (r != 0) {
        err = !strncmp(last, "error: ", 7) ? last + 7 : last;
        return false;
    }
    remove(log.c_str());
    return true;
}

// Pokémon Z-A's button icons redrawn for the keyboard and the controllers (textures/prompts/<style>/, the one
// runtime/hle/pl.c picks at boot) from the player's own game files by zaprompts (tools/za/prompt_textures.py frozen,
// with ooz: GPLv3, a program of its own - third_party/ooz/README.ZARecomp.md). Each icon names the key or button bound
// to its input, so they are made for one set of bindings: textures/prompts/.made holds the stamp they were made for
// (PROMPTS_VERSION + a hash of the bindings), and they are made again when it differs - at the setup (waited for: the
// first boot has them), at a boot (runtime/main.c: a set-up copy skips this screen) and right after a rebind in the
// Controls tab (both in the background: from the next start). zaprompts --install draws them into textures/prompts.new
// and swaps that in whole itself (a game never sees half a set). Up to 3 tries per stamp (textures/prompts.tried):
// failing leaves the game's own icons. Log: textures/prompts.log.
#define PROMPTS_VERSION "3"
static std::atomic<bool> g_prompts_busy{false};
static PlatProc g_prompts_proc;  // (the one left running: a newer rebind ends it and starts over)
static std::string prompts_stamp(void) {
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](const void *p, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    };
    mix(g_bind_pad, sizeof g_bind_pad);
    mix(g_bind_key, sizeof g_bind_key);
    char s[48];
    snprintf(s, sizeof s, "%s-%016llx", PROMPTS_VERSION, (unsigned long long)h);
    return s;
}
// a marker file holding this stamp, and (textures/prompts.tried: "<stamp> <tries>") its count
static bool prompts_marked(const char *path, int *tries = NULL) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char v[48] = "";
    int n = 0;
    bool ok = fscanf(f, "%47s %d", v, &n) >= 1 && prompts_stamp() == v;
    fclose(f);
    if (tries) *tries = ok ? n : 0;
    return ok;
}
// (3 tries: a game closed before zaprompts is done may end it with the process)
static bool prompts_wanted(void) {
    char tool[1100];
    int tries = 0;
    prompts_marked("textures/prompts.tried", &tries);
    return rt_resource("zaprompts/zaprompts" PLAT_EXE_SUFFIX, "build/helper/zaprompts/zaprompts" PLAT_EXE_SUFFIX, tool, sizeof tool) &&
           file_exists(g_root + "/romfs.bin") && !prompts_marked("textures/prompts/.made") && tries < 3;
}
// wait: until made (the setup), else started and left running (a boot, a rebind)
static bool make_prompts(bool wait) {
    char tool[1100];
    if (g_prompts_busy.exchange(true)) return false;
    if (!rt_resource("zaprompts/zaprompts" PLAT_EXE_SUFFIX, "build/helper/zaprompts/zaprompts" PLAT_EXE_SUFFIX, tool, sizeof tool)) {
        g_prompts_busy = false;
        return false;
    }
    if (g_prompts_proc && plat_proc_wait(g_prompts_proc, false) < 0) plat_proc_kill(g_prompts_proc);  // (made for old bindings)
    g_prompts_proc = 0;
    mkdir("textures", 0755);
    std::string stamp = prompts_stamp();
    int tries = 0;
    prompts_marked("textures/prompts.tried", &tries);
    if (FILE *f = fopen("textures/prompts.tried", "wb")) fprintf(f, "%s %d", stamp.c_str(), tries + 1), fclose(f);
    std::string romfs = g_root + "/romfs.bin";
    const char *ini = getenv("LARECOMP_SETTINGS");  // (as runtime/settings.c: the bindings the icons name)
    if (!ini || !*ini) ini = "larecomp.ini";
    const char *argv[] = {tool, "--romfs", romfs.c_str(), "--settings", ini, "--mods", "mods", "--out", "textures/prompts",
                          "--install", stamp.c_str(), NULL};
    PlatProc p = plat_spawn(argv, "textures/prompts.log", false);
    if (!wait || !p) {
        fprintf(stderr, "[launcher] button icons: %s\n", p ? "being made (zaprompts, textures/prompts.log)" : "zaprompts didn't start");
        g_prompts_proc = p;
        g_prompts_busy = false;
        return p != 0;
    }
    int r = plat_proc_wait(p, true);
    fprintf(stderr, "[launcher] button icons: %s (zaprompts exit %d; textures/prompts.log)\n", r == 0 ? "made" : "not made", r);
    g_prompts_busy = false;
    return r == 0;
}
// runtime/main.c, a set-up copy booting straight into the game: the icons made in the background if they never were,
// or were made for other bindings
extern "C" void launcher_prompts_kick(const char *root) {
    g_root = root;
    settings_load();  // (the bindings: the boot loads them later, runtime/main.c boot)
    if (prompts_wanted()) make_prompts(false);
}
// runtime/ui/controls_ui.mm, a binding changed (larecomp.ini saved): the icons made again for it, from the next start
extern "C" void launcher_prompts_refresh(void) {
    if (!g_root.empty() && prompts_wanted()) make_prompts(false);
}

// progress (the overlay's bar): the romfs copy, then the image build - each about the size of romfs/ (g_done, g_total)
// The bar: copying and the data image are the first tenth, building the game code (runtime/gamebuild.c) the rest
static unsigned long long g_phase_base;
static std::atomic<float> g_build_frac{-1};  // -1: not building
static std::atomic<bool> g_cancel_build{false};  // (the Play page's Cancel while compiling)
static bool build_cancelled(void) { return g_cancel_build.load(); }
static bool g_copy_skipped;                  // (only the game code to build: the bar is all of that)
static void setup_progress(uint64_t done, uint64_t) { g_done = g_phase_base + done; }
static void build_progress(double f, const char *step) {
    g_build_frac = (float)f;
    static const char *last;
    if (step != last) last = step, set_job(step);
}
static float extract_progress(void) {
    unsigned long long t = g_total, d = g_done;
    float pre = g_import_mode ? 0.25f : 0.1f;  // (Z-A: reading the packages is a quarter of it)
    float b = g_build_frac, f = b >= 0 ? (g_copy_skipped ? b : pre + (1 - pre) * b) : t ? pre * (float)((double)d / (double)t) : 0;
    return f < 0 ? 0 : f > 0.99f ? 0.99f : f;
}

// Builds the game code from the copied files (a few minutes); false = failed (the job line says so)
static bool build_game_code(void) {
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {build_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1, build_cancelled};
    char why[256] = "";
    g_build_frac = 0;
    bool ok = gamebuild_run(g_root.c_str(), &o, why, sizeof why);
    if (!ok) {
        fprintf(stderr, "[launcher] game code: %s\n", why);
        if (g_cancel_build) set_job("Compilation cancelled. Completed files are kept.");
        else set_failure(FAIL_BUILD, gamebuild_killed() ? why : "");
    }
    return ok;
}

static void *extract_thread(void *) {
    std::string src;
    pthread_mutex_lock(&g_lock);
    src = g_src;
    pthread_mutex_unlock(&g_lock);
    g_done = 0, g_total = 0, g_phase_base = 0, g_build_frac = -1, g_copy_skipped = false, g_import_mode = false;
    g_fail = 0, g_fail_detail.clear();
    g_cancel_build = false;
    std::string exefs, romfs;
    std::vector<std::string> pkgs;
    bool unusable = false;  // (game packages rather than its files)
    if (src.empty() && check_copied_files(g_root).empty()) {  // copied already (an update needs new game code)
        g_copy_skipped = true;
        if (!build_game_code()) goto done;
        goto check;
    }
    set_job("Looking for the game files...");
    find_game(src, 0, exefs, romfs, unusable);
    if ((exefs.empty() || romfs.empty()) && unusable) {  // packages: Pokémon Legends Z-A's (zaimport reads them)
        int compressed = 0;
        find_packages(src, 0, pkgs, compressed);
        if (pkgs.empty() && compressed) {
            set_failure(FAIL_UNUSABLE, TR("Your dump is compressed (.nsz / .xcz): decompress it to .nsp / .xci with "
                                          "NSZ first.", "O seu dump está comprimido (.nsz / .xcz): descomprima para .nsp / "
                                          ".xci com o NSZ antes."));
            goto done;
        }
        std::string bad, keys = find_keys(src, &bad), err;
        if (keys.empty()) {
            set_failure(FAIL_KEYS, bad.empty() ? "" : "\"" + bad + TR("\" isn't a prod.keys: it has no header_key line.",
                                                                       "\" não é um prod.keys: não tem a linha header_key."));
            goto done;
        }
        std::string title_keys = find_title_keys(src, keys);
        fprintf(stderr, "[launcher] %zu package(s), keys %s, title keys %s\n", pkgs.size(), keys.c_str(),
                title_keys.empty() ? "-" : title_keys.c_str());
        rt_remove_game_files(g_root.c_str());
        mkdir(g_root.c_str(), 0755);
        g_import_mode = true;
        set_job("Reading your game files...");
        if (!import_packages(pkgs, keys, title_keys, err)) {
            set_failure(err.find("base game") != std::string::npos || err.find("no game packages") != std::string::npos
                            ? FAIL_UNUSABLE : FAIL_IMPORT, err);
            goto done;
        }
        if (!build_id_ok(g_root + "/exefs/main")) { set_failure(FAIL_VERSION); goto done; }
        if (!build_game_code()) goto done;
        goto check;
    }
    if (exefs.empty() || romfs.empty()) { set_failure(unusable ? FAIL_UNUSABLE : FAIL_NOT_FOUND); goto done; }
    if (!build_id_ok(exefs + "/main")) { set_failure(FAIL_VERSION); goto done; }
    {
        // (the picked folder can't be LAS Recomp's own copy: setting up again deletes that first)
        char a[4096], b[4096];
        mkdir(g_root.c_str(), 0755);
        if (realpath(exefs.c_str(), a) && realpath(g_root.c_str(), b) && !strncmp(a, b, strlen(b)) &&
            (a[strlen(b)] == '/' || a[strlen(b)] == '\\' || !a[strlen(b)])) {
            set_failure(FAIL_NOT_FOUND);
            goto done;
        }
        rt_remove_game_files(g_root.c_str());  // (an earlier or interrupted setup's; removes the folder too)
        mkdir(g_root.c_str(), 0755);
        unsigned long long rbytes = romfs_dir_bytes(romfs.c_str());
        g_total = rbytes;
        set_job("Copying the game files...");
        if (!romfs_copy_tree(exefs.c_str(), (g_root + "/exefs").c_str(), NULL) ||
            !romfs_copy_tree(romfs.c_str(), (g_root + "/romfs").c_str(), setup_progress)) {
            set_failure(FAIL_COPY);
            goto done;
        }
        g_phase_base = rbytes, g_done = rbytes;
        // (no data image: runtime/services/fs.c serves the copied romfs/ folder itself.) Retrying a cancelled
        // compilation then uses the copied files and their build cache.
        pthread_mutex_lock(&g_lock);
        g_src.clear();
        pthread_mutex_unlock(&g_lock);
        if (!build_game_code()) goto done;
    }
check:
    {
        std::string why = check_game_files(g_root);
        if (why.empty() && prompts_wanted()) {
            set_job("Drawing the button icons for your controller...");
            make_prompts(true);
        }
        if (why.empty()) set_job("Game files ready.");
        else set_failure(why.find("version") != std::string::npos ? FAIL_VERSION : FAIL_MISSING);
    }
done:
    pthread_mutex_lock(&g_lock);
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

// ------------------------------------------------------------------ folder dialog (SDL calls back on any thread)
static void SDLCALL picked(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_src = files[0];
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ a save from the player's Switch (or another copy)
// The game's save is its own files, as JKSV backs them up on a modded Switch (SD card: JKSV/<game>/<user - date>/,
// holding "main") and as save/save_0/ holds them here (runtime/services/fs.c): copied over as they are. The picked
// folder may be that backup, or one above it (the game's JKSV folder: its newest backup). The current save is moved
// to save/backup-<date>/ first.
static std::string g_save_pick, g_save_msg;
static bool g_save_failed;

static void SDLCALL picked_save(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_save_pick = files[0];
    pthread_mutex_unlock(&g_lock);
}

// the folder holding "main" (the newest one, a few levels down)
static void find_save(const std::string &dir, int depth, std::string &best, time_t &best_t) {
    struct stat st;
    if (stat((dir + "/main").c_str(), &st) == 0 && S_ISREG(st.st_mode) && (best.empty() || st.st_mtime > best_t))
        best = dir, best_t = st.st_mtime;
    if (depth >= 3) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] != '.' && is_dir(dir + "/" + n)) sub.push_back(dir + "/" + n);
    }
    closedir(d);
    for (auto &s : sub) find_save(s, depth + 1, best, best_t);
}

// plain copies (never links: the game writes its save in place, the backup it came from stays as it was)
static bool copy_save_tree(const std::string &src, const std::string &dst) {
    mkdir(dst.c_str(), 0755);
    DIR *d = opendir(src.c_str());
    if (!d) return false;
    bool ok = true;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string a = src + "/" + n, b = dst + "/" + n;
        if (is_dir(a)) { ok &= copy_save_tree(a, b); continue; }
        FILE *in = fopen(a.c_str(), "rb"), *out = in ? fopen(b.c_str(), "wb") : NULL;
        if (in && out) {
            char buf[65536];
            size_t k;
            while ((k = fread(buf, 1, sizeof buf, in)) > 0) ok &= fwrite(buf, 1, k, out) == k;
        } else {
            ok = false;
        }
        if (in) fclose(in);
        if (out && fclose(out) != 0) ok = false;
    }
    closedir(d);
    return ok;
}

static std::string system_tar(void);
static bool run_tool(const std::vector<const char *> &args, const char *log);
static void import_save() {
    pthread_mutex_lock(&g_lock);
    std::string pick;
    pick.swap(g_save_pick);
    pthread_mutex_unlock(&g_lock);
    if (pick.empty()) return;
    g_save_failed = true;
    mkdir("save", 0755);
    std::string tmp;
    if (!is_dir(pick)) {  // a file: a .zip of the save (JKSV can zip its backups), opened into save/.import
        tmp = "save/.import";
        plat_remove_tree(tmp.c_str());
        mkdir(tmp.c_str(), 0755);
        std::string tar = system_tar(), log = std::string(rt_data_root()) + "/save/.import.log";
        if (!run_tool({tar.c_str(), "-x", "-f", pick.c_str(), "-C", tmp.c_str()}, log.c_str())) {
            plat_remove_tree(tmp.c_str());
            g_save_msg = TR("That file couldn't be opened: choose the save's folder, or a .zip of it.",
                            "Não deu para abrir esse arquivo: escolha a pasta do save, ou um .zip dela.");
            return;
        }
        remove(log.c_str());
    }
    std::string from;
    time_t t = 0;
    find_save(tmp.empty() ? pick : tmp, 0, from, t);
    if (from.empty()) {
        if (!tmp.empty()) plat_remove_tree(tmp.c_str());
        g_save_msg = TR("No save found there (a folder with the file \"main\").",
                        "Nenhum save aí (uma pasta com o arquivo \"main\").");
        return;
    }
    std::string backup;
    if (is_dir("save/save_0")) {
        char name[64];
        time_t now = time(NULL);
        strftime(name, sizeof name, "save/backup-%Y%m%d-%H%M%S", localtime(&now));
        backup = name;
        if (rename("save/save_0", backup.c_str()) != 0) {
            if (!tmp.empty()) plat_remove_tree(tmp.c_str());
            g_save_msg = TR("The current save couldn't be moved aside: nothing changed.",
                            "Não deu para guardar o save atual: nada mudou.");
            return;
        }
    }
    bool ok = copy_save_tree(from, "save/save_0");
    if (!tmp.empty()) plat_remove_tree(tmp.c_str());
    if (!ok) {
        g_save_msg = std::string(TR("The save couldn't be copied.", "Não deu para copiar o save.")) +
                     (backup.empty() ? "" : std::string(TR(" Your previous save is in ", " O seu save anterior está em ")) + backup + ".");
        return;
    }
    fprintf(stderr, "[launcher] save imported from %s%s%s\n", pick.c_str(), backup.empty() ? "" : ", previous one in ", backup.c_str());
    g_save_failed = false;
    g_save_msg = std::string(TR("Save imported.", "Save importado.")) +
                 (backup.empty() ? "" : std::string(TR(" Your previous save was kept as a backup.", " O seu save anterior virou um backup.")));
}

// ------------------------------------------------------------------ the user's own Nintendo fonts (not shipped)
// runtime/hle/pl.c loads fonts/ first, else a close substitute (Hiragino Sans). Import copies recognized files only.
static const char *FONT_STD[] = {"nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_std_003.ttf",
                                 "nintendo_udsgr_std_003.ttf", "nintendo_udsgr_std_003.bfttf"};
static const char *FONT_EXT[] = {"nintendo_ext_003.bfttf", "nintendo_ext_003.ttf"};
static std::vector<std::string> g_font_picks;
static std::string g_font_msg;

static bool have_font(const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        if (file_exists(std::string("fonts/") + names[i])) return true;
    return false;
}

static void SDLCALL picked_fonts(void *, const char *const *files, int) {
    if (!files) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; files[i]; i++) g_font_picks.push_back(files[i]);
    pthread_mutex_unlock(&g_lock);
}

static void import_fonts() {
    pthread_mutex_lock(&g_lock);
    std::vector<std::string> picks;
    picks.swap(g_font_picks);
    pthread_mutex_unlock(&g_lock);
    if (picks.empty()) return;
    mkdir("fonts", 0755);
    int ok = 0, skipped = 0;
    for (auto &src : picks) {
        std::string base = src.substr(src.find_last_of('/') + 1), low = base;
        for (auto &c : low) c = (char)tolower(c);
        bool known = false;
        for (auto n : FONT_STD) known |= low == n;
        for (auto n : FONT_EXT) known |= low == n;
        if (!known) { skipped++; continue; }
        FILE *in = fopen(src.c_str(), "rb"), *out = fopen(("fonts/" + low).c_str(), "wb");
        if (in && out) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
            ok++;
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }
    g_font_msg = std::to_string(ok) + " font file(s) imported" + (skipped ? ", " + std::to_string(skipped) + " not recognized" : "") +
                 ". Used from the next game start.";
}

static void remove_fonts() {
    for (auto n : FONT_STD) unlink((std::string("fonts/") + n).c_str());
    for (auto n : FONT_EXT) unlink((std::string("fonts/") + n).c_str());
    g_font_msg = "Imported fonts removed: the substitute font is used.";
}

// ------------------------------------------------------------------ helpers for the Mods and Saves pages
// A system tool beside the OS: Windows 10+ ships bsdtar (libarchive: .zip, .rar, .7z) as System32\tar.exe; macOS's
// tar is bsdtar too; Linux: bsdtar when installed, else GNU tar (.tar archives only)
static std::string system_tar(void) {
#ifdef _WIN32
    const char *sr = getenv("SystemRoot");
    return std::string(sr ? sr : "C:\\Windows") + "\\System32\\tar.exe";
#else
    return file_exists("/usr/bin/bsdtar") ? "/usr/bin/bsdtar" : "/usr/bin/tar";
#endif
}
static bool run_tool(const std::vector<const char *> &args, const char *log) {
    std::vector<const char *> av = args;
    av.push_back(NULL);
    PlatProc p = plat_spawn(av.data(), log, false);
    return p && plat_proc_wait(p, true) == 0;
}
static std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::string base_name(const std::string &p) {
    std::string s = p;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    size_t k = s.find_last_of("/\\");
    return k == std::string::npos ? s : s.substr(k + 1);
}
static bool is_archive(const std::string &p) {
    std::string l = lower(p);
    for (const char *x : {".zip", ".rar", ".7z", ".tar", ".tar.gz", ".tgz"})
        if (l.size() > strlen(x) && !l.compare(l.size() - strlen(x), strlen(x), x)) return true;
    return false;
}
static std::string strip_archive_ext(std::string n) {
    std::string l = lower(n);
    for (const char *x : {".tar.gz", ".tgz", ".zip", ".rar", ".7z", ".tar"})
        if (l.size() > strlen(x) && !l.compare(l.size() - strlen(x), strlen(x), x)) return n.substr(0, n.size() - strlen(x));
    return n;
}
// a folder name that works on every OS: no separators or reserved characters, no trailing dots / spaces
static std::string safe_name(std::string n) {
    std::string o;
    for (char c : n) o += strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32 ? '_' : c;
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    while (!o.empty() && (o[0] == '.' || o[0] == '_' || o[0] == ' ')) o.erase(0, 1);  // (fs.c: those are off)
    return o.empty() ? "mod" : o;
}
static int count_files(const std::string &dir, int depth = 0) {
    if (depth > 12) return 0;
    DIR *d = opendir(dir.c_str());
    if (!d) return 0;
    int n = 0;
    while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string p = dir + "/" + e->d_name;
        n += is_dir(p) ? count_files(p, depth + 1) : 1;
    }
    closedir(d);
    return n;
}
// newest file time and total size under dir
static void tree_stat(const std::string &dir, time_t &newest, unsigned long long &bytes, int depth = 0) {
    DIR *d = depth < 8 ? opendir(dir.c_str()) : NULL;
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) tree_stat(p, newest, bytes, depth + 1);
        else bytes += (unsigned long long)st.st_size, newest = st.st_mtime > newest ? st.st_mtime : newest;
    }
    closedir(d);
}
static std::string fmt_time(time_t t) {
    char b[64];
    strftime(b, sizeof b, g_pt ? "%d/%m/%Y, %H:%M" : "%Y-%m-%d, %H:%M", localtime(&t));
    return b;
}
static std::string fmt_size(unsigned long long n) {
    char b[32];
    if (n >= (1ull << 30)) snprintf(b, sizeof b, "%.1f GB", n / 1073741824.0);
    else if (n >= (1ull << 20)) snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    else snprintf(b, sizeof b, "%llu KB", (n + 1023) / 1024);
    std::string s = b;
    if (g_pt) for (auto &c : s) if (c == '.') c = ',';
    return s;
}

// ------------------------------------------------------------------ mods (mods/<name>/romfs: runtime/services/fs.c)
// The list in the order fs.c layers them (settings mods_order, the rest by name: the first one wins where two replace
// the same file), each on unless it's in mods_off. Installed from a downloaded archive (.zip / .rar / .7z, by the
// system's tar) or a folder: the folder holding a romfs/ (or an arc/ - a romfs without its own folder) in it becomes
// mods/<name>/romfs. Mods of the game's code (exefs, IPS patches, cheats) are refused: this port has no such layer.
struct ModEntry { std::string name; bool on; int files; };
static std::vector<ModEntry> g_mods;
static pthread_mutex_t g_mod_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_mod_pick, g_mod_msg;  // a pick waiting to install; the last install's result
static std::string g_mod_installed;        // its folder name, for the UI thread to put first (g_mod_lock)
static bool g_mod_failed;
static std::atomic<bool> g_mod_busy{false}, g_mods_changed{true};

static void mods_reload(void) {
    std::vector<std::string> names;
    if (DIR *d = opendir("mods")) {
        while (struct dirent *e = readdir(d))
            if (e->d_name[0] != '.' && e->d_name[0] != '_' && is_dir(std::string("mods/") + e->d_name + "/romfs"))
                names.push_back(e->d_name);
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    std::vector<std::string> ordered;
    for (const char *p = g_settings.mods_order; *p;) {  // (listed ones that still exist, in their order)
        const char *bar = strchr(p, '|');
        std::string n(p, bar ? (size_t)(bar - p) : strlen(p));
        if (std::find(names.begin(), names.end(), n) != names.end() &&
            std::find(ordered.begin(), ordered.end(), n) == ordered.end())
            ordered.push_back(n);
        if (!bar) break;
        p = bar + 1;
    }
    for (auto &n : names)
        if (std::find(ordered.begin(), ordered.end(), n) == ordered.end()) ordered.push_back(n);
    g_mods.clear();
    for (auto &n : ordered)
        g_mods.push_back({n, !settings_list_has(g_settings.mods_off, n.c_str()), count_files("mods/" + n + "/romfs")});
}
static void mods_store(void) {
    std::string order, off;
    for (auto &m : g_mods) {
        order += (order.empty() ? "" : "|") + m.name;
        if (!m.on) off += (off.empty() ? "" : "|") + m.name;
    }
    snprintf(g_settings.mods_order, sizeof g_settings.mods_order, "%s", order.c_str());
    snprintf(g_settings.mods_off, sizeof g_settings.mods_off, "%s", off.c_str());
    settings_save();
}

// the mod's romfs inside dir (a few levels down): a folder named romfs, or one holding arc/ (Pokémon Z-A's packs).
// exefs: a folder named exefs was seen (a code mod).
static std::string find_mod_romfs(const std::string &dir, int depth, bool &exefs) {
    if (depth > 5) return "";
    if (is_dir(dir + "/arc")) return dir;
    DIR *d = opendir(dir.c_str());
    if (!d) return "";
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == ".." || !is_dir(dir + "/" + n)) continue;
        if (lower(n) == "romfs") { closedir(d); return dir + "/" + n; }
        if (lower(n) == "exefs") exefs = true;
        sub.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(sub.begin(), sub.end());
    for (auto &s : sub) {
        std::string r = find_mod_romfs(s, depth + 1, exefs);
        if (!r.empty()) return r;
    }
    return "";
}

static void *mod_install_thread(void *) {
    pthread_mutex_lock(&g_mod_lock);
    std::string pick;
    pick.swap(g_mod_pick);
    pthread_mutex_unlock(&g_mod_lock);
    std::string msg;
    bool ok = false;
    mkdir("mods", 0755);
    const std::string tmp = "mods/.install";  // (fs.c skips '.' folders)
    plat_remove_tree(tmp.c_str());
    std::string base = pick, name = safe_name(strip_archive_ext(base_name(pick)));
    bool archive = !is_dir(pick);
    if (archive) {
        mkdir(tmp.c_str(), 0755);
        std::string tar = system_tar(), log = std::string(rt_data_root()) + "/mods/.install.log";
        if (!run_tool({tar.c_str(), "-x", "-f", pick.c_str(), "-C", tmp.c_str()}, log.c_str())) {
            msg = TR("That file couldn't be opened (a .zip, .rar or .7z is expected).",
                     "Não deu para abrir esse arquivo (precisa ser .zip, .rar ou .7z).");
            goto done;
        }
        remove(log.c_str());
        base = tmp;
    }
    {
        bool exefs = false;
        std::string romfs = find_mod_romfs(base, 0, exefs);
        if (romfs.empty()) {
            msg = exefs ? TR("This mod changes the game's code (exefs): that kind of mod doesn't work in ZA Recomp.",
                             "Esse mod mexe no código do jogo (exefs): esse tipo de mod não funciona no ZA Recomp.")
                        : TR("No romfs folder in it: this doesn't look like a mod of the game's files.",
                             "Não tem pasta romfs: não parece um mod dos arquivos do jogo.");
            goto done;
        }
        // the mod's name: the folder around its romfs (as mods come: "<Name>/romfs"), else the file / folder name
        std::string parent = romfs.substr(0, romfs.find_last_of('/'));
        if (lower(base_name(romfs)) == "romfs" && parent != base && parent.size() > base.size()) name = safe_name(base_name(parent));
        std::string dst = "mods/" + name;
        for (int i = 2; file_exists(dst); i++) dst = "mods/" + name + " (" + std::to_string(i) + ")";
        name = dst.substr(5);
        mkdir(dst.c_str(), 0755);
        ok = archive ? plat_rename(romfs.c_str(), (dst + "/romfs").c_str()) == 0 : copy_save_tree(romfs, dst + "/romfs");
        if (!ok) {
            plat_remove_tree(dst.c_str());
            msg = TR("The mod couldn't be copied into the mods folder (disk full?).",
                     "Não deu para copiar o mod para a pasta de mods (disco cheio?).");
            goto done;
        }
        msg = std::string(TR("Installed: ", "Instalado: ")) + name;
        fprintf(stderr, "[launcher] mod installed: %s -> %s\n", pick.c_str(), dst.c_str());
    }
done:
    if (archive) plat_remove_tree(tmp.c_str());
    if (!ok) fprintf(stderr, "[launcher] mod install from %s: %s\n", pick.c_str(), msg.c_str());
    pthread_mutex_lock(&g_mod_lock);
    g_mod_msg = msg, g_mod_failed = !ok;
    if (ok) g_mod_installed = name;
    pthread_mutex_unlock(&g_mod_lock);
    g_mods_changed = true;
    g_mod_busy = false;
    return NULL;
}
static void mod_install(const std::string &path) {
    if (path.empty() || g_mod_busy.exchange(true)) return;
    pthread_mutex_lock(&g_mod_lock);
    g_mod_pick = path, g_mod_msg = TR("Installing...", "Instalando...");
    g_mod_failed = false;
    pthread_mutex_unlock(&g_mod_lock);
    pthread_t t;
    if (pthread_create(&t, NULL, mod_install_thread, NULL) == 0) pthread_detach(t);
    else g_mod_busy = false;
}
static void SDLCALL picked_mod(void *, const char *const *files, int) {
    if (files && files[0]) mod_install(files[0]);
}

// ------------------------------------------------------------------ saves (save/save_0, its backups beside it)
struct SaveBackup { std::string dir; time_t when; };
static std::vector<SaveBackup> g_backups;
static time_t g_save_time;
static unsigned long long g_save_bytes;
static bool g_save_scanned;
static std::string g_export_pick;

static void saves_scan(void) {
    g_save_time = 0, g_save_bytes = 0;
    if (is_dir("save/save_0")) tree_stat("save/save_0", g_save_time, g_save_bytes);
    g_backups.clear();
    if (DIR *d = opendir("save")) {
        while (struct dirent *e = readdir(d)) {
            int Y, M, D, h, m, sec;
            if (sscanf(e->d_name, "backup-%4d%2d%2d-%2d%2d%2d", &Y, &M, &D, &h, &m, &sec) != 6) continue;
            struct tm tm = {};
            tm.tm_year = Y - 1900, tm.tm_mon = M - 1, tm.tm_mday = D, tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = sec;
            tm.tm_isdst = -1;
            g_backups.push_back({std::string("save/") + e->d_name, mktime(&tm)});
        }
        closedir(d);
    }
    std::sort(g_backups.begin(), g_backups.end(), [](const SaveBackup &a, const SaveBackup &b) { return a.when > b.when; });
    g_save_scanned = true;
}
// the current save moved aside as a dated backup ("" when there was none; false: it couldn't be)
static bool save_set_aside(std::string &backup) {
    backup.clear();
    if (!is_dir("save/save_0")) return true;
    char name[64];
    time_t now = time(NULL);
    strftime(name, sizeof name, "save/backup-%Y%m%d-%H%M%S", localtime(&now));
    backup = name;
    for (int i = 2; file_exists(backup); i++) backup = std::string(name) + "-" + std::to_string(i);
    return rename("save/save_0", backup.c_str()) == 0;
}
static void save_restore(const std::string &dir) {
    std::string backup;
    g_save_failed = true;
    if (!save_set_aside(backup)) { g_save_msg = TR("The current save couldn't be moved aside: nothing changed.",
                                                   "Não deu para guardar o save atual: nada mudou."); return; }
    if (!copy_save_tree(dir, "save/save_0")) {
        g_save_msg = TR("The backup couldn't be copied.", "Não deu para copiar o backup.");
        return;
    }
    fprintf(stderr, "[launcher] save restored from %s%s%s\n", dir.c_str(), backup.empty() ? "" : ", previous one in ",
            backup.c_str());
    g_save_failed = false;
    g_save_msg = TR("Backup restored. The save it replaced is now a backup too.",
                    "Backup restaurado. O save que estava antes também virou backup.");
}
static void SDLCALL picked_export(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_export_pick = files[0];
    pthread_mutex_unlock(&g_lock);
}
static void save_export(void) {
    pthread_mutex_lock(&g_lock);
    std::string out;
    out.swap(g_export_pick);
    pthread_mutex_unlock(&g_lock);
    if (out.empty()) return;
    if (lower(out).size() < 4 || lower(out).compare(lower(out).size() - 4, 4, ".zip")) out += ".zip";
    std::string tar = system_tar(), log = std::string(rt_data_root()) + "/save/.export.log";
    remove(out.c_str());
    // -a: the archive type from the name (.zip)
    bool ok = run_tool({tar.c_str(), "-a", "-c", "-f", out.c_str(), "-C", "save", "save_0"}, log.c_str());
    if (ok) remove(log.c_str());
    g_save_failed = !ok;
    g_save_msg = ok ? std::string(TR("Save exported to ", "Save exportado para ")) + out
                    : TR("The save couldn't be exported (save/.export.log says why).",
                         "Não deu para exportar o save (o motivo está em save/.export.log).");
    fprintf(stderr, "[launcher] save export to %s: %s\n", out.c_str(), ok ? "ok" : "failed");
}

// ------------------------------------------------------------------ screen
// ZA Recomp's start screen (2026-10-27, its own look - not LAS Recomp's): a top bar (logo, version, the pages Play,
// Mods, Saves, Settings and About, PT | EN), the page under it over a night city of our own (drawn here: nothing of
// the game's), a status bar at the bottom. Layout in 1280 x 720 units scaled to the window's height.
static ImFont *g_font_title, *g_font_menu, *g_font_small;  // (the in-game menu's tabs and the first-play questions)
static ImFont *F_BODY, *F_SMALL, *F_LABEL, *F_H2, *F_H1, *F_BTN;  // the start screen's own sizes
static float g_fb = 1;  // pixel size per unit of a font's nominal size (Baloo 2 runs small: 1.5)
static float g_s = 1;   // window pixels per layout unit
static ImVec2 g_o;      // the window's top left
static float W_ = 1280, H_ = 720;  // the window in layout units
extern "C" int g_start_screen;  // runtime/hle/warp.c: the start screen is up (no game loaded: don't read its memory)
void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float));  // menu.mm
void menu_tabs_focus(bool first_tab);
extern int g_tab_request_ext;
extern "C" void menu_apply_window_requests(void *sdl_window);
// The Randomizer tab's Play / Play Now on the start screen (rando_tab.mm, Link's Awakening): start with that seed
static bool g_play_pending;
extern "C" void launcher_play_seed(const char *seed) {
    snprintf(g_settings.rando_seed, sizeof g_settings.rando_seed, "%s", seed ? seed : "");
    g_play_pending = true;
}

static const ImU32 C_TEXT = IM_COL32(238, 243, 246, 255), C_SOFT = IM_COL32(194, 209, 219, 255),
                   C_MUTED = IM_COL32(159, 179, 194, 255), C_GOLD = IM_COL32(242, 181, 68, 255),
                   C_GOLD_HI = IM_COL32(255, 215, 122, 255), C_TEAL = IM_COL32(63, 208, 201, 255),
                   C_GREEN = IM_COL32(74, 208, 138, 255), C_RED = IM_COL32(255, 138, 112, 255),
                   C_INK = IM_COL32(27, 20, 5, 255), C_PANEL = IM_COL32(8, 19, 31, 218),
                   C_LINE = IM_COL32(255, 255, 255, 28), C_GLASS = IM_COL32(255, 255, 255, 16);

static ImVec2 P(float x, float y) { return ImVec2(g_o.x + x * g_s, g_o.y + y * g_s); }
// the last item is hovered, or has the keyboard / controller cursor while that cursor shows (not the focus a mouse
// user never sees)
static bool item_hot(void) { return ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible); }
static ImVec2 S(float w, float h) { return ImVec2(w * g_s, h * g_s); }
// text of `size` units (the design's px at 720p); wrap: units, 0 = one line. Returns its size in units.
static ImVec2 text_size(ImFont *f, float size, const char *t, float wrap = 0) {
    ImVec2 v = f->CalcTextSizeA(size * g_fb * g_s, FLT_MAX, wrap * g_s, t);
    return ImVec2(v.x / g_s, v.y / g_s);
}
static ImVec2 text(ImDrawList *dl, ImFont *f, float size, float x, float y, ImU32 col, const char *t, float wrap = 0) {
    dl->AddText(f, size * g_fb * g_s, P(x, y), col, t, NULL, wrap * g_s);
    return text_size(f, size, t, wrap);
}
static void panel(ImDrawList *dl, float x, float y, float w, float h, ImU32 fill = C_PANEL, ImU32 border = C_LINE,
                  float r = 18) {
    dl->AddRectFilled(P(x, y), P(x + w, y + h), fill, r * g_s);
    if (border) dl->AddRect(P(x, y), P(x + w, y + h), border, r * g_s, 0, 1.2f * g_s);
}

// line icons, drawn (the fonts have none): size = the box's side in units, centred on cx, cy
enum Icon { IC_NONE, IC_PLAY, IC_FOLDER, IC_UP_ARROW, IC_DOWN_ARROW, IC_CHECK, IC_CHEV_UP, IC_CHEV_DOWN, IC_TRASH,
            IC_LAYERS, IC_SAVE, IC_RESTORE, IC_GLOBE };
static void icon(ImDrawList *dl, int k, float cx, float cy, float size, ImU32 col) {
    float u = size / 24, t = 2.0f * u * g_s;
    auto Q = [&](float x, float y) { return P(cx + (x - 12) * u, cy + (y - 12) * u); };
    auto L = [&](float x0, float y0, float x1, float y1) { dl->AddLine(Q(x0, y0), Q(x1, y1), col, t); };
    switch (k) {
    case IC_PLAY: dl->AddTriangleFilled(Q(7, 4), Q(20, 12), Q(7, 20), col); break;
    case IC_FOLDER: {
        ImVec2 p[] = {Q(3, 6), Q(9, 6), Q(11, 8), Q(21, 8), Q(21, 19), Q(3, 19)};
        dl->AddPolyline(p, 6, col, ImDrawFlags_Closed, t);
        break;
    }
    case IC_UP_ARROW: L(12, 16, 12, 4), L(7, 9, 12, 4), L(17, 9, 12, 4), L(4, 20, 20, 20); break;
    case IC_DOWN_ARROW: L(12, 4, 12, 16), L(7, 11, 12, 16), L(17, 11, 12, 16), L(4, 20, 20, 20); break;
    case IC_CHECK: L(5, 12, 10, 17), L(10, 17, 19, 7); break;
    case IC_CHEV_UP: L(6, 15, 12, 9), L(12, 9, 18, 15); break;
    case IC_CHEV_DOWN: L(6, 9, 12, 15), L(12, 15, 18, 9); break;
    case IC_TRASH: L(4, 7, 20, 7), L(9, 7, 9, 4), L(9, 4, 15, 4), L(15, 4, 15, 7), L(6, 7, 7, 20), L(7, 20, 17, 20),
                   L(17, 20, 18, 7); break;
    case IC_LAYERS: {
        ImVec2 a[] = {Q(12, 3), Q(21, 8), Q(12, 13), Q(3, 8)};
        dl->AddPolyline(a, 4, col, ImDrawFlags_Closed, t);
        L(3, 13, 12, 18), L(12, 18, 21, 13);
        break;
    }
    case IC_SAVE: {
        ImVec2 a[] = {Q(5, 3), Q(16, 3), Q(19, 6), Q(19, 21), Q(5, 21)};
        dl->AddPolyline(a, 5, col, ImDrawFlags_Closed, t);
        L(8, 3, 8, 9), L(8, 9, 16, 9), L(16, 9, 16, 3);
        break;
    }
    case IC_RESTORE:
        dl->PathArcTo(Q(12, 12), 8 * u * g_s, 3.14159f * 1.15f, 3.14159f * 2.95f, 20);
        dl->PathStroke(col, 0, t);
        L(4, 4, 4, 8), L(4, 8, 8, 8);
        break;
    case IC_GLOBE:
        dl->AddCircle(Q(12, 12), 9 * u * g_s, col, 24, t);
        dl->AddEllipse(Q(12, 12), ImVec2(4 * u * g_s, 9 * u * g_s), col, 0, 24, t);
        L(3, 12, 21, 12);
        break;
    }
}
// the project's mark: a hexagon with a Z (drawn: no image file)
static void logo(ImDrawList *dl, float x, float y, float size) {
    float u = size / 30;
    auto Q = [&](float a, float b) { return P(x + a * u, y + b * u); };
    ImVec2 hex[] = {Q(15, 2), Q(26, 8.5f), Q(26, 21.5f), Q(15, 28), Q(4, 21.5f), Q(4, 8.5f)};
    dl->AddPolyline(hex, 6, C_GOLD, ImDrawFlags_Closed, 2 * u * g_s);
    ImVec2 z[] = {Q(10.5f, 10.5f), Q(19.5f, 10.5f), Q(10.5f, 19.5f), Q(19.5f, 19.5f)};
    dl->AddPolyline(z, 4, C_TEXT, 0, 2.4f * u * g_s);
}

// A button. kind: primary (gold), secondary (glass), ghost (text), tab. w <= 0: as wide as its text. True when pressed.
enum { BTN_PRIMARY, BTN_SECONDARY, BTN_GHOST, BTN_DANGER };
static bool button(const char *id, const char *label, float x, float y, float w, float h, int kind, bool enabled = true,
                   int ic = IC_NONE, ImFont *f = NULL, float fs = 15, bool default_focus = false) {
    if (!f) f = F_LABEL;
    float isz = fs * 1.15f, pad = h * 0.42f, tw = text_size(f, fs, label).x, gap = ic ? isz * 0.55f : 0;
    if (w <= 0) w = tw + 2 * pad + (ic ? isz + gap : 0);
    ImGui::SetCursorScreenPos(P(x, y));
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::InvisibleButton(id, S(w, h), ImGuiButtonFlags_EnableNav);
    if (default_focus) ImGui::SetItemDefaultFocus();
    bool hot = enabled && item_hot();
    ImGui::EndDisabled();
    if (hot && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = fminf(14, h * 0.3f);
    ImU32 fg = C_TEXT;
    if (!enabled && kind == BTN_GHOST) {
        fg = IM_COL32(159, 179, 194, 70);  // (an arrow with nowhere to go: just faded)
    } else if (!enabled) {
        dl->AddRectFilled(P(x, y), P(x + w, y + h), IM_COL32(255, 255, 255, 14), r * g_s);
        dl->AddRect(P(x, y), P(x + w, y + h), IM_COL32(255, 255, 255, 30), r * g_s, 0, 1.2f * g_s);
        fg = C_MUTED;
    } else if (kind == BTN_PRIMARY) {
        dl->AddRectFilled(P(x, y + 4), P(x + w, y + h + 6), IM_COL32(242, 181, 68, hot ? 70 : 40), r * g_s);  // (glow)
        dl->AddRectFilled(P(x, y), P(x + w, y + h), hot ? IM_COL32(255, 206, 102, 255) : C_GOLD, r * g_s);
        dl->AddRectFilled(P(x + 2, y + 2), P(x + w - 2, y + h * 0.48f), IM_COL32(255, 255, 255, hot ? 46 : 30), r * g_s,
                          ImDrawFlags_RoundCornersTop);
        fg = C_INK;
    } else if (kind == BTN_SECONDARY || kind == BTN_DANGER) {
        dl->AddRectFilled(P(x, y), P(x + w, y + h), hot ? IM_COL32(255, 255, 255, 34) : C_GLASS, r * g_s);
        dl->AddRect(P(x, y), P(x + w, y + h), kind == BTN_DANGER ? IM_COL32(255, 138, 112, hot ? 200 : 120)
                                              : IM_COL32(255, 255, 255, hot ? 90 : 42), r * g_s, 0, 1.2f * g_s);
        if (kind == BTN_DANGER) fg = C_RED;
    } else {
        if (hot) dl->AddRectFilled(P(x, y), P(x + w, y + h), IM_COL32(255, 255, 255, 22), r * g_s);
        fg = hot ? C_TEXT : C_SOFT;
    }
    float cx = x + (w - tw - (ic ? isz + gap : 0)) / 2, th = text_size(f, fs, label).y;
    if (ic) icon(dl, ic, cx + isz / 2, y + h / 2, isz, fg), cx += isz + gap;
    text(dl, f, fs, cx, y + (h - th) / 2, fg, label);
    return pressed;
}
// an on / off switch (a button: A / click flips it)
static bool toggle(const char *id, bool *v, float x, float y) {
    float w = 54, h = 30;
    ImGui::SetCursorScreenPos(P(x, y));
    bool pressed = ImGui::InvisibleButton(id, S(w, h), ImGuiButtonFlags_EnableNav);
    bool hot = item_hot();
    if (pressed) *v = !*v;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(P(x, y), P(x + w, y + h), *v ? IM_COL32(63, 191, 127, 255) : IM_COL32(255, 255, 255, hot ? 70 : 46),
                      h / 2 * g_s);
    float kx = *v ? x + w - h / 2 : x + h / 2;
    dl->AddCircleFilled(P(kx, y + h / 2), (h / 2 - 3) * g_s, *v ? IM_COL32(255, 255, 255, 255) : C_SOFT);
    return pressed;
}

// ---- the background: a night city of our own (sky, aurora, crescent moon, a tower with golden rings, lit windows),
// drawn to cover the window (a 1280 x 720 picture, cropped to the window's shape)
static void draw_art(ImDrawList *dl, ImVec2 o, ImVec2 ds) {
    float k = fmaxf(ds.x / 1280, ds.y / 720);
    ImVec2 off(o.x + (ds.x - 1280 * k) / 2, o.y + (ds.y - 720 * k) / 2);
    auto A = [&](float x, float y) { return ImVec2(off.x + x * k, off.y + y * k); };
    auto band = [&](float y0, float y1, ImU32 a, ImU32 b) { dl->AddRectFilledMultiColor(A(0, y0), A(1280, y1), a, a, b, b); };
    band(0, 360, IM_COL32(5, 11, 23, 255), IM_COL32(11, 36, 54, 255));
    band(360, 590, IM_COL32(11, 36, 54, 255), IM_COL32(23, 68, 81, 255));
    band(590, 720, IM_COL32(23, 68, 81, 255), IM_COL32(42, 90, 92, 255));
    for (int i = 0; i < 18; i++)  // (a soft teal aurora up left: stacked translucent discs)
        dl->AddCircleFilled(A(320, 150), (640 - i * 34) * k, IM_COL32(63, 208, 201, 3), 64);
    static const float stars[][4] = {{64, 48, 1.2f, .8f}, {142, 110, .9f, .6f}, {230, 36, 1.4f, .9f}, {318, 88, .8f, .5f},
                                     {402, 22, 1.1f, .7f}, {470, 140, .9f, .5f}, {556, 64, 1.3f, .8f}, {640, 30, .8f, .6f},
                                     {712, 118, 1, .6f}, {790, 52, 1.4f, .9f}, {860, 96, .9f, .5f}, {934, 28, 1.1f, .7f},
                                     {1180, 60, 1.2f, .8f}, {1240, 132, .9f, .6f}, {1112, 210, .8f, .5f}, {96, 190, 1, .5f},
                                     {276, 226, .8f, .4f}, {602, 198, .9f, .4f}, {760, 250, .8f, .4f}, {1004, 170, 1, .5f}};
    float tw = (float)ImGui::GetTime();
    for (int i = 0; i < 20; i++) {  // (a slow twinkle)
        float a = stars[i][3] * (0.75f + 0.25f * sinf(tw * 1.3f + i * 1.7f));
        dl->AddCircleFilled(A(stars[i][0], stars[i][1]), stars[i][2] * k * 1.2f, IM_COL32(223, 234, 240, (int)(a * 255)));
    }
    dl->AddCircleFilled(A(1060, 132), 38 * k, IM_COL32(244, 230, 191, 235), 48);  // crescent moon
    dl->AddCircleFilled(A(1076, 120), 35 * k, IM_COL32(7, 19, 34, 255), 48);
    dl->AddRectFilledMultiColor(A(874, 150), A(886, 670), IM_COL32(242, 181, 68, 0), IM_COL32(242, 181, 68, 0),
                                IM_COL32(242, 181, 68, 60), IM_COL32(242, 181, 68, 60));  // the tower's light beam
    static const short far_[][3] = {{0, 40, 452}, {40, 80, 420}, {80, 150, 462}, {150, 175, 404}, {175, 190, 382},
                                    {190, 230, 404}, {230, 300, 470}, {300, 360, 432}, {360, 400, 410}, {400, 470, 482},
                                    {470, 520, 396}, {520, 600, 440}, {600, 650, 416}, {650, 720, 470}, {720, 790, 432},
                                    {790, 830, 406}, {830, 960, 462}, {960, 1000, 420}, {1000, 1070, 446},
                                    {1070, 1110, 402}, {1110, 1180, 432}, {1180, 1240, 456}, {1240, 1280, 426}};
    for (auto &b : far_) dl->AddRectFilled(A(b[0], b[2]), A(b[1], 720), IM_COL32(17, 48, 63, 255));
    for (int i = 0; i < 16; i++)  // a warm glow over the city, low right
        dl->AddCircleFilled(A(896, 640), (560 - i * 32) * k, IM_COL32(242, 181, 68, 5), 64);
    ImVec2 tower[] = {A(866, 660), A(866, 340), A(872, 312), A(876, 236), A(880, 150), A(884, 236), A(888, 312),
                      A(894, 340), A(894, 660)};
    dl->AddConcavePolyFilled(tower, 9, IM_COL32(10, 31, 45, 255));
    float ring_a = 0.75f + 0.25f * sinf(tw * 0.9f);
    dl->AddEllipse(A(880, 300), ImVec2(30 * k, 6 * k), IM_COL32(242, 181, 68, (int)(215 * ring_a)), 0, 40, 2 * k);
    dl->AddEllipse(A(880, 356), ImVec2(40 * k, 8 * k), IM_COL32(242, 181, 68, (int)(150 * ring_a)), 0, 40, 2 * k);
    dl->AddEllipse(A(880, 420), ImVec2(50 * k, 10 * k), IM_COL32(242, 181, 68, (int)(100 * ring_a)), 0, 48, 2 * k);
    dl->AddCircleFilled(A(880, 152), 4 * k, IM_COL32(255, 215, 122, 255));
    static const short mid[][3] = {{0, 90, 520}, {95, 60, 470}, {160, 110, 545}, {275, 70, 500}, {340, 120, 560},
                                   {465, 55, 455}, {525, 95, 530}, {625, 80, 495}, {710, 130, 560}, {935, 70, 480},
                                   {1010, 110, 540}, {1125, 60, 465}, {1190, 90, 515}};
    for (auto &b : mid) dl->AddRectFilled(A(b[0], b[2]), A(b[0] + b[1], 720), IM_COL32(10, 29, 43, 255));
    static const short win[][3] = {{16, 540, 8}, {40, 540, 4}, {64, 560, 7}, {108, 490, 9}, {130, 510, 5}, {108, 530, 6},
                                   {180, 565, 7}, {214, 565, 4}, {246, 585, 8}, {290, 520, 6}, {320, 540, 9},
                                   {360, 580, 5}, {400, 580, 8}, {436, 600, 6}, {478, 475, 9}, {500, 500, 6},
                                   {478, 525, 4}, {545, 550, 7}, {580, 570, 9}, {640, 515, 5}, {672, 535, 8},
                                   {640, 555, 7}, {730, 580, 6}, {770, 580, 9}, {810, 600, 5}, {950, 500, 8},
                                   {980, 520, 6}, {1030, 560, 9}, {1066, 580, 5}, {1096, 560, 7}, {1140, 485, 6},
                                   {1164, 505, 9}, {1206, 535, 7}, {1244, 555, 5}};
    for (auto &w : win) dl->AddRectFilled(A(w[0], w[1]), A(w[0] + 5, w[1] + 7), IM_COL32(242, 199, 102, w[2] * 26));
    static const short near_[][3] = {{0, 120, 640}, {120, 210, 615}, {210, 330, 650}, {330, 470, 625}, {470, 600, 660},
                                     {600, 760, 630}, {760, 900, 655}, {900, 1050, 620}, {1050, 1160, 645},
                                     {1160, 1280, 610}};
    for (auto &b : near_) dl->AddRectFilled(A(b[0], b[2]), A(b[1], 720), IM_COL32(4, 13, 22, 255));
    for (int i = 0; i < 11; i++)
        dl->AddCircleFilled(A(60 + i * 120.0f, i & 1 ? 696 : 690), 2 * k, IM_COL32(242, 181, 68, i & 1 ? 150 : 180));
}

// ---- first PLAY ever: the aspect ratio question, then how to open the in-game menu
static bool g_tip_test;  // (LARECOMP_LAUNCHER_SHOT=<frame>:tip / :aspect)
static bool begin_question(const char *id, float s) {
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.03f, 0.07f, 0.12f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.95f, 0.71f, 0.27f, 0.5f));
    bool open = ImGui::BeginPopupModal(id, NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    if (open) ImGui::SetWindowFontScale(1.25f);
    return open;
}
static bool aspect_prompt(float s) {
    bool done = false;
    if (begin_question("##aspect", s)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(TR("How would you like to play?", "Como você quer jogar?"));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", TR("You can change this any time in Settings.", "Dá para mudar isso quando quiser em Configurações."));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        float bw = 300 * s, gap = 20 * s;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 2 * bw - gap) * 0.5f);
        int pick = -1;
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(TR("Authentic 16:9", "16:9 original"), ImVec2(bw, 44 * s))) pick = ASPECT_ORIGINAL;
        ImGui::SameLine(0, gap);
        if (ImGui::Button(TR("Unlocked (fill the screen)", "Livre (tela toda)"), ImVec2(bw, 44 * s))) pick = ASPECT_UNLOCKED;
        if (pick >= 0) {
            g_settings.aspect = pick;
            g_settings.aspect_asked = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            done = true;
        }
        ImGui::EndPopup();
    }
    return done;
}
static bool menu_tip(float s) {
    bool go = false;
    if (begin_question("##menutip", s)) {
        // the controller's home button first (what players reach for: the PS button on a DualSense), the combo second
        static const char *const home_en[] = {"Home button", "Xbox button", "PlayStation button"};  // (bind_label_family)
        static const char *const home_pt[] = {"botão Home", "botão Xbox", "botão PlayStation"};
        static const char *const pad[] = {"Minus + Plus", "View + Menu", "Create + Options"};
        int fam = bind_label_family() % 3;
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(TR("ZA Recomp has its own menu for settings: F1 (or Minus+Plus on a controller).",
                                  "O ZA Recomp tem um menu próprio de configurações: F1 (ou Menos+Mais no controle)."));
        ImGui::Spacing();
        if (g_pt) ImGui::Text("Abra no jogo com F1 ou Esc, ou no controle com o %s (ou %s juntos).", home_pt[fam], pad[fam]);
        else ImGui::Text("Open it in game with F1 or Esc, or on a controller with the %s (or %s together).", home_en[fam], pad[fam]);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 180 * s) * 0.5f);
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button("OK", ImVec2(180 * s, 44 * s)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
            g_settings.menu_tip_seen = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            go = true;
        }
        ImGui::EndPopup();
    }
    return go;
}

// Controllers, as in the in-game menu (runtime/ui/menu.mm): read straight from SDL, confirm / back as the Controls
// tab's Confirm button says (pad_nav.h: the game's layout, or the controller's convention); d-pad / left stick move;
// L / R change pages. (ImGui's own SDL gamepad handling is switched off.)
static void launcher_pad_open_all(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}
// true when the event was a controller event (handled here)
static bool launcher_pad_event(const SDL_Event &e) {
    ImGuiIO &io = ImGui::GetIO();
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        SDL_OpenGamepad(e.gdevice.which);
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        ImGuiKey k = ui_pad_key(e.gbutton.which, e.gbutton.button, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = e.gaxis.value / 32767.0f;
        // the left stick also works as the d-pad (ImGui moves between items with the d-pad only): pressed past half
        // way, released under 0.3
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            static bool held[4];  // left, right, up, down
            bool x = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
            int neg = x ? 0 : 2, pos = neg + 1;
            const ImGuiKey keys[4] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadUp,
                                      ImGuiKey_GamepadDpadDown};
            for (int i : {neg, pos}) {
                float m = i == neg ? -v : v;
                bool now = held[i] ? m > 0.3f : m > 0.5f;
                if (now != held[i]) { held[i] = now; io.AddKeyEvent(keys[i], now); }
            }
        }
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, v > 0.3f, v > 0.3f ? v : 0);
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, v > 0.3f, v > 0.3f ? v : 0);
        }
        return true;
    }
    default: return false;
    }
}

// Open a folder in Finder / Explorer (relative paths are in the data root, the working directory)
static void reveal_folder(const std::string &dir) {
    std::string path = dir;
    bool abs = !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
    if (!abs) path = std::string(rt_data_root()) + "/" + path;
    std::string url = "file://";
    if (path[0] != '/') url += "/";  // (Windows: file:///C:/...)
    for (char ch : path) {
        if (ch == '\\') ch = '/';
        if (ch == ' ') url += "%20";
        else if (ch == '%') url += "%25";
        else if (ch == '#') url += "%23";
        else url += ch;
    }
    SDL_OpenURL(url.c_str());
}

// Settings' first tab, on the start screen only: the set-up game files (size, delete) and the player's own Switch
// fonts (import / back to the substitute)
static SDL_Window *g_launcher_win;
static bool g_files_deleted;  // set here: the start screen goes back to setting the game files up
static void prelaunch_tab(float sc) {
    const ImVec4 gold(0.95f, 0.71f, 0.27f, 1);
    auto heading = [&](const char *t) {
        ImGui::PushFont(g_font_menu);
        ImGui::TextColored(gold, "%s", t);
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
    };
    heading(TR("Game files", "Arquivos do jogo"));
    static double gb = -1;
    if (gb < 0) gb = rt_game_files_size(g_root.c_str()) / 1e9;
    if (ImGui::Button(TR("Open the folder", "Abrir a pasta"))) reveal_folder(g_root);
    ImGui::SameLine();
    static bool shot_del = getenv("LARECOMP_LAUNCHER_SHOT") && strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":askdelete");
    if (ImGui::Button(TR("Delete game files", "Apagar arquivos do jogo")) || shot_del)
        ImGui::OpenPopup("##delfiles"), shot_del = false;  // (test: :askdelete)
    if (ImGui::BeginPopupModal("##delfiles", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushFont(g_font_menu);
        ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "%s", TR("Are you sure?", "Tem certeza?"));
        ImGui::PopFont();
        ImGui::Text(TR("This deletes ZA Recomp's copy of the game files (%.1f GB).",
                       "Isso apaga a cópia dos arquivos do jogo do ZA Recomp (%.1f GB)."), gb);
        ImGui::TextDisabled("%s", TR("Saves, mods and settings are kept. Your own game files aren't touched.",
                                     "Saves, mods e configurações ficam. Os seus próprios arquivos do jogo não são tocados."));
        if (rt_dev_mode()) {  // (a source checkout: the files are the project's own extracted/)
            bool abs = !g_root.empty() && (g_root[0] == '/' || (g_root.size() > 1 && g_root[1] == ':'));
            std::string where = abs ? g_root : std::string(rt_data_root()) + "/" + g_root;
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Dev checkout: this is %s", where.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button(TR("Delete", "Apagar"), ImVec2(220 * sc, 0))) {
            bool ok = rt_remove_game_files(g_root.c_str());  // (runtime/paths.c: only what the extraction made)
            set_job(ok ? "Game files deleted." : "Could not delete the game files (not in ZA Recomp's data folder?).", !ok);
            g_files_deleted = true, gb = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(TR("Cancel", "Cancelar"), ImVec2(220 * sc, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    heading(TR("Game font", "Fonte do jogo"));
    import_fonts();
    bool fs = have_font(FONT_STD, 4), fe = have_font(FONT_EXT, 2);
    ImGui::Text(TR("Text: %s    Button icons: %s", "Texto: %s    Ícones dos botões: %s"),
                fs ? TR("Nintendo font (imported)", "fonte da Nintendo (importada)") : TR("substitute", "substituta"),
                fe ? TR("imported", "importados") : TR("substitute", "substitutos"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", TR("Fonts aren't included. For the original look, import your own copies of: "
                                 "nintendo_udsg-r_std_003.bfttf (text) and nintendo_ext_003.bfttf (button icons). "
                                 ".ttf versions with the same names work too.",
                                 "As fontes não vêm junto. Para o visual original, importe as suas próprias cópias de: "
                                 "nintendo_udsg-r_std_003.bfttf (texto) e nintendo_ext_003.bfttf (ícones dos botões). "
                                 "Versões .ttf com os mesmos nomes também servem."));
    ImGui::PopTextWrapPos();
    if (!g_font_msg.empty()) ImGui::Text("%s", g_font_msg.c_str());
    static const SDL_DialogFileFilter font_f[] = {{"Switch fonts", "bfttf;ttf"}};
    if (ImGui::Button(TR("Import fonts", "Importar fontes"))) SDL_ShowOpenFileDialog(picked_fonts, NULL, g_launcher_win, font_f, 1, NULL, true);
    if (fs || fe) {
        ImGui::SameLine();
        if (ImGui::Button(TR("Use substitute font", "Usar fonte substituta"))) remove_fonts();
    }
}

// ---- the pages
enum Tab { TAB_PLAY, TAB_MODS, TAB_SAVES, TAB_SETTINGS, TAB_ABOUT, N_TABS };
static int g_tab = TAB_PLAY;
static bool g_changing_folder;  // Play page: "Change game folder" pressed on a set-up copy
static bool g_focus_play;       // put the controller's cursor on PLAY (opening, back from a page)

// a status line's kind for the bottom bar
struct Status { ImU32 dot; std::string text; };

static void page_title(ImDrawList *dl, float x, float y, const char *kicker, const char *title) {
    if (kicker && *kicker) text(dl, F_SMALL, 13, x, y, C_TEAL, kicker), y += 24;
    text(dl, F_H1, 46, x, y, C_TEXT, title);
}

// chips under the Play page's title: version, mods on, the save's date
static float chip(ImDrawList *dl, float x, float y, int ic, const std::string &t) {
    float tw = text_size(F_BODY, 14, t.c_str()).x, w = tw + 30 + 22, h = 34;
    dl->AddRectFilled(P(x, y), P(x + w, y + h), IM_COL32(255, 255, 255, 18), h / 2 * g_s);
    dl->AddRect(P(x, y), P(x + w, y + h), IM_COL32(255, 255, 255, 32), h / 2 * g_s, 0, 1.2f * g_s);
    icon(dl, ic, x + 22, y + h / 2, 16, C_TEAL);
    text(dl, F_BODY, 14, x + 36, y + (h - text_size(F_BODY, 14, t.c_str()).y) / 2, C_TEXT, t.c_str());
    return w;
}

static int mods_on_count(void) {
    int n = 0;
    for (auto &m : g_mods) n += m.on;
    return n;
}

// ------------------------------------------------------------------ "Your team" (the Play page, from the player's save)
// The party read straight out of save/save_0/main (read only), re-implemented from the format as PKHeX documents it:
// the file XORed with a 0x7F-byte pad (its last 32 bytes a SHA-256, not checked), then blocks of u32 key + type
// (+ size) + data, each XORed by a xorshift32 stream seeded by its key. The party is block 0x3AA1A9AD: 6 slots of
// 0x1E0, each a Gen 8 Pokémon (0x148 bytes LCG-encrypted from the encryption constant and shuffled in 4 blocks of
// 0x50, then the party stats, level at 0x148).
struct PartyMon { int species, form, level; bool egg, shiny; std::string name; };
static std::vector<PartyMon> g_party;
static time_t g_party_time = -1;  // (the save's time it was read for: read again when saves_scan sees a new one)

struct SaveXs {  // the blocks' xorshift32 stream: bytes LSB first, the state advanced popcount(key) times first
    uint32_t s;
    int c = 0;
    static uint32_t adv(uint32_t s) { s ^= s << 2; s ^= s >> 15; s ^= s << 13; return s; }
    explicit SaveXs(uint32_t key) : s(key) { for (int i = __builtin_popcount(key); i > 0; i--) s = adv(s); }
    uint8_t next(void) {
        uint8_t r = (uint8_t)(s >> (c * 8));
        if (c == 3) s = adv(s), c = 0;
        else c++;
        return r;
    }
    uint32_t next32(void) { uint32_t v = next(); v |= next() << 8; v |= next() << 16; return v | (uint32_t)next() << 24; }
};

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool party_block(std::vector<uint8_t> &p, std::vector<uint8_t> &out) {
    static const uint8_t pad[0x7F] = {
        0xA0, 0x92, 0xD1, 0x06, 0x07, 0xDB, 0x32, 0xA1, 0xAE, 0x01, 0xF5, 0xC5, 0x1E, 0x84, 0x4F, 0xE3, 0x53, 0xCA, 0x37,
        0xF4, 0xA7, 0xB0, 0x4D, 0xA0, 0x18, 0xB7, 0xC2, 0x97, 0xDA, 0x5F, 0x53, 0x2B, 0x75, 0xFA, 0x48, 0x16, 0xF8, 0xD4,
        0x8A, 0x6F, 0x61, 0x05, 0xF4, 0xE2, 0xFD, 0x04, 0xB5, 0xA3, 0x0F, 0xFC, 0x44, 0x92, 0xCB, 0x32, 0xE6, 0x1B, 0xB9,
        0xB1, 0x2E, 0x01, 0xB0, 0x56, 0x53, 0x36, 0xD2, 0xD1, 0x50, 0x3D, 0xDE, 0x5B, 0x2E, 0x0E, 0x52, 0xFD, 0xDF, 0x2F,
        0x7B, 0xCA, 0x63, 0x50, 0xA4, 0x67, 0x5D, 0x23, 0x17, 0xC0, 0x52, 0xE1, 0xA6, 0x30, 0x7C, 0x2B, 0xB6, 0x70, 0x36,
        0x5B, 0x2A, 0x27, 0x69, 0x33, 0xF5, 0x63, 0x7B, 0x36, 0x3F, 0x26, 0x9B, 0xA3, 0xED, 0x7A, 0x53, 0x00, 0xA4, 0x48,
        0xB3, 0x50, 0x9E, 0x14, 0xA0, 0x52, 0xDE, 0x7E, 0x10, 0x2B, 0x1B, 0x77, 0x6E};
    static const int width[18] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 2, 4, 8, 1, 2, 4, 8, 4, 8};  // (types 8..17: numbers)
    if (p.size() < 64) return false;
    size_t n = p.size() - 32;
    for (size_t i = 0; i < n; i++) p[i] ^= pad[i % 0x7F];
    for (size_t o = 0; o + 5 <= n;) {
        uint32_t key = rd32(&p[o]);
        o += 4;
        SaveXs x(key);
        uint8_t t = p[o++] ^ x.next();
        uint64_t size;
        if (t == 1 || t == 2) continue;  // (a bool: no data)
        if (t == 4) {  // an object
            if (o + 4 > n) return false;
            size = rd32(&p[o]) ^ x.next32(), o += 4;
        } else if (t == 5) {  // an array of numbers
            if (o + 5 > n) return false;
            uint32_t cnt = rd32(&p[o]) ^ x.next32();
            uint8_t sub = p[o + 4] ^ x.next();
            o += 5;
            if (sub >= 18 || !width[sub]) return false;
            size = (uint64_t)cnt * width[sub];
        } else if (t < 18 && width[t]) size = width[t];
        else return false;  // (a type this doesn't know: the rest can't be walked)
        if (o + size > n) return false;
        if (key == 0x3AA1A9AD) {
            out.resize(size);
            for (uint64_t i = 0; i < size; i++) out[i] = p[o + i] ^ x.next();
            return true;
        }
        o += size;
    }
    return false;
}

static void party_decrypt(uint8_t *d) {  // one slot's first 0x158 bytes, in place
    static const uint8_t pos[96] = {0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 1, 0, 2, 3, 1, 0,
                                    3, 2, 2, 0, 1, 3, 3, 0, 1, 2, 2, 0, 3, 1, 3, 0, 2, 1, 1, 2, 0, 3, 1, 3, 0, 2, 2, 1, 0, 3,
                                    3, 1, 0, 2, 2, 3, 0, 1, 3, 2, 0, 1, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3,
                                    1, 0, 3, 2, 1, 0};
    uint32_t ec = rd32(d), seed = ec;
    for (int i = 8; i < 0x158; i += 2) {
        if (i == 0x148) seed = ec;  // (the party stats: their own run from the same seed)
        seed = 0x41C64E6D * seed + 0x6073;
        uint16_t v = (uint16_t)(d[i] | d[i + 1] << 8) ^ (uint16_t)(seed >> 16);
        d[i] = (uint8_t)v, d[i + 1] = (uint8_t)(v >> 8);
    }
    uint8_t src[0x140];
    memcpy(src, d + 8, sizeof src);
    int sv = (int)((ec >> 13) & 31) % 24;
    for (int b = 0; b < 4; b++) memcpy(d + 8 + 0x50 * b, src + 0x50 * pos[sv * 4 + b], 0x50);
}

static void party_load(void) {
    g_party.clear();
    g_party_time = g_save_time;
    FILE *f = fopen("save/save_0/main", "rb");
    if (!f) return;
    std::vector<uint8_t> p, blk;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < (64 << 20)) {
        p.resize(size);
        if (fread(p.data(), 1, size, f) != (size_t)size) p.clear();
    }
    fclose(f);
    if (!party_block(p, blk)) {
        if (!p.empty()) fprintf(stderr, "[launcher] team: no party in save/save_0/main\n");
        return;
    }
    for (size_t i = 0; i + 0x158 <= blk.size() && i < 6 * 0x1E0; i += 0x1E0) {
        uint8_t d[0x158];
        memcpy(d, &blk[i], sizeof d);
        if (!rd32(d)) continue;  // (an empty slot)
        party_decrypt(d);
        PartyMon m;
        m.species = d[8] | d[9] << 8, m.form = d[0x24], m.level = d[0x148];
        m.egg = rd32(d + 0x8C) >> 30 & 1;
        uint32_t pid = rd32(d + 0x1C), id = rd32(d + 0x0C);
        m.shiny = ((pid >> 16) ^ (pid & 0xFFFF) ^ (id >> 16) ^ (id & 0xFFFF)) < 16;
        for (int k = 0; k < 13; k++) {  // the nickname (the species' name when not nicknamed), UTF-16 -> UTF-8
            unsigned c = d[0x58 + 2 * k] | d[0x59 + 2 * k] << 8;
            if (!c) break;
            if (c < 0x80) m.name += (char)c;
            else if (c < 0x800) m.name += (char)(0xC0 | c >> 6), m.name += (char)(0x80 | (c & 63));
            else m.name += (char)(0xE0 | c >> 12), m.name += (char)(0x80 | (c >> 6 & 63)), m.name += (char)(0x80 | (c & 63));
        }
        if (m.species <= 0 || m.species > 1025 || m.level > 100) continue;  // (not a Pokémon: a format this misreads)
        g_party.push_back(m);
    }
    fprintf(stderr, "[launcher] team: %d in the party\n", (int)g_party.size());
}

// The icons: the game's own (pm<model>_<form>_00_00_<0|1>, 0 normal / 1 shiny), drawn from the player's romfs.bin into
// cache/party_icons/ by zaprompts --party-icons (tools/za/prompt_textures.py: ~10 s, in the background, once; never
// shipped). The model numbers are the National Dex ones up to Gen 5 and the game's own after that: Kalos in X / Y's
// development order, the later ones in theirs (matched by eye to the game's icons).
#define PARTY_ICONS "cache/party_icons"
static int party_model(int species) {
    static const short kalos[72] = {  // 650 Chespin ... 721 Volcanion
        720, 721, 722, 717, 718, 719, 723, 724, 725, 711, 712, 753, 754, 755, 706, 707, 708, 704, 705, 713, 714, 715, 728, 729,
        752, 730, 701, 733, 734, 744, 745, 746, 758, 759, 742, 743, 726, 727, 747, 748, 709, 710, 756, 757, 731, 732, 737, 738,
        739, 740, 767, 761, 741, 766, 763, 764, 765, 760, 702, 703, 735, 736, 749, 751, 716, 762, 768, 769, 770, 772, 774, 773};
    static const short later[][2] = {
        {739, 859}, {740, 860}, {767, 866}, {768, 867}, {769, 822}, {770, 823}, {778, 819}, {780, 856}, {801, 882},
        {802, 883}, {807, 888}, {808, 891}, {809, 892}, {821, 918}, {822, 919}, {823, 920}, {827, 901}, {828, 902},
        {848, 921}, {849, 922}, {850, 923}, {851, 923}, {852, 964}, {853, 965}, {876, 942}, {877, 941}, {920, 1002},
        {926, 1074}, {927, 1075}, {931, 1064}, {932, 1067}, {933, 1068}, {934, 1069}, {935, 1107}, {936, 1108},
        {937, 1109}, {942, 1076}, {943, 1077}, {951, 1042}, {952, 1043}, {957, 1104}, {958, 1105}, {959, 1106},
        {965, 1072}, {966, 1073}, {967, 1057}, {969, 1070}, {970, 1071}, {971, 1028}, {972, 1029}, {973, 1065},
        {977, 1035}, {978, 1056}, {979, 1114}, {996, 1053}, {997, 1054}, {998, 1055}, {999, 1080}, {1000, 1081}};
    if (species < 650) return species;
    if (species <= 721) return kalos[species - 650];
    for (auto &l : later)
        if (l[0] == species) return l[1];
    return -1;
}

static PlatProc g_party_icons_proc;
static void party_icons_kick(void) {  // made when missing (3 tries: cache/party_icons.tried)
    static bool asked;  // (once a start)
    char tool[1100];
    if (asked) return;
    asked = true;
    // (the game's data: the RomFS image the setup makes, else a romfs/ folder extracted by the player)
    std::string romfs = file_exists(g_root + "/romfs.bin") ? g_root + "/romfs.bin" : g_root + "/romfs";
    if (file_exists(PARTY_ICONS "/.made") || !file_exists(romfs + (is_dir(romfs) ? "/arc/data.trpfs" : "")) ||
        !rt_resource("zaprompts/zaprompts" PLAT_EXE_SUFFIX, "build/helper/zaprompts/zaprompts" PLAT_EXE_SUFFIX, tool, sizeof tool))
        return;
    int tries = 0;
    if (FILE *f = fopen(PARTY_ICONS ".tried", "rb")) tries = fscanf(f, "%d", &tries) == 1 ? tries : 0, fclose(f);
    if (tries >= 3) return;
    mkdir("cache", 0755);
    if (FILE *f = fopen(PARTY_ICONS ".tried", "wb")) fprintf(f, "%d", tries + 1), fclose(f);
    const char *argv[] = {tool, "--romfs", romfs.c_str(), "--party-icons", PARTY_ICONS, NULL};
    g_party_icons_proc = plat_spawn(argv, PARTY_ICONS ".log", false);
    fprintf(stderr, "[launcher] team icons: %s\n", g_party_icons_proc ? "being made (" PARTY_ICONS ".log)" : "zaprompts didn't start");
}

#ifdef __APPLE__
static uint64_t party_texture(const void *rgba, int w, int h) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
    return (uint64_t)(__bridge_retained void *)t;
}
#else
static uint64_t party_texture(const void *rgba, int w, int h) { return vk_imgui_texture_rgba(rgba, w, h); }
#endif

static std::vector<std::pair<std::string, uint64_t>> g_party_icons;  // (mon -> texture, 0 = none: emptied when made)
static uint64_t party_icon(const PartyMon &m) {  // 0: none (yet)
    int model = m.egg ? 0 : party_model(m.species);
    if (model < 0) return 0;
    char name[64];
    snprintf(name, sizeof name, "%d/%d/%d", model, m.form, m.shiny);
    std::string key = name;
    for (auto &l : g_party_icons)
        if (l.first == key) return l.second;
    std::string path;
    // the form's own icon (a form number, or 11 + it for species whose base form is 11: Vivillon, Furfrou...), else
    // the base form's; the shiny one, else the normal one
    for (int sh = m.shiny && !m.egg ? 1 : 0; sh >= 0 && path.empty(); sh--)
        for (int form : {m.egg ? 0 : m.form, m.egg ? 0 : 11 + m.form, 0, 11}) {
            snprintf(name, sizeof name, PARTY_ICONS "/pm%04d_%02d_00_00_%d.png", model, form, sh);
            if (file_exists(name)) { path = name; break; }
        }
    uint64_t tex = 0;
    if (SDL_Surface *s0 = path.empty() ? NULL : SDL_LoadPNG(path.c_str())) {
        if (SDL_Surface *s = SDL_ConvertSurface(s0, SDL_PIXELFORMAT_ABGR8888)) {  // (R, G, B, A bytes)
            std::vector<uint8_t> px((size_t)s->w * s->h * 4);
            for (int y = 0; y < s->h; y++) memcpy(&px[(size_t)y * s->w * 4], (uint8_t *)s->pixels + (size_t)y * s->pitch, (size_t)s->w * 4);
            tex = party_texture(px.data(), s->w, s->h);
            SDL_DestroySurface(s);
        }
        SDL_DestroySurface(s0);
    }
    g_party_icons.push_back({key, tex});
    return tex;
}

// the panel: up to six cards (icon, name, level) - drawn only with a save that has a party
static float party_panel(ImDrawList *dl, float x, float y, float w) {
    if (g_party_time != g_save_time) party_load();
    if (g_party.empty()) return 0;
    if (g_party_icons_proc && plat_proc_wait(g_party_icons_proc, false) >= 0)
        g_party_icons_proc = 0, g_party_icons.clear();  // (made: looked for again)
    text(dl, F_LABEL, 13, x, y, C_GOLD_HI, TR("YOUR TEAM", "SEU TIME"));
    y += 26;
    float gap = 10, cw = (w - gap * 5) / 6, ch = cw + 44;
    for (size_t i = 0; i < g_party.size(); i++) {
        const PartyMon &m = g_party[i];
        float cx = x + i * (cw + gap);
        panel(dl, cx, y, cw, ch, IM_COL32(8, 19, 31, 200), m.shiny ? IM_COL32(255, 215, 122, 110) : C_LINE, 14);
        float is = cw - 16;
        if (uint64_t tex = party_icon(m))
            dl->AddImage((ImTextureID)tex, P(cx + 8, y + 6), P(cx + 8 + is, y + 6 + is));
        else {  // (no icon: a ball drawn in its place)
            ImVec2 c = P(cx + cw / 2, y + 6 + is / 2);
            float r = is * 0.3f * g_s;
            dl->AddCircleFilled(c, r, IM_COL32(255, 255, 255, 30), 32);
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(255, 255, 255, 60), 2 * g_s);
            dl->AddCircle(c, r * 0.3f, IM_COL32(255, 255, 255, 60), 16, 2 * g_s);
        }
        if (m.shiny) text(dl, F_LABEL, 13, cx + cw - 18, y + 6, C_GOLD_HI, "\xe2\x98\x85");
        const char *nm = m.egg ? TR("Egg", "Ovo") : m.name.c_str();
        float nw = fminf(text_size(F_BODY, 13, nm).x, cw - 8);
        dl->PushClipRect(P(cx + 4, y), P(cx + cw - 4, y + ch), true);
        text(dl, F_BODY, 13, cx + (cw - nw) / 2, y + cw - 8, C_TEXT, nm);
        dl->PopClipRect();
        if (!m.egg) {
            char lv[24];
            snprintf(lv, sizeof lv, TR("Lv. %d", "Nv. %d"), m.level);
            text(dl, F_LABEL, 13, cx + (cw - text_size(F_LABEL, 13, lv).x) / 2, y + cw + 12, C_TEAL, lv);
        }
    }
    return 26 + ch;
}

extern "C" void launcher_run(const char *root) {
    g_root = root;
    g_start_screen = 1;
    settings_load();
    lang_update();
    static bool update_checked;  // (once per launch, not again after a "Title screen" reboot)
    if (!update_checked && !getenv("LARECOMP_NO_UPDATE_CHECK")) {
        update_checked = true;
        pthread_t t;
        if (pthread_create(&t, NULL, update_thread, NULL) == 0) pthread_detach(t);
    }
    ui_window_init();
    SDL_Window *win = ui_window();
    g_launcher_win = win;
    input_mouse_release(win);  // (back from the game: its mouse camera held the cursor)
#ifdef __APPLE__
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
#endif

    IMGUI_CHECKVERSION();
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // the in-game menu's, when back here after a reboot
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    // fonts belong to this context: back here via "Title screen" (in-place reboot) the previous run's are gone with
    // its context, so load them again (a stale pointer here aborted in PushFont - user, 0.1 RC)
    g_font_small = g_font_menu = g_font_title = NULL;
    F_BODY = F_SMALL = F_LABEL = F_H2 = F_H1 = F_BTN = NULL;
    // Baloo 2 SemiBold (OFL, shipped: third_party/fonts/Baloo2) - rounded, game-like; it runs small, hence the sizes
    char baloo[1024];
    std::string own = rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo) ? baloo : "";
    const char *fonts[] = {getenv("LARECOMP_LAUNCHER_FONT") ? getenv("LARECOMP_LAUNCHER_FONT") : "",  // (test)
                           own.c_str(), "fonts/nintendo_udsgr_std_003.ttf", "/System/Library/Fonts/Avenir Next.ttc",
                           "/System/Library/Fonts/Helvetica.ttc", "C:/Windows/Fonts/segoeui.ttf"};
    // Latin-1 (Portuguese's accents) + ‹ › → ✓ ▸ — …
    static const ImWchar ranges[] = {0x20, 0xFF, 0x2014, 0x2014, 0x2026, 0x2026, 0x2039, 0x203A, 0x2192, 0x2192,
                                     0x2713, 0x2713, 0x25B8, 0x25B8, 0};
    for (const char *p : fonts) {
        if (g_font_menu || !*p || !file_exists(p)) continue;
        bool b2 = p == own.c_str() || getenv("LARECOMP_LAUNCHER_FONT");
        g_font_small = io.Fonts->AddFontFromFileTTF(p, 30 * (b2 ? 1.75f : 1), NULL, ranges);
        g_font_menu = io.Fonts->AddFontFromFileTTF(p, 52 * (b2 ? 1.85f : 1), NULL, ranges);
        // the start screen's own sizes, built at 1.5x the 720p layout (sharp up to about 1080p)
        g_fb = b2 ? 1.5f : 1;
        auto add = [&](float units) { return io.Fonts->AddFontFromFileTTF(p, units * g_fb * 1.5f, NULL, ranges); };
        F_SMALL = add(13), F_BODY = add(15), F_LABEL = add(17), F_H2 = add(26), F_BTN = add(28), F_H1 = add(56);
    }
    if (!g_font_menu) {
        g_font_small = g_font_menu = io.Fonts->AddFontDefault();
        F_BODY = F_SMALL = F_LABEL = F_H2 = F_H1 = F_BTN = g_font_small;
    }
    g_font_title = g_font_menu;
    ImGuiStyle &st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.08f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.14f);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.71f, 0.27f, 1);
    st.Colors[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.07f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.95f, 0.71f, 0.27f, 0.35f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.71f, 0.27f, 0.55f);
    st.FrameRounding = st.WindowRounding = 10;
#ifdef __APPLE__
    ImGui_ImplSDL3_InitForMetal(win);
    ImGui_ImplMetal_Init(dev);
#else
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();
#endif
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, NULL, 0);  // (controllers: launcher_pad_event)
    launcher_pad_open_all();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    std::string status = check_game_files(g_root);
    if (status.empty() && prompts_wanted()) make_prompts(false);  // (set up before the icons existed: made now, aside)
    g_tab = TAB_PLAY, g_focus_play = true, g_changing_folder = false;
    g_mods_changed = true, g_save_scanned = false;
    bool play = false;
    // LARECOMP_LAUNCHER_SHOT=<frame>[:<page>] (test): save build/launcher.png at that frame and exit; page: mods /
    // saves / about / settings[:<tab>] / askdelete / setup / extracting / fail<1-8> / tip / aspect; :pt or :en after it
    // picks the language
    long shot_at = getenv("LARECOMP_LAUNCHER_SHOT") ? atol(getenv("LARECOMP_LAUNCHER_SHOT")) : 0, frame = 0;
    bool test_busy = false;
    if (shot_at) {
#ifdef __APPLE__
        layer.framebufferOnly = NO;
#endif
        const char *spec = getenv("LARECOMP_LAUNCHER_SHOT"), *pg = strchr(spec, ':');
        if (strstr(spec, ":pt")) g_settings.ui_lang = UI_LANG_PT, lang_update();
        if (strstr(spec, ":en")) g_settings.ui_lang = UI_LANG_EN, lang_update();
        if (pg && !strncmp(pg + 1, "mods", 4)) g_tab = TAB_MODS;
        if (pg && !strncmp(pg + 1, "saves", 5)) g_tab = TAB_SAVES;
        if (pg && !strncmp(pg + 1, "about", 5)) g_tab = TAB_ABOUT;
        if (pg && (!strncmp(pg + 1, "settings", 8) || !strcmp(pg + 1, "askdelete"))) g_tab = TAB_SETTINGS, menu_tabs_focus(true);
        if (pg && !strncmp(pg + 1, "settings", 8) && pg[9] == ':' && isdigit((unsigned char)pg[10])) g_tab_request_ext = atoi(pg + 10);
        if (pg && !strncmp(pg + 1, "setup", 5)) g_changing_folder = true;
        if (pg && !strncmp(pg + 1, "extracting", 10)) test_busy = true;
        if (pg && !strncmp(pg + 1, "ready", 5)) status.clear();  // (the ready page without a built game library)
        if (pg && !strncmp(pg + 1, "fail", 4)) g_changing_folder = true, set_failure(atoi(pg + 5));  // (the failure messages)
        if (pg && !strncmp(pg + 1, "tip", 3)) g_settings.menu_tip_seen = false, g_tip_test = true;
        if (pg && !strncmp(pg + 1, "aspect", 6)) g_settings.aspect_asked = g_settings.menu_tip_seen = false, g_tip_test = true;
    }

    static int runs;  // (test: =once - only the first time, so a "Title screen" reboot lands on the start screen)
    const char *ap = getenv("LARECOMP_LAUNCHER_AUTOPLAY");
    if (ap && status.empty() && !(runs && !strcmp(ap, "once"))) play = true;  // (test: PLAY at once)
    runs++;
    if (const char *t = getenv("LARECOMP_LAUNCHER_EXTRACT")) {  // (test) <folder>: set the game files up, log, exit
        g_src = t;
        g_busy = true;
        extract_thread(NULL);
        fprintf(stderr, "[launcher] setup: %s -> %s\n%s\n", g_job.c_str(),
                check_game_files(g_root).empty() ? "READY" : "NOT READY", g_job_hint.c_str());
        exit(0);
    }
    if (const char *t = getenv("LARECOMP_LAUNCHER_SETUP")) {  // (test) <folder>: setup started on the start screen
        g_src = t;
        g_busy = true;
        pthread_t th;
        pthread_create(&th, NULL, extract_thread, NULL);
        pthread_detach(th);
    }
    if (const char *t = getenv("LARECOMP_LAUNCHER_IMPORT_SAVE")) {  // (test) <folder>: Import save, logged
        g_save_pick = t;
        import_save();
        fprintf(stderr, "[launcher] import save: %s\n", g_save_msg.c_str());
    }
    if (const char *t = getenv("LARECOMP_MOD_INSTALL")) {  // (test) <archive or folder>: install it, log, exit
        mod_install(t);
        while (g_mod_busy) SDL_Delay(50);
        fprintf(stderr, "[launcher] mod install: %s (%s)\n", g_mod_msg.c_str(), g_mod_failed ? "failed" : "ok");
        exit(g_mod_failed);
    }
    if (getenv("LARECOMP_LAUNCHER_DELETE")) {  // (test) what DELETE GAME FILES does, logged; then the start screen
        double gb = rt_game_files_size(g_root.c_str()) / 1e9;
        bool ok = rt_remove_game_files(g_root.c_str());
        status = check_game_files(g_root);
        fprintf(stderr, "[launcher] delete %.1f GB: %s -> %s\n", gb, ok ? "ok" : "refused", status.c_str());
    }
    static bool typed_before;
    while (!play) {
        menu_apply_window_requests(win);  // (the Video tab's Fullscreen)
        if (g_files_deleted) {  // (Settings > Prelaunch deleted them)
            g_files_deleted = false;
            status = check_game_files(g_root);
            if (!status.empty()) { bind_capture_cancel(); settings_save(); g_tab = TAB_PLAY; }
        }
        if (g_mods_changed.exchange(false)) {  // (an install finished, or a mod was deleted)
            pthread_mutex_lock(&g_mod_lock);
            std::string fresh;
            fresh.swap(g_mod_installed);
            pthread_mutex_unlock(&g_mod_lock);
            if (!fresh.empty()) {  // on and first: a mod just installed wins over the older ones
                std::string order = fresh + (g_settings.mods_order[0] ? "|" : "") + g_settings.mods_order;
                snprintf(g_settings.mods_order, sizeof g_settings.mods_order, "%s", order.c_str());
            }
            mods_reload();
            if (!fresh.empty()) mods_store();
        }
        pthread_mutex_lock(&g_lock);
        bool busy = g_busy;
        pthread_mutex_unlock(&g_lock);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (g_tab == TAB_SETTINGS && bind_capture_event(&e)) continue;  // (waiting for a new binding: controls_ui.mm)
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                // a file or folder dropped on the window: a mod on the Mods page, a save on the Saves page, the game's
                // folder on the Play page while it isn't set up
                std::string path = e.drop.data;
                if (g_tab == TAB_MODS) mod_install(path);
                else if (g_tab == TAB_SAVES) { pthread_mutex_lock(&g_lock); g_save_pick = path; pthread_mutex_unlock(&g_lock); }
                else if (g_tab == TAB_PLAY && !busy && (!status.empty() || g_changing_folder)) {
                    pthread_mutex_lock(&g_lock);
                    g_src = is_dir(path) ? path : path.substr(0, path.find_last_of("/\\"));
                    pthread_mutex_unlock(&g_lock);
                }
                continue;
            }
            if (!launcher_pad_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) exit(0);
        }
        // setup finished: check again
        static bool was_busy;
        if (was_busy && !busy) {
            status = check_game_files(g_root);
            if (status.empty()) g_changing_folder = false, g_focus_play = true;
        }
        was_busy = busy;
        busy |= test_busy;
        // minimised, the window has no size: DisplaySize 0 made g_s 0 and every button zero-sized, and ImGui's assert
        // in InvisibleButton then stopped the start screen in the middle of a game code build (#39, #53). The build
        // and setup go on in their threads; nothing to draw until the window is back
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }

#ifdef __APPLE__
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        layer.drawableSize = CGSizeMake(w, h);
        @autoreleasepool {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) { SDL_Delay(16); continue; }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.02, 0.04, 0.09, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_NewFrame(rp);
#else
        {
            // one frame per refresh of the window's display: the swapchain presents without waiting (mailbox, for the
            // game's own pacing: device.c), so the start screen alone would draw as fast as the GPU goes
            static uint64_t next_frame;
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
            uint64_t period = (uint64_t)(1e9 / (dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f)), now = plat_now_ns();
            bool on_time = next_frame > now && next_frame - now <= period;  // (else late, or the rate changed: start over)
            if (on_time) plat_sleep_until_ns(next_frame);
            next_frame = (on_time ? next_frame : now) + period;
            static const float clear_col[4] = {0.02f, 0.04f, 0.09f, 1};
            if (!vk_frame_begin(clear_col)) { SDL_Delay(16); continue; }
            ImGui_ImplVulkan_NewFrame();
#endif
            ImGui_ImplSDL3_NewFrame();
            // the backend (manual mode, no gamepads of its own) clears HasGamepad every frame, and ImGui then drops the
            // controller presses launcher_pad_event fed in and turns gamepad navigation off: set it again while a
            // controller is connected
            if (SDL_HasGamepad()) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            ImGui::NewFrame();

            ImVec2 ds = io.DisplaySize;
            float s = ds.y / 720.0f;
            g_s = s, g_o = ImVec2(0, 0), W_ = ds.x / s, H_ = 720;
            io.FontGlobalScale = s * 0.5f;  // (the in-game menu's tabs and the questions: fonts built for ~2x)
            bool typing = typed_before || ui_text_busy();
            bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

            ImDrawList *bg = ImGui::GetBackgroundDrawList();
            draw_art(bg, ImVec2(0, 0), ds);
            if (g_tab == TAB_PLAY) {  // the left side darkened under the text, the city open on the right
                float a = W_ * 0.42f, b = W_ * 0.72f;
                bg->AddRectFilledMultiColor(P(0, 0), P(a, H_), IM_COL32(4, 10, 19, 235), IM_COL32(4, 10, 19, 150),
                                            IM_COL32(4, 10, 19, 150), IM_COL32(4, 10, 19, 235));
                bg->AddRectFilledMultiColor(P(a, 0), P(b, H_), IM_COL32(4, 10, 19, 150), IM_COL32(4, 10, 19, 0),
                                            IM_COL32(4, 10, 19, 0), IM_COL32(4, 10, 19, 150));
            } else {
                bg->AddRectFilled(P(0, 0), P(W_, H_), IM_COL32(4, 10, 19, 150));
            }

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ds);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                                 ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            ImGui::SetScrollX(0), ImGui::SetScrollY(0);
            ImDrawList *dl = ImGui::GetWindowDrawList();

            // ---- top bar: logo, version, the pages, PT | EN
            dl->AddRectFilled(P(0, 0), P(W_, 64), IM_COL32(4, 10, 19, 150));
            dl->AddLine(P(0, 64), P(W_, 64), IM_COL32(255, 255, 255, 18), 1.2f * g_s);
            logo(dl, 26, 17, 30);
            float lx = 64 + text(dl, F_H2, 20, 64, 32 - text_size(F_H2, 20, "ZA RECOMP").y / 2, C_TEXT, "ZA RECOMP").x;
            text(dl, F_SMALL, 13, lx + 8, 32 - text_size(F_SMALL, 13, "v").y / 2 + 1, C_MUTED, LARECOMP_VERSION);
            const char *tab_names[N_TABS] = {TR("Play", "Jogar"), TR("Mods", "Mods"), TR("Saves", "Saves"),
                                             TR("Settings", "Configurações"), TR("About", "Sobre")};
            float tx = lx + 70;
            int prev_tab = g_tab;
            for (int i = 0; i < N_TABS; i++) {
                float tw = text_size(F_LABEL, 15, tab_names[i]).x + 32;
                bool cur = g_tab == i;
                ImGui::SetCursorScreenPos(P(tx, 12));
                ImGui::PushID(i);
                bool en = !busy || i != TAB_SETTINGS;  // (settings stay put while the game is being set up)
                ImGui::BeginDisabled(!en);
                if (ImGui::InvisibleButton("##tab", S(tw, 40), ImGuiButtonFlags_EnableNav)) g_tab = i;
                ImGui::EndDisabled();
                bool hot = item_hot();
                ImGui::PopID();
                if (cur) {
                    dl->AddRectFilled(P(tx, 12), P(tx + tw, 52), IM_COL32(242, 181, 68, 40), 10 * g_s);
                    dl->AddRectFilled(P(tx + 10, 49), P(tx + tw - 10, 52), C_GOLD, 2 * g_s);
                } else if (hot) dl->AddRectFilled(P(tx, 12), P(tx + tw, 52), IM_COL32(255, 255, 255, 18), 10 * g_s);
                float th = text_size(F_LABEL, 15, tab_names[i]).y;
                text(dl, F_LABEL, 15, tx + 16, 32 - th / 2, cur ? C_GOLD_HI : !en ? IM_COL32(159, 179, 194, 110) : C_SOFT,
                     tab_names[i]);
                tx += tw + 4;
            }
            {  // PT | EN
                float sw = 92, sx = W_ - 24 - sw, sy = 17, sh = 30;
                dl->AddRectFilled(P(sx, sy), P(sx + sw, sy + sh), IM_COL32(255, 255, 255, 18), sh / 2 * g_s);
                for (int i = 0; i < 2; i++) {
                    bool on = (i == 0) == g_pt;
                    float bx = sx + 3 + i * (sw - 6) / 2, bw = (sw - 6) / 2;
                    ImGui::SetCursorScreenPos(P(bx, sy + 3));
                    ImGui::PushID(100 + i);
                    if (ImGui::InvisibleButton("##lang", S(bw, sh - 6), ImGuiButtonFlags_EnableNav)) {
                        g_settings.ui_lang = i == 0 ? UI_LANG_PT : UI_LANG_EN;
                        settings_save();
                        lang_update();
                    }
                    bool hot = item_hot();
                    ImGui::PopID();
                    if (on) dl->AddRectFilled(P(bx, sy + 3), P(bx + bw, sy + sh - 3), C_TEXT, (sh - 6) / 2 * g_s);
                    else if (hot) dl->AddRectFilled(P(bx, sy + 3), P(bx + bw, sy + sh - 3), IM_COL32(255, 255, 255, 30), (sh - 6) / 2 * g_s);
                    const char *l = i == 0 ? "PT" : "EN";
                    ImVec2 ts = text_size(F_LABEL, 13, l);
                    text(dl, F_LABEL, 13, bx + (bw - ts.x) / 2, sy + (sh - ts.y) / 2, on ? IM_COL32(8, 19, 31, 255) : C_SOFT, l);
                }
            }
            // L / R (Q / E) change pages (not while typing, in a question, or setting a binding)
            int cap_dev;
            bind_capture_active(&cap_dev, NULL, NULL);
            if (!typing && !popup && cap_dev < 0 && !(busy && g_tab == TAB_PLAY)) {
                int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (g_tab != TAB_SETTINGS && ImGui::IsKeyPressed(ImGuiKey_E, false)) ? 1
                         : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (g_tab != TAB_SETTINGS && ImGui::IsKeyPressed(ImGuiKey_Q, false)) ? -1 : 0;
                if (g_tab == TAB_SETTINGS) step = 0;  // (L / R are the settings tabs' own there)
                if (step) g_tab = (g_tab + step + N_TABS) % N_TABS;
            }
            if (g_tab != prev_tab) {
                bind_capture_cancel();
                settings_save();
                if (g_tab == TAB_SETTINGS) { static bool opened; menu_tabs_focus(!opened); opened = true; }
                if (g_tab == TAB_SAVES) g_save_scanned = false;
                if (g_tab == TAB_MODS) g_mods_changed = true;
                if (g_tab == TAB_PLAY) g_focus_play = true;
            }
            // B / Esc on a page goes back to Play
            bool back_key = !typing && !popup && cap_dev < 0 &&
                            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));

            pthread_mutex_lock(&g_lock);
            std::string src = g_src, job = g_job;
            bool failed = g_job_failed;
            pthread_mutex_unlock(&g_lock);
            bool ready = status.empty();
            Status bar = {C_GREEN, TR("All set · game files checked · game code built",
                                      "Tudo pronto · arquivos verificados · código do jogo compilado")};

            // ================= Play
            if (g_tab == TAB_PLAY) {
                float x = 72, w = fminf(640, W_ - 144);
                if (busy) {
                    // ---- setting up: the steps, the bar, PLAY greyed out until it's done
                    float f = test_busy ? 0.62f : extract_progress();
                    bool building = g_build_frac >= 0, icons = job.find("button icons") != std::string::npos;
                    int step = icons ? 2 : building ? 1 : 0;
                    page_title(dl, x, 104, TR("FIRST TIME · ONLY ONCE", "PRIMEIRA VEZ · SÓ ACONTECE UMA VEZ"),
                               TR("Getting the game ready", "Preparando o jogo"));
                    text(dl, F_BODY, 16, x, 196, C_SOFT, TR("Your game files are read and the game's code is compiled for "
                                                            "your processor. You can leave this window open and do "
                                                            "something else.", "Os seus arquivos do jogo são lidos e o "
                                                            "código do jogo é compilado para o seu processador. Pode "
                                                            "deixar esta janela aberta e fazer outra coisa."), w);
                    float cy = 262, ch = 230;
                    panel(dl, x, cy, w, ch);
                    char pct[16];
                    snprintf(pct, sizeof pct, "%d%%", (int)(f * 100));
                    ImVec2 ps = text(dl, F_H2, 30, x + 24, cy + 16, C_GOLD_HI, pct);
                    const char *now_t = step == 0 ? TR("Reading your game files", "Lendo os seus arquivos do jogo")
                                      : step == 1 ? TR("Compiling the game's code", "Compilando o código do jogo")
                                                  : TR("Drawing the button icons", "Desenhando os ícones dos botões");
                    text(dl, F_BODY, 15, x + 34 + ps.x, cy + 16 + ps.y - text_size(F_BODY, 15, now_t).y - 4, C_SOFT, now_t);
                    float by = cy + 24 + ps.y;
                    dl->AddRectFilled(P(x + 24, by), P(x + w - 24, by + 12), IM_COL32(255, 255, 255, 22), 6 * g_s);
                    if (f > 0) dl->AddRectFilledMultiColor(P(x + 24, by), P(x + 24 + (w - 48) * f, by + 12), C_TEAL, C_GOLD,
                                                           C_GOLD, C_TEAL);
                    const char *steps[3] = {TR("Read your game files", "Ler os seus arquivos do jogo"),
                                            TR("Compile the game's code (the longest part)", "Compilar o código do jogo (a parte mais demorada)"),
                                            TR("Draw the button icons", "Desenhar os ícones dos botões")};
                    for (int i = 0; i < 3; i++) {
                        float ly = by + 30 + i * 34;
                        ImU32 c = i < step ? C_TEXT : i == step ? C_GOLD_HI : C_MUTED;
                        if (i < step) icon(dl, IC_CHECK, x + 36, ly + 10, 20, C_GREEN);
                        else if (i == step) {
                            float t = (float)ImGui::GetTime() * 5;
                            dl->AddCircle(P(x + 36, ly + 10), 8 * g_s, IM_COL32(255, 215, 122, 70), 24, 2 * g_s);
                            dl->PathArcTo(P(x + 36, ly + 10), 8 * g_s, t, t + 1.8f, 12);
                            dl->PathStroke(C_GOLD_HI, 0, 2.2f * g_s);
                        } else dl->AddCircle(P(x + 36, ly + 10), 8 * g_s, C_MUTED, 24, 1.6f * g_s);
                        text(dl, F_BODY, 15, x + 56, ly, c, steps[i]);
                    }
                    button("##play_wait", TR("PLAY", "JOGAR"), x, cy + ch + 26, 200, 60, BTN_PRIMARY, false, IC_PLAY, F_BTN, 24);
                    text(dl, F_BODY, 14, x + 220, cy + ch + 46, C_MUTED,
                         TR("After an update only what changed is compiled again.",
                            "Depois de atualizações, só o que mudou é compilado de novo."));
                    bar = {C_GOLD, TR("Setting up the game...", "Preparando o jogo...")};
                } else if (!ready || g_changing_folder) {
                    // ---- not set up (or choosing another dump): the folder with the dump + keys, then set up
                    bool rebuild = !ready && src.empty() && check_copied_files(g_root).empty();  // (an update: new code)
                    page_title(dl, x, 104, rebuild ? TR("AFTER AN UPDATE", "DEPOIS DE UMA ATUALIZAÇÃO") : TR("SET UP", "CONFIGURAÇÃO"),
                               rebuild ? TR("The game code needs a rebuild", "O código do jogo precisa ser recompilado")
                                       : TR("Let's set the game up", "Vamos preparar o jogo"));
                    const char *what = rebuild
                        ? TR("This version of ZA Recomp builds the game's code a little differently. Your game files "
                             "are already here: only the code is compiled again (a few minutes).",
                             "Esta versão do ZA Recomp compila o código do jogo de um jeito um pouco diferente. Os seus "
                             "arquivos do jogo já estão aqui: só o código é compilado de novo (alguns minutos).")
                        : TR("Choose the folder with your own copy of Pokémon Legends Z-A, either way:\n"
                             "· the game extracted (exefs and romfs folders, as an emulator or your Switch extracts them): "
                             "no keys needed;\n"
                             "· or your dump: the base game (.xci or .nsp), optionally the update and the DLC (.nsp), and "
                             "the prod.keys from your Switch.\n"
                             "You can also drop the folder on this window. The game isn't included with ZA Recomp.",
                             "Escolha a pasta com a sua própria cópia de Pokémon Legends Z-A, de um destes jeitos:\n"
                             "· o jogo extraído (pastas exefs e romfs, como um emulador ou o seu Switch extraem): sem "
                             "precisar de keys;\n"
                             "· ou o seu dump: o jogo base (.xci ou .nsp), se quiser a atualização e a DLC (.nsp), e o "
                             "prod.keys do seu Switch.\n"
                             "Também dá para arrastar a pasta para esta janela. O jogo não vem com o ZA Recomp.");
                    float ty = 196 + text(dl, F_BODY, 16, x, 196, C_SOFT, what, w).y + 22;
                    if (!rebuild) {
                        panel(dl, x, ty, w, 64, C_PANEL);
                        icon(dl, IC_FOLDER, x + 30, ty + 32, 22, src.empty() ? C_MUTED : C_TEAL);
                        std::string shown = src.empty() ? TR("No folder chosen yet", "Nenhuma pasta escolhida ainda") : src;
                        dl->PushClipRect(P(x + 50, ty), P(x + w - 150, ty + 64), true);
                        text(dl, F_BODY, 15, x + 52, ty + 32 - text_size(F_BODY, 15, "A").y / 2, src.empty() ? C_MUTED : C_TEXT,
                             shown.c_str());
                        dl->PopClipRect();
                        if (button("##pick", TR("Choose...", "Escolher..."), x + w - 138, ty + 12, 124, 40, BTN_SECONDARY))
                            SDL_ShowOpenFolderDialog(picked, NULL, win, NULL, false);
                        ty += 84;
                    }
                    bool can = rebuild || !src.empty();
                    if (button("##setup", rebuild ? TR("Compile now", "Compilar agora") : TR("Set up the game", "Preparar o jogo"),
                               x, ty, 0, 54, BTN_PRIMARY, can, IC_PLAY, F_LABEL, 17, true)) {
                        pthread_mutex_lock(&g_lock);
                        g_busy = true;
                        pthread_mutex_unlock(&g_lock);
                        pthread_t t;
                        pthread_create(&t, NULL, extract_thread, NULL);
                        pthread_detach(t);
                    }
                    if (g_changing_folder && ready) {
                        if (button("##cancel_folder", TR("Cancel", "Cancelar"), x + 260, ty + 7, 0, 40, BTN_GHOST) || back_key)
                            g_changing_folder = false, g_focus_play = true;
                    }
                    ty += 74;
                    if (failed && g_fail) {  // what went wrong, and what to do
                        std::string hint = g_fail_detail.empty() ? fail_hint(g_fail, g_pt) : g_fail_detail + "\n" + fail_hint(g_fail, g_pt);
                        float hh = text_size(F_BODY, 14, hint.c_str(), w - 48).y;
                        panel(dl, x, ty, w, hh + 64, IM_COL32(40, 12, 10, 220), IM_COL32(255, 138, 112, 90));
                        text(dl, F_LABEL, 16, x + 24, ty + 14, C_RED, fail_head(g_fail, g_pt));
                        text(dl, F_BODY, 14, x + 24, ty + 42, C_SOFT, hint.c_str(), w - 48);
                    }
                    bar = failed && g_fail ? Status{C_RED, fail_head(g_fail, g_pt)}
                        : Status{C_GOLD, rebuild ? TR("The game code needs a rebuild", "O código do jogo precisa ser recompilado")
                                                 : TR("Set up the game files to play", "Prepare os arquivos do jogo para jogar")};
                } else {
                    // ---- ready
                    page_title(dl, x, 104, "POKÉMON LEGENDS: Z-A", TR("Ready to play", "Pronto para jogar"));
                    float ty = 196 + text(dl, F_BODY, 17, x, 196, C_SOFT, TR("Your game files are set up and the game's code "
                                                                              "is compiled for this PC.", "Os seus arquivos do "
                                                                              "jogo estão configurados e o código já foi "
                                                                              "compilado para este PC."), w).y + 18;
                    if (!g_save_scanned) saves_scan();
                    float cx = x;
                    cx += chip(dl, cx, ty, IC_CHECK, std::string(TR("Version ", "Versão ")) + game_version(g_root) +
                                                      (is_dir(g_root + "/aoc") ? " + DLC" : "")) + 8;
                    int on = mods_on_count();
                    char mods_t[64];
                    snprintf(mods_t, sizeof mods_t, on == 1 ? TR("1 mod on", "1 mod ativo") : TR("%d mods on", "%d mods ativos"), on);
                    cx += chip(dl, cx, ty, IC_LAYERS, mods_t) + 8;
                    if (g_save_time) chip(dl, cx, ty, IC_SAVE, std::string(TR("Save from ", "Save de ")) + fmt_time(g_save_time));
                    ty += 34 + 34;
                    bool pressed = button("##play", TR("PLAY", "JOGAR"), x, ty, 0, 68, BTN_PRIMARY, true, IC_PLAY, F_BTN, 26);
                    if (g_focus_play) ImGui::SetKeyboardFocusHere(-1), g_focus_play = false;
                    float pw = ImGui::GetItemRectSize().x / g_s;
                    if (button("##chg", TR("Change game folder", "Trocar pasta do jogo"), x + pw + 14, ty + 8, 0, 52, BTN_SECONDARY,
                               true, IC_FOLDER)) {
                        g_changing_folder = true;
                        pthread_mutex_lock(&g_lock);
                        g_src.clear();
                        pthread_mutex_unlock(&g_lock);
                    }
                    party_icons_kick();
                    party_panel(dl, x, ty + 68 + 34, w);
                    if (g_play_pending) pressed = true, g_play_pending = false;
                    if (pressed) {  // first PLAY ever: the aspect ratio question, then how to reach our menu
                        if (!g_settings.aspect_asked) ImGui::OpenPopup("##aspect");
                        else if (!g_settings.menu_tip_seen) ImGui::OpenPopup("##menutip");
                        else play = true;
                    }
                    if (g_tip_test) ImGui::OpenPopup(g_settings.aspect_asked ? "##menutip" : "##aspect"), g_tip_test = false;
                    if (aspect_prompt(s)) {
                        if (g_settings.menu_tip_seen) play = true;
                        else ImGui::OpenPopup("##menutip");
                    }
                    if (menu_tip(s)) play = true;
                }
            }

            // ================= Mods
            if (g_tab == TAB_MODS) {
                float x = 56, w = W_ - 112, y = 88;
                ImVec2 ts = text(dl, F_H1, 36, x, y, C_TEXT, TR("Mods", "Mods"));
                char cnt[64];
                snprintf(cnt, sizeof cnt, TR("%d of %d on", "%d de %d ativos"), mods_on_count(), (int)g_mods.size());
                float cw = text_size(F_LABEL, 13, cnt).x + 24;
                dl->AddRectFilled(P(x + ts.x + 14, y + ts.y / 2 - 13), P(x + ts.x + 14 + cw, y + ts.y / 2 + 13),
                                  IM_COL32(63, 208, 201, 40), 13 * g_s);
                text(dl, F_LABEL, 13, x + ts.x + 26, y + ts.y / 2 - text_size(F_LABEL, 13, cnt).y / 2, IM_COL32(127, 230, 224, 255), cnt);
                if (button("##openmods", TR("Open mods folder", "Abrir pasta de mods"), x + w - 220, y + 6, 220, 44, BTN_SECONDARY,
                           true, IC_FOLDER)) {
                    mkdir("mods", 0755);
                    reveal_folder("mods");
                }
                y += ts.y + 10;
                y += text(dl, F_BODY, 15, x, y, C_SOFT, TR("Mods replace the game's files (a romfs folder): translations, "
                                                          "textures, models. Mods of the game's code (exefs, IPS, cheats) "
                                                          "don't work here. Where two mods replace the same file, the one "
                                                          "higher up wins.", "Mods trocam arquivos do jogo (uma pasta romfs): "
                                                          "tradução, texturas, modelos. Mods que mexem no código do jogo "
                                                          "(exefs, IPS, cheats) não funcionam aqui. Se dois mods trocam o "
                                                          "mesmo arquivo, vale o que está mais acima."), w).y + 16;
                float list_h = H_ - 48 - 24 - 112 - 16 - y;
                ImGui::SetCursorScreenPos(P(x, y));
                ImGui::BeginChild("##modlist", S(w, list_h), 0, ImGuiWindowFlags_NavFlattened);
                ImDrawList *cl = ImGui::GetWindowDrawList();
                int move_from = -1, move_to = -1, remove_at = -1;
                bool changed = false;
                if (g_mods.empty()) {
                    ImVec2 o = ImGui::GetCursorScreenPos();
                    cl->AddText(F_BODY, 15 * g_fb * g_s, ImVec2(o.x, o.y + 10 * g_s), C_MUTED,
                                TR("No mods yet. Install one below, or drop a .zip / folder on this window.",
                                   "Nenhum mod ainda. Instale um aqui embaixo, ou arraste um .zip / pasta para esta janela."));
                }
                for (int i = 0; i < (int)g_mods.size(); i++) {
                    ModEntry &m = g_mods[i];
                    ImVec2 o = ImGui::GetCursorScreenPos();
                    float rw = w - 16, rh = 64;
                    ImVec2 save_o = g_o;
                    g_o = ImVec2(o.x - 0, o.y);  // (row-local coordinates for the helpers)
                    cl->AddRectFilled(P(0, 0), P(rw, rh), m.on ? C_PANEL : IM_COL32(8, 19, 31, 170), 14 * g_s);
                    cl->AddRect(P(0, 0), P(rw, rh), C_LINE, 14 * g_s, 0, 1.2f * g_s);
                    cl->AddRectFilled(P(12, 10), P(56, 54), m.on ? IM_COL32(63, 208, 201, 36) : IM_COL32(255, 255, 255, 16), 10 * g_s);
                    std::string ini = m.name.substr(0, 2);
                    for (auto &c : ini) c = (char)toupper((unsigned char)c);
                    ImVec2 is = text_size(F_H2, 16, ini.c_str());
                    text(cl, F_H2, 16, 34 - is.x / 2, 32 - is.y / 2, m.on ? IM_COL32(127, 230, 224, 255) : C_MUTED, ini.c_str());
                    text(cl, F_LABEL, 17, 70, 10, m.on ? C_TEXT : C_SOFT, m.name.c_str());
                    char sub[96];
                    snprintf(sub, sizeof sub, TR("%d files · mods/%s%s", "%d arquivos · mods/%s%s"), m.files, m.name.c_str(),
                             m.on ? "" : TR(" · off", " · desligado"));
                    text(cl, F_BODY, 13, 70, 36, C_MUTED, sub);
                    ImGui::PushID(i);
                    float bx = rw - 16 - 54 - 3 * 44;
                    if (toggle("##on", &m.on, bx, 17)) changed = true;
                    if (button("##up", "", bx + 66, 14, 36, 36, BTN_GHOST, i > 0, IC_CHEV_UP)) move_from = i, move_to = i - 1;
                    if (button("##dn", "", bx + 106, 14, 36, 36, BTN_GHOST, i + 1 < (int)g_mods.size(), IC_CHEV_DOWN))
                        move_from = i, move_to = i + 1;
                    if (button("##rm", "", bx + 146, 14, 36, 36, BTN_GHOST, true, IC_TRASH)) remove_at = i;
                    ImGui::PopID();
                    g_o = save_o;
                    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + (rh + 10) * g_s));
                    ImGui::Dummy(ImVec2(1, 0));
                }
                ImGui::EndChild();
                if (move_from >= 0) std::swap(g_mods[move_from], g_mods[move_to]), changed = true;
                static std::string confirm_name;
                if (remove_at >= 0) confirm_name = g_mods[remove_at].name, ImGui::OpenPopup("##rmmod");
                if (begin_question("##rmmod", s)) {
                    ImGui::PushTextWrapPos(0);
                    ImGui::Text(TR("Delete the mod \"%s\"?", "Apagar o mod \"%s\"?"), confirm_name.c_str());
                    ImGui::TextDisabled("%s", TR("Its folder in mods/ is deleted. To keep it, just switch it off.",
                                                 "A pasta dele em mods/ é apagada. Para guardar, é só desligar."));
                    ImGui::PopTextWrapPos();
                    ImGui::Dummy(ImVec2(0, 10 * s));
                    if (ImGui::Button(TR("Delete", "Apagar"), ImVec2(220 * s, 44 * s))) {
                        plat_remove_tree(("mods/" + confirm_name).c_str());
                        fprintf(stderr, "[launcher] mod deleted: %s\n", confirm_name.c_str());
                        g_mods_changed = true;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    ImGui::SetItemDefaultFocus();
                    if (ImGui::Button(TR("Cancel", "Cancelar"), ImVec2(220 * s, 44 * s)) ||
                        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                if (changed) mods_store();
                // install: a big drop zone (also a button: the file picker)
                float zy = H_ - 48 - 24 - 112;
                bool mbusy = g_mod_busy;
                static const SDL_DialogFileFilter mod_f[] = {{"Mods (.zip, .rar, .7z)", "zip;rar;7z"}};
                if (button("##install", "", x, zy, w - 236, 112, BTN_GHOST, !mbusy))
                    SDL_ShowOpenFileDialog(picked_mod, NULL, win, mod_f, 1, NULL, false);
                ImVec2 a = P(x, zy), b = P(x + w - 236, zy + 112);
                {  // the dashed border
                    float seg = 14 * g_s, gap = 8 * g_s;
                    for (float px = a.x; px < b.x; px += seg + gap) {
                        dl->AddLine(ImVec2(px, a.y), ImVec2(fminf(px + seg, b.x), a.y), IM_COL32(242, 181, 68, 130), 2 * g_s);
                        dl->AddLine(ImVec2(px, b.y), ImVec2(fminf(px + seg, b.x), b.y), IM_COL32(242, 181, 68, 130), 2 * g_s);
                    }
                    for (float py = a.y; py < b.y; py += seg + gap) {
                        dl->AddLine(ImVec2(a.x, py), ImVec2(a.x, fminf(py + seg, b.y)), IM_COL32(242, 181, 68, 130), 2 * g_s);
                        dl->AddLine(ImVec2(b.x, py), ImVec2(b.x, fminf(py + seg, b.y)), IM_COL32(242, 181, 68, 130), 2 * g_s);
                    }
                }
                icon(dl, IC_UP_ARROW, x + 46, zy + 56, 30, C_GOLD);
                text(dl, F_LABEL, 18, x + 78, zy + 26, C_TEXT, mbusy ? TR("Installing...", "Instalando...") : TR("Install a mod", "Instalar um mod"));
                text(dl, F_BODY, 14, x + 78, zy + 56, C_SOFT, TR("Drop a .zip, .rar, .7z or a folder here, or click to choose a file",
                                                                 "Arraste um .zip, .rar, .7z ou uma pasta para cá, ou clique para escolher"),
                     w - 236 - 100);
                if (button("##installdir", TR("Install a folder...", "Instalar pasta..."), x + w - 220, zy + 34, 220, 44,
                           BTN_SECONDARY, !mbusy, IC_FOLDER))
                    SDL_ShowOpenFolderDialog(picked_mod, NULL, win, NULL, false);
                pthread_mutex_lock(&g_mod_lock);
                std::string mm = g_mod_msg;
                bool mf = g_mod_failed;
                pthread_mutex_unlock(&g_mod_lock);
                bar = mbusy ? Status{C_GOLD, TR("Installing a mod...", "Instalando um mod...")}
                    : !mm.empty() ? Status{mf ? C_RED : C_GREEN, mm}
                    : Status{C_TEAL, TR("Changes apply from the next game start", "As mudanças valem a partir do próximo início do jogo")};
                if (back_key) g_tab = TAB_PLAY, g_focus_play = true;
            }

            // ================= Saves
            if (g_tab == TAB_SAVES) {
                import_save();  // (a folder picked or dropped: runs here, on the UI thread)
                save_export();
                static std::string last_msg;
                if (g_save_msg != last_msg) last_msg = g_save_msg, g_save_scanned = false;
                if (!g_save_scanned) saves_scan();
                float x = 56, gap = 20, cw = (W_ - 112 - gap) / 2, y = 88, h = H_ - 48 - 24 - y;
                panel(dl, x, y, cw, h);
                float ly = y + 22;
                ly += text(dl, F_H1, 32, x + 26, ly, C_TEXT, TR("Saves", "Saves")).y + 10;
                panel(dl, x + 26, ly, cw - 52, 96, IM_COL32(63, 208, 201, 26), IM_COL32(63, 208, 201, 90), 14);
                text(dl, F_SMALL, 12, x + 44, ly + 14, C_TEAL, TR("CURRENT SAVE", "SAVE ATUAL"));
                std::string cur = g_save_time ? std::string(TR("Saved ", "Salvo em ")) + fmt_time(g_save_time)
                                              : TR("No save yet", "Nenhum save ainda");
                text(dl, F_H2, 20, x + 44, ly + 32, C_TEXT, cur.c_str());
                if (g_save_time) text(dl, F_BODY, 13, x + 44, ly + 66, C_SOFT, (fmt_size(g_save_bytes) + " · save/save_0").c_str());
                ly += 112;
                float bx = x + 26;
                if (button("##import", TR("Import save...", "Importar save..."), bx, ly, 0, 46, BTN_PRIMARY, true, IC_UP_ARROW,
                           F_LABEL, 15, false))
                    SDL_ShowOpenFolderDialog(picked_save, NULL, win, NULL, false);
                bx += ImGui::GetItemRectSize().x / g_s + 10;
                static const SDL_DialogFileFilter zip_f[] = {{"Zip", "zip"}};
                if (button("##export", TR("Export (.zip)", "Exportar (.zip)"), bx, ly, 0, 46, BTN_SECONDARY, g_save_time != 0,
                           IC_DOWN_ARROW))
                    SDL_ShowSaveFileDialog(picked_export, NULL, win, zip_f, 1, "ZA Recomp save.zip");
                bx += ImGui::GetItemRectSize().x / g_s + 10;
                if (button("##opensave", TR("Open folder", "Abrir pasta"), bx, ly, 0, 46, BTN_SECONDARY, true, IC_FOLDER)) {
                    mkdir("save", 0755);
                    reveal_folder("save");
                }
                ly += 64;
                const char *from = TR("Where from: a JKSV backup (modded Switch: JKSV/Pokémon Legends Z-A/...), a save from "
                                      "an emulator or from another copy of ZA Recomp. Choose the folder holding the file "
                                      "\"main\", or drop it (or a .zip) on this window. Your current save becomes a backup "
                                      "first, so nothing is lost. The save must be from the same game version or older.",
                                      "De onde: um backup do JKSV (Switch desbloqueado: JKSV/Pokémon Legends Z-A/...), um "
                                      "save de emulador ou de outra cópia do ZA Recomp. Escolha a pasta com o arquivo "
                                      "\"main\", ou arraste ela (ou um .zip) para esta janela. O save atual vira um backup "
                                      "antes, então nada se perde. O save precisa ser da mesma versão do jogo ou mais antiga.");
                float fh = text_size(F_BODY, 14, from, cw - 92).y;
                panel(dl, x + 26, ly, cw - 52, fh + 32, IM_COL32(255, 255, 255, 10), IM_COL32(255, 255, 255, 40), 14);
                text(dl, F_BODY, 14, x + 46, ly + 16, C_SOFT, from, cw - 92);
                ly += fh + 48;
                if (!g_save_msg.empty())
                    text(dl, F_BODY, 14, x + 26, ly, g_save_failed ? C_RED : C_GREEN, g_save_msg.c_str(), cw - 52);

                float rx = x + cw + gap;
                panel(dl, rx, y, cw, h);
                ImVec2 bt = text(dl, F_H2, 24, rx + 26, y + 22, C_TEXT, TR("Backups", "Backups"));
                text(dl, F_BODY, 13, rx + 36 + bt.x, y + 22 + bt.y - text_size(F_BODY, 13, "A").y - 4, C_MUTED,
                     TR("made on their own before an import or a restore", "feitos sozinhos antes de importar ou restaurar"));
                ImGui::SetCursorScreenPos(P(rx + 20, y + 30 + bt.y));
                ImGui::BeginChild("##backups", S(cw - 40, h - 46 - bt.y - 34), 0, ImGuiWindowFlags_NavFlattened);
                ImDrawList *cl = ImGui::GetWindowDrawList();
                static std::string confirm_dir;
                if (g_backups.empty()) {
                    ImVec2 o = ImGui::GetCursorScreenPos();
                    cl->AddText(F_BODY, 14 * g_fb * g_s, ImVec2(o.x + 6 * g_s, o.y + 8 * g_s), C_MUTED,
                                TR("No backups yet.", "Nenhum backup ainda."));
                }
                for (size_t i = 0; i < g_backups.size(); i++) {
                    ImVec2 o = ImGui::GetCursorScreenPos();
                    ImVec2 save_o = g_o;
                    g_o = o;
                    float rw = cw - 56, rh = 58;
                    cl->AddRectFilled(P(0, 0), P(rw, rh), IM_COL32(255, 255, 255, 13), 12 * g_s);
                    icon(cl, IC_RESTORE, 26, 29, 20, C_MUTED);
                    text(cl, F_LABEL, 15, 50, 9, C_TEXT, fmt_time(g_backups[i].when).c_str());
                    text(cl, F_BODY, 12, 50, 32, C_MUTED, g_backups[i].dir.c_str());
                    ImGui::PushID((int)i);
                    if (button("##restore", TR("Restore", "Restaurar"), rw - 128, 11, 116, 36, BTN_SECONDARY))
                        confirm_dir = g_backups[i].dir, ImGui::OpenPopup("##restore_q");
                    if (begin_question("##restore_q", s)) {
                        ImGui::PushTextWrapPos(0);
                        ImGui::TextUnformatted(TR("Restore this backup?", "Restaurar este backup?"));
                        ImGui::TextDisabled("%s", TR("The save you have now becomes a backup too.",
                                                     "O save que você tem agora também vira um backup."));
                        ImGui::PopTextWrapPos();
                        ImGui::Dummy(ImVec2(0, 10 * s));
                        if (ImGui::Button(TR("Restore", "Restaurar"), ImVec2(220 * s, 44 * s))) {
                            save_restore(confirm_dir);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        ImGui::SetItemDefaultFocus();
                        if (ImGui::Button(TR("Cancel", "Cancelar"), ImVec2(220 * s, 44 * s)) ||
                            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                            ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    g_o = save_o;
                    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + (rh + 8) * g_s));
                    ImGui::Dummy(ImVec2(1, 0));
                }
                ImGui::EndChild();
                bar = {C_TEAL, TR("Saves live in the save folder next to ZA Recomp", "Os saves ficam na pasta save, ao lado do ZA Recomp")};
                if (back_key) g_tab = TAB_PLAY, g_focus_play = true;
            }

            // ================= Settings: the in-game menu's tabs (runtime/ui/menu.mm), in a panel
            if (g_tab == TAB_SETTINGS) {
                float sc = ds.y / 1080.0f;
                ImGui::SetCursorScreenPos(P(40, 80));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.07f, 0.12f, 0.94f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 18 * g_s);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 22 * sc));
                ImGui::BeginChild("##settings", S(W_ - 80, H_ - 80 - 64), ImGuiChildFlags_AlwaysUseWindowPadding,
                                  ImGuiWindowFlags_NavFlattened);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                ImGui::PushFont(g_font_small);
                menu_tabs_draw(sc, g_font_small, g_font_menu, false, prelaunch_tab);
                ImGui::PopFont();
                ImGui::EndChild();
                bar = {C_TEAL, TR("Settings are saved as you change them · in game: F1", "As configurações são salvas na hora · no jogo: F1")};
                if (back_key && !controls_ui_took_back() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                    bind_capture_cancel();
                    settings_save();
                    g_tab = TAB_PLAY, g_focus_play = true;
                }
            }

            // ================= About
            if (g_tab == TAB_ABOUT) {
                float x = 72, w = fminf(760, W_ - 144), y = 100;
                y += text(dl, F_H1, 40, x, y, C_TEXT, "ZA Recomp").y + 4;
                y += text(dl, F_LABEL, 16, x, y, C_GOLD_HI,
                          (std::string(TR("Version ", "Versão ")) + tag_label(LARECOMP_VERSION, false)).c_str()).y + 18;
                const char *about = TR("A PC port of Pokémon Legends: Z-A made by static recompilation: the game's code is "
                                       "turned into native code for your PC, and the Switch's system and graphics are "
                                       "rebuilt around it. Built on LAS Recomp by Peiikai and its contributors, with "
                                       "Ryujinx as a reference.\n\nNo part of the game is included: you bring your own "
                                       "dump and keys. Pokémon and Nintendo Switch are trademarks of their owners; this "
                                       "project isn't affiliated with them.",
                                       "Um port para PC de Pokémon Legends: Z-A feito por recompilação estática: o código "
                                       "do jogo vira código nativo para o seu PC, e o sistema e os gráficos do Switch são "
                                       "refeitos em volta dele. Feito sobre o LAS Recomp, do Peiikai e colaboradores, com "
                                       "o Ryujinx como referência.\n\nNenhuma parte do jogo vem junto: você usa o seu "
                                       "próprio dump e as suas chaves. Pokémon e Nintendo Switch são marcas dos seus donos; "
                                       "este projeto não tem ligação com eles.");
                y += text(dl, F_BODY, 16, x, y, C_SOFT, about, w).y + 26;
                if (button("##gh", TR("Project page (GitHub)", "Página do projeto (GitHub)"), x, y, 0, 48, BTN_SECONDARY))
                    SDL_OpenURL("https://github.com/Hexveil404/PokeZ-ARecomp");
                float bx2 = x + ImGui::GetItemRectSize().x / g_s + 12;
                if (button("##issues", TR("Report a problem", "Relatar um problema"), bx2, y, 0, 48, BTN_SECONDARY))
                    SDL_OpenURL("https://github.com/Hexveil404/PokeZ-ARecomp/issues");
                bx2 += ImGui::GetItemRectSize().x / g_s + 12;
                if (button("##logs", TR("Open the logs folder", "Abrir a pasta de logs"), bx2, y, 0, 48, BTN_SECONDARY, true, IC_FOLDER))
                    reveal_folder("logs");
                bar = {C_TEAL, TR("Thank you for playing!", "Obrigado por jogar!")};
                if (back_key) g_tab = TAB_PLAY, g_focus_play = true;
            }

            // ---- bottom bar: status, and a newer release when the update check found one
            dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(P(0, H_ - 48), P(W_, H_), IM_COL32(4, 10, 19, 185));
            dl->AddLine(P(0, H_ - 48), P(W_, H_ - 48), IM_COL32(255, 255, 255, 18), 1.2f * g_s);
            dl->AddCircleFilled(P(32, H_ - 24), 4.5f * g_s, bar.dot);
            text(dl, F_BODY, 14, 46, H_ - 24 - text_size(F_BODY, 14, "A").y / 2, C_SOFT, bar.text.c_str());
            {
                pthread_mutex_lock(&g_update_lock);
                std::string upd = g_update_tag;
                pthread_mutex_unlock(&g_update_lock);
                if (!upd.empty()) {
                    std::string link = std::string(TR("New version ", "Nova versão ")) + tag_label(upd, false) +
                                       TR(" available — see what's new", " disponível — ver novidades");
                    ImVec2 ls = text_size(F_LABEL, 14, link.c_str());
                    float lx2 = W_ - 28 - ls.x, ly2 = H_ - 24 - ls.y / 2;
                    ImGui::SetCursorScreenPos(P(lx2, ly2));
                    bool click = ImGui::InvisibleButton("##update", S(ls.x, ls.y), ImGuiButtonFlags_EnableNav);
                    bool hover = item_hot();
                    text(dl, F_LABEL, 14, lx2, ly2, hover ? C_GOLD_HI : C_GOLD, link.c_str());
                    if (hover) {
                        dl->AddLine(P(lx2, ly2 + ls.y), P(lx2 + ls.x, ly2 + ls.y), C_GOLD_HI, 1.2f * g_s);
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                    if (click) SDL_OpenURL((RELEASE_PAGE + upd).c_str());
                }
            }
            if (const char *f = getenv("LARECOMP_LAUNCHER_FONT")) {  // (test) which typeface this window shows
                const char *b = strrchr(f, '/');
                dl->AddText(P(8, 70), IM_COL32(255, 255, 255, 255), b ? b + 1 : f);
            }
            ImGui::End();
            ImGui::PopStyleColor();
            osk_draw(g_font_menu, s * 0.7f);  // (a text field activated with the controller)
            typed_before = ui_text_busy();

            ImGui::Render();
#ifdef __APPLE__
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
            [enc endEncoding];
            id<MTLTexture> shot = nil;
            if (shot_at && ++frame == shot_at) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.texture.pixelFormat
                                             width:drawable.texture.width height:drawable.texture.height mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                shot = [dev newTextureWithDescriptor:td];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:drawable.texture toTexture:shot];
                [bl endEncoding];
            }
            [cb presentDrawable:drawable];
            [cb commit];
            if (shot) {
                [cb waitUntilCompleted];
                save_png(shot, getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png");
                exit(0);
            }
#else
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
            bool shot = shot_at && ++frame == shot_at;
            const char *shot_path = getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png";
            if (shot) remove(shot_path);  // (an old one mustn't pass for this one below)
            vk_frame_end(shot ? shot_path : NULL);
            if (shot) {  // (the PNG is written on a thread of its own: wait for it, or exit could cut it off)
                for (long last = -1, i = 0; i < 100; i++) {
                    SDL_Delay(50);
                    struct stat st;
                    long size = stat(shot_path, &st) == 0 ? (long)st.st_size : -1;
                    if (size > 0 && size == last) break;
                    last = size;
                }
                exit(0);
            }
#endif
        }
    }
    // play: hand the window to the game
    settings_save();
#ifdef __APPLE__
    ImGui_ImplMetal_Shutdown();
#else
    vk_imgui_backend_shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
    g_start_screen = 0;
}
