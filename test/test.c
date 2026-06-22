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
static void test_classify(void) {
    CHECK(ws_classify_opcode(0x0) == WS_CLASS_DATA, "0x0 cont = data (P5)");
    CHECK(ws_classify_opcode(0x1) == WS_CLASS_DATA, "0x1 text = data (P5)");
    CHECK(ws_classify_opcode(0x2) == WS_CLASS_DATA, "0x2 bin = data (P5)");
    CHECK(ws_classify_opcode(0x8) == WS_CLASS_CONTROL, "0x8 close = control (P5)");
    CHECK(ws_classify_opcode(0x9) == WS_CLASS_CONTROL, "0x9 ping = control (P5)");
    CHECK(ws_classify_opcode(0xA) == WS_CLASS_CONTROL, "0xA pong = control (P5)");
    CHECK(ws_classify_opcode(0x3) == WS_CLASS_RESERVED, "0x3 = reserved (P5)");
    CHECK(ws_classify_opcode(0xB) == WS_CLASS_RESERVED, "0xB = reserved (P5)");
    CHECK(ws_classify_opcode(0xF) == WS_CLASS_RESERVED, "0xF = reserved (P5)");
}

// ---- frame: parse_header (RFC 6455 5.2; P3/P4 lengths, P6 control) ----
static void test_parse_header(void) {
    ws_frame_header h;

    // tiny, FIN text, unmasked, len 5 -> 2 header bytes.
    const uint8_t tiny[] = {0x81, 0x05};
    CHECK(ws_parse_header(tiny, 2, &h) == 2, "tiny unmasked header = 2 bytes");
    CHECK(h.fin && h.opcode == WS_OP_TEXT && !h.masked && h.payload_len == 5, "tiny fields");

    // masked tiny -> 6 header bytes, mask key read.
    const uint8_t masked[] = {0x82, 0x83, 0x01, 0x02, 0x03, 0x04};
    CHECK(ws_parse_header(masked, 6, &h) == 6, "masked tiny header = 6 bytes");
    CHECK(h.masked && h.payload_len == 3 && h.opcode == WS_OP_BIN, "masked fields");
    CHECK(h.mask_key[0] == 1 && h.mask_key[3] == 4, "mask key read");

    // 126 form: len 200 -> 4 header bytes.
    const uint8_t s[] = {0x81, 126, 0x00, 0xC8};
    CHECK(ws_parse_header(s, 4, &h) == 4, "short header = 4 bytes");
    CHECK(h.payload_len == 200, "short len 200");

    // 127 form: len 0x10000 -> 10 header bytes.
    const uint8_t l[] = {0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0};
    CHECK(ws_parse_header(l, 10, &h) == 10, "long header = 10 bytes");
    CHECK(h.payload_len == 0x10000, "long len 0x10000");

    // need more bytes.
    CHECK(ws_parse_header(tiny, 1, &h) == WS_ERR_NEED_MORE, "1 byte = need more");
    CHECK(ws_parse_header(s, 3, &h) == WS_ERR_NEED_MORE, "short missing len = need more");

    // P6: control opcode (close) with payload > 125 must be rejected.
    const uint8_t bigctl[] = {0x88, 126, 0x00, 0xC8};
    CHECK(ws_parse_header(bigctl, 4, &h) == WS_ERR_PROTOCOL, "control >125 rejected (P6)");
    // control exactly 125 ok.
    const uint8_t okctl[] = {0x88, 125};
    CHECK(ws_parse_header(okctl, 2, &h) == 2, "control =125 ok (P6)");

    // 127 form with MSB set is illegal (RFC 5.2).
    const uint8_t msb[] = {0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0};
    CHECK(ws_parse_header(msb, 10, &h) == WS_ERR_PROTOCOL, "127 MSB set rejected");
}

// ---- frame: build_header + roundtrip (Lean P3/P4) ----
static void test_build_header(void) {
    uint8_t buf[14];
    ws_frame_header h = {0};
    ws_frame_header r;

    // tiny boundary 125 -> 2 bytes.
    h.fin = 1;
    h.opcode = WS_OP_TEXT;
    h.payload_len = 125;
    CHECK(ws_build_header(&h, buf, sizeof buf) == 2, "build tiny 125 = 2 bytes");
    CHECK(ws_parse_header(buf, 2, &r) == 2 && r.payload_len == 125, "roundtrip 125");

    // 126 boundary -> short form, 4 bytes.
    h.payload_len = 126;
    CHECK(ws_build_header(&h, buf, sizeof buf) == 4, "build 126 = short 4 bytes");
    CHECK(ws_parse_header(buf, 4, &r) == 4 && r.payload_len == 126, "roundtrip 126");

    // 0xFFFF boundary -> short form.
    h.payload_len = 0xFFFF;
    CHECK(ws_build_header(&h, buf, sizeof buf) == 4, "build 0xFFFF = short 4 bytes");
    CHECK(ws_parse_header(buf, 4, &r) == 4 && r.payload_len == 0xFFFF, "roundtrip 0xFFFF");

    // 0x10000 -> long form, 10 bytes.
    h.payload_len = 0x10000;
    CHECK(ws_build_header(&h, buf, sizeof buf) == 10, "build 0x10000 = long 10 bytes");
    CHECK(ws_parse_header(buf, 10, &r) == 10 && r.payload_len == 0x10000, "roundtrip 0x10000");

    // masked build -> +4 bytes, key preserved.
    h.payload_len = 5;
    h.masked = 1;
    h.mask_key[0] = 0xDE;
    h.mask_key[3] = 0xEF;
    CHECK(ws_build_header(&h, buf, sizeof buf) == 6, "build masked tiny = 6 bytes");
    CHECK(ws_parse_header(buf, 6, &r) == 6 && r.masked && r.mask_key[0] == 0xDE &&
              r.mask_key[3] == 0xEF,
          "roundtrip masked key");

    // too small.
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

void run_tests(void) {
    test_mem();
    test_mask();
    test_classify();
    test_parse_header();
    test_build_header();
    test_sha1();
}
