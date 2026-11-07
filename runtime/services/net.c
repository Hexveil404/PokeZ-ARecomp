// Network services for local wireless play (ldn:u, bsd:u / bsd:s; nifm:u is misc.c's - local play never opens it): Link Play > Nearby Players between
// copies of the game on one PC or one LAN. Command ids follow the public interface layouts (checked against the
// sdk's proxy vtables).
//
// The "air" is a host UDP socket per running game (ports LDN_PORT .. +7, the first free one). A game hosting a
// network announces it (its NetworkInfo, with the game's advertise data: the link code) every 200 ms to the other
// ports on this PC and as a LAN broadcast; scans return what was heard in the last second. Joining is a CONNECT /
// ACCEPT exchange with the host, who then sends everyone the new node list. The game's own UDP traffic (Pia, port
// 12345) runs on emulated sockets with the network's addresses (10.114.0.<node + 1>): a station sends everything to
// the host, the host delivers it or passes it on - a star, as on the real local wireless.
// LARECOMP_LDN_PEERS=<ip[:port]>,...: more places to announce to (another PC, a VPN address).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET hsock_t;
#define BAD_HSOCK INVALID_SOCKET
#define hsock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int hsock_t;
#define BAD_HSOCK (-1)
#define hsock_close close
#endif
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../hle.h"
#undef IN  // (winsock's empty IN / OUT: ipc.h has its own)
#undef OUT
#include "../ipc.h"

static void hexdump(const char *tag, const void *p, uint64_t n) {
    const uint8_t *b = p;
    if (n > 0x200) n = 0x200;
    for (uint64_t i = 0; i < n; i += 32) {
        char line[3 * 32 + 1];
        int k = 0;
        for (uint64_t j = i; j < n && j < i + 32; j++) k += snprintf(line + k, sizeof line - k, "%02x ", b[j]);
        LOG("[%s]   %04llx: %s\n", tag, (unsigned long long)i, line);
    }
}

static void log_req(const char *tag, const char *what, IpcReq *req) {
    LOG("[%s] %s (cmd %u, in %u bytes, x%d a%d b%d c%d)\n", tag, what, req->cmd, req->data_size, req->nx, req->na,
        req->nb, req->nc);
    if (req->data_size) hexdump(tag, req->data, req->data_size);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t new_event(KEvent **ev) {
    uint32_t w, r;
    KEvent *e = kernel_create_event_pair(&w, &r);
    if (ev) *ev = e;
    return r;
}

static void ev_signal(KEvent *e) {
    if (!e) return;
    pthread_mutex_lock(&g_klock);
    kernel_signal(e);
    pthread_mutex_unlock(&g_klock);
}

// One lock for the ldn state, the emulated sockets and the transport; g_net_cond: a packet arrived, an eventfd was
// written, a join was answered (waiters re-check their own condition).
static pthread_mutex_t g_net = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_net_cond = PTHREAD_COND_INITIALIZER;

static void cond_wait_ms(int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&g_net_cond, &g_net, &ts);
}

// ================================================================== ldn state
enum { LDN_NONE, LDN_INITIALIZED, LDN_AP, LDN_AP_CREATED, LDN_STATION, LDN_STATION_CONNECTED, LDN_ERROR };
static const char *const ldn_state_name[] = {"none", "initialized", "access point", "access point created",
                                             "station", "station connected", "error"};

#define LDN_RESULT(desc) (0xCBu | ((uint32_t)(desc) << 9))  // module 203
#define LDN_BAD_STATE LDN_RESULT(32)
#define LDN_CONNECT_FAILED LDN_RESULT(64)

// NetworkInfo (0x480): NetworkId 0x20 | CommonNetworkInfo 0x30 | LdnNetworkInfo 0x430
enum {
    NI_INTENT = 0x00, NI_SESSION = 0x10, NI_BSSID = 0x20, NI_SSID = 0x26, NI_CHANNEL = 0x48, NI_LINK = 0x4A,
    NI_TYPE = 0x4B, NI_SECPARAM = 0x50, NI_SECMODE = 0x60, NI_ACCEPT = 0x62, NI_NODEMAX = 0x66, NI_NODECOUNT = 0x67,
    NI_NODES = 0x68, NI_ADVSIZE = 0x26A, NI_ADV = 0x26C, NI_SIZE = 0x480
};
// NodeInfo (0x40): ip u32, mac[6], node id i8, connected i8, user name[0x21], pad, local comm version i16, pad 0x10
#define NODE(info, i) ((info) + NI_NODES + 0x40 * (i))
#define VIP_BASE 0x0A720000u  // 10.114.0.0/24 (host byte order, as GetIpv4Address returns it)
#define VIP_BCAST (VIP_BASE | 0xFF)

typedef struct sockaddr_in HAddr;

static struct {
    int state;
    KEvent *ev;
    uint32_t ev_r;
    uint8_t info[NI_SIZE];  // the network: ours (access point) or the one joined (station)
    uint32_t ip;            // our address in it
    int node;               // our node id in it
    uint8_t node_update[8]; // per node: 1 connected, 2 disconnected since the last GetNetworkInfoLatestUpdate
    uint16_t disconnect_reason;
    HAddr members[8];       // host: each station's transport address (by node id)
    HAddr host;             // station: the host's
    int join_answer;        // station: 0 waiting, 1 accepted, -1 refused
} g_ldn;
static void socks_down(void);  // (below: a lost network's sockets)
static uint64_t g_host_heard;  // (station: when the host was last heard - its 200 ms announcements; tp_main's timeout)

static void ldn_set_state(int st) {  // (g_net held)
    if (g_ldn.state != st) LOG("[ldn] state %s -> %s\n", ldn_state_name[g_ldn.state], ldn_state_name[st]);
    g_ldn.state = st;
    ev_signal(g_ldn.ev);
}

static void node_fill(uint8_t *info, int id, const uint8_t *user_name, uint16_t version) {
    uint8_t *n = NODE(info, id);
    memset(n, 0, 0x40);
    *(uint32_t *)n = VIP_BASE | (uint32_t)(id + 1);
    const uint8_t mac[6] = {0x02, 0x5A, 0x41, 0x00, 0x00, (uint8_t)(id + 1)};
    memcpy(n + 4, mac, 6);
    n[10] = (uint8_t)id;
    n[11] = 1;
    memcpy(n + 12, user_name, 0x20);
    *(uint16_t *)(n + 0x30) = version;
}

static void info_recount(uint8_t *info) {
    int c = 0;
    for (int i = 0; i < 8; i++) c += NODE(info, i)[11] != 0;
    info[NI_NODECOUNT] = (uint8_t)c;
}

// ================================================================== transport
#define LDN_PORT 47810
#define TP_MAGIC 0x444C415Au  // "ZALD"
enum { TP_ADV = 1, TP_CONNECT, TP_ACCEPT, TP_REFUSE, TP_NETINFO, TP_LEAVE, TP_CLOSE, TP_DATA, TP_PROBE, TP_FRAG };

typedef struct {
    uint32_t magic, from;  // from: this game's random id (a game ignores its own broadcasts)
    uint8_t type, node;
    uint16_t len;
} TpHdr;

static hsock_t g_tp = BAD_HSOCK;
static uint16_t g_tp_port;
static uint32_t g_tp_id;
// (LARECOMP_BSD_TRACE) ldn + bsd calls traced since the last join (600 at most): the moments after a join are the ones
// that matter - the internet test spent the old limit on minutes of hosting / scanning before the players met
static int g_traced;
static uint64_t g_lost_at;  // (a station that lost its host: traced for 15 s more)
static HAddr g_targets[32];  // where announcements go
static int g_ntargets;

// Networks heard (scan results)
static struct { uint8_t info[NI_SIZE]; HAddr from; uint64_t at; } g_heard[8];

bool net_inet_strict(void);
bool net_inet_sign(const HAddr *to, const uint8_t *pkt, size_t n, uint8_t tag[8]);
bool net_inet_verify(const HAddr *from, const uint8_t *pkt, size_t n, const uint8_t tag[8]);
static bool same_addr(const HAddr *a, const HAddr *b) { return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port; }
static bool is_lan(const HAddr *a) {  // (this PC, or a private network: local play, not signed)
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 16) == 0xC0A8 || (ip >> 20) == 0xAC1 || ip == 0xFFFFFFFFu;
}
#define TP_TAG 8  // F1 > Online: every packet to / from the internet ends with its HMAC (net_inet.c), 8 bytes

// LARECOMP_INET_DEBUG=1: why internet packets are not sent / not taken (unsigned sends, the first packets from outside
// the LAN and what tp_recv made of them)
static int inet_dbg(void) { static int v = -1; if (v < 0) v = getenv("LARECOMP_INET_DEBUG") && atoi(getenv("LARECOMP_INET_DEBUG")); return v; }
// (test) a file "drop.flag" in the data folder cuts this copy off: nothing sent, everything received thrown away, the
// game left running - a network that drops mid-trade, to see what each side does with its save. Looked at every 200 ms
static bool tp_dropped(void) {
    static uint64_t next;
    static bool on;
    uint64_t now = now_ms();
    if (now < next) return on;
    next = now + 200;
    struct stat st;
    static int test = -1;  // (only with LARECOMP_TEST_DROP=1: a player's stray file must never cut the network)
    if (test < 0) test = getenv("LARECOMP_TEST_DROP") && atoi(getenv("LARECOMP_TEST_DROP"));
    bool d = test && stat("drop.flag", &st) == 0;
    if (d != on) LOG("[ldn] test: network %s (drop.flag)\n", d ? "CUT" : "back");
    return on = d;
}
static void tp_send(const HAddr *to, int type, int node, const void *body, size_t len) {
    uint8_t buf[sizeof(TpHdr) + 0x600 + TP_TAG];
    if (g_tp == BAD_HSOCK || len > 0x600 || tp_dropped()) return;
    TpHdr h = {TP_MAGIC, g_tp_id, (uint8_t)type, (uint8_t)node, (uint16_t)len};
    memcpy(buf, &h, sizeof h);
    if (len) memcpy(buf + sizeof h, body, len);
    size_t total = sizeof h + len;
    if (net_inet_strict() && !is_lan(to)) {  // (strict: signed for that friend, or not sent at all)
        if (!net_inet_sign(to, buf, total, buf + total)) {
            static int nlog;
            if (inet_dbg() && nlog++ < 20) LOG("[inet] not sent (no friend's key for %s:%u), type %d\n", inet_ntoa(to->sin_addr), ntohs(to->sin_port), type);
            return;
        }
        total += TP_TAG;
    }
    int sent = (int)sendto(g_tp, (const char *)buf, (int)total, 0, (const struct sockaddr *)to, sizeof *to);
    if (inet_dbg() && !is_lan(to)) {
        static int nlog;
        if (nlog++ < 12) LOG("[inet] out: type %d, %d of %d bytes to %s:%u\n", type, sent, (int)total, inet_ntoa(to->sin_addr), ntohs(to->sin_port));
    }
    if (sent < 0) {
        static int nerr;
#ifdef _WIN32
        int e = WSAGetLastError();
#else
        int e = errno;
#endif
        if (nerr++ < 10) LOG("[inet] send to %s:%u failed: %d\n", inet_ntoa(to->sin_addr), ntohs(to->sin_port), e);
    }
}

// net_inet.c: players reached over the internet (connection codes), and its STUN packets on our socket
int net_inet_peers(HAddr *out, int max);
int net_inet_punch(HAddr *out, uint8_t *full, int max);
void net_inet_saw(const HAddr *from);
void net_inet_init(uint16_t port);
void net_inet_tick(void);
bool net_inet_packet(const uint8_t *b, size_t n, const HAddr *from);

void tp_raw_send(const HAddr *to, const void *buf, size_t n) {
    if (g_tp != BAD_HSOCK && !tp_dropped()) sendto(g_tp, (const char *)buf, (int)n, 0, (const struct sockaddr *)to, sizeof *to);
}

static void tp_send_all(int type, const void *body, size_t len) {  // local targets + internet players
    HAddr in[16];
    int n = net_inet_peers(in, 16);
    for (int i = 0; i < g_ntargets; i++) tp_send(&g_targets[i], type, 0, body, len);
    for (int i = 0; i < n; i++) tp_send(&in[i], type, 0, body, len);
}

static void tp_announce(void) { tp_send_all(TP_ADV, g_ldn.info, NI_SIZE); }

static void host_send_netinfo(void) {  // (g_net held) the node list to every station
    for (int i = 1; i < 8; i++)
        if (NODE(g_ldn.info, i)[11]) tp_send(&g_ldn.members[i], TP_NETINFO, i, g_ldn.info, NI_SIZE);
}

static void sock_deliver(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *data, size_t n);

// Game datagrams over the internet: with our headers a full-size one (Pia's) is over 1500 bytes, so IP fragments it
// and mobile carriers' NATs drop fragments (the internet test joined, then the session died). Bigger than FRAG_MAX:
// sent in pieces (TP_FRAG: message id u16, index u8, count u8, piece) and put together again on arrival.
#define FRAG_MAX 1100
static struct { uint64_t tx, rx, tx_max, rx_max, frag_tx, frag_rx; } g_dstat;

static void tp_send_data(const HAddr *to, const uint8_t *b, size_t n) {
    g_dstat.tx++;
    if (n > g_dstat.tx_max) g_dstat.tx_max = n;
    if (n <= FRAG_MAX) { tp_send(to, TP_DATA, 0, b, n); return; }
    static uint16_t msg_id;
    msg_id++;
    int count = (int)((n + FRAG_MAX - 1) / FRAG_MAX);
    for (int i = 0; i < count; i++) {
        uint8_t piece[4 + FRAG_MAX];
        size_t off = (size_t)i * FRAG_MAX, len = n - off < FRAG_MAX ? n - off : FRAG_MAX;
        memcpy(piece, &msg_id, 2);
        piece[2] = (uint8_t)i;
        piece[3] = (uint8_t)count;
        memcpy(piece + 4, b + off, len);
        tp_send(to, TP_FRAG, 0, piece, 4 + len);
        g_dstat.frag_tx++;
    }
}

static struct { HAddr from; uint16_t id; uint8_t count, got_mask; size_t len[8]; uint8_t data[8 * FRAG_MAX]; uint64_t at; } g_reasm[4];

// a piece -> the whole message (in *out, its size returned) once every piece is in, else 0
static size_t frag_add(const HAddr *from, const uint8_t *p, size_t n, const uint8_t **out) {
    if (n < 4) return 0;
    uint16_t id;
    memcpy(&id, p, 2);
    int idx = p[2], count = p[3];
    if (count < 1 || count > 8 || idx >= count || n - 4 > FRAG_MAX) return 0;
    int slot = -1, oldest = 0;
    for (int i = 0; i < 4; i++) {
        if (g_reasm[i].id == id && g_reasm[i].from.sin_port == from->sin_port &&
            g_reasm[i].from.sin_addr.s_addr == from->sin_addr.s_addr) slot = i;
        if (g_reasm[i].at < g_reasm[oldest].at) oldest = i;
    }
    if (slot < 0) {
        slot = oldest;
        memset(&g_reasm[slot], 0, sizeof g_reasm[slot]);
        g_reasm[slot].from = *from;
        g_reasm[slot].id = id;
        g_reasm[slot].count = (uint8_t)count;
    }
    if (count != g_reasm[slot].count || (idx < count - 1 && n - 4 != FRAG_MAX)) return 0;  // (pieces that don't fit together)
    g_reasm[slot].at = now_ms();
    memcpy(g_reasm[slot].data + (size_t)idx * FRAG_MAX, p + 4, n - 4);
    g_reasm[slot].len[idx] = n - 4;
    g_reasm[slot].got_mask |= (uint8_t)(1u << idx);
    g_dstat.frag_rx++;
    if (g_reasm[slot].got_mask != (uint8_t)((1u << count) - 1)) return 0;
    size_t total = (size_t)(count - 1) * FRAG_MAX + g_reasm[slot].len[count - 1];
    g_reasm[slot].got_mask = 0;
    g_reasm[slot].id = 0;
    *out = g_reasm[slot].data;
    return total;
}

// DATA body: src ip u32, src port u16, dst port u16, dst ip u32, payload
static void tp_route_data(const uint8_t *b, size_t n, int from_node) {  // (g_net held)
    if (n < 12) return;
    uint32_t src = *(const uint32_t *)b, dst = *(const uint32_t *)(b + 8);
    uint16_t sport = *(const uint16_t *)(b + 4), dport = *(const uint16_t *)(b + 6);
    bool bcast = dst == VIP_BCAST || dst == 0xFFFFFFFFu;
    if (bcast || dst == g_ldn.ip) sock_deliver(src, sport, dport, b + 12, n - 12);
    if (g_ldn.state != LDN_AP_CREATED) return;  // (a station only receives)
    for (int i = 1; i < 8; i++) {  // the host passes it on
        if (i == from_node || !NODE(g_ldn.info, i)[11]) continue;
        if (bcast || dst == (VIP_BASE | (uint32_t)(i + 1))) tp_send_data(&g_ldn.members[i], b, n);
    }
}

static void tp_recv(const uint8_t *buf, int n, const HAddr *from) {
    if (n < (int)sizeof(TpHdr)) return;
    TpHdr h;
    memcpy(&h, buf, sizeof h);
    if (inet_dbg() && !is_lan(from)) {
        static int nlog;
        if (nlog++ < 20) LOG("[inet] in: %d bytes from %s:%u, magic %s, from %s, len %u\n", n, inet_ntoa(from->sin_addr), ntohs(from->sin_port),
                             h.magic == TP_MAGIC ? "ok" : "bad", h.from == g_tp_id ? "ourselves" : "other", h.len);
    }
    if (h.magic != TP_MAGIC || h.from == g_tp_id || (int)(sizeof h + h.len) > n) return;
    if (net_inet_strict() && !is_lan(from)) {
        // F1 > Online: from the internet only a friend's packet, whole and signed with the pair's key - anything else
        // (strangers, scans, forged or cut packets) is dropped here, before any of it reaches ldn or the game
        if ((int)(sizeof h + h.len) + TP_TAG != n || !net_inet_verify(from, buf, (size_t)n - TP_TAG, buf + n - TP_TAG)) {
            static int nlog;
            if (nlog++ < 10) LOG("[inet] dropped an unsigned / unknown packet from %s:%u (%d bytes)\n", inet_ntoa(from->sin_addr),
                                 ntohs(from->sin_port), n);
            return;
        }
    } else net_inet_saw(from);  // (an internet player: the port its NAT really uses)
    const uint8_t *b = buf + sizeof h;
    pthread_mutex_lock(&g_net);
    if (g_ldn.state == LDN_STATION_CONNECTED && same_addr(from, &g_ldn.host)) g_host_heard = now_ms();
    switch (h.type) {
    case TP_ADV: {
        if (h.len != NI_SIZE) break;
        int slot = 0;
        for (int i = 0; i < 8; i++) {
            if (!memcmp(g_heard[i].info + NI_SESSION, b + NI_SESSION, 0x10)) { slot = i; break; }
            if (g_heard[i].at < g_heard[slot].at) slot = i;
        }
        if (!g_heard[slot].at || memcmp(g_heard[slot].info + NI_SESSION, b + NI_SESSION, 0x10))
            LOG("[ldn] heard a network from %s:%u (%u/%u players)\n", inet_ntoa(from->sin_addr), ntohs(from->sin_port),
                b[NI_NODECOUNT], b[NI_NODEMAX]);
        memcpy(g_heard[slot].info, b, NI_SIZE);
        g_heard[slot].from = *from;
        g_heard[slot].at = now_ms();
        break;
    }
    case TP_PROBE:  // a scan: our network, now
        if (g_ldn.state == LDN_AP_CREATED) tp_send(from, TP_ADV, 0, g_ldn.info, NI_SIZE);
        break;
    case TP_CONNECT: {  // body: session id 0x10, user name 0x20, local comm version u16
        if (h.len < 0x32 || g_ldn.state != LDN_AP_CREATED || memcmp(b, g_ldn.info + NI_SESSION, 0x10)) {
            tp_send(from, TP_REFUSE, 0, NULL, 0);
            break;
        }
        int id = -1;
        for (int i = 1; i < 8 && id < 0; i++)  // (the same game asking again: its old node)
            if (NODE(g_ldn.info, i)[11] && g_ldn.members[i].sin_port == from->sin_port &&
                g_ldn.members[i].sin_addr.s_addr == from->sin_addr.s_addr) id = i;
        for (int i = 1; i < g_ldn.info[NI_NODEMAX] && i < 8 && id < 0; i++)
            if (!NODE(g_ldn.info, i)[11]) id = i;
        if (id < 0 || g_ldn.info[NI_ACCEPT] == 1) {  // full, or accepting nobody
            LOG("[ldn] refused a player (%s)\n", id < 0 ? "network full" : "accept policy");
            tp_send(from, TP_REFUSE, 0, NULL, 0);
            break;
        }
        node_fill(g_ldn.info, id, b + 0x10, *(const uint16_t *)(b + 0x30));
        info_recount(g_ldn.info);
        g_ldn.members[id] = *from;
        g_ldn.node_update[id] |= 1;
        g_traced = 0;
        LOG("[ldn] player \"%.32s\" joined as node %d (%s:%u)\n", (const char *)(b + 0x10), id,
            inet_ntoa(from->sin_addr), ntohs(from->sin_port));
        tp_send(from, TP_ACCEPT, id, g_ldn.info, NI_SIZE);
        host_send_netinfo();
        ev_signal(g_ldn.ev);
        break;
    }
    case TP_ACCEPT: case TP_REFUSE:  // (only from the host we asked: ldn_connect noted it)
        if (g_ldn.state != LDN_STATION || g_ldn.join_answer || !same_addr(from, &g_ldn.host)) break;
        if (h.type == TP_ACCEPT && h.len == NI_SIZE) {
            memcpy(g_ldn.info, b, NI_SIZE);
            g_ldn.node = h.node;
            g_ldn.ip = VIP_BASE | (uint32_t)(h.node + 1);
            g_ldn.host = *from;
            g_ldn.join_answer = 1;
        } else {
            g_ldn.join_answer = -1;
        }
        pthread_cond_broadcast(&g_net_cond);
        break;
    case TP_NETINFO:  // (our host's only)
        if (g_ldn.state != LDN_STATION_CONNECTED || h.len != NI_SIZE || !same_addr(from, &g_ldn.host)) break;
        for (int i = 0; i < 8; i++) {
            uint8_t was = NODE(g_ldn.info, i)[11], is = NODE(b, i)[11];
            if (was != is) g_ldn.node_update[i] |= is ? 1 : 2;
        }
        memcpy(g_ldn.info, b, NI_SIZE);
        ev_signal(g_ldn.ev);
        break;
    case TP_LEAVE:  // (a station for itself only)
        if (g_ldn.state != LDN_AP_CREATED || h.node < 1 || h.node > 7 || !NODE(g_ldn.info, h.node)[11] ||
            !same_addr(from, &g_ldn.members[h.node])) break;
        LOG("[ldn] node %d left\n", h.node);
        memset(NODE(g_ldn.info, h.node), 0, 0x40);
        info_recount(g_ldn.info);
        g_ldn.node_update[h.node] |= 2;
        host_send_netinfo();
        ev_signal(g_ldn.ev);
        break;
    case TP_CLOSE:  // (our host's only)
        if (g_ldn.state != LDN_STATION_CONNECTED || !same_addr(from, &g_ldn.host)) break;
        LOG("[ldn] the host closed the network\n");
        for (int i = 0; i < 8; i++) if (NODE(g_ldn.info, i)[11]) g_ldn.node_update[i] |= 2;
        g_ldn.disconnect_reason = 3;  // destroyed by the host
        socks_down();
        ldn_set_state(LDN_STATION);
        break;
    case TP_DATA: case TP_FRAG:
        if (g_ldn.state == LDN_AP_CREATED || g_ldn.state == LDN_STATION_CONNECTED) {
            const uint8_t *msg = b;
            size_t len = h.len;
            if (h.type == TP_FRAG && !(len = frag_add(from, b, h.len, &msg))) break;
            g_dstat.rx++;
            if (len > g_dstat.rx_max) g_dstat.rx_max = len;
            int from_node = 0;
            for (int i = 1; i < 8; i++)
                if (NODE(g_ldn.info, i)[11] && same_addr(&g_ldn.members[i], from)) from_node = i;
            // a host takes data from its stations only, each with its own address as the sender; a station from its
            // host only (which also passes on the other stations')
            if (g_ldn.state == LDN_AP_CREATED && (!from_node || len < 12 ||
                                                  *(const uint32_t *)msg != (VIP_BASE | (uint32_t)(from_node + 1)))) break;
            if (g_ldn.state == LDN_STATION_CONNECTED && !same_addr(from, &g_ldn.host)) break;
            tp_route_data(msg, len, from_node);
        }
        break;
    }
    pthread_mutex_unlock(&g_net);
}

static void *tp_main(void *arg) {
    (void)arg;
    uint8_t buf[0x800];
    uint64_t last_adv = 0;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_tp, &rd);
        struct timeval tv = {0, 50000};
        if (select((int)g_tp + 1, &rd, NULL, NULL, &tv) > 0) {
            HAddr from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(g_tp, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n < 0) {  // (internet tests: Windows reports an ICMP "port unreachable" from an earlier send here)
                static int nerr;
#ifdef _WIN32
                int e = WSAGetLastError();
#else
                int e = errno;
#endif
                if (nerr++ < 10) LOG("[inet] receive error %d\n", e);
            }
            static uint64_t rate_t;
            static int rate_n;
            uint64_t tnow = now_ms();
            if (tnow - rate_t >= 1000) rate_t = tnow, rate_n = 0;
            if (n > 0 && ++rate_n > 4000) {
                if (rate_n == 4001) LOG("[inet] over 4000 packets this second: the rest dropped\n");
                n = 0;
            }
            if (inet_dbg() && n > 0 && !is_lan(&from) && buf[0] != 0x01) {  // (STUN answers would use up the lines)
                static int nlog;
                if (nlog++ < 12) LOG("[inet] raw in: %d bytes from %s:%u, first byte %02x\n", n, inet_ntoa(from.sin_addr), ntohs(from.sin_port), buf[0]);
            }
            if (n > 0 && tp_dropped()) n = 0;  // (test: drop.flag)
            if (n > 0 && !net_inet_packet(buf, (size_t)n, &from)) tp_recv(buf, n, &from);
        }
        uint64_t t = now_ms();
        net_inet_tick();
        if (t - last_adv >= 200) {
            last_adv = t;
            pthread_mutex_lock(&g_net);
            if (g_ldn.state == LDN_AP_CREATED) tp_announce();
            // A station that hears nothing from its host (whose announcements come every 200 ms) for 5 s has lost
            // it - a network gone mid-trade. The Switch's ldn tells the game so ("signal lost"); without it the
            // station's game waited forever for a state change, frozen (drop.flag test). LARECOMP_LDN_HOST_TIMEOUT=<ms>
            static int64_t host_timeout = -1;
            if (host_timeout < 0) host_timeout = getenv("LARECOMP_LDN_HOST_TIMEOUT") ? atoll(getenv("LARECOMP_LDN_HOST_TIMEOUT")) : 5000;
            if (g_ldn.state == LDN_STATION_CONNECTED && host_timeout > 0 && t - g_host_heard > (uint64_t)host_timeout) {
                LOG("[ldn] nothing from the host for %llu ms: the network is lost\n", (unsigned long long)(t - g_host_heard));
                for (int i = 0; i < 8; i++) if (NODE(g_ldn.info, i)[11]) g_ldn.node_update[i] |= 2;
                g_ldn.disconnect_reason = 6;  // signal lost
                g_lost_at = t, g_traced = 0;
                socks_down();
                ldn_set_state(LDN_STATION);
            }
            static uint64_t last_stat, logged_tx, logged_rx;
            if (t - last_stat >= 2000 && (g_dstat.tx != logged_tx || g_dstat.rx != logged_rx)) {
                last_stat = t;
                logged_tx = g_dstat.tx;
                logged_rx = g_dstat.rx;
                LOG("[ldn] game data: sent %llu (largest %llu B, %llu pieces), got %llu (largest %llu B, %llu pieces)\n",
                    (unsigned long long)g_dstat.tx, (unsigned long long)g_dstat.tx_max, (unsigned long long)g_dstat.frag_tx,
                    (unsigned long long)g_dstat.rx, (unsigned long long)g_dstat.rx_max, (unsigned long long)g_dstat.frag_rx);
            }
            // internet players: a packet every second opens the NATs (hole punching: net_inet_punch says to whom, and
            // which go with a low TTL) and keeps them open. LARECOMP_INET_TTL=<n>: that low TTL
            static uint64_t last_punch;
            if (t - last_punch >= 1000) {
                last_punch = t;
                HAddr in[64];
                uint8_t full[64];
                int n = net_inet_punch(in, full, 64);
                static int low = -1, normal = 64;
                if (low < 0) {
                    const char *e = getenv("LARECOMP_INET_TTL");
                    low = e && atoi(e) > 0 ? atoi(e) : 6;  // (out of our own NATs, a phone's carrier too; short of the friend's)
                    socklen_t ol = sizeof normal;
                    getsockopt(g_tp, IPPROTO_IP, IP_TTL, (char *)&normal, &ol);
                }
                for (int i = 0; i < n; i++) {
                    if (!full[i]) setsockopt(g_tp, IPPROTO_IP, IP_TTL, (const char *)&low, sizeof low);
                    if (inet_dbg() && full[i]) LOG("[inet] full packet to %s:%u\n", inet_ntoa(in[i].sin_addr), ntohs(in[i].sin_port));
                    tp_send(&in[i], TP_PROBE, 0, NULL, 0);
                    if (!full[i]) setsockopt(g_tp, IPPROTO_IP, IP_TTL, (const char *)&normal, sizeof normal);
                }
                static int nlog;  // (internet tests: who is probed, every 10 s for a while)
                static uint64_t last_list;
                if (n && t - last_list >= 10000 && nlog++ < 12) {
                    last_list = t;
                    char line[1024];
                    int k = snprintf(line, sizeof line, "[inet] probing %d:", n);
                    for (int i = 0; i < n && k < (int)sizeof line - 32; i++)
                        k += snprintf(line + k, sizeof line - k, " %s:%u", inet_ntoa(in[i].sin_addr), ntohs(in[i].sin_port));
                    LOG("%s\n", line);
                }
            }
            pthread_mutex_unlock(&g_net);
        }
    }
    return NULL;
}

static void add_target(uint32_t ip_net, uint16_t port) {
    if (g_ntargets >= (int)(sizeof g_targets / sizeof *g_targets)) return;
    HAddr *a = &g_targets[g_ntargets++];
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = ip_net;
    a->sin_port = htons(port);
}

static bool g_online_port;  // F1 > Online opened the transport: a port the OS picks (see below)
static void tp_start(void) {
    if (g_tp != BAD_HSOCK) return;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    // (arc4random, not rand(): the CRT's rand() is per thread and never seeded, so two games made the same numbers -
    // the same transport id, and the same network session id below, which kept internet players from connecting)
    g_tp_id = arc4random();
    // LARECOMP_LDN_BIND=<local ip>: this connection only (tests: one game on the home network, one on a phone's 4G)
    const char *bind_ip = getenv("LARECOMP_LDN_BIND");
    // Internet only (no LAN announcements): a port the OS picks, new each run. The home router of the second internet
    // test kept the fixed port 47810 for one destination and gave another one 1030 (a stale mapping of that port from
    // earlier runs), which made it look symmetric and hole punching failed; fresh ports kept their number everywhere.
    const char *inet = getenv("LARECOMP_LDN_INET"), *lan = getenv("LARECOMP_LDN_LOCAL");
    bool any_port = g_online_port || (inet && atoi(inet) && lan && atoi(lan) == 0);
    for (int i = 0; i < 8; i++) {
        hsock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == BAD_HSOCK) break;
        HAddr a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = bind_ip && *bind_ip ? inet_addr(bind_ip) : htonl(INADDR_ANY);
        // (tests) LARECOMP_LDN_PORT=<n>: exactly that port (the other side is told it beforehand)
        const char *fixed = getenv("LARECOMP_LDN_PORT");
        a.sin_port = fixed && atoi(fixed) ? htons((uint16_t)(atoi(fixed) + i)) : any_port ? 0 : htons(LDN_PORT + i);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0) {
            int on = 1;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
            g_tp = s;
            socklen_t al = sizeof a;
            g_tp_port = getsockname(s, (struct sockaddr *)&a, &al) == 0 ? ntohs(a.sin_port) : (uint16_t)(LDN_PORT + i);
            break;
        }
        hsock_close(s);
    }
    if (g_tp == BAD_HSOCK) { LOG("[ldn] no free transport port (%d..%d): local play off\n", LDN_PORT, LDN_PORT + 7); return; }
    // LARECOMP_LDN_LOCAL=0: no announcements on this PC / LAN (an internet test must not find the other game here)
    const char *local = getenv("LARECOMP_LDN_LOCAL");
    for (int i = 0; i < 8 && !(local && atoi(local) == 0); i++) {
        if (LDN_PORT + i != g_tp_port) add_target(htonl(INADDR_LOOPBACK), (uint16_t)(LDN_PORT + i));
        add_target(htonl(INADDR_BROADCAST), (uint16_t)(LDN_PORT + i));
    }
    const char *peers = getenv("LARECOMP_LDN_PEERS");
    for (const char *p = peers; p && *p;) {
        char host[64] = {0};
        int k = 0;
        while (*p && *p != ',' && *p != ':' && k < 63) host[k++] = *p++;
        int port = LDN_PORT;
        if (*p == ':') port = (int)strtol(p + 1, (char **)&p, 10);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
        uint32_t ip = inet_addr(host);
        if (ip != INADDR_NONE) for (int i = 0; i < (port == LDN_PORT ? 8 : 1); i++) add_target(ip, (uint16_t)(port + i));
        if (ip != INADDR_NONE && port != LDN_PORT) {  // (internet tests: probed from the start)
            extern void net_inet_add(uint32_t ip_net, uint16_t port);
            net_inet_add(ip, (uint16_t)port);
        }
    }
    LOG("[ldn] local play transport on UDP %s:%u (%d announce targets)\n", bind_ip && *bind_ip ? bind_ip : "*", g_tp_port,
        g_ntargets);
    net_inet_init(g_tp_port);
    pthread_t t;
    pthread_create(&t, NULL, tp_main, NULL);
}

// ================================================================== ldn:u
static void ldn_create(IpcReq *req, bool priv) {  // (g_net held)
    // CreateNetwork(SecurityConfig 0x44, UserConfig 0x30, pad 4, NetworkConfig 0x20) (+ SecurityParameter, addresses)
    const uint8_t *d = req->data;
    uint8_t *ni = g_ldn.info;
    const uint8_t *netcfg = d + (priv ? 0x88 : 0x78);
    uint16_t keep_adv = *(uint16_t *)(ni + NI_ADVSIZE);
    uint8_t adv[0x180];
    memcpy(adv, ni + NI_ADV, sizeof adv);
    memset(ni, 0, NI_SIZE);
    memcpy(ni + NI_INTENT, netcfg, 0x10);
    for (int i = 0; i < 0x10; i++) ni[NI_SESSION + i] = (uint8_t)arc4random();  // (unique per game: see tp_start)
    for (int i = 0; i < 0x10; i++) ni[NI_SECPARAM + i] = (uint8_t)arc4random();
    const uint8_t mac[6] = {0x02, 0x5A, 0x41, 0x00, 0x00, 0x01};
    memcpy(ni + NI_BSSID, mac, 6);
    ni[NI_SSID] = 32;
    for (int i = 0; i < 32; i++) ni[NI_SSID + 1 + i] = "0123456789abcdef"[arc4random() & 15];
    *(uint16_t *)(ni + NI_CHANNEL) = *(const uint16_t *)(netcfg + 0x10) ? *(const uint16_t *)(netcfg + 0x10) : 6;
    ni[NI_LINK] = 3;
    ni[NI_TYPE] = 2;  // ldn
    *(uint16_t *)(ni + NI_SECMODE) = *(const uint16_t *)d;
    ni[NI_NODEMAX] = netcfg[0x12];
    *(uint16_t *)(ni + NI_ADVSIZE) = keep_adv;
    memcpy(ni + NI_ADV, adv, sizeof adv);
    node_fill(ni, 0, d + 0x44, *(const uint16_t *)(netcfg + 0x14));
    info_recount(ni);
    memset(g_ldn.node_update, 0, sizeof g_ldn.node_update);
    g_ldn.node_update[0] = 1;
    g_ldn.node = 0;
    g_ldn.ip = VIP_BASE | 1;
    LOG("[ldn] %s: up to %u players, link code \"%.8s\"\n", priv ? "CreateNetworkPrivate" : "CreateNetwork",
        ni[NI_NODEMAX], keep_adv >= 0x64 ? (const char *)ni + NI_ADV + 0x5c : "?");
    ldn_set_state(LDN_AP_CREATED);
    tp_announce();
}

static void ldn_leave(void) {  // (g_net held) a station leaves / the host closes its network
    if (g_ldn.state == LDN_STATION_CONNECTED) tp_send(&g_ldn.host, TP_LEAVE, g_ldn.node, NULL, 0);
    if (g_ldn.state == LDN_AP_CREATED)
        for (int i = 1; i < 8; i++) if (NODE(g_ldn.info, i)[11]) tp_send(&g_ldn.members[i], TP_CLOSE, i, NULL, 0);
}

static void ldn_connect(IpcReq *req, IpcResp *resp) {  // (g_net held)
    // Connect(SecurityConfig 0x44, UserConfig 0x30, local comm version u32, option u32) + NetworkInfo in a buffer
    IpcBuf *b = in_buf(req, 0);
    if (g_ldn.state != LDN_STATION || !b || b->size < NI_SIZE) { resp->result = LDN_BAD_STATE; return; }
    const uint8_t *want = (const uint8_t *)(uintptr_t)b->addr;
    int found = -1;
    for (int i = 0; i < 8; i++)
        if (g_heard[i].at && !memcmp(g_heard[i].info + NI_SESSION, want + NI_SESSION, 0x10)) found = i;
    if (found < 0) { LOG("[ldn] Connect: that network isn't around any more\n"); resp->result = LDN_CONNECT_FAILED; return; }
    uint8_t body[0x32] = {0};
    memcpy(body, want + NI_SESSION, 0x10);
    memcpy(body + 0x10, req->data + 0x44, 0x20);
    *(uint16_t *)(body + 0x30) = (uint16_t)(req->data_size >= 0x78 ? *(const uint32_t *)(req->data + 0x74) : 0);
    g_ldn.join_answer = 0;
    for (int tries = 0; tries < 4 && !g_ldn.join_answer; tries++) {
        g_ldn.host = g_heard[found].from;  // (its ACCEPT / NETINFO / CLOSE / data only: tp_recv)
        tp_send(&g_heard[found].from, TP_CONNECT, 0, body, sizeof body);
        uint64_t until = now_ms() + 500;
        while (!g_ldn.join_answer && now_ms() < until) cond_wait_ms(100);
    }
    if (g_ldn.join_answer != 1) {
        LOG("[ldn] Connect: %s\n", g_ldn.join_answer ? "refused by the host" : "no answer from the host");
        resp->result = LDN_CONNECT_FAILED;
        return;
    }
    memset(g_ldn.node_update, 0, sizeof g_ldn.node_update);
    for (int i = 0; i < 8; i++) if (NODE(g_ldn.info, i)[11]) g_ldn.node_update[i] = 1;
    g_ldn.disconnect_reason = 0;
    g_traced = 0;
    g_host_heard = now_ms();
    LOG("[ldn] joined as node %d (10.114.0.%d), %u players\n", g_ldn.node, g_ldn.node + 1, g_ldn.info[NI_NODECOUNT]);
    ldn_set_state(LDN_STATION_CONNECTED);
}

static void ldn_iuser(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    pthread_mutex_lock(&g_net);
    static int trace = -1;  // (LARECOMP_BSD_TRACE also lists the ldn calls while a network is up)
    if (trace < 0) trace = getenv("LARECOMP_BSD_TRACE") ? atoi(getenv("LARECOMP_BSD_TRACE")) : 0;
    if (trace && g_traced < 600 && (g_ldn.state == LDN_AP_CREATED || g_ldn.state == LDN_STATION_CONNECTED || now_ms() - g_lost_at < 15000)) {
        g_traced++;
        LOG("[ldn] trace cmd %u (state %s)\n", req->cmd, ldn_state_name[g_ldn.state]);
    }
    switch (req->cmd) {
    case 0:  // GetState
        OUT(uint32_t, 0) = (uint32_t)g_ldn.state;
        resp_size(resp, 4);
        break;
    case 1: case 101: {  // GetNetworkInfo / GetNetworkInfoLatestUpdate
        if (g_ldn.state != LDN_AP_CREATED && g_ldn.state != LDN_STATION_CONNECTED) { resp->result = LDN_BAD_STATE; break; }
        IpcBuf *b = out_buf(req, 0);
        if (b && b->addr) memcpy((void *)(uintptr_t)b->addr, g_ldn.info, b->size < NI_SIZE ? b->size : NI_SIZE);
        if (req->cmd == 101) {  // + NodeLatestUpdate[] {u8 state change; pad 7}, cleared by reading
            IpcBuf *u = out_buf(req, 1);
            if (u && u->addr) {
                uint8_t *o = (uint8_t *)(uintptr_t)u->addr;
                memset(o, 0, u->size);
                for (uint64_t i = 0; i < 8 && i * 8 < u->size; i++) o[i * 8] = g_ldn.node_update[i];
            }
            memset(g_ldn.node_update, 0, sizeof g_ldn.node_update);
        }
        break;
    }
    case 2:  // GetIpv4Address
        OUT(uint32_t, 0) = g_ldn.ip;
        OUT(uint32_t, 4) = 0xFFFFFF00u;
        resp_size(resp, 8);
        break;
    case 3:  // GetDisconnectReason
        OUT(uint16_t, 0) = g_ldn.disconnect_reason;
        resp_size(resp, 4);
        break;
    case 4:  // GetSecurityParameter (data 0x10 + session id 0x10)
        memcpy(&OUT(uint8_t, 0), g_ldn.info + NI_SECPARAM, 0x10);
        memcpy(&OUT(uint8_t, 0x10), g_ldn.info + NI_SESSION, 0x10);
        resp_size(resp, 0x20);
        break;
    case 5:  // GetNetworkConfig
        memcpy(&OUT(uint8_t, 0), g_ldn.info + NI_INTENT, 0x10);
        OUT(uint16_t, 0x10) = *(uint16_t *)(g_ldn.info + NI_CHANNEL);
        OUT(uint8_t, 0x12) = g_ldn.info[NI_NODEMAX];
        OUT(uint16_t, 0x14) = *(uint16_t *)(NODE(g_ldn.info, 0) + 0x30);
        resp_size(resp, 0x20);
        break;
    case 100:  // AttachStateChangeEvent
        // a new event each time: the game closes the handle it got when it finalizes ldn, and it alternates hosting and
        // scanning (Finalize / Initialize) until a partner shows up - with one event made at the first Initialize, a
        // host past its first round waited on a closed handle, and a player joining then never woke it (the internet
        // tests: joined, then "the host closed the network" after 3 s with no game data; the local tests joined in the
        // first round)
        g_ldn.ev_r = new_event(&g_ldn.ev);
        resp->copy_handles[resp->ncopy++] = g_ldn.ev_r;
        break;
    case 102: case 103: {  // Scan / ScanPrivate: the networks heard in the last second
        // A PROBE makes every host answer now: "Join a Room" scans once, right after Initialize, before any periodic
        // announcement could have arrived (it found no room).
        tp_send_all(TP_PROBE, NULL, 0);
        pthread_mutex_unlock(&g_net);
        struct timespec ts = {0, 150 * 1000000L};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&g_net);
        IpcBuf *b = out_buf(req, 0);
        int max = b && b->addr ? (int)(b->size / NI_SIZE) : 0, n = 0;
        uint64_t t = now_ms();
        for (int i = 0; i < 8 && n < max; i++) {
            if (!g_heard[i].at || t - g_heard[i].at > 1000) continue;
            memcpy((uint8_t *)(uintptr_t)b->addr + (size_t)n * NI_SIZE, g_heard[i].info, NI_SIZE);
            n++;
        }
        static int last_n = -1;
        if (n != last_n) LOG("[ldn] Scan: %d network(s)\n", n);
        last_n = n;
        OUT(uint16_t, 0) = (uint16_t)n;
        resp_size(resp, 4);
        break;
    }
    case 200:  // OpenAccessPoint
        if (g_ldn.state == LDN_AP_CREATED) ldn_leave();
        ldn_set_state(LDN_AP);
        break;
    case 201:  // CloseAccessPoint
        if (g_ldn.state == LDN_AP_CREATED) ldn_leave();
        ldn_set_state(LDN_INITIALIZED);
        break;
    case 202: case 203:
        if (g_ldn.state != LDN_AP) { resp->result = LDN_BAD_STATE; break; }
        ldn_create(req, req->cmd == 203);
        break;
    case 204:  // DestroyNetwork
        if (g_ldn.state == LDN_AP_CREATED) ldn_leave();
        ldn_set_state(LDN_AP);
        break;
    case 205:  // Reject(ip)
        LOG("[ldn] Reject %08x (ignored)\n", IN(uint32_t, 0));
        break;
    case 206: {  // SetAdvertiseData
        IpcBuf *b = in_buf(req, 0);
        uint64_t n = b ? b->size : 0;
        if (n > 0x180) n = 0x180;
        memset(g_ldn.info + NI_ADV, 0, 0x180);
        if (n) memcpy(g_ldn.info + NI_ADV, (void *)(uintptr_t)b->addr, n);
        *(uint16_t *)(g_ldn.info + NI_ADVSIZE) = (uint16_t)n;
        break;
    }
    case 207:  // SetStationAcceptPolicy
        g_ldn.info[NI_ACCEPT] = IN(uint8_t, 0);
        break;
    case 208: case 209:  // AddAcceptFilterEntry / ClearAcceptFilter
        break;
    case 300:  // OpenStation
        ldn_set_state(LDN_STATION);
        break;
    case 301:  // CloseStation
        if (g_ldn.state == LDN_STATION_CONNECTED) ldn_leave();
        ldn_set_state(LDN_INITIALIZED);
        break;
    case 302: case 303:  // Connect / ConnectPrivate
        ldn_connect(req, resp);
        break;
    case 304:  // Disconnect
        if (g_ldn.state == LDN_STATION_CONNECTED) ldn_leave();
        g_ldn.disconnect_reason = 1;  // by the user
        ldn_set_state(LDN_STATION);
        break;
    case 400: case 402:  // Initialize / Initialize2
        if (!g_ldn.ev) g_ldn.ev_r = new_event(&g_ldn.ev);
        tp_start();
        ldn_set_state(LDN_INITIALIZED);
        break;
    case 401:  // Finalize
        if (g_ldn.state == LDN_AP_CREATED || g_ldn.state == LDN_STATION_CONNECTED) ldn_leave();
        ldn_set_state(LDN_NONE);
        break;
    default:
        log_req("ldn", "unhandled (ok)", req);
    }
    pthread_mutex_unlock(&g_net);
}

static void ldn_monitor(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s; (void)req; (void)resp;  // IClientProcessMonitor: RegisterClient (nn::ldn::Initialize)
}

SERVICE("ldn:u", ldn_u) {
    (void)s;
    if (req->cmd == 0) {  // CreateUserLocalCommunicationService
        ipc_return_object(resp, service_new("ldn:IUserLocalCommunicationService", ldn_iuser, NULL));
        return;
    }
    if (req->cmd == 1) {  // CreateClientProcessMonitor (nn::ldn::Initialize registers the process on it: cmd 0)
        ipc_return_object(resp, service_new("ldn:IClientProcessMonitor", ldn_monitor, NULL));
        return;
    }
    LOG("[ldn] IUserServiceCreator cmd %u (ok)\n", req->cmd);
}

// ================================================================== bsd (emulated sockets)
// UDP sockets on the local network's addresses, and eventfds (Pia wakes its Poll with one). Nothing reaches the host
// network directly: datagrams go through the transport above.
#define BSD_EAGAIN 11
#define BSD_EBADF 9
#define BSD_ENETDOWN 100
#define POLLERR_ 0x8
#define BSD_MSG_DONTWAIT 0x80
#define BSD_O_NONBLOCK 0x800
#define POLLIN_ 0x1
#define POLLOUT_ 0x4
#define MAX_FDS 64
#define QUEUE_MAX 256

typedef struct Packet { struct Packet *next; uint32_t src_ip; uint16_t src_port; uint16_t n; uint8_t data[]; } Packet;
typedef struct {
    int kind;  // 0 free, 1 udp socket, 2 eventfd, 3 other socket (never receives)
    bool nonblock;
    uint16_t port;
    uint64_t count;  // eventfd
    Packet *head, *tail;
    int queued;
    bool down;  // (a station that lost its host: its network is gone, as on the Switch - socks_down)
} GSock;
static GSock g_fds[MAX_FDS];
static uint16_t g_next_port = 49152;

static int fd_new(int kind) {  // (g_net held)
    for (int i = 3; i < MAX_FDS; i++)
        if (!g_fds[i].kind) { memset(&g_fds[i], 0, sizeof g_fds[i]); g_fds[i].kind = kind; return i; }
    return -1;
}

static GSock *fd_get(int fd) { return fd >= 0 && fd < MAX_FDS && g_fds[fd].kind ? &g_fds[fd] : NULL; }

// A station that lost its host (closed, or silent: tp_main's timeout): on the Switch its network interface goes away,
// and the game's UDP sockets on it fail - that is how Pia learns the session is over and the game says so. Here they
// kept answering "nothing yet" (EAGAIN) for good, and the station's game waited forever, frozen (drop.flag test).
// Sockets made afterwards are fresh. (g_net held)
static void socks_down(void) {
    int n = 0;
    for (int i = 3; i < MAX_FDS; i++) if (g_fds[i].kind == 1 && !g_fds[i].down) g_fds[i].down = true, n++;
    if (n) LOG("[bsd] the network is gone: %d socket(s) fail from now on (ENETDOWN)\n", n);
    pthread_cond_broadcast(&g_net_cond);
}

static void fd_free(int fd) {
    GSock *g = fd_get(fd);
    if (!g) return;
    for (Packet *p = g->head, *n; p; p = n) { n = p->next; free(p); }
    memset(g, 0, sizeof *g);
}

static void sock_deliver(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *data, size_t n) {  // (g_net held)
    for (int i = 3; i < MAX_FDS; i++) {
        GSock *g = &g_fds[i];
        if (g->kind != 1 || g->port != dst_port || g->queued >= QUEUE_MAX) continue;
        Packet *p = n <= 0xFFFF ? malloc(sizeof *p + n) : NULL;
        if (!p) continue;
        p->next = NULL;
        p->src_ip = src_ip;
        p->src_port = src_port;
        p->n = (uint16_t)n;
        memcpy(p->data, data, n);
        if (g->tail) g->tail->next = p; else g->head = p;
        g->tail = p;
        g->queued++;
    }
    pthread_cond_broadcast(&g_net_cond);
}

static bool fd_readable(GSock *g) { return g->kind == 1 ? g->head != NULL : g->kind == 2 ? g->count != 0 : false; }

static void bsd_ret(IpcResp *resp, int32_t ret, uint32_t err) {
    OUT(int32_t, 0) = ret;
    OUT(uint32_t, 4) = err;
    resp_size(resp, 8);
}

// Switch sockaddr_in: len u8, family u8, port (network order), address (network order)
static void sa_write(IpcBuf *b, uint32_t ip, uint16_t port) {
    if (!b || !b->addr || b->size < 8) return;
    uint8_t *sa = (uint8_t *)(uintptr_t)b->addr;
    memset(sa, 0, b->size < 16 ? b->size : 16);
    sa[0] = 16; sa[1] = 2;
    sa[2] = (uint8_t)(port >> 8); sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)(ip >> 24); sa[5] = (uint8_t)(ip >> 16); sa[6] = (uint8_t)(ip >> 8); sa[7] = (uint8_t)ip;
}

// recv / recvfrom / read on fd: (g_net held) waits unless non-blocking
static int fd_take(int fd, int flags, uint8_t *out, size_t cap, uint32_t *src_ip, uint16_t *src_port, uint32_t *err) {
    GSock *g = fd_get(fd);
    if (!g) { *err = BSD_EBADF; return -1; }
    while (!fd_readable(g)) {
        if (g->kind == 1 && g->down) { *err = BSD_ENETDOWN; return -1; }
        if (g->nonblock || (flags & BSD_MSG_DONTWAIT)) { *err = BSD_EAGAIN; return -1; }
        cond_wait_ms(100);
        if (!(g = fd_get(fd))) { *err = BSD_EBADF; return -1; }
    }
    if (g->kind == 2) {
        if (cap >= 8) memcpy(out, &g->count, 8);
        g->count = 0;
        return 8;
    }
    Packet *p = g->head;
    g->head = p->next;
    if (!g->head) g->tail = NULL;
    g->queued--;
    size_t n = p->n < cap ? p->n : cap;
    memcpy(out, p->data, n);
    if (src_ip) *src_ip = p->src_ip;
    if (src_port) *src_port = p->src_port;
    free(p);
    return (int)n;
}

static int fd_send(int fd, const uint8_t *data, size_t n, uint32_t dst_ip, uint16_t dst_port, uint32_t *err) {  // (g_net held)
    GSock *g = fd_get(fd);
    if (!g) { *err = BSD_EBADF; return -1; }
    if (g->kind == 2) { uint64_t v = 0; memcpy(&v, data, n < 8 ? n : 8); g->count += v; pthread_cond_broadcast(&g_net_cond); return (int)n; }
    if (g->kind != 1) return (int)n;
    if (g->down) { *err = BSD_ENETDOWN; return -1; }
    if (!g->port) g->port = g_next_port++;
    if (n > 0x5C0) n = 0x5C0;
    uint8_t body[12 + 0x5C0];
    *(uint32_t *)body = g_ldn.ip;
    *(uint16_t *)(body + 4) = g->port;
    *(uint16_t *)(body + 6) = dst_port;
    *(uint32_t *)(body + 8) = dst_ip;
    memcpy(body + 12, data, n);
    bool bcast = dst_ip == VIP_BCAST || dst_ip == 0xFFFFFFFFu;
    if (bcast || dst_ip == g_ldn.ip) sock_deliver(g_ldn.ip, g->port, dst_port, data, n);  // (to ourselves, too)
    if (dst_ip != g_ldn.ip) {
        if (g_ldn.state == LDN_STATION_CONNECTED) tp_send_data(&g_ldn.host, body, 12 + n);
        else if (g_ldn.state == LDN_AP_CREATED) tp_route_data(body, 12 + n, 0);
    }
    return (int)n;
}

static uint32_t sa_ip(const uint8_t *sa) { return (uint32_t)sa[4] << 24 | sa[5] << 16 | sa[6] << 8 | sa[7]; }
static uint16_t sa_port(const uint8_t *sa) { return (uint16_t)(sa[2] << 8 | sa[3]); }

static void bsd_client(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    uint32_t err = 0;
    pthread_mutex_lock(&g_net);
    // (research aid) LARECOMP_BSD_TRACE=1: the game's socket calls once a network is up (first 600, Poll left out)
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_BSD_TRACE") ? atoi(getenv("LARECOMP_BSD_TRACE")) : 0;
    if (trace && req->cmd != 6 && g_traced < 600 && (g_ldn.state == LDN_AP_CREATED || g_ldn.state == LDN_STATION_CONNECTED || now_ms() - g_lost_at < 15000)) {
        g_traced++;
        IpcBuf *a1 = in_buf(req, 1);
        const uint8_t *sa = req->cmd == 11 && a1 && a1->addr && a1->size >= 8 ? (const uint8_t *)(uintptr_t)a1->addr : NULL;
        IpcBuf *a0 = in_buf(req, 0);
        LOG("[bsd] trace cmd %u fd %d in %u%s", req->cmd, req->data_size >= 4 ? IN(int32_t, 0) : -1,
            req->cmd == 11 && a0 ? (unsigned)a0->size : 0, sa ? " to " : "\n");
        if (sa) LOG("%u.%u.%u.%u:%u\n", sa[4], sa[5], sa[6], sa[7], (sa[2] << 8) | sa[3]);
    }
    switch (req->cmd) {
    case 0:  // RegisterClient
        OUT(uint64_t, 0) = 0;  // (nn::socket::Initialize fails on anything else)
        resp_size(resp, 8);
        break;
    case 1:  // StartMonitoring
        break;
    case 2: case 3: {  // Socket / SocketExempt(domain, type, protocol)
        int type = IN(int32_t, 4);
        int fd = fd_new(type == 2 ? 1 : 3);
        LOG("[bsd] Socket(%d, %d, %d) = %d\n", IN(int32_t, 0), type, IN(int32_t, 8), fd);
        bsd_ret(resp, fd, fd < 0 ? 24 : 0);
        break;
    }
    case 6: {  // Poll(nfds, timeout): pollfd {fd i32, events i16, revents i16}
        uint32_t nfds = IN(uint32_t, 0);
        int32_t timeout = IN(int32_t, 4);
        IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
        uint8_t pf[64 * 8];
        if (nfds > 64) nfds = 64;
        if (in && in->addr) memcpy(pf, (void *)(uintptr_t)in->addr, nfds * 8 < in->size ? nfds * 8 : in->size);
        uint64_t until = timeout < 0 ? UINT64_MAX : now_ms() + (uint64_t)timeout;
        int ready;
        for (;;) {
            ready = 0;
            for (uint32_t i = 0; i < nfds; i++) {
                int fd = *(int32_t *)(pf + i * 8);
                int16_t ev = *(int16_t *)(pf + i * 8 + 4), rev = 0;
                GSock *g = fd_get(fd);
                if (g && (ev & POLLIN_) && fd_readable(g)) rev |= POLLIN_;
                if (g && g->kind == 1 && g->down) rev |= POLLERR_;  // (its network is gone: socks_down)
                if (g && (ev & POLLOUT_) && g->kind != 2) rev |= POLLOUT_;
                if (g && (ev & POLLOUT_) && g->kind == 2) rev |= POLLOUT_;
                *(int16_t *)(pf + i * 8 + 6) = rev;
                ready += rev != 0;
            }
            uint64_t t = now_ms();
            if (ready || t >= until) break;
            cond_wait_ms((int)(until - t < 100 ? until - t : 100));
        }
        if (out && out->addr) memcpy((void *)(uintptr_t)out->addr, pf, nfds * 8 < out->size ? nfds * 8 : out->size);
        bsd_ret(resp, ready, 0);
        break;
    }
    case 8: case 9: case 25: {  // Recv(fd, flags) / RecvFrom(fd, flags) / Read(fd)
        int fd = IN(int32_t, 0), flags = req->cmd == 25 ? 0 : IN(int32_t, 4);
        IpcBuf *b = out_buf(req, 0);
        uint32_t ip = 0;
        uint16_t port = 0;
        uint8_t tmp[0x800];
        size_t cap = b && b->addr ? (b->size < sizeof tmp ? b->size : sizeof tmp) : 0;
        int n = fd_take(fd, flags, tmp, cap, &ip, &port, &err);
        if (n > 0) memcpy((void *)(uintptr_t)b->addr, tmp, (size_t)n);
        bsd_ret(resp, n, err);
        if (req->cmd == 9) {
            if (n >= 0) sa_write(out_buf(req, 1), ip, port);
            OUT(uint32_t, 8) = n >= 0 ? 16 : 0;
            resp_size(resp, 12);
        }
        break;
    }
    case 10: case 11: case 24: {  // Send(fd, flags) / SendTo(fd, flags) + address / Write(fd)
        IpcBuf *b = in_buf(req, 0);
        int fd = IN(int32_t, 0);
        uint32_t ip = 0;
        uint16_t port = 0;
        if (req->cmd == 11) {
            IpcBuf *to = in_buf(req, 1);
            if (to && to->addr && to->size >= 8) { ip = sa_ip((uint8_t *)(uintptr_t)to->addr); port = sa_port((uint8_t *)(uintptr_t)to->addr); }
        }
        int n = b && b->addr ? fd_send(fd, (uint8_t *)(uintptr_t)b->addr, b->size, ip, port, &err) : 0;
        bsd_ret(resp, n, err);
        break;
    }
    case 13: {  // Bind
        GSock *g = fd_get(IN(int32_t, 0));
        IpcBuf *b = in_buf(req, 0);
        if (g && b && b->addr && b->size >= 8) {
            uint16_t port = sa_port((uint8_t *)(uintptr_t)b->addr);
            g->port = port ? port : g_next_port++;
            LOG("[bsd] Bind fd %d -> port %u\n", IN(int32_t, 0), g->port);
        }
        bsd_ret(resp, g ? 0 : -1, g ? 0 : BSD_EBADF);
        break;
    }
    case 16: {  // GetSockName
        GSock *g = fd_get(IN(int32_t, 0));
        sa_write(out_buf(req, 0), g_ldn.ip, g ? g->port : 0);
        bsd_ret(resp, 0, 0);
        OUT(uint32_t, 8) = 16;
        resp_size(resp, 12);
        break;
    }
    case 17: {  // GetSockOpt -> zeros
        IpcBuf *b = out_buf(req, 0);
        if (b && b->addr) memset((void *)(uintptr_t)b->addr, 0, b->size);
        bsd_ret(resp, 0, 0);
        OUT(uint32_t, 8) = b ? (uint32_t)b->size : 0;
        resp_size(resp, 12);
        break;
    }
    case 20: {  // Fcntl(fd, cmd, arg): F_GETFL 3 / F_SETFL 4 (O_NONBLOCK)
        GSock *g = fd_get(IN(int32_t, 0));
        int cmd = IN(int32_t, 4), arg = IN(int32_t, 8);
        int ret = 0;
        if (g && cmd == 3) ret = g->nonblock ? BSD_O_NONBLOCK : 0;
        if (g && cmd == 4) g->nonblock = (arg & BSD_O_NONBLOCK) != 0;
        bsd_ret(resp, g ? ret : -1, g ? 0 : BSD_EBADF);
        break;
    }
    case 26:  // Close
        fd_free(IN(int32_t, 0));
        bsd_ret(resp, 0, 0);
        break;
    case 31: {  // EventFd(initval u64, flags)
        int fd = fd_new(2);
        if (fd >= 0) { g_fds[fd].count = IN(uint64_t, 0); g_fds[fd].nonblock = (IN(int32_t, 8) & BSD_O_NONBLOCK) != 0; }
        bsd_ret(resp, fd, fd < 0 ? 24 : 0);
        break;
    }
    case 21: case 22: case 19: case 23:  // SetSockOpt / Shutdown / Ioctl / ShutdownAllSockets
        bsd_ret(resp, 0, 0);
        break;
    default:
        log_req("bsd", "unhandled (ok)", req);
        bsd_ret(resp, 0, 0);
    }
    pthread_mutex_unlock(&g_net);
}

SERVICE("bsd:u", bsd_u) { bsd_client(s, req, resp); }
SERVICE("bsd:s", bsd_s) { bsd_client(s, req, resp); }

// (research aid) LARECOMP_THREAD_DUMP_AT=<s>[,<s>...]: the "[dump]" thread list (kernel_dump_threads) at those times
// after start - where the game waits when a screen hangs without the renderer noticing.
static void *dump_at_main(void *arg) {
    const char *p = arg;
    double last = 0;
    while (p && *p) {
        double at = strtod(p, (char **)&p);
        if (at > last) { struct timespec ts = {(time_t)(at - last), 0}; nanosleep(&ts, NULL); }
        last = at;
        kernel_dump_threads("[dump]");
        if (*p == ',') p++;
    }
    return NULL;
}
__attribute__((constructor)) static void dump_at_init(void) {
    const char *e = getenv("LARECOMP_THREAD_DUMP_AT");
    pthread_t t;
    if (e && *e) pthread_create(&t, NULL, dump_at_main, (void *)e);
}

// ------------------------------------------------------------------ F1 > Online (runtime/ui/menu.mm)
void net_inet_force(uint16_t port);
bool net_inet_code(char *out, size_t n);
int net_inet_add_code(const char *code);
int net_inet_status(char *out, size_t n);

// The page opened: the transport (if Link Play hasn't started it yet: on a port of its own) with internet play on
void net_online_open(void) {
    pthread_mutex_lock(&g_net);
    if (g_tp == BAD_HSOCK) g_online_port = true;
    tp_start();
    if (g_tp != BAD_HSOCK) net_inet_force(g_tp_port);
    pthread_mutex_unlock(&g_net);
}
bool net_online_code(char *out, size_t n) {
    pthread_mutex_lock(&g_net);
    bool ok = net_inet_code(out, n);
    pthread_mutex_unlock(&g_net);
    return ok;
}
int net_online_connect(const char *code) {
    pthread_mutex_lock(&g_net);
    int r = net_inet_add_code(code);
    pthread_mutex_unlock(&g_net);
    if (r > 0) LOG("[inet] F1 > Online: friend's code %s added\n", code);
    return r;
}
int net_online_status(char *out, size_t n) {
    pthread_mutex_lock(&g_net);
    int r = net_inet_status(out, n);
    pthread_mutex_unlock(&g_net);
    return r;
}
int net_online_nat(void) {  // net_inet_nat: 0 not known yet, 1 hole punching works, 2 symmetric
    extern int net_inet_nat(void);
    pthread_mutex_lock(&g_net);
    int r = net_inet_nat();
    pthread_mutex_unlock(&g_net);
    return r;
}

// (tests) LARECOMP_ONLINE_SHARE=<folder>, LARECOMP_ONLINE_ME=a|b: F1 > Online without the clicks - the same calls
// (open, our code, the friend's code), the codes passed through <folder>/<me>.txt instead of a chat
static void *online_share_main(void *arg) {
    (void)arg;
    const char *dir = getenv("LARECOMP_ONLINE_SHARE"), *me = getenv("LARECOMP_ONLINE_ME");
    if (!me) return NULL;
    char mine[1024], theirs[1024], code[48], other[64];
    snprintf(mine, sizeof mine, "%s/%s.txt", dir, me);
    snprintf(theirs, sizeof theirs, "%s/%s.txt", dir, strcmp(me, "a") ? "a" : "b");
    {   // (the game boots first) LARECOMP_ONLINE_SHARE_WAIT=<seconds>
        const char *w = getenv("LARECOMP_ONLINE_SHARE_WAIT");
        sleep(w ? (unsigned)atoi(w) : 45);
    }
    net_online_open();
    bool wrote = false, sent = false;
    for (int i = 0; i < 600 && !(sent && wrote); i++) {
        sleep(1);
        if (!wrote && net_online_code(code, sizeof code)) {
            FILE *f = fopen(mine, "wb");
            if (f) { fputs(code, f); fclose(f); wrote = true; LOG("[inet] test: our code %s written\n", code); }
        }
        FILE *f = fopen(theirs, "rb");
        // (LARECOMP_ONLINE_SHARE_DELAY=<seconds>: this player pastes that much later)
        const char *d = getenv("LARECOMP_ONLINE_SHARE_DELAY");
        if (!sent && i >= (d ? atoi(d) : 0) && f && fgets(other, sizeof other, f)) sent = net_online_connect(other) > 0;
        if (f) fclose(f);
    }
    return NULL;
}
__attribute__((constructor)) static void online_share_start(void) {
    if (!getenv("LARECOMP_ONLINE_SHARE")) return;
    pthread_t t;
    if (pthread_create(&t, NULL, online_share_main, NULL) == 0) pthread_detach(t);
}
