// User settings (larecomp.ini next to the executable's working directory), edited from the in-game menu.
#pragma once
#include "cosmetic_colour.h"
#include <stdbool.h>

enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12, RES_X15 = 13 };
// (11 was 67% of the display: larecomp.ini "67%" now loads as RES_Q75)
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { PROMPTS_AUTO = 0, PROMPTS_NINTENDO = 1, PROMPTS_XBOX = 2, PROMPTS_PLAYSTATION = 3, PROMPTS_KEYBOARD = 4, PROMPTS_STEAMDECK = 5 };  // (keyboard: Pokémon Z-A's font glyphs, runtime/hle/pl.c)
enum { MOUSE_CAM_OFF = 0, MOUSE_CAM_HOLD = 1 /* right button held */, MOUSE_CAM_CAPTURED = 2 /* cursor held by the window */ };
// Which face button confirms in our own menus (runtime/ui/pad_nav.mm); the game's input is never affected
enum { MENU_CONFIRM_GAME = 0, MENU_CONFIRM_CONTROLLER = 1 };
enum { TIMER_POS_LEFT = 0, TIMER_POS_RIGHT = 1 };  // the run timer's corner (bottom left / right)
enum { CHAOS_LETTERS_OFF = 0, CHAOS_LETTERS_EXPLORED = 1, CHAOS_LETTERS_MAP = 2 /* explored, all with the Map */ };
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, N_LANGUAGES = 12 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ..., "Spanish")

typedef struct {
    int aspect;          // ASPECT_*: unlocked = fill the window (wider/taller views), original = 16:9 with bars
    bool fullscreen;
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    bool res_auto;       // never chosen: the default for this GPU (settings_resolve_defaults); saved as "auto"
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    bool upscaler_auto;  // never chosen: the default for this GPU (settings_resolve_defaults); saved as "auto"
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool show_fps;
    bool fps60;  // Pokémon Z-A: its frame clock at 1/60 s per frame (runtime/hle/framepace.c)
    bool aniso16;  // 16x anisotropic filtering on every mipmapped sampler (runtime/nvn/vulkan/textures.c)
    int opmode;     // the mode the game is told it runs in (runtime/services/am.c): 0 auto (handheld on a Steam Deck), 1 docked (1080p), 2 handheld (720p)
    bool game_drs;  // Pokémon Z-A: its own dynamic resolution (else always full: runtime/nvn/vulkan/exec.c)
    bool tex_hd, tex_dump;  // HD texture packs (textures/hd) on; dump the game's textures (textures/dump): vulkan/textures.c
    int shadow_res;    // 1-4: shadow maps drawn at that many times the game's size, from the next start (vulkan/textures.c)
    int shadow_sharp;  // 1-4: the game's shadow filter that many times narrower, up to shadow_res (vulkan/exec.c)
    bool dpad_move;      // the d-pad (and arrow keys) move Link like the stick
    bool rumble;         // controller vibration from the game (runtime/hle/input.c)
    int rumble_strength; // its intensity, percent (25 / 50 / 75 / 100)
    bool controller_light; // the controller's light bar shows Link's health (runtime/led.h, hle/input.c)
    bool bg_input;       // controllers keep working while the window isn't in front (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS)
    bool move_360;       // analog movement in any direction (the game snaps the stick to 8): runtime/hle/qol.c
    bool battle_abxy_position;  // while ZL is held, the controller's A/B/X/Y go by position: runtime/hle/input.c
    int mouse_camera;    // MOUSE_CAM_*: how the mouse turns the camera (runtime/hle/input.c)
    float mouse_sens;    // mouse camera speed, 1 = default
    int rstick_snap;     // degrees: a right stick this close to an axis is put on it (0 = off): runtime/hle/input.c
    bool blur_removal;   // no tilt-shift blur at the top / bottom of the screen
    bool hide_hero_icon; // no Hero Mode icon on the HUD
    bool mirror_mode;    // the world mirrored left to right (Gameplay tab): renderers + runtime/hle/input.c
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int prompts;         // PROMPTS_*: button glyphs (auto = follow the controller SDL reports): runtime/nvn/texpatch.c
    int menu_confirm;    // MENU_CONFIRM_*: game layout (right confirms, bottom goes back) or the controller's convention
    bool inv_keys;       // randomizer: show every dungeon's keys on the pause menu's Items page
    bool tracker;        // randomizer: check tracker on the pause menu's Map page
    int tracker_filters; // its filter bar (runtime/ui/tracker_overlay.mm bits), -1 = the defaults; no menu row
    int chaos_letters;   // CHAOS_LETTERS_*: room-shuffle doorway letters on the dungeon map
    bool aspect_asked;   // the start screen's one-time "authentic 16:9 or unlocked?" question was answered
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
    bool onedrive_kept;  // the start screen's OneDrive warning (Windows) was answered with Keep here
    // cheats (runtime/ui/cheats.mm), applied every frame while a save is loaded
    bool cheat_hearts;   // health kept full
    bool cheat_ammo;     // bombs / arrows / magic powder kept full (for the bags Link owns)
    bool cheat_rupees;   // rupees kept at 999
    bool cheat_hookshot; // hookshot latches onto any surface (Gameplay tab; runtime/hle/hookshot.c; seeds made with it: always)
    bool no_buff_drops;  // no Guardian Acorns / Pieces of Power (Gameplay tab; runtime/hle/qol.c; seeds made with it: always)
    // difficulty (runtime/ui/difficulty.mm)
    float diff_damage;   // damage taken multiplier (1 = normal)
    float walk_speed;    // Link's walking speed multiplier (1 = normal; Gameplay tab, runtime/hle/qol.c)
    bool walk_speed_dungeons;  // walk speed also indoors: houses, caves, dungeons (Gameplay tab, under Walk speed)
    bool diff_ohko;      // any hit kills
    bool diff_hero;      // Hero Mode flag forced on in the loaded save (2x damage, no recovery hearts)
    // cosmetics (Cosmetics tab, runtime/ui/menu.mm), live, never part of a seed: colour rows of the shared palette
    // (runtime/cosmetic_colour.h; Default = the game's own, for the tunic also the seed's)
    CosColour tunic, hearts;  // runtime/nvn/texpatch.c (tunic map, heart tints)
    CosColour hair, shield;   // runtime/nvn/recolour_link.c
    CosColour skin;           // runtime/nvn/texpatch.c skin_apply
    CosColour leaves, grass;  // the overworld's foliage, runtime/nvn/recolour_seasons.c
    CosColour sand;           // the beaches, the desert, the sandy south (recolour_seasons.c)
    int enemy_colours;        // 0 Off, 1 Random per seed, 2 Random every load (runtime/nvn/recolour_enemies.c)
    int screen_filter;        // 0 Off, 1 Game Boy, 2 Game Boy Color, 3 Grayscale (runtime/nvn/screen_filter.h)
    // speedrun (Gameplay tab; runtime/hle/run_timer.c, runtime/livesplit.c)
    bool timer;            // the in-game run timer in the corner (game time: level loads don't count)
    int timer_pos;         // TIMER_POS_*: which bottom corner it sits in
    bool livesplit;        // drive LiveSplit (its Server component) over TCP: start, splits, game time
    char livesplit_host[64];  // host:port of the LiveSplit Server ("localhost:16834")
    char rando_seed[128];  // active randomizer seed (rando/<name>/), empty = vanilla
    char rando_options[48];  // the Randomizer tab's New seed options: a seed ID without its random part (empty = defaults)
    bool fps60_lock;  // with fps60: stays at 60 even when this PC can't hold it (slow motion below; runtime/hle/framepace.c)
    int ui_lang;      // UI_LANG_*: the start screen's language (runtime/ui/launcher.mm); auto = the system's
    // mods/ folders (runtime/services/fs.c layers them): '|' between names. order: first wins where two replace the
    // same file (folders not listed come after, by name); off: not loaded. Edited on the start screen's Mods page.
    char mods_order[1024];
    char mods_off[1024];
} Settings;


enum { N_ENEMY_COLOUR_MODES = 3, N_SCREEN_FILTERS = 4 };
extern const char *const g_screen_filters[N_SCREEN_FILTERS];  // (the menu's names)
extern const char *const g_enemy_colour_modes[N_ENEMY_COLOUR_MODES];  // (the menu's names)
enum { UI_LANG_AUTO = 0, UI_LANG_EN = 1, UI_LANG_PT = 2 };
// Our own screens (the start screen, the F1 menu) in Portuguese: ui_lang, or for auto the system's language.
// UI_TR("English", "Português") picks the line.
bool settings_ui_portuguese(void);
#define UI_TR(en, pt) (settings_ui_portuguese() ? (pt) : (en))
// name is one of list's '|'-separated entries
bool settings_list_has(const char *list, const char *name);

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// At device init: the defaults that depend on the GPU. integrated (Vulkan: an integrated GPU; never on macOS): plain
// bilinear upscaling instead of the sharp upscaler, whose pass at the window's size costs as much as the game's frame
// there (UHD 770: FSR 1 + SMAA 8 ms of GPU per frame at 1080p, 20 ms at 4K), and Dynamic resolution instead of Match
// display. A player's own choice is kept. Anti-aliasing is off by default everywhere. LARECOMP_INTEGRATED=0/1 (test):
// treat the GPU as dedicated / integrated.
void settings_resolve_defaults(bool integrated);
extern int g_default_upscaler;  // UPSCALE_*: what the menu shows as the default
extern int g_default_res_mode;  // RES_*: likewise
// An integrated GPU (as settings_resolve_defaults was told): the fixed 1x renders 1280x720 there, what the game itself
// picked on such a GPU (runtime/nvn/output_scale.c)
extern bool g_gpu_integrated;
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);
