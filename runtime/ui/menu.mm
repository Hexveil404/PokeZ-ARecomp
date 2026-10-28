// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#include "option_rows.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "pad_nav.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
// runtime/nvn/vulkan/device.c: the command buffer ImGui records into (a secondary one the recorder thread executes in
// the window image's commands, or that buffer itself after the recorder caught up), then handed back
extern "C" void *vk_overlay_begin(void *window_cmd);
extern "C" void vk_overlay_end(void *window_cmd, void *cmd);
extern "C" int vk_hdr_state(void);  // device.c: 0 the display isn't in HDR mode, 1 it is, 2 HDR output on
#endif

extern "C" {
#include "../hle.h"
#include "../settings.h"
extern int g_game_is_la;  // runtime/kernel.c: Link's Awakening (its own tabs: randomizer, warp, cheats...)
#include "menu.h"
void input_reset(void);
void save_editor_draw(float scale);  // save_editor.mm
extern "C" int game_in_play(void);  // hle/warp.c
void save_editor_on_open(void);
void rando_tab_draw(float scale);
bool warp_tab_draw(float scale);  // warp_tab.mm: true when a warp started
extern "C" void cheats_draw(float scale);  // cheats.mm
extern "C" int settings_on_steam_deck(void);  // settings.c
extern "C" void difficulty_draw(float scale);  // difficulty.mm
void controls_tab_draw(float scale);  // controls_ui.mm
bool controls_ui_took_back(void);
bool rando_tab_took_back(void);  // rando_tab.mm: B went back from the Triforce hunt's count
#include "../input_bind.h"
#include "../paths.h"
}

#define TR(en, pt) UI_TR(en, pt)  // (runtime/settings.h: English, or Portuguese as the start screen's PT | EN)

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

// A line at the bottom of the screen for a few seconds, from any thread (runtime/hle/framepace.c: 60 fps turned off)
static char g_notice_text[256];
static std::atomic<uint64_t> g_notice_until{0};
extern "C" void menu_notice(const char *text, int seconds) {
    pthread_mutex_lock(&q_lock);
    snprintf(g_notice_text, sizeof g_notice_text, "%s", text);
    pthread_mutex_unlock(&q_lock);
    g_notice_until = SDL_GetTicks() + (uint64_t)seconds * 1000;
}

bool menu_is_open(void) { return g_open.load(); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        save_editor_on_open();
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}


// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + confirm, back deletes, Start / + OK; buttons as in pad_nav.h) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
static bool capturing() { return g_open.load() || g_kbd.load() == 1; }
// The game's name limit counts characters as the console's keyboard does, in UTF-16 units (a character past U+FFFF
// takes two); the text is UTF-8. Cuts s after max_units of them, only between characters (a character left incomplete
// - a cut inside it, or bytes that are no character - goes too, with the rest), and returns how many units are left.
static int kbd_cut(char *s, int max_units) {
    int units = 0;
    for (unsigned char *p = (unsigned char *)s; *p;) {
        int n = *p < 0x80 ? 1 : *p >= 0xc2 && *p <= 0xdf ? 2 : *p >= 0xe0 && *p <= 0xef ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
        int i = 1;
        while (i < n && (p[i] & 0xc0) == 0x80) i++;
        if (i == n && n > 2 && ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] > 0x9f) || (*p == 0xf0 && p[1] < 0x90) ||
                                (*p == 0xf4 && p[1] > 0x8f)))
            n = 0;  // (overlong, a surrogate or past U+10FFFF: the game's UTF-8 conversion refuses those too)
        int u = n == 4 ? 2 : 1;
        if (!n || i < n || units + u > max_units) { *p = 0; break; }
        units += u, p += n;
    }
    return units;
}
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static bool back_down, start_down;
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = ui_pad_key(e->gbutton.which, e->gbutton.button, down);  // (confirm / back: pad_nav.h)
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

extern "C" uint32_t g_window_resize_request;  // (test scripts: runtime/hle/input.c "resize")
void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || g_want_fullscreen_apply.exchange(false)) {
        first = false;
        SDL_SetWindowFullscreen(w, g_settings.fullscreen);
    }
    if (uint32_t r = __atomic_exchange_n(&g_window_resize_request, 0u, __ATOMIC_RELAXED))
        SDL_SetWindowSize(w, (int)(r >> 16), (int)(r & 0xffff));
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Baloo 2); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
static bool g_page_fresh_seen;  // (focus_check: the menu opened)
static bool g_page_fresh;  // the page shown hasn't been entered yet: the first Down goes to its first enabled row
static bool g_page_focus_next;  // (that page has no option rows: its first control takes the cursor next frame)
static int g_shown_tab = -1000;  // the page drawn last (its id)
void menu_tabs_focus(bool first_tab) {
    g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0, g_page_fresh = g_page_fresh_seen = true;
}
int g_tab_request_ext = -1;

// ZA Recomp's look, as its start screen: night blue panels, gold for what's selected, teal for headings
static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 18; s.FrameRounding = 10; s.GrabRounding = 10; s.TabRounding = 10; s.ChildRounding = 14;
    s.PopupRounding = 14;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 1; s.PopupBorderSize = 1; s.ScrollbarSize = 16; s.TabBarBorderSize = 1;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.95f, 0.71f, 0.27f, 1), gold_dim(0.62f, 0.47f, 0.18f, 1), glass(1, 1, 1, 0.07f), glass_hi(1, 1, 1, 0.13f);
    c[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.07f, 0.12f, 0.95f);
    c[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.10f, 0.16f, 0.80f);
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.09f, 0.15f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.95f, 0.96f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.70f, 0.76f, 1);
    c[ImGuiCol_FrameBg] = glass;
    c[ImGuiCol_FrameBgHovered] = glass_hi;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.95f, 0.71f, 0.27f, 0.25f);
    c[ImGuiCol_Button] = glass;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.95f, 0.71f, 0.27f, 0.30f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.71f, 0.27f, 0.50f);
    c[ImGuiCol_Header] = glass_hi;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.95f, 0.71f, 0.27f, 0.22f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.95f, 0.71f, 0.27f, 0.38f);
    c[ImGuiCol_Tab] = ImVec4(1, 1, 1, 0.04f);
    c[ImGuiCol_TabHovered] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_TabSelected] = ImVec4(0.95f, 0.71f, 0.27f, 0.20f);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
    c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold_dim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    char dejavu[1024];  // none of the system's: the bundled DejaVu Sans (third_party/fonts/DejaVu, in every release)
    const char *bundled = rt_resource("DejaVuSans.ttf", "third_party/fonts/DejaVu/DejaVuSans.ttf", dejavu, sizeof dejavu) ? dejavu : NULL;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", bundled};
    for (const char *p : paths) {
        if (!p) continue;
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b); }
    }
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    if (const char *f = getenv("LARECOMP_MENU_FONT")) strlcpy(baloo, f, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(baloo, 39, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(baloo, 72, &cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

static bool menu_la_tabs = true;  // tabs_body: Link's Awakening's tabs and rows are shown (false: ZA Recomp's own set)
extern "C" bool menu_la_rows(void) { return menu_la_tabs; }  // (runtime/ui/controls_ui.mm)
extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    bool la = g_game_is_la && menu_la_tabs;  // (Link's Awakening's own rows)
    opt_begin("##video", scale);
    opt_section(TR("Display", "Tela"));
    const char *aspects[] = {TR("Unlocked", "Livre"), TR("Original 16:9", "16:9 original")};
    opt_choice(TR("Aspect ratio", "Proporção"), &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               TR("Unlocked: the view fills the window, so wider (or taller) windows show more of the world - up to "
                  "ultrawide. Original 16:9: the game's own framing, with bars.",
                  "Livre: a imagem ocupa a janela toda, então janelas mais largas (ou mais altas) mostram mais do mundo "
                  "- até ultrawide. 16:9 original: o enquadramento do próprio jogo, com barras."));
    bool fs = g_settings.fullscreen;
    if (opt_toggle(TR("Fullscreen", "Tela cheia"), &fs, false,
                   TR("Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.", "Ocupa a tela toda. Também: F11, Alt+Enter ou Cmd+Ctrl+F."))) {
        g_settings.fullscreen = fs;
        g_want_fullscreen_apply.store(true);
    }
    opt_toggle(TR("Show FPS", "Mostrar FPS"), &g_settings.show_fps, false,
               TR("Shows the frame rate in the top right corner.", "Mostra a taxa de quadros no canto superior direito."));
    {   // Pokémon Z-A's frame mode (runtime/hle/framepace.c rt_za_frame_cfg): applies from the next frame
        extern bool g_za_frame_hook, g_fps60_fell_back;
        if (g_za_frame_hook) {
            int fr = g_settings.fps60 ? (g_settings.fps60_lock ? 2 : 1) : 0;
            const char *rates[] = {TR("30 fps (Switch)", "30 fps (Switch)"), TR("60 fps (experimental)", "60 fps (experimental)"),
                                   TR("60 fps (always)", "60 fps (sempre)")};
            if (opt_choice(TR("Frame rate", "Taxa de quadros"), &fr, rates, 3, 0,
                           TR("30 fps: as on the Switch (recommended). 60 fps (experimental): the game presents every vsync "
                              "and steps its world 1/60 s a frame - smoother, but it needs a fast CPU: the game counts "
                              "frames, not time, so below 60 fps everything moves in slow motion. When this PC doesn't "
                              "keep 60, ZA Recomp goes back to 30 by itself (choose 60 again to retry). 60 fps (always): "
                              "never goes back to 30 - the game slows down whenever the frame rate drops below 60.",
                              "30 fps: como no Switch (recomendado). 60 fps (experimental): o jogo mostra um quadro a cada "
                              "vsync e avança o mundo 1/60 s por quadro - mais fluido, mas pede uma CPU rápida: o jogo conta "
                              "quadros, não tempo, então abaixo de 60 fps tudo fica em câmera lenta. Quando este PC não "
                              "segura 60, o ZA Recomp volta para 30 sozinho (escolha 60 de novo para tentar outra vez). "
                              "60 fps (sempre): nunca volta para 30 - o jogo fica lento quando a taxa cai abaixo de 60.")))
                g_settings.fps60 = fr >= 1, g_settings.fps60_lock = fr == 2;
            if (g_settings.fps60 && g_fps60_fell_back)
                ImGui::TextColored(ImVec4(0.95f, 0.71f, 0.27f, 1), "%s",
                                   TR("Running at 30 fps: this PC didn't keep 60 fps.", "Rodando a 30 fps: este PC não segurou 60 fps."));
        }
    }
    {   // (runtime/nvn/vulkan/textures.c vk_sampler_settings: every sampler made again, at once)
        int tf = g_settings.aniso16 ? 1 : 0;
        const char *tfs[] = {TR("Game's", "Do jogo"), TR("High", "Alta")};
        if (opt_choice(TR("Texture filtering", "Filtragem de texturas"), &tf, tfs, 2, 0,
                       TR("High: 16x anisotropic and trilinear filtering - the ground and walls stay sharp at a distance and "
                          "at grazing angles, with no visible bands where their detail steps down. Game's: as on the Switch.",
                          "Alta: filtragem anisotrópica 16x e trilinear - o chão e as paredes ficam nítidos de longe e em "
                          "ângulos rasos, sem faixas visíveis onde o detalhe diminui. Do jogo: como no Switch.")))
            g_settings.aniso16 = tf == 1;
    }
#ifndef __APPLE__
    {   // HDR output (runtime/nvn/vulkan/device.c create_swapchain, exec.c vk_hdr_encode): applies at once
        bool avail = vk_hdr_state() > 0;
        opt_toggle(TR("HDR", "HDR"), &g_settings.hdr, false,
                   avail ? TR("For HDR displays: the game's bright highlights - sky, sun, lights, neon - get brighter than "
                              "white, up to the peak brightness below. The HUD and menus stay at the UI brightness.",
                              "Para telas HDR: os pontos mais claros do jogo - céu, sol, luzes, neon - ficam mais claros que o "
                              "branco, até o brilho máximo abaixo. O HUD e os menus ficam no brilho da interface.")
                         : TR("Needs an HDR display with HDR turned on in Windows (Settings > System > Display > Use HDR).",
                              "Precisa de uma tela HDR com o HDR ligado no Windows (Configurações > Sistema > Tela > Usar HDR)."));
        if (g_settings.hdr) {
            static const int peaks[] = {400, 600, 800, 1000, 1500}, papers[] = {100, 150, 200, 250, 300};
            static const char *const peak_names[] = {"400 nits", "600 nits", "800 nits", "1000 nits", "1500 nits"};
            static const char *const paper_names[] = {"100 nits", "150 nits", "200 nits", "250 nits", "300 nits"};
            int pk = 0, pw = 0;
            for (int i = 0; i < 5; i++) {
                if (abs(peaks[i] - g_settings.hdr_peak) < abs(peaks[pk] - g_settings.hdr_peak)) pk = i;
                if (abs(papers[i] - g_settings.hdr_paper) < abs(papers[pw] - g_settings.hdr_paper)) pw = i;
            }
            if (opt_choice(TR("HDR peak brightness", "Brilho máximo do HDR"), &pk, peak_names, 5, 1,
                           TR("The brightest highlights. Set it to your display's peak (its HDR rating: DisplayHDR 600 = 600 "
                              "nits) or a little below.",
                              "Os pontos mais claros. Use o pico da sua tela (a classificação HDR: DisplayHDR 600 = 600 nits) "
                              "ou um pouco menos.")))
                g_settings.hdr_peak = peaks[pk];
            if (opt_choice(TR("HDR UI brightness", "Brilho da interface no HDR"), &pw, paper_names, 5, 2,
                           TR("White in the HUD and menus, and the rest of the picture below its highlights (\"paper white\").",
                              "O branco do HUD e dos menus, e o resto da imagem abaixo dos pontos mais claros (\"branco de papel\").")))
                g_settings.hdr_paper = papers[pw];
        }
    }
#endif
    {   // Pokémon Z-A's dynamic resolution (runtime/nvn/vulkan/exec.c vk_counter_mode)
        extern bool g_za_frame_hook;
        if (g_za_frame_hook) {
            int dr = g_settings.game_drs ? 1 : 0;
            const char *drs[] = {TR("Off (full)", "Desligada (cheia)"), TR("Game's", "Do jogo")};
            if (opt_choice(TR("Dynamic resolution", "Resolução dinâmica"), &dr, drs, 2, 0,
                           TR("Off: the game always renders at its full resolution - sharp. Game's: as on the Switch, it "
                              "lowers the resolution (to about 55%) whenever it thinks the GPU is busy.",
                              "Desligada: o jogo sempre renderiza na resolução cheia - nítido. Do jogo: como no Switch, ele "
                              "baixa a resolução (para uns 55%) quando acha que a GPU está ocupada.")))
                g_settings.game_drs = dr == 1;
        }
    }
    {   // the mode the game is told it runs in (runtime/services/am.c): read when the game starts
        extern bool g_za_frame_hook;
        if (g_za_frame_hook) {
            int hh = g_settings.opmode;
            const char *modes[] = {TR("Auto (handheld on a Steam Deck)", "Auto (portátil no Steam Deck)"),
                                   TR("Docked (1080p)", "Na base (1080p)"), TR("Handheld (720p)", "Portátil (720p)")};
            if (opt_choice(TR("Game mode", "Modo do jogo"), &hh, modes, 3, 0,
                           TR("The mode the game believes it runs in, from the next start. Handheld: the game draws 720p and lighter "
                              "effects, like on the Switch's own screen - far less for the GPU (a Steam Deck: ~25 fps instead of ~12). "
                              "Docked: 1080p. Auto picks handheld on a Steam Deck.",
                              "O modo em que o jogo acha que está, a partir do próximo início. Portátil: o jogo desenha em 720p "
                              "com efeitos mais leves, como na tela do Switch - bem menos trabalho para a GPU (no Steam Deck: "
                              "~25 fps em vez de ~12). Na base: 1080p. Auto escolhe portátil no Steam Deck.")))
                g_settings.opmode = hh;
        }
    }
    {   // HD texture pack (textures/hd: runtime/nvn/vulkan/textures.c hd_switch, tools/za/hd_pack.py): switches at once
        int tx = g_settings.tex_hd ? 1 : 0;
        const char *tex[] = {TR("Original", "Originais"), "HD"};
        if (opt_choice(TR("Textures", "Texturas"), &tx, tex, 2, 1,
                       TR("Original: the game's own. HD: the HD texture pack (textures/hd), where it has a texture - "
                          "sharper, more GPU memory.",
                          "Originais: as do próprio jogo. HD: o pacote de texturas HD (textures/hd), onde ele tiver a "
                          "textura - mais nítido, usa mais memória da GPU.")))
            g_settings.tex_hd = tx == 1;
    }

    opt_section(TR("Rendering", "Renderização"));
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    const char *res[] = {"25%", "50%", "75%", TR("Match display", "Igual à tela"), TR("Dynamic", "Dinâmica"), "1x",
                         TR("1.5x (experimental)", "1.5x (experimental)"), "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, RES_X15, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT, rdef = RES_MATCH_AT;  // (rdef: the GPU's default - Dynamic on an integrated GPU)
        for (int i = 0; i < NRES; i++) {
            if (res_mode[i] == g_settings.res_mode) ri = i;
            if (res_mode[i] == g_default_res_mode) rdef = i;
        }
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[700];
        int n = snprintf(d, sizeof d, TR("How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                         "as sharp as your GPU keeps at full speed. 1x-4x: the Switch's 1080p times that%s. "
                                         "75%% / 50%% / 25%%: a share of the display, for slower machines. Only 1x-4x are "
                                         "exact: at the other sizes (1.5x, Match display, Dynamic, the shares) some objects "
                                         "can vanish for a moment - being fixed.\n\nRendering at %.0f x %.0f.",
                                         "O quão nítida é a imagem 3D. Igual à tela: a resolução da sua própria tela. "
                                         "Dinâmica: o mais nítido que a sua GPU aguenta a toda velocidade. 1x-4x: o 1080p do "
                                         "Switch vezes isso%s. 75%% / 50%% / 25%%: uma parte da tela, para máquinas mais "
                                         "fracas. Só 1x-4x são exatas: nos outros tamanhos (1.5x, Igual à tela, Dinâmica, "
                                         "as partes) alguns objetos podem sumir por um instante - sendo corrigido.\n\n"
                                         "Renderizando em %.0f x %.0f."),
                         g_gpu_integrated ? TR(" (1x: 720p on this GPU)", " (1x: 720p nesta GPU)") : "", 1920.0 * sx, 1080.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, TR(" Dynamic: %.0f%% of the display right now.", " Dinâmica: %.0f%% da tela agora."),
                     output_scale_dynamic_factor() * 100);
        if (opt_choice(TR("Internal resolution", "Resolução interna"), &ri, res, NRES, rdef, d, false))
            g_settings.res_mode = res_mode[ri], g_settings.res_auto = false;
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        const char *ups[] = {TR("Bilinear", "Bilinear"), "MetalFX"};
        const char *ud = TR("How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                            "upscaler, sharper. Bilinear: plain and soft.",
                            "Como a imagem é ampliada para a janela quando renderiza numa resolução menor. MetalFX: o "
                            "upscaler da Apple, mais nítido. Bilinear: simples e suave.");
#else
        const char *ups[] = {TR("Bilinear", "Bilinear"), "AMD FSR 1"};
        const char *ud = TR("How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                            "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.",
                            "Como a imagem é ampliada para a janela quando renderiza numa resolução menor. AMD FSR 1: "
                            "ampliação que respeita as bordas, com nitidez - mais nítido. Bilinear: simples e suave.");
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[700];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\n%s", ud,
                     TR("Not used at Match display: the game already renders at your display's resolution.",
                        "Não é usado em Igual à tela: o jogo já renderiza na resolução da sua tela."));
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Upscaler", &ui, ups, 2, g_default_upscaler == UPSCALE_BILINEAR ? 0 : 1, ud))
            g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR, g_settings.upscaler_auto = false;
    }
    const char *aa[] = {TR("Off", "Desligado"), "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               TR("Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
                  "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.",
                  "Suaviza as bordas serrilhadas da imagem antes de ela ser ampliada para a janela. SMAA: acha as bordas "
                  "e mistura só ao longo delas, então as texturas continuam nítidas. Ajuda mais numa resolução interna menor."));
    {   // shadow map size (vulkan/textures.c tex_shadow_scale: taken at start) and filter width (vulkan/exec.c texture_info)
        const char *sz[] = {TR("Game's", "Do jogo"), "2x", "3x", "4x"};
        int si = g_settings.shadow_res - 1;
        if (opt_choice(TR("Shadow resolution", "Resolução das sombras"), &si, sz, 4, 0,
                       TR("The size of the map the sun's shadows are drawn from (the game's: 1024 x 1024). Higher: smoother "
                          "shadow edges, without the stair steps, at a little GPU time and more GPU memory (4x: about 240 MB, "
                          "2x: 60 MB). From the next start.",
                          "O tamanho do mapa de onde saem as sombras do sol (o do jogo: 1024 x 1024). Maior: bordas das "
                          "sombras mais lisas, sem os degraus, por um pouco de tempo da GPU e mais memória da GPU (4x: uns "
                          "240 MB, 2x: 60 MB). A partir do próximo início.")))
            g_settings.shadow_res = si + 1;
        int sh = g_settings.shadow_sharp - 1;
        const char *shd = TR("How soft the edges of the sun's shadows are: the game's own blur, or 2-4 times narrower - "
                             "sharper shadows. At most the shadow resolution (its blur is measured in the map's texels). "
                             "Changes at once.",
                             "O quanto as bordas das sombras do sol são suaves: o desfoque do próprio jogo, ou 2-4 vezes "
                             "mais estreito - sombras mais nítidas. No máximo a resolução das sombras (o desfoque é medido "
                             "nos texels do mapa). Muda na hora.");
        if (g_settings.shadow_res == 1) opt_disable_next();  // (nothing to narrow at the game's size)
        if (opt_choice(TR("Shadow sharpness", "Nitidez das sombras"), &sh, sz, 4, 0, shd))
            g_settings.shadow_sharp = sh + 1;
    }
#ifndef __APPLE__
    {   // (runtime/nvn/vulkan/exec.c post_process)
        const char *sh[] = {"Auto", TR("Off", "Desligada"), TR("Low", "Baixa"), TR("Medium", "Média"), TR("High", "Alta")};
        opt_choice(TR("Sharpening", "Nitidez"), &g_settings.sharpen, sh, SHARPEN_N, SHARPEN_AUTO,
                   TR("Sharpens the image in the window (AMD FSR 1's contrast-adaptive sharpening), at any internal "
                      "resolution. Auto: only after the upscaler's own upscale, as before. Off: never. Low / Medium / High: "
                      "always, stronger each step.",
                      "Deixa a imagem na janela mais nítida (a nitidez adaptativa do AMD FSR 1), em qualquer resolução "
                      "interna. Auto: só depois da ampliação do upscaler, como antes. Desligada: nunca. Baixa / Média / "
                      "Alta: sempre, mais forte a cada passo."));
    }
#endif
    {   // GTAO (vulkan/exec.c gtao_*): Pokémon Z-A has no ambient occlusion of its own
        const char *ao[] = {TR("Off", "Desligado"), TR("Low", "Baixa"), TR("Medium", "Média"), TR("High", "Alta")};
        opt_choice(TR("Ambient occlusion", "Oclusão ambiente"), &g_settings.gtao, ao, 4, 0,
                   TR("Soft shadows where things meet: the base of walls and lamp posts, under bushes and characters, in "
                      "corners (GTAO). Higher: smoother, at more GPU time (about 1-2 ms at 1.5x). Changes at once.",
                      "Sombras suaves onde as coisas se encontram: a base de paredes e postes, sob arbustos e personagens, "
                      "nos cantos (GTAO). Maior: mais suave, por mais tempo da GPU (cerca de 1-2 ms em 1.5x). Muda na hora."));
        static const int pct[] = {25, 50, 75, 100};
        const char *ps[] = {"25%", "50%", "75%", "100%"};
        int pi = 3;
        for (int i = 0; i < 4; i++) if (pct[i] == g_settings.gtao_strength) pi = i;
        if (g_settings.gtao == 0) opt_disable_next();
        if (opt_choice(TR("Ambient occlusion strength", "Intensidade da oclusão ambiente"), &pi, ps, 4, 3,
                       TR("How dark those soft shadows get: 100% the default, lower for a subtler look. The same GPU time "
                          "at any strength. Changes at once.",
                          "O quão escuras essas sombras suaves ficam: 100% o padrão, menos para um visual mais sutil. O "
                          "mesmo tempo da GPU em qualquer intensidade. Muda na hora.")))
            g_settings.gtao_strength = pct[pi];
    }
    {   // indirect light (vulkan/exec.c ssgi_apply): one bounce of the game's light from what is on screen
        const char *gi[] = {TR("Off", "Desligada"), TR("Low", "Baixa"), TR("Medium", "Média"), TR("High", "Alta")};
        opt_choice(TR("Indirect light", "Luz indireta"), &g_settings.ssgi, gi, 4, 0,
                   TR("Light bouncing off what is on screen (screen-space global illumination): sunlit ground and walls "
                      "light up shaded corners, characters and walls nearby in their own colour. Strongest in shade and "
                      "near colourful walls. Higher: smoother, at more GPU time (about 0.5 ms). Changes at once.",
                      "Luz refletida pelo que está na tela (iluminação global em espaço de tela): chão e paredes ao sol "
                      "iluminam cantos na sombra, personagens e paredes próximas com a sua cor. Mais forte na sombra e "
                      "perto de paredes coloridas. Maior: mais suave, por mais tempo da GPU (cerca de 0,5 ms). Muda na hora."));
        static const int gp[] = {50, 100, 150, 200, 300};
        const char *gps[] = {"50%", "100%", "150%", "200%", "300%"};
        int gpi = 1;
        for (int i = 0; i < 5; i++) if (gp[i] == g_settings.ssgi_strength) gpi = i;
        if (g_settings.ssgi == 0) opt_disable_next();
        if (opt_choice(TR("Indirect light strength", "Intensidade da luz indireta"), &gpi, gps, 5, 1,
                       TR("How much light bounces: 100% one physical bounce, more for a brighter, more colourful look. "
                          "The same GPU time at any strength. Changes at once.",
                          "Quanta luz é refletida: 100% um reflexo físico, mais para um visual mais claro e colorido. O "
                          "mesmo tempo da GPU em qualquer intensidade. Muda na hora.")))
            g_settings.ssgi_strength = gp[gpi];
    }
    opt_toggle(TR("Water reflections", "Reflexos na água"), &g_settings.water_ssr, false,  // (vulkan/exec.c water_apply)
               TR("Fountains, ponds and the sea mirror the buildings, characters and sky around them, with gentle "
                  "moving ripples; where the picture has nothing to mirror, the game's own sky reflection. Most visible "
                  "at low angles. Changes at once.",
                  "Fontes, lagos e o mar refletem os prédios, personagens e o céu ao redor, com ondulações suaves; onde "
                  "a imagem não tem o que refletir, o reflexo do céu do próprio jogo. Mais visível em ângulos baixos. "
                  "Muda na hora."));
#ifndef __APPLE__
    if (!la) {  // (runtime/nvn/vulkan/exec.c fsr2_copy: applies at once)
        int t = g_settings.taa ? 1 : 0;
        const char *taa[] = {TR("Game's", "Do jogo"), "AMD FSR 2"};
        if (opt_choice(TR("Temporal anti-aliasing", "Anti-aliasing temporal"), &t, taa, 2, 0,
                       TR("AMD FSR 2 in place of the game's own edge smoothing: it combines several frames, so fences, "
                          "railings, leaves and thin lines stay smooth and stop flickering as you move. Uses a little GPU "
                          "time. With it on, Anti-aliasing (SMAA) above isn't needed.",
                          "AMD FSR 2 no lugar da suavização de bordas do próprio jogo: ele combina vários quadros, então "
                          "grades, corrimões, folhas e linhas finas ficam lisos e param de tremular quando você anda. Usa um "
                          "pouco da GPU. Com ele ligado, o Anti-aliasing (SMAA) acima não é necessário.")))
            g_settings.taa = t == 1;
    }
#endif

    if (la) {
        opt_section("Game");
        opt_toggle("Remove blur", &g_settings.blur_removal, false,
                   "Turns off the tilt-shift blur at the top and bottom of the screen.");
        opt_toggle("Hide Hero Mode icon", &g_settings.hide_hero_icon, false,
                   "Hides the Hero Mode emblem in the corner of the screen while playing a Hero Mode file.");
    }
    opt_end();
}

extern "C" bool cosmetics_seed_on(void);  // runtime/cosmetics_seed.c

// Cosmetics: tunic / heart colours, live (runtime/nvn/texpatch.c), and recolours by draw (runtime/nvn/recolour*.c);
// a seed made with "Random cosmetics" fills the rows left on Default (shown as "Seed: <colour>")
static void tab_cosmetics(void) {
    opt_begin("##cosmetics", ImGui::GetFontSize() / 34);
    CosColour *rows[] = {&g_settings.tunic, &g_settings.hearts, &g_settings.hair, &g_settings.skin, &g_settings.shield,
                         &g_settings.leaves, &g_settings.grass, &g_settings.sand};
    if (opt_button("Randomize all", "Every color below gets a random color."))
        for (CosColour *c : rows) c->pick = 1 + rand() % (COL_RANDOM - 1);  // (a named colour: shown as what it is)
    if (opt_button("Default all", "Everything on this page back to the game's own look.")) {
        for (CosColour *c : rows) c->pick = COL_DEFAULT;
        g_settings.enemy_colours = 0;
        g_settings.screen_filter = 0;
    }
    opt_section("Colors");
    opt_colour("Tunic color", &g_settings.tunic, COL_SLOT_TUNIC,
               "Link's tunic color. Default: the game's own (or the randomizer seed's).");
    opt_colour("Heart color", &g_settings.hearts, COL_SLOT_HEARTS, "The color of the hearts on the HUD.");
    opt_colour("Hair color", &g_settings.hair, COL_SLOT_HAIR, "Link's hair color.");
    opt_colour("Body color", &g_settings.skin, COL_SLOT_SKIN, "Link's body color.");  // (larecomp.ini: skin)
    opt_colour("Shield color", &g_settings.shield, COL_SLOT_SHIELD, "The color of Link's shield.");
    opt_section("Overworld");
    {   // Season: fills Leaves, Grass and Sand at once (palette: Orange 2, Gold 3, Pink 9, White 10); "Mixed" when
        // they match none
        static const char *const names[] = {"Spring", "Autumn", "Winter", "Cherry blossom", "Mixed"};
        static const int preset[4][3] = {{COL_DEFAULT, COL_DEFAULT, COL_DEFAULT}, {2, 3, 3}, {10, 10, 10},
                                         {9, COL_DEFAULT, COL_DEFAULT}};
        int cur = 4;  // (as shown: a row on Default shows the seed's colour, if it has its own)
        for (int i = 0; i < 4; i++)
            if (col_pick(&g_settings.leaves, COL_SLOT_LEAVES) == preset[i][0] &&
                col_pick(&g_settings.grass, COL_SLOT_GRASS) == preset[i][1] &&
                col_pick(&g_settings.sand, COL_SLOT_SAND) == preset[i][2])
                cur = i;
        int v = cur;
        if (opt_choice("Season", &v, names, 5, 0, "Sets the leaves, grass and sand colors below.") && v != cur) {
            if (v == 4) v = cur == 0 ? 3 : 0;  // (Mixed is never chosen: past the ends, back round)
            g_settings.leaves.pick = preset[v][0];
            g_settings.grass.pick = preset[v][1];
            g_settings.sand.pick = preset[v][2];
        }
    }
    opt_colour("Leaves color", &g_settings.leaves, COL_SLOT_LEAVES, "The overworld's tree leaves and bushes.");
    opt_colour("Grass color", &g_settings.grass, COL_SLOT_GRASS, "The overworld's grass.");
    opt_colour("Sand color", &g_settings.sand, COL_SLOT_SAND, "The beaches, the desert and the sandy south.");
    opt_section("Enemies");
    {   // (Off in a seed with its own cosmetics: "Seed", its enemies are per seed)
        const char *modes[N_ENEMY_COLOUR_MODES];
        for (int i = 0; i < N_ENEMY_COLOUR_MODES; i++) modes[i] = g_enemy_colour_modes[i];
        if (cosmetics_seed_on()) modes[0] = "Seed";
        opt_choice("Enemy colors", &g_settings.enemy_colours, modes, N_ENEMY_COLOUR_MODES, 0,
                   "Each kind of enemy in another color. Enemies whose color shows their strength or type keep theirs.");
    }
    opt_section("Screen");
    opt_choice("Screen filter", &g_settings.screen_filter, g_screen_filters, N_SCREEN_FILTERS, 0,
               "A filter over the whole picture, the HUD included.");
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section(TR("Sound", "Som"));
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider(TR("Volume", "Volume"), &pct, 0, 200, 5, 100, "%g%%",
                   TR("The game's volume. 100% is the game's own level; above it, louder.",
                      "O volume do jogo. 100% é o nível do próprio jogo; acima disso, mais alto.")))
        g_settings.volume = pct / 100.0f;
    opt_toggle(TR("Room reverb", "Eco dos ambientes"), &g_settings.reverb, true,
               TR("The game's echo in caves, buildings and big rooms.", "O eco do jogo em cavernas, prédios e salas grandes."));
    opt_toggle(TR("Movie audio", "Som dos vídeos"), &g_settings.movie_audio, true,
               TR("Sound in the game's movies. Applies the next time a movie starts.",
                  "O som dos vídeos do jogo. Vale a partir do próximo vídeo."));
    opt_end();
}

// Quit: back to the start screen (in game: the game reboots to it) or out to the desktop. Each asks first (progress
// since the last save is lost); B / Esc or Cancel backs out.
extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
static void tab_quit(float scale, bool in_game) {
    static int ask;  // 0: the two choices; 1: start screen?; 2: desktop?
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = 0;
    const char *lost = in_game ? TR("\n\nProgress since your last save is lost.", "\n\nO progresso desde o último save se perde.") : "";
    char d1[300], d2[300];
    snprintf(d1, sizeof d1, "%s%s", TR("Leaves the game and goes back to the ZA Recomp start screen.",
                                       "Sai do jogo e volta para a tela inicial do ZA Recomp."), lost);
    snprintf(d2, sizeof d2, "%s%s", TR("Closes ZA Recomp.", "Fecha o ZA Recomp."), lost);
    opt_begin("##quit", scale);
    opt_section(TR("Quit", "Sair"));
    if (!ask) {
        if (!in_game) opt_disable_next();  // (already on it)
        if (opt_action(TR("Quit to Start Screen", "Sair para a tela inicial"), "",
                       in_game ? d1 : TR("You're on the start screen.", "Você já está na tela inicial."))) ask = 1;
        if (opt_action(TR("Quit to Desktop", "Sair para a área de trabalho"), "", d2)) ask = 2;
    } else {
        if (opt_action(ask == 1 ? TR("Yes, quit to Start Screen", "Sim, sair para a tela inicial")
                                : TR("Yes, quit to Desktop", "Sim, sair para a área de trabalho"), "", ask == 1 ? d1 : d2)) {
            settings_save();
            if (ask == 1) {
                rt_request_reboot(2);
                set_open(false);
            } else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            ask = 0;
        }
        if (opt_action(TR("Cancel", "Cancelar"), "", TR("Stays where you are.", "Continua onde está."))) ask = 0;
    }
    opt_end();
}

// Pokémon Z-A's Game page: its text language, and how to reach this menu
static void tab_game(void) {
    opt_begin("##game", ImGui::GetFontSize() / 34);
    opt_section(TR("Game", "Jogo"));  // (its button icons: the Controls tab, with the rest of the controller)
    {   // what the game is told the console's language is: its text follows this (runtime/settings.c)
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", settings_language_name());
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
        opt_choice(TR("Game language", "Idioma do jogo"), &g_settings.language, names, N_LANGUAGES, LANG_AUTO,
                   TR("The game's text language, as its own system setting. Applies when the game restarts: Quit > "
                      "Start Screen, then Play. (A translation mod, like a PT-BR one, goes in Mods on the start screen.)",
                      "O idioma dos textos do jogo, como a configuração do próprio sistema. Vale quando o jogo reinicia: "
                      "Sair > Tela inicial, depois Jogar. (Um mod de tradução, como o PT-BR, vai em Mods na tela inicial.)"));
    }
    opt_section(TR("This menu", "Este menu"));
    opt_button(TR("Shortcuts", "Atalhos"),
               TR("Open / close: F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start.\n"
                  "Change tabs: L / R (Q / E). Back: B (Esc).\nFullscreen: F11 or Alt+Enter.",
                  "Abrir / fechar: F1, Esc, o botão Home / Xbox / PlayStation do controle, ou Back + Start.\n"
                  "Trocar de aba: L / R (Q / E). Voltar: B (Esc).\nTela cheia: F11 ou Alt+Enter."));
    opt_end();
}

extern "C" bool inventory_overlay_wanted(void);
extern "C" bool tracker_overlay_wanted(void);
extern "C" bool tracker_dungeon_wanted(void);
extern "C" void tracker_dungeon_draw(ImVec2 display, ImFont *font);
extern "C" void tracker_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool chaos_overlay_wanted(void);
extern "C" void chaos_overlay_draw(ImVec2 display, ImFont *font);
extern "C" void inventory_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool race_overlay_wanted(void);
extern "C" void race_overlay_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool pickup_flyin_wanted(void);  // (silent pickups: pickup_flyin.mm)
extern "C" void pickup_flyin_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool race_overlay_key(char *out, size_t cap);

// ---- reusing the overlay's last drawing (LARECOMP_OVERLAY_CACHE=0: off, a new ImGui frame every present as before)
// While only the FPS counter and / or the run timer are up (no menu, no input, nothing interactive or animated), the
// overlay draws the same thing frame after frame: ImGui's NewFrame + layout + Render then run for nothing on the
// render thread. Their drawing is described by a key (the text shown, the window size); while it stays the same, the
// last frame's draw data (ImGui keeps it until the next NewFrame) is drawn again. Only after three frames in a row
// built with that key: an auto-sized window (the FPS counter) settles over the frames after its text changes.
// The FPS counter then shows the same average as ImGui's io.Framerate (the last 60 frame times), sampled twice a
// second instead of every frame.
static int g_ov_cache = -1;
static char g_ov_key[96];
static int g_ov_same;  // frames built in a row with g_ov_key
static float g_fps_dt[60];
static int g_fps_n, g_fps_i;
static double g_fps_sum, g_fps_shown, g_fps_at;
static void fps_note(double now, float dt) {
    if (g_fps_n == 60) g_fps_sum -= g_fps_dt[g_fps_i];
    else g_fps_n++;
    g_fps_dt[g_fps_i] = dt;
    g_fps_sum += dt;
    g_fps_i = (g_fps_i + 1) % 60;
    if (now - g_fps_at >= 0.5 || now < g_fps_at) {
        g_fps_at = now;
        g_fps_shown = g_fps_sum > 0 ? g_fps_n / g_fps_sum : 0;
    }
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.95f, 0.71f, 0.27f, 1), "%s", TR("Enter your name", "Digite seu nome"));
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        int units = kbd_cut(g_kbd_text, g_kbd_max);
        ImGui::TextDisabled(TR("Enter  confirm      Esc  cancel      (%d / %d)", "Enter  confirma      Esc  cancela      (%d / %d)"),
                            units, g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    // Re-present only while the game itself doesn't: a game with its own render thread (Pokémon Z-A) keeps presenting
    // (its menu_render draws this dialog), and two threads in the backend at once break it (ImGui, Vulkan)
    extern uint64_t renderer_present_count(void);
    uint64_t seen = renderer_present_count(), quiet_since = SDL_GetTicks();
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        if (renderer_present_count() != seen) {
            seen = renderer_present_count();
            quiet_since = t0;
        } else if (t0 - quiet_since >= 250) {
            backend_represent();
            seen = renderer_present_count();
        }
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    kbd_cut(g_kbd_text, g_kbd_max);  // (whatever entered it: typed, on-screen keys, the test hook)
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// LARECOMP_MENU_FOCUS_LOG=1 (test; tools/menu_focus_sweep.sh): every Down / Up on a page of option rows is checked a
// few frames later against the rows' order and logged "[menu] focus check ... OK / FAIL": the first Down after a tab
// switch (however it came) or the menu opening, and Down from the tab bar, land on the page's first enabled row; Down
// goes to the next row (the last one stays), Up to the row before, and Up from the top row onto the selected tab.
static void focus_check(bool down, bool up, bool any_down, int focused_tab, int tab, const char *tab_name) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_MENU_FOCUS_LOG") != NULL;
    if (!on) return;
    static int shown = -1000, due_in, want, from, prev_focus = -1;  // (want: a row index, -1: the selected tab)
    static bool first_down_due;
    static const char *what;
    if (tab != shown || g_page_fresh_seen) shown = tab, first_down_due = true, g_page_fresh_seen = false, prev_focus = -1;
    int n = opt_row_count(), first = opt_first_row(), focus = opt_focused_row();
    if (any_down && !down) first_down_due = false;  // (a held Down's repeat: the first Down was checked or skipped)
    if ((down || up) && !due_in && n > 0) {
        from = prev_focus, due_in = 4;
        if (down && (first_down_due || from < 0)) want = first, what = first_down_due ? "first Down" : "Down from the tab bar";
        else if (down) want = from + 1 < n ? from + 1 : from, what = "Down";
        else if (from > 0) want = from - 1, what = "Up";
        else if (from == 0) want = -1, what = "Up from the top row";
        else due_in = 0;  // (Up on the tab bar: not a page move)
        if (down) first_down_due = false;
    } else if (down && n == 0 && first_down_due) {
        first_down_due = false;
        LOG("[menu] focus check: first Down on %s SKIP (no option rows)\n", tab_name);
    }
    prev_focus = focus;
    if (!due_in || --due_in) return;
    bool ok = want >= 0 ? focus == want : focused_tab == tab;
    char got[160];
    if (focus >= 0) snprintf(got, sizeof got, "\"%s\"", opt_row_label(focus));
    else snprintf(got, sizeof got, "tab %d", focused_tab);
    char exp[160];
    if (want >= 0) snprintf(exp, sizeof exp, "\"%s\"", opt_row_label(want));
    else snprintf(exp, sizeof exp, "tab %d", tab);
    LOG("[menu] focus check: %s on %s from %s -> %s (want %s) %s\n", what, tab_name,
        from >= 0 ? opt_row_label(from) : "the tab bar", got, exp, ok ? "OK" : "FAIL");
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[12];
    int ntabs = 0;
    // Pokémon Z-A (ZA Recomp's start screen, or the game loaded): Controls, Video, Audio, Game, Quit. Link's Awakening
    // keeps its own set (randomizer, cheats, warp, save editor...). (ids stay: test hooks)
    menu_la_tabs = in_game && g_game_is_la;
    if (prelaunch) tabs[ntabs++] = {TR("Files", "Arquivos"), -1, true};
    if (!menu_la_tabs) {
        const char *names[] = {TR("Controls", "Controles"), TR("Video", "Vídeo"), TR("Audio", "Áudio"), TR("Game", "Jogo"),
                               TR("Quit", "Sair")};
        static const int ids[] = {8, 0, 1, 10, 9};
        for (int i = 0; i < 5; i++)
            if (ids[i] != 9 || !prelaunch) tabs[ntabs++] = {names[i], ids[i], true};
    } else {
        static const char *names[] = {"Video", "Audio", "Save Editor", "Randomizer", "Warp", "Cheats", "Gameplay",
                                      "Cosmetics", "Controls", "Quit"};
        static const int order[] = {8, 0, 1, 6, 7, 3, 5, 4, 2, 9};  // Controls, Video, Audio, Gameplay, Cosmetics,
                                                                    // Randomizer, Cheats, Warp, Save Editor, Quit
        for (int i : order)  // (Quit: in game only - the start screen has its own QUIT, and an eleventh tab cut names short)
            if (i != 9 || !prelaunch) tabs[ntabs++] = {names[i], i, in_game || (i != 2 && i != 4 && i != 5)};
    }
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    bool switching = false;  // (L / R this frame: the page changes next frame)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor goes onto the new tab (left behind on the tab bar, A on it switched straight back - user
            // 2026-10-11; left in the old page, it stayed on a row that is gone, and Down went on from where that row
            // had been)
            g_focus_tab = tabs[i].id, g_focus_frames = 0;
            switching = true;
        }
    }
    bool down = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    bool up = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp) || ImGui::IsKeyPressed(ImGuiKey_UpArrow));
    // The first Down on a page just switched to (L / R, a click or A on a tab, the menu opening), or Down with the
    // cursor on the tab bar: the page's first enabled row, top to bottom - never ImGui's pick from wherever its cursor
    // was (a row of the page before, the tab bar's x). Option rows take it (opt_focus_first); a page without them gets
    // its first control (g_page_focus_next).
    if (ImGui::GetIO().MouseClicked[0]) g_page_fresh = false;  // (the mouse went into the page)
    bool down_from_tab = down && !switching &&
                         ((focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible) || g_page_fresh);
    if (down_from_tab) {
        ImGui::NavMoveRequestCancel();  // (ImGui's own move this frame)
        g_page_fresh = false;
        g_focus_tab = -99, g_focus_frames = 0;  // (the cursor stops going back to the tab)
    }
    opt_focus_first(down_from_tab);
    // Up / Down on a page of option rows: the next / previous row in order (opt_nav_step), not ImGui's spatial pick
    if ((down || up) && !down_from_tab && !switching && opt_nav_step(down ? 1 : -1)) ImGui::NavMoveRequestCancel();
    opt_frame_reset();
    // The cursor moved out of the page (ImGui's pick, applied this frame for last frame's press - Up from the page's
    // top control): onto the selected tab. ImGui's pick was the tab nearest the control's x (Controls, from most
    // rows), or the close button
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiWindow *root = ImGui::GetCurrentWindow()->RootWindow;
    static bool nav_in_page;  // (the frame before: the cursor on a control of the page - its child windows)
    if (nav_in_page && g.NavJustMovedToId && g.NavWindow == root && !any_popup) g_focus_tab = -100, g_focus_frames = 0;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[80];
            // (the tab's id after ###: its label changes with the language, its selection mustn't)
            snprintf(lbl, sizeof lbl, menu_la_tabs ? (prelaunch ? " %s ###t%d" : "  %s  ###t%d") : "   %s   ###t%d", t.name, t.id);
            ImGui::BeginDisabled(!t.on);  // (out of the game: Save Editor, Warp, Cheats)
            // focus (controller / keyboard): the tab lights up a little with a gold bar along its bottom, instead of
            // ImGui's outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (lit) {
                ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(1, 1, 1, 0.14f));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(1, 1, 1, 0.14f));
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(lit ? 3 : 1);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 4 * scale > 2 ? 4 * scale : 2;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - h), ImVec2(b.x - 2, b.y),
                                                          IM_COL32(242, 181, 68, 255));  // (the menu's gold)
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                if (t.id != g_shown_tab) g_shown_tab = t.id, g_page_fresh = true, g_page_focus_next = false;
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                if (g_page_focus_next) {  // (a page without option rows: its first control)
                    g_page_focus_next = false;
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 1: tab_audio(); break;
                case 2:
                    if (game_in_play()) save_editor_draw(scale);
                    else ImGui::TextDisabled("Load a save file and get into the game to edit it.");
                    break;
                case 3: rando_tab_draw(scale); break;
                case 4: if (warp_tab_draw(scale)) set_open(false); break;
                case 5: cheats_draw(scale); break;
                case 6: difficulty_draw(scale); break;
                case 7: tab_cosmetics(); break;
                case 8: controls_tab_draw(scale); break;
                case 9: tab_quit(scale, in_game); break;
                case 10: tab_game(); break;
                }
                static const char *scroll_test = getenv("LARECOMP_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                // no enabled option row took the cursor: the page's first control, next frame (SetKeyboardFocusHere:
                // the first item that takes the cursor, in order)
                if (opt_focus_first_left() && down_from_tab) g_page_focus_next = true;
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first_left();  // (no page drawn: nothing took it)
        ImGui::EndTabBar();
    }
    nav_in_page = g.NavWindow && g.NavWindow != g.NavWindow->RootWindow && g.NavWindow->RootWindow == root;
    // (checks: fresh presses only - a held key's repeats come faster than a check's few frames)
    bool down1 = down && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, false));
    bool up1 = up && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false));
    for (int k = 0; k < ntabs; k++)
        if (tabs[k].id == g_tab) focus_check(down1, up1, down, focused_tab, g_tab, tabs[k].name);
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

// Whether menu_render draws anything: the menu, the keyboard, an overlay (FPS, inventory, tracker, chaos, race, a
// pickup card).
static bool render_needed(bool open, bool kbd, bool inv, bool trk, bool trkd, bool chaos, bool race, bool flyin) {
    return open || kbd || g_settings.show_fps || inv || trk || trkd || chaos || race || flyin;
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load();
    bool inv = !open && inventory_overlay_wanted();
    bool trk = !open && tracker_overlay_wanted();
    bool trkd = !open && tracker_dungeon_wanted();
    bool chaos = !open && chaos_overlay_wanted();
    bool kbd = g_kbd.load() == 1;
    bool notice = SDL_GetTicks() < g_notice_until.load();  // (menu_notice: a line at the bottom for a few seconds)
    bool race = race_overlay_wanted();  // (run timer, race seed hash: race_overlay.mm)
    bool flyin = pickup_flyin_wanted() && !open;  // (a silent pickup's card)
    if (!render_needed(open, kbd, inv, trk, trkd, chaos, race, flyin) && !notice) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        g_ov_same = 0;  // (shown again: built afresh first)
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    if (g_ov_cache < 0) g_ov_cache = !getenv("LARECOMP_OVERLAY_CACHE") || atoi(getenv("LARECOMP_OVERLAY_CACHE")) != 0;
    char key[sizeof g_ov_key];
    key[0] = 0;
    if (g_ov_cache) {
        fps_note(now, io.DeltaTime);
        char rk[64];
        // (only the FPS counter and the run timer; input only reaches ImGui while the menu or the name entry is up)
        if (!open && !kbd && !inv && !trk && !trkd && !chaos && !flyin && race_overlay_key(rk, sizeof rk)) {
            pthread_mutex_lock(&q_lock);
            bool events = !q_events.empty();
            pthread_mutex_unlock(&q_lock);
            if (!events) snprintf(key, sizeof key, "%.0fx%.0f fps %d %.0f | %s", tw, th, (int)g_settings.show_fps,
                                  g_settings.show_fps ? g_fps_shown : 0.0, rk);
        }
        ImDrawData *dd = ImGui::GetDrawData();
        if (key[0] && g_ov_same >= 3 && !strcmp(key, g_ov_key) && dd && dd->Valid) {
            g_text_busy = false;  // (the menu is closed)
#ifdef __APPLE__
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            ImGui_ImplMetal_NewFrame(rp);  // (the backend's target description only)
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_RenderDrawData(dd, cb, enc);
            [enc endEncoding];
#else
            void *oc = vk_overlay_begin(cmd);  // (as below)
            ImGui_ImplVulkan_RenderDrawData(dd, (VkCommandBuffer)oc);
            vk_overlay_end(cmd, oc);
#endif
            return;
        }
        if (key[0] && !strcmp(key, g_ov_key)) g_ov_same++;
        else { memcpy(g_ov_key, key, sizeof key); g_ov_same = key[0] ? 1 : 0; }
    }
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (g_settings.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps", g_ov_cache ? g_fps_shown : io.Framerate);
        ImGui::End();
    }

    if (notice) {  // menu_notice: a line at the bottom for a few seconds
        char text[sizeof g_notice_text];
        pthread_mutex_lock(&q_lock);
        memcpy(text, g_notice_text, sizeof text);
        pthread_mutex_unlock(&q_lock);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 40 * scale), 0, ImVec2(0.5f, 1));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(io.DisplaySize.x * 0.8f, FLT_MAX));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##notice", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PushTextWrapPos(io.DisplaySize.x * 0.75f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::End();
    }

    if (race) race_overlay_draw(io.DisplaySize, g_menu_font_big, scale);
    if (flyin) pickup_flyin_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont(), scale);
    if (trk) tracker_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (chaos) chaos_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (trkd) tracker_dungeon_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (inv) {
        inventory_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    }

    if (open) {
        // dim the game behind the menu (towards the start screen's night blue)
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(4, 10, 19, 150));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header as on the start screen's Settings: the title, and a close X in the corner (B / Esc too). ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.)
        {   // ZA Recomp's mark (a hexagon with a Z, as on the start screen), then the title
            float u = ImGui::GetFontSize() * 1.15f / 30;
            ImVec2 o = ImGui::GetCursorScreenPos();
            o.y += ImGui::GetFontSize() * 0.35f;
            auto Q = [&](float x, float y) { return ImVec2(o.x + x * u, o.y + y * u); };
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 hex[] = {Q(15, 2), Q(26, 8.5f), Q(26, 21.5f), Q(15, 28), Q(4, 21.5f), Q(4, 8.5f)};
            dl->AddPolyline(hex, 6, IM_COL32(242, 181, 68, 255), ImDrawFlags_Closed, 2 * u);
            ImVec2 z[] = {Q(10.5f, 10.5f), Q(19.5f, 10.5f), Q(10.5f, 19.5f), Q(19.5f, 19.5f)};
            dl->AddPolyline(z, 4, IM_COL32(238, 243, 246, 255), 0, 2.4f * u);
            ImGui::Dummy(ImVec2(32 * u, 1));
            ImGui::SameLine();
        }
        ImGui::PushFont(g_font_big);
        ImGui::TextColored(ImVec4(0.95f, 0.71f, 0.27f, 1), "%s", TR("Settings", "Configurações"));
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? IM_COL32(242, 181, 68, 255) : IM_COL32(194, 209, 219, 255);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && !controls_ui_took_back() && !rando_tab_took_back() &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    // ImGui records its commands itself. Not into cmd, the window image's command buffer, while the recorder thread
    // (record.c) may still be recording it (one thread at a time on a command buffer and its pool: f054d8b, the device
    // loss entering Mabe Village), and without waiting for the recorder to catch up every frame either (a full drain:
    // 20-30 ms a frame on the Steam Deck): into a secondary command buffer of device.c's, which the recorder executes
    // in cmd in order - after the uploads queued above (overlay icons). Decided right here, where the draw data is
    // final, so an overlay turning on during this function can't miss it. (Recorder off: into cmd directly.)
    void *oc = vk_overlay_begin(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)oc);  // inside the window image's rendering
    vk_overlay_end(cmd, oc);
#endif
}
