// Deterministic IPC -> real input bridge -> fake SDL sink regression test.
// Compile with -O2; unrelated input entry points are made translation-unit local.
// HLE registrations are suppressed so unrelated runtime functions can be dropped.
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

static uint64_t ticks = 1000;
static bool menu, focus = true, supported = true;
static unsigned calls;
static Uint16 low, high;
static Uint32 duration;
Uint64 SDL_GetTicks(void) { return ticks; }
SDL_Window *SDL_GetKeyboardFocus(void) { return focus ? (SDL_Window *)1 : NULL; }
bool menu_is_open(void) { return menu; }
bool menu_cursor_needed(void) { return false; }  // (ZA Recomp's input.c: the mouse cursor for F1 / the start screen)
Settings g_settings = {.rumble = true, .rumble_strength = 100};  // (the Rumble settings: on, full, as by default)
void SDL_LockSpinlock(SDL_SpinLock *p) { (void)p; }
void SDL_UnlockSpinlock(SDL_SpinLock *p) { (void)p; }
const char *SDL_GetError(void) { return "test: unsupported controller"; }
bool SDL_RumbleGamepad(SDL_Gamepad *p, Uint16 l, Uint16 h, Uint32 ms) {
    assert(p == (SDL_Gamepad *)1); calls++; low = l; high = h; duration = ms;
    return supported;
}
// Unrelated HID service cases in the same switch; never called by these tests.
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { abort(); }
KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle) { abort(); }
Service *service_new(const char *n, ServiceHandler h, void *state) { abort(); }
void ipc_return_object(IpcResp *r, Service *s) { abort(); }
// The controller light (input_led_poll) and the menus' button family (input_menu_family) in input.c; not under test.
int game_in_play(void) { abort(); }
int cheats_link_health(int *max) { abort(); }
int bind_label_family(void) { abort(); }

static IpcResp request(uint32_t cmd, const void *data, uint32_t size) {
    IpcReq r = {.cmd=cmd, .data=data, .data_size=size}; IpcResp out = {0};
    hid(NULL, &r, &out); return out;
}
static void single(uint32_t h, RumbleValue v) {
    uint8_t payload[32] = {0};
    memcpy(payload, &h, 4); memcpy(payload+4, &v, sizeof v);
    assert(request(201, payload, sizeof payload).result == 0);
}
int main(void) {
    assert(rumble_amplitude(NAN) == 0 && rumble_amplitude(INFINITY) == 0);
    assert(rumble_amplitude(-1) == 0 && rumble_amplitude(2) == 65535);
    assert(rumble_amplitude(0.5f) == 32768);
    assert(rumble_side(0x03000000) == 0 && rumble_side(0x03000001) == 1);
    assert(rumble_side(3) == 0 && rumble_side(0x10003) == 1);
    assert(rumble_side(0x03000100) == -1 && rumble_side(0x20003) == -1);
    uint32_t info[2], handle = 0x03000001;
    Cpu cpu = {0}; cpu.x[0] = (uintptr_t)info; cpu.x[1] = (uintptr_t)&handle;
    hid_vibration_info(&cpu); assert(info[0] == 1 && info[1] == 2);
    uint32_t generated[2], player=0;
    cpu.x[0]=(uintptr_t)generated; cpu.x[1]=2; cpu.x[2]=(uintptr_t)&player;
    hid_vibration_handles(&cpu);
    assert(cpu.x[0]==2 && generated[0]==3 && generated[1]==0x10003);
    assert(request(211, &generated[0], 4).data[0]==1);
    pad = (SDL_Gamepad *)1; rumble_frame_ms = ticks;
    single(0x03000000, (RumbleValue){.low_amp=.5f, .high_amp=.25f});
    input_rumble_poll(); assert(low == 32768 && high == 16384 && duration == 250);
    uint8_t query[16] = {0}; uint32_t query_handle = 0x03000000;
    memcpy(query, &query_handle, 4);
    IpcResp actual = request(202, query, sizeof query); RumbleValue readback;
    memcpy(&readback, actual.data, sizeof readback);
    assert(actual.data_size == 16 && readback.low_amp == .5f && readback.high_amp == .25f);
    unsigned before = calls; ticks += 50; input_rumble_poll(); assert(calls == before);
    ticks += 50; input_rumble_poll(); assert(calls == before + 1); // renewal

    uint32_t handles[2] = {0x03000000, 0x03000001};
    RumbleValue values[2] = {{.low_amp=.25f}, {.high_amp=.75f}};
    IpcReq batch = {.cmd=206, .nx=2, .x={
        {(uintptr_t)handles, sizeof handles}, {(uintptr_t)values, sizeof values}}};
    IpcResp out = {0}; hid(NULL, &batch, &out);
    input_rumble_poll(); assert(low == 16384 && high == 49151);
    single(handles[0], (RumbleValue){0}); input_rumble_poll();
    assert(low == 0 && high == 49151); // left zero must not erase right effect
    single(handles[1], (RumbleValue){0}); input_rumble_poll(); assert(low == 0 && high == 0);
    batch.x[1].size--; out = (IpcResp){0}; hid(NULL, &batch, &out); assert(out.result != 0);
    assert(request(201, handles, 4).result != 0);

    uint8_t permission = 0; request(204, &permission, 1);
    assert(request(205, NULL, 0).data[0] == 0);
    single(handles[0], (RumbleValue){.low_amp=1}); input_rumble_poll(); assert(low == 0);
    permission = 1; request(204, &permission, 1); assert(request(205, NULL, 0).data[0] == 1);
    single(handles[0], (RumbleValue){.low_amp=1}); input_rumble_poll(); assert(low == 65535);
    ticks += 300; input_rumble_poll(); assert(low == 0 && high == 0); // stall watchdog
    rumble_frame_ms = ticks;
    single(handles[0], (RumbleValue){.low_amp=1}); menu = true; input_rumble_poll(); assert(low == 0);
    menu = false; single(handles[0], (RumbleValue){.low_amp=1}); focus = false;
    input_rumble_poll(); assert(low == 0); focus = true;
    g_settings.rumble = false; single(handles[0], (RumbleValue){.low_amp=1});
    input_rumble_poll(); assert(low == 0); g_settings.rumble = true;  // Rumble setting off
    g_settings.rumble_strength = 50; single(handles[0], (RumbleValue){.low_amp=1});
    input_rumble_poll(); assert(low == 32767); g_settings.rumble_strength = 100;  // Intensity 50%
    single(handles[0], (RumbleValue){0}); input_rumble_poll(); assert(low == 0);
    pad = NULL; single(handles[0], (RumbleValue){.low_amp=1}); before = calls;
    input_rumble_poll(); assert(calls == before); pad = (SDL_Gamepad *)1;
    input_rumble_poll(); assert(low == 0); // no replay on reconnect
    supported = false; single(handles[0], (RumbleValue){.low_amp=1});
    input_rumble_poll(); assert(rumble_warned); // graceful unsupported hardware
    input_rumble_reset(); assert(low == 0 && high == 0 && input_rumble_permitted());
    puts("PASS: IPC, mapping, mixing, permission, renewal, stop, stall, focus/menu, reconnect, unsupported, reset");
    return 0;
}
