// Connection lifecycle state machine. Each handler maps 1:1 to a disjunct of
// Next in spec/WsLifecycle.tla. Invariants INV1 (CLOSED iff both close bits)
// and INV3 (CLOSED => frag NONE) are preserved by the RecvClose handler.
// These are internal: the driver (stream.c) drives them; the old public
// ws_conn_step / transition-event enum are gone (see ws_internal.h).
#include "ws_internal.h"

// Active == state in {OPEN, CLOSING} (frames processable). TLA+ `Active`.
_Bool ws_lc_active(const ws_conn *c) {
    return (_Bool)(c->state == WS_OPEN || c->state == WS_CLOSING);
}

// Handshake: CONNECTING -> OPEN.
int ws_lc_handshake(ws_conn *c) {
    if (c->state != WS_CONNECTING) {
        return WS_ERR_ILLEGAL_STATE;
    }
    int from = c->state;
    c->state = WS_OPEN;
    WS_TRACE_STATE(from, c->state, "handshake");
    return WS_OK;
}

// SendClose: OPEN -> CLOSING, sent_close set (INV5).
int ws_lc_send_close(ws_conn *c) {
    if (c->state != WS_OPEN) {
        return WS_ERR_ILLEGAL_STATE;
    }
    int from = c->state;
    c->sent_close = true;
    c->state = WS_CLOSING;
    WS_TRACE_STATE(from, c->state, "send_close");
    return WS_OK;
}

// RecvClose: Active -> CLOSED, both bits set (reply-on-receive), frag discarded
// (R6a). This is the only transition reaching CLOSED, so INV1/INV3 hold.
int ws_lc_recv_close(ws_conn *c) {
    if (!ws_lc_active(c)) {
        return WS_ERR_ILLEGAL_STATE;
    }
    int from = c->state;
    c->rcvd_close = true;
    c->sent_close = true;
    c->state = WS_CLOSED;
    c->frag = WS_FRAG_NONE;
    c->msg_len = 0; // R6a: discard in-flight assembly (SINV2/SINV8).
    WS_TRACE_STATE(from, c->state, "recv_close");
    return WS_OK;
}

// StartFrag: Active && frag == NONE -> frag = start_type.
int ws_lc_start_frag(ws_conn *c, ws_frag start_type) {
    if (!ws_lc_active(c)) {
        return WS_ERR_ILLEGAL_STATE;
    }
    if (c->frag != WS_FRAG_NONE) {
        return WS_ERR_ILLEGAL_STATE;
    }
    c->frag = start_type;
    return WS_OK;
}

// FinishFrag: Active && frag != NONE -> frag = NONE.
int ws_lc_finish_frag(ws_conn *c) {
    if (!ws_lc_active(c)) {
        return WS_ERR_ILLEGAL_STATE;
    }
    if (c->frag == WS_FRAG_NONE) {
        return WS_ERR_ILLEGAL_STATE;
    }
    c->frag = WS_FRAG_NONE;
    return WS_OK;
}
