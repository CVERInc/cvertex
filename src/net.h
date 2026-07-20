// net.h — deterministic lockstep networking. Two peers, no server, no game state on the
// wire. Only ONE Input per player per frame crosses the socket, plus a periodic state
// checksum used purely as a desync tripwire.
//
// 🔴 The whole thing rests on game.h's one rule: tick() is pure. If both peers feed the
// same in[2] every frame from the same init, both sims stay byte-identical forever — so
// there is nothing to synchronise except the two players' intentions. That is what makes a
// 4-byte-per-frame protocol enough to play the same game on two machines.
//
// Zero third-party deps: this is BSD sockets (<sys/socket.h> …), which are OS-level like
// mac.c's Cocoa calls, not a library. POSIX (macOS + Linux) as written; the one Windows
// delta (WSAStartup / closesocket) is fenced under _WIN32 so the file still compiles there.
#ifndef NET_H
#define NET_H
#include <stdint.h>
#include "core.h"   // Input

#define NET_DEFAULT_PORT 47800

// Become player 1: listen on `port`, block until exactly one peer connects, then stop
// listening. Returns 0 on success, -1 on failure (prints why to stderr).
int net_host(int port);

// Become player 2: connect to the host at ip:port. Returns 0 on success, -1 on failure.
int net_join(const char *ip, int port);

// One lockstep frame. Sends our local input and our current state checksum, then BLOCKS
// for the peer's. `remote_in` and `remote_sum` are filled from the peer. Returns 0 on
// success, -1 on disconnect/error — on -1 the caller must NOT tick a half-synced frame.
//
// Ordering is send-then-receive on both ends. The payload is 12 bytes, far under any
// socket buffer, so neither send() blocks the other and there is no deadlock.
int net_exchange(const Input *local_in, Input *remote_in,
                 uint64_t local_sum, uint64_t *remote_sum);

int  net_active(void);   // 1 once hosting/joined; 0 keeps the default single-machine path untouched
int  net_role(void);     // 0 = host / player 1, 1 = joiner / player 2 (valid only when net_active())
void net_close(void);

#endif
