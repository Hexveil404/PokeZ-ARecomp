// Local play over the internet (runtime/services/net.c's transport): two players exchange connection codes (F1 >
// Online: their public address as STUN sees the transport socket, and a session token), then their games send each
// other signed packets until both NATs let them through (hole punching). Same UDP socket for everything, so what the
// STUN servers see is the NAT mapping the other player must reach. No server of ours and no rendezvous: players find
// each other only through the codes they give each other (the BitTorrent DHT lookup by link code was dropped - slow,
// and it handed out strangers' addresses). LARECOMP_LDN_INET=1: on without F1 (tests, with LARECOMP_LDN_PEERS).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hle.h"

typedef struct sockaddr_in HAddr;

// net.c
void tp_raw_send(const HAddr *to, const void *buf, size_t n);

static uint64_t ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static const char *astr(const HAddr *a) {  // (one caller at a time: the transport thread)
    static char s[2][32];
    static int k;
    k ^= 1;
    snprintf(s[k], sizeof s[k], "%s:%u", inet_ntoa(a->sin_addr), ntohs(a->sin_port));
    return s[k];
}

static bool same(const HAddr *a, const HAddr *b) { return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port; }

// ------------------------------------------------------------------ network time (SNTP)
// Hole punching sends at fixed times of the clock (net_inet_punch), so both PCs must agree on the time: with the
// Windows clock alone that held only within ~15 s, and a PC a minute off never connected. The time comes from public
// NTP servers instead (Cloudflare, Google, pool.ntp.org - not ours), asked through the transport socket like STUN:
// net time = monotonic + offset, from the first valid answer (its originate field must echo our request), asked again
// every 10 minutes. Until an answer comes (or with none: NTP blocked), the PC's own clock as before.
// LARECOMP_INET_CLOCK_SKEW=<s>: (test) our own clock that far off, to see the network time make up for it.
static HAddr g_ntp[3];
static int g_nntp;
static uint64_t g_ntp_sent, g_ntp_at, g_ntp_tx;  // (monotonic ms of the request; its transmit field)
static int64_t g_ntp_off = INT64_MIN;           // net unix ms - monotonic ms (INT64_MIN: none yet)
static int64_t g_ntp_skew_ms;

static int64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + g_ntp_skew_ms;
}

int64_t net_inet_time_ms(void) {  // unix ms: the network's when known, else this PC's
    int64_t off = __atomic_load_n(&g_ntp_off, __ATOMIC_RELAXED);
    return off != INT64_MIN ? (int64_t)ms() + off : wall_ms();
}
bool net_inet_time_synced(void) { return __atomic_load_n(&g_ntp_off, __ATOMIC_RELAXED) != INT64_MIN; }

static void ntp_send(void) {
    uint8_t m[48] = {0x23};  // (LI 0, version 4, mode 3: client)
    g_ntp_tx = ((uint64_t)rand() << 32) ^ (uint64_t)rand() << 16 ^ (uint64_t)rand();
    for (int i = 0; i < 8; i++) m[40 + i] = (uint8_t)(g_ntp_tx >> (56 - 8 * i));  // (echoed back as the originate field)
    g_ntp_sent = ms();
    for (int i = 0; i < g_nntp; i++) tp_raw_send(&g_ntp[i], m, sizeof m);
    g_ntp_at = g_ntp_sent;
}

static bool ntp_reply(const uint8_t *b, size_t n, const HAddr *from) {
    if (n < 48 || ntohs(from->sin_port) != 123 || (b[0] & 7) != 4) return false;
    uint64_t orig = 0, tx_s = 0, tx_f = 0;
    for (int i = 0; i < 8; i++) orig = orig << 8 | b[24 + i];
    for (int i = 0; i < 4; i++) tx_s = tx_s << 8 | b[40 + i], tx_f = tx_f << 8 | b[44 + i];
    if (orig != g_ntp_tx || !g_ntp_sent || !b[1] || b[1] > 15 || !tx_s) return true;  // (stale, kiss-of-death or unsynced: ours, ignored)
    uint64_t now = ms(), rtt = now - g_ntp_sent;
    if (rtt > 2000) return true;
    int64_t server_ms = (int64_t)(tx_s - 2208988800ull) * 1000 + (int64_t)((tx_f * 1000) >> 32);
    int64_t off = server_ms - (int64_t)(g_ntp_sent + rtt / 2);
    bool first = !net_inet_time_synced();
    __atomic_store_n(&g_ntp_off, off, __ATOMIC_RELAXED);
    g_ntp_sent = 0;  // (the first answer counts; the other servers' are dropped)
    if (first) LOG("[inet] network time from %s: this PC's clock is %+.1f s off (round trip %llu ms)\n", astr(from),
                   (double)(wall_ms() - net_inet_time_ms()) / 1000, (unsigned long long)rtt);
    return true;
}

// ------------------------------------------------------------------ SHA-1 (the pair's key, HMAC)
static void sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t *m = calloc(1, total);
    memcpy(m, msg, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[off + 4 * i] << 24 | m[off + 4 * i + 1] << 16 | m[off + 4 * i + 2] << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) { uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = x << 1 | x >> 31; }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d; d = c; c = b << 30 | b >> 2; b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

// ------------------------------------------------------------------ state
static bool g_on;
static HAddr g_public[2];  // as two STUN servers saw us
static HAddr g_mapped;     // the latest STUN answer: what our connection code says (F1 > Online)
static bool g_forced;      // F1 > Online turned internet play on (without LARECOMP_LDN_INET)
static HAddr g_code_peer;  // a friend's code pasted on the UI thread, for net_inet_tick (the peer list is its own)
static uint8_t g_code_key[20];  // ... and the key both games derive from the two codes
static uint8_t g_token[8];  // this session's secret, in our code: packets from a friend are signed with both
static int g_code_pending;
static int g_npublic;

#define MAX_PEERS 64
static struct {
    HAddr a;
    uint64_t found, last_rx;
    int64_t punch_slot;  // net_inet_punch's last
    uint8_t key[20];  // with a code
    bool keyed;
} g_peers[MAX_PEERS];
static int g_npeers;


static bool is_self(const HAddr *a) {
    for (int i = 0; i < g_npublic; i++) if (same(&g_public[i], a)) return true;
    return false;
}

// An address from a code (or LARECOMP_LDN_PEERS) is a candidate until one of our own packets comes from it.
static void peer_add(const HAddr *a, const char *how) {
    if (is_self(a)) return;
    for (int i = 0; i < g_npeers; i++) if (same(&g_peers[i].a, a)) return;
    uint64_t t = ms();
    if (g_npeers >= MAX_PEERS) {  // full: the oldest silent candidate goes
        int victim = -1;
        for (int i = 0; i < g_npeers; i++)
            if (!g_peers[i].last_rx && (victim < 0 || g_peers[i].found < g_peers[victim].found)) victim = i;
        if (victim < 0) return;
        g_peers[victim] = g_peers[--g_npeers];
    }
    g_peers[g_npeers].a = *a;
    g_peers[g_npeers].found = t;
    g_peers[g_npeers].last_rx = 0;
    g_peers[g_npeers].punch_slot = 0;
    g_peers[g_npeers].keyed = false;
    g_npeers++;
    static int logged;
    if (logged++ < 40) LOG("[inet] candidate %s (%s)\n", astr(a), how);
}

int net_inet_peers(HAddr *out, int max) {  // players (answered us)
    int n = 0;
    for (int i = 0; i < g_npeers && n < max; i++) if (g_peers[i].last_rx) out[n++] = g_peers[i].a;
    return n;
}

// Hole punching, called every second: who to send to, and whether with the full TTL (else a low one). Order decides it
// (all measured, home router <-> 5G): a packet reaching a router before the game behind it has sent to that address
// leaves a state there for 30 s after the last one, and if the game sends during those 30 s the pair stays blocked for
// over 3 minutes, even after a silence (so did every design that kept sending and hoped: only "never send then" works).
// One low-TTL packet opens our own NAT for about a minute and dies before the friend's router. So until a friend
// answers, both games send at fixed times of the clock: a low-TTL packet at :45 of every minute and a full one at :00.
// A full one reaching a friend who hasn't pasted yet is over by :30, before their first packet; one reaching a friend
// who has gets in (at most a minute and a bit after the second paste). Needs the two times within ~15 s: network time
// (SNTP, above), so the PCs' own clocks don't matter. A friend who
// answers gets full packets from then on.
int net_inet_punch(HAddr *out, uint8_t *full, int max) {
    uint64_t t = ms();
    for (int i = 0; i < g_npeers;)
        if (!g_peers[i].last_rx && t - g_peers[i].found > 15 * 60 * 1000) g_peers[i] = g_peers[--g_npeers];
        // (kept 15 min: hole punching needs both sides probing at the same time, and friends paste codes minutes apart)
        else i++;
    int n = 0;
    for (int i = 0; i < g_npeers && n < max; i++) {
        if (g_forced && !g_peers[i].keyed) continue;  // (strict: friends only)
        bool f = g_peers[i].last_rx && t - g_peers[i].last_rx < 15000;
        if (!f) {
            int64_t slot = net_inet_time_ms() / 5000;  // (5 s each, network time: once per slot, whatever the second we're called in)
            f = slot % 12 == 0 && g_peers[i].punch_slot == slot - 3;  // (never before our low one: two pasted just after :45)
            if (!f && (slot % 12 != 9 || slot == g_peers[i].punch_slot)) continue;
            g_peers[i].punch_slot = slot;
        }
        out[n] = g_peers[i].a;
        full[n++] = f;
    }
    return n;
}

void net_inet_add(uint32_t ip_net, uint16_t port) {  // (tests) LARECOMP_LDN_PEERS: a player known beforehand
    HAddr a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip_net;
    a.sin_port = htons(port);
    peer_add(&a, "given in LARECOMP_LDN_PEERS");
}

void net_inet_saw(const HAddr *from) {  // one of our packets came from there: a player (maybe a new NAT port)
    if (!g_on || g_forced) return;  // (F1 > Online: net_inet_verify does this, for signed packets from friends only)
    for (int i = 0; i < g_npeers; i++)
        if (same(&g_peers[i].a, from)) {
            if (!g_peers[i].last_rx) LOG("[inet] player at %s answers: connected through the internet\n", astr(from));
            g_peers[i].last_rx = ms();
            return;
        }
    uint32_t ip = ntohl(from->sin_addr.s_addr);
    if ((ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 16) == 0xC0A8 || (ip >> 20) == 0xAC1) return;  // (LAN: not ours)
    peer_add(from, "it reached us first");
    for (int i = 0; i < g_npeers; i++)
        if (same(&g_peers[i].a, from)) {
            g_peers[i].last_rx = ms();
            LOG("[inet] player at %s answers: connected through the internet\n", astr(from));
        }
}

// ------------------------------------------------------------------ STUN (our public address, and the NAT's kind)
static HAddr g_stun[2];
static int g_nstun;
static uint64_t g_stun_at;

static void stun_send(const HAddr *to) {
    uint8_t m[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    for (int i = 8; i < 20; i++) m[i] = (uint8_t)rand();
    tp_raw_send(to, m, sizeof m);
}

static void stun_reply(const uint8_t *b, size_t n, const HAddr *from) {
    for (size_t p = 20; p + 4 <= n;) {
        uint16_t t = (uint16_t)(b[p] << 8 | b[p + 1]), l = (uint16_t)(b[p + 2] << 8 | b[p + 3]);
        if ((t == 0x0020 || t == 0x0001) && l >= 8 && p + 4 + l <= n) {
            HAddr a = {0};
            a.sin_family = AF_INET;
            uint16_t port = (uint16_t)(b[p + 6] << 8 | b[p + 7]);
            uint32_t ip = (uint32_t)b[p + 8] << 24 | b[p + 9] << 16 | b[p + 10] << 8 | b[p + 11];
            if (t == 0x0020) { port ^= 0x2112; ip ^= 0x2112A442u; }
            a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(ip);
            if (!same(&g_mapped, &a)) { g_mapped = a; LOG("[inet] our connection code is for %s\n", astr(&a)); }
            for (int i = 0; i < g_npublic; i++) if (same(&g_public[i], &a)) return;
            if (g_npublic < 2) {
                g_public[g_npublic++] = a;
                LOG("[inet] STUN %s sees us as %s\n", astr(from), astr(&a));
                if (g_npublic == 2)
                    LOG("[inet] NAT: %s\n", g_public[0].sin_port == g_public[1].sin_port
                                               ? "same port for every destination (hole punching should work)"
                                               : "a new port per destination (symmetric: the other side must be reachable)");
            }
            return;
        }
        p += 4 + ((l + 3u) & ~3u);
    }
}

// ------------------------------------------------------------------ entry points (net.c's transport thread)
static int resolve(const char *host, uint16_t port, HAddr *out, int max) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return 0;
    int n = 0;
    for (struct addrinfo *r = res; r && n < max; r = r->ai_next) {
        out[n] = *(HAddr *)r->ai_addr;
        out[n].sin_port = htons(port);
        n++;
    }
    freeaddrinfo(res);
    return n;
}

void net_inet_init(uint16_t port) {
    const char *e = getenv("LARECOMP_LDN_INET");
    g_on = g_forced || (e && atoi(e) != 0);
    if (!g_on || g_nstun) return;  // (once: F1 > Online may turn it on after the transport started)
    g_nstun += resolve("stun.l.google.com", 19302, g_stun + g_nstun, 1);
    g_nstun += resolve("stun.cloudflare.com", 3478, g_stun + g_nstun, 1);  // (another operator: stun1.l.google.com was the same IP)
    LOG("[inet] internet play on: %d STUN servers\n", g_nstun);
    const char *sk = getenv("LARECOMP_INET_CLOCK_SKEW");
    if (sk) g_ntp_skew_ms = (int64_t)(atof(sk) * 1000);
    g_nntp += resolve("time.cloudflare.com", 123, g_ntp + g_nntp, 1);
    g_nntp += resolve("time.google.com", 123, g_ntp + g_nntp, 1);
    g_nntp += resolve("pool.ntp.org", 123, g_ntp + g_nntp, 1);
    if (g_nntp) ntp_send();
    else LOG("[inet] no NTP server found: punching by this PC's clock\n");
    for (int i = 0; i < g_nstun; i++) stun_send(&g_stun[i]);
    g_stun_at = ms();
}

bool net_inet_packet(const uint8_t *b, size_t n, const HAddr *from) {  // true: ours (STUN), not the game's
    if (!g_on) return false;
    if (n >= 20 && b[0] == 0x01 && b[1] == 0x01 && b[4] == 0x21 && b[5] == 0x12 && b[6] == 0xA4 && b[7] == 0x42) {
        stun_reply(b, n, from);
        return true;
    }
    if (g_nntp && ntp_reply(b, n, from)) return true;
    return false;
}

void net_inet_tick(void) {
    if (!g_on) return;
    uint64_t t = ms();
    if (g_npublic < 2 && t - g_stun_at > 3000 && t - g_stun_at < 20000) { for (int i = 0; i < g_nstun; i++) stun_send(&g_stun[i]); g_stun_at = t; }
    // every 25 s again: keeps the NAT mapping our connection code names alive until the friend connects, and notices
    // a new one (the router's mapping can expire after ~30-60 s of silence)
    if (g_forced && t - g_stun_at > 25000) { for (int i = 0; i < g_nstun; i++) stun_send(&g_stun[i]); g_stun_at = t; }
    // network time: again 3 s after an unanswered request (up to a minute), and every 10 minutes
    if (g_nntp && ((!net_inet_time_synced() && t - g_ntp_at > 3000 && t - g_ntp_at < 60000) || t - g_ntp_at > 600000)) {
        ntp_send();
    }
    {
        static bool said;
        if (!said && g_nntp && !net_inet_time_synced() && t - g_stun_at > 60000) {
            said = true;
            LOG("[inet] no network time (NTP blocked?): punching by this PC's clock - both PCs must show the right time\n");
        }
    }
    if (__atomic_load_n(&g_code_pending, __ATOMIC_ACQUIRE)) {  // (F1 > Online: a friend's code, tried anew)
        __atomic_store_n(&g_code_pending, 0, __ATOMIC_RELAXED);
        peer_add(&g_code_peer, "a connection code");
        for (int i = 0; i < g_npeers; i++)
            if (same(&g_peers[i].a, &g_code_peer)) { g_peers[i].found = t; memcpy(g_peers[i].key, g_code_key, 20); g_peers[i].keyed = true; }
    }
}

// ------------------------------------------------------------------ F1 > Online: connection codes
// A player's code is its public IPv4 address and port as STUN sees our transport socket (so the NAT's own port), plus a
// check byte: 7 bytes, 12 characters of Crockford base32 in groups of 4 (no I, L, O, U: read aloud or retyped without
// mix-ups). Both players paste each other's code: each side then probes the other, which opens both routers (a home
// router lets in only an address it has sent to) - connected in about a second, no server. Then Link Play >
// Nearby Players works as on one network, as often as wanted, while the game runs.
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static uint8_t code_check(const uint8_t *b, int n) { uint8_t c = 0x5A; for (int i = 0; i < n; i++) c = (uint8_t)(c * 31 + b[i]); return c; }

// HMAC-SHA1 (RFC 2104), cut to 8 bytes: a packet's signature. Without the two codes' tokens nobody can make one, so a
// stranger who learns an address (or scans for the port) gets nothing through to the game.
static void hmac_sha1(const uint8_t key[20], const uint8_t *msg, size_t n, uint8_t out[20]) {
    uint8_t pad[64 + 2048 + 32], inner[20];
    if (n > 2048 + 32) { memset(out, 0, 20); return; }
    memset(pad, 0x36, 64);
    for (int i = 0; i < 20; i++) pad[i] ^= key[i];
    memcpy(pad + 64, msg, n);
    sha1(pad, 64 + n, inner);
    memset(pad, 0x5c, 64);
    for (int i = 0; i < 20; i++) pad[i] ^= key[i];
    memcpy(pad + 64, inner, 20);
    sha1(pad, 84, out);
}

void net_inet_force(uint16_t port) {  // (net.c, transport running) internet play on for F1 > Online
    if (!g_forced) for (int i = 0; i < 8; i += 4) { uint32_t r = arc4random(); memcpy(g_token + i, &r, 4); }
    g_forced = true;
    net_inet_init(port);
}

// F1 > Online is strict: only players whose code was pasted, every packet signed
bool net_inet_strict(void) { return g_forced; }

static void b32_put(const uint8_t *b, int nbytes, char *c, int nchars) {  // big-endian bits, zero-padded at the end
    int bit = 0;
    for (int i = 0; i < nchars; i++) {
        int v = 0;
        for (int k = 0; k < 5; k++, bit++) v = v << 1 | (bit < nbytes * 8 ? b[bit / 8] >> (7 - bit % 8) & 1 : 0);
        c[i] = B32[v];
    }
}

bool net_inet_code(char *out, size_t n) {  // our code, once STUN answered: XXXX-XXXX-XXXX-XXXX-XXXX-XXXX
    if (!g_mapped.sin_port || n < 30) return false;
    uint8_t b[15];
    memcpy(b, &g_mapped.sin_addr.s_addr, 4);
    memcpy(b + 4, &g_mapped.sin_port, 2);
    memcpy(b + 6, g_token, 8);
    b[14] = code_check(b, 14);
    char c[24];
    b32_put(b, 15, c, 24);
    snprintf(out, n, "%.4s-%.4s-%.4s-%.4s-%.4s-%.4s", c, c + 4, c + 8, c + 12, c + 16, c + 20);
    return true;
}

int net_inet_add_code(const char *code) {  // 1 added, 0 not a code, -1 our own
    uint8_t b[15] = {0};
    int nc = 0;  // characters read (24 x 5 bits = the 15 bytes)
    for (const char *p = code; *p; p++) {
        char ch = (char)toupper((unsigned char)*p);
        if (ch == '-' || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        const char *f = ch ? strchr(B32, ch) : NULL;
        if (!f || nc >= 24) return 0;
        int v = (int)(f - B32);
        for (int j = 0; j < 5; j++) {
            int bit = nc * 5 + j;
            if (v >> (4 - j) & 1) b[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
        }
        nc++;
    }
    if (nc != 24 || b[14] != code_check(b, 14)) return 0;
    HAddr a = {0};
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr.s_addr, b, 4);
    memcpy(&a.sin_port, b + 4, 2);
    if (!memcmp(b + 6, g_token, 8)) return -1;
    // the pair's key: both games hash the same two tokens, in the same (sorted) order
    uint8_t m[18 + 16];
    memcpy(m, "ZA-Recomp online 1", 18);
    bool mine_first = memcmp(g_token, b + 6, 8) < 0;
    memcpy(m + 18, mine_first ? g_token : b + 6, 8);
    memcpy(m + 26, mine_first ? b + 6 : g_token, 8);
    sha1(m, sizeof m, g_code_key);
    g_code_peer = a;  // (the transport thread adds it: the peer list is its own)
    __atomic_store_n(&g_code_pending, 1, __ATOMIC_RELEASE);
    return 1;
}

// (net.c) the signature of an outgoing packet to a friend: false when the address isn't a friend's (no key)
bool net_inet_sign(const HAddr *to, const uint8_t *pkt, size_t n, uint8_t tag[8]) {
    for (int i = 0; i < g_npeers; i++)
        if (g_peers[i].keyed && same(&g_peers[i].a, to)) {
            uint8_t h[20];
            hmac_sha1(g_peers[i].key, pkt, n, h);
            memcpy(tag, h, 8);
            return true;
        }
    return false;
}

// (net.c) an incoming packet's signature checked against every friend's key (its NAT may use another port than its
// code says): true and the friend's address updated when one matches
bool net_inet_verify(const HAddr *from, const uint8_t *pkt, size_t n, const uint8_t tag[8]) {
    for (int i = 0; i < g_npeers; i++) {
        if (!g_peers[i].keyed || g_peers[i].a.sin_addr.s_addr != from->sin_addr.s_addr) continue;
        uint8_t h[20];
        hmac_sha1(g_peers[i].key, pkt, n, h);
        uint8_t d = 0;
        for (int k = 0; k < 8; k++) d |= (uint8_t)(h[k] ^ tag[k]);  // (constant time)
        if (d) continue;
        if (!same(&g_peers[i].a, from)) { LOG("[inet] friend %s now answers from port %u\n", astr(&g_peers[i].a), ntohs(from->sin_port)); g_peers[i].a = *from; }
        if (!g_peers[i].last_rx) LOG("[inet] player at %s answers: connected through the internet (signed)\n", astr(from));
        g_peers[i].last_rx = ms();
        return true;
    }
    return false;
}
// This PC's NAT as the two STUN servers saw the transport socket: 0 not known yet, 1 the same port for every destination
// (hole punching works), 2 a new port per destination ("symmetric": a friend can't reach the port our code names)
int net_inet_nat(void) {
    if (g_npublic < 2) return 0;
    return g_public[0].sin_port == g_public[1].sin_port ? 1 : 2;
}

int net_inet_status(char *out, size_t n) {  // players answering in the last 10 s; out: one line per code tried
    uint64_t t = ms();
    int live = 0;
    size_t k = 0;
    if (n) out[0] = 0;
    for (int i = 0; i < g_npeers; i++) {
        bool on = g_peers[i].last_rx && t - g_peers[i].last_rx < 10000;
        live += on;
        if (k + 64 < n) k += (size_t)snprintf(out + k, n - k, "%s %s\n", on ? "+" : "-", astr(&g_peers[i].a));
    }
    return live;
}
