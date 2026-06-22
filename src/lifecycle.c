// Connection lifecycle state machine. Each handler maps 1:1 to a disjunct of
// Next in spec/WsLifecycle.tla; the dispatch table mirrors the event enum.
// Invariants INV1 (CLOSED iff both close bits) and INV3 (CLOSED => frag NONE)
// are preserved by the RecvClose handler and checked in the bridge tests.
#include "ws.h"

void ws_conn_init(ws_conn *c) {
    c->state = WS_CONNECTING;
    c->frag = WS_FRAG_NONE;
    c->sent_close = false;
    c->rcvd_close = false;
}

// Active == state in {OPEN, CLOSING} (frames processable). TLA+ `Active`.
static _Bool active(const ws_conn *c) {
    return (_Bool)(c->state == WS_OPEN || c->state == WS_CLOSING);
}

// Handshake: CONNECTING -> OPEN.
static int ev_handshake(ws_conn *c, ws_frag start_type) {
    (void)start_type;
    if (c->state != WS_CONNECTING) {
        return WS_ERR_ILLEGAL_STATE;
    }
    c->state = WS_OPEN;
    return WS_OK;
}

// SendClose: OPEN -> CLOSING, sent_close set (INV5).
static int ev_send_close(ws_conn *c, ws_frag start_type) {
    (void)start_type;
    if (c->state != WS_OPEN) {
        return WS_ERR_ILLEGAL_STATE;
    }
    c->sent_close = true;
    c->state = WS_CLOSING;
    return WS_OK;
}

// RecvClose: Active -> CLOSED, both bits set (reply-on-receive), frag discarded
// (R6a). This is the only transition reaching CLOSED, so INV1/INV3 hold.
static int ev_recv_close(ws_conn *c, ws_frag start_type) {
    (void)start_type;
    if (!active(c)) {
        return WS_ERR_ILLEGAL_STATE;
    }
    c->rcvd_close = true;
    c->sent_close = true;
    c->state = WS_CLOSED;
    c->frag = WS_FRAG_NONE;
    return WS_OK;
}

// StartFrag: Active && frag == NONE -> frag = start_type.
static int ev_start_frag(ws_conn *c, ws_frag start_type) {
    if (!active(c)) {
        return WS_ERR_ILLEGAL_STATE;
    }
    if (c->frag != WS_FRAG_NONE) {
        return WS_ERR_ILLEGAL_STATE;
    }
    c->frag = start_type;
    return WS_OK;
}

// FinishFrag: Active && frag != NONE -> frag = NONE.
static int ev_finish_frag(ws_conn *c, ws_frag start_type) {
    (void)start_type;
    if (!active(c)) {
        return WS_ERR_ILLEGAL_STATE;
    }
    if (c->frag == WS_FRAG_NONE) {
        return WS_ERR_ILLEGAL_STATE;
    }
    c->frag = WS_FRAG_NONE;
    return WS_OK;
}

typedef int (*ev_handler)(ws_conn *, ws_frag);

// Indexed by ws_event (one entry per Next disjunct).
static const ev_handler kHandlers[] = {
    [WS_EV_HANDSHAKE] = ev_handshake,     [WS_EV_SEND_CLOSE] = ev_send_close,
    [WS_EV_RECV_CLOSE] = ev_recv_close,   [WS_EV_START_FRAG] = ev_start_frag,
    [WS_EV_FINISH_FRAG] = ev_finish_frag,
};

int ws_conn_step(ws_conn *c, ws_event ev, ws_frag start_type) {
    return kHandlers[ev](c, start_type);
}
