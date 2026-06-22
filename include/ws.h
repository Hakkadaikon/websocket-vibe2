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

// ---- connection state machine (TLA+ WsLifecycle) ----
typedef enum { WS_CONNECTING, WS_OPEN, WS_CLOSING, WS_CLOSED } ws_state;
typedef enum { WS_FRAG_NONE, WS_FRAG_TEXT, WS_FRAG_BIN } ws_frag;

typedef struct {
    ws_state state;
    ws_frag frag;
    _Bool sent_close;
    _Bool rcvd_close;
} ws_conn;

// Events driving the machine (one per TLA+ Next disjunct).
typedef enum {
    WS_EV_HANDSHAKE,   // CONNECTING -> OPEN
    WS_EV_SEND_CLOSE,  // OPEN -> CLOSING
    WS_EV_RECV_CLOSE,  // -> CLOSED (reply-on-receive)
    WS_EV_START_FRAG,  // begin fragmented data message
    WS_EV_FINISH_FRAG, // FIN=1 completes message
} ws_event;

void ws_conn_init(ws_conn *c);
// Apply an event. Returns 0 on a legal transition, negative ws_status if the
// event is illegal in the current state (e.g. a frame while CONNECTING/CLOSED).
int ws_conn_step(ws_conn *c, ws_event ev, ws_frag start_type);

// ---- status codes (negative returns) ----
typedef enum {
    WS_OK = 0,
    WS_ERR_NEED_MORE = -1,    // header incomplete
    WS_ERR_PROTOCOL = -2,     // RFC violation (control too big, bad opcode use)
    WS_ERR_TOO_SMALL = -3,    // output buffer too small
    WS_ERR_ILLEGAL_STATE = -4 // event not allowed in current state
} ws_status;

#endif
