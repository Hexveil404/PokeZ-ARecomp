// acc:u0 (one local user) and the preselected-user launch parameter.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

const uint8_t g_user_id[16] = {0x4c, 0x41, 0x52, 0x65, 0x63, 0x6f, 0x6d, 0x70, 0x01, 0, 0, 0, 0, 0, 0, 0x01};

static void write_uid_list(IpcReq *req) {
    IpcBuf *c = out_buf(req, 0);
    if (!c) return;
    memset((void *)(uintptr_t)c->addr, 0, c->size);
    if (c->size >= 16) memcpy((void *)(uintptr_t)c->addr, g_user_id, 16);
}

static void profile(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 1: {  // Get / GetBase -> ProfileBase {uid, u64 timestamp, nickname[0x21]}
        memset(resp->data, 0, 0x38);
        memcpy(resp->data, g_user_id, 16);
        *(uint64_t *)(resp->data + 16) = 1600000000;
        strcpy((char *)resp->data + 24, "Link");
        resp_size(resp, 0x38);
        if (req->cmd == 0 && out_buf(req, 0)) memset((void *)(uintptr_t)out_buf(req, 0)->addr, 0, out_buf(req, 0)->size);
        break;
    }
    case 10: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetImageSize
    case 11: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // LoadImage
    default: LOG("[acc] IProfile cmd %u (ok)\n", req->cmd);
    }
}

// Going online (only with Mystery Gift on: misc.c nifm_online): a Nintendo Account whose token is always ready. The
// async contexts are done at once, with success; the token is a placeholder (nothing here checks it - the game's own
// servers would, and the game never reaches them). Without Mystery Gift these stay as they were (no connection, so
// the game doesn't ask).
bool nifm_online(void);

static void async_context(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // GetSystemEvent: signalled (done)
        uint32_t w, r;
        KEvent *e = kernel_create_event_pair(&w, &r);
        pthread_mutex_lock(&g_klock);
        kernel_signal(e);
        pthread_mutex_unlock(&g_klock);
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 1: break;                                            // Cancel
    case 2: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;  // HasDone
    case 3: break;                                            // GetResult: success
    case 100: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetNetworkServiceLicenseKind: Common
    case 101: OUT(int64_t, 0) = 4102444800; resp_size(resp, 8); break;  // GetNetworkServiceLicenseExpiration (2100)
    default: LOG("[acc] IAsyncContext cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static const char k_id_token[] = "mystery-gift-offline";

static void manager(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: break;  // CheckAvailability
    case 1: OUT(uint64_t, 0) = 0x1234567890abcdefull; resp_size(resp, 8); break;  // GetAccountId
    case 2:    // EnsureIdTokenCacheAsync
    case 170:  // LoadNetworkServiceLicenseKindAsync
        if (!nifm_online()) goto other;
        LOG("[acc] IManagerForApplication cmd %u: done (Mystery Gift)\n", req->cmd);
        ipc_return_object(resp, service_new("IAsyncContext", async_context, NULL));
        break;
    case 3:    // LoadIdTokenCacheDeprecated
    case 4: {  // LoadIdTokenCache(out buffer) -> u32 size
        if (!nifm_online()) goto other;
        IpcBuf *b = out_buf(req, 0);
        uint32_t n = 0;
        if (b && b->size) {
            n = (uint32_t)(sizeof k_id_token - 1 < b->size ? sizeof k_id_token - 1 : b->size);
            memcpy((void *)(uintptr_t)b->addr, k_id_token, n);
        }
        OUT(uint32_t, 0) = n;
        resp_size(resp, 4);
        break;
    }
    other:
    default: LOG("[acc] IManagerForApplication cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("acc:u0", acc_u0) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(int32_t, 0) = 1; resp_size(resp, 4); break;  // GetUserCount
    case 1: OUT(uint8_t, 0) = !memcmp(req->data, g_user_id, 16); resp_size(resp, 1); break;
    case 2: case 3: case 141: write_uid_list(req); break;  // ListAllUsers / ListOpenUsers / ListQualifiedUsers
    case 4: case 51: memcpy(resp->data, g_user_id, 16); resp_size(resp, 16); break;  // GetLastOpenedUser
    case 5: ipc_return_object(resp, service_new("IProfile", profile, NULL)); break;
    case 50: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 100: case 140: break;  // InitializeApplicationInfo
    case 101: ipc_return_object(resp, service_new("IManagerForApplication", manager, NULL)); break;
    case 150: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsUserAccountSwitchLocked
    default: LOG("[acc] acc:u0 cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}
