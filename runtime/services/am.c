// Applet manager (appletOE), apm, and lm.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../settings.h"

#define AM_NO_MESSAGE MAKE_RESULT(128, 3)
#define AM_NO_DATA MAKE_RESULT(128, 2)

static uint32_t msg_event_r, msg_event_w;
static KEvent *msg_event;
static uint32_t msg_queue[16];
static int msg_count;

static bool preselected_popped;
RT_ON_RESET(am_reset) {  // (reboot: the message queue starts over with FocusStateChanged)
    msg_event = NULL;
    msg_event_r = msg_event_w = 0;
    msg_count = 0;
    preselected_popped = false;
}

static void am_init_once(void) {
    if (msg_event) return;
    msg_event = kernel_create_event_pair(&msg_event_w, &msg_event_r);
    msg_queue[msg_count++] = 15;  // FocusStateChanged
    pthread_mutex_lock(&g_klock);
    kernel_signal(msg_event);
    pthread_mutex_unlock(&g_klock);
}

static uint32_t new_event(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

#define UNHANDLED(iface) LOG("[am] %s cmd %u (unhandled, ok)\n", iface, req->cmd)

// The mode the game is told: docked (1080p, performance boost) or handheld (720p, normal). Handheld draws far fewer pixels, which is what
// a Steam Deck's GPU can carry (docked: ~140 ms of GPU per frame in the city at 1920x1080, 1600 MHz).
bool am_handheld(void) {   // (also the render scale's: runtime/nvn/output_scale.c)
    static int v = -1;
    extern int settings_on_steam_deck(void);
    if (v < 0) {
        const char *e = getenv("LARECOMP_OPERATION_MODE");
        v = e ? !strcmp(e, "handheld") : g_settings.opmode == 2 || (g_settings.opmode == 0 && settings_on_steam_deck());
        LOG("[am] game mode: %s (%s)\n", v ? "handheld" : "docked", e ? "LARECOMP_OPERATION_MODE" : g_settings.opmode ? "chosen" : "auto");
    }
    return v;
}

static void common_state_getter(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = msg_event_r; break;  // GetEventHandle
    case 1:  // ReceiveMessage
        pthread_mutex_lock(&g_klock);
        if (msg_count) {
            OUT(uint32_t, 0) = msg_queue[0];
            memmove(msg_queue, msg_queue + 1, --msg_count * sizeof(uint32_t));
            if (!msg_count) msg_event->signaled = false;
            resp_size(resp, 4);
        } else {
            resp->result = AM_NO_MESSAGE;
        }
        pthread_mutex_unlock(&g_klock);
        break;
    case 5: OUT(uint8_t, 0) = am_handheld() ? 0 : 1; resp_size(resp, 1); break;   // GetOperationMode: docked (handheld: LARECOMP_OPERATION_MODE=handheld, operation_mode in the ini)
    case 6: OUT(uint32_t, 0) = am_handheld() ? 0 : 1; resp_size(resp, 4); break;  // GetPerformanceMode: boost (handheld: normal)
    case 8: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;   // GetBootMode
    case 9: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;   // GetCurrentFocusState: in focus
    case 13: case 61: resp->copy_handles[resp->ncopy++] = new_event(); break;
    case 60: {  // GetDefaultDisplayResolution
        int h = am_handheld() ? 720 : 1080, w = getenv("LARECOMP_DISPLAY_W") ? atoi(getenv("LARECOMP_DISPLAY_W")) : h * 16 / 9;
        LOG("[am] GetDefaultDisplayResolution -> %dx%d (%s)\n", w, h, am_handheld() ? "handheld" : "docked");
        OUT(int32_t, 0) = w; OUT(int32_t, 4) = h; resp_size(resp, 8); break;
    }
    default: UNHANDLED("ICommonStateGetter");
    }
}

static void self_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: LOG("[am] application requested exit\n"); exit(0);
    case 9: {  // GetLibraryAppletLaunchableEvent: signalled - applets can start (nn::applet::CreateLibraryApplet
        // waits on it first: unsignalled, the game froze the first time it wanted to show an error dialog)
        uint32_t w, r;
        KEvent *e = kernel_create_event_pair(&w, &r);
        pthread_mutex_lock(&g_klock);
        kernel_signal(e);
        pthread_mutex_unlock(&g_klock);
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 40: OUT(uint64_t, 0) = 1; resp_size(resp, 8); break;  // CreateManagedDisplayLayer
    case 69: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    default: UNHANDLED("ISelfController");
    }
}

static void window_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: OUT(uint64_t, 0) = 0x1234; resp_size(resp, 8); break;  // GetAppletResourceUserId
    default: UNHANDLED("IWindowController");
    }
}

static void generic_ok(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[am] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

// ------------------------------------------------------------------ IStorage / IStorageAccessor
typedef struct { uint8_t *data; uint64_t size; } AmStorage;
static AmStorage *g_last_storage;  // (the last storage the game wrote: the one it pushes to an applet next)

static void storage_accessor(Service *s, IpcReq *req, IpcResp *resp) {
    AmStorage *st = s->state;
    switch (req->cmd) {
    case 0: OUT(int64_t, 0) = (int64_t)st->size; resp_size(resp, 8); break;  // GetSize
    case 10: {  // Write(offset, A)
        int64_t off = IN(int64_t, 0);
        IpcBuf *a = in_buf(req, 0);
        uint64_t n = a->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy(st->data + off, (void *)(uintptr_t)a->addr, n);
        g_last_storage = st;
        break;
    }
    case 11: {  // Read(offset, B)
        int64_t off = IN(int64_t, 0);
        IpcBuf *b = out_buf(req, 0);
        uint64_t n = b->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy((void *)(uintptr_t)b->addr, st->data + off, n);
        break;
    }
    default: LOG("[am] IStorageAccessor cmd %u (ok)\n", req->cmd);
    }
}

static void am_storage(Service *s, IpcReq *req, IpcResp *resp) {
    if (req->cmd == 0) ipc_return_object(resp, service_new("IStorageAccessor", storage_accessor, s->state));
    else LOG("[am] IStorage cmd %u (ok)\n", req->cmd);
}

static Service *new_am_storage(const void *data, uint64_t size) {
    AmStorage *st = calloc(1, sizeof *st);
    st->data = calloc(1, size);
    st->size = size;
    if (data) memcpy(st->data, data, size);
    return service_new("IStorage", am_storage, st);
}

extern const uint8_t g_user_id[16];

// ------------------------------------------------------------------ ILibraryAppletCreator / ILibraryAppletAccessor
// System applets the game starts (the error dialog: nn::err::ShowApplicationError and the like; swkbd is hooked at its
// own function, runtime/hle/swkbd.c). The creator only answered "ok" with no object, so the game waited forever for
// an applet that never ran: the game froze for good the first time it wanted to show an error dialog (a
// communication error, for one). Now the applet runs and finishes at once, as if the player had pressed OK: its state
// event is signalled, it has no output, and its input (the error's arguments) is logged.
static const char *applet_name(uint32_t id) {
    switch (id) {
    case 0x0a: return "auth";
    case 0x0b: return "amiibo";
    case 0x0c: return "controller";
    case 0x0d: return "data erase";
    case 0x0e: return "error";
    case 0x0f: return "net connect";
    case 0x10: return "player select";
    case 0x11: return "keyboard";
    case 0x12: return "Mii edit";
    case 0x13: return "web";
    case 0x14: return "shop";
    case 0x15: return "photo viewer";
    case 0x16: return "settings";
    case 0x17: return "web (offline)";
    case 0x18: return "login share";
    case 0x19: return "wifi web auth";
    case 0x1a: return "my page";
    default: return "?";
    }
}
// The browser applets (web, shop, offline web) hand back how they were left, as the console's do: Pokemon Z-A's
// "Downloadable Content Available" on the title screen (Minus, without the DLC) opens the shop on its add-on content
// page (?scene=aocs&mode=mini), then pops that answer - with none, it aborted at once (#194: "the game stopped at one
// of its own checks"). Their answer here: WebCommonReturnValue (exit reason u32, pad, last URL [0x1000], its length
// u64) with exit reason 0, the close button - the player left the shop without buying.
static bool applet_is_web(uint32_t id) { return id == 0x13 || id == 0x14 || id == 0x17; }
typedef struct { uint32_t id; KEvent *state_ev; uint32_t state_r; bool started, out_popped; } AmApplet;
static void applet_accessor(Service *s, IpcReq *req, IpcResp *resp) {
    AmApplet *a = s->state;
    switch (req->cmd) {
    case 0:  // GetAppletStateChangedEvent: signalled once it has run (Start)
        resp->copy_handles[resp->ncopy++] = a->state_r;
        break;
    case 1:  // IsCompleted
        OUT(uint8_t, 0) = a->started;
        resp_size(resp, 1);
        break;
    case 10:  // Start: done at once
        a->started = true;
        LOG("[am] %s applet (0x%02x): finished at once (as if dismissed)\n", applet_name(a->id), a->id);
        pthread_mutex_lock(&g_klock);
        kernel_signal(a->state_ev);
        pthread_mutex_unlock(&g_klock);
        break;
    case 30:  // GetResult: fine
        break;
    case 100: case 103: {  // PushInData / PushInteractiveInData: the arguments, logged (the error applet's code)
        AmStorage *st = g_last_storage;
        if (st && st->size >= 16) {
            char hex[3 * 48 + 1] = "";
            for (uint64_t i = 0; i < st->size && i < 48; i++) snprintf(hex + 3 * i, 4, "%02x ", st->data[i]);
            LOG("[am] %s applet input (%llu bytes): %s\n", applet_name(a->id), (unsigned long long)st->size, hex);
        }
        break;
    }
    case 101:  // PopOutData: the browser applets' answer (once, after Start), else nothing
        if (applet_is_web(a->id) && a->started && !a->out_popped) {
            static uint8_t ret[0x1010];  // (WebCommonReturnValue: all zero = exit reason 0, the close button; no URL)
            a->out_popped = true;
            LOG("[am] %s applet: answers \"closed by the player\" (%u bytes)\n", applet_name(a->id), (unsigned)sizeof ret);
            ipc_return_object(resp, new_am_storage(ret, sizeof ret));
        } else {
            resp->result = AM_NO_DATA;
        }
        break;
    case 104:  // PopInteractiveOutData: nothing
        resp->result = AM_NO_DATA;
        break;
    case 105: {  // GetPopOutDataEvent: signalled when there is an answer (the browser applets', at Start)
        uint32_t w, r;
        KEvent *ev = kernel_create_event_pair(&w, &r);
        if (applet_is_web(a->id)) {
            pthread_mutex_lock(&g_klock);
            kernel_signal(ev);
            pthread_mutex_unlock(&g_klock);
        }
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 106:  // GetPopInteractiveOutDataEvent: never
        resp->copy_handles[resp->ncopy++] = new_event();
        break;
    default: LOG("[am] ILibraryAppletAccessor (%s) cmd %u (ok)\n", applet_name(a->id), req->cmd);
    }
}
static void library_applet_creator(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // CreateLibraryApplet(applet id, mode)
        AmApplet *a = calloc(1, sizeof *a);
        a->id = IN(uint32_t, 0);
        uint32_t w;
        a->state_ev = kernel_create_event_pair(&w, &a->state_r);
        LOG("[am] the game starts the %s applet (0x%02x, mode %u)\n", applet_name(a->id), a->id, IN(uint32_t, 4));
        ipc_return_object(resp, service_new("ILibraryAppletAccessor", applet_accessor, a));
        break;
    }
    case 10: case 11: case 12:  // CreateStorage(size) / CreateTransferMemoryStorage / CreateHandleStorage
        ipc_return_object(resp, new_am_storage(NULL, IN(uint64_t, 0) ? IN(uint64_t, 0) : 0x10));
        break;
    default: UNHANDLED("ILibraryAppletCreator");
    }
}

static void application_functions(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1:  // PopLaunchParameter(kind)
        if (IN(uint32_t, 0) == 2 && !preselected_popped) {  // PreselectedUser
            uint8_t p[0x88] = {0};
            *(uint32_t *)p = 0xC79497CA;
            *(uint32_t *)(p + 4) = 1;
            memcpy(p + 8, g_user_id, 16);
            preselected_popped = true;
            ipc_return_object(resp, new_am_storage(p, sizeof p));
        } else {
            resp->result = AM_NO_DATA;
        }
        break;
    case 20: OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;  // EnsureSaveData
    case 21: {  // GetDesiredLanguage: the game's text language (settings: runtime/settings.c, "language")
        char lang[8] = {0};
        snprintf(lang, sizeof lang, "%s", settings_language());
        memcpy(resp->data, lang, 8);
        resp_size(resp, 8);
        break;
    }
    case 23: {  // GetDisplayVersion: the game's own version, as on the console. It was "1.0.0" for any game (Link's
                // Awakening's): code mods that pick their hooks by it read Pokémon Z-A 2.0.0 / 2.0.2 as 1.0.0 (#258)
        static const struct { uint8_t id[8]; const char *v; } za[] = {
            {{0x72, 0x22, 0xe1, 0x3e, 0xcf, 0x6a, 0xdb, 0x32}, "1.0.0"},
            {{0x8a, 0x1c, 0x86, 0xc4, 0x37, 0x39, 0x4b, 0x69}, "2.0.0"},
            {{0xb1, 0xf1, 0x2f, 0xd9, 0x19, 0xea, 0xe8, 0x6a}, "2.0.2"}};
        extern uint8_t g_main_build_id[8];
        const char *v = "1.0.0";
        for (size_t i = 0; i < sizeof za / sizeof za[0]; i++)
            if (!memcmp(g_main_build_id, za[i].id, 8)) v = za[i].v;
        memset(resp->data, 0, 0x10);
        memcpy(resp->data, v, strlen(v) + 1);
        resp_size(resp, 0x10);
        break;
    }
    case 40: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // NotifyRunning
    case 130: case 140: resp->copy_handles[resp->ncopy++] = new_event(); break;
    default: UNHANDLED("IApplicationFunctions");
    }
}

static void application_proxy(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    static const struct { int cmd; const char *name; ServiceHandler h; } subs[] = {
        {0, "ICommonStateGetter", common_state_getter},
        {1, "ISelfController", self_controller},
        {2, "IWindowController", window_controller},
        {3, "IAudioController", generic_ok},
        {4, "IDisplayController", generic_ok},
        {10, "IProcessWindingController", generic_ok},
        {11, "ILibraryAppletCreator", library_applet_creator},
        {20, "IApplicationFunctions", application_functions},
        {1000, "IDebugFunctions", generic_ok},
    };
    for (unsigned i = 0; i < sizeof subs / sizeof *subs; i++) {
        if (subs[i].cmd == (int)req->cmd) {
            ipc_return_object(resp, service_new(subs[i].name, subs[i].h, NULL));
            return;
        }
    }
    UNHANDLED("IApplicationProxy");
}

SERVICE("appletOE", applet_oe) {
    (void)s;
    am_init_once();
    if (req->cmd == 0) ipc_return_object(resp, service_new("IApplicationProxy", application_proxy, NULL));
    else UNHANDLED("appletOE");
}

// ------------------------------------------------------------------ apm
static void apm_session(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 1) { OUT(uint32_t, 0) = 0x92220008; resp_size(resp, 4); }  // GetPerformanceConfiguration
}

SERVICE("apm", apm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("apm:ISession", apm_session, NULL));
    else if (req->cmd == 1) { OUT(uint32_t, 0) = 1; resp_size(resp, 4); }
}

// ------------------------------------------------------------------ lm (guest log output)
static void lm_logger(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s; (void)resp;
    if (req->cmd != 0) return;
    IpcBuf *b = in_buf(req, 0);
    if (!b || !b->size) return;
    // Log packets are TLV chunks after a 0x18-byte header; print the string chunks.
    const uint8_t *p = (const uint8_t *)(uintptr_t)b->addr, *end = p + b->size;
    p += 0x18;
    char line[1024];
    int n = 0;
    while (p + 2 <= end) {
        uint8_t key = p[0], len = p[1];
        p += 2;
        if (p + len > end) break;
        if (key == 2 || key == 4 || key == 5 || key == 6 || key == 7) {  // message, file, function, module, thread
            n += snprintf(line + n, sizeof line - n, "%s%.*s", n ? " | " : "", len, (const char *)p);
            if (n >= (int)sizeof line) break;
        }
        p += len;
    }
    if (n) LOG("[lm] %s\n", line);
    if (n && strstr(line, "OODLE ERROR")) {
        extern void fs_log_recent_reads(void);
        if (!rt_game_data_bad) fs_log_recent_reads();  // (once: which file, and are its bytes our copy's, #173)
        rt_game_data_bad = true;
    }  // (the crash dialog: the abort that follows isn't a timing race, #89)
}

SERVICE("lm", lm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("lm:ILogger", lm_logger, NULL));
}
