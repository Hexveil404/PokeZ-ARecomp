// The settings' Controls tab (in game: menu.mm; start screen: launcher.mm, both through menu_tabs_draw), in option rows
// (runtime/ui/option_rows.h), and its Configure inputs page for the bindings (runtime/input_bind.h): DEVICE and the
// categories on the left, the chosen category's bindings on the right. A on a category moves the cursor to its
// bindings; A on a binding waits for the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over); B goes back a step (bindings -> categories -> the tab). Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"  // (SetFocusID, NavCursorVisible)
#include "option_rows.h"

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
void launcher_prompts_refresh(void);  // runtime/ui/launcher.mm
int input_pad_family(void);              // runtime/hle/input.c
int settings_on_steam_deck(void);        // runtime/settings.c
SDL_JoystickID input_active_pad(void);   // runtime/hle/input.c: the controller the game reads (0: none yet)
void input_apply_background(void);
bool menu_la_rows(void);  // runtime/ui/menu.mm: Link's Awakening's rows shown (the game loaded is it)
}
#define TR(en, pt) UI_TR(en, pt)  // (runtime/settings.h: the start screen's PT | EN)

// ------------------------------------------------------------------ Configure inputs: what's on it
enum { CAT_BUTTONS, CAT_TRIGGERS, CAT_STICKS, CAT_DPAD, N_CAT };
static const char *cat_name(int c) {
    switch (c) {
    case CAT_BUTTONS: return TR("Buttons", "Botões");
    case CAT_TRIGGERS: return TR("Triggers", "Gatilhos");
    case CAT_STICKS: return TR("Sticks", "Analógicos");
    default: return TR("D-pad", "Direcional");
    }
}
static const char *const k_cat_hook[N_CAT] = {"buttons", "triggers", "sticks", "dpad"};  // (LARECOMP_CONTROLS_SUB)

struct Bind { int slot; const char *label; const char *section; };  // section: a header before this row
static int cat_rows(int cat, bool pad, Bind *out) {
    int n = 0;
    auto add = [&](int slot, const char *label, const char *section = NULL) { out[n++] = {slot, label, section}; };
    const char *up = TR("Up", "Cima"), *down = TR("Down", "Baixo"), *left = TR("Left", "Esquerda"),
               *right = TR("Right", "Direita");
    switch (cat) {
    case CAT_BUTTONS:
        add(BIND_A, "A"), add(BIND_B, "B"), add(BIND_X, "X"), add(BIND_Y, "Y"), add(BIND_PLUS, TR("Plus", "Mais")),
            add(BIND_MINUS, TR("Minus", "Menos"));
        break;
    case CAT_TRIGGERS: add(BIND_L, "L"), add(BIND_R, "R"), add(BIND_ZL, "ZL"), add(BIND_ZR, "ZR"); break;
    case CAT_STICKS:  // (a controller's sticks stay its sticks: only their presses are buttons)
        add(BIND_LSTICK, TR("Press", "Apertar"), TR("Left stick", "Analógico esquerdo"));
        if (!pad) add(BIND_LS_UP, up), add(BIND_LS_DOWN, down), add(BIND_LS_LEFT, left), add(BIND_LS_RIGHT, right);
        add(BIND_RSTICK, TR("Press", "Apertar"), TR("Right stick", "Analógico direito"));
        if (!pad) add(BIND_RS_UP, up), add(BIND_RS_DOWN, down), add(BIND_RS_LEFT, left), add(BIND_RS_RIGHT, right);
        break;
    case CAT_DPAD: add(BIND_UP, up), add(BIND_DOWN, down), add(BIND_LEFT, left), add(BIND_RIGHT, right); break;
    }
    return n;
}
static int slot_cat(int slot) {
    Bind b[16];
    for (int c = 0; c < N_CAT; c++)
        for (int i = 0, n = cat_rows(c, false, b); i < n; i++)
            if (b[i].slot == slot) return c;
    return CAT_BUTTONS;
}

// The right stick's snap and the mouse (runtime/hle/input.c): option rows in the page's list. (They were drawn under
// the list with plain ImGui widgets, and the list takes the page's whole height: nobody could reach them - #44.)
static void stick_mouse_rows() {
    bool changed = false;
    opt_section(TR("Right stick", "Analógico direito"));
    {
        static const int deg[] = {0, 10, 15, 20, 25};
        const char *const names[] = {TR("Off", "Desligado"), TR("10 degrees", "10 graus"), TR("15 degrees", "15 graus"),
                                     TR("20 degrees", "20 graus"), TR("25 degrees", "25 graus")};
        int cur = 0;
        for (int i = 0; i < 5; i++) if (g_settings.rstick_snap == deg[i]) cur = i;
        if (opt_choice(TR("Snap to axis", "Encaixar no eixo"), &cur, names, 5, 0,
                       TR("Turning the camera sideways with the stick tilts it a little up or down when the stick isn't "
                          "pushed exactly sideways. Within this angle of an axis the stick counts as on it (15-20 degrees "
                          "for most controllers).",
                          "Girar a câmera para o lado com o analógico inclina ela um pouco para cima ou para baixo quando "
                          "o analógico não está exatamente de lado. Dentro deste ângulo de um eixo, o analógico conta "
                          "como estando nele (15-20 graus para a maioria dos controles)."), false)) {
            g_settings.rstick_snap = deg[cur];
            changed = true;
        }
    }
    opt_section(TR("Mouse", "Mouse"));
    {
        const char *const modes[] = {TR("Off", "Desligado"), TR("Hold the right button", "Segurando o botão direito"),
                                     TR("Always", "Sempre")};
        changed |= opt_choice(TR("Camera", "Câmera"), &g_settings.mouse_camera, modes, 3, MOUSE_CAM_CAPTURED,
                              TR("Always: while you play, the cursor is hidden and moving the mouse turns the camera; F1 or "
                                 "Esc (the menu) or switching windows gives the cursor back. The mouse buttons below work "
                                 "only then.",
                                 "Sempre: enquanto você joga, o cursor some e mexer o mouse gira a câmera; F1 ou Esc (o "
                                 "menu) ou trocar de janela devolve o cursor. Os botões do mouse abaixo só valem nesse modo."),
                              false);
        changed |= opt_slider(TR("Camera speed", "Velocidade da câmera"), &g_settings.mouse_sens, 0.1f, 5.0f, 0.05f, 1.0f,
                              "%.2f", TR("How fast the mouse turns the camera.", "O quanto o mouse gira a câmera."));
        // the Switch buttons a mouse button can press (as in Configure inputs)
        static const int opts[] = {-1, BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
                                   BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT, BIND_LSTICK, BIND_RSTICK};
        enum { NOPT = sizeof opts / sizeof opts[0] };
        const char *names[NOPT];
        for (int i = 0; i < NOPT; i++) names[i] = opts[i] >= 0 ? bind_input_name(opts[i]) : TR("(none)", "(nenhum)");
        for (int m = 0; m < MOUSEB_COUNT; m++) {
            int cur = 0, def = 0;
            for (int i = 0; i < NOPT; i++) {
                if (opts[i] == g_bind_mouse[m]) cur = i;
                if (opts[i] == bind_mouse_default(m)) def = i;
            }
            static const char *const mnames[MOUSEB_COUNT][2] = {{"Left button", "Botão esquerdo"}, {"Right button", "Botão direito"},
                {"Middle button", "Botão do meio"}, {"Back button", "Botão voltar"}, {"Forward button", "Botão avançar"}};
            if (opt_choice(TR(mnames[m][0], mnames[m][1]), &cur, names, NOPT, def,
                           TR("The Switch button this mouse button presses (Camera: Always).",
                              "O botão do Switch que este botão do mouse aperta (Câmera: Sempre)."))) {
                g_bind_mouse[m] = (int16_t)opts[cur];
                changed = true;
            }
        }
        if (!bind_mouse_is_default() &&
            opt_button(TR("Reset mouse buttons", "Voltar os botões do mouse ao padrão"),
                       TR("The left button presses ZR, the right one ZL, the others nothing.",
                          "O botão esquerdo aperta ZR, o direito ZL, os outros nada."))) {
            bind_mouse_reset();
            changed = true;
        }
    }
    if (changed) settings_save();
}

// Devices: each connected controller by name (they share one set of bindings; the name picks the button names), or
// "Controller" when none is connected (bindings for one connected later), then the keyboard.
struct Device { SDL_JoystickID id; char name[64]; int fam; bool pad; };
static int pad_fam(SDL_JoystickID id) {
    if (!id || getenv("LARECOMP_PAD_TYPE")) return bind_label_family();  // (none / a test pretending one)
    return input_family_of_type(SDL_GetGamepadTypeForID(id));
}
static int list_devices(Device *d, int cap) {
    int n = 0, np = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&np);
    for (int i = 0; i < np && n < cap - 1; i++) {
        const char *nm = SDL_GetGamepadNameForID(ids[i]);
        d[n].id = ids[i], d[n].pad = true, d[n].fam = pad_fam(ids[i]);
        snprintf(d[n].name, sizeof d[n].name, "%s", nm && *nm ? nm : TR("Controller", "Controle"));
        if (strlen(d[n].name) > 30) strcpy(d[n].name + 27, "...");  // (fits beside DEVICE)
        n++;
    }
    SDL_free(ids);
    if (!n) d[n].id = 0, d[n].pad = true, d[n].fam = pad_fam(0), snprintf(d[n].name, sizeof d[n].name, "%s", TR("Controller", "Controle")), n++;
    d[n].id = 0, d[n].pad = false, d[n].fam = 0, snprintf(d[n].name, sizeof d[n].name, "%s", TR("Keyboard", "Teclado")), n++;
    return n;
}

// ------------------------------------------------------------------ state
static bool g_sub;                 // on the Configure inputs page
static int g_cat = CAT_BUTTONS;    // the category shown on the right
static bool g_dev_pad = true;      // the device chosen: a controller (g_dev_id) or the keyboard
static SDL_JoystickID g_dev_id;
static bool g_in_right;            // the cursor is on a binding (last frame)
static int g_last_frame = -10;     // (a frame without this tab: the page starts over)
static int g_back_frame = -10;     // the frame B / Esc went back a step here (the menu mustn't close on it)
enum { FOCUS_NONE, FOCUS_CONFIGURE, FOCUS_DEVICE, FOCUS_CAT, FOCUS_RIGHT };
static int g_focus;                // put the cursor on this row when it's drawn

// B / Esc this frame went back a step on the Configure inputs page (menu.mm / launcher.mm: don't close too)
extern "C" bool controls_ui_took_back(void) { return g_back_frame == ImGui::GetFrameCount(); }

static void take_focus(int want) {  // (right after the row: it's the last item)
    if (g_focus != want) return;
    g_focus = FOCUS_NONE;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    ImGui::SetScrollHereY(0.3f);
}

// The device the page opens on: the keyboard when it was opened with one (Enter / Space), else the controller the game
// reads (or the first connected; "Controller" when there's none)
static void pick_device(bool keyboard) {
    SDL_JoystickID act = input_active_pad();
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!act && n > 0) act = ids[0];
    SDL_free(ids);
    g_dev_pad = !keyboard, g_dev_id = act;
}
static bool keyboard_pressed(void) {
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
}

// A finished capture keeps larecomp.ini current; a swap is said under the list (another row changed too)
static void watch_results(int fam, char *msg, size_t cap) {
    static int seen = -1;
    int rdev, rslot, rother, serial, kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen < 0) seen = serial;
    if (serial == seen) return;
    seen = serial;
    msg[0] = 0;
    if (kind == 2)
        snprintf(msg, cap, "%s moved to %s (swapped).", bind_input_name(rother),
                 rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rother], fam) : bind_key_label(g_bind_key[rother]));
    if (kind == 1 || kind == 2 || kind == 5) {
        settings_save();
        launcher_prompts_refresh();  // (the game's button icons drawn again for the new bindings: runtime/ui/launcher.mm)
    }
}

static bool nav_on_item(void) { return ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible; }

static void configure_inputs(float scale) {
    Device devs[9];
    int nd = list_devices(devs, 9), di = -1;
    for (int i = 0; i < nd; i++)
        if (devs[i].pad == g_dev_pad && (!g_dev_pad || devs[i].id == g_dev_id)) di = i;
    if (di < 0) di = g_dev_pad ? 0 : nd - 1;  // (that controller is gone: the first one)
    g_dev_pad = devs[di].pad, g_dev_id = devs[di].id;
    const int dev = g_dev_pad ? BIND_DEV_PAD : BIND_DEV_KEY, fam = devs[di].fam;

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    static char msg[160];
    watch_results(fam, msg, sizeof msg);

    // B / Esc: from the bindings back to their category, from the categories back to the tab (a capture takes Esc and
    // every controller button itself: runtime/hle/bindings.c)
    if (cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        g_back_frame = ImGui::GetFrameCount();
        if (g_in_right) g_focus = FOCUS_CAT;
        else { g_sub = false, g_focus = FOCUS_CONFIGURE, msg[0] = 0; return; }  // (the tab is drawn instead)
    }
    // Left on a binding: back to its category too
    if (g_in_right && cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                                 ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)))
        g_focus = FOCUS_CAT;

    opt_begin("##configure", scale);
    opt_section(TR("Configure inputs", "Configurar comandos"));
    const char *names[9];
    for (int i = 0; i < nd; i++) names[i] = devs[i].name;
    int ndi = di;
    if (opt_choice(TR("Device", "Dispositivo"), &ndi, names, nd, di, NULL)) {
        bind_capture_cancel();
        g_dev_pad = devs[ndi].pad, g_dev_id = devs[ndi].id, msg[0] = 0;
    }
    take_focus(FOCUS_DEVICE);
    for (int c = 0; c < N_CAT; c++) {
        if (g_in_right && c == g_cat) opt_mark_next();
        bool open = opt_link(cat_name(c), NULL);
        if (c == g_cat) take_focus(FOCUS_CAT);
        if (nav_on_item() && g_cat != c) g_cat = c, msg[0] = 0;  // (moving down the list shows each)
        if (open) { if (g_cat != c) msg[0] = 0; g_cat = c, g_focus = FOCUS_RIGHT; bind_capture_cancel(); }
    }
    if (bind_is_default(dev)) opt_disable_next();
    if (opt_action(TR("Restore default controls", "Voltar os comandos ao padrão"), "",
                   g_dev_pad ? TR("Puts every controller button back to its default.", "Volta cada botão do controle ao padrão.")
                             : TR("Puts every key back to its default.", "Volta cada tecla ao padrão."))) {
        bind_capture_cancel();
        bind_reset(dev);
    }
    // (the cursor or the mouse on a row with a description: it shows on the right instead of the bindings)
    bool desc_right = (ImGui::IsItemHovered() || nav_on_item()) && g_focus != FOCUS_RIGHT;
    opt_split();
    if (desc_right) {
        opt_desc_text();
        g_in_right = false;
    } else {
        Bind rows[16];
        int n = cat_rows(g_cat, g_dev_pad, rows);
        opt_section(cat_name(g_cat));
        bool in_right = false;
        for (int i = 0; i < n; i++) {
            const Bind &b = rows[i];
            if (b.section) opt_section(b.section);
            bool waiting = cd == dev && cs == b.slot;
            char val[64];
            if (waiting) snprintf(val, sizeof val, "%s...  %d", g_dev_pad ? TR("Press a button", "Aperte um botão")
                                                                          : TR("Press a key", "Aperte uma tecla"), (ms + 999) / 1000);
            else snprintf(val, sizeof val, "%s", g_dev_pad ? bind_pad_label(g_bind_pad[b.slot], fam) : bind_key_label(g_bind_key[b.slot]));
            // the same button / key on two inputs (a hand-edited larecomp.ini): red
            const int16_t *t = g_dev_pad ? g_bind_pad : g_bind_key;
            bool dup = false;
            for (int j = 0, m = g_dev_pad ? BIND_PAD_COUNT : BIND_KEY_COUNT; j < m; j++)
                dup |= j != b.slot && t[j] == t[b.slot] && t[b.slot] > (g_dev_pad ? -1 : 0);
            if (dup && !waiting) opt_warn_next();
            ImGui::PushID(b.slot);
            bool hit = opt_action(b.label, val, NULL, true, waiting || !bind_is_default_slot(dev, b.slot));
            ImGui::PopID();
            if (i == 0) take_focus(FOCUS_RIGHT);
            if (ImGui::IsItemFocused()) in_right = true;
            if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
            if (hit) {
                if (waiting) bind_capture_cancel();
                else { msg[0] = 0; bind_capture_begin(dev, b.slot); }
            }
        }
        g_in_right = in_right;
        if (cd == dev) ImGui::TextDisabled("%s", TR("Esc cancels.", "Esc cancela."));
        else if (msg[0]) ImGui::TextDisabled("%s", msg);
    }
    opt_split_end();
}

// ------------------------------------------------------------------ the tab
extern "C" void controls_tab_draw(float scale) {
    int frame = ImGui::GetFrameCount();
    if (frame - g_last_frame > 1) g_sub = false, g_focus = FOCUS_NONE, g_in_right = false;  // (back on the tab)
    g_last_frame = frame;
    // (test) LARECOMP_CONTROLS_SUB=<buttons|triggers|sticks|dpad>[:right][:key]: on that page, the cursor on the category
    // (right: on its first binding; key: the keyboard's) - for a few frames, past the menu putting it on the tab bar
    static bool hook_done;
    static int hook_frames, hook_focus;
    if (!hook_done) {
        hook_done = true;
        if (const char *h = getenv("LARECOMP_CONTROLS_SUB")) {
            pick_device(strstr(h, ":key") != NULL);
            g_sub = true, hook_focus = strstr(h, "right") ? FOCUS_RIGHT : FOCUS_CAT, hook_frames = 10;
            for (int c = 0; c < N_CAT; c++) if (!strncmp(h, k_cat_hook[c], strlen(k_cat_hook[c]))) g_cat = c;
        }
    }
    if (hook_frames > 0) hook_frames--, g_focus = hook_focus;
    {   // a capture started elsewhere (LARECOMP_BIND_TEST): show it waiting
        int cd, cs;
        if (bind_capture_active(&cd, &cs, NULL) && (!g_sub || g_dev_pad != (cd == BIND_DEV_PAD) || g_cat != slot_cat(cs))) {
            if (!g_sub) pick_device(cd == BIND_DEV_KEY);
            g_sub = true, g_dev_pad = cd == BIND_DEV_PAD, g_cat = slot_cat(cs);
        }
    }
    if (g_sub) {
        configure_inputs(scale);
        if (g_sub) return;
    }

    opt_begin("##controls", scale);
    opt_section(TR("Inputs", "Comandos"));
    if (opt_link(TR("Configure inputs", "Configurar comandos"),
                 TR("Choose the buttons and keys for the game. The menu always opens with F1, Esc, Home or Back + Start, "
                    "so no binding can lock you out.",
                    "Escolha os botões e as teclas do jogo. O menu sempre abre com F1, Esc, Home ou Back + Start, então "
                    "nenhum comando deixa você preso fora dele."))) {
        pick_device(keyboard_pressed());
        g_sub = true, g_focus = FOCUS_DEVICE, g_in_right = false;
    }
    take_focus(FOCUS_CONFIGURE);
    if (opt_toggle(TR("Allow background inputs", "Comandos em segundo plano"), &g_settings.bg_input, false,
                   TR("Controllers keep working while another window is in front. macOS may ask for Input Monitoring "
                      "permission.", "Os controles continuam funcionando com outra janela na frente. O macOS pode pedir a "
                      "permissão de Monitoramento de Entrada.")))
        input_apply_background();

    if (menu_la_rows()) {  // (Link's Awakening's movement hooks: runtime/hle/qol.c, nothing for Pokémon Z-A)
        opt_section("Movement");
        opt_toggle("D-pad moves Link", &g_settings.dpad_move, false, "The d-pad moves Link like the control stick.");
        opt_toggle("360-degree movement", &g_settings.move_360, false,
                   "Link walks in the exact direction of the stick instead of snapping to 8 directions.");
    }

    opt_section(TR("Controller", "Controle"));
    opt_toggle(TR("Rumble", "Vibração"), &g_settings.rumble, true, TR("Controller vibration.", "A vibração do controle."));
    if (g_settings.rumble) {
        static const char *const pct[] = {"25%", "50%", "75%", "100%"};
        int i = g_settings.rumble_strength / 25 - 1;
        if (i < 0 || i > 3) i = 3;
        opt_indent(1);
        if (opt_choice(TR("Intensity", "Intensidade"), &i, pct, 4, 3,
                       TR("How strong the vibration is.", "A força da vibração."), false))
            g_settings.rumble_strength = (i + 1) * 25;
        opt_indent(-1);
    }
    if (menu_la_rows())  // (the light bar shows Link's health: runtime/led.h)
        opt_toggle("Controller light", &g_settings.controller_light, true, "The light bar shows Link's health.");
    {
        char autol[64];  // (what Auto stands for now: bind_prompts_auto, the same rule as runtime/hle/pl.c)
        const char *names[] = {autol, "Nintendo", "Xbox", "PlayStation", TR("Keyboard", "Teclado"), "Steam Deck"};
        snprintf(autol, sizeof autol, "Auto (%s)", names[bind_prompts_auto()]);
        opt_choice(TR("Button icons", "Ícones dos botões"), &g_settings.prompts, names, 6, PROMPTS_AUTO,
                   TR("The button icons the game shows. Auto: the connected controller's (the keyboard's with none). "
                      "Applies from the next start.",
                      "Os ícones de botões que o jogo mostra. Auto: os do controle conectado (os do teclado se não houver "
                      "controle). Vale a partir do próximo início."));
    }
    opt_toggle(TR("Battle moves by position", "Golpes por posição"), &g_settings.battle_abxy_position, false,
               TR("While ZL is held, the controller's A / B / X / Y go by position (the Switch's A on the right), so each "
                  "move is on the button where the game draws it. The rest of the game keeps your bindings.",
                  "Com ZL segurado, o A / B / X / Y do controle valem pela posição (o A do Switch fica à direita), então cada "
                  "golpe fica no botão onde o jogo desenha ele. O resto do jogo mantém os seus comandos."));
    opt_toggle(TR("Xbox attack layout", "Layout de golpes Xbox"), &g_settings.xbox_attack_layout, false,
               TR("Place battle moves at their bound controller buttons without changing controls. Prepared automatically. "
                  "Leave Battle moves by position off. Restart after changing this option or bindings.",
                  "Posiciona os golpes nos botões configurados do controle, sem alterar os comandos. Preparado automaticamente. "
                  "Deixe Golpes por posição desligado. Reinicie após mudar esta opção ou os comandos."));
    {
        const char *const names[] = {TR("Game layout", "Layout do jogo"), TR("Controller convention", "Padrão do controle")};
        opt_choice(TR("Confirm button", "Botão de confirmar"), &g_settings.menu_confirm, names, 2, MENU_CONFIRM_CONTROLLER,
                   TR("Which button confirms in ZA Recomp's menus.", "Qual botão confirma nos menus do ZA Recomp."));
    }

    stick_mouse_rows();
    opt_end();
}
