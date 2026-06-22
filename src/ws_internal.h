// Internal declarations shared across SDK translation units (freestanding).
#ifndef WS_INTERNAL_H
#define WS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

void *ws_memcpy(void *dst, const void *src, size_t n);
void *ws_memset(void *dst, int c, size_t n);
int ws_memcmp(const void *a, const void *b, size_t n);

void ws_sha1(const uint8_t *msg, size_t len, uint8_t out[20]);
size_t ws_base64(const uint8_t *in, size_t len, uint8_t *out);

// ---- internal lifecycle steps (formerly the public ws_conn_step) ----
// Each maps 1:1 to a disjunct of Next in spec/WsLifecycle.tla. The driver
// (stream.c) calls these; INV1 (CLOSED iff both close bits) and INV3 (CLOSED
// => frag NONE) live here. Return WS_OK or WS_ERR_ILLEGAL_STATE.
#include "ws/ws.h"

_Bool ws_lc_active(const ws_conn *c);       // state in {OPEN, CLOSING}
int ws_lc_handshake(ws_conn *c);            // CONNECTING -> OPEN
int ws_lc_send_close(ws_conn *c);           // OPEN -> CLOSING, sent_close
int ws_lc_recv_close(ws_conn *c);           // Active -> CLOSED, both bits, frag discard (R6a)
int ws_lc_start_frag(ws_conn *c, ws_frag start_type); // Active && frag==NONE -> frag=type
int ws_lc_finish_frag(ws_conn *c);          // Active && frag!=NONE -> frag=NONE

#endif
