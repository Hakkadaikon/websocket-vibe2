// lifecycle state machine (TLA+ WsLifecycle), driven via the internal ws_lc_*
// action helpers (1:1 with the spec's Next disjuncts).
// #include'd by test/test.c (single TU; ws_internal.h + harness already in scope).

static uint8_t lc_buf[64];
static void lc_init(ws_conn *c) {
    ws_conn_init(c, WS_ROLE_SERVER, lc_buf, sizeof lc_buf);
}

static void test_lc_init(void) {
    ws_conn c;
    lc_init(&c);
    CHECK(c.state == WS_CONNECTING, "init state CONNECTING");
    CHECK(c.frag == WS_FRAG_NONE, "init frag NONE");
    CHECK(!c.sent_close && !c.rcvd_close, "init close bits clear");
}

// INV1: CLOSED iff (sent && rcvd). INV3: CLOSED implies frag NONE.
static _Bool inv_holds(const ws_conn *c) {
    _Bool both = c->sent_close;
    if (!c->rcvd_close) {
        both = false;
    }
    _Bool closed = (_Bool)(c->state == WS_CLOSED);
    if (closed != both) {
        return false; // INV1 violated
    }
    if (closed) {
        return (_Bool)(c->frag == WS_FRAG_NONE); // INV3
    }
    return true;
}

static void test_lc_handshake(void) {
    ws_conn c;
    lc_init(&c);
    CHECK(ws_lc_handshake(&c) == WS_OK, "handshake legal");
    CHECK(c.state == WS_OPEN, "handshake -> OPEN");
    CHECK(inv_holds(&c), "INV after handshake");
    CHECK(ws_lc_handshake(&c) == WS_ERR_ILLEGAL_STATE, "handshake from OPEN illegal");
}

static void test_lc_send_close(void) {
    ws_conn c;
    lc_init(&c);
    CHECK(ws_lc_send_close(&c) == WS_ERR_ILLEGAL_STATE, "send-close while CONNECTING illegal");
    ws_lc_handshake(&c);
    CHECK(ws_lc_send_close(&c) == WS_OK, "send-close from OPEN legal");
    CHECK(c.state == WS_CLOSING && c.sent_close, "send-close -> CLOSING, sent");
    CHECK(inv_holds(&c), "INV after send-close");
}

static void test_lc_recv_close(void) {
    ws_conn c;
    lc_init(&c);
    ws_lc_handshake(&c);
    ws_lc_start_frag(&c, WS_FRAG_TEXT); // in-flight fragment
    CHECK(ws_lc_recv_close(&c) == WS_OK, "recv-close from OPEN legal");
    CHECK(c.state == WS_CLOSED, "recv-close -> CLOSED");
    CHECK(c.sent_close && c.rcvd_close, "recv-close sets both bits (reply-on-receive)");
    CHECK(c.frag == WS_FRAG_NONE, "recv-close discards fragment (R6a)");
    CHECK(inv_holds(&c), "INV after recv-close");
    CHECK(ws_lc_start_frag(&c, WS_FRAG_BIN) == WS_ERR_ILLEGAL_STATE,
          "start-frag after CLOSED illegal");
}

static void test_lc_frag(void) {
    ws_conn c;
    lc_init(&c);
    ws_lc_handshake(&c);
    CHECK(ws_lc_start_frag(&c, WS_FRAG_BIN) == WS_OK, "start-frag legal");
    CHECK(c.frag == WS_FRAG_BIN, "start-frag sets type");
    CHECK(ws_lc_start_frag(&c, WS_FRAG_TEXT) == WS_ERR_ILLEGAL_STATE,
          "start-frag while fragmenting illegal");
    CHECK(ws_lc_finish_frag(&c) == WS_OK, "finish-frag legal");
    CHECK(c.frag == WS_FRAG_NONE, "finish-frag clears type");
    CHECK(ws_lc_finish_frag(&c) == WS_ERR_ILLEGAL_STATE, "finish-frag with no fragment illegal");
    ws_conn c2;
    lc_init(&c2);
    CHECK(ws_lc_start_frag(&c2, WS_FRAG_TEXT) == WS_ERR_ILLEGAL_STATE,
          "start-frag while CONNECTING illegal");
}
