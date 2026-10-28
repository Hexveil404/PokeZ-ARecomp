// Control remapping (runtime/hle/bindings.c): which controller button / key drives each Switch input. Edited from
// the in-game menu's Controls tab and the start screen (runtime/ui/controls_ui.mm), saved in larecomp.ini.
// Menus (the overlay, the start screen) never use these, so no binding can lock the player out: they follow the
// game's layout, or the controller's own (Confirm button; runtime/ui/pad_nav.h).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Switch inputs, in nn::hid button-bit order (runtime/hle/input.c BTN_*), then the keyboard's stick directions.
enum {
    BIND_A, BIND_B, BIND_X, BIND_Y, BIND_LSTICK, BIND_RSTICK, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
    BIND_LEFT, BIND_UP, BIND_RIGHT, BIND_DOWN,
    BIND_PAD_COUNT,                                      // controller: the 16 buttons (its sticks stay the sticks)
    BIND_LS_UP = BIND_PAD_COUNT, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT,
    BIND_RS_UP, BIND_RS_DOWN, BIND_RS_LEFT, BIND_RS_RIGHT,
    BIND_KEY_COUNT                                       // keyboard: buttons + both sticks
};
enum { BIND_DEV_PAD, BIND_DEV_KEY };
// Controller binding values: an SDL_GamepadButton, or the analog triggers as buttons
enum { PADB_LTRIGGER = 26 /* SDL_GAMEPAD_BUTTON_COUNT */, PADB_RTRIGGER, PADB_HOST_COUNT };

extern int16_t g_bind_pad[BIND_PAD_COUNT];  // PADB value per Switch button
extern int16_t g_bind_key[BIND_KEY_COUNT];  // SDL_Scancode per input (0 = none)
// Mouse buttons -> Switch button (BIND_A..BIND_DOWN, -1 none): the other way round from the tables above, so a mouse
// button adds to the keyboard's key instead of taking its place. Only while the mouse camera holds the cursor.
enum { MOUSEB_LEFT, MOUSEB_RIGHT, MOUSEB_MIDDLE, MOUSEB_X1, MOUSEB_X2, MOUSEB_COUNT };
extern int16_t g_bind_mouse[MOUSEB_COUNT];
const char *bind_mouse_name(int mb);           // "Left button"
int bind_mouse_from_sdl(int sdl_button);       // SDL_BUTTON_* -> MOUSEB_* (-1: another button)
void bind_mouse_reset(void);
int bind_mouse_default(int mb);                 // the BIND_* that mouse button presses by default (-1: none)
bool bind_mouse_is_default(void);

const char *bind_input_name(int slot);         // "A", "D-pad Up", "Left stick Up"
const char *bind_pad_label(int host, int fam); // controller button in the pad family's own words (fam: input_pad_family)
const char *bind_key_label(int scancode);
int bind_label_family(void);                   // the connected controller's family (also on the start screen)
int bind_prompts_auto(void);                   // what "Auto" button icons are now (PROMPTS_*)
int input_family_of_type(int sdl_gamepad_type); // runtime/hle/input.c: 0 Nintendo, 1 Xbox (or labelled like one), 2 PS
int input_menu_family(uint32_t sdl_joystick_id); // runtime/hle/input.c: our menus' layout for that pad (0: any)
void bind_reset(int dev);
bool bind_is_default(int dev);
bool bind_is_default_slot(int dev, int slot);

// "Click a binding, then press": begun from the UI; the next press (any thread's SDL event) is taken. Escape cancels,
// as does BIND_CAPTURE_MS without a press. On a conflict the two inputs swap.
#define BIND_CAPTURE_MS 5000
void bind_capture_begin(int dev, int slot);
void bind_capture_cancel(void);
// capturing: dev / slot (else -1), ms left
bool bind_capture_active(int *dev, int *slot, int *ms_left);
// Main thread, before anything else sees the event: true = consumed by a capture
bool bind_capture_event(const SDL_Event *e);
// Last change for the UI: 0 none, 1 set, 2 swapped (with *other), 3 cancelled, 4 timed out, 5 reset; *serial bumps
int bind_last_result(int *dev, int *slot, int *other, int *serial);

// larecomp.ini: bind_pad_<input> / bind_key_<input> (only the ones that differ from the defaults are written)
bool bind_settings_load(const char *key, const char *val);
void bind_settings_save(FILE *f);

// bind_* names (larecomp.ini): Switch input "a", "zl", "ls_up"... -> slot; controller button "south"... -> PADB value
int bind_slot_from_name(const char *name);
int bind_pad_from_name(const char *name);

// Button prompts (runtime/nvn/texpatch.c): the controller button now driving Switch input `slot` (BIND_A..)
int bind_prompt_host(int slot);
// The controller button Switch input `slot` is on in the default layout (by position: the Switch's A = east)
int bind_pad_default(int slot);

#ifdef __cplusplus
}
#endif
