#include "../src/ws_internal.h"
#include "ws.h"

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

// ---- lifecycle state machine (TLA+ WsLifecycle) ----
static void test_lc_init(void) {
    ws_conn c;
    ws_conn_init(&c);
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
    ws_conn_init(&c);
    CHECK(ws_conn_step(&c, WS_EV_HANDSHAKE, WS_FRAG_NONE) == WS_OK, "handshake legal");
    CHECK(c.state == WS_OPEN, "handshake -> OPEN");
    CHECK(inv_holds(&c), "INV after handshake");
    // Second handshake illegal (not CONNECTING).
    CHECK(ws_conn_step(&c, WS_EV_HANDSHAKE, WS_FRAG_NONE) == WS_ERR_ILLEGAL_STATE,
          "handshake from OPEN illegal");
}

static void test_lc_send_close(void) {
    ws_conn c;
    ws_conn_init(&c);
    // SEND_CLOSE before OPEN is illegal.
    CHECK(ws_conn_step(&c, WS_EV_SEND_CLOSE, WS_FRAG_NONE) == WS_ERR_ILLEGAL_STATE,
          "send-close while CONNECTING illegal");
    ws_conn_step(&c, WS_EV_HANDSHAKE, WS_FRAG_NONE);
    CHECK(ws_conn_step(&c, WS_EV_SEND_CLOSE, WS_FRAG_NONE) == WS_OK, "send-close from OPEN legal");
    CHECK(c.state == WS_CLOSING && c.sent_close, "send-close -> CLOSING, sent");
    CHECK(inv_holds(&c), "INV after send-close");
}

static void test_lc_recv_close(void) {
    ws_conn c;
    ws_conn_init(&c);
    ws_conn_step(&c, WS_EV_HANDSHAKE, WS_FRAG_NONE);
    ws_conn_step(&c, WS_EV_START_FRAG, WS_FRAG_TEXT); // in-flight fragment
    CHECK(ws_conn_step(&c, WS_EV_RECV_CLOSE, WS_FRAG_NONE) == WS_OK, "recv-close from OPEN legal");
    CHECK(c.state == WS_CLOSED, "recv-close -> CLOSED");
    CHECK(c.sent_close && c.rcvd_close, "recv-close sets both bits (reply-on-receive)");
    CHECK(c.frag == WS_FRAG_NONE, "recv-close discards fragment (R6a)");
    CHECK(inv_holds(&c), "INV after recv-close");
    // Frame event after CLOSED is illegal.
    CHECK(ws_conn_step(&c, WS_EV_START_FRAG, WS_FRAG_BIN) == WS_ERR_ILLEGAL_STATE,
          "start-frag after CLOSED illegal");
}

static void test_lc_frag(void) {
    ws_conn c;
    ws_conn_init(&c);
    ws_conn_step(&c, WS_EV_HANDSHAKE, WS_FRAG_NONE);
    CHECK(ws_conn_step(&c, WS_EV_START_FRAG, WS_FRAG_BIN) == WS_OK, "start-frag legal");
    CHECK(c.frag == WS_FRAG_BIN, "start-frag sets type");
    // Second start while already fragmenting is illegal.
    CHECK(ws_conn_step(&c, WS_EV_START_FRAG, WS_FRAG_TEXT) == WS_ERR_ILLEGAL_STATE,
          "start-frag while fragmenting illegal");
    CHECK(ws_conn_step(&c, WS_EV_FINISH_FRAG, WS_FRAG_NONE) == WS_OK, "finish-frag legal");
    CHECK(c.frag == WS_FRAG_NONE, "finish-frag clears type");
    // Finish with no fragment is illegal.
    CHECK(ws_conn_step(&c, WS_EV_FINISH_FRAG, WS_FRAG_NONE) == WS_ERR_ILLEGAL_STATE,
          "finish-frag with no fragment illegal");
    // Frame events while CONNECTING are illegal.
    ws_conn c2;
    ws_conn_init(&c2);
    CHECK(ws_conn_step(&c2, WS_EV_START_FRAG, WS_FRAG_TEXT) == WS_ERR_ILLEGAL_STATE,
          "start-frag while CONNECTING illegal");
}

void run_tests(void) {
    test_mem();
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
    test_lc_init();
    test_lc_handshake();
    test_lc_send_close();
    test_lc_recv_close();
    test_lc_frag();
}
