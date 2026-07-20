// net.c — the lockstep transport. TCP on a fixed port: reliable and ordered, which is all
// lockstep needs on a LAN where the round trip is a fraction of a 16ms frame. No game state
// is ever sent; see net.h for why 4 input bytes a frame is the entire protocol.
#include "net.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  // Windows delta, fenced off so this file still compiles there. Not wired into a Windows
  // main loop yet (see win.c note) — but the socket layer itself is ready.
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_BAD INVALID_SOCKET
  #define close_sock closesocket
  static int wsa_up = 0;
  static void net_platform_init(void) { if (!wsa_up) { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); wsa_up = 1; } }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int sock_t;
  #define SOCK_BAD (-1)
  #define close_sock close
  static void net_platform_init(void) {}
#endif

// Wire packet: 14 bytes, hand-serialised little-endian so struct padding can never make the
// two ends disagree about byte layout. [0..5] = Input (x,y,rx,ry,jump,act); [6..13] = checksum.
// rx,ry (the LOOK stick) ride the wire too: a dual-stick game folds pitch into its
// checksum, so a remote whose look never arrived would desync the instant the peer looked up.
#define NET_PKT 14

static sock_t g_sock = SOCK_BAD;
static int    g_role = -1;   // -1 = inactive, 0 = host, 1 = joiner

int net_active(void) { return g_sock != SOCK_BAD; }
int net_role(void)   { return g_role; }

static void set_nodelay(sock_t s) {
    int one = 1;
    // Nagle would hold our 12-byte packet waiting for more; at 60Hz that is a stall we never
    // want. Send each frame's packet the instant it is written.
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

// send()/recv() may move fewer bytes than asked; loop until the whole packet is through.
static int send_all(const uint8_t *b, int n) {
    int off = 0;
    while (off < n) {
        int r = (int)send(g_sock, (const char *)b + off, n - off, 0);
        if (r <= 0) return -1;
        off += r;
    }
    return 0;
}
static int recv_all(uint8_t *b, int n) {
    int off = 0;
    while (off < n) {
        int r = (int)recv(g_sock, (char *)b + off, n - off, 0);
        if (r <= 0) return -1;   // 0 = peer closed, <0 = error
        off += r;
    }
    return 0;
}

int net_host(int port) {
    net_platform_init();
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == SOCK_BAD) { fprintf(stderr, "[net] socket() failed\n"); return -1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);

    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "[net] bind port %d failed (in use?)\n", port); close_sock(ls); return -1;
    }
    if (listen(ls, 1) < 0) { fprintf(stderr, "[net] listen failed\n"); close_sock(ls); return -1; }

    fprintf(stderr, "[net] hosting on port %d — waiting for a peer to join…\n", port);
    sock_t cs = accept(ls, 0, 0);
    close_sock(ls);
    if (cs == SOCK_BAD) { fprintf(stderr, "[net] accept failed\n"); return -1; }

    g_sock = cs; g_role = 0;
    set_nodelay(g_sock);
    fprintf(stderr, "[net] peer connected — you are player 1.\n");
    return 0;
}

int net_join(const char *ip, int port) {
    net_platform_init();
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_BAD) { fprintf(stderr, "[net] socket() failed\n"); return -1; }

    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        fprintf(stderr, "[net] bad address '%s'\n", ip); close_sock(s); return -1;
    }
    fprintf(stderr, "[net] joining %s:%d…\n", ip, port);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "[net] connect to %s:%d failed (is the host up?)\n", ip, port);
        close_sock(s); return -1;
    }
    g_sock = s; g_role = 1;
    set_nodelay(g_sock);
    fprintf(stderr, "[net] connected — you are player 2.\n");
    return 0;
}

int net_exchange(const Input *local_in, Input *remote_in,
                 uint64_t local_sum, uint64_t *remote_sum) {
    if (g_sock == SOCK_BAD) return -1;

    uint8_t tx[NET_PKT];
    tx[0] = (uint8_t)local_in->x;
    tx[1] = (uint8_t)local_in->y;
    tx[2] = (uint8_t)local_in->rx;
    tx[3] = (uint8_t)local_in->ry;
    tx[4] = local_in->jump;
    tx[5] = local_in->act;
    for (int i = 0; i < 8; i++) tx[6 + i] = (uint8_t)((local_sum >> (8 * i)) & 0xff);
    if (send_all(tx, NET_PKT)) return -1;

    uint8_t rx[NET_PKT];
    if (recv_all(rx, NET_PKT)) return -1;
    remote_in->x    = (int8_t)rx[0];
    remote_in->y    = (int8_t)rx[1];
    remote_in->rx   = (int8_t)rx[2];
    remote_in->ry   = (int8_t)rx[3];
    remote_in->jump = rx[4];
    remote_in->act  = rx[5];
    uint64_t s = 0;
    for (int i = 0; i < 8; i++) s |= (uint64_t)rx[6 + i] << (8 * i);
    *remote_sum = s;
    return 0;
}

void net_close(void) {
    if (g_sock != SOCK_BAD) { close_sock(g_sock); g_sock = SOCK_BAD; }
    g_role = -1;
}
