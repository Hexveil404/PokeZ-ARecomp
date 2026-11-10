// A first start with an Xbox / PlayStation pad gets A/B/X/Y by the pad's letters (#241): SDL virtual gamepads of
// each type, the real bindings.c. A settings file (a player who has played) or a Nintendo pad keeps the positions, and
// so do changed bindings. Never touches a physical pad.
#include <SDL3/SDL.h>
#include "../runtime/hle/bindings.c"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

Settings g_settings;
bool g_settings_fresh;
static int saves;
void settings_save(void) { saves++; }
int input_pad_family(void) { return 0; }  // (the start screen: input.c hasn't opened a pad, bind_label_family asks SDL)
int input_family_of_type(int type) {      // (runtime/hle/input.c's rule)
    switch (SDL_GetGamepadButtonLabelForType((SDL_GamepadType)type, SDL_GAMEPAD_BUTTON_SOUTH)) {
    case SDL_GAMEPAD_BUTTON_LABEL_B: return 0;
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return 2;
    default: return 1;
    }
}
int settings_on_steam_deck(void) { return 0; }
void rt_launcher_log(const char *fmt, ...) { (void)fmt; }

static SDL_JoystickID attach(SDL_GamepadType type) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD, d.naxes = 6, d.nbuttons = 15, d.axis_mask = 0x3f, d.button_mask = 0x7fff;
    d.vendor_id = type == SDL_GAMEPAD_TYPE_PS5 ? 0x054c : type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ? 0x057e : 0x045e;
    d.product_id = type == SDL_GAMEPAD_TYPE_PS5 ? 0x0ce6 : type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ? 0x2009 : 0x0b12;
    d.name = "virtual";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    assert(id);
    SDL_UpdateJoysticks();
    return id;
}

// one start: fresh or not, a pad of that type (or none) -> A on the bottom button (Xbox letters)?
static bool start(bool fresh, int type, bool custom) {
    memcpy(g_bind_pad, k_def_pad, sizeof g_bind_pad);
    if (custom) g_bind_pad[BIND_ZL] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;  // (a player's own change)
    g_settings_fresh = fresh;
    SDL_JoystickID id = type >= 0 ? attach((SDL_GamepadType)type) : 0;
    first_start_done = false;  // (once per process: each case is a new start)
    bind_pad_first_start();
    bool letters = g_bind_pad[BIND_A] == SDL_GAMEPAD_BUTTON_SOUTH;
    if (id) SDL_DetachVirtualJoystick(id), SDL_UpdateJoysticks();
    return letters;
}

int main(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    int fails = 0;
#define CASE(want, fresh, type, custom, what)                                                                         \
    do {                                                                                                              \
        bool got = start(fresh, type, custom);                                                                        \
        printf("%-58s %s%s\n", what, got ? "Xbox letters" : "Nintendo positions", got == want ? "" : "  <- FAIL");      \
        fails += got != want;                                                                                         \
    } while (0)
    CASE(true, true, SDL_GAMEPAD_TYPE_XBOXONE, false, "first start, Xbox pad:");
    CASE(true, true, SDL_GAMEPAD_TYPE_PS5, false, "first start, PlayStation pad:");
    CASE(false, true, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, false, "first start, Nintendo pad:");
    CASE(false, true, -1, false, "first start, no pad:");
    CASE(false, false, SDL_GAMEPAD_TYPE_XBOXONE, false, "settings file there (has played), Xbox pad:");
    CASE(false, true, SDL_GAMEPAD_TYPE_XBOXONE, true, "first start, bindings already changed, Xbox pad:");
    printf("saves: %d\n", saves);
    if (saves != 2) printf("  <- FAIL (2 expected: the Xbox and PlayStation cases)\n"), fails++;
    SDL_Quit();
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
