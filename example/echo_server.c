// WebSocket echo server — freestanding C23, -nostdlib, libc-free.
// All TCP I/O, epoll multiplexing, and the HTTP upgrade live in the SDK's
// POSIX I/O layer (ws/io.h, src/io_posix.c). This example is just an event
// handler plus the ELF entry point. x86-64 only (raw syscalls in io_posix.c).
#include "ws/io.h"

#if !defined(__x86_64__)
#error "echo_server example supports x86-64 only"
#endif

// Bounce a data message back / answer a ping. Other types: no reply here.
static void echo_reply(ws_io *io, ws_conn *c, const ws_event *ev) {
    if (ev->type == WS_EV_MESSAGE) {
        ws_io_send_message(io, c, ev->op, ev->data, ev->len);
        return;
    }
    if (ev->type == WS_EV_PING) {
        ws_io_send_pong(io, c, ev->data, ev->len);
    }
}

// Echo handler. CLOSE mirrors the peer code; the runtime then drops the conn.
// PONG/NONE/ERROR need no reply.
static void on_event(ws_io *io, ws_conn *c, const ws_event *ev) {
    if (ev->type == WS_EV_CLOSE) {
        ws_io_send_close(io, c, ev->close_code);
        return;
    }
    echo_reply(io, c, ev);
}

// Exit via raw syscall (no libc). ws_serve only returns on a setup failure
// (e.g. the port is already in use); exit with its status instead of running
// off the end of _start, which is undefined behaviour (a segfault).
static void sys_exit(int code) {
    register long rax __asm__("rax") = 60;
    register long rdi __asm__("rdi") = code;
    __asm__ volatile("syscall" : : "r"(rax), "r"(rdi) : "memory");
    __builtin_unreachable();
}

static void warn(const char *s) {
    long n = 0;
    while (s[n]) {
        n++;
    }
    register long rax __asm__("rax") = 1; // write
    register long rdi __asm__("rdi") = 2; // stderr
    register long rsi __asm__("rsi") = (long)s;
    register long rdx __asm__("rdx") = n;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
}

#define DEFAULT_PORT 8080

// Fold one decimal digit into acc, or return >65535 to signal invalid input
// (non-digit or overflow), which parse_port maps to 0.
static unsigned long port_digit(unsigned long acc, char ch) {
    if (ch < '0' || ch > '9') {
        return 0x10000; // out of range -> invalid
    }
    return acc * 10 + (unsigned long)(ch - '0');
}

// Parse a base-10 port from a NUL-terminated string. Returns 0 (invalid) or the
// port; out-of-range / non-digit yields 0 so the caller falls back to default.
static uint16_t parse_port(const char *s) {
    unsigned long v = 0;
    for (long i = 0; s[i]; i++) {
        v = port_digit(v, s[i]);
        if (v > 65535) {
            return 0;
        }
    }
    return (uint16_t)v;
}

// Pick the port from argv[1] if present and valid, else DEFAULT_PORT. The stack
// at _start holds [argc][argv[0]][argv[1]]...; sp[0]=argc, sp[1..]=argv.
static uint16_t port_from_stack(const long *sp) {
    long argc = sp[0];
    if (argc < 2) {
        return DEFAULT_PORT;
    }
    uint16_t p = parse_port((const char *)sp[2]); // sp[1]=argv[0], sp[2]=argv[1]
    return p != 0 ? p : DEFAULT_PORT;
}

// Real entry: gets the initial stack pointer from the asm _start below.
// Non-static so the asm `call ws_main` resolves to a stable symbol.
void ws_main(const long *sp);
void ws_main(const long *sp) {
    uint16_t port = port_from_stack(sp);
    int rc = ws_serve(port, WS_ROLE_SERVER, on_event);
    // ws_serve only returns on a setup failure; report which step and exit
    // cleanly (running off the entry would be UB — a segfault).
    if (rc != 0) {
        warn("echo-server: ");
        warn(ws_io_strerror(rc));
        warn("\n");
    }
    sys_exit(rc == 0 ? 0 : 1);
}

// ELF entry. At _start rsp points at argc; a C prologue would clobber it, so a
// naked stub hands the original rsp to ws_main as its argument (System V: rdi).
__attribute__((naked)) void
// NOLINTNEXTLINE(bugprone-reserved-identifier) — _start is the ELF entry point.
_start(void) {
    __asm__ volatile("mov %rsp, %rdi\n\t"
                     "and $-16, %rsp\n\t" // 16-byte align for the call
                     "call ws_main");
}
