// libws POSIX I/O layer — optional convenience runtime over the sans-I/O core.
//
// The protocol core (ws/ws.h) does no I/O. This layer adds an epoll-driven TCP
// server (Linux, raw syscalls, freestanding) so a caller can run a WebSocket
// endpoint by writing only an event handler:
//
//     #include "ws/io.h"
//
//     static void on_event(ws_io *io, ws_conn *c, const ws_event *ev) {
//         switch (ev->type) {
//         case WS_EV_MESSAGE: ws_io_send_message(io, c, ev->op, ev->data, ev->len); break;
//         case WS_EV_PING:    ws_io_send_pong(io, c, ev->data, ev->len);            break;
//         case WS_EV_CLOSE:   ws_io_send_close(io, c, ev->close_code);              break;
//         default: break;
//         }
//     }
//
//     void _start(void) { ws_serve(8080, WS_ROLE_SERVER, on_event); }
//
// socket/bind/listen/accept/epoll/read/write/close all live inside ws_serve.
// The HTTP upgrade handshake (Sec-WebSocket-Accept) is performed automatically;
// on_event only ever sees post-OPEN WebSocket events. Multiple connections are
// multiplexed via epoll. Linux x86-64 only.
#ifndef WS_IO_H
#define WS_IO_H

#include "ws/ws.h"

// Opaque per-server I/O context (holds the listen fd, epoll fd, connection
// pool). Passed to the handler so it can send on the originating connection.
typedef struct ws_io ws_io;

// Event handler. Called for each semantic event drained from a connection.
// `c` is the connection the event came from; reply with the ws_io_send_*
// helpers below. WS_EV_ERROR / WS_EV_CLOSE connections are closed by the
// runtime after the handler returns.
typedef void (*ws_io_handler)(ws_io *io, ws_conn *c, const ws_event *ev);

// Which setup step failed (ws_serve return value, negated errno carried too).
typedef enum {
    WS_IO_ERR_SOCKET = -1, // socket() failed
    WS_IO_ERR_BIND = -2,   // bind() failed (e.g. address already in use)
    WS_IO_ERR_LISTEN = -3, // listen() failed
    WS_IO_ERR_EPOLL = -4,  // epoll_create1()/epoll_ctl() failed
} ws_io_error;

// Run a WebSocket server on the given TCP port until a fatal error. Accepts and
// multiplexes connections with epoll, performs each HTTP upgrade, then drives
// the verified driver and calls `on_event` per event. On a setup failure
// returns the matching ws_io_error (negative); otherwise loops forever.
int ws_serve(uint16_t port, ws_role role, ws_io_handler on_event);

// Human-readable text for a ws_serve setup-failure return. Static string.
const char *ws_io_strerror(int rc);

// Send helpers usable from the handler. They build the frame with ws_send_*
// (server-unmasked / client-masked) and write it to the connection's socket.
// Return 0 on success, negative on a write/build failure.
int ws_io_send_message(ws_io *io, ws_conn *c, ws_opcode op, const uint8_t *payload, size_t len);
int ws_io_send_pong(ws_io *io, ws_conn *c, const uint8_t *payload, size_t len);
int ws_io_send_close(ws_io *io, ws_conn *c, uint16_t code);

#ifndef WS_IO_MAX_CONNS
#define WS_IO_MAX_CONNS 64
#endif

#endif
