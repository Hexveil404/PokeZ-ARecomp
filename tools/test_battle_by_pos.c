// "Battle moves by position" (#224): A / B / X / Y go by position while ZL is held for a move, not while ZL and ZR
// are held to throw a ball - there the Switch's B (cancel) must stay on the button bound to it. The real input.c,
// host buttons set as a test script does (host_test), no controller touched.
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
// the defaults: by position (the Switch's A on the right, B at the bottom); the player's bindings here: by letter
int bind_pad_default(int slot) {
    return slot == BIND_A ? SDL_GAMEPAD_BUTTON_EAST : slot == BIND_B ? SDL_GAMEPAD_BUTTON_SOUTH
         : slot == BIND_X ? SDL_GAMEPAD_BUTTON_NORTH : slot == BIND_Y ? SDL_GAMEPAD_BUTTON_WEST : g_bind_pad[slot];
}
bool menu_input_captured(void) { return false; }
bool menu_cursor_needed(void) { return false; }
bool bugrec_active(void) { return false; }
void bugrec_mark(void) { abort(); }

static uint64_t game(uint32_t host) {  // the Switch buttons the game gets for these host buttons
    test_pad = host;
    recompute_pad();
    return pad_buttons;
}

int main(void) {
    for (int i = 0; i < BIND_PAD_COUNT; i++) g_bind_pad[i] = -1;
    g_bind_pad[BIND_A] = SDL_GAMEPAD_BUTTON_SOUTH, g_bind_pad[BIND_B] = SDL_GAMEPAD_BUTTON_EAST;  // (by letter: Xbox A = A)
    g_bind_pad[BIND_X] = SDL_GAMEPAD_BUTTON_WEST, g_bind_pad[BIND_Y] = SDL_GAMEPAD_BUTTON_NORTH;
    g_bind_pad[BIND_ZL] = PADB_LTRIGGER, g_bind_pad[BIND_ZR] = PADB_RTRIGGER;
    host_test = true;
    g_settings.battle_abxy_position = true;
    const uint32_t east = 1u << SDL_GAMEPAD_BUTTON_EAST, lt = 1u << PADB_LTRIGGER, rt = 1u << PADB_RTRIGGER;
    const uint64_t A = 1ull << BIND_A, B = 1ull << BIND_B, ZL = 1ull << BIND_ZL, ZR = 1ull << BIND_ZR;
    assert(game(east) == B);                 // (no trigger: the player's binding)
    assert(game(rt | east) == (ZR | B));     // (RT alone, the ball ready: B cancels)
    assert(game(lt | east) == (ZL | A));     // (LT alone, a battle's moves: by position, the right button is A)
    assert(game(lt | rt | east) == (ZL | ZR | B));  // (#224: aiming and the ball ready - B still cancels)
    g_settings.battle_abxy_position = false;
    assert(game(lt | east) == (ZL | B));     // (the option off: always the bindings)
    printf("battle moves by position: ok\n");
    return 0;
}
