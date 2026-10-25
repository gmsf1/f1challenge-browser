/* Winsock (wsock32) for the game's own network code, over a virtual LAN.
 *
 * Every player gets a virtual address 10.77.0.<id> from a small relay server (server/lan_relay.py); UDP datagrams
 * (broadcasts included) and TCP streams between players travel through it as binary WebSocket frames. The game
 * can't tell it from a LAN: its LAN game list broadcasts, its hosts listen, its clients connect.
 * Only with RC_LAN set (the page's ?lan=<relay url>): without it WSAStartup fails and the game runs offline.
 *
 * Frame (little endian): kind u8, peer u8 (destination to the relay, source from it; 0 = everyone else),
 * sport u16, dport u16, conn u32, payload. Kinds: 0 hello (peer = our id), 1 UDP, 2 SYN, 3 SYNACK, 4 RST, 5 DATA, 6 FIN. */
#include "win32.h"

#include <pthread.h>
#include <time.h>
#include <strings.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define W(name) static void w_##name(CPU *c)

enum { K_HELLO, K_UDP, K_SYN, K_SYNACK, K_RST, K_DATA, K_FIN };
enum { ST_FREE, ST_OPEN, ST_LISTEN, ST_CONNECTING, ST_CONNECTED };
#define SOCK_STREAM_ 1
#define SOCK_DGRAM_ 2
#define FD_READ 1
#define FD_WRITE 2
#define FD_ACCEPT 8
#define FD_CONNECT 16
#define FD_CLOSE 32
#define WSAEWOULDBLOCK 10035
#define WSAENOTSOCK 10038
#define WSAEMSGSIZE 10040
#define WSAEADDRINUSE 10048
#define WSAENOTCONN 10057
#define WSAECONNREFUSED 10061
#define WSAEINVAL 10022
#define WSAHOST_NOT_FOUND 11001
#define SOCKET_BASE 0x1000u
#define MAX_SOCKS 64

typedef struct Dgram { struct Dgram *next; uint32_t from_ip; uint16_t from_port; uint32_t len; uint8_t data[]; } Dgram;
typedef struct {
    int state, type;
    uint16_t port;               /* bound local port (host order) */
    uint32_t hwnd, msg, events;  /* WSAAsyncSelect */
    int nonblock;
    Dgram *dq, *dq_tail;         /* datagrams received */
    uint32_t peer_id, conn;      /* TCP: the other player and the connection id */
    uint16_t peer_port;
    uint8_t *rx; uint32_t rx_len, rx_cap;   /* TCP bytes received */
    int remote_closed;
    int connect_err;
    int accepting[8], naccept;   /* listener: pending accepted sockets (indices) */
} Sock;

static Sock g_socks[MAX_SOCKS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_enabled = -1;
static volatile uint32_t g_id = 1;          /* our player id, from the relay's hello */
/* Lowest free ephemeral port, so a scan socket reopened after the game closed the last one gets the same port and
 * still receives replies that were slow to come back through the relay. Caller holds g_lock. */
static uint16_t alloc_ephemeral(void) {
    for (uint16_t p = 49152;; p++) {
        int used = 0;
        for (int i = 0; i < MAX_SOCKS; i++) if (g_socks[i].state != ST_FREE && g_socks[i].port == p) used = 1;
        if (!used) return p;
    }
}
static uint32_t g_conn_counter;
static __thread int g_last_error;

static int lan_enabled(void) {
    if (g_enabled < 0) {
        g_enabled = getenv("RC_LAN") != NULL;
        if (getenv("RC_LAN_ID")) g_id = (uint32_t)atoi(getenv("RC_LAN_ID"));
    }
    return g_enabled;
}
static uint32_t ip_of(uint32_t id) { return 10u | 77u << 8 | (id & 255u) << 24; }   /* network byte order as a little-endian word */
static uint32_t id_of(uint32_t ip) { return ip >> 24; }
static Sock *sock_of(uint32_t h) {
    if (h < SOCKET_BASE || (h - SOCKET_BASE) % 4) return NULL;
    uint32_t i = (h - SOCKET_BASE) / 4;
    return i < MAX_SOCKS && g_socks[i].state != ST_FREE ? &g_socks[i] : NULL;
}
static uint32_t handle_of(Sock *s) { return SOCKET_BASE + 4u * (uint32_t)(s - g_socks); }

/* ------------------------------------------------------------------ transport */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE uint8_t *lan_alloc(int n) { return malloc((size_t)n); }
EMSCRIPTEN_KEEPALIVE void lan_free(uint8_t *p) { free(p); }
static void transport(const uint8_t *frame, uint32_t len) {
    uint8_t *copy = malloc(len);
    memcpy(copy, frame, len);
    MAIN_THREAD_ASYNC_EM_ASM({
        const f = new Uint8Array(wasmMemory.buffer, $0 >>> 0, $1).slice();   // >>> 0: pointers above 2 GB arrive negative
        Module._lan_free($0);
        if (Module.lanSend) Module.lanSend(f);
    }, copy, len);
}
#else
static void lan_rx_frame(const uint8_t *f, uint32_t len);
static void transport(const uint8_t *frame, uint32_t len) {   /* no relay natively: only what a player says to itself comes back */
    if (frame[1] == 0 || frame[1] == g_id) { uint8_t *c = malloc(len); memcpy(c, frame, len); c[1] = (uint8_t)g_id; lan_rx_frame(c, len); free(c); }
}
#endif

static void send_frame(int kind, uint32_t peer, uint16_t sport, uint16_t dport, uint32_t conn, const void *data, uint32_t n) {
    uint8_t *f = malloc(10 + n);
    f[0] = (uint8_t)kind; f[1] = (uint8_t)peer;
    f[2] = sport & 255; f[3] = sport >> 8; f[4] = dport & 255; f[5] = dport >> 8;
    f[6] = conn & 255; f[7] = conn >> 8 & 255; f[8] = conn >> 16 & 255; f[9] = conn >> 24;
    if (n) memcpy(f + 10, data, n);
    transport(f, 10 + n);
    free(f);
}

static void notify(Sock *s, uint32_t event, uint32_t err) {   /* WSAAsyncSelect message */
    if (s->hwnd && (s->events & event)) post_message(s->hwnd, s->msg, handle_of(s), err << 16 | event);
}
static void wake(void) {}

static Sock *alloc_sock(int type) {
    for (int i = 0; i < MAX_SOCKS; i++)
        if (g_socks[i].state == ST_FREE) { memset(&g_socks[i], 0, sizeof g_socks[i]); g_socks[i].state = ST_OPEN; g_socks[i].type = type; return &g_socks[i]; }
    return NULL;
}
static void free_sock(Sock *s) {
    for (Dgram *d = s->dq; d;) { Dgram *n = d->next; free(d); d = n; }
    free(s->rx);
    memset(s, 0, sizeof *s);
}

/* a frame from the relay (any thread) */
#ifdef __EMSCRIPTEN__
static void lan_rx_frame(const uint8_t *f, uint32_t len);
EMSCRIPTEN_KEEPALIVE void lan_rx(const uint8_t *f, int len) { lan_rx_frame(f, (uint32_t)len); }
#endif
static void lan_rx_frame(const uint8_t *f, uint32_t len) {
    if (len < 10) return;
    int kind = f[0];
    uint32_t src = f[1];
    uint16_t sport = (uint16_t)(f[2] | f[3] << 8), dport = (uint16_t)(f[4] | f[5] << 8);
    uint32_t conn = f[6] | f[7] << 8 | f[8] << 16 | (uint32_t)f[9] << 24;
    const uint8_t *pl = f + 10;
    uint32_t n = len - 10;
    pthread_mutex_lock(&g_lock);
    if (kind == K_HELLO) { g_id = src; LOG("[winsock] relay says we are player %u (10.77.0.%u)\n", src, src); }
    else if (kind == K_UDP) {
        for (int i = 0; i < MAX_SOCKS; i++) {
            Sock *s = &g_socks[i];
            if (s->state != ST_OPEN || s->type != SOCK_DGRAM_ || s->port != dport) continue;
            { static int logged; if (logged++ < 30) LOG("[winsock] udp in: from player %u port %u to our port %u (%u bytes)\n", src, sport, dport, n); }
            Dgram *d = malloc(sizeof *d + n);
            d->next = NULL; d->from_ip = ip_of(src); d->from_port = sport; d->len = n;
            memcpy(d->data, pl, n);
            if (s->dq_tail) s->dq_tail->next = d; else s->dq = d;
            s->dq_tail = d;
            notify(s, FD_READ, 0);
        }
        { static int missed; int any = 0; for (int i = 0; i < MAX_SOCKS; i++) if (g_socks[i].state == ST_OPEN && g_socks[i].type == SOCK_DGRAM_ && g_socks[i].port == dport) any = 1; if (!any && (dport >= 49152 || missed++ < 10)) LOG("[winsock] udp in for port %u: nobody is bound there\n", dport); }
    } else if (kind == K_SYN) {
        Sock *l = NULL;
        for (int i = 0; i < MAX_SOCKS; i++) if (g_socks[i].state == ST_LISTEN && g_socks[i].port == dport) l = &g_socks[i];
        Sock *a = l && l->naccept < 8 ? alloc_sock(SOCK_STREAM_) : NULL;
        if (!a) send_frame(K_RST, src, dport, sport, conn, NULL, 0);
        else {
            a->state = ST_CONNECTED; a->port = dport; a->peer_id = src; a->peer_port = sport; a->conn = conn;
            l->accepting[l->naccept++] = (int)(a - g_socks);
            send_frame(K_SYNACK, src, dport, sport, conn, NULL, 0);
            notify(l, FD_ACCEPT, 0);
        }
    } else {
        for (int i = 0; i < MAX_SOCKS; i++) {
            Sock *s = &g_socks[i];
            if (s->state == ST_FREE || s->type != SOCK_STREAM_ || s->conn != conn || s->peer_id != src) continue;
            if (kind == K_SYNACK && s->state == ST_CONNECTING) { s->state = ST_CONNECTED; notify(s, FD_CONNECT, 0); notify(s, FD_WRITE, 0); }
            else if (kind == K_RST && s->state == ST_CONNECTING) { s->state = ST_OPEN; s->connect_err = WSAECONNREFUSED; notify(s, FD_CONNECT, WSAECONNREFUSED); }
            else if (kind == K_DATA && s->state == ST_CONNECTED) {
                if (s->rx_len + n > s->rx_cap) { s->rx_cap = (s->rx_len + n) * 2; s->rx = realloc(s->rx, s->rx_cap); }
                memcpy(s->rx + s->rx_len, pl, n); s->rx_len += n;
                notify(s, FD_READ, 0);
            } else if (kind == K_FIN) { s->remote_closed = 1; notify(s, FD_READ, 0); notify(s, FD_CLOSE, 0); }
        }
    }
    pthread_mutex_unlock(&g_lock);
    wake();
}

/* ------------------------------------------------------------------ helpers */

static void rd_addr(uint32_t p, uint32_t *ip, uint16_t *port) {   /* sockaddr_in: family u16, port (big endian), addr (as is) */
    *port = (uint16_t)(R8(p + 2) << 8 | R8(p + 3));
    *ip = R32(p + 4);
}
static void wr_addr(uint32_t p, uint32_t ip, uint16_t port) {
    W16(p, 2); W8(p + 2, port >> 8); W8(p + 3, port & 255); W32(p + 4, ip); W32(p + 8, 0); W32(p + 12, 0);
}
static int err(int e) { g_last_error = e; return -1; }
static void nap(void) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
static int is_broadcast(uint32_t ip) { return ip == 0xFFFFFFFFu || (ip >> 24) == 255; }

/* ------------------------------------------------------------------ API */

W(ws_WSAStartup) {
    if (!lan_enabled()) RET(2, 10091 /* WSASYSNOTREADY: no network */);
    uint32_t d = ARG(1);
    W16(d, 0x0202); W16(d + 2, 0x0202);
    memset(HP(d + 4), 0, 256 + 1 + 128 + 1 + 4);
    memcpy(HP(d + 4), "Virtual LAN", 12);
    RET(2, 0);
}
W(ws_WSACleanup) { RET(0, 0); }
W(ws_WSAGetLastError) { RET(0, g_last_error); }

W(ws_socket) {
    int type = (int)ARG(1);
    pthread_mutex_lock(&g_lock);
    Sock *s = type == SOCK_STREAM_ || type == SOCK_DGRAM_ ? alloc_sock(type) : NULL;
    uint32_t h = s ? handle_of(s) : 0;
    pthread_mutex_unlock(&g_lock);
    if (!s) { g_last_error = WSAEINVAL; RET(3, 0xFFFFFFFFu); }
    LOG("[winsock] socket(%s) = %x\n", type == SOCK_STREAM_ ? "tcp" : "udp", h);
    RET(3, h);
}
W(ws_closesocket) {
    { Sock *cs = sock_of(ARG(0)); LOG("[winsock] closesocket(%x) port %u, %d datagram(s) unread\n", ARG(0), cs ? cs->port : 0, cs && cs->dq ? 1 : 0); }
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    if (s && s->state == ST_CONNECTED) send_frame(K_FIN, s->peer_id, s->port, s->peer_port, s->conn, NULL, 0);
    if (s) free_sock(s);
    pthread_mutex_unlock(&g_lock);
    RET(1, s ? 0 : err(WSAENOTSOCK));
}
W(ws_shutdown) {
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    if (s && s->state == ST_CONNECTED && ARG(1) != 0) send_frame(K_FIN, s->peer_id, s->port, s->peer_port, s->conn, NULL, 0);
    pthread_mutex_unlock(&g_lock);
    RET(2, s ? 0 : err(WSAENOTSOCK));
}
W(ws_setsockopt) { LOG("[winsock] setsockopt(%x, level %u, opt %u)\n", ARG(0), ARG(1), ARG(2)); RET(5, 0); }
W(ws_bind) {
    uint32_t ip; uint16_t port;
    rd_addr(ARG(1), &ip, &port);
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    int rc = 0;
    if (!s) rc = err(WSAENOTSOCK);
    else {
        if (!port) port = alloc_ephemeral();
        for (int i = 0; i < MAX_SOCKS; i++)   /* ports are per protocol, and a second bind to one is refused */
            if (&g_socks[i] != s && g_socks[i].state != ST_FREE && g_socks[i].type == s->type && g_socks[i].port == port && s->type == SOCK_STREAM_) rc = err(WSAEADDRINUSE);
        if (!rc) s->port = port;
    }
    pthread_mutex_unlock(&g_lock);
    LOG("[winsock] bind(%x, port %u) = %d\n", ARG(0), port, rc);
    RET(3, rc);
}
W(ws_getsockname) {
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    int rc = 0;
    if (!s) rc = err(WSAENOTSOCK);
    else { if (!s->port) s->port = alloc_ephemeral(); wr_addr(ARG(1), ip_of(g_id), s->port); if (ARG(2)) W32(ARG(2), 16); }
    pthread_mutex_unlock(&g_lock);
    RET(3, rc);
}
W(ws_getpeername) {
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    int rc = 0;
    if (!s) rc = err(WSAENOTSOCK);
    else if (s->state != ST_CONNECTED) rc = err(WSAENOTCONN);
    else { wr_addr(ARG(1), ip_of(s->peer_id), s->peer_port); if (ARG(2)) W32(ARG(2), 16); }
    pthread_mutex_unlock(&g_lock);
    RET(3, rc);
}

W(ws_sendto) {
    uint32_t ip; uint16_t port;
    rd_addr(ARG(4), &ip, &port);
    uint32_t n = ARG(2);
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    if (!s) { pthread_mutex_unlock(&g_lock); RET(6, err(WSAENOTSOCK)); }
    if (!s->port) s->port = alloc_ephemeral();
    uint16_t sport = s->port;
    pthread_mutex_unlock(&g_lock);
    uint32_t peer = is_broadcast(ip) ? 0 : id_of(ip);
    if ((ip & 255) == 127) peer = g_id;
    LOG("[winsock] sendto(%x, %u bytes -> %u.%u.%u.%u:%u)\n", ARG(0), n, ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24, port);
    send_frame(K_UDP, peer, sport, port, 0, HP(ARG(1)), n);
    RET(6, n);
}
W(ws_recvfrom) {
    uint32_t cap = ARG(2);
    for (;;) {
        pthread_mutex_lock(&g_lock);
        Sock *s = sock_of(ARG(0));
        if (!s) { pthread_mutex_unlock(&g_lock); RET(6, err(WSAENOTSOCK)); }
        Dgram *d = s->dq;
        if (d) {
            s->dq = d->next; if (!s->dq) s->dq_tail = NULL;
            uint32_t n = d->len < cap ? d->len : cap;
            memcpy(HP(ARG(1)), d->data, n);
            if (ARG(4)) { wr_addr(ARG(4), d->from_ip, d->from_port); if (ARG(5)) W32(ARG(5), 16); }
            { static int logged; if (logged++ < 30) LOG("[winsock] recvfrom(%x) = %u bytes from 10.77.0.%u:%u\n", ARG(0), n, d->from_ip >> 24, d->from_port); }
            int trunc = d->len > cap;
            free(d);
            pthread_mutex_unlock(&g_lock);
            RET(6, trunc ? err(WSAEMSGSIZE) : n);
        }
        int nb = s->nonblock || s->hwnd;
        pthread_mutex_unlock(&g_lock);
        if (nb) RET(6, err(WSAEWOULDBLOCK));
        nap();
    }
}

W(ws_listen) {
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    int rc = 0;
    if (!s || s->type != SOCK_STREAM_) rc = err(WSAENOTSOCK);
    else { s->state = ST_LISTEN; LOG("[winsock] listen(port %u)\n", s->port); }
    pthread_mutex_unlock(&g_lock);
    RET(2, rc);
}
W(ws_accept) {
    for (;;) {
        pthread_mutex_lock(&g_lock);
        Sock *l = sock_of(ARG(0));
        if (!l) { pthread_mutex_unlock(&g_lock); RET(3, err(WSAENOTSOCK)); }
        if (l->naccept) {
            Sock *a = &g_socks[l->accepting[0]];
            memmove(l->accepting, l->accepting + 1, sizeof(int) * (size_t)(--l->naccept));
            a->hwnd = l->hwnd; a->msg = l->msg; a->events = l->events;   /* accepted sockets inherit the notifications */
            if (ARG(1)) { wr_addr(ARG(1), ip_of(a->peer_id), a->peer_port); if (ARG(2)) W32(ARG(2), 16); }
            uint32_t h = handle_of(a);
            pthread_mutex_unlock(&g_lock);
            LOG("[winsock] accept -> %x from player %u\n", h, a->peer_id);
            RET(3, h);
        }
        int nb = l->nonblock || l->hwnd;
        pthread_mutex_unlock(&g_lock);
        if (nb) RET(3, err(WSAEWOULDBLOCK));
        nap();
    }
}
W(ws_connect) {
    uint32_t ip; uint16_t port;
    rd_addr(ARG(1), &ip, &port);
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    if (!s) { pthread_mutex_unlock(&g_lock); RET(3, err(WSAENOTSOCK)); }
    if (s->type == SOCK_DGRAM_) { pthread_mutex_unlock(&g_lock); RET(3, 0); }
    if (!s->port) s->port = alloc_ephemeral();
    s->peer_id = id_of(ip); s->peer_port = port;
    s->conn = (g_id << 24) | (++g_conn_counter & 0xFFFFFF);
    s->state = ST_CONNECTING; s->connect_err = 0;
    uint16_t sport = s->port; uint32_t peer = s->peer_id, conn = s->conn;
    int nb = s->nonblock || s->hwnd;
    pthread_mutex_unlock(&g_lock);
    LOG("[winsock] connect(%x -> 10.77.0.%u:%u)\n", ARG(0), peer, port);
    send_frame(K_SYN, peer, sport, port, conn, NULL, 0);
    if (nb) RET(3, err(WSAEWOULDBLOCK));
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&g_lock);
        s = sock_of(ARG(0));
        int st = s ? s->state : ST_FREE, ce = s ? s->connect_err : 0;
        pthread_mutex_unlock(&g_lock);
        if (st == ST_CONNECTED) RET(3, 0);
        if (ce || st != ST_CONNECTING) RET(3, err(ce ? ce : WSAENOTSOCK));
        nap();
    }
    RET(3, err(WSAECONNREFUSED));
}
W(ws_send) {
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    if (!s || s->state != ST_CONNECTED || s->remote_closed) { pthread_mutex_unlock(&g_lock); RET(4, err(s ? WSAENOTCONN : WSAENOTSOCK)); }
    uint16_t sport = s->port, dport = s->peer_port; uint32_t peer = s->peer_id, conn = s->conn;
    pthread_mutex_unlock(&g_lock);
    uint32_t n = ARG(2), sent = 0;
    while (sent < n) {
        uint32_t k = n - sent > 32768 ? 32768 : n - sent;
        send_frame(K_DATA, peer, sport, dport, conn, HP(ARG(1) + sent), k);
        sent += k;
    }
    RET(4, n);
}
W(ws_recv) {
    uint32_t cap = ARG(2);
    for (;;) {
        pthread_mutex_lock(&g_lock);
        Sock *s = sock_of(ARG(0));
        if (!s) { pthread_mutex_unlock(&g_lock); RET(4, err(WSAENOTSOCK)); }
        if (s->type == SOCK_DGRAM_) { pthread_mutex_unlock(&g_lock); RET(4, err(WSAEINVAL)); }
        if (s->rx_len) {
            uint32_t n = s->rx_len < cap ? s->rx_len : cap;
            memcpy(HP(ARG(1)), s->rx, n);
            if (!(ARG(3) & 2 /* MSG_PEEK */)) { memmove(s->rx, s->rx + n, s->rx_len - n); s->rx_len -= n; }
            pthread_mutex_unlock(&g_lock);
            RET(4, n);
        }
        int closed = s->remote_closed || s->state != ST_CONNECTED;
        int nb = s->nonblock || s->hwnd;
        pthread_mutex_unlock(&g_lock);
        if (closed) RET(4, 0);
        if (nb) RET(4, err(WSAEWOULDBLOCK));
        nap();
    }
}
W(ws_WSAAsyncSelect) {
    pthread_mutex_lock(&g_lock);
    Sock *s = sock_of(ARG(0));
    if (!s) { pthread_mutex_unlock(&g_lock); RET(4, err(WSAENOTSOCK)); }
    s->hwnd = ARG(1); s->msg = ARG(2); s->events = ARG(3);
    LOG("[winsock] WSAAsyncSelect(%x, hwnd %x, msg %x, events %x)\n", ARG(0), ARG(1), ARG(2), ARG(3));
    if (s->type == SOCK_DGRAM_ || s->state == ST_CONNECTED) notify(s, FD_WRITE, 0);
    if (s->dq || s->rx_len) notify(s, FD_READ, 0);
    for (int i = 0; i < s->naccept; i++) notify(s, FD_ACCEPT, 0);
    pthread_mutex_unlock(&g_lock);
    RET(4, 0);
}

/* fd_set: u_int fd_count; SOCKET fd_array[64] */
static int in_set(uint32_t set, uint32_t h) {
    if (!set) return 0;
    for (uint32_t i = 0; i < R32(set); i++) if (R32(set + 4 + 4 * i) == h) return 1;
    return 0;
}
W(ws___WSAFDIsSet) { RET(2, in_set(ARG(1), ARG(0))); }
static int readable(Sock *s) { return s->dq || s->rx_len || s->naccept || (s->type == SOCK_STREAM_ && (s->remote_closed)); }
static int writable(Sock *s) { return s->type == SOCK_DGRAM_ || s->state == ST_CONNECTED; }
W(ws_select) {
    static int nsel; if (nsel++ < 60) LOG("[winsock] select(read %x, write %x, timeout %d.%06d s)\n", ARG(1), ARG(2), ARG(4) ? (int)R32(ARG(4)) : -1, ARG(4) ? (int)R32(ARG(4) + 4) : 0);
    uint32_t rs = ARG(1), wsx = ARG(2), es = ARG(3), tv = ARG(4);
    double deadline = tv ? host_seconds() + (int32_t)R32(tv) + (int32_t)R32(tv + 4) / 1e6 : 0;
    for (;;) {
        uint32_t rin[64], win[64], nr = 0, nw = 0, rcount = rs ? R32(rs) : 0, wcount = wsx ? R32(wsx) : 0;
        pthread_mutex_lock(&g_lock);
        for (uint32_t i = 0; i < rcount && i < 64; i++) { Sock *s = sock_of(R32(rs + 4 + 4 * i)); if (s && readable(s)) rin[nr++] = R32(rs + 4 + 4 * i); }
        for (uint32_t i = 0; i < wcount && i < 64; i++) { Sock *s = sock_of(R32(wsx + 4 + 4 * i)); if (s && writable(s)) win[nw++] = R32(wsx + 4 + 4 * i); }
        pthread_mutex_unlock(&g_lock);
        if (nr + nw || (tv && host_seconds() >= deadline)) {
            if (rs) { W32(rs, nr); for (uint32_t i = 0; i < nr; i++) W32(rs + 4 + 4 * i, rin[i]); }
            if (wsx) { W32(wsx, nw); for (uint32_t i = 0; i < nw; i++) W32(wsx + 4 + 4 * i, win[i]); }
            if (es) W32(es, 0);
            { static int nl; if (nl++ < 60) LOG("[winsock] select -> %u readable, %u writable (timeout %s)\n", nr, nw, tv ? "given" : "none"); }
            RET(5, nr + nw);
        }
        nap();
    }
}

W(ws_inet_addr) {
    unsigned a, b, cc, d;
    char tail;
    if (sscanf(GSTR(ARG(0)), "%u.%u.%u.%u%c", &a, &b, &cc, &d, &tail) != 4 || a > 255 || b > 255 || cc > 255 || d > 255) RET(1, 0xFFFFFFFFu);
    RET(1, a | b << 8 | cc << 16 | d << 24);
}
W(ws_inet_ntoa) {
    uint32_t ip = ARG(0);
    char buf[20];
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24);
    static __thread uint32_t slot;
    if (!slot) slot = heap_alloc(32, 1);
    memcpy(HP(slot), buf, strlen(buf) + 1);
    RET(1, slot);
}
W(ws_gethostbyname) {   /* a dotted address, or this machine under any name */
    const char *name = ARG(0) ? GSTR(ARG(0)) : "";
    LOG("[winsock] gethostbyname(%s)\n", name);
    unsigned a, b, cc, d;
    char tail;
    uint32_t ip = ip_of(g_id);
    if (sscanf(name, "%u.%u.%u.%u%c", &a, &b, &cc, &d, &tail) == 4) ip = a | b << 8 | cc << 16 | d << 24;
    else if (*name && strcasecmp(name, "localhost") && strncasecmp(name, "player", 6) && strcasecmp(name, "virtual-lan")) { g_last_error = WSAHOST_NOT_FOUND; RET(1, 0); }
    /* hostent { char *name; char **aliases; short type; short length; char **addr_list } */
    static __thread uint32_t he;
    if (!he) he = heap_alloc(64, 1);
    uint32_t nm = guest_strdup(name[0] ? name : "virtual-lan"), alias = he + 16, addrs = he + 20, addr = he + 28;
    W32(alias, 0); W32(addr, ip); W32(addrs, addr); W32(addrs + 4, 0);
    W32(he, nm); W32(he + 4, alias); W16(he + 8, 2); W16(he + 10, 4); W32(he + 12, addrs);
    RET(1, he);
}
W(ws_htons) { RET(1, __builtin_bswap16((uint16_t)ARG(0))); }
W(ws_ntohs) { RET(1, __builtin_bswap16((uint16_t)ARG(0))); }
W(ws_htonl) { RET(1, __builtin_bswap32(ARG(0))); }

#define S(name) { "WSOCK32.DLL!" #name, w_ws_##name }
#define SO(n, name) { "WSOCK32.DLL!#" #n, w_ws_##name }
static const ImportHandler g_list[] = {
    S(accept), S(bind), S(connect), S(getpeername), S(getsockname), S(listen), S(recv), S(recvfrom), S(select), S(send), S(sendto),
    S(setsockopt), S(shutdown), S(socket), S(closesocket), S(inet_addr), S(WSAAsyncSelect), S(gethostbyname), S(inet_ntoa),
    S(__WSAFDIsSet), S(htons), S(ntohs), S(htonl), S(WSAStartup), S(WSACleanup), S(WSAGetLastError),
    SO(1, accept), SO(2, bind), SO(3, closesocket), SO(4, connect), SO(5, getpeername), SO(6, getsockname), SO(8, htonl), SO(9, htons),
    SO(10, inet_addr), SO(11, inet_ntoa), SO(13, listen), SO(15, ntohs), SO(16, recv), SO(17, recvfrom), SO(18, select), SO(19, send),
    SO(20, sendto), SO(21, setsockopt), SO(22, shutdown), SO(23, socket), SO(52, gethostbyname), SO(101, WSAAsyncSelect),
    SO(111, WSAGetLastError), SO(115, WSAStartup), SO(116, WSACleanup), SO(151, __WSAFDIsSet),
};
const ImportHandler *winsock_imports(uint32_t *n) { *n = (uint32_t)(sizeof g_list / sizeof g_list[0]); return g_list; }
