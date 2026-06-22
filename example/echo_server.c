// WebSocket echo server example — freestanding C23, -nostdlib, libc-free.
// The SDK (libws) is sans-I/O; this example owns the TCP I/O via raw Linux
// syscalls. x86-64 only (example): aarch64 socket syscall numbers are not
// verified here, so we #error on other targets.
//
// Flow: socket -> setsockopt(SO_REUSEADDR) -> bind(0.0.0.0:8080) -> listen ->
// accept loop. Per connection (sequential; no fork/epoll, YAGNI): read the HTTP
// upgrade request, compute Sec-WebSocket-Accept, reply 101, then echo frames.
//
// Frame handling is entirely the SDK driver's job: feed received bytes with
// ws_conn_recv, drain semantic events with ws_conn_poll, build replies with
// ws_send_*. Fragment reassembly, masking, and the close handshake all happen
// inside the (TLA+/Lean-verified) SDK, so this example stays thin.
//
// Simplifications (example scope):
//   - one accept handles one client sequentially (no fork/epoll, YAGNI).
//   - read/message buffers are fixed (handshake 2 KiB, message WS_MAX_MESSAGE).
#include "../src/ws_internal.h" // ws_memcmp (SDK internal, linked in)
#include "ws/ws.h"

#if !defined(__x86_64__)
#error "echo_server example supports x86-64 only"
#endif

// ---- raw syscalls (x86-64) ----
static long syscall6(long n, long a, long b, long c, long d, long e, long f) {
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a;
    register long rsi __asm__("rsi") = b;
    register long rdx __asm__("rdx") = c;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return rax;
}

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_socket 41
#define SYS_accept 43
#define SYS_bind 49
#define SYS_listen 50
#define SYS_setsockopt 54
#define SYS_exit 60

static long sys_read(int fd, void *buf, unsigned long n) {
    return syscall6(SYS_read, fd, (long)buf, (long)n, 0, 0, 0);
}
static long sys_write(int fd, const void *buf, unsigned long n) {
    return syscall6(SYS_write, fd, (long)buf, (long)n, 0, 0, 0);
}
static void sys_close(int fd) {
    (void)syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
}
static void sys_exit(int code) {
    (void)syscall6(SYS_exit, code, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

// ---- socket plumbing (no netinet/sys headers; build structs by hand) ----
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define PORT 8080

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port; // network byte order
    uint32_t sin_addr; // network byte order
    uint8_t sin_zero[8];
};

// htons without libc: high byte first.
static uint16_t htons16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

// bind(0.0.0.0:PORT) then listen. Returns 0 on success.
static int bind_listen(long fd) {
    int one = 1;
    (void)syscall6(SYS_setsockopt, fd, SOL_SOCKET, SO_REUSEADDR, (long)&one, sizeof one, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons16(PORT);
    addr.sin_addr = 0; // INADDR_ANY (0.0.0.0)
    if (syscall6(SYS_bind, fd, (long)&addr, sizeof addr, 0, 0, 0) < 0) {
        return -1;
    }
    return syscall6(SYS_listen, fd, 16, 0, 0, 0, 0) < 0 ? -1 : 0;
}

static int listen_socket(void) {
    long fd = syscall6(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) {
        return (int)fd;
    }
    return bind_listen(fd) < 0 ? -1 : (int)fd;
}

// Write the full buffer; loops on short writes. Returns 0 on success.
static int write_all(int fd, const uint8_t *buf, unsigned long n) {
    unsigned long off = 0;
    while (off < n) {
        long w = sys_write(fd, buf + off, n - off);
        if (w <= 0) {
            return -1;
        }
        off += (unsigned long)w;
    }
    return 0;
}

// ---- handshake ----
static unsigned long str_len(const char *s) {
    unsigned long n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

// Case-sensitive substring search within [buf, buf+len). Returns offset or -1.
static long find_sub(const uint8_t *buf, unsigned long len, const char *needle) {
    unsigned long nlen = str_len(needle);
    long last = (long)len - (long)nlen;
    for (long i = 0; i <= last; i++) {
        if (ws_memcmp(buf + i, (const uint8_t *)needle, nlen) == 0) {
            return i;
        }
    }
    return -1;
}

static int is_space(uint8_t c) {
    return c == ' ' || c == '\t';
}

static int is_eol(uint8_t c) {
    return c == '\r' || c == '\n';
}

// Skip leading spaces/tabs in [buf+i, buf+len). Returns the new index.
static unsigned long skip_spaces(const uint8_t *buf, unsigned long i, unsigned long len) {
    while (i < len && is_space(buf[i])) {
        i++;
    }
    return i;
}

// Copy header-value bytes (up to EOL or cap) into key[]. Returns the count.
static unsigned long copy_value(const uint8_t *buf, unsigned long i, unsigned long limit,
                                uint8_t *key) {
    unsigned long n = 0;
    while (i < limit && !is_eol(buf[i])) {
        key[n++] = buf[i++];
    }
    return n;
}

// Extract the Sec-WebSocket-Key value into key[]; returns its length or -1.
static long extract_key(const uint8_t *buf, unsigned long len, uint8_t *key, unsigned long cap) {
    static const char hdr[] = "Sec-WebSocket-Key:";
    long at = find_sub(buf, len, hdr);
    if (at < 0) {
        return -1;
    }
    unsigned long i = skip_spaces(buf, (unsigned long)at + str_len(hdr), len);
    unsigned long limit = len < i + cap ? len : i + cap;
    return (long)copy_value(buf, i, limit, key);
}

// Read the HTTP request into req[] until the blank line. Returns byte count or -1.
static long read_request(int fd, uint8_t *req, unsigned long cap) {
    unsigned long got = 0;
    while (find_sub(req, got, "\r\n\r\n") < 0) {
        long r = sys_read(fd, req + got, cap - got);
        if (r <= 0) {
            return -1;
        }
        got += (unsigned long)r;
    }
    return (long)got;
}

// Write the 101 response carrying the 28-byte Accept. Returns 0 on success.
static int send_accept(int fd, const uint8_t accept[28]) {
    static const char pre[] = "HTTP/1.1 101 Switching Protocols\r\n"
                              "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: ";
    if (write_all(fd, (const uint8_t *)pre, str_len(pre)) != 0) {
        return -1;
    }
    if (write_all(fd, accept, 28) != 0) {
        return -1;
    }
    return write_all(fd, (const uint8_t *)"\r\n\r\n", 4);
}

// Compute the Accept from the request bytes into accept[28]. Returns 0 on success.
static int compute_accept(const uint8_t *req, unsigned long got, uint8_t accept[28]) {
    uint8_t key[64];
    long klen = extract_key(req, got, key, sizeof key);
    if (klen <= 0) {
        return -1;
    }
    return ws_handshake_accept(key, (unsigned long)klen, accept) == 28 ? 0 : -1;
}

// Read the request, compute Accept, send the 101. Returns 0 on success.
static int do_handshake(int fd) {
    uint8_t req[2048];
    long got = read_request(fd, req, sizeof req);
    if (got < 0) {
        return -1;
    }
    uint8_t accept[28];
    if (compute_accept(req, (unsigned long)got, accept) != 0) {
        return -1;
    }
    return send_accept(fd, accept);
}

// ---- frame loop (driver-based) ----
// Write a frame already built into out[0..n). 0 on success, -1 on failure.
static int send_built(int fd, const uint8_t *out, size_t n) {
    if (n == 0) {
        return -1;
    }
    return write_all(fd, out, (unsigned long)n);
}

// CLOSE/ERROR end the connection. ponytail: the driver has already torn the
// lifecycle down (CLOSED); we drop without a courtesy reply (the verifying
// client closes after the echo and does not await a server CLOSE).
static int is_terminal(ws_event_type t) {
    if (t == WS_EV_CLOSE) {
        return 1;
    }
    return t == WS_EV_ERROR;
}

// React to one drained event. Returns 0 to keep going, 1 to close the conn.
static int on_event(int fd, ws_conn *c, const ws_event *ev) {
    uint8_t out[WS_MAX_MESSAGE + 16];
    if (ev->type == WS_EV_MESSAGE) {
        return send_built(fd, out, ws_send_message(c, ev->op, ev->data, ev->len, out, sizeof out));
    }
    if (ev->type == WS_EV_PING) {
        return send_built(fd, out, ws_send_pong(c, ev->data, ev->len, out, sizeof out));
    }
    return is_terminal(ev->type); // NONE/PONG -> 0 keep going.
}

// Drain all buffered events. Returns 0 to keep reading, 1 to close.
static int drain(int fd, ws_conn *c) {
    ws_event ev;
    while (ws_conn_poll(c, &ev) != WS_EV_NONE) {
        if (on_event(fd, c, &ev) != 0) {
            return 1;
        }
    }
    return 0;
}

// Read one chunk from the socket, feed it to the driver, drain events.
// Returns 0 to keep looping, 1 to close the connection.
static int pump(int fd, ws_conn *c) {
    uint8_t chunk[4096];
    long r = sys_read(fd, chunk, sizeof chunk);
    if (r <= 0) {
        return 1; // peer closed / error
    }
    if (ws_conn_recv(c, chunk, (size_t)r) < 0) {
        return 1; // message exceeds WS_MAX_MESSAGE
    }
    return drain(fd, c);
}

static void serve(int fd) {
    if (do_handshake(fd) != 0) {
        return;
    }
    static uint8_t msg_buf[WS_MAX_MESSAGE];
    ws_conn conn;
    ws_conn_init(&conn, WS_ROLE_SERVER, msg_buf, sizeof msg_buf);
    ws_conn_open(&conn);
    while (pump(fd, &conn) == 0) {
    }
}

// Accept one client, serve it, close it. Skips on accept failure.
static void accept_one(int lfd) {
    long cfd = syscall6(SYS_accept, lfd, 0, 0, 0, 0, 0);
    if (cfd < 0) {
        return;
    }
    serve((int)cfd);
    sys_close((int)cfd);
}

#if defined(__x86_64__)
__attribute__((force_align_arg_pointer))
#endif
void
// NOLINTNEXTLINE(bugprone-reserved-identifier) — _start is the ELF entry point.
_start(void) {
    int lfd = listen_socket();
    if (lfd < 0) {
        sys_exit(1);
    }
    for (;;) {
        accept_one(lfd);
    }
}
