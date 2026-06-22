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

#if defined(__x86_64__)
__attribute__((force_align_arg_pointer))
#endif
void
// NOLINTNEXTLINE(bugprone-reserved-identifier) — _start is the ELF entry point.
_start(void) {
    int rc = ws_serve(8080, WS_ROLE_SERVER, on_event);
    // ws_serve only returns on a setup failure; report which step and exit
    // cleanly (running off _start would be UB — a segfault).
    if (rc != 0) {
        warn("echo-server: ");
        warn(ws_io_strerror(rc));
        warn("\n");
    }
    sys_exit(rc == 0 ? 0 : 1);
}
