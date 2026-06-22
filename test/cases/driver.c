// driver: ws_conn_recv / ws_conn_poll / ws_send_* (TLA+ WsStream).
// #include'd by test/test.c (single TU; ws/ws.h + harness already in scope).

// Build a frame into buf and return its length. masked => client->server form.
static const uint8_t kKey[4] = {0x11, 0x22, 0x33, 0x44};
static size_t mk_frame(uint8_t *buf, _Bool fin, ws_opcode op, _Bool masked, const uint8_t *pay,
                       size_t plen) {
    ws_frame_header h = {.opcode = op, .payload_len = plen, .fin = fin, .masked = masked};
    if (masked) {
        ws_memcpy(h.mask_key, kKey, 4);
    }
    int hn = ws_build_header(&h, buf, 32);
    ws_memcpy(buf + hn, pay, plen);
    if (masked) {
        ws_mask(buf + hn, plen, kKey);
    }
    return (size_t)hn + plen;
}

// A server connection driven to OPEN over a fresh 256-byte message buffer.
static void open_server(ws_conn *c, uint8_t *mb, size_t cap) {
    ws_conn_init(c, WS_ROLE_SERVER, mb, cap);
    ws_conn_open(c);
}

static void test_drv_single_text(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t f[32];
    size_t n = mk_frame(f, true, WS_OP_TEXT, true, (const uint8_t *)"hello", 5);
    CHECK(ws_conn_recv(&c, f, n) == (int)n, "recv accepts frame");
    ws_event ev;
    CHECK(ws_conn_poll(&c, &ev) == WS_EV_MESSAGE, "single TEXT -> MESSAGE");
    CHECK(ev.op == WS_OP_TEXT, "op TEXT");
    CHECK(ev.len == 5 && ws_memcmp(ev.data, "hello", 5) == 0, "payload hello");
    CHECK(ws_conn_poll(&c, &ev) == WS_EV_NONE, "drained");
    CHECK(c.frag == WS_FRAG_NONE && c.msg_len == 0, "SINV5 boundary after MESSAGE");
}

static void test_drv_incremental(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t f[32];
    size_t n = mk_frame(f, true, WS_OP_TEXT, true, (const uint8_t *)"hi", 2);
    ws_event ev;
    ws_conn_recv(&c, f, 1); // header byte only
    CHECK(ws_conn_poll(&c, &ev) == WS_EV_NONE, "S3: header incomplete -> NONE");
    ws_conn_recv(&c, f + 1, n - 1);
    CHECK(ws_conn_poll(&c, &ev) == WS_EV_MESSAGE, "rest arrives -> MESSAGE");
    CHECK(ev.len == 2 && ws_memcmp(ev.data, "hi", 2) == 0, "incremental payload");
}

static void test_drv_fragmented(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t f[32];
    size_t n1 = mk_frame(f, false, WS_OP_TEXT, true, (const uint8_t *)"ab", 2);
    ws_conn_recv(&c, f, n1);
    ws_event ev;
    CHECK(ws_conn_poll(&c, &ev) == WS_EV_NONE, "S4: non-final fragment -> NONE");
    CHECK(c.frag == WS_FRAG_TEXT && c.msg_len == 2, "fragment accumulating");
    size_t n2 = mk_frame(f, true, WS_OP_CONT, true, (const uint8_t *)"cd", 2);
    ws_conn_recv(&c, f, n2);
    CHECK(ws_conn_poll(&c, &ev) == WS_EV_MESSAGE, "S6: final CONT -> MESSAGE");
    CHECK(ev.len == 4 && ws_memcmp(ev.data, "abcd", 4) == 0, "fragments joined");
    CHECK(c.frag == WS_FRAG_NONE && c.msg_len == 0, "SINV5 reset after join");
}

static void test_drv_ping(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t f[32];
    size_t n = mk_frame(f, true, WS_OP_PING, true, (const uint8_t *)"pq", 2);
    ws_conn_recv(&c, f, n);
    ws_event ev;
    CHECK(ws_conn_poll(&c, &ev) == WS_EV_PING, "PING event");
    CHECK(ev.len == 2 && ws_memcmp(ev.data, "pq", 2) == 0, "ping payload");
    CHECK(c.frag == WS_FRAG_NONE && c.msg_len == 0, "S10: ping leaves frag/msg untouched");
}

static void test_drv_close(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t body[2] = {0x03, 0xE8}; // 1000
    uint8_t f[32];
    size_t n = mk_frame(f, true, WS_OP_CLOSE, true, body, 2);
    ws_conn_recv(&c, f, n);
    ws_event ev;
    CHECK(ws_conn_poll(&c, &ev) == WS_EV_CLOSE, "CLOSE event");
    CHECK(ev.close_code == 1000, "close code 1000");
    CHECK(c.state == WS_CLOSED, "S11: lifecycle reaches CLOSED");
}

// After any error, poll stays latched on ERROR and assembly is reset (SINV8).
static void check_latched(ws_conn *c) {
    ws_event ev;
    CHECK(ws_conn_poll(c, &ev) == WS_EV_ERROR, "error path -> ERROR");
    CHECK(c->failed, "SINV7: failed latched on ERROR");
    CHECK(c->frag == WS_FRAG_NONE, "SINV8: error resets frag");
    CHECK(c->msg_len == 0, "SINV8: error resets msg_len");
}

static void test_drv_interleave(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t f[32];
    size_t n1 = mk_frame(f, false, WS_OP_TEXT, true, (const uint8_t *)"ab", 2);
    ws_conn_recv(&c, f, n1);
    ws_event ev;
    ws_conn_poll(&c, &ev);                                                    // accumulating
    size_t n2 = mk_frame(f, true, WS_OP_TEXT, true, (const uint8_t *)"x", 1); // new data mid-frag
    ws_conn_recv(&c, f, n2);
    check_latched(&c);
    check_latched(&c);
}

static void test_drv_stray_cont(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t f[32];
    size_t n = mk_frame(f, true, WS_OP_CONT, true, (const uint8_t *)"x", 1);
    ws_conn_recv(&c, f, n);
    check_latched(&c);
}

// S2/SINV1: a frame that would overflow msg_cap is rejected at recv (the binding
// overflow guard; with the co-located buffer, poll's defensive S9 is unreachable
// because the frame can't be buffered in the first place).
static void test_drv_too_big(void) {
    ws_conn c;
    uint8_t mb[8];
    ws_conn_init(&c, WS_ROLE_SERVER, mb, sizeof mb); // tiny cap
    ws_conn_open(&c);
    uint8_t f[32];
    size_t n = mk_frame(f, true, WS_OP_TEXT, true, (const uint8_t *)"hello", 5); // 11-byte frame
    CHECK(ws_conn_recv(&c, f, n) == WS_ERR_TOO_SMALL, "S2: overflow rejected at recv");
    CHECK(c.rx_len == 0, "S2: rx_len unchanged on reject");
}

static void test_drv_unmasked_server(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t f[32];
    size_t n = mk_frame(f, true, WS_OP_TEXT, false, (const uint8_t *)"hi", 2); // unmasked
    ws_conn_recv(&c, f, n);
    check_latched(&c);
}

static void test_drv_illegal_state(void) {
    ws_conn c;
    uint8_t mb[256];
    ws_conn_init(&c, WS_ROLE_SERVER, mb, sizeof mb); // still CONNECTING
    uint8_t f[32];
    size_t n = mk_frame(f, true, WS_OP_TEXT, true, (const uint8_t *)"hi", 2);
    ws_conn_recv(&c, f, n);
    check_latched(&c);
}

static void test_drv_bad_utf8(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t bad[2] = {0xC0, 0x80}; // overlong
    uint8_t f[32];
    size_t n = mk_frame(f, true, WS_OP_TEXT, true, bad, 2);
    ws_conn_recv(&c, f, n);
    check_latched(&c);
}

// Server builds an unmasked frame; parse it back and compare payload (roundtrip).
static void test_drv_send_message(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t out[64];
    size_t n = ws_send_message(&c, WS_OP_TEXT, (const uint8_t *)"echo", 4, out, sizeof out);
    CHECK(n == 6, "send_message TEXT 4 -> 6 bytes (unmasked)");
    ws_frame_header h;
    int hn = ws_parse_header(out, n, &h);
    CHECK(hn == 2 && !h.masked && h.opcode == WS_OP_TEXT, "built header unmasked TEXT");
    CHECK(h.payload_len == 4 && ws_memcmp(out + hn, "echo", 4) == 0, "built payload echo");
}

static void test_drv_send_close(void) {
    ws_conn c;
    uint8_t mb[256];
    open_server(&c, mb, sizeof mb);
    uint8_t out[64];
    size_t n = ws_send_close(&c, 1000, out, sizeof out);
    CHECK(n == 4, "send_close -> 4 bytes (2 hdr + 2 code)");
    CHECK(c.state == WS_CLOSING, "send_close drives OPEN -> CLOSING");
    ws_frame_header h;
    int hn = ws_parse_header(out, n, &h);
    CHECK(hn == 2 && h.opcode == WS_OP_CLOSE, "close frame header");
    CHECK((out[hn] << 8 | out[hn + 1]) == 1000, "close code 1000 on wire");
}
