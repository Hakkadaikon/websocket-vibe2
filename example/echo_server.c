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

#if defined(__x86_64__)
__attribute__((force_align_arg_pointer))
#endif
void
// NOLINTNEXTLINE(bugprone-reserved-identifier) — _start is the ELF entry point.
_start(void) {
    ws_serve(8080, WS_ROLE_SERVER, on_event);
    __builtin_unreachable();
}
