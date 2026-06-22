// libws POSIX I/O layer — epoll-driven TCP server over the sans-I/O core.
// Linux x86-64 only, freestanding C23, no libc. The socket/HTTP-handshake
// plumbing is lifted from the (working) echo_server example; epoll multiplexing
// and a fixed connection pool are added so one event loop serves many clients.
//
// Memory: the pool is a single static ws_io. Each slot owns a WS_MAX_MESSAGE
// message buffer, so the server costs WS_IO_MAX_CONNS * WS_MAX_MESSAGE
// (default 64 * 64 KiB = 4 MiB) plus per-slot handshake scratch. No malloc.
#include "ws/io.h"
#include "../ws_internal.h" // ws_memcmp

#if !defined(__x86_64__)
#error "io_posix supports x86-64 only"
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
#define SYS_epoll_wait 232
#define SYS_epoll_ctl 233
#define SYS_epoll_create1 291

static long sys_read(int fd, void *buf, unsigned long n) {
    return syscall6(SYS_read, fd, (long)buf, (long)n, 0, 0, 0);
}
static long sys_write(int fd, const void *buf, unsigned long n) {
    return syscall6(SYS_write, fd, (long)buf, (long)n, 0, 0, 0);
}
static void sys_close(int fd) {
    (void)syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
}

// ---- socket plumbing (no netinet/sys headers; build structs by hand) ----
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_REUSEADDR 2

// epoll: epoll_event is packed on x86-64 (12 bytes: u32 events; u64 data).
#define EPOLLIN 0x001
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

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

// bind(0.0.0.0:port). Returns 0 or WS_IO_ERR_BIND (e.g. address in use).
static int do_bind(long fd, uint16_t port) {
    int one = 1;
    (void)syscall6(SYS_setsockopt, fd, SOL_SOCKET, SO_REUSEADDR, (long)&one, sizeof one, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons16(port);
    addr.sin_addr = 0; // INADDR_ANY (0.0.0.0)
    if (syscall6(SYS_bind, fd, (long)&addr, sizeof addr, 0, 0, 0) < 0) {
        return WS_IO_ERR_BIND;
    }
    return 0;
}

// epoll register/unregister of fd for EPOLLIN. Returns the syscall result.
static long epoll_add(int epfd, int fd) {
    struct epoll_event ev = {.events = EPOLLIN, .data = (uint64_t)fd};
    return syscall6(SYS_epoll_ctl, epfd, EPOLL_CTL_ADD, fd, (long)&ev, 0, 0);
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

// ---- handshake (HTTP upgrade) ----
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

// ---- connection pool ----
typedef struct {
    int fd;
    _Bool used;
    _Bool open; // handshake done -> WS frames flow
    ws_conn conn;
    uint8_t msg_buf[WS_MAX_MESSAGE]; // per-conn reassembly buffer (no malloc)
    uint8_t hsbuf[2048];             // HTTP upgrade request accumulator
    size_t hslen;
} ws_slot;

struct ws_io {
    int listen_fd;
    int epoll_fd;
    ws_role role;
    ws_io_handler on_event;
    ws_slot conns[WS_IO_MAX_CONNS];
};

// ponytail: linear scan over a 64-slot pool — O(n) accept/lookup is fine at
// this scale; swap for a free-list + fd->slot map only if WS_IO_MAX_CONNS grows.
static ws_slot *alloc_slot(ws_io *io) {
    for (int i = 0; i < WS_IO_MAX_CONNS; i++) {
        if (!io->conns[i].used) {
            return &io->conns[i];
        }
    }
    return NULL;
}

static int slot_has_fd(const ws_slot *s, int fd) {
    if (!s->used) {
        return 0;
    }
    return s->fd == fd ? 1 : 0;
}

static int slot_has_conn(const ws_slot *s, const ws_conn *c) {
    if (!s->used) {
        return 0;
    }
    return &s->conn == c ? 1 : 0;
}

static ws_slot *slot_by_fd(ws_io *io, int fd) {
    for (int i = 0; i < WS_IO_MAX_CONNS; i++) {
        if (slot_has_fd(&io->conns[i], fd)) {
            return &io->conns[i];
        }
    }
    return NULL;
}

static ws_slot *slot_by_conn(ws_io *io, const ws_conn *c) {
    for (int i = 0; i < WS_IO_MAX_CONNS; i++) {
        if (slot_has_conn(&io->conns[i], c)) {
            return &io->conns[i];
        }
    }
    return NULL;
}

static void slot_close(ws_io *io, ws_slot *s) {
    WS_TRACE_IO("close", s->fd, -1);
    (void)syscall6(SYS_epoll_ctl, io->epoll_fd, EPOLL_CTL_DEL, s->fd, 0, 0, 0);
    sys_close(s->fd);
    s->used = false;
}

// ---- send helpers (build with the verified ws_send_*, write to the socket) ----
// conn -> fd via the pool; out is a per-call stack frame buffer.
static int send_built(ws_io *io, ws_conn *c, size_t n, const uint8_t *out) {
    ws_slot *s = slot_by_conn(io, c);
    if (s == NULL || n == 0) {
        return -1;
    }
    return write_all(s->fd, out, (unsigned long)n);
}

int ws_io_send_message(ws_io *io, ws_conn *c, ws_opcode op, const uint8_t *payload, size_t len) {
    uint8_t out[WS_MAX_MESSAGE + 16];
    return send_built(io, c, ws_send_message(c, op, payload, len, out, sizeof out), out);
}

int ws_io_send_pong(ws_io *io, ws_conn *c, const uint8_t *payload, size_t len) {
    uint8_t out[WS_MAX_MESSAGE + 16];
    return send_built(io, c, ws_send_pong(c, payload, len, out, sizeof out), out);
}

int ws_io_send_close(ws_io *io, ws_conn *c, uint16_t code) {
    uint8_t out[32];
    return send_built(io, c, ws_send_close(c, code, out, sizeof out), out);
}

// ---- accept path ----
static void on_accept(ws_io *io) {
    long cfd = syscall6(SYS_accept, io->listen_fd, 0, 0, 0, 0, 0);
    if (cfd < 0) {
        return;
    }
    ws_slot *s = alloc_slot(io);
    if (s == NULL) {
        WS_TRACE_IO("reject", (int)cfd, -1);
        sys_close((int)cfd); // pool full: refuse
        return;
    }
    *s = (ws_slot){.fd = (int)cfd, .used = true};
    (void)epoll_add(io->epoll_fd, (int)cfd);
    WS_TRACE_IO("accept", (int)cfd, -1);
}

// ---- per-event drain (post-OPEN) ----
// Returns 1 if the connection must close (CLOSE/ERROR seen), else 0.
static int is_terminal(ws_event_type t) {
    return t == WS_EV_CLOSE || t == WS_EV_ERROR;
}

static int drain_events(ws_io *io, ws_slot *s) {
    ws_event ev;
    while (ws_conn_poll(&s->conn, &ev) != WS_EV_NONE) {
        io->on_event(io, &s->conn, &ev);
        if (is_terminal(ev.type)) {
            return 1;
        }
    }
    return 0;
}

// Feed raw bytes into the driver and drain. Returns 1 to close, else 0.
static int feed(ws_io *io, ws_slot *s, const uint8_t *bytes, size_t len) {
    if (len > 0 && ws_conn_recv(&s->conn, bytes, len) < 0) {
        return 1;
    }
    return drain_events(io, s);
}

// ---- handshake path (pre-OPEN) ----
// Complete the upgrade: 101 reply, init+open the driver, then feed any WS bytes
// that arrived in the same read after "\r\n\r\n". Returns 1 to close, else 0.
static int finish_handshake(ws_io *io, ws_slot *s, long body_at) {
    uint8_t accept[28];
    if (compute_accept(s->hsbuf, (unsigned long)body_at, accept) != 0) {
        return 1;
    }
    if (send_accept(s->fd, accept) != 0) {
        return 1;
    }
    ws_conn_init(&s->conn, io->role, s->msg_buf, sizeof s->msg_buf);
    ws_conn_open(&s->conn);
    s->open = true;
    WS_TRACE_IO("handshake", s->fd, -1);
    unsigned long off = (unsigned long)body_at + 4; // past "\r\n\r\n"
    return feed(io, s, s->hsbuf + off, s->hslen - off);
}

// The blank line is here -> upgrade; not yet -> close only if the buffer is
// full (header too large). Returns 1 to close, else 0.
static int handshake_step(ws_io *io, ws_slot *s) {
    long at = find_sub(s->hsbuf, s->hslen, "\r\n\r\n");
    if (at < 0) {
        return s->hslen >= sizeof s->hsbuf ? 1 : 0; // header too large -> close
    }
    return finish_handshake(io, s, at);
}

// Accumulate the HTTP request, then try to complete the upgrade.
// Returns 1 to close, else 0.
static int on_handshake_data(ws_io *io, ws_slot *s) {
    long r = sys_read(s->fd, s->hsbuf + s->hslen, sizeof s->hsbuf - s->hslen);
    if (r <= 0) {
        return 1;
    }
    s->hslen += (size_t)r;
    return handshake_step(io, s);
}

// One readable client fd: dispatch to handshake or frame pump.
static int on_client_data(ws_io *io, ws_slot *s) {
    if (!s->open) {
        return on_handshake_data(io, s);
    }
    uint8_t chunk[4096];
    long r = sys_read(s->fd, chunk, sizeof chunk);
    if (r <= 0) {
        return 1;
    }
    return feed(io, s, chunk, (size_t)r);
}

// ---- event loop ----
static void service_client(ws_io *io, ws_slot *s) {
    if (on_client_data(io, s) != 0) {
        slot_close(io, s);
    }
}

static void on_readable(ws_io *io, int fd) {
    if (fd == io->listen_fd) {
        on_accept(io);
        return;
    }
    ws_slot *s = slot_by_fd(io, fd);
    if (s != NULL) {
        service_client(io, s);
    }
}

static void run_loop(ws_io *io) {
    struct epoll_event evs[WS_IO_MAX_CONNS + 1] = {0}; // kernel fills it; init quiets the analyzer
    for (;;) {
        long n = syscall6(SYS_epoll_wait, io->epoll_fd, (long)evs, WS_IO_MAX_CONNS + 1, -1, 0, 0);
        for (long i = 0; i < n; i++) {
            on_readable(io, (int)evs[i].data);
        }
    }
}

// bind then listen on an open socket. Returns 0 or the failing step's error.
static int bind_then_listen(int fd, uint16_t port) {
    int rc = do_bind(fd, port);
    if (rc != 0) {
        return rc;
    }
    return syscall6(SYS_listen, fd, 16, 0, 0, 0, 0) < 0 ? WS_IO_ERR_LISTEN : 0;
}

// Create + bind + listen the server socket. Returns 0 or a ws_io_error step.
static int open_listener(ws_io *io, uint16_t port) {
    io->listen_fd = (int)syscall6(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
    if (io->listen_fd < 0) {
        return WS_IO_ERR_SOCKET;
    }
    return bind_then_listen(io->listen_fd, port);
}

// Create the epoll fd and register the listener for EPOLLIN. 0 or WS_IO_ERR_EPOLL.
static int open_epoll(ws_io *io) {
    io->epoll_fd = (int)syscall6(SYS_epoll_create1, 0, 0, 0, 0, 0, 0);
    if (io->epoll_fd < 0) {
        return WS_IO_ERR_EPOLL;
    }
    return epoll_add(io->epoll_fd, io->listen_fd) < 0 ? WS_IO_ERR_EPOLL : 0;
}

// Full setup: listener then epoll. Returns 0 or the failing step's ws_io_error.
static int io_setup(ws_io *io, uint16_t port) {
    int rc = open_listener(io, port);
    if (rc != 0) {
        return rc;
    }
    return open_epoll(io);
}

int ws_serve(uint16_t port, ws_role role, ws_io_handler on_event) {
    // ponytail: single static ws_io (~WS_IO_MAX_CONNS * WS_MAX_MESSAGE, 4 MiB by
    // default) keeps the runtime malloc-free; one server per process.
    static ws_io io;
    io.role = role;
    io.on_event = on_event;
    int rc = io_setup(&io, port);
    if (rc != 0) {
        return rc;
    }
    run_loop(&io);
    return 0;
}

const char *ws_io_strerror(int rc) {
    // rc is 0 or a ws_io_error in -1..-4; index by -rc.
    static const char *const msg[] = {
        "ok",
        "socket() failed",
        "bind() failed (port already in use?)",
        "listen() failed",
        "epoll setup failed",
    };
    unsigned idx = (rc <= 0 && rc >= WS_IO_ERR_EPOLL) ? (unsigned)(-rc) : 0;
    return msg[idx];
}
