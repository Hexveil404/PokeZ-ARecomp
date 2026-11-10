// UDP for code mods: nn::socket's UDP calls on the PC's own sockets, and nn::nifm saying the network is there. Only for
// code mods' imports (loader.c resolve: net_mod_only) - the game itself keeps its SDK code on the console's services
// (nifm "no network"; bsd:u is runtime/services/net.c's local play, emulated sockets on the ldn network only), so
// turning a code mod on never sends the game to Nintendo's servers.
// Lumiose Online's relay traffic is what this is for (its beacon and play sessions: UDP only, IPv4). TCP, poll and the
// rest of nn::socket stay the SDK's (stubbed) code.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#include <pthread.h>
#include <string.h>

#include "../hle.h"

#define NET_FDS 64
#define MSG_DONTWAIT_NX 0x80  // nn::socket::MsgFlag::Msg_DontWait (BSD's)
static SOCKET fds[NET_FDS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool started;

// (names whose host versions only code mods get: loader.c)
bool net_mod_only(const char *name) { return !strncmp(name, "_ZN2nn6socket", 13) || !strncmp(name, "_ZN2nn4nifm", 11); }

static bool net_start(void) {  // (lock held)
    if (started) return true;
#ifdef _WIN32
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d)) return false;
#endif
    for (int i = 0; i < NET_FDS; i++) fds[i] = INVALID_SOCKET;
    started = true;
    return true;
}

static SOCKET fd_sock(int fd) {
    pthread_mutex_lock(&lock);
    SOCKET s = started && fd >= 0 && fd < NET_FDS ? fds[fd] : INVALID_SOCKET;
    pthread_mutex_unlock(&lock);
    return s;
}

RT_ON_RESET(net_reset) {
    pthread_mutex_lock(&lock);
    if (started) {
        for (int i = 0; i < NET_FDS; i++)
            if (fds[i] != INVALID_SOCKET) closesocket(fds[i]);
#ifdef _WIN32
        WSACleanup();
#endif
        started = false;
    }
    pthread_mutex_unlock(&lock);
}

// the Switch's sockaddr_in: u8 length, u8 family (2), u16 port and u32 address in network order, 8 zeros
static bool sa_in(uint64_t guest, uint64_t size, struct sockaddr_in *out) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)guest;
    if (!guest || size < 8 || p[1] != 2) return false;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, p + 2, 2);
    memcpy(&out->sin_addr, p + 4, 4);
    return true;
}
static void sa_out(uint64_t guest, uint64_t size_at, const struct sockaddr_in *a) {
    if (!guest || !size_at || *(uint32_t *)(uintptr_t)size_at < 16) return;
    uint8_t *p = (uint8_t *)(uintptr_t)guest;
    memset(p, 0, 16);
    p[0] = 16, p[1] = 2;
    memcpy(p + 2, &a->sin_port, 2);
    memcpy(p + 4, &a->sin_addr, 4);
    *(uint32_t *)(uintptr_t)size_at = 16;
}

HLE_FUNC("_ZN2nn4nifm10InitializeEv", net_nifm_init) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn4nifm18IsNetworkAvailableEv", net_nifm_available) { c->x[0] = 1; }
HLE_FUNC("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", net_nifm_request) { (void)c; }

HLE_FUNC("_ZN2nn6socket10InitializeEPvmmi", net_init) {  // (pool, pool size, allocation size, concurrency)
    pthread_mutex_lock(&lock);
    bool ok = net_start();
    pthread_mutex_unlock(&lock);
    LOG("[net] nn::socket::Initialize for a code mod: %s\n", ok ? "host sockets" : "WSAStartup failed");
    c->x[0] = ok ? 0 : 0x10;  // (Result: any failure)
}
HLE_FUNC("_ZN2nn6socket8FinalizeEv", net_fini) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", net_socket) {
    int family = (int)c->x[0], type = (int)c->x[1], proto = (int)c->x[2], fd = -1;
    pthread_mutex_lock(&lock);
    if (net_start() && family == 2 && type == 2 && (proto == 0 || proto == 17)) {  // AF_INET, SOCK_DGRAM, UDP
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        for (int i = 0; s != INVALID_SOCKET && i < NET_FDS; i++)
            if (fds[i] == INVALID_SOCKET) { fds[i] = s, fd = i; break; }
        if (fd < 0 && s != INVALID_SOCKET) closesocket(s);
    }
    pthread_mutex_unlock(&lock);
    LOG("[net] socket(%d, %d, %d) -> %d%s\n", family, type, proto, fd, fd < 0 ? " (UDP over IPv4 only)" : "");
    c->x[0] = (uint32_t)fd;
}

HLE_FUNC("_ZN2nn6socket5CloseEi", net_close) {
    int fd = (int)c->x[0], r = -1;
    pthread_mutex_lock(&lock);
    if (started && fd >= 0 && fd < NET_FDS && fds[fd] != INVALID_SOCKET) r = closesocket(fds[fd]), fds[fd] = INVALID_SOCKET;
    pthread_mutex_unlock(&lock);
    c->x[0] = (uint32_t)r;
}

HLE_FUNC("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", net_sendto) {  // (fd, buf, len, flags, to, tolen)
    SOCKET s = fd_sock((int)c->x[0]);
    struct sockaddr_in to = {0};
    int r = -1;
    if (s != INVALID_SOCKET && c->x[2] <= 0x10000 && sa_in(c->x[4], c->x[5], &to))
        r = (int)sendto(s, (const char *)(uintptr_t)c->x[1], (int)c->x[2], 0, (struct sockaddr *)&to, sizeof to);
    static int nlog;
    if (nlog < 8 || r < 0) {
        const uint8_t *ip = (const uint8_t *)&to.sin_addr;
        if (nlog++ < 16) LOG("[net] sendto %u.%u.%u.%u:%u, %llu bytes -> %d\n", ip[0], ip[1], ip[2], ip[3], ntohs(to.sin_port),
                              (unsigned long long)c->x[2], r);
    }
    c->x[0] = (uint32_t)r;
}

// (fd, buf, len, flags, from, fromlen*): waits for a datagram unless Msg_DontWait, as the console's - in 100 ms slices,
// so a reboot (g_rt_unwind) still stops the thread
HLE_FUNC("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", net_recvfrom) {
    SOCKET s = fd_sock((int)c->x[0]);
    bool wait = !(c->x[3] & MSG_DONTWAIT_NX);
    int r = -1;
    while (s != INVALID_SOCKET && c->x[2] <= 0x10000) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = {0, wait ? 100000 : 0};
        int ready = select((int)s + 1, &rd, NULL, NULL, &tv);
        if (ready > 0) {
            struct sockaddr_in from;
            socklen_t n = sizeof from;
            r = (int)recvfrom(s, (char *)(uintptr_t)c->x[1], (int)c->x[2], 0, (struct sockaddr *)&from, &n);
            if (r >= 0) sa_out(c->x[4], c->x[5], &from);
            break;
        }
        if (ready < 0 || !wait || g_rt_unwind || (s = fd_sock((int)c->x[0])) == INVALID_SOCKET) break;
    }
    static int nlog;
    if (r >= 0 && nlog++ < 8) LOG("[net] recvfrom: %d bytes\n", r);
    c->x[0] = (uint32_t)r;
}
