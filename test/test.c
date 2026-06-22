#include "../src/ws_internal.h"
#include "ws/ws.h"

#include "harness.h"

// ---- mem ----
static void test_mem(void) {
    unsigned char a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char b[8];
    ws_memset(b, 0, sizeof b);
    CHECK(b[0] == 0 && b[7] == 0, "memset zeroes");
    ws_memcpy(b, a, sizeof a);
    CHECK(ws_memcmp(a, b, sizeof a) == 0, "memcpy then memcmp equal");
    b[3] = 99;
    CHECK(ws_memcmp(a, b, sizeof a) != 0, "memcmp detects diff");
    CHECK(ws_memcmp(a, b, 3) == 0, "memcmp respects length");
}

// ---- masking: bridges Lean P1/P2 ----
static void test_mask(void) {
    const uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};

    // i mod 4 application on a known vector (more than 4 bytes to exercise wrap).
    uint8_t buf[6] = {0x00, 0x00, 0x00, 0x00, 0xff, 0xff};
    ws_mask(buf, 6, key);
    CHECK(buf[0] == 0x37 && buf[1] == 0xfa && buf[2] == 0x21 && buf[3] == 0x3d,
          "mask applies key[i] for i<4");
    CHECK(buf[4] == (0xff ^ 0x37) && buf[5] == (0xff ^ 0xfa), "mask wraps key at i mod 4 (P-impl)");

    // P1 involution: masking twice with the same key restores the original.
    uint8_t orig[13] = {'H', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!', 0x80};
    uint8_t work[13];
    ws_memcpy(work, orig, 13);
    ws_mask(work, 13, key);
    CHECK(ws_memcmp(work, orig, 13) != 0, "mask changes data");
    ws_mask(work, 13, key);
    CHECK(ws_memcmp(work, orig, 13) == 0, "mask involution: unmask(mask)=id (Lean P1)");

    // P2 length preservation: bytes outside [0,len) untouched.
    uint8_t guard[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    ws_mask(guard, 3, key);
    CHECK(guard[3] == 0xAA && guard[4] == 0xAA, "mask touches only len bytes (Lean P2)");

    // len 0 is a no-op.
    ws_mask(guard, 0, key);
    CHECK(guard[0] == (0xAA ^ 0x37), "mask len 0 is a no-op");
}

// ---- frame: classify (Lean P5) ----
static void test_classify_data(void) {
    CHECK(ws_classify_opcode(0x0) == WS_CLASS_DATA, "0x0 cont = data (P5)");
    CHECK(ws_classify_opcode(0x1) == WS_CLASS_DATA, "0x1 text = data (P5)");
    CHECK(ws_classify_opcode(0x2) == WS_CLASS_DATA, "0x2 bin = data (P5)");
}

static void test_classify_control(void) {
    CHECK(ws_classify_opcode(0x8) == WS_CLASS_CONTROL, "0x8 close = control (P5)");
    CHECK(ws_classify_opcode(0x9) == WS_CLASS_CONTROL, "0x9 ping = control (P5)");
    CHECK(ws_classify_opcode(0xA) == WS_CLASS_CONTROL, "0xA pong = control (P5)");
}

static void test_classify_reserved(void) {
    CHECK(ws_classify_opcode(0x3) == WS_CLASS_RESERVED, "0x3 = reserved (P5)");
    CHECK(ws_classify_opcode(0xB) == WS_CLASS_RESERVED, "0xB = reserved (P5)");
    CHECK(ws_classify_opcode(0xF) == WS_CLASS_RESERVED, "0xF = reserved (P5)");
}

// Predicate helpers keep the && out of CHECK (cognitive complexity). Written as
// guard chains so neither bare bool operands nor `== true` trip clang-tidy.
static _Bool tiny_ok(const ws_frame_header *h) {
    if (!h->fin) {
        return false;
    }
    if (h->masked) {
        return false;
    }
    return (_Bool)(h->opcode == WS_OP_TEXT && h->payload_len == 5);
}
static _Bool masked_ok(const ws_frame_header *h) {
    if (!h->masked) {
        return false;
    }
    return (_Bool)(h->payload_len == 3 && h->opcode == WS_OP_BIN && h->mask_key[0] == 1 &&
                   h->mask_key[3] == 4);
}

// ---- frame: parse_header lengths (RFC 6455 5.2; P3/P4) ----
static void test_parse_lengths(void) {
    ws_frame_header h;
    const uint8_t tiny[] = {0x81, 0x05};
    CHECK(ws_parse_header(tiny, 2, &h) == 2, "tiny unmasked header = 2 bytes");
    CHECK(tiny_ok(&h), "tiny fields");

    const uint8_t masked[] = {0x82, 0x83, 0x01, 0x02, 0x03, 0x04};
    CHECK(ws_parse_header(masked, 6, &h) == 6, "masked tiny header = 6 bytes");
    CHECK(masked_ok(&h), "masked fields + key");

    const uint8_t s[] = {0x81, 126, 0x00, 0xC8};
    CHECK(ws_parse_header(s, 4, &h) == 4, "short header = 4 bytes");
    CHECK(h.payload_len == 200, "short len 200");

    const uint8_t l[] = {0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0};
    CHECK(ws_parse_header(l, 10, &h) == 10, "long header = 10 bytes");
    CHECK(h.payload_len == 0x10000, "long len 0x10000");
}

// ---- frame: parse_header errors (NEED_MORE, P6, MSB) ----
static void test_parse_errors(void) {
    ws_frame_header h;
    const uint8_t tiny[] = {0x81, 0x05};
    const uint8_t s[] = {0x81, 126, 0x00, 0xC8};
    CHECK(ws_parse_header(tiny, 1, &h) == WS_ERR_NEED_MORE, "1 byte = need more");
    CHECK(ws_parse_header(s, 3, &h) == WS_ERR_NEED_MORE, "short missing len = need more");

    const uint8_t bigctl[] = {0x88, 126, 0x00, 0xC8};
    CHECK(ws_parse_header(bigctl, 4, &h) == WS_ERR_PROTOCOL, "control >125 rejected (P6)");
    const uint8_t okctl[] = {0x88, 125};
    CHECK(ws_parse_header(okctl, 2, &h) == 2, "control =125 ok (P6)");

    const uint8_t msb[] = {0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0};
    CHECK(ws_parse_header(msb, 10, &h) == WS_ERR_PROTOCOL, "127 MSB set rejected");
}

// The masked tiny frame parsed back keeps its mask flag and key bytes.
static _Bool masked_key_ok(const ws_frame_header *r) {
    if (!r->masked) {
        return false;
    }
    return (_Bool)(r->mask_key[0] == 0xDE && r->mask_key[3] == 0xEF);
}

// One build+parse roundtrip at a given length; returns true if it survives.
static _Bool roundtrip(uint64_t plen, int want_hdr) {
    uint8_t buf[14];
    ws_frame_header h = {0};
    ws_frame_header r;
    h.fin = true;
    h.opcode = WS_OP_TEXT;
    h.payload_len = plen;
    int n = ws_build_header(&h, buf, sizeof buf);
    if (n != want_hdr) {
        return false;
    }
    return (_Bool)(ws_parse_header(buf, (size_t)n, &r) == n && r.payload_len == plen);
}

// ---- frame: build_header + roundtrip (Lean P3/P4) ----
static void test_build_header(void) {
    CHECK(roundtrip(125, 2), "roundtrip tiny 125 (2 bytes)");
    CHECK(roundtrip(126, 4), "roundtrip 126 (short 4 bytes)");
    CHECK(roundtrip(0xFFFF, 4), "roundtrip 0xFFFF (short 4 bytes)");
    CHECK(roundtrip(0x10000, 10), "roundtrip 0x10000 (long 10 bytes)");

    // masked build -> +4 bytes, key preserved.
    uint8_t buf[14];
    ws_frame_header h = {0};
    ws_frame_header r;
    h.fin = true;
    h.opcode = WS_OP_BIN;
    h.payload_len = 5;
    h.masked = true;
    h.mask_key[0] = 0xDE;
    h.mask_key[3] = 0xEF;
    CHECK(ws_build_header(&h, buf, sizeof buf) == 6, "build masked tiny = 6 bytes");
    CHECK(ws_parse_header(buf, 6, &r) == 6, "parse masked back");
    CHECK(masked_key_ok(&r), "masked key preserved");

    CHECK(ws_build_header(&h, buf, 1) == WS_ERR_TOO_SMALL, "build cap too small");
}

// ---- sha1 (RFC 3174) ----
static void test_sha1(void) {
    uint8_t out[20];

    // "abc" -> a9993e364706816aba3e25717850c26c9cd0d89d
    const uint8_t abc[] = {'a', 'b', 'c'};
    const uint8_t abc_h[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                               0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    ws_sha1(abc, 3, out);
    CHECK(ws_memcmp(out, abc_h, 20) == 0, "sha1(abc)");

    // "" -> da39a3ee5e6b4b0d3255bfef95601890afd80709
    const uint8_t empty_h[20] = {0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
                                 0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09};
    ws_sha1((const uint8_t *)"", 0, out);
    CHECK(ws_memcmp(out, empty_h, 20) == 0, "sha1(empty)");

    // 56-byte message forces a second padding block (RFC 3174 §4).
    const uint8_t m56[56] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3',
                             '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6', '7',
                             '8', '9', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1',
                             '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5'};
    const uint8_t m56_h[20] = {0x0a, 0x40, 0xb8, 0xfb, 0xda, 0xaf, 0xb7, 0xc2, 0x96, 0x51,
                               0x61, 0x8a, 0xc1, 0x5d, 0x27, 0xe7, 0x72, 0x28, 0x71, 0x30};
    ws_sha1(m56, 56, out);
    CHECK(ws_memcmp(out, m56_h, 20) == 0, "sha1(56-byte, 2-block pad)");
}

// ---- base64 encode (standard alphabet, padded) ----
static _Bool b64_eq(const uint8_t *in, size_t len, const char *want) {
    uint8_t out[64];
    size_t n = ws_base64(in, len, out);
    size_t wlen = 0;
    while (want[wlen]) {
        wlen++;
    }
    return (_Bool)(n == wlen && ws_memcmp(out, want, wlen) == 0);
}

static void test_base64(void) {
    CHECK(b64_eq((const uint8_t *)"abc", 3, "YWJj"), "b64 abc (no pad)");
    CHECK(b64_eq((const uint8_t *)"", 0, ""), "b64 empty");
    CHECK(b64_eq((const uint8_t *)"a", 1, "YQ=="), "b64 1 byte (==)");
    CHECK(b64_eq((const uint8_t *)"ab", 2, "YWI="), "b64 2 bytes (=)");

    // 20-byte sha1-sized input -> 28 chars (ceil(20/3)*4), one trailing '='.
    const uint8_t twenty[20] = {0};
    uint8_t out[64];
    size_t n = ws_base64(twenty, 20, out);
    CHECK(n == 28, "b64 20 bytes -> 28 chars");
    CHECK(out[27] == '=', "b64 20 bytes one trailing pad");
    CHECK(out[26] != '=', "b64 20 bytes only one pad");
}

// ---- handshake accept (RFC 6455 §4.2.2) ----
static void test_handshake(void) {
    // RFC 6455 §4.2.2 worked example.
    const uint8_t key[] = "dGhlIHNhbXBsZSBub25jZQ==";
    const char *want = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    uint8_t out[28];
    int n = ws_handshake_accept(key, 24, out);
    CHECK(n == 28, "accept length = 28");
    CHECK(ws_memcmp(out, want, 28) == 0, "accept = RFC 4.2.2 example");
}

// ---- lifecycle state machine (TLA+ WsLifecycle), via internal ws_lc_* ----
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

// ---- utf8 validation (RFC 6455 §8.1; Lean P8) ----
// Vectors mirror proofs/WsProto/Utf8.lean exactly so code <-> proof stay pinned.
// reject: overlong / surrogate / out-of-range (Lean reject_* theorems).
static void test_utf8_reject(void) {
    const uint8_t over2[] = {0xC0, 0x80};
    const uint8_t over3[] = {0xE0, 0x80, 0x80};
    const uint8_t surr[] = {0xED, 0xA0, 0x80};
    const uint8_t above[] = {0xF4, 0x90, 0x80, 0x80};
    CHECK(!ws_utf8_valid(over2, 2), "reject 2B overlong C0 80 (Lean reject_overlong2)");
    CHECK(!ws_utf8_valid(over3, 3), "reject 3B overlong E0 80 80 (Lean reject_overlong3)");
    CHECK(!ws_utf8_valid(surr, 3), "reject surrogate ED A0 80 (Lean reject_surrogate)");
    CHECK(!ws_utf8_valid(above, 4), "reject >U+10FFFF F4 90 80 80 (Lean reject_above_max)");
}

// accept: 1/2/3/4-byte + empty (Lean accept_* theorems).
static void test_utf8_accept(void) {
    const uint8_t dollar[] = {0x24};
    const uint8_t cent[] = {0xC2, 0xA2};
    const uint8_t euro[] = {0xE2, 0x82, 0xAC};
    const uint8_t clef[] = {0xF0, 0x9D, 0x84, 0x9E};
    CHECK(ws_utf8_valid(dollar, 1), "accept $ (Lean accept_dollar)");
    CHECK(ws_utf8_valid(cent, 2), "accept cent (Lean accept_cent)");
    CHECK(ws_utf8_valid(euro, 3), "accept euro (Lean accept_euro)");
    CHECK(ws_utf8_valid(clef, 4), "accept clef U+1D11E (Lean accept_clef)");
    CHECK(ws_utf8_valid((const uint8_t *)"", 0), "accept empty");
}

// boundary: lone continuation / truncated multibyte / F5 / ASCII run.
static void test_utf8_boundary(void) {
    const uint8_t lone[] = {0x80};
    const uint8_t trunc[] = {0xC2};
    const uint8_t f5[] = {0xF5, 0x80, 0x80, 0x80};
    CHECK(!ws_utf8_valid(lone, 1), "reject lone continuation 80");
    CHECK(!ws_utf8_valid(trunc, 1), "reject truncated 2B C2");
    CHECK(!ws_utf8_valid(f5, 4), "reject F5 lead byte");
    CHECK(ws_utf8_valid((const uint8_t *)"hello", 5), "accept ASCII run");
}

// ---- close code wire validity: bridges Lean P7 ----
static void test_close_forbidden(void) {
    CHECK(!ws_close_code_sendable(1005), "1005 forbidden on wire (P7)");
    CHECK(!ws_close_code_sendable(1006), "1006 forbidden on wire (P7)");
    CHECK(!ws_close_code_sendable(1015), "1015 forbidden on wire (P7)");
    CHECK(!ws_close_code_sendable(1004), "1004 reserved (P7)");
}

static void test_close_protocol(void) {
    CHECK(ws_close_code_sendable(1000), "1000 normal closure ok (P7)");
    CHECK(ws_close_code_sendable(1011), "1011 ok (P7)");
    CHECK(!ws_close_code_sendable(999), "999 below range (P7)");
    CHECK(!ws_close_code_sendable(1012), "1012 above protocol range (P7)");
}

static void test_close_app(void) {
    CHECK(ws_close_code_sendable(3000), "3000 app range ok (P7)");
    CHECK(ws_close_code_sendable(4999), "4999 app range ok (P7)");
    CHECK(!ws_close_code_sendable(5000), "5000 above app range (P7)");
}

// ---- driver: ws_conn_recv / ws_conn_poll / ws_send_* (TLA+ WsStream) ----
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

void run_tests(void) {
    test_mem();
    test_close_forbidden();
    test_close_protocol();
    test_close_app();
    test_mask();
    test_classify_data();
    test_classify_control();
    test_classify_reserved();
    test_parse_lengths();
    test_parse_errors();
    test_build_header();
    test_sha1();
    test_base64();
    test_handshake();
    test_utf8_reject();
    test_utf8_accept();
    test_utf8_boundary();
    test_lc_init();
    test_lc_handshake();
    test_lc_send_close();
    test_lc_recv_close();
    test_lc_frag();
    test_drv_single_text();
    test_drv_incremental();
    test_drv_fragmented();
    test_drv_ping();
    test_drv_close();
    test_drv_interleave();
    test_drv_stray_cont();
    test_drv_too_big();
    test_drv_unmasked_server();
    test_drv_illegal_state();
    test_drv_bad_utf8();
    test_drv_send_message();
    test_drv_send_close();
}
