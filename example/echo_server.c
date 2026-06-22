// WebSocket echo server example — freestanding C23, -nostdlib, libc-free.
// The SDK (libws) is sans-I/O; this example owns the TCP I/O via raw Linux
// syscalls. x86-64 only (example): aarch64 socket syscall numbers are not
// verified here, so we #error on other targets.
//
// Flow: socket -> setsockopt(SO_REUSEADDR) -> bind(0.0.0.0:8080) -> listen ->
// accept loop. Per connection (sequential; no fork/epoll, YAGNI): read the HTTP
// upgrade request, compute Sec-WebSocket-Accept, reply 101, then echo frames.
//
// Simplifications (example scope):
//   - fragmented messages (CONT / fin=0) are unsupported -> close 1003.
//   - one accept handles one client sequentially.
//   - read buffers are fixed (handshake 2 KiB, frame 64 KiB).
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

// ---- frame loop ----
#define FRAME_CAP 65536

// Connection-local read buffer with a fill cursor.
typedef struct {
    int fd;
    uint8_t buf[FRAME_CAP];
    unsigned long len;
} rbuf;

// Read once into the buffer's free space. Returns 0 on success.
static int read_more(rbuf *rb) {
    long r = sys_read(rb->fd, rb->buf + rb->len, FRAME_CAP - rb->len);
    if (r <= 0) {
        return -1;
    }
    rb->len += (unsigned long)r;
    return 0;
}

// Ensure at least `need` bytes are buffered; reads more if short. Returns 0 ok.
// need > FRAME_CAP self-fails: the buffer fills, read_more then reads 0 -> -1.
static int fill_to(rbuf *rb, unsigned long need) {
    while (rb->len < need) {
        if (read_more(rb) != 0) {
            return -1;
        }
    }
    return 0;
}

// Drop the first `n` bytes, shifting the remainder down. dst < src, so a
// forward byte copy is overlap-safe (no memmove in the SDK).
static void consume(rbuf *rb, unsigned long n) {
    unsigned long rest = rb->len - n;
    for (unsigned long i = 0; i < rest; i++) {
        rb->buf[i] = rb->buf[n + i];
    }
    rb->len = rest;
}

// Read one full frame header, growing the buffer until ws_parse_header is
// satisfied. Returns header bytes consumed (>0) or negative on error.
static int read_header(rbuf *rb, ws_frame_header *h) {
    int hn = ws_parse_header(rb->buf, rb->len, h);
    while (hn == WS_ERR_NEED_MORE) {
        if (fill_to(rb, rb->len + 1) != 0) {
            return -1;
        }
        hn = ws_parse_header(rb->buf, rb->len, h);
    }
    return hn;
}

// Send a non-masked frame (server->client) with the given opcode and payload.
static int send_frame(int fd, ws_opcode op, const uint8_t *payload, uint64_t plen) {
    ws_frame_header h = {.opcode = op, .payload_len = plen, .fin = true, .masked = false};
    uint8_t hdr[14];
    int hn = ws_build_header(&h, hdr, sizeof hdr);
    if (hn < 0) {
        return -1;
    }
    if (write_all(fd, hdr, (unsigned long)hn) != 0) {
        return -1;
    }
    return write_all(fd, payload, (unsigned long)plen);
}

static int send_close(int fd, uint16_t code) {
    uint8_t body[2] = {(uint8_t)(code >> 8), (uint8_t)code};
    return send_frame(fd, WS_OP_CLOSE, body, 2);
}

// Pull the full payload of the current header into the buffer and unmask it.
// On success the payload sits at rb->buf[0..plen) and the header is consumed.
static int take_payload(rbuf *rb, const ws_frame_header *h, unsigned long hn) {
    unsigned long plen = (unsigned long)h->payload_len;
    // ponytail: frames > 64 KiB rejected by fill_to (need > FRAME_CAP); bump FRAME_CAP if needed.
    if (fill_to(rb, hn + plen) != 0) {
        return -1;
    }
    consume(rb, hn);
    ws_mask(rb->buf, plen, h->mask_key); // header already gated masked (RFC 5.1).
    return 0;
}

// Handle a data-class frame. CONT means a fragment, which this example does
// not support -> close 1003. TEXT/BIN are echoed unmasked. 0 continue, 1 close.
static int handle_data(int fd, const ws_frame_header *h, const uint8_t *p) {
    if (h->opcode == WS_OP_CONT) {
        send_close(fd, 1003); // ponytail: fragmented messages unsupported.
        return 1;
    }
    return send_frame(fd, h->opcode, p, h->payload_len) == 0 ? 0 : 1;
}

// Reply to a CLOSE: drive the state machine and echo a close. Always closes.
static int reply_close(int fd, ws_conn *c) {
    ws_conn_step(c, WS_EV_RECV_CLOSE, WS_FRAG_NONE);
    send_close(fd, 1000); // ponytail: echo 1000; peer's code not re-validated.
    return 1;
}

// Reply to a PING with a PONG carrying the same payload. 0 continue, 1 close.
static int reply_pong(int fd, const ws_frame_header *h, const uint8_t *p) {
    return send_frame(fd, WS_OP_PONG, p, h->payload_len) == 0 ? 0 : 1;
}

// Handle a control frame (CLOSE/PING/PONG). Returns 0 continue, 1 close.
static int handle_control(int fd, ws_conn *c, const ws_frame_header *h, const uint8_t *p) {
    if (h->opcode == WS_OP_CLOSE) {
        return reply_close(fd, c);
    }
    if (h->opcode == WS_OP_PING) {
        return reply_pong(fd, h, p);
    }
    return 0; // PONG: ignore.
}

// Dispatch one frame by opcode class. Returns 0 to continue, 1 to close.
static int handle_frame(int fd, ws_conn *c, const ws_frame_header *h, const uint8_t *p) {
    if (ws_classify_opcode((uint8_t)h->opcode) == WS_CLASS_CONTROL) {
        return handle_control(fd, c, h, p);
    }
    if (ws_classify_opcode((uint8_t)h->opcode) == WS_CLASS_DATA) {
        return handle_data(fd, h, p);
    }
    send_close(fd, 1002); // reserved opcode -> protocol error.
    return 1;
}

// Process one frame: header, mask check, payload, dispatch, consume.
// Returns 0 to keep looping, 1 to close the connection.
// Read a header; reject unmasked client frames (RFC 5.1). Returns header
// length, or -1 if the connection must close (sending 1002 when warranted).
static int read_masked_header(int fd, rbuf *rb, ws_frame_header *h) {
    int hn = read_header(rb, h);
    if (hn <= 0) {
        return -1; // truncated / connection closed.
    }
    if (!h->masked) {
        send_close(fd, 1002); // client frames must be masked (RFC 5.1).
        return -1;
    }
    return hn;
}

static int process_one(int fd, ws_conn *c, rbuf *rb) {
    ws_frame_header h;
    int hn = read_masked_header(fd, rb, &h);
    if (hn < 0 || take_payload(rb, &h, (unsigned long)hn) != 0) {
        return 1;
    }
    int close = handle_frame(fd, c, &h, rb->buf);
    consume(rb, (unsigned long)h.payload_len);
    return close;
}

static void serve(int fd) {
    if (do_handshake(fd) != 0) {
        return;
    }
    ws_conn conn;
    ws_conn_init(&conn);
    ws_conn_step(&conn, WS_EV_HANDSHAKE, WS_FRAG_NONE);
    static rbuf rb;
    rb.fd = fd;
    rb.len = 0;
    while (process_one(fd, &conn, &rb) == 0) {
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
