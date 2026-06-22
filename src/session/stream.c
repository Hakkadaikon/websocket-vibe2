// High-level sans-I/O driver (ws_conn_recv / ws_conn_poll / ws_send_*).
// Source of truth: spec/WsStream.tla (S1..S14, SINV1..SINV8). The byte core
// (parse/build/mask/utf8/close-code) is Lean-verified (P1..P8); this layer is
// the deterministic state routing (S12) on top of WsLifecycle.
//
// Buffer layout in msg_buf: [0, msg_len) = reassembled message so far,
// [msg_len, msg_len+rx_len) = unparsed raw receive bytes. Invariant SINV1:
// msg_len + rx_len <= msg_cap always.
#include "../ws_internal.h"

uint8_t ws_client_mask_key[4]; // caller fills with fresh random bytes per frame.

void ws_conn_init(ws_conn *c, ws_role role, uint8_t *msg_buf, size_t msg_cap) {
    c->state = WS_CONNECTING;
    c->frag = WS_FRAG_NONE;
    c->role = role;
    c->sent_close = false;
    c->rcvd_close = false;
    c->msg_buf = msg_buf;
    c->msg_cap = msg_cap;
    c->msg_len = 0;
    c->rx_len = 0;
    c->close_code = 0;
    c->failed = false;
}

void ws_conn_open(ws_conn *c) {
    (void)ws_lc_handshake(c);
}

// S1/S2/SINV1: append raw bytes after the assembled message, rejecting overflow.
int ws_conn_recv(ws_conn *c, const uint8_t *bytes, size_t len) {
    if (c->msg_len + c->rx_len + len > c->msg_cap) {
        return WS_ERR_TOO_SMALL; // S2: reject, rx_len unchanged.
    }
    ws_memcpy(c->msg_buf + c->msg_len + c->rx_len, bytes, len);
    c->rx_len += len;
    return (int)len;
}

// Forward byte copy, overlap-safe only when dst < src (no memmove freestanding).
static void fwd_copy(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

// Raw region start (first unparsed byte) and its length live at msg_len.
static uint8_t *raw(ws_conn *c) {
    return c->msg_buf + c->msg_len;
}

// Drop `take` bytes from the front of the raw region, sliding the rest down to
// msg_len. dst (msg_len) < src (msg_len+take), so forward copy is safe.
static void raw_consume(ws_conn *c, size_t take) {
    fwd_copy(raw(c), raw(c) + take, c->rx_len - take);
    c->rx_len -= take;
}

// Latch a protocol violation: set failed and reset assembly to the message
// boundary (S9a / SINV8). One place keeps the invariant un-missable.
static ws_event_type fail(ws_conn *c, ws_event *ev) {
    c->failed = true;
    c->frag = WS_FRAG_NONE;
    c->msg_len = 0;
    ev->type = WS_EV_ERROR;
    WS_TRACE_EVENT(WS_EV_ERROR, 0, 0);
    return WS_EV_ERROR;
}

static ws_event_type emit(ws_event *ev, ws_event_type t) {
    ev->type = t;
    // NONE means "nothing to report"; trace only real events to avoid noise.
    if (t != WS_EV_NONE) {
        WS_TRACE_EVENT(t, ev->len, ev->close_code);
    }
    return t;
}

// Is the buffered frame complete? Returns total frame bytes (hdr+payload) via
// *total, or 0 if more bytes are needed / the header is malformed (NONE).
static _Bool frame_ready(ws_conn *c, ws_frame_header *h, size_t *hdr, size_t *total) {
    int hn = ws_parse_header(raw(c), c->rx_len, h);
    if (hn <= 0) {
        return false; // NEED_MORE or protocol error in header -> treat as not-ready/none
    }
    *hdr = (size_t)hn;
    *total = (size_t)hn + (size_t)h->payload_len;
    return (_Bool)(c->rx_len >= *total);
}

// Unmask a frame's payload in place (server receives masked, client unmasked).
static void unmask_payload(ws_conn *c, const ws_frame_header *h, size_t hdr) {
    if (h->masked) {
        ws_mask(raw(c) + hdr, (size_t)h->payload_len, h->mask_key);
    }
}

// Append this data frame's payload to the assembled message, then consume the
// whole frame from the raw region. msg_len < raw positions, so forward-safe.
static void accumulate(ws_conn *c, const ws_frame_header *h, size_t hdr) {
    size_t plen = (size_t)h->payload_len;
    fwd_copy(raw(c), raw(c) + hdr, plen); // payload down by hdr (into msg tail)
    fwd_copy(raw(c) + plen, raw(c) + hdr + plen, c->rx_len - hdr - plen); // raw tail down
    c->rx_len -= hdr; // header bytes are gone; payload now belongs to the message
    c->msg_len += plen;
    c->rx_len -= plen;
}

// TEXT messages must be valid UTF-8 (RFC 6455 §8.1, MUST). Returns true if ok.
static _Bool message_utf8_ok(const ws_conn *c) {
    if (c->frag != WS_FRAG_TEXT) {
        return true;
    }
    return ws_utf8_valid(c->msg_buf, c->msg_len);
}

// Finish a data frame (FIN): emit MESSAGE with the assembled payload, reset to
// the message boundary (S6 / SINV5). UTF-8 failure -> ERROR (close 1007).
static ws_event_type finish_message(ws_conn *c, ws_event *ev) {
    if (!message_utf8_ok(c)) {
        return fail(c, ev);
    }
    ev->op = (c->frag == WS_FRAG_BIN) ? WS_OP_BIN : WS_OP_TEXT;
    ev->data = c->msg_buf;
    ev->len = c->msg_len;
    (void)ws_lc_finish_frag(c); // WsLifecycle FinishFrag: frag -> NONE (read op above first)
    c->msg_len = 0;
    return emit(ev, WS_EV_MESSAGE);
}

// S7 interleave: a new (non-continuation) data frame arrived mid-fragment.
static _Bool is_interleave(const ws_conn *c, _Bool is_cont) {
    if (is_cont) {
        return false;
    }
    return (_Bool)(c->frag != WS_FRAG_NONE);
}

// S8 stray continuation: a CONT frame arrived with no fragment in flight.
static _Bool is_stray_cont(const ws_conn *c, _Bool is_cont) {
    if (!is_cont) {
        return false;
    }
    return (_Bool)(c->frag == WS_FRAG_NONE);
}

// Any of S7/S8/S9 -> this data frame is a protocol violation.
static _Bool data_illegal(const ws_conn *c, const ws_frame_header *h, _Bool is_cont) {
    if (is_stray_cont(c, is_cont)) {
        return true; // S8
    }
    if (is_interleave(c, is_cont)) {
        return true; // S7
    }
    return (_Bool)(c->msg_len + (size_t)h->payload_len > c->msg_cap); // S9 too big
}

// Begin a new data message: set frag from the opcode (S4). CONT keeps frag.
// Drives the verified lifecycle action (WsLifecycle StartFrag) rather than
// assigning frag directly, so the TLA+ guard (Active && frag==NONE) is enforced
// in one place. data_illegal already ruled out interleave, so this succeeds.
static void begin_data(ws_conn *c, const ws_frame_header *h, _Bool is_cont) {
    if (is_cont) {
        return;
    }
    ws_frag type = (h->opcode == WS_OP_BIN) ? WS_FRAG_BIN : WS_FRAG_TEXT;
    (void)ws_lc_start_frag(c, type);
}

// A data frame (TEXT/BIN/CONT). Routes S4-S9 on (opcode, frag, fit).
static ws_event_type on_data(ws_conn *c, const ws_frame_header *h, size_t hdr, ws_event *ev) {
    _Bool is_cont = (_Bool)(h->opcode == WS_OP_CONT);
    if (data_illegal(c, h, is_cont)) {
        return fail(c, ev); // S7/S8/S9
    }
    begin_data(c, h, is_cont); // S4
    accumulate(c, h, hdr);     // S4/S5: append payload, consume frame
    if (!h->fin) {
        return emit(ev, WS_EV_NONE); // S3/S5: message incomplete
    }
    return finish_message(c, ev); // S6
}

// A code+reason close body (plen >= 2): the code must be sendable (§7.4.1) and
// the reason must be valid UTF-8 (§5.5.1).
static _Bool close_code_reason_ok(ws_conn *c, size_t hdr, size_t plen) {
    uint16_t code = (uint16_t)((raw(c)[hdr] << 8) | raw(c)[hdr + 1]);
    if (!ws_close_code_sendable(code)) {
        return false; // §7.4.1: reserved/out-of-range code
    }
    return ws_utf8_valid(raw(c) + hdr + 2, plen - 2); // §5.5.1: reason must be valid UTF-8
}

// A CLOSE frame body is either empty, or a 2-byte code optionally followed by a
// UTF-8 reason (RFC §5.5.1). A 1-byte body is malformed.
static _Bool close_body_ok(ws_conn *c, const ws_frame_header *h, size_t hdr) {
    size_t plen = (size_t)h->payload_len;
    if (plen == 0) {
        return true; // no code: allowed
    }
    if (plen == 1) {
        return false; // RFC §5.5.1: a 1-byte close payload is invalid
    }
    return close_code_reason_ok(c, hdr, plen);
}

// CLOSE: validate the body, capture the peer's code (if any), drive the
// lifecycle (R6a), emit (S11). A malformed body fails the connection (1007/1002).
static ws_event_type on_close(ws_conn *c, const ws_frame_header *h, size_t hdr, ws_event *ev) {
    if (!close_body_ok(c, h, hdr)) {
        return fail(c, ev);
    }
    size_t plen = (size_t)h->payload_len;
    if (plen >= 2) {
        c->close_code = (uint16_t)((raw(c)[hdr] << 8) | raw(c)[hdr + 1]);
    }
    ev->close_code = c->close_code;
    (void)ws_lc_recv_close(c); // Active -> CLOSED, R6a discards frag
    raw_consume(c, hdr + plen);
    return emit(ev, WS_EV_CLOSE);
}

// A control frame (CLOSE/PING/PONG). frag/msg_len untouched except CLOSE's R6a.
static ws_event_type on_control(ws_conn *c, const ws_frame_header *h, size_t hdr, ws_event *ev) {
    if (h->opcode == WS_OP_CLOSE) {
        return on_close(c, h, hdr, ev);
    }
    // ponytail: ev->data points into the raw region and stays valid until the
    // next poll/recv (same contract as WS_EV_MESSAGE). Consuming slides later
    // buffered frames over it; with pipelined frames copy it out before polling.
    ev->data = raw(c) + hdr;
    ev->len = (size_t)h->payload_len;
    ws_event_type t = (h->opcode == WS_OP_PING) ? WS_EV_PING : WS_EV_PONG;
    raw_consume(c, hdr + (size_t)h->payload_len);
    return emit(ev, t);
}

// Route a complete, validated frame to its single event (S12 determinism).
static ws_event_type route(ws_conn *c, const ws_frame_header *h, size_t hdr, ws_event *ev) {
    ws_opclass cls = ws_classify_opcode((uint8_t)h->opcode);
    if (cls == WS_CLASS_CONTROL) {
        return on_control(c, h, hdr, ev);
    }
    if (cls == WS_CLASS_DATA) {
        return on_data(c, h, hdr, ev);
    }
    return fail(c, ev); // reserved opcode
}

// A server must receive masked frames (RFC 5.1); a client receives unmasked.
static _Bool mask_ok(const ws_conn *c, const ws_frame_header *h) {
    if (c->role != WS_ROLE_SERVER) {
        return true;
    }
    return h->masked;
}

// Guards that must hold before routing: state Active, and the mask side is
// correct for the role. Returns true to proceed, else sets *out to ERROR.
static _Bool admit(ws_conn *c, const ws_frame_header *h, ws_event *ev, ws_event_type *out) {
    if (!ws_lc_active(c)) {
        *out = fail(c, ev); // S14 illegal state (CONNECTING/CLOSED)
        return false;
    }
    if (!mask_ok(c, h)) {
        *out = fail(c, ev); // RFC 5.1 mask violation
        return false;
    }
    return true;
}

static void clear_event(ws_event *ev) {
    ev->type = WS_EV_NONE;
    ev->op = WS_OP_TEXT;
    ev->data = NULL;
    ev->len = 0;
    ev->close_code = 0;
}

// Dispatch a complete, buffered frame (admit -> unmask -> route). S12.
static ws_event_type dispatch(ws_conn *c, ws_frame_header *h, size_t hdr, ws_event *ev) {
    ws_event_type out;
    if (!admit(c, h, ev, &out)) {
        return out;
    }
    unmask_payload(c, h, hdr);
    return route(c, h, hdr, ev);
}

ws_event_type ws_conn_poll(ws_conn *c, ws_event *ev) {
    clear_event(ev);
    if (c->failed) {
        return emit(ev, WS_EV_ERROR); // S13 latched
    }
    ws_frame_header h;
    size_t hdr;
    size_t total;
    if (!frame_ready(c, &h, &hdr, &total)) {
        return WS_EV_NONE; // S3: incomplete frame
    }
    return dispatch(c, &h, hdr, ev);
}

// ---- outbound frame builders ----

// Fill the outbound header, masking client frames (RFC 5.1).
static void out_header(const ws_conn *c, ws_frame_header *h) {
    if (c->role == WS_ROLE_CLIENT) {
        h->masked = true;
        ws_memcpy(h->mask_key, ws_client_mask_key, 4);
    }
}

// Does a header of hn bytes + len payload fit in cap? (hn<0 means build failed.)
static _Bool frame_fits(int hn, size_t len, size_t cap) {
    if (hn < 0) {
        return false;
    }
    return (_Bool)(cap - (size_t)hn >= len);
}

// Build header + (client) mask + payload into out. Returns total bytes or 0.
static size_t build_frame(const ws_conn *c, ws_opcode op, const uint8_t *payload, size_t len,
                          uint8_t *out, size_t cap) {
    ws_frame_header h = {.opcode = op, .payload_len = len, .fin = true, .masked = false};
    out_header(c, &h);
    int hn = ws_build_header(&h, out, cap);
    if (!frame_fits(hn, len, cap)) {
        return 0;
    }
    ws_memcpy(out + hn, payload, len);
    if (h.masked) {
        ws_mask(out + hn, len, h.mask_key);
    }
    return (size_t)hn + len;
}

size_t ws_send_message(ws_conn *c, ws_opcode op, const uint8_t *payload, size_t len, uint8_t *out,
                       size_t cap) {
    if (!ws_lc_active(c)) {
        return 0;
    }
    return build_frame(c, op, payload, len, out, cap);
}

size_t ws_send_pong(ws_conn *c, const uint8_t *payload, size_t len, uint8_t *out, size_t cap) {
    if (!ws_lc_active(c)) {
        return 0;
    }
    return build_frame(c, WS_OP_PONG, payload, len, out, cap);
}

size_t ws_send_close(ws_conn *c, uint16_t code, uint8_t *out, size_t cap) {
    if (c->state != WS_OPEN) {
        return 0;
    }
    uint8_t body[2] = {(uint8_t)(code >> 8), (uint8_t)code};
    size_t n = build_frame(c, WS_OP_CLOSE, body, 2, out, cap);
    if (n > 0) {
        (void)ws_lc_send_close(c);
    }
    return n;
}
