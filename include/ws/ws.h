// libws — RFC 6455 WebSocket protocol core. sans-I/O, freestanding C23.
// The caller owns all buffers, all I/O, and all allocation. This library only
// transforms bytes: parse/build frames, mask, compute the handshake accept,
// validate UTF-8, and drive the connection state machine.
//
// Verification backing (see spec/ and proofs/):
//   - state machine        : TLA+  WsLifecycle  (INV1..INV6)
//   - masking / lengths /
//     opcode / close codes  : Lean  WsProto.Basic (P1..P7)
#ifndef WS_H
#define WS_H

#include <stddef.h>
#include <stdint.h>

// ---- opcodes (RFC 6455 §5.2) ----
typedef enum {
    WS_OP_CONT = 0x0,
    WS_OP_TEXT = 0x1,
    WS_OP_BIN = 0x2,
    WS_OP_CLOSE = 0x8,
    WS_OP_PING = 0x9,
    WS_OP_PONG = 0xA,
} ws_opcode;

typedef enum { WS_CLASS_DATA, WS_CLASS_CONTROL, WS_CLASS_RESERVED } ws_opclass;

ws_opclass ws_classify_opcode(uint8_t opcode); // Lean P5

// ---- masking (RFC 6455 §5.3) — Lean P1/P2 ----
// In place: out[i] = in[i] ^ key[i % 4]. Same call masks and unmasks.
void ws_mask(uint8_t *data, size_t len, const uint8_t key[4]);

// ---- frame header (RFC 6455 §5.2) ----
typedef struct {
    ws_opcode opcode;
    uint64_t payload_len;
    uint8_t mask_key[4];
    _Bool fin;
    _Bool masked;
} ws_frame_header;

// Parse a frame header from `buf` (len bytes available). Returns the header
// byte count consumed, 0 if more bytes are needed, or a negative ws_status on
// protocol error (e.g. control frame > 125, RFC §5.5 — Lean P6).
int ws_parse_header(const uint8_t *buf, size_t len, ws_frame_header *out);

// Serialize a frame header into `buf` (cap bytes). Returns bytes written or a
// negative ws_status if the buffer is too small. Length form chosen per
// RFC §5.2 boundaries (Lean P3/P4).
int ws_build_header(const ws_frame_header *h, uint8_t *buf, size_t cap);

// ---- close codes (RFC 6455 §7.4.1) — Lean P7 ----
_Bool ws_close_code_sendable(uint16_t code);

// ---- opening handshake (RFC 6455 §4.2) ----
// Compute Sec-WebSocket-Accept (28-byte base64, no NUL) from the client's
// Sec-WebSocket-Key. `out` must hold >= 28 bytes. Returns bytes written (28).
int ws_handshake_accept(const uint8_t *key, size_t key_len, uint8_t out[28]);

// ---- UTF-8 validation (RFC 6455 §8.1) — Lean P8 ----
_Bool ws_utf8_valid(const uint8_t *data, size_t len);

// ============================================================================
//  High-level sans-I/O driver
//  Feed received bytes with ws_conn_recv, drain semantic events with
//  ws_conn_poll, build outbound frames with ws_send_*. No I/O, no allocation:
//  the caller owns the socket and the message buffer. The connection lifecycle
//  (CONNECTING/OPEN/CLOSING/CLOSED) and fragment reassembly are verified in
//  TLA+ (spec/WsStream). The opening HTTP upgrade is the caller's job; use
//  ws_handshake_accept and start the driver once the socket is OPEN.
// ============================================================================

typedef enum { WS_ROLE_SERVER, WS_ROLE_CLIENT } ws_role;

typedef enum { WS_CONNECTING, WS_OPEN, WS_CLOSING, WS_CLOSED } ws_state;
typedef enum { WS_FRAG_NONE, WS_FRAG_TEXT, WS_FRAG_BIN } ws_frag;

// Semantic events surfaced by ws_conn_poll.
typedef enum {
    WS_EV_NONE = 0, // queue drained / more bytes needed
    WS_EV_MESSAGE,  // a complete TEXT or BIN message (data/len; opcode in 'op')
    WS_EV_PING,     // a ping (data/len = payload); reply with ws_send_pong
    WS_EV_PONG,     // a pong (informational)
    WS_EV_CLOSE,    // peer close (close_code set); reply with ws_send_close
    WS_EV_ERROR,    // protocol violation; the connection must be closed
} ws_event_type;

typedef struct {
    ws_event_type type;
    ws_opcode op;        // for WS_EV_MESSAGE: WS_OP_TEXT or WS_OP_BIN
    const uint8_t *data; // payload (into the caller's message buffer)
    size_t len;          // payload length
    uint16_t close_code; // for WS_EV_CLOSE (0 if peer sent none)
} ws_event;

// Connection state. Fields are internal — declared here only so the caller can
// stack-allocate it. Do not read/write them directly; use the functions.
typedef struct {
    ws_state state;
    ws_frag frag; // in-flight message type (WS_FRAG_NONE between msgs)
    ws_role role;
    _Bool sent_close;
    _Bool rcvd_close;
    uint8_t *msg_buf; // caller-owned: holds reassembled message + scratch
    size_t msg_cap;
    size_t msg_len;      // bytes of the in-flight message assembled so far
    size_t rx_len;       // unparsed raw bytes buffered after msg_len
    uint16_t close_code; // close code from the peer's CLOSE frame
    _Bool failed;        // a protocol error latched the connection to ERROR
} ws_conn;

// Initialize a server/client connection. msg_buf (msg_cap bytes) is borrowed
// for the connection's lifetime: it holds both the reassembled message and the
// raw receive scratch. WS_MAX_MESSAGE is a sensible default cap.
void ws_conn_init(ws_conn *c, ws_role role, uint8_t *msg_buf, size_t msg_cap);

// Mark the upgrade complete (CONNECTING -> OPEN). Call after the HTTP 101.
void ws_conn_open(ws_conn *c);

// Feed received bytes into the connection. Copies into msg_buf scratch. Returns
// the bytes accepted, or WS_ERR_TOO_SMALL if they would overflow msg_cap.
int ws_conn_recv(ws_conn *c, const uint8_t *bytes, size_t len);

// Pop the next semantic event. Returns ev->type (also the function result for
// convenience). WS_EV_NONE when no complete frame is buffered. Drives the
// verified state machine internally (close handshake, fragment assembly).
ws_event_type ws_conn_poll(ws_conn *c, ws_event *ev);

// ---- outbound frame builders (write into the caller's buffer) ----
// All return the number of bytes written into out, or 0 on failure (buffer too
// small, or illegal in the current state). Server frames are unmasked, client
// frames are masked (RFC 6455 §5.1); the mask key is drawn from `mask_key`.
size_t ws_send_message(ws_conn *c, ws_opcode op, const uint8_t *payload, size_t len, uint8_t *out,
                       size_t cap);
size_t ws_send_pong(ws_conn *c, const uint8_t *payload, size_t len, uint8_t *out, size_t cap);
size_t ws_send_close(ws_conn *c, uint16_t code, uint8_t *out, size_t cap);

// Client masking key source. Servers ignore it. The caller sets this to fresh
// random bytes per frame (the SDK does no I/O, so it cannot gather entropy).
extern uint8_t ws_client_mask_key[4];

#ifndef WS_MAX_MESSAGE
#define WS_MAX_MESSAGE 65536
#endif

// ---- status codes (negative returns) ----
typedef enum {
    WS_OK = 0,
    WS_ERR_NEED_MORE = -1,    // header incomplete
    WS_ERR_PROTOCOL = -2,     // RFC violation (control too big, bad opcode use)
    WS_ERR_TOO_SMALL = -3,    // output buffer too small
    WS_ERR_ILLEGAL_STATE = -4 // event not allowed in current state
} ws_status;

#endif
