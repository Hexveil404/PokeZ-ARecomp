// The game's controller follows the gamepad in use (#455 / #456: a two-port adapter is two gamepads, the working
// controller in port 2). Two SDL virtual gamepads, real SDL events, the real input.c. Never touches a physical pad.
#include <SDL3/SDL.h>
static void input_reset(void);
static void input_load_user_mappings(void);
static bool input_user_blocked(const SDL_Event *e);
static int input_pad_family(void);
static void input_handle_event(const SDL_Event *e);
static void input_frame(uint64_t frame);
static uint64_t input_buttons_now(void);
#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#undef SERVICE
#define SERVICE(name, fn) static void fn(Service *s, IpcReq *req, IpcResp *resp)
#include "../runtime/hle/input.c"
#include "../runtime/services/hid_set.c"
#undef NDEBUG
#include <assert.h>

bool menu_is_open(void) { return false; }
Settings g_settings;
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { abort(); }
KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle) { abort(); }
Service *service_new(const char *n, ServiceHandler h, void *state) { abort(); }
void ipc_return_object(IpcResp *r, Service *s) { abort(); }
int game_in_play(void) { abort(); }
int cheats_link_health(int *max) { abort(); }
int bind_label_family(void) { abort(); }
int16_t g_bind_pad[BIND_PAD_COUNT], g_bind_key[BIND_KEY_COUNT];  // (A on the bottom button, as by default)
int16_t g_bind_mouse[MOUSEB_COUNT];  // (ZA Recomp's mouse buttons and menus: unused here)
int bind_mouse_from_sdl(int b) { return -1; }
int bind_pad_default(int slot) { return g_bind_pad[slot]; }
bool menu_input_captured(void) { return false; }
bool menu_cursor_needed(void) { return false; }

static SDL_JoystickID attach(const char *name) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD, d.naxes = 6, d.nbuttons = 15, d.axis_mask = 0x3f, d.button_mask = 0x7fff;
    d.name = name;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    assert(id && SDL_OpenJoystick(id));  // (open already, as the start screen leaves every controller)
    return id;
}
static void pump(void) {  // the event loop: every event to input.c, as ui_run does
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) input_handle_event(&e);
}
static void button(SDL_JoystickID id, int b, bool down) {
    SDL_Joystick *j = SDL_GetJoystickFromID(id);
    assert(j && SDL_SetJoystickVirtualButton(j, b, down));
    pump();
}
static void axis(SDL_JoystickID id, int a, int16_t v) {
    SDL_Joystick *j = SDL_GetJoystickFromID(id);
    assert(j && SDL_SetJoystickVirtualAxis(j, a, v));
    pump();
}
static uint64_t south(void) { return pad_bits(SDL_GAMEPAD_BUTTON_SOUTH); }

int main(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    const char *off[] = {SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_IOKIT, SDL_HINT_JOYSTICK_MFI, SDL_HINT_JOYSTICK_RAWINPUT,
                         SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT, SDL_HINT_JOYSTICK_DIRECTINPUT,
                         SDL_HINT_XINPUT_ENABLED};
    for (size_t i = 0; i < sizeof off / sizeof *off; i++) SDL_SetHint(off[i], "0");  // (physical controllers: left alone)
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    for (int i = 0; i < BIND_PAD_COUNT; i++) g_bind_pad[i] = -1;
    g_bind_pad[BIND_A] = SDL_GAMEPAD_BUTTON_SOUTH;
    SDL_JoystickID p1 = attach("port 1 (virtual)"), p2 = attach("port 2 (virtual)");
    SDL_Event none = {0};
    input_handle_event(&none);  // (the first event: connected pads opened, the first one is the game's)
    pump();
    assert(input_active_pad() == p1);

    // held on port 2: the game's pad switches and the hold stays held (#456), not just a tap
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(input_active_pad() == p2 && (pad_buttons & south()));
    pump(); pump();
    assert(pad_buttons & south());
    // port 2's stick moves Link (#455)
    axis(p2, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    assert(pad_lx == 32767);
    axis(p2, SDL_GAMEPAD_AXIS_LEFTX, 0);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    assert(!(pad_buttons & south()));

    // a noisy stick on port 1: drift doesn't take the pad, nor does a full push while port 2 is in use
    axis(p1, SDL_GAMEPAD_AXIS_LEFTX, 9000);
    assert(input_active_pad() == p2 && pad_lx == 0);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    axis(p1, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    assert(input_active_pad() == p2 && pad_lx == 0);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    axis(p1, SDL_GAMEPAD_AXIS_LEFTX, 0);
    // a clear stick push on port 1 while port 2 rests does
    axis(p1, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    assert(input_active_pad() == p1 && pad_ly > 32000);
    axis(p1, SDL_GAMEPAD_AXIS_LEFTY, 0);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(input_active_pad() == p2);
    button(p2, SDL_GAMEPAD_BUTTON_SOUTH, false);

    // the game's pad unplugged: the other one takes over
    assert(SDL_DetachVirtualJoystick(p2));
    pump();
    assert(input_active_pad() == p1);
    button(p1, SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(pad_buttons & south());
    button(p1, SDL_GAMEPAD_BUTTON_SOUTH, false);
    assert(SDL_DetachVirtualJoystick(p1));
    pump();
    assert(input_active_pad() == 0 && !pad_buttons);
    SDL_Quit();
    puts("PASS: press / stick on port 2 takes the game's pad and holds stay held; drift and in-use guard; unplug fallback");
    return 0;
}
