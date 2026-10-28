// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard WASD left stick, arrows
// d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus, numpad 8/4/5/6 right stick;
// controller by position, Switch A = east).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"
#include "../rumble.h"
#include "../led.h"
#include "../ui/menu.h"

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;
static _Atomic int64_t sampling;
static bool host_test;  // scripted runs must not vibrate the user's controller

// Guest IPC threads publish values; only the UI thread touches SDL's pad.
// The lock also makes paired value snapshots consistent with permission/reset.
static SDL_SpinLock rumble_lock;
static RumbleValue rumble_values[2];
static bool rumble_allowed = true;
static _Atomic uint64_t rumble_frame_ms;
static uint16_t rumble_last_lo, rumble_last_hi;
static uint64_t rumble_last_ms;
static bool rumble_warned;

static void rumble_clear(void) {
    SDL_LockSpinlock(&rumble_lock);
    memset(rumble_values, 0, sizeof rumble_values);
    SDL_UnlockSpinlock(&rumble_lock);
}
void input_rumble_send(uint32_t h, const RumbleValue *v) {
    int side = rumble_side(h);
    SDL_LockSpinlock(&rumble_lock);
    if (side < 0) { SDL_UnlockSpinlock(&rumble_lock); return; }
    if (rumble_allowed) rumble_values[side] = *v;
    SDL_UnlockSpinlock(&rumble_lock);
}
RumbleValue input_rumble_get(uint32_t h) {
    RumbleValue v = {0, 160, 0, 320};
    int side = rumble_side(h);
    SDL_LockSpinlock(&rumble_lock);
    if (side >= 0 && rumble_allowed) v = rumble_values[side];
    SDL_UnlockSpinlock(&rumble_lock);
    return v;  // requested value, not physical motor feedback
}
void input_rumble_permit(bool allowed) {
    SDL_LockSpinlock(&rumble_lock);
    rumble_allowed = allowed;
    if (!allowed) memset(rumble_values, 0, sizeof rumble_values);
    SDL_UnlockSpinlock(&rumble_lock);
}
bool input_rumble_permitted(void) {
    SDL_LockSpinlock(&rumble_lock);
    bool allowed = rumble_allowed;
    SDL_UnlockSpinlock(&rumble_lock);
    return allowed;
}
void input_rumble_poll(void) {
    uint64_t now = SDL_GetTicks();
    uint64_t last_frame = atomic_load(&rumble_frame_ms);
    uint16_t lo = 0, hi = 0;
    // Switch values are latched until changed: they contain no duration.
    // Renew SDL's 250 ms effect every 100 ms while frames are progressing.
    // A stalled render/UI thread therefore cannot leave an infinite effect.
    // This watchdog is a host policy, not a duration supplied by the game.
    bool suppress = (now > last_frame && now - last_frame > 250) || !pad || host_test || !g_settings.rumble || menu_is_open() ||
                    !SDL_GetKeyboardFocus();
    SDL_LockSpinlock(&rumble_lock);
    if (suppress) memset(rumble_values, 0, sizeof rumble_values);
    else if (rumble_allowed) {
        rumble_mix(rumble_values, &lo, &hi);
        int pct = g_settings.rumble_strength;  // (Controls tab: Intensity)
        if (pct > 0 && pct < 100) lo = (uint16_t)(lo * pct / 100), hi = (uint16_t)(hi * pct / 100);
    }
    SDL_UnlockSpinlock(&rumble_lock);
    if (pad && (lo != rumble_last_lo || hi != rumble_last_hi ||
                ((lo || hi) && now - rumble_last_ms >= 100))) {
        if (!SDL_RumbleGamepad(pad, lo, hi, (lo || hi) ? 250 : 0) && !rumble_warned) {
            LOG("[rumble] unavailable: %s\n", SDL_GetError());
            rumble_warned = true;  // keyboard/non-rumble pads are still usable
        }
        rumble_last_ms = now;
    }
    rumble_last_lo = lo; rumble_last_hi = hi;
}
void input_rumble_stop(void) {  // UI thread: also called before normal app exit
    rumble_clear();
    atomic_store(&rumble_frame_ms, 0);
    if (pad) SDL_RumbleGamepad(pad, 0, 0, 0);
    rumble_last_lo = rumble_last_hi = 0;
}
RT_ON_RESET(input_rumble_reset) {
    input_rumble_stop();
    input_rumble_permit(true);
}

// Controller light (runtime/led.h): the light bar of the game's controller shows Link's health. UI thread only.
// While the option is off (or nothing is in play) SDL_SetGamepadLED is never called: the one call that hands the
// light back is made when we stop owning it.
int game_in_play(void);                // hle/warp.c
int cheats_link_health(int *max);      // ui/cheats.mm
static SDL_JoystickID led_pad_id;      // the pad the state below is about
static bool led_capable, led_lit;      // it has an RGB light / we set its colour
static LedColor led_last;
static uint64_t led_last_ms, led_log_ms, led_hurt_ms;
static int led_prev_hp = -1, led_prev_max;

static void led_forget(void) { led_lit = false, led_prev_hp = -1, led_pad_id = 0; }
static void led_restore(const char *why) {
    if (led_lit && pad) {
        LedColor c = led_default_color(SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS4, SDL_GetGamepadPlayerIndex(pad));
        SDL_SetGamepadLED(pad, c.r, c.g, c.b);
        LOG("[led] %s: controller colour restored\n", why);
    }
    led_lit = false, led_prev_hp = -1;
}
void input_led_stop(void) { led_restore("quit"); }  // before exit / teardown
static void led_atexit(void) { led_restore("exit"); }
RT_ON_RESET(input_led_reset) { led_restore("reset"); }

void input_led_poll(void) {
    if (!pad || host_test) { led_forget(); return; }
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id != led_pad_id) {
        led_forget();
        led_pad_id = id;
        led_capable = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
        if (led_capable) LOG("[led] %s has a light\n", SDL_GetGamepadName(pad));
    }
    if (!led_capable) return;
    if (!g_settings.controller_light) { led_restore("off"); return; }
    int max = 0, hp = game_in_play() ? cheats_link_health(&max) : -1;
    if (hp < 0 || max <= 0) { led_restore("not in play"); return; }
    uint64_t now = SDL_GetTicks();
    if (led_prev_hp >= 0 && hp < led_prev_hp && max == led_prev_max) led_hurt_ms = now;
    if (led_prev_hp < 0) led_hurt_ms = 0;  // (entering play: no flash)
    led_prev_hp = hp, led_prev_max = max;
    LedColor c = led_health_color(hp, max, now, led_hurt_ms ? now - led_hurt_ms : UINT64_MAX);
    if (led_lit && (led_equal(c, led_last) || now - led_last_ms < LED_MIN_INTERVAL_MS)) return;
    if (!SDL_SetGamepadLED(pad, c.r, c.g, c.b)) {
        LOG("[led] unavailable: %s\n", SDL_GetError());
        led_capable = false;
        return;
    }
    static bool registered;
    if (!registered) registered = true, atexit(led_atexit);
    if (!led_lit || now - led_log_ms >= 1000) {  // (the pulse changes it all the time: one line a second at most)
        LOG("[led] %d,%d,%d (health %d/%d quarter-hearts%s)\n", c.r, c.g, c.b, hp, max,
            led_hurt_ms && now - led_hurt_ms < LED_FLASH_MS ? ", hurt" : "");
        led_log_ms = now;
    }
    led_lit = true, led_last = c, led_last_ms = now;
}

// Host keys / buttons held by a test script (LARECOMP_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in LARECOMP_HOST_INPUT)
static _Atomic int32_t test_rx, test_ry;  // ("rstick": a stick pushed sideways that also reads a little up)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

static int held_key(int sc) { return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]); }

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    key_buttons = b;
    key_lx = (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * 32767;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static bool pad_by_pos;  // (battle_abxy_position: ZL held at the last recompute_pad)
static int pad_host(int i, bool by_pos) { return by_pos && i <= BIND_Y ? bind_pad_default(i) : g_bind_pad[i]; }
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    // battle_abxy_position: Pokémon Z-A's moves are used only with ZL held and sit where the Switch's A/B/X/Y are,
    // so with A/B/X/Y bound by letter (Xbox A = the Switch's A) they read swapped. While ZL is held those four go by
    // position, as in the defaults; the rest of the game keeps the player's bindings (#26).
    int zl = g_bind_pad[BIND_ZL];
    bool by_pos = pad_by_pos = g_settings.battle_abxy_position && zl >= 0 && zl < PADB_HOST_COUNT && (host >> zl & 1);
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++) {
        int h = pad_host(i, by_pos);
        if (h >= 0 && h < PADB_HOST_COUNT && (host >> h & 1)) b |= 1ull << i;
    }
    pad_buttons = b;
    int32_t rx, ry;
    if (!pad || host_test) pad_lx = test_lx, pad_ly = test_ly, rx = test_rx, ry = test_ry;
    else {
        pad_lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        pad_ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    }
    // rstick_snap: a stick pushed sideways also reads a little up or down (an Xbox One pad: 4 degrees on average, up
    // to 12), and the camera tilts with it. That close to an axis the stick is put on the axis; the angles in between
    // are spread over the rest, so nothing jumps at the edge. The length is kept.
    int snap = g_settings.rstick_snap;
    if (snap > 0 && snap < 45 && (rx || ry)) {
        double m = sqrt((double)rx * rx + (double)ry * ry), a = atan2(fabs((double)ry), fabs((double)rx)) * 180 / M_PI;
        a = a <= snap ? 0 : a >= 90 - snap ? 90 : (a - snap) * 90 / (90 - 2 * snap);
        rx = (int32_t)copysign(m * cos(a * M_PI / 180), rx);
        ry = (int32_t)copysign(m * sin(a * M_PI / 180), ry);
    }
    pad_rx = rx, pad_ry = ry;
}

// Every press reaches the game, however short. recompute_pad reads the pad as it is now, so a quick tap whose press
// and release SDL hands over together (a Bluetooth pad's reports often come batched), or that fell between two of the
// game's polls, never showed: the game's menus took a second press now and then. A press the game hasn't polled yet
// that is released is still given to the game's next polls (fill_state). UI thread: note_press; game thread: the rest.
static _Atomic uint64_t press_unseen;  // pressed (events), not yet in a state the game polled
static _Atomic uint64_t tap_buttons;   // released while unseen: the game's next poll gets them
// The same mapping as recompute_pad's, ZL by position included: by letter here, a move pressed with ZL held left its
// by-letter button unseen, and the release gave the game a tap of it - the move on the other side, used when the
// pressed one was cooling down (#42)
static uint64_t pad_bits(int host) {   // the game buttons bound to host button `host` (SDL button / PADB_*TRIGGER)
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (pad_host(i, pad_by_pos) == host) b |= 1ull << i;
    return b;
}
static void note_press(uint64_t down, uint64_t up) {
    if (down) press_unseen |= down;
    if (up) {
        uint64_t u = atomic_fetch_and(&press_unseen, ~up) & up;
        if (u) tap_buttons |= u;
    }
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    press_unseen = 0, tap_buttons = 0;
    rumble_clear();
    memset(keys_down, 0, sizeof keys_down);
    recompute_keyboard();
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log
volatile int g_shot_request;         // F10: the game image -> screenshots/shot_<frame>.png

// Events a test pushes (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: / mouse:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_MOTION: return e->motion.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: return e->button.which == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// LARECOMP_NO_USER_INPUT=1, on by default whenever LARECOMP_INPUT / LARECOMP_DOORJUMP_TEST / LARECOMP_PLACE is set
// (or LARECOMP_HOST_INPUT / LARECOMP_BIND_TEST; LARECOMP_NO_USER_INPUT=0 lets it through again).
static bool input_user_blocked_any(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                 getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    return on;
}
bool input_user_blocked(const SDL_Event *e) {
    if (!input_user_blocked_any()) return false;
    if (test_event(e)) return false;  // (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// The mouse cursor during play: hidden as soon as a controller (a button, a stick or trigger past a small deadzone) or
// a key is used, shown again only when the mouse moves (a few points, so a bumped desk doesn't count), clicks or
// scrolls. Every window mode and platform: SDL hides it over our window only. Main thread (the event loops: the
// game's ui_run and the start screen).
static bool cursor_hidden;  // (the start screen keeps ImGui's backend from showing it again meanwhile)
bool input_cursor_hidden(void) { return cursor_hidden; }
void input_cursor_event(const SDL_Event *e) {
    static float moved;  // (points the mouse moved since the cursor was hidden)
    bool hide = false, show = false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:  // (a modifier alone - Cmd / Alt for switching apps - isn't play)
        hide = !e->key.repeat && !(e->key.scancode >= SDL_SCANCODE_LCTRL && e->key.scancode <= SDL_SCANCODE_RGUI);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: hide = true; break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: hide = abs(e->gaxis.value) > 12000; break;  // (stick drift stays below)
    case SDL_EVENT_MOUSE_MOTION:
        moved += fabsf(e->motion.xrel) + fabsf(e->motion.yrel);
        show = moved > 6;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_WHEEL: show = true; break;
    default: break;
    }
    if (hide && !cursor_hidden) {
        SDL_HideCursor();
        cursor_hidden = true, moved = 0;
        LOG("[cursor] hidden (%s): SDL cursor visible=%d\n", e->type == SDL_EVENT_KEY_DOWN ? "key" : "controller",
            SDL_CursorVisible());
    } else if (show && cursor_hidden) {
        SDL_ShowCursor();
        cursor_hidden = false;
        LOG("[cursor] shown (mouse %s): SDL cursor visible=%d\n",
            e->type == SDL_EVENT_MOUSE_MOTION ? "moved" : e->type == SDL_EVENT_MOUSE_WHEEL ? "wheel" : "button",
            SDL_CursorVisible());
    }
}

// "Allow background inputs" (Controls tab, off by default): controllers keep working while the window isn't in front.
// SDL drops controller events without keyboard focus unless this hint is set; it reads the hint live. (On macOS the
// hint makes the system ask for Input Monitoring - reading HID input for a background app - so it was taken out as a
// default there; the player opts in here.)
SDL_JoystickID input_active_pad(void) { return pad ? SDL_GetGamepadID(pad) : 0; }
void input_apply_background(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_settings.bg_input ? "1" : "0");
}

// A controller SDL has no mapping for (on macOS e.g. a generic Bluetooth HID pad, such as a phone app posing as one)
// is only a joystick to SDL, and the game reads gamepads only. gamecontrollerdb.txt in the data root holds the
// player's own mappings, in SDL's format (lines for other platforms are skipped, so SDL_GameControllerDB's file works
// as is). Called right after SDL_Init: a pad already connected becomes a gamepad then (SDL sends GAMEPAD_ADDED).
void input_load_user_mappings(void) {
    if (!SDL_GetPathInfo("gamecontrollerdb.txt", NULL)) return;
    int n = SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");
    if (n < 0) LOG("[input] gamecontrollerdb.txt: %s\n", SDL_GetError());
    else LOG("[input] gamecontrollerdb.txt: %d controller mapping(s) added\n", n);
}

// Controller family for the button prompts (runtime/nvn/texpatch.c) and our menus' layout: 0 Nintendo / none, 1 Xbox
// (and every other pad SDL labels like one: generic / third-party pads with A on the bottom), 2 PlayStation.
// LARECOMP_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
// By SDL's own face-button labels for the type: the bottom button reads B on Nintendo pads, A on Xbox-style ones,
// Cross on PlayStation ones.
int input_family_of_type(int type) {
    switch (SDL_GetGamepadButtonLabelForType((SDL_GamepadType)type, SDL_GAMEPAD_BUTTON_SOUTH)) {
    case SDL_GAMEPAD_BUTTON_LABEL_B: return 0;
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return 2;
    default: return 1;
    }
}
static _Atomic int pad_family;
static void note_family(void) {
    int f = pad ? input_family_of_type(SDL_GetGamepadType(pad)) : 0;
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo");
    pad_family = f;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

// mouse camera: the right stick, from how fast the mouse moves. Setting mouse_camera: captured = the window holds the
// cursor (relative mode, input_mouse_update) and every motion turns the camera; hold = only with the right button
// held, the cursor free. input_handle_event adds up the motion (1/16 px); mouse_stick, at each read of the game's
// controller, turns it into a speed (px/s, smoothed over ~30 ms) and that into a deflection: past the stick's dead
// zone at once (else slow moves do nothing), full at MOUSE_FULL px/s divided by mouse_sens. (It used to be each
// motion event's own pixels x 2500 held for 80 ms: 3-4 px already full, so the speed setting changed nothing and the
// camera turned in jerks.) The game's own top turning speed is still the limit: its camera speed option raises it.
static _Atomic int32_t mouse_acc_x, mouse_acc_y;
static void mouse_stick(int32_t *rx, int32_t *ry) {
    static uint64_t last;
    static double vx, vy;
    static double full = -1, floor_ = -1;
    if (full < 0) {
        full = getenv("LARECOMP_MOUSE_FULL") ? atof(getenv("LARECOMP_MOUSE_FULL")) : 1200;   // px/s for full tilt at speed 1
        floor_ = getenv("LARECOMP_MOUSE_FLOOR") ? atof(getenv("LARECOMP_MOUSE_FLOOR")) : 0.2;  // (the stick's dead zone)
    }
    uint64_t now = SDL_GetTicksNS();
    double dt = last ? (double)(now - last) / 1e9 : 0;
    if (!last || dt > 0.25) { last = now; atomic_exchange(&mouse_acc_x, 0); atomic_exchange(&mouse_acc_y, 0); vx = vy = 0; }
    else if (dt >= 0.004) {  // (the game reads several times a frame: speeds over at least 4 ms)
        double tx = atomic_exchange(&mouse_acc_x, 0) / 16.0 / dt, ty = atomic_exchange(&mouse_acc_y, 0) / 16.0 / dt;
        double a = 1 - exp(-dt / 0.03);
        vx += (tx - vx) * a, vy += (ty - vy) * a;
        last = now;
    }
    double v = sqrt(vx * vx + vy * vy);
    if (v < 15) { *rx = *ry = 0; return; }  // (at rest: below ~15 px/s)
    double m = v * g_settings.mouse_sens / full;
    double d = (floor_ + (1 - floor_) * (m > 1 ? 1 : m)) * 32767 / v;
    *rx = (int32_t)(vx * d), *ry = (int32_t)(-vy * d);
}
// mouse buttons through g_bind_mouse (Switch button bits), counted only while the cursor is held: a click that brings
// the window to the front, or one meant for the menu, is not a ball thrown
static _Atomic bool mouse_held;
static _Atomic uint64_t mouse_buttons;
static uint8_t mouse_down[MOUSEB_COUNT];
static void recompute_mouse(void) {
    uint64_t b = 0;
    for (int m = 0; m < MOUSEB_COUNT; m++)
        if (mouse_down[m] && g_bind_mouse[m] >= 0 && g_bind_mouse[m] < BIND_PAD_COUNT) b |= 1ull << g_bind_mouse[m];
    mouse_buttons = b;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm
// Main thread, after each batch of events (runtime/nvn/vulkan/device.c ui_run): the cursor held while the game has
// the window and the controls - not with the menu or the name entry up (they take the mouse), nor without focus.
void input_mouse_update(SDL_Window *w) {
    bool want = g_settings.mouse_camera == MOUSE_CAM_CAPTURED && !menu_input_captured() && !input_user_blocked_any() &&
                (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS);
    if (want == mouse_held) return;
    SDL_SetWindowRelativeMouseMode(w, want);
    mouse_held = want;
    memset(mouse_down, 0, sizeof mouse_down);  // (a button held across the change: released)
    recompute_mouse();
    mouse_acc_x = mouse_acc_y = 0;
    LOG("[input] mouse %s\n", want ? "held by the window: it turns the camera" : "free");
}
// Our menus' face buttons: the family whose button names they show, and (Confirm button = Controller convention, #172)
// whose layout they use - Nintendo (0): the right button confirms, the bottom one goes back; Xbox / PlayStation
// (1 / 2): the bottom one confirms, the right one goes back (runtime/ui/pad_nav.mm). "Button icons"
// forced to a style wins (icons and menus agree); Auto: the controller `which` that pressed (0, or not a connected
// gamepad - a test's pushed press: the game's controller / the first connected one, else Nintendo).
int input_menu_family(SDL_JoystickID which) {
    switch (g_settings.prompts) {
    case PROMPTS_NINTENDO: return 0;
    case PROMPTS_XBOX: return 1;
    case PROMPTS_PLAYSTATION: return 2;
    default: break;
    }
    if (getenv("LARECOMP_PAD_TYPE")) return input_pad_family();
    if (which && SDL_IsGamepad(which)) return input_family_of_type(SDL_GetGamepadTypeForID(which));
    return bind_label_family();
}

// Back on the start screen (runtime/ui/launcher.mm, also after the menu's Quit to Start Screen): the cursor is free
// again - the game held it for the camera, and nothing calls input_mouse_update while the start screen is up
void input_mouse_release(SDL_Window *w) {
    if (!mouse_held) return;
    SDL_SetWindowRelativeMouseMode(w, false);
    mouse_held = false;
    memset(mouse_down, 0, sizeof mouse_down);
    recompute_mouse();
    mouse_acc_x = mouse_acc_y = 0;
    LOG("[input] mouse free (start screen)\n");
}
void input_handle_event(const SDL_Event *e) {
    static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop
    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (n > 0 && !pad) { pad = SDL_OpenGamepad(ids[0]); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad)); note_family(); }
        SDL_free(ids);
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F10 && !e->key.repeat) g_shot_request = 1;
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        uint64_t before = key_buttons;
        recompute_keyboard();
        uint64_t now = key_buttons;
        note_press(now & ~before, before & ~now);
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pad) { pad = SDL_OpenGamepad(e->gdevice.which); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad)); note_family(); }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) {
            SDL_RumbleGamepad(pad, 0, 0, 0);
            SDL_CloseGamepad(pad); pad = NULL; note_family();
            led_forget();  // (gone: it shows its own colour when it comes back)
            rumble_clear(); rumble_last_lo = rumble_last_hi = 0; rumble_warned = false;
        }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (!host_test || e->gbutton.which == TEST_DEVICE) {  // (the press as the event has it, before reading the pad)
            uint64_t b = pad_bits(e->gbutton.button);
            note_press(e->gbutton.down ? b : 0, e->gbutton.down ? 0 : b);
        }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if ((e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
            !host_test) {  // (the triggers as buttons: past half way)
            static bool trig[2];
            int t = e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            bool on = e->gaxis.value > 16000;
            if (on != trig[t]) {
                uint64_t b = pad_bits(PADB_LTRIGGER + t);
                note_press(on ? b : 0, on ? 0 : b);
                trig[t] = on;
            }
        }
        recompute_pad();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        int m = bind_mouse_from_sdl(e->button.button);
        if (m < 0) break;
        if (e->type == SDL_EVENT_MOUSE_BUTTON_UP) mouse_down[m] = 0;
        else if (mouse_held) mouse_down[m] = 1;
        recompute_mouse();
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:  // the mouse turns the camera (right stick): cursor held, or the right button held
        if (mouse_held || (g_settings.mouse_camera == MOUSE_CAM_HOLD && (e->motion.state & SDL_BUTTON_RMASK))) {
            atomic_fetch_add(&mouse_acc_x, (int32_t)lroundf(e->motion.xrel * 16));  // (mouse_stick turns it into speed)
            atomic_fetch_add(&mouse_acc_y, (int32_t)lroundf(e->motion.yrel * 16));
        }
        break;
    }
}

// Scripted input for testing: LARECOMP_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
// "frame:ls:<x>/<y>:frames" / "frame:rs:<x>/<y>:frames" hold a stick there (-32767..32767, y up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;
static const char *const input_names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                          "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};

// LARECOMP_RECORD_INPUT=<file>: what the player holds, frame by frame, written as LARECOMP_INPUT replays it - a battle
// played once from a save, then played again by a test (tools/za/perf-win.sh IN=@<file>) as often as needed.
// Sticks in steps of 256 with a dead zone (a resting controller's drift would make an entry every frame).
static FILE *rec_f;
static uint64_t rec_start, rec_held;
static int32_t rec_v[4];
static void rec_write(uint64_t frame) {  // what was held from rec_start to this frame
    if (frame <= rec_start) return;
    unsigned long long s = rec_start, n = frame - rec_start;
    for (int i = 0; i < 16; i++) if (rec_held >> i & 1) fprintf(rec_f, "%llu:%s:%llu,", s, input_names[i], n);
    if (rec_v[0] || rec_v[1]) fprintf(rec_f, "%llu:ls:%d/%d:%llu,", s, rec_v[0], rec_v[1], n);
    if (rec_v[2] || rec_v[3]) fprintf(rec_f, "%llu:rs:%d/%d:%llu,", s, rec_v[2], rec_v[3], n);
    fflush(rec_f);
}
static void rec_at_exit(void) {  // (what is still held as the game closes)
    extern uint64_t renderer_present_count(void);
    rec_write(renderer_present_count());
}
static void record_input(uint64_t frame, uint64_t b, const int32_t st_[4]) {
    static int on = -1;
    if (on < 0 && (on = getenv("LARECOMP_RECORD_INPUT") && (rec_f = fopen(getenv("LARECOMP_RECORD_INPUT"), "w"))))
        atexit(rec_at_exit);
    if (!on) return;
    int32_t q[4];
    for (int a = 0; a < 4; a++) q[a] = abs(st_[a]) < 4000 ? 0 : abs(st_[a]) > 32767 - 256 ? st_[a] : st_[a] / 256 * 256;
    b &= 0xFFFF;
    if (b == rec_held && !memcmp(q, rec_v, sizeof q)) return;
    rec_write(frame);
    rec_start = frame, rec_held = b;
    memcpy(rec_v, q, sizeof rec_v);
}

// Tests of the bindings, through them like a real player:
// LARECOMP_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in larecomp.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick),
//   "frame:rstick:<x>/<y>:frames" the right one.
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:mouse:<dx>/<dy>" moves the mouse by that many points, "frame:mouse:click" clicks it (real events).
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// "frame:resize:<W>x<H>" resizes the window to W x H points (menu_apply_window_requests: the main thread).
// LARECOMP_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
uint32_t g_window_resize_request;  // (W << 16 | H: menu_apply_window_requests applies it)
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "resize")) {
        unsigned w = 0, h = 0;
        if (frame == start && sscanf(arg, "%ux%u", &w, &h) == 2 && w && h && w < 65536 && h < 65536) {
            LOG("[input] test: window to %ux%u\n", w, h);
            __atomic_store_n(&g_window_resize_request, w << 16 | h, __ATOMIC_RELAXED);
        }
        return;
    }
    if (!strcmp(dev, "shot")) {
        extern char g_window_shot_request[256];  // runtime/hle/hookshot.c
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "mouse")) {
        if (frame != start) return;
        float mx, my, dx = 0, dy = 0;
        SDL_GetMouseState(&mx, &my);
        if (!strcmp(arg, "click")) {
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.which = TEST_DEVICE, e.button.button = SDL_BUTTON_LEFT, e.button.down = true, e.button.clicks = 1;
            e.button.x = mx, e.button.y = my;
            SDL_PushEvent(&e);
            e.type = SDL_EVENT_MOUSE_BUTTON_UP, e.button.down = false;
        } else {
            sscanf(arg, "%f/%f", &dx, &dy);
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.which = TEST_DEVICE, e.motion.x = mx + dx, e.motion.y = my + dy, e.motion.xrel = dx, e.motion.yrel = dy;
        }
        SDL_PushEvent(&e);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("LARECOMP_HOST_INPUT"); bspec = getenv("LARECOMP_BIND_TEST"); host_test = hspec; }
    static char *buf;  // (a copy to cut up; as long as the longest spec: a long scripted run is thousands of steps)
    static size_t cap;
    size_t need = 2048;
    if (hspec && strlen(hspec) + 1 > need) need = strlen(hspec) + 1;
    if (bspec && strlen(bspec) + 1 > need) need = strlen(bspec) + 1;
    if (need > cap) { free(buf); buf = malloc(need); cap = need; }
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0, srx = 0, sry = 0;
        snprintf(buf, cap, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot") ||
                !strcmp(dev, "resize") || !strcmp(dev, "mouse")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else if (!strcmp(dev, "rstick")) sscanf(name, "%d/%d", &srx, &sry);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0,
             pch = padm != test_pad || sx != test_lx || sy != test_ly || srx != test_rx || sry != test_ry;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy, test_rx = srx, test_ry = sry;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, cap, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A/B skip the opening on rando seeds)
RT_ON_RESET(input_reboot_reset) { g_pad_obj = 0; g_inject_plus = 0; }  // (reboot: the controller object was guest memory)
void input_frame(uint64_t frame) {
    atomic_store(&rumble_frame_ms, SDL_GetTicks());
    if (g_pad_obj && getenv("LARECOMP_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    const char *spec = getenv("LARECOMP_INPUT");
    if (!spec) return;
    // (parsed once: a recording (LARECOMP_RECORD_INPUT) is thousands of entries; a later entry's axis wins)
    typedef struct { uint64_t start, len, b; int32_t v[4]; uint8_t set; } Ev;  // (v: lx ly rx ry, set: which of them)
    static Ev *ev;
    static int nev = -1;
    if (nev < 0) {
        char *buf = NULL;
        if (spec[0] == '@') {  // @<file>: the entries in a file (a recording: past the 32 KB a variable can hold)
            FILE *sf = fopen(spec + 1, "rb");
            long n = 0;
            if (sf && !fseek(sf, 0, SEEK_END) && (n = ftell(sf)) >= 0 && !fseek(sf, 0, SEEK_SET) && (buf = malloc(n + 1)))
                buf[fread(buf, 1, n, sf)] = 0;
            if (sf) fclose(sf);
            LOG("[input] script %s: %s\n", spec + 1, buf ? "read" : "not found");
        }
        if (!buf) buf = strdup(spec[0] == '@' ? "" : spec);
        int cap = 0;
        nev = 0;
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            unsigned long start = 0, len = 10;
            char name[32] = "";
            int x = 0, y = 0;
            Ev e = {0};
            if (sscanf(tok, "%lu:ls:%d/%d:%lu", &start, &x, &y, &len) >= 3) e.set = 3, e.v[0] = x, e.v[1] = y;
            else if (sscanf(tok, "%lu:rs:%d/%d:%lu", &start, &x, &y, &len) >= 3) e.set = 12, e.v[2] = x, e.v[3] = y;
            else if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
            else if (!strcmp(name, "STICK_UP")) e.set = 2, e.v[1] = 32767;
            else if (!strcmp(name, "STICK_DOWN")) e.set = 2, e.v[1] = -32767;
            else if (!strcmp(name, "STICK_LEFT")) e.set = 1, e.v[0] = -32767;
            else if (!strcmp(name, "STICK_RIGHT")) e.set = 1, e.v[0] = 32767;
            else if (!strcmp(name, "RSTICK_UP")) e.set = 8, e.v[3] = 32767;
            else if (!strcmp(name, "RSTICK_DOWN")) e.set = 8, e.v[3] = -32767;
            else if (!strcmp(name, "RSTICK_LEFT")) e.set = 4, e.v[2] = -32767;
            else if (!strcmp(name, "RSTICK_RIGHT")) e.set = 4, e.v[2] = 32767;
            else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
                double r = atoi(name + 6) * 3.14159265358979 / 180;
                e.set = 3, e.v[0] = (int32_t)lround(cos(r) * 32767), e.v[1] = (int32_t)lround(sin(r) * 32767);
            } else
                for (int i = 0; i < 16; i++) if (!strcmp(name, input_names[i])) e.b |= 1ull << i;
            e.start = start, e.len = len;
            if (nev == cap) ev = realloc(ev, (cap = cap ? cap * 2 : 64) * sizeof *ev);
            ev[nev++] = e;
        }
        free(buf);
    }
    uint64_t b = 0;
    int32_t v[4] = {0};
    for (int i = 0; i < nev; i++) {
        const Ev *e = &ev[i];
        if (frame < e->start || frame >= e->start + e->len) continue;
        b |= e->b;
        for (int a = 0; a < 4; a++) if (e->set >> a & 1) v[a] = e->v[a];
    }
    int32_t lx = v[0], ly = v[1], rx = v[2], ry = v[3];
    {  // test hooks steering the stick (runtime/hle/doorjump.c LARECOMP_DOORJUMP_TEST)
        extern int32_t g_test_lx, g_test_ly;
        extern uint64_t g_test_stick_until;
        if (frame < g_test_stick_until) { lx = g_test_lx; ly = g_test_ly; }
        extern uint64_t g_test_buttons, g_test_buttons_until;  // (LARECOMP_PLACE action script)
        if (frame < g_test_buttons_until) b |= g_test_buttons;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

// Buttons held right now (keyboard, gamepad, test script), independent of whether the game polls (movies).
uint64_t input_buttons_now(void) { return (menu_input_captured() ? 0 : key_buttons | pad_buttons) | script_buttons; }

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    *(int64_t *)s = ++sampling;
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id != 0) return;  // docked: only player 1 (FullKey); Handheld stays disconnected
    if (g_game_is_la) {
    extern void warp_game_tick(Cpu *c);
    warp_game_tick(c);  // game main thread, once per frame: apply a queued warp
    { extern void give_item_tick(Cpu *c, uint64_t frame); extern uint64_t renderer_present_count(void);
      give_item_tick(c, renderer_present_count());
      extern void chamber_game_tick(Cpu *c, uint64_t frame);
      chamber_game_tick(c, renderer_present_count());
      extern void digs_game_tick(Cpu *c, uint64_t frame);
      digs_game_tick(c, renderer_present_count());
      extern void pot_bush_game_tick(Cpu *c, uint64_t frame);
      pot_bush_game_tick(c, renderer_present_count());
      extern void shell_spots_tick(Cpu *c, uint64_t frame);
      shell_spots_tick(c, renderer_present_count());
      extern void qol_tick(void);
      qol_tick();
      extern void run_timer_tick(void);
      run_timer_tick();  // (run timer / LiveSplit: runtime/hle/run_timer.c)
      extern void pets_game_tick(Cpu *c);
      pets_game_tick(c); }
    }
    extern uint64_t g_watch_lo, g_watch_hi;
    if (id == 0 && !g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (id == 0 && !g_watch_lo && getenv("LARECOMP_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("LARECOMP_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    uint64_t held = key_buttons | pad_buttons | mouse_buttons;
    atomic_fetch_and(&press_unseen, ~held);  // (the game sees these now)
    static uint64_t tap, tap_frame;  // (pressed and released since the last poll: held for the rest of this frame)
    {
        extern uint64_t renderer_present_count(void);
        uint64_t t = atomic_exchange(&tap_buttons, 0), f = renderer_present_count();
        if (t) {
            tap |= t, tap_frame = f;
            if (getenv("LARECOMP_INPUT")) LOG("[input] short press %#llx\n", (unsigned long long)t);
        } else if (f > tap_frame) tap = 0;
    }
    uint64_t b = menu ? script_buttons : held | tap | script_buttons;
    {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_present_count(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_present_count() + 10; g_inject_plus = 0; }
        if (renderer_present_count() < until) b |= 1ull << BTN_PLUS;
    }
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx) + script_lx), ly = clamp_axis((menu ? 0 : key_ly + pad_ly) + script_ly);
    int32_t mrx, mry;
    mouse_stick(&mrx, &mry);
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx + mrx) + script_rx), ry = clamp_axis((menu ? 0 : key_ry + pad_ry + mry) + script_ry);
    {
        extern uint64_t renderer_present_count(void);
        record_input(renderer_present_count(), b, (int32_t[4]){lx, ly, rx, ry});  // (before mirror mode swaps them)
    }
    extern bool mirror_input(void);  // mirror mode, the world on screen (runtime/hle/mirror.c): left and right swap
    bool mirrored = !menu && mirror_input();
    if (mirrored) {
        lx = -lx;
        uint64_t l = (b >> BTN_LEFT) & 1, r = (b >> BTN_RIGHT) & 1;
        b = (b & ~((1ull << BTN_LEFT) | (1ull << BTN_RIGHT))) | (l << BTN_RIGHT) | (r << BTN_LEFT);
    }
    // d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick
    // never reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
    if (!menu && g_settings.dpad_move && (int64_t)lx * lx + (int64_t)ly * ly < 8000ll * 8000) {
        uint64_t d = key_buttons | pad_buttons | script_buttons;
        int dx = (int)((d >> BTN_RIGHT) & 1) - (int)((d >> BTN_LEFT) & 1);
        if (mirrored) dx = -dx;
        int dy = (int)((d >> BTN_UP) & 1) - (int)((d >> BTN_DOWN) & 1);
        int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
        if (dx || dy) lx = dx * m, ly = dy * m;
    }
    {   // door jumps stop Link for a moment before moving him (runtime/hle/doorjump.c): stick + d-pad released
        extern int32_t g_stick_lx, g_stick_ly;  // (what the player holds, before that: doorjump.c reads it)
        g_stick_lx = lx, g_stick_ly = ly;
        extern uint64_t g_stick_freeze_until;
        extern uint64_t renderer_present_count(void);
        if (renderer_present_count() < g_stick_freeze_until) {
            lx = ly = 0;
            b &= ~(0xfull << 12);
        }
    }
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    {   // the pause menu's companion slot (runtime/hle/pets.c) takes the d-pad / sticks / A while the cursor is on it
        extern uint64_t pets_menu_input(uint64_t b, int32_t st[4]);
        int32_t st[4] = {lx, ly, rx, ry};
        b = pets_menu_input(b, st);
        // the check tracker drives the Map page in randomizer seeds (runtime/ui/tracker_overlay.mm): its cursor takes
        // the d-pad / left stick / A / Y (and B while one of its panels is open)
        extern uint64_t tracker_map_input(uint64_t b, int32_t st[4]);
        b = tracker_map_input(b, st);
        lx = st[0], ly = st[1], rx = st[2], ry = st[3];
    }
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && getenv("LARECOMP_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    static int32_t last_rx, last_ry;
    if ((rx != last_rx || ry != last_ry) && getenv("LARECOMP_INPUT")) LOG("[input] id %#x right stick %d %d\n", id, rx, ry);
    last_rx = rx, last_ry = ry;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    { extern void input_note_rstick(int32_t ry); input_note_rstick(ry); }
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// FullKey for player 1 only (we report docked mode, where Handheld is unavailable)
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = id == 0 ? 1 : 0;  // docked: no handheld controller
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// The SDK vibration node graph stays recompiled; HID IPC 201/206 sends its
// output to rumble.h / SDL using the native-layout handles below.
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    // Native wire layout is style, player, device index, padding (one byte each).
    // The old synthetic handles placed the style byte in padding.
    for (int i = 0; i < n; i++) out[i] = 3u | ((id & 0xFF) << 8) | ((uint32_t)i << 16);
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    int side = rumble_side(h);
    info[1] = side < 0 ? 0 : (uint32_t)side + 1;  // None=0, Left=1, Right=2
}
